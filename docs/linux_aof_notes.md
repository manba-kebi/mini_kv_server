# Linux 下 AOF 持久化与手动恢复验证笔记

这份笔记记录了对 `mini_kv_server` 的 AOF(Append Only File)持久化功能进行手动验证的过程。



验证的核心逻辑是：

1. 启动支持 AOF 的服务端，写入若干数据。
2. 观察生成的 `minikv.aof` 文件，验证其是否只记录”改写状态的命令“。
3. 异常关闭服务端（模拟宕机），保留AOF文件。
4. 重新启动服务端，验证服务端是否能自动读取 AOF 文件并恢复之前有关 `SET`、`DEL` 操作的所有数据。



---



## 1. 调试环境与准备

- **项目分支**：支持 Epoll 与 AOF 的新版本 

- **系统环境**：Ubuntu / Linux 
- **构建方式**：CMake Debug/Release 构建 
- **AOF 配置文件/参数**：开启 AOF 选项

如下图所示，能正常构建并运行。

![build](C:\Users\28251\Desktop\mini_kv_server\docs\images\epoll\build.png)

---



## 2. 实验步骤与截图记录 

### 启动服务并写入测试数据 

首先，我们清理旧的 AOF 文件，启动 `mini_kv_server`。 

然后，使用客户端连接服务端，依次发送以下指令： 

- `SET name akai` 、`SET akai student`、`GET name`
- `GET name` 、`GET akai`
- `DEL name`

之后断开服务端服务，再重新开启服务端，依次发送以下指令：

- `GET name` 、`GET akai`



在此过程中，`GET` 只读命令不会写入 AOF，只有 `SET` 、`DEL`  这类改写状态的命令才会被持久化。

![test_aof_flag](C:\Users\28251\Desktop\mini_kv_server\docs\images\epoll\test_aof_flag.png)



并且如上图所示，在验证过程中也多次使用命令 `cat data/minikv.aof` 获取文件内容，并且在服务端断开连接重连后，也能正常读取到 AOF 文件的内容。

并且也能观察到：AOF 文件内只保留了 `SET` 、`DEL`命令，且格式为标准的原始协议命令，没有包含任何 `GET` 命令。这符合 AOF “只记录改写状态的命令” 的预期，避免了不必要的磁盘 IO 开销。



### 服务端重启并加载AOF日志

![log_and_aof](C:\Users\28251\Desktop\mini_kv_server\docs\images\epoll\log_and_aof.png)

通过之前我们在重启服务端，并在客户端执行命令 `GET name` 、`GET akai` 得到的结果 `NOT_FOUND`、`VALUE student` 可知，虽然内存曾经清空，但通过 AOF 的重放（Replay），数据已经被完美恢复。



## 思考

1. **为什么只记录改写状态的命令（`SET`、`DEL`），不记录读命令（`GET`）?**

​	AOF 的目的是为了在恢复时 "重建状态" 。读命令不会改变内存中的数据状态，如果记录读命令，会导致 AOF 文件无意义的膨胀，白白浪费磁盘IO性能和恢复时的加载时间。

2. **AOF恢复的本质是什么？**

​	本质上是一个命令重放（`Command Replay`）的过程。服务端在启动时，扮演了一个“特殊的客户端”，读取minikv.aof 中的每一行，然后像执行普通客户端命令一样，把它们输入给底层的 KeyValueStore 重新执行一遍。
