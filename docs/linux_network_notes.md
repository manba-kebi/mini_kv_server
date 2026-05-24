# Linux 网络状态与 strace 系统调用笔记

这份笔记记录 mini_kv_server 在 Linux 下的两类观察：

1. 使用 `ss` 查看 TCP socket 状态，理解 `LISTEN`、`ESTAB/ESTABLISHED`、`TIME_WAIT`。
2. 使用 `strace` 查看网络系统调用，把 Linux 输出和项目代码中的 `Socket::create_tcp()`、`bind_and_listen()`、`accept()`、`receive()`、`send_all()` 对上。



## 1. ss 和 strace

1. `ss` 负责回答：服务端现在真的监听 9000 端口了吗？客户端连接建立了吗？连接关闭后状态变了吗？
2. `strace` 负责回答：我的 C++ 函数最后到底调用了哪些 Linux 系统调用？这些调用的参数和返回值是什么？



## 2. ss 查看网络状态

![ss](C:\Users\28251\Desktop\mini_kv_server\docs\images\ss.jpg)

### 2.1 `ss` 是什么

`ss` 全称可以理解为 socket statistics，是 Linux 下查看 socket 状态的工具。它可以查看 TCP、UDP、监听端口、已建立连接，以及这些连接属于哪个进程。

它比老工具 `netstat` 更现代，Linux 上更推荐用 `ss`。

### 2.2 `ss -lntp | grep 9000` 是什么

命令：

```bash
ss -lntp | grep 9000
```

拆开看：

```text
ss      查看 socket 状态
-l      只看 listening sockets，也就是正在监听的 socket
-n      numeric，不做名字解析，直接显示数字端口，比如 9000
-t      只看 TCP
-p      显示进程信息，比如进程名、pid、fd
grep 9000 只保留包含 9000 的行
```

所以这条命令的意思是：

> 查看当前系统中，和 9000 端口有关的 TCP 监听 socket，并显示它属于哪个进程。

### 2.3 `ss -antp | grep 9000` 是什么

命令：

```bash
ss -antp | grep 9000
```

拆开看：

```text
ss      查看 socket 状态
-a      all，显示所有 socket，包括监听和非监听
-n      numeric，直接显示数字端口
-t      只看 TCP
-p      显示进程信息
grep 9000 只保留包含 9000 的行
```

这条命令的意思是：

> 查看所有和 9000 端口有关的 TCP socket，包括监听 socket 和已经建立的连接。

### 2.4 客户端连接后的输出怎么看

第一行：

```text
LISTEN ... 0.0.0.0:9000 ... mini_kv_server fd=3
```

这还是服务端监听 socket。它一直存在，因为服务端还要继续等待新客户端连接。

第二行：

```text
ESTAB 127.0.0.1:59532 -> 127.0.0.1:9000 users:(("nc",pid=6539,fd=3))
```

这是客户端 `nc` 那一端。

含义：

```text
客户端 nc 使用本地临时端口 59532
连接到服务端 127.0.0.1:9000
连接状态是 ESTAB，也就是 ESTABLISHED，表示 TCP 连接已经建立
```

第三行：

```text
ESTAB 127.0.0.1:9000 -> 127.0.0.1:59532 users:(("mini_kv_server",pid=4543,fd=5))
```

这是服务端那一端。

含义：

```text
服务端 9000 端口
正在和客户端临时端口 59532 通信
这个连接 socket 属于 mini_kv_server
文件描述符是 fd=5
```

注意这里有两个 fd：

```text
fd=3 监听 socket
fd=5 已连接客户端 socket
```

这正好证明了一个重要概念：

> `listen socket` 只负责等待连接；`accept()` 成功后会返回一个新的 connected socket，用来和某个具体客户端通信。

### 2.5 关闭连接后为什么只剩 LISTEN

1. 客户端连接已经关闭，所以两个 `ESTAB` 连接项消失了。
2. 服务端进程还在运行，所以监听 socket 仍然存在。
3. 服务端可以继续接受新的客户端连接。

这说明连接处理结束后，服务端没有退出，监听 socket 仍然保持工作。

### 2.6 TIME_WAIT

`TIME_WAIT` 只会在**主动关闭连接**的一方出现，且持续一小段时间。

在 TCP 四次挥手断开连接时，**先发起关闭请求（调用 close / shutdown）的那一端**，在发送完最后一个 ACK 后，不会立刻消失，而是进入 `TIME_WAIT` 状态，并停留 **2MSL**（Maximum Segment Lifetime，报文最大生存时间，通常为 30 秒～2 分钟）后才彻底释放 socket。

流程简化：

1. 主动方发 FIN
2. 被动方回 ACK，然后也发 FIN
3. 主动方回最后一个 ACK，**随即进入 TIME_WAIT**
4. 等待 2MSL 后，连接彻底关闭

#### TIME_WAIT 的两个核心作用

##### 1. 可靠地终止 TCP 全双工连接

如果主动方发送的最后一个 ACK **在网络中丢失**，被动方（处于 LAST_ACK 状态）会超时重传它的 FIN。
假如主动方没有 TIME_WAIT 而是直接关闭了，它就再也收不到这个重传的 FIN，也就无法重发 ACK。被动方会一直卡在 LAST_ACK 无法关闭。
**TIME_WAIT 让主动方保持一段时间，能重新应答可能丢失的最后一个 ACK，保证双方最终都能正常关闭。**

##### 2. 让旧连接的“迷路报文”在网络中自然消失

TCP 用四元组（源IP、源端口、目的IP、目的端口）标识一个连接。如果连接关闭后，**立刻用完全相同的四元组建立一条新连接**，网络上可能还残留着上一个连接延迟到达的数据段。
新连接会把这些旧数据当成自己的数据，造成数据错乱。
**2MSL 等待时间确保一个方向上的最大存活报文（MSL）加上另一个方向的最大存活应答报文，总计 2MSL 时间内，所有旧连接的分组都能从网络中被丢弃。** 之后这个四元组才可以安全地被新连接复用。

### 2.7 总结

这次 `ss` 观察证明了：

1. `mini_kv_server` 启动后确实在监听 9000 端口。
2. `LISTEN` 状态对应服务端代码中的 `bind()` + `listen()`。
3. `0.0.0.0:9000` 对应代码中的 `INADDR_ANY`，表示监听所有 IPv4 地址。
4. `fd=3` 是监听 socket。
5. 客户端连接后，会出现两条 `ESTAB`，分别代表客户端一端和服务端一端。
6. 服务端接受连接后会产生新的连接 socket，例如 `fd=5`。
7. 连接关闭后，`ESTAB` 消失，但 `LISTEN` 仍然存在，说明服务端还在继续监听。



## 3. strace 看系统调用

![strace](C:\Users\28251\Desktop\mini_kv_server\docs\images\strace.jpg)

### 3.1 `strace` 是什么

`strace` 是 Linux 下跟踪系统调用的工具。

C++ 代码里写的是：

```cpp
::socket(...)
::bind(...)
::listen(...)
::accept(...)
::recv(...)
::send(...)
::shutdown(...)
```

但这些函数最终要进入 Linux 内核执行。`strace` 就能把这些系统调用打印出来，让我看到：

1. 调用了哪个系统调用。
2. 参数是什么。
3. 返回值是什么。
4. 是哪个线程调用的。

### 3.2 `strace -f -e trace=network ...` 是什么

命令：

```bash
strace -f -e trace=network ./build/mini_kv_server --port 9000 --workers 4 --queue 64 --backlog 64
```

拆开看：

```text
strace
```

启动程序并跟踪它的系统调用。

```text
-f
```

跟踪子进程/线程。mini_kv_server 会创建 worker 线程，如果不加 `-f`，可能只能看到主线程的系统调用，看不到 worker 线程里的 `recv/send`。

在上图输出里：

```text
strace: Process 7323 attached
strace: Process 7324 attached
strace: Process 7325 attached
strace: Process 7326 attached
```

这说明 strace 跟踪到了线程池创建的 4 个 worker 线程。

```text
-e trace=network
```

只显示网络相关系统调用，过滤掉大量无关调用，比如内存分配、文件读写、动态库加载等。

```text
./build/mini_kv_server --port 9000 --workers 4 --queue 64 --backlog 64
```

这是被 strace 启动和跟踪的服务端程序。



### 3.3 strace 输出和项目代码映射表

| strace 输出                                           | Linux 含义                 | 项目代码位置                                     | C++ 函数作用                   |
| ----------------------------------------------------- | -------------------------- | ------------------------------------------------ | ------------------------------ |
| `socket(AF_INET, SOCK_STREAM, IPPROTO_IP) = 3`        | 创建 TCP socket，返回 fd=3 | `Socket::create_tcp()`                           | 创建监听 socket                |
| `setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0` | 开启地址复用               | `Socket::set_reuse_address()`                    | 允许服务端重启后更容易复用端口 |
| `bind(3, ... 0.0.0.0:9000 ...) = 0`                   | 绑定 9000 端口             | `Socket::bind_and_listen()`                      | 把 socket 绑定到本地地址       |
| `listen(3, 64) = 0`                                   | 进入监听状态，backlog=64   | `Socket::bind_and_listen()`                      | 开始等待客户端连接             |
| `accept(3, ... 127.0.0.1:43140 ...) = 4`              | 接受客户端连接，返回 fd=4  | `Socket::accept()` / `Server::run()`             | 得到客户端连接 socket          |
| `sendto(4, "WELCOME...", 47, ...) = 47`               | 向客户端发送欢迎信息       | `Server::handle_client()` / `Socket::send_all()` | 客户端连接后先发欢迎语         |
| `recvfrom(4, "ping\n", 1024, ...) = 5`                | 从客户端读取命令字节       | `Server::read_line()` / `Socket::receive()`      | 读取 TCP 字节流                |
| `sendto(4, "PONG\n", 5, ...) = 5`                     | 返回 PING 响应             | `execute_command()` / `send_all()`               | 执行业务后响应客户端           |
| `shutdown(4, SHUT_RDWR) = 0`                          | 关闭连接读写方向           | `Socket::shutdown_both()`                        | 处理 EXIT 后关闭连接           |



### 3.4 总结

1. `Socket::create_tcp()` 最终调用 Linux 的 `socket()`，返回监听 fd。
2. `set_reuse_address()` 最终调用 `setsockopt(SO_REUSEADDR)`。
3. `bind_and_listen()` 最终调用 `bind()` 和 `listen()`。
4. `Server::run()` 中的 `accept()` 会阻塞等待客户端连接，连接到来后返回新的 fd。
5. 监听 fd 和客户端连接 fd 不是同一个。监听 fd 是 3，客户端连接 fd 是 4。
6. 线程池 worker 线程负责对客户端连接执行 `send/recv`。
7. `send()` 在 strace 里可能显示为 `sendto()`，`recv()` 可能显示为 `recvfrom()`，这是 Linux 实现细节。
8. **`parse_command()`、`execute_command()`、`KeyValueStore` 是用户态逻辑**，**strace 看不到它们**，只能看到它们前后的网络系统调用。
9. `EXIT` 命令会触发 `shutdown(fd, SHUT_RDWR)`。
10. `SIGWINCH` 和 `ERESTARTSYS` 是终端信号打断阻塞 `accept()` 的表现，不是程序 bug。



















