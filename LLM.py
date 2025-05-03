from dashscope import Generation
from dashscope.api_entities.dashscope_response import Role
from langchain_community.vectorstores import Chroma
from langchain_community.embeddings.huggingface import HuggingFaceEmbeddings
import dashscope
import mmap
import posix_ipc
import ctypes
import os
import time

class SharedMemoryIPC:
    def __init__(self, shm_name='/my_shared_mem1', sem_name='/my_semaphore1', size=1024):
        self.shm_name = shm_name
        self.sem_name = sem_name
        self.size = size
        
        # 创建/打开信号量
        try:
            self.semaphore = posix_ipc.Semaphore(
                sem_name, 
                flags=posix_ipc.O_CREAT,
                mode=0o666,
                initial_value=1
            )
        except posix_ipc.ExistentialError:
            print("Semaphore already exists, opening...")
            self.semaphore = posix_ipc.Semaphore(sem_name)
        
        # 创建/打开共享内存
        try:
            self.shm = posix_ipc.SharedMemory(
                shm_name,
                flags=posix_ipc.O_CREAT,
                mode=0o666,
                size=size
            )
        except posix_ipc.ExistentialError:
            print("Shared memory already exists, opening...")
            self.shm = posix_ipc.SharedMemory(shm_name)
        
        # 创建内存映射
        self.mmap = mmap.mmap(
            self.shm.fd,
            self.size,
            access=mmap.ACCESS_READ | mmap.ACCESS_WRITE
        )
        # 关闭文件描述符，mmap会保持它打开
        os.close(self.shm.fd)
    
    def read_data(self):
        """从共享内存读取数据"""
        try:
            # 获取信号量
            self.semaphore.acquire()

            #print("python have already read data")
            
            # 读取数据 (去除空字符)
            data = self.mmap.read().decode('utf-8').rstrip('\x00')
            self.mmap.seek(0)  # 重置指针
            
            print("python have already read data")
            
            return data
        finally:
            # 确保释放信号量
            self.semaphore.release()
    
    def write_data(self, data = "1"):
        """写入数据到共享内存"""
        if len(data) > self.size:
            raise ValueError("Data size exceeds shared memory size")
        
        try:
            # 获取信号量
            self.semaphore.acquire()
            
            # 清空并写入新数据
            #self.mmap.seek(0)
            self.mmap.write(b"hello work")
            self.mmap.seek(0)  # 重置指针
            self.mmap.flush()
            print("python have already send data",data)
        finally:
            # 确保释放信号量
            self.semaphore.release()
    
    def close(self):
        """清理资源"""
        self.mmap.close()
        self.shm.close_fd()
        self.semaphore.close()
        # 注意：通常在最后一个进程中使用unlink
        # self.shm.unlink()
        # self.semaphore.unlink()


if __name__ == "__main__":


    dashscope.api_key="xxxxx....9"  #用的阿里QWEN的api
    ipc1 = SharedMemoryIPC()
    ipc2 = SharedMemoryIPC(shm_name='/my_shared_mem2', sem_name='/my_semaphore2', size=1024)

    messages = []

    model_name = r"text2vec"
    model_kwargs = {'device': 'cpu'}
    encode_kwargs = {'normalize_embeddings': False}
    embeddings = HuggingFaceEmbeddings(
        model_name=model_name,
        model_kwargs=model_kwargs,
        encode_kwargs=encode_kwargs
    )
    db = Chroma(persist_directory="./chroma/news_test", embedding_function=embeddings)

    while True:
        message = List(data = ipc1.read_data())

        if message:

            similarDocs = db.similarity_search(message, k=3)
            summary_prompt = "".join([doc.page_content for doc in similarDocs])

            send_message = f"你是一个只能回答助手,请参考{summary_prompt}对{message}的问题进行解答，如果{summary_prompt}中没有提及，请直接回答"
            messages.append({'role': Role.USER, 'content': send_message})
            whole_message = ''
            # 切换模型
            responses = Generation.call(Generation.Models.qwen_max, messages=messages, result_format='message', stream=True, incremental_output=True)
            # responses = Generation.call(Generation.Models.qwen_turbo, messages=messages, result_format='message', stream=True, incremental_output=True)
            print('system:',end='')
            for response in responses:
                whole_message += response.output.choices[0]['message']['content']
                print(response.output.choices[0]['message']['content'], end='')
                ipc2.write_data()

            print()
            messages.append({'role': 'assistant', 'content': whole_message})


