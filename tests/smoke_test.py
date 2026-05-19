import socket

HOST = "127.0.0.1"
PORT = 9000

def recv_text(sock):
    return sock.recv(4096).decode("utf-8",errors="replace")
# 从 socket 接收最多 4096 字节的数据，返回的是 bytes 对象
#.decode("utf-8", errors="replace")：把接收到的字节按 UTF-8 编码解码成 Python 字符串（str 类型）。
# errors="replace" 的意思是如果遇到无法解码的字节，就用 � 替代而不是抛出异常。

def send_cmd(sock,cmd):
    sock.sendall((cmd + "\n").encode("utf-8"))
    # sendall 会保证把所有数据都发送完，内部会循环调用底层的 send 直到全部数据送出。
    return recv_text(sock)
    # 发送后，立即调用 recv_text(sock) 读取服务器的响应并返回。

def main():
    with socket.create_connection((HOST,PORT),timeout = 3) as sock:         # 创建一个 TCP 套接字并连接到 127.0.0.1:9000，超时设为 3 秒。
        # as sock：把创建好的 socket 对象命名为 sock。
        # with 块结束时，无论是否发生异常，都会自动调用 sock.close() 关闭套接字。
        #这就好比 C++ 的 RAII，用智能指针或析构函数自动释放资源。
        
        welcome = recv_text(sock)
        assert "WELCOME" in welcome,welcome

        assert send_cmd(sock,"PING") == "PONG\n"
        assert send_cmd(sock,"SET 23 zhao") == "OK\n"
        assert send_cmd(sock,"GET 23") == "VALUE zhao\n"
        assert send_cmd(sock,"SIZE") == "SIZE 1\n"
        assert send_cmd(sock,"DEL 23") == "DELETED\n"
        assert send_cmd(sock,"GET 23") == "NOT_FOUND\n"
        assert send_cmd(sock,"EXIT") == "BYE\n"

    print("smoke test passed")


if __name__ == "__main__":
    main()
