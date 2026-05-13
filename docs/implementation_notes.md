# MiniKV Server 实现笔记



## 1. Server 为什么可以不写析构函数

`Server` 没有显式写析构函数，这是可以的。

原因是 `Server` 的成员对象已经各自负责自己的资源释放：

```cpp
Serverconfig config_;
net::Socket listen_socket_;
core::ThreadPool pool_;
core::KeyValueStore store_;
std::atomic<bool> started_{false};
```

其中：

- `listen_socket_` 是 `Socket`，析构时会自动调用 `close()`。
- `pool_` 是 `ThreadPool`，析构时会调用 `shutdown()`，唤醒并 join 所有 worker 线程。
- `store_`、`config_`、`started_` 不需要手动释放。

所以编译器默认生成的析构函数已经足够。这正是 RAII 的好处：外层对象不需要手动写一堆清理逻辑，成员对象会按构造的逆序自动析构。

如果想显式写析构函数，可以写：

```cpp
~Server() = default;
```

不要只声明：

```cpp
~Server();
```

如果只声明不定义，链接阶段会报 unresolved external symbol。

## 2. static 成员函数为什么定义处不写 static

`socket.h` 中：

```cpp
class Socket {
public:
    static std::optional<Socket> create_tcp();
};
```

`socket.cpp` 中：

```cpp
std::optional<Socket> Socket::create_tcp() {
    const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle == kInvalidSocket) return std::nullopt;
    return Socket(handle);
}
```

这是正确写法。

`static` 写在类内声明处，表示这个函数属于类本身，不依赖某个具体对象。调用时可以写：

```cpp
auto socket = net::Socket::create_tcp();
```

类外定义时不再写 `static`。如果写成：

```cpp
static std::optional<Socket> Socket::create_tcp() {
}
```

反而是不合法的。成员函数是否为 `static`，由类内声明决定。

## 3. SocketRuntime：把平台初始化交给对象生命周期

Windows 下使用 socket API 前必须调用：

```cpp
WSAStartup(...)
```

结束时调用：

```cpp
WSACleanup()
```

Linux/macOS 不需要这一步。项目通过 `SocketRuntime` 把平台差异封装起来：

```cpp
minikv::net::SocketRuntime runtime;
if (!runtime.ok()) {
    std::cerr << "Failed to initialize socket runtime\n";
    return 1;
}
```

这个对象在 `main()` 一开始创建，保证后续 socket 操作前 Winsock 已经初始化。程序退出时，它的析构函数会自动执行清理。

这类“必须成对出现”的初始化和清理，非常适合用 RAII 封装。

## 4. Socket 为什么要禁止拷贝

`Socket` 内部保存的是原生 socket 句柄：

```cpp
NativeSocket handle_ = kInvalidSocket;
```

这个句柄代表操作系统资源，不是普通业务数据。

如果允许拷贝，会出现两个 C++ 对象持有同一个底层句柄的问题：

```cpp
Socket a(handle);
Socket b = a;
```

此时 `a` 和 `b` 都认为自己应该关闭这个 socket。它们析构时可能重复关闭同一个句柄。

所以代码中删除了拷贝构造和拷贝赋值：

```cpp
Socket(const Socket&) = delete;
Socket& operator=(const Socket&) = delete;
```

只允许移动：

```cpp
Socket(Socket&& other) noexcept {
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
}
```

移动后必须把原对象置为无效句柄，否则原对象析构时仍然会关闭资源。

## 5. send_all 为什么要循环发送

`send()` 不保证一次把所有数据发完。它返回的是本次实际发送出去的字节数。

所以 `send_all()` 要维护两个变量：

```cpp
const char* current = data.data();
std::size_t remaining = data.size();
```

- `current` 指向还没发送的数据起点。
- `remaining` 表示还剩多少字节没发。

每次发送成功后：

```cpp
current += sent;
remaining -= static_cast<std::size_t>(sent);
```

直到 `remaining == 0` 才说明整段响应都发完了。

另外，Windows 下 `send()` 的长度参数是 `int`，而 `std::string_view::size()` 返回 `size_t`。因此代码用：

```cpp
const auto max_send_size = static_cast<std::size_t>((std::numeric_limits<int>::max)());
const auto chunk_size = static_cast<int>(std::min<std::size_t>(remaining, max_send_size));
```

把单次发送长度限制在 `int` 能表示的范围内。

`(std::numeric_limits<int>::max)()` 这种写法是为了规避 Windows 头文件中可能出现的 `max` 宏污染。

## 6. read_line 为什么需要 pending

TCP 是字节流，不是消息队列。客户端输入一条命令，不代表服务端一次 `recv()` 就刚好读到一条完整命令。

可能出现：

```text
SET 27 xiao
```

下一次才收到：

```text
dou\n
```

也可能一次收到：

```text
PING\nSIZE\nEXIT\n
```

所以 `read_line()` 使用 `pending` 保存“已经收到但还没形成完整命令”的数据：

```cpp
const std::size_t newline = pending.find('\n');
if (newline != std::string::npos) {
    line = pending.substr(0, newline);
    pending.erase(0, newline + 1);
}
```

只有遇到 `\n` 才认为一条命令完整。

如果行尾是 `\r\n`，代码会去掉 `\r`：

```cpp
if (!line.empty() && line.back() == '\r') {
    line.pop_back();
}
```

这是为了兼容 telnet 和一些 Windows 风格文本协议。

## 7. ThreadPool 的核心模型

线程池维护一组 worker：

```cpp
std::vector<std::thread> workers_;
std::queue<Task> tasks_;
```

worker 线程都执行同一个循环：

```cpp
worker_loop();
```

当任务队列为空时，线程不应该一直空转浪费 CPU，而是通过条件变量睡眠：

```cpp
condition_.wait(lock, [this] {
    return stopping_ || !tasks_.empty();
});
```

predicate 的作用是防止虚假唤醒，同时表达线程继续运行的条件：

- `stopping_ == true`：线程池正在停止。
- `!tasks_.empty()`：队列里有任务可取。

取任务时需要持有锁：

```cpp
task = std::move(tasks_.front());
tasks_.pop();
```

但执行任务时不能继续持有锁，否则一个 worker 执行任务期间，其他 worker 都无法从队列取任务，会削弱并发效果。

## 8. 有界队列和背压

`ThreadPool::submit()` 中有：

```cpp
if (tasks_.size() >= max_queue_size_) return false;
```

这表示队列是有上限的。

如果队列无限增长，短时间看起来所有任务都被接受了，但实际上可能出现：

- 内存持续上涨。
- 请求等待时间越来越长。
- 服务看似还活着，但延迟已经不可接受。

因此队列满时拒绝新任务，服务端返回：

```text
BUSY server queue is full ,retry later
```

这是一种简单的背压策略。

## 9. KeyValueStore 为什么用 shared_mutex

KV 存储使用：

```cpp
std::unordered_map<std::string, std::string> data_;
mutable std::shared_mutex mutex_;
```

`unordered_map` 本身不是线程安全的，多个线程同时读写必须加锁。

读操作使用共享锁：

```cpp
std::shared_lock<std::shared_mutex> lock(mutex_);
```

写操作使用独占锁：

```cpp
std::unique_lock<std::shared_mutex> lock(mutex_);
```

这样多个 `GET` 可以并发执行，而 `SET/DEL` 会独占访问。这个设计适合读多写少的场景。

`mutex_` 写成 `mutable`，是因为 `get()`、`keys()`、`size()` 是 `const` 成员函数。加锁会修改 mutex 内部状态，但不会修改 KV 存储对外表现出来的逻辑数据，所以这里使用 `mutable` 是合理的。

## 10. Command 解析为什么单独拆出来

`parse_command()` 只负责把文本解析成结构化命令：

```cpp
Command parse_command(std::string_view line);
```

例如：

```text
SET 27 xiaodou
```

解析后得到：

```cpp
Command{
    CommandType::Set,
    "27",
    "xiaodou",
    {}
}
```

它不直接操作 `KeyValueStore`。真正执行命令的是 `Server::execute_command()`。

这样拆分的好处是：

- 网络收发、命令解析、业务执行分离。
- 后续可以单独测试命令解析。
- 添加新命令时，修改边界更清楚。

## 11. telnet 下 Backspace 不能正常删除的原因

这不是服务端故意设计成不能删除，而是因为当前使用 `telnet` 测试。

在某些 telnet 客户端里，Backspace 不一定由本地终端处理成“删除前一个字符”，而可能作为控制字符发送给服务端，例如：

```text
\b
```

或：

```text
0x7f
```

当前 `read_line()` 没有解释这些控制字符，所以它不会把 Backspace 当成删除操作。

后续可以有两种优化：

- 在 `read_line()` 中识别 `\b` 和 `0x7f`，并删除 `pending` 中的上一个字符。
- 写一个专门的客户端程序，在客户端侧完成输入编辑，再把最终命令发给服务端。

第二种更符合职责划分；第一种适合作为练习。
