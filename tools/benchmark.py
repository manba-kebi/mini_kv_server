# 写一个多客户端并发发送 SET/GET 的脚本

import socket
import threading
import time

HOST = "127.0.0.1"
PORT = 9000
CLIENTS = 8
ROUNDS = 500

def request(sock,cmd):
    sock.sendall((cmd+"\n").encode())
    return sock.recv(4096)
    
def worker(worker_id):
    # 每个 worker 线程创建一个 socket 连接。
    with socket.create_connection((HOST,PORT),timeout=3) as sock:
        sock.recv(4096)
        for i in range(ROUNDS):
            key = f"k{worker_id}_{i}"
            # 循环发送 `SET key value` 和 `GET key`
            request(sock,f"SET {key} value")
            request(sock,f"GET {key}")
        request(sock,"EXIT")
        

def main():
    start = time.perf_counter()     # 用 `time.perf_counter()` 统计耗时。
    threads = [threading.Thread(target=worker,args=(i,)) for i in range(CLIENTS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    
    elapsed = time.perf_counter() - start
    total = CLIENTS*ROUNDS*2
    # 最后输出总请求数和每秒请求数。
    print(f"request={total},seconds={elapsed:.3f},qps={total / elapsed:.1f}")
    
if __name__ == "__main__":
    main()
