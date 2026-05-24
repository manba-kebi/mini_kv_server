# benchmark 与背压测试笔记

这份笔记记录两组实验：

1. 对比不同 `workers` 数量对吞吐的影响。
2. 通过小 `queue` 验证有界任务队列的背压效果。



## 1. 当前服务端模型

mini_kv_server 当前采用的是“一个连接对应一个线程池任务”的模型。

`Server::run()` 中，每当 `accept()` 成功得到一个客户端连接，就会提交一个任务到线程池：

```cpp
const bool accepted = pool_.submit([this, client_ptr] {
    handle_client(client_ptr);
});
```

注意：这里提交的任务不是一条命令，而是“处理一个客户端连接”。

`handle_client()` 内部会循环读取这个客户端发来的命令：

```cpp
while (true) {
    const ReadLineStatus status = read_line(*client, pending, line);
    const core::Command command = core::parse_command(line);
    const std::string response = execute_command(command, should_close);
    client->send_all(response);
}
```

所以当前模型可以概括为：

```text
workers 控制同时处理多少个客户端连接
max_queue_size 控制最多缓存多少个等待处理的连接任务
backlog 控制 accept 之前内核最多缓存多少个等待连接
```

---

 

## 2. workers 数量对吞吐的影响

![benchmark](C:\Users\28251\Desktop\mini_kv_server\docs\images\benchmark.png)

### 2.1 测试方式

服务端分别使用 1、4、8 个 worker 启动：

```bash
./build/mini_kv_server --port 9000 --workers 1 --queue 64 --backlog 64
python3 tools/benchmark.py

./build/mini_kv_server --port 9000 --workers 4 --queue 64 --backlog 64
python3 tools/benchmark.py

./build/mini_kv_server --port 9000 --workers 8 --queue 64 --backlog 64
python3 tools/benchmark.py
```

`benchmark.py` 当前配置：

```python
CLIENTS = 8
ROUNDS = 500
```

每个客户端连接会循环发送：

```text
SET key value
GET key
```

所以总请求数：

```text
8 * 500 * 2 = 8000
```

### 2.2 实测结果

| workers | clients | rounds | requests | seconds | qps      |
| ------- | ------- | ------ | -------- | ------- | -------- |
| 1       | 8       | 500    | 8000     | 0.071   | 112316.7 |
| 4       | 8       | 500    | 8000     | 0.100   | 80242.7  |
| 8       | 8       | 500    | 8000     | 0.105   | 76548.2  |

### 2.3 现象分析

这次测试中，`workers=1` 反而 QPS 最高，主要原因是：

1. 测试运行在本机 loopback，网络延迟极低。
2. 每条命令非常短，业务逻辑也非常轻。
3. `workers=1` 基本没有多线程竞争，执行路径更简单。
4. **`workers=4/8` 会产生更多线程调度、上下文切换和锁竞争**。
5. 所有 worker 共享同一个 `KeyValueStore`，`SET/GET` 都会访问同一个存储对象。
6. 多个 Python 客户端线程同时跑，也会引入客户端侧调度开销。

所以这次 benchmark 测到的不是“网络带宽上限”，而是一个本机、小请求、短连接压力下的综合开销。

### 2.4 应该怎么理解 workers 的作用

`workers` 的价值不一定体现在这个 benchmark 的 QPS 上。

`workers=1` 的特点：

```text
吞吐在这个微型测试中可能更高
但所有客户端连接串行处理
如果某个客户端很慢，会阻塞后面的连接任务
并发公平性差
```

`workers=4/8` 的特点：

```text
能同时服务多个客户端连接
慢客户端不容易拖住所有连接
并发连接的响应延迟更稳定
但会带来线程调度和锁竞争
```

所以不能简单说：

```text
worker 越多，QPS 一定越高
```

更准确的结论是：

> 在当前本机短命令 benchmark 中，增加 worker 没有提高吞吐，反而因为调度和锁竞争导致 QPS 下降。但 worker 的意义主要是提升并发连接处理能力和避免单个慢客户端阻塞所有连接，而不是保证微型压测下 QPS 单调上升。



## 3.背压测试

这次测试在终端上的结果有点长，我将终端相关内容放在了[docs/pressure_error.txt](docs/pressure_error.txt)。

### 3.1 测试方式

服务端使用极小线程池和极小队列启动：

```bash
./build/mini_kv_server --port 9000 --workers 1 --queue 1 --backlog 64
```

然后运行：

```bash
python3 tools/benchmark.py
```

### 3.2 观察到的现象

Python 客户端出现：

```text
BrokenPipeError: [Errno 32] Broken pipe
```

错误位置大致在：

```python
sock.sendall((cmd + "\n").encode())
```

### 3.3 BrokenPipeError 是什么

`BrokenPipeError` 表示：

```text
客户端还想继续往 socket 写数据，但对端已经关闭了连接。
```

在这次测试中，对端就是 `mini_kv_server`。

这不是服务端崩溃，而是服务端主动关闭了某些连接。

### 3.4 为什么 queue=1 会触发这个问题

当前服务端逻辑：

```cpp
const bool accepted = pool_.submit([this, client_ptr] {
    handle_client(client_ptr);
});

if (!accepted) {
    client_ptr->send_all("BUSY server queue is full ,retry later\n");
    client_ptr->shutdown_both();
    client_ptr->close();
}
```

当配置为：

```text
workers=1
queue=1
clients=8
```

大致过程是：

```text
1 个客户端连接被 worker 正在处理
1 个客户端连接任务进入线程池队列等待
后续更多客户端连接被 accept 后，submit() 失败
服务端发送 BUSY
服务端 shutdown + close 这些连接
```

此时 benchmark 脚本的问题是：

```python
sock.recv(4096)
```

它只是读了一次服务端响应，但没有判断读到的是：

```text
WELCOME ...
```

还是：

```text
BUSY server queue is full ,retry later
```

如果某个客户端读到的是 `BUSY`，说明服务端已经准备关闭连接。但 benchmark 仍然继续：

```python
request(sock, f"SET {key} value")
request(sock, f"GET {key}")
```

于是客户端继续向已经被服务端关闭的 socket 写数据，就会出现：

```text
BrokenPipeError: [Errno 32] Broken pipe
```

### 3.5 这个现象是否说明背压生效

是的，这个现象可以说明背压生效了。

因为服务端在线程池队列满时没有无限接收任务，而是：

```text
返回 BUSY
关闭连接
保护自身
```

但是当前 benchmark 脚本不够健壮，它没有识别 `BUSY`，所以表现成 Python 异常。

## 4. 有界队列是背压，不是性能优化

`max_queue_size` 的作用不是让服务变快，而是防止服务被压垮。

如果线程池队列是无界的，那么在请求速度大于处理速度时：

```text
任务会无限堆积
每个任务可能持有 socket、shared_ptr、lambda 等资源
内存持续增长
延迟持续变大
最终服务可能崩溃
```

有界队列的策略是：

```text
队列未满：接受任务
队列已满：拒绝任务，返回 BUSY
```

这就是背压。

背压的核心目的：

```text
让系统在过载时明确拒绝一部分请求，而不是无限排队拖垮整个服务。
```

所以应该这样理解：

> 有界队列牺牲了一部分请求成功率，但换来了服务端内存和延迟的可控性。它不是提升 QPS 的性能优化，而是过载保护机制。

## 5. 当前测试的改进

增加了一个专门的背压测试脚本[tools/backpressure_test.py](tools/backpressure_test.py)，逻辑是：

1. 多个客户端同时连接服务端。
2. 每个客户端先读取第一条响应。
3. 如果读到 `WELCOME`，说明连接进入正常处理。
4. 如果读到 `BUSY`，说明服务端拒绝连接任务。
5. 统计 `WELCOME` 和 `BUSY` 数量。
6. 不要在收到 `BUSY` 后继续发命令。



## 6. 结论

这两天测试后的结论是：

1. 当前 benchmark 中 `workers=1` QPS 最高是合理现象，不代表线程池设计错误。
2. 原因主要是本机短请求测试中，多线程带来的调度和锁竞争开销超过了并行收益。
3. 当前服务端是“一个连接一个任务”。
4. `workers` 的主要价值是并发连接处理能力和抗慢客户端能力，不保证 QPS 随 worker 增加而增加。
5. `queue=1` 时出现 `BrokenPipeError`，本质是服务端触发背压后关闭了部分连接。
6. 当前 benchmark 没有识别 `BUSY`，所以继续写关闭后的 socket，导致 BrokenPipe。
7. 有界队列的意义是过载保护，不是性能优化。
8. 设计了专门的 `backpressure_test.py` 统计 `BUSY` 数量，更准确的验证背压。

