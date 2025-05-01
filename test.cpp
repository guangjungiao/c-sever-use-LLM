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

#define PORT 8080
#define MAX_EVENTS 10000
#define THREAD_POOL_SIZE 16
#define BUFFER_SIZE 4096

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
                        if(this->stop && this->tasks.empty())
                            return;
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
        for(std::thread &worker: workers)
            worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// HTTP服务器类
class HttpServer {
public:
    HttpServer() : pool(THREAD_POOL_SIZE) {
        // 创建socket
        server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (server_fd == -1) {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        // 设置socket选项
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
            perror("setsockopt");
            exit(EXIT_FAILURE);
        }

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(PORT);

        // 绑定socket
        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            perror("bind");
            exit(EXIT_FAILURE);
        }

        // 监听
        if (listen(server_fd, SOMAXCONN) < 0) {
            perror("listen");
            exit(EXIT_FAILURE);
        }

        // 创建epoll实例
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }

        // 添加服务器socket到epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // 边缘触发模式
        ev.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
            perror("epoll_ctl: server_fd");
            exit(EXIT_FAILURE);
        }

        std::cout << "Server started on port " << PORT << std::endl;
    }

    void run() {
        struct epoll_event events[MAX_EVENTS];
        
        while (true) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
            if (nfds == -1) {
                perror("epoll_wait");
                exit(EXIT_FAILURE);
            }

            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == server_fd) {
                    // 处理新连接
                    handleNewConnection();
                } else {
                    int fd1 = events[i].data.fd;
                    // 处理客户端请求
                    pool.enqueue(&HttpServer::handleClient, this, fd1);
                }
            }
        }
    }

private:

    std::string storedData; // 用于存储接收到的数据

    void handleNewConnection() {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd;

        // 接受所有待处理连接
        while ((client_fd = accept4(server_fd, (struct sockaddr*)&client_addr, 
                                  &client_addr_len, SOCK_NONBLOCK)) > 0) {
            // 设置客户端socket为非阻塞
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            // 添加客户端socket到epoll
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
            ev.data.fd = client_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                perror("epoll_ctl: client_fd");
                close(client_fd);
                continue;
            }
        }

        if (client_fd == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
    }

    void handleClient(int client_fd) {
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        std::string request;

        // 读取请求数据
        while ((bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1)) > 0) {
            buffer[bytes_read] = '\0';
            request.append(buffer);
            
            // 简单判断请求是否结束(根据空行)
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }

        if (bytes_read == -1 && errno != EAGAIN) {
            perror("read");
            closeClient(client_fd);
            return;
        }

        if (request.empty()) {
            closeClient(client_fd);
            return;
        }

        // 处理请求并发送响应
        std::string response = processRequest(request);
        sendResponse(client_fd, response); 
        
        // 关闭连接(简单实现，实际应该支持keep-alive)
        closeClient(client_fd);
    }

    std::string processRequest( std::string& request) {
        std::cout<<request<<std::endl;
        std::istringstream iss(request);
        std::string method, path, protocol;
        iss >> method >> path >> protocol;

        // 解析请求头
        std::unordered_map<std::string, std::string> headers;
        std::string line;
        while (std::getline(iss, line) && line != "\r") {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
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
        if (path == "/submit" && method == "POST") {
            return handlePostRequest(body);
        } else if (path == "/getdata" && method == "GET") {
            return handleGetRequest();
        } else {
            // 静态文件处理 (保留之前的代码)
            //if (path == "/") 
            {
                path = "/index.html";
            }
            std::cout<<1<<std::endl;

            // 尝试读取文件
            std::string filePath = "." + path;
            std::ifstream file(filePath, std::ios::binary);

            if (!file.is_open()) {
                
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




        /*
        std::istringstream iss(request);
        std::string method, path, protocol;
        iss >> method >> path >> protocol;

        // 简单的路由处理
        //if (path == "/") 
        {
            path = "/index.html";
        }

        // 尝试读取文件
        std::string filePath = "." + path;
        std::ifstream file(filePath, std::ios::binary);

        if (!file.is_open()) {
            
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
        */
    }

    void sendResponse(int client_fd, const std::string& response) {
        size_t total_sent = 0;
        const char* data = response.data();
        size_t remaining = response.size();

        while (remaining > 0) {
            ssize_t sent = write(client_fd, data + total_sent, remaining);
            if (sent == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

    void closeClient(int client_fd) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
        close(client_fd);
    }

    std::string handlePostRequest(const std::string& body) {
        std::cout<<"----------------------------------------post-------------------------------"<<std::endl;
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
            std::cout<<"-------------------------------storedData33------------------------------"<<storedData<<std::endl;
            
            // 构建JSON响应
            json response;
            response["message"] = "数据接收成功: " + storedData;
            return buildJsonResponse(response.dump());
        } 
        
        catch (const std::exception& e) {
            json error;
            error["error"] = "无效的请求数据";
            std::cout<<"no message"<<std::endl;
            return buildJsonResponse(error.dump(), 400);
        }
        
    }

    std::string handleGetRequest() {
        json response;
        response["message"] = storedData.empty() ? "暂无数据" : storedData;
        std::cout<<"----------------------get-------------------------------------"<<std::endl;
        std::cout<<"-------------------------------storedData------------------------------"<<response["message"]<<std::endl;
        return buildJsonResponse(response.dump());
    }

    std::string serveStaticFile( std::string& path) {
        // ... 保留之前的静态文件处理代码 ...
        // if (path == "/") {
        //     path = "/index.html";
        // }
        std::cout<<1<<std::endl;
        // 尝试读取文件
        std::string filePath = "./index.html";
        std::ifstream file(filePath, std::ios::binary);

        if (!file.is_open()) {
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

    std::string buildJsonResponse(const std::string& jsonContent, int statusCode = 200) {
        std::ostringstream response;
        response << "HTTP/1.1 " << statusCode << " " << getStatusMessage(statusCode) << "\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << jsonContent.size() << "\r\n"
                 << "Connection: close\r\n"
                 << "\r\n"
                 << jsonContent;
        return response.str();
    }

    std::string getStatusMessage(int statusCode) {
        switch(statusCode) {
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
