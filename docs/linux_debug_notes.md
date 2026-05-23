# Linux 调试笔记：GDB 请求链路与 core dump

这份笔记记录两件事：

1. 使用 GDB 跟踪一次客户端请求从 `accept()` 到 `execute_command()` 的调用链。
2. 使用 core dump 分析一个崩溃程序，并理解 `core dump`、`bt`、`frame` 的含义。



## 1. 调试环境

- 项目：`mini_kv_server`
- 系统：Ubuntu
- 构建方式：CMake Debug 构建
- 调试器：GDB
- 客户端：`nc 127.0.0.1 9000`

Debug 构建命令：

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

启动 GDB：

```bash
gdb ./build-debug/mini_kv_server
```

设置断点：

```gdb
break minikv::server::Server::start
break minikv::net::Socket::accept
break minikv::server::Server::handle_client
break minikv::server::Server::execute_command
```

启动服务端：

```gdb
run --port 9000 --workers 4 --queue 64 --backlog 64
```

另开终端连接服务端：

```bash
nc 127.0.0.1 9000
```

发送测试命令：

```text
PING
SET 27 xiaodou
GET 27
KEYS
SIZE
DEL 27
EXIT
```

---

## 

## 2. 一次请求的整体调用链

一次客户端请求大致经过下面这条路径：

```text
Server::run()
  -> listen_socket_.accept()
  -> Socket::accept()
  -> std::make_shared<Socket>(std::move(*client))
  -> ThreadPool::submit(...)
  -> worker thread
  -> Server::handle_client(...)
  -> Server::read_line(...)
  -> Socket::receive(...)
  -> parse_command(line)
  -> Server::execute_command(command, should_close)
  -> KeyValueStore::set/get/erase/keys/size
  -> Socket::send_all(response)
```

这里最关键的点是：`accept()` 发生在主线程的 `Server::run()` 中，而 `handle_client()` 和 `execute_command()` 发生在线程池的 worker 线程中。

GDB 里出现类似输出：

```text
[Switching to Thread 0x7ffff6ffe6c0]
Thread 3 "mini_kv_server" hit Breakpoint ..., Server::handle_client(...)
```

这说明：主线程 accept 到客户端连接之后，把连接处理任务投递给线程池，真正处理客户端命令的是某个 worker 线程。



## 3. 从 accept 到线程池投递

`Server::run()` 中的核心逻辑：

```cpp
auto client = listen_socket_.accept();
if (!client.has_value()) {
    log_line("[server] accept failed: " + net::last_socket_error_message());
    continue;
}

auto client_ptr = std::make_shared<net::Socket>(std::move(*client));

const bool accepted = pool_.submit([this, client_ptr] {
    handle_client(client_ptr);
});
```

解释：

1. `listen_socket_.accept()` 阻塞等待客户端连接。
2. 连接到来后，`Socket::accept()` 返回一个新的客户端 socket。
3. 这个客户端 socket 被移动进 `std::shared_ptr<net::Socket>`。
4. **使用 `shared_ptr` 是为了让客户端连接对象在线程池任务执行期间保持存活**。
5. `pool_.submit(...)` 把 `handle_client(client_ptr)` 包装成任务提交给线程池。

这一步之后，客户端处理逻辑从主线程切换到 worker 线程。



## 4. handle_client 如何处理一条命令

`handle_client()` 的核心流程：

```cpp
client->send_all("WELCOME mini_kv_server.Type HELP for commands.\n");

std::string pending;
std::string line;

while (true) {
    const ReadLineStatus status = read_line(*client, pending, line);

    bool should_close = false;
    const core::Command command = core::parse_command(line);
    const std::string response = execute_command(command, should_close);

    if (!client->send_all(response)) {
        return;
    }

    if (should_close) {
        client->shutdown_both();
        return;
    }
}
```

解释：

1. 客户端连接成功后，服务端先发送欢迎信息。
2. `pending` 保存 TCP 字节流中暂时还没处理完的数据。
3. `read_line()` 从 socket 中读取数据，并按 `\n` 切出一条完整命令。
4. `parse_command(line)` 把字符串命令解析成 `Command` 结构体。
5. `execute_command()` 根据命令类型执行具体逻辑。
6. `send_all(response)` 把响应完整发送回客户端。



## 5. read_line 为什么重要

**TCP 是字节流，不保证一次 `recv()` 就刚好读到一条完整命令。**

所以不能这样理解：

```text
客户端 send 一次 PING
服务端 recv 一次就一定得到完整 PING
```

真实情况可能是：

```text
第一次 recv: "PI"
第二次 recv: "NG\nSET 27 xiaodou\n"
```

因此 `read_line()` 使用 `pending` 缓冲区：

```cpp
const std::size_t newline = pending.find('\n');
if (newline != std::string::npos) {
    line = pending.substr(0, newline);
    pending.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return ReadLineStatus::Ok;
}
```

这段逻辑的含义：

1. 如果 `pending` 中已经有 `\n`，说明至少有一条完整命令。
2. 截取 `\n` 前面的部分作为一行命令。
3. 从 `pending` 中删除已经处理过的部分。
4. 如果命令以 `\r\n` 结尾，去掉 `\r`，兼容 telnet / Windows 风格换行。
5. 如果还没有 `\n`，继续调用 `receive()` 读取更多字节。



## 6. execute_command 如何分发命令

当客户端输入：

```text
SET 27 xiaodou
```

`parse_command()` 会生成：

```text
type  = CommandType::Set
key   = "27"
value = "xiaodou"
```

然后进入：

```cpp
case core::CommandType::Set:
    store_.set(command.key, command.value);
    return "OK\n";
```

当客户端输入：

```text
GET 27
```

会进入：

```cpp
case core::CommandType::Get: {
    const auto value = store_.get(command.key);
    if (!value.has_value()) {
        return "NOT_FOUND\n";
    }
    return "VALUE " + *value + '\n';
}
```

当客户端输入错误命令时，会进入：

```cpp
case core::CommandType::Invalid:
    return "ERR " + command.error + '\n';
```

所以 `execute_command()` 是协议层到存储层之间的分发点。



## 7. GDB 中我观察到的关键现象

### 7.1 accept 后会进入 worker 线程

现象：

```text
[Switching to Thread 0x7ffff6ffe6c0]
Thread 3 "mini_kv_server" hit Breakpoint ..., Server::handle_client(...)
```

说明：

主线程负责监听和 accept，新连接被提交给线程池后，由 worker 线程执行 `handle_client()`。

### 7.2 shared_ptr 的 use_count 会变化

在 `handle_client()` 断点处，GDB 可能显示：

```text
client=std::shared_ptr<minikv::net::Socket> (use count 2, weak count 0)
```

说明：

此时至少有两个 `shared_ptr` 持有同一个客户端 socket：

1. `Server::run()` 里的 `client_ptr`。
2. lambda 任务中捕获的 `client_ptr`。

等任务真正执行后，外层临时对象结束，`use_count` 可能变成 1。这个现象说明 `shared_ptr` 确实在帮我们延长客户端连接对象的生命周期。

### 7.3 execute_command 是业务分发点

在 `execute_command()` 处查看 `command`，可以确认：

```gdb
print command.type
print command.key
print command.value
```

这能证明：网络收到的是字符串，经过 `read_line()` 和 `parse_command()` 后，已经变成了结构化命令。



## 8. 本次 GDB 调试结论

这次调试后，我确认了 mini_kv_server 的请求处理路径：

1. `Server::run()` 在主线程中循环调用 `accept()`。
2. `Socket::accept()` 返回客户端连接 socket。
3. 服务端把客户端 socket 移动进 `shared_ptr`，保证连接对象在线程池任务中存活。
4. `ThreadPool::submit()` 把客户端处理逻辑投递给 worker。
5. worker 执行 `handle_client()`。
6. `read_line()` 使用 `pending` 从 TCP 字节流中切出完整命令。
7. `parse_command()` 把文本命令解析成 `Command`。
8. `execute_command()` 根据命令类型调用 `KeyValueStore`。
9. `send_all()` 把响应发回客户端。

这条链路说明：项目不是简单地“收到字符串然后返回字符串”，而是包含了连接生命周期管理、线程池任务投递、TCP 字节流解析、命令解析和线程安全 KV 存储。



## 9. core dump 实验

为了理解 Linux 下崩溃后如何定位问题，我写了一个必然崩溃的小程序：

```cpp
int main() {
    int* p = nullptr;
    *p = 1;
}
```

编译时加 `-g`：

```bash
g++ -g crash.cpp -o crash
```

允许系统生成 core 文件：

```bash
ulimit -c unlimited
```

如果 Ubuntu 没有在当前目录生成 `core` 文件，可以临时设置：

```bash
echo "core" | sudo tee /proc/sys/kernel/core_pattern
```

运行程序触发崩溃：

```bash
./crash
```

使用 GDB 打开 core：

```bash
gdb ./crash core
```

---

## 

## 10. core dump / bt / frame 的正确理解

### 10.1 core dump 是什么

我的理解：

**`core dump` 是程序崩溃时，操作系统保存下来的一份进程现场快照。**

里面通常包含：

1. 崩溃时的调用栈。
2. 寄存器状态。
3. 部分内存内容。
4. 线程状态。
5. 程序崩溃时所在位置。

它的作用是：程序已经崩溃退出后，仍然可以用 GDB 回到崩溃现场进行分析。

### 10.2 bt 是什么

**`bt` 是 `backtrace` 的缩写，用于查看当前线程的调用栈。**

更准确地说：

**`bt` 显示程序崩溃或暂停时，当前线程是通过哪些函数一步步调用到当前位置的。**

例如：

```text
#0  crash() at crash.cpp:3
#1  main() at crash.cpp:7
```

含义是：

1. `#0` 是当前崩溃现场，程序在 `crash()` 里崩溃。
2. `#1` 是调用者，说明 `main()` 调用了 `crash()`。

如果是多线程程序，`bt` 默认只看当前线程。想看所有线程调用栈，可以用：

```gdb
thread apply all bt
```

### 10.3 frame 是什么

**`frame` 用于切换调用栈中的某一层。**

更准确地说：

每一个 `#0`、`#1`、`#2` 都是一个栈帧，也就是一次函数调用现场。`frame 0` 表示切到崩溃现场，`frame 1` 表示切到调用者那一层。

常见用法：

```gdb
bt
frame 0
list
info locals
print p
```

含义：

1. `bt` 先看调用栈。
2. `frame 0` 进入崩溃那一层。
3. `list` 查看当前栈帧附近源码。
4. `info locals` 查看当前函数局部变量。
5. `print p` 查看指针变量的值。



## 11. core dump 调试流程总结

一次标准 core dump 分析流程是：

```bash
g++ -g crash.cpp -o crash
ulimit -c unlimited
./crash
gdb ./crash core
```

进入 GDB 后：

```gdb
bt
frame 0
list
info locals
print p
```

如果是多线程程序：

```gdb
info threads
thread apply all bt
```

这套流程能回答三个问题：

1. 程序崩在哪里？
2. 它是怎么调用到这里的？
3. 崩溃现场的关键变量是什么？



## core dump 学习结论

这次实验后，我对 `core dump`、`bt`、`frame` 的理解是：

1. `core dump` 是程序崩溃时的进程现场快照，方便后续调试
2. `bt` 用于查看当前线程的调用栈
3. `frame` 用于切换调用栈中的某一层函数调用现场
4. `frame 0` 通常是崩溃发生的最直接位置
5. `frame 1`、`frame 2` 可以帮助理解上层调用关系
6. `list`、`info locals`、`print` 用于查看当前 `frame`下的源码和变量

对于 `mini_kv_server` 这种服务端程序来说，`core dump` 的价值在于：如果服务端线程池、`socket` 处理或命令执行过程中崩溃，可以通过 `core` 文件在程序崩溃退出后继续分析崩溃现场。



