#include <iostream>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <functional>
#include <unordered_map>
#include <fstream>
#include <nlohmann/json.hpp> // 需要添加JSON库
#include <queue>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstring>
#include <semaphore.h>
#include <cerrno>
#include <hiredis/hiredis.h>
#include <openssl/md5.h> // 用于生成MD5
#include <unordered_map>

#define PORT 8080
#define MAX_EVENTS 10000
#define THREAD_POOL_SIZE 16
#define BUFFER_SIZE 4096

//const std::string& shm_name = "/my_shared_mem";

using json = nlohmann::json;

/*
    g++ -std=c++11 -O2 -pthread http_server.cpp -o http_server
    ./http_server
    wrk -t12 -c400 -d30s http://localhost:8080/
*/

// 线程池实现
class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while(true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F, class... Args>
    void enqueue(F&& f, Args&&... args) {
        // {
        //     std::unique_lock<std::mutex> lock(queue_mutex);
        //     tasks.emplace([=] { f(args...); });
        // }
        //std::cout<<"start"<<std::endl;
        std::function<void()> task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex);
            tasks.emplace(task);
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) worker.join();
        //std::cout<<"delete"<<std::endl;
        
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// 共享内存与信号量
class SharedMemoryIPC 
{
public:
    SharedMemoryIPC(const std::string& shm_name, const std::string& sem_name, size_t size): shm_name_(shm_name), sem_name_(sem_name), size_(size) 
    {
        // 创建或打开信号量 (初始值为1，二进制信号量)
        sem_ = sem_open(sem_name_.c_str(), O_CREAT, 0666, 1);
        if (sem_ == SEM_FAILED) 
        {
            std::cout << "Failed to create semaphore: " << strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }

        // 创建共享内存
        shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (shm_fd_ == -1) 
        {
            std::cout << "Failed to create shared memory: " << strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
        ftruncate(shm_fd_, size_);//xitongdiaoyong shezhidaxiao
    }

    ~SharedMemoryIPC() {
        //shifang
        close(shm_fd_);
        sem_close(sem_);
        // 注意：通常由最后一个使用的进程执行unlink

        // shm_unlink(shm_name_.c_str());
        // sem_unlink(sem_name_.c_str());
    }

    void write_data(const std::string& data) {
        if (data.size() > size_) {
            std::cerr << "Data size exceeds shared memory size" << std::endl;
            return;
        }

        // 获取信号量
        if (sem_wait(sem_) == -1) {
            std::cerr << "sem_wait failed: " << strerror(errno) << std::endl;
            return;
        }

        // 映射共享内存
        void* ptr = mmap(0, size_, PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        if (ptr == MAP_FAILED) {
            std::cerr << "mmap failed: " << strerror(errno) << std::endl;
            sem_post(sem_);
            return;
        }

        // 写入数据
        memset(ptr, 0, size_); // 清空内存
        memcpy(ptr, data.c_str(), data.size());

        // 清理
        munmap(ptr, size_);
        sem_post(sem_); // 释放信号量
    }

    std::string read_data() 
    {
        // 获取信号量
        if (sem_wait(sem_) == -1) 
        {
            std::cerr << "sem_wait failed: " << strerror(errno) << std::endl;
            return "";
        }

        // 映射共享内存
        void* ptr = mmap(0, size_, PROT_READ, MAP_SHARED, shm_fd_, 0);
        if (ptr == MAP_FAILED) 
        {
            std::cerr << "mmap failed: " << strerror(errno) << std::endl;
            sem_post(sem_);
            return "";
        }

        // 读取数据
        std::string data(static_cast<char*>(ptr));

        // 清理
        munmap(ptr, size_);
        sem_post(sem_); // 释放信号量

        std::cout<<"success read"<<std::endl;

        return data;
    }

private:
    std::string shm_name_;
    std::string sem_name_;
    size_t size_;
    int shm_fd_;
    sem_t* sem_;
};

//Redis封装
class RedisCache {
/*
    key = "qa:" + MD5(question)
    value = answer
*/


public:
    RedisCache(const std::string& host = "127.0.0.1", int port = 6379): host_(host), port_(port), context_(nullptr) 
    {
        if(!connect()) return ;
    }
    
    ~RedisCache() 
    {
        if (context_) 
        {
            redisFree(context_);
        }
    }

    redisContext* get_context_()
    {
        return context_;
    }
    
    // 检查并重新连接
    bool checkConnection() {
        if (!context_ || context_->err) 
        {
            if (context_) 
            {
                redisFree(context_);
                context_ = nullptr;
            }
            return connect();
        }
        return true;
    }
    
    // 从缓存获取答案
    bool getAnswer(const std::string& question, std::string& answer) {
        if (!checkConnection()) return false;
        
        std::string key = "qa:" + generateMD5(question);
        redisReply* reply = (redisReply*)redisCommand(context_, "GET %s", key.c_str());
        
        if (!reply) return false;
        
        bool found = false;
        if (reply->type == REDIS_REPLY_STRING) {
            answer = std::string(reply->str, reply->len);
            found = true;
        }
        
        freeReplyObject(reply);
        return found;
    }
    
    // 设置缓存答案
    bool setAnswer(const std::string& question, const std::string& answer, int ttl = 3600) 
    {
        if (!checkConnection()) return false;
        
        std::string key = "qa:" + generateMD5(question);
        save_MD5ToQueswion[key] = question;
        redisReply* reply = (redisReply*)redisCommand(context_, "SETEX %s %d %s", key.c_str(), ttl, answer.c_str());  //SETEX 设置超时淘汰 ttl
        
        if (!reply) return false;
        
        bool success = (reply->type == REDIS_REPLY_STATUS && strcasecmp(reply->str, "OK") == 0);
        
        freeReplyObject(reply);
        return success;
    }

    std::string getQuestion(const std::string& question)
    {
        return save_MD5ToQueswion[question];
    }

private:
    bool connect() 
    {
        context_ = redisConnect(host_.c_str(), port_);
        if (!context_ || context_->err) 
        {
            if (context_) 
            {
                std::cout << "Redis connection error: " << context_->errstr << std::endl;
                redisFree(context_);
                context_ = nullptr;
            } 
            else 
            {
                std::cout << "Redis connection error: can't allocate redis context" << std::endl;
            }
            return false;
        }
        std::cout << "Redis connection successed" << std::endl;
        return true;
    }
    
    std::string generateMD5(const std::string& input) 
    {
        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5((const unsigned char*)input.c_str(), input.size(), digest);
        
        char mdString[33];
        for (int i = 0; i < 16; i++) sprintf(&mdString[i*2], "%02x", (unsigned int)digest[i]);
        
        return std::string(mdString);
    }

    std::unordered_map<std::string,std::string> save_MD5ToQueswion;

    std::string host_;
    int port_;
    redisContext* context_;
};

// HTTP服务器类
class HttpServer 
{
public:
    HttpServer() : pool(THREAD_POOL_SIZE) ,ipc1("/my_shared_mem1", "/my_semaphore1", 1024),ipc2("/my_shared_mem2", "/my_semaphore2", 1024)
    {
        //实例化共享内存对象
        //SharedMemoryIPC ipc("/my_shared_mem", "/my_semaphore", 1024);

        // 创建socket
        server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (server_fd == -1) 
        {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        // 设置socket选项
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) 
        {
            perror("setsockopt");
            exit(EXIT_FAILURE);
        }

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(PORT);

        // 绑定socket
        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) 
        {
            perror("bind");
            exit(EXIT_FAILURE);
        }

        // 监听
        if (listen(server_fd, SOMAXCONN) < 0) 
        {
            perror("listen");
            exit(EXIT_FAILURE);
        }

        // 创建epoll实例
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) 
        {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }

        // 添加服务器socket到epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // 边缘触发模式
        ev.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) 
        {
            perror("epoll_ctl: server_fd");
            exit(EXIT_FAILURE);
        }

        std::cout << "Server started on port " << PORT << std::endl;
    }

    void run() 
    {
        struct epoll_event events[MAX_EVENTS];
        
        while (true) 
        {
            int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
            if (nfds == -1) 
            {
                perror("epoll_wait");
                exit(EXIT_FAILURE);
            }

            for (int i = 0; i < nfds; ++i) 
            {
                if (events[i].data.fd == server_fd) 
                {
                    // 处理新连接
                    handleNewConnection();
                } 
                else 
                {
                    int fd1 = events[i].data.fd;
                    // 处理客户端请求
                    pool.enqueue(&HttpServer::handleClient, this, fd1);
                }
            }
        }
    }
    

private:

    std::string storedData; // 用于存储接收到的数据
    

    SharedMemoryIPC ipc1;//共享内存 sever2pyhton
    SharedMemoryIPC ipc2;//共享内存 python2sever

    RedisCache redisCache;//redis 

    void handleNewConnection() 
    {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd;

        // 接受所有待处理连接
        while ((client_fd = accept4(server_fd, (struct sockaddr*)&client_addr, &client_addr_len, SOCK_NONBLOCK)) > 0) 
        {
            // 设置客户端socket为非阻塞
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            // 添加客户端socket到epoll
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
            ev.data.fd = client_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) 
            {
                perror("epoll_ctl: client_fd");
                close(client_fd);
                continue;
            }
        }

        if (client_fd == -1 && errno != EAGAIN && errno != EWOULDBLOCK) 
        {
            perror("accept");
        }
    }

    void handleClient(int client_fd) 
    {
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        std::string request;

        // 读取请求数据
        while ((bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1)) > 0) 
        {
            buffer[bytes_read] = '\0';
            request.append(buffer);
            
            // 简单判断请求是否结束(根据空行)
            if (request.find("\r\n\r\n") != std::string::npos) 
            {
                break;
            }
        }

        if (bytes_read == -1 && errno != EAGAIN) 
        {
            perror("read");
            closeClient(client_fd);
            return;
        }

        if (request.empty()) 
        {
            closeClient(client_fd);
            return;
        }

        // 处理请求并发送响应
        std::string response = processRequest(request);
        sendResponse(client_fd, response); 
        
        // 关闭连接(简单实现，实际应该支持keep-alive)
        closeClient(client_fd);
    }

    std::string processRequest( std::string& request) 
    {
        std::cout<<request<<std::endl;
        std::istringstream iss(request);
        std::string method, path, protocol;
        iss >> method >> path >> protocol;

        // 解析请求头
        std::unordered_map<std::string, std::string> headers;
        std::string line;
        while (std::getline(iss, line) && line != "\r") 
        {
            size_t colon = line.find(':');
            if (colon != std::string::npos) 
            {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                // 去除首尾空白字符
                key.erase(0, key.find_first_not_of(" \t\r\n"));
                key.erase(key.find_last_not_of(" \t\r\n") + 1);
                value.erase(0, value.find_first_not_of(" \t\r\n"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);
                headers[key] = value;
            }
        }

        // 读取请求体(POST请求)
        std::string body;
        if (method == "POST") {
            std::getline(iss, body, '\0'); // 读取剩余部分作为请求体
        }

        // 处理不同路径的请求
        if (path == "/submit" && method == "POST")   //post请求 发送问题
        {
            return handlePostRequest(body,headers);
        } 
        else if (path == "/getdata" && method == "GET") //get请求 
        {
            return handleGetRequest();
        }
        else if(path == "/get-cached-questions" ) //获取缓存问题
        {
            return getAllQuestionKeys();
        }
        // else if(path == "/get-answer" && method =="POST")
        // {
        //     return handleGetCachedQuestions(body);
        // }
        else 
        {
            // 静态文件处理 (保留之前的代码)
            //if (path == "/") 
            {
                path = "/index.html";
            }
            std::cout<<1<<std::endl;

            // 尝试读取文件
            std::string filePath = "." + path;
            std::ifstream file(filePath, std::ios::binary);

            if (!file.is_open()) 
            {
                
                // 文件不存在，返回404
                return "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/html\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "<html><body><h1>404 Not Found</h1></body></html>";
            }
            

            // 读取文件内容
            std::vector<char> fileContents((std::istreambuf_iterator<char>(file)), 
                                        std::istreambuf_iterator<char>());

            // 确定内容类型
            std::string contentType = "text/plain";
            if (path.find(".html") != std::string::npos) {
                contentType = "text/html";
            } else if (path.find(".css") != std::string::npos) {
                contentType = "text/css";
            } else if (path.find(".js") != std::string::npos) {
                contentType = "application/javascript";
            } else if (path.find(".jpg") != std::string::npos || path.find(".jpeg") != std::string::npos) {
                contentType = "image/jpeg";
            } else if (path.find(".png") != std::string::npos) {
                contentType = "image/png";
            }

            // 构建HTTP响应
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                    << "Content-Type: " << contentType << "\r\n"
                    << "Content-Length: " << fileContents.size() << "\r\n"
                    << "Connection: close\r\n"
                    << "\r\n";

            response.write(fileContents.data(), fileContents.size());

            return response.str();
        }



    }

    std::string getAllQuestionKeys() 
    {
        if (!redisCache.checkConnection()) return "";
        
        redisReply* reply = (redisReply*)redisCommand(redisCache.get_context_(), "KEYS qa:*");
        if (!reply || reply->type != REDIS_REPLY_ARRAY) 
        {
            if (reply) freeReplyObject(reply);
            return "";
        }

        json response;

        
        std::vector<std::string> questions;
        for (size_t i = 0; i < reply->elements; i++) 
        {
            //keys.push_back(reply->element[i]->str);
            //response["questions"] = reply->element[i]->str;
            //questions.push_back(redisCache.getQuestion(reply->element[i]->str));
            std::string ii = redisCache.getQuestion(reply->element[i]->str);

            if(ii!="") questions.push_back(ii);
        }
        //std::cout<<response<<std::endl;

        response["questions"] = questions;
        std::cout<<response<<std::endl;

        //std::string result;
        //keys.push_back(reply->element[0]->str);

        //std::string result = keys[0];

        // json response;
        
        // response["message"] = result;
        
        freeReplyObject(reply);
        
        return buildJsonResponse(response.dump());
    }

    std::string handleGetCachedQuestions(const std::string& body)
    {
        
        // try {
        //     auto keys = this.getAllQuestionKeys();
        //     std::vector<std::string> questions;
            
        //     // 这里可以优化为批量获取，而不是一个个获取
        //     for (const auto& key : keys) {
        //         std::string answer;
        //         if (redisCache.getAnswer(key.substr(3), answer)) { // 去掉"qa:"前缀
        //             questions.push_back(answer);
        //         }
        //     }
            
        //     json response;
        //     response["questions"] = questions;
        //     return buildJsonResponse(response.dump());
        // } catch (const std::exception& e) {
        //     res.setStatus(500);
        //     res.setBody(R"({"error": "Internal server error"})");
        // }
        
    }



    void sendResponse(int client_fd, const std::string& response) 
    {
        size_t total_sent = 0;
        const char* data = response.data();
        size_t remaining = response.size();

        while (remaining > 0) {
            ssize_t sent = write(client_fd, data + total_sent, remaining);
            if (sent == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) 
                {
                    // 资源暂时不可用，稍后再试
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                perror("write");
                break;
            }
            total_sent += sent;
            remaining -= sent;
        }
    }

    void closeClient(int client_fd) 
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
        close(client_fd);
    }

    std::string handlePostRequest(const std::string& body,std::unordered_map<std::string, std::string> headers) 
    {
        std::cout<<"----------------------------------------post-------------------------------"<<std::endl;

        //check Content-Length && Content-Type
        auto content_length_it = headers.find("Content-Length");
        if (content_length_it != headers.end()) 
        {
            size_t declared_length = std::stoul(content_length_it->second);
            if (body.size() != declared_length) 
            {
                return buildJsonResponse(R"({"error": "Content-Length mismatch"})", 400);
            }
        }
        auto content_type_it = headers.find("Content-Type");
        if(content_type_it == headers.end() || content_type_it->second.find("application/json") == std::string::npos) 
        {
            json error;
            error["error"] = "Invalid Content-Type";
            return buildJsonResponse(error.dump(), 400);
        }


        size_t json_start = body.find("\r\n\r\n");
        
        json_start += 4; // 跳过空行（\r\n\r\n共4字节）
        std::string json_str = body.substr(json_start);
        try 
        {
            // 解析JSON数据
            std::cout<<"-------------------------------storedData11------------------------------"<<storedData<<std::endl;
            auto j = json::parse(json_str);
            std::cout<<"-------------------------------storedData22------------------------------"<<storedData<<std::endl;
            std::cout<<j<<std::endl;
            storedData = j["data"].get<std::string>();
            
            //search redisCache
            std::string answer = "";
            if(redisCache.getAnswer(storedData, answer))
            {
                json response;
                response["message"] = answer;
                std::cout<<"cache read"<<std::endl;
                return buildJsonResponse(response.dump());
            }
            else
            {
                //shm 共享内存和python通信
                ipc1.write_data(storedData);
                std::cout << "Data written to shared memory" <<storedData<< std::endl;

                std::cout<<"-------------------------------storedData33------------------------------"<<storedData<<std::endl;
                
                // 构建JSON响应
                json response;
                sleep(3);
                std::string result = ipc2.read_data();
                std::cout<<"behind read"<<std::endl;
                response["message"] = "answering: " + result;
                //set cache
                redisCache.setAnswer(storedData,result);
                return buildJsonResponse(response.dump());
            }
        } 
        
        catch (const std::exception& e) {
            json error;
            error["error"] = "无效的请求数据";
            std::cout<<"no message"<<std::endl;
            return buildJsonResponse(error.dump(), 400);
        }
        
    }

    std::string handleGetRequest() 
    {
        json response;
        //response["message"] = storedData.empty() ? "暂无数据" : storedData;
        std::string result = ipc2.read_data();
        std::cout << "Received result: " << result << std::endl;
        response["message"] = result.empty() ? "暂无数据" : result;
        std::cout<<"----------------------get-------------------------------------"<<std::endl;
        std::cout<<response["message"]<<std::endl;

        //set cache
        //redisCache.setAnswer(storedData,result);

        return buildJsonResponse(response.dump());
    }

    std::string serveStaticFile( std::string& path) 
    {
        // ... 保留之前的静态文件处理代码 ...
        // if (path == "/") {
        //     path = "/index.html";
        // }
        std::cout<<1<<std::endl;
        // 尝试读取文件
        std::string filePath = "./index.html";
        std::ifstream file(filePath, std::ios::binary);

        if (!file.is_open()) 
        {
            std::cout<<"fail open"<<std::endl;
            // 文件不存在，返回404
            return "HTTP/1.1 404 Not Found\r\n"
                   "Content-Type: text/html\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "<html><body><h1>404 Not Found</h1></body></html>";
        }
        

        // 读取文件内容
        std::vector<char> fileContents((std::istreambuf_iterator<char>(file)), 
                                     std::istreambuf_iterator<char>());

        // 确定内容类型
        std::string contentType = "text/plain";
        if (path.find(".html") != std::string::npos) {
            contentType = "text/html";
        } else if (path.find(".css") != std::string::npos) {
            contentType = "text/css";
        } else if (path.find(".js") != std::string::npos) {
            contentType = "application/javascript";
        } else if (path.find(".jpg") != std::string::npos || path.find(".jpeg") != std::string::npos) {
            contentType = "image/jpeg";
        } else if (path.find(".png") != std::string::npos) {
            contentType = "image/png";
        }

        // 构建HTTP响应
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: " << contentType << "\r\n"
                 << "Content-Length: " << fileContents.size() << "\r\n"
                 << "Connection: close\r\n"
                 << "\r\n";

        response.write(fileContents.data(), fileContents.size());

        return response.str();
    }

    std::string buildJsonResponse(const std::string& jsonContent, int statusCode = 200) 
    {
        std::ostringstream response;
        response << "HTTP/1.1 " << statusCode << " " << getStatusMessage(statusCode) << "\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << jsonContent.size() << "\r\n"
                 << "Connection: close\r\n"
                 << "\r\n"
                 << jsonContent;
        return response.str();
    }

    std::string getStatusMessage(int statusCode) 
    {
        switch(statusCode) 
        {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            default: return "Unknown";
        }
    }

    int server_fd;
    int epoll_fd;
    struct sockaddr_in address;
    ThreadPool pool;
};

int main() {
    HttpServer server;
    server.run();
    return 0;
}
