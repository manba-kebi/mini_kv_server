#include "minikv/server/epoll_server.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "asynclogger/async_logger.h"
#include "minikv/net/socket_runtime.h"

namespace minikv::server {
    namespace {
        constexpr int kEpollTimeoutMs = 1000;   // epoll 等待超时时间 1000 毫秒

        bool would_block() {
            return errno == EAGAIN || errno == EWOULDBLOCK;
            // EAGAIN：资源暂时不可用，请重试
            // EWOULDBLOCK：操作会阻塞（在非阻塞模式下）
            // 这两个错误码在 Linux 中实际相同，表示"现在没数据，但不代表出错"
        }

        // 将文件设置为非阻塞模式
        bool set_non_blocking(int fd) { // fd: 要设置的文件描述符
            const int flags = ::fcntl(fd,F_GETFL,0);    //获取 fd 当前的文件状态标志
            if (flags == -1) {      //获取失败
                return false;
            }
            //在原有标志上添加 O_NONBLOCK （非阻塞标志）
            return ::fcntl(fd,F_SETFL,flags | O_NONBLOCK) == 0;
        }


        std::string join_keys(const std::vector<std::string>& keys) {
            std::ostringstream output;
            output <<"KEYS";
            for (const auto& key : keys) {
                output<< ' ' <<key;
            }
            output << '\n';
            return output.str();    //返回拼接后的完整字符串
        }

        // 基础连接事件集合
        std::uint32_t base_connection_events() {
            return EPOLLIN | EPOLLRDHUP | RPOLLERR | EPOLLHUP;
            // EPOLLIN：    数据可读（客户端发来了数据）
            // EPOLLRDHUP： 客户端半关闭连接（对方调用了 shutdown(SHUT_WR)）
            // EPOLLERR：   发生错误
            // EPOLLHUP：   连接挂起（对方完全关闭了连接）
        }
    }  //namespace

    // 初始化服务器配置和日志
    EpollServer::EpollServer(EpollServerConfig config,asynclogger::AsyncLogger& logger): config_(std::move(config)),logger_(logger){}

    // 清理资源
    EpollServer::~EpollServer() {
        if (epoll_fd_ != -1) {
            ::close(epoll_fd_);
        }
    }

    bool EpollServer::start() {
        //AOF 持久化
        if (!config_.aof_path.empty()) {
            //创建 AOF 对象
            aof_ = std::make_unique<core::AppendOnlyFile>(config_.aof_path);
            std::string error;
            //把磁盘上的历史数据加载回内存
            if (!aof_->replay(store_,error)) {
                logger_.error("[epoll] AOF replay failed: " + error);
                return false;
            }
            logger_.info("[epoll] AOF replay completed: " + config_.aof_path.string());
        }

        // 创建 TCP 监听套接字
        auto socket = net::Socket::create_tcp();
        if (!socket.has_value()) {
            logger_.error("[epoll] Failed to create listen socket:" + net::last_socket_error_message());
            return false;
        }

        //设置地址重用（避免重启时“端口已被占用”的问题）
        if (!socket->set_reuse_address()) {
            logger_.warn("[epoll] warning: failed to enable SO_REUSEADDR");
        }

        //绑定端口并开始监听
        if (!socket->bind_and_listen(config_.port,config_.backlog)) {
            logger_.error("[epoll] bind/listen failed: " + net::last_socket_error_message());
            return false;
        }

        // 创建 epoll 实例
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        // epoll_create1：Linux 系统调用，创建一个 epoll 实例
        // 返回的文件描述符 epoll_fd_ 代表这个 epoll 实例
        // EPOLL_CLOEXEC：exec 新程序时自动关闭此 fd（防止泄露）
        if (epoll_fd_ == -1) {      //创建失败
            logger_.error("[epoll] epoll_create1 failed: " + std::string(std::strerror(errno)));
            return false;
        }

        //将监听套接字加入 epoll 监控
        // epoll 会监控这个 fd 上的事件（这里只监控EPOLLIN，即可读事件）
        if (!add_fd(listen_socket.native_handle(),EPOLLIN)) {
            // native_handle() 返回底层的 fd 整数
            // 当有新连接到来时，listen socket 变为可读
            logger_.error("[epoll] failed to add listen socket to epoll");
            return false;
        }

        // 标记启动成功
        started_ = true;
        logger_.info(
            "[epoll] listening on port " + std::to_string(config_.port) +
            ", backlog=" + std::to_string(config_.backlog) +
            ", max_events=" + std::to_string(config_.max_events)
        );
        return true;
    }

    void EpollServer::run() {
        if (!started_) {
            logger_.error("[epoll] start() must succeed before run()");
            return;
        }

        //预分配事件数组，避免每次循环分配内存
        std::vector<epoll_event> events(static_cast<std::size_t>(config_.max_events));
        //epoll_event 结构体包括
        //   events: 发生的事件类型（EPOLLIN/EPOLLOUT等）
        //   data:   用户数据（这里存的是 fd）

        while (true) {
            // 等待事件发生，timeout 后也会返回
            const int ready = ::epoll_wait(
                epoll_fd_,                          // epoll 实例
                events.data(),                      // 输出参数，存储就绪的事件
                static_cast<int>(events.size()),    // 最多处理的事件数
                kEpollTimeoutMs                     // 超时时间（毫秒）
                );

            if (ready < 0) {        // 返回负数表示出错
                if (errno = EINTR) {        // EINTR：被信号中断
                    continue;    // 这不算错，重新等待
                }
                logger_.error("[epoll] epoll_wait failed: " + std::string(std::strerror(errno)));
                continue;
            }

            //遍历所有就绪的事件
            for (int i = 0; i<ready ;i++) {
                // 从事件中提取fd和事件类型
                const int fd = events[static_cast<std::size_t>(i)].data.fd;
                const std::uint32_t event_mask = events[static_cast<std::size_t>(i)].events;

                if (fd == listen_socket_.native_handle()) {
                    // 如果是监听套接字就绪，说明有新连接
                    accept_new_connection();
                }else {
                    // 否则是客户端连接有数据或事件
                    handle_connection_event(fd,event_mask);
                }
            }

            // 超时后清理空闲连接（超时时间到或被信号中断都会执行到这里）
            expire_idle_connections();
        }
    }

    //向 epoll 添加要监控的文件描述符
    bool EpollServer::add_fd(int fd,std::uint32_t events) {
        epoll_event event{};
        event.events = events;      // 设置关心的事件
        event.data.fd = fd;         // 设置关联数据（这里是fd本身）
        // EPOLL_CTL_ADD：添加操作
        return ::epoll_ctl(epoll_fd_,EPOLL_CTL_ADD,fd,&event) == 0;
    }

    // 修改已在 epoll 中的 fd 的监控事件
    bool EpollServer::update_fd(int fd,std::uint32_t events) {
        epoll_event event{};
        event.events = events;
        event.data.fd = fd;
        // EPOLL_CTL_MOD：修改操作
        return ::epoll_ctl(epoll_fd_,EPOLL_CTL_MOD,fd,&event) == 0;
    }

    //接受新连接
    void EpollServer::accept_new_connection() {
        while (true) {      //循环接受所有待处理的连接
            auto client = listen_socket_.accept();
            //accept() 从全连接队列中取出一个客户端连接
            //返回新的 socket 对象，代表与这个客户端的连接

            if (!client.has_value()) {      //失败
                if (would_block()) {        //队列空了，没有更多连接
                    return;
                }
                logger_.error("[epoll] accept failed: " + net::last_socket_error_message());
                return ;
            }

            //获取新连接的 fd
            const int fd = client->native_handle();

            //设置为非阻塞模式
            if (!set_non_blocking(fd)) {
                logger_.warn("[epoll] failed to set client non-blocking");
                continue;
            }

            //创建新对象
            Connection connection;
            connection.socket = std::move(*client);     //移动socket对象
            //发送欢迎消息
            connection.output = "WELCOME mini_kv_server epoll. Type HELP for commands.\n";
            //记录最后活跃的时间（用于超时断开）
            connection.last_active = std::chrono::steady_clock::now();

            //将新连接的 fd 加入 epoll 监控
            if (!add_fd(fd,base_connection_events() | EPOLLOUT)) {
                // 关心的事件包括基本事件 + EPOLLOUT（可写，为了发送欢迎消息）
                logger_.warn("[epoll] failed to add client fd to epoll");
                continue;
            }

            //保存连接
            connections_.emplace(fd,std::move(connection));
        }
    }

    // 事件分发 根据事件的类型分发给不同的处理函数
    void EpollServer::handle_connection_event(int fd,std::uint32_t events) {
        // 在连接表中查找这个fd
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return ;        //找不到就返回（可能已被关闭）
        }

        // 优先级1：检查是否发生错误或连接关闭
        // EPOLLERR: 发生错误
        // EPOLLHUP: 连接挂断（对方完全关闭）
        // EPOLLRDHUP: 对方半关闭（不在发送数据）
        if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
            close_connection(fd);       // 直接关闭连接
            return;
        }

        // 优先级2: 有数据可读（客户端发来了请求）
        if ((events & EPOLLIN)!=0) {
            handle_read(fd,it->second);
            // 重新查找，因为 handle_read 可能已经关闭了连接
            it = connections_.find(fd);
            if (it == connections_.end()) {
                return ;    // 连接已关闭，不再处理后续事件
            }
        }

        // 优先级3: 可以发送数据了（响应准备好或缓冲区有空位）
        if ((events & EPOLLOUT) != 0) {
            handle_write(fd,it->second);
        }
    }

    // 从连接读取数据，按行分割成命令，执行命令并准备响应
    void EpollServer::handle_read(int fd,Connection& connection) {
        std::array<char,4096> buffer{};     //4KB 的读取缓冲区

        while (true) {  //外层循环 :
            // 系统调用 recv: 从 fd 读取数据到 buffer
            const ssize_t received = ::recv(fd,buffer.data(),buffer.size(),0);

            if (received > 0) {
                // 读到了数据，更新活跃时间
                connection.last_active = std::chrono::steady_clock::now();
                // 追加到连接的输入缓冲区
                connection.input.append(buffer.data(),static_cast<std::size_t>(received));
            }else if (received == 0) {
                // 返回 0 表示对方正常关闭了连接（EOF）
                connection.closing = true;
                break;
            }else {
                // 返回-1表示出错
                if (would_block()) {    //没数据可读了
                    break;
                }
                close_connection(fd);   // 真正的错误，关闭连接
                return;
            }

            //内层循环: 解析缓冲区中的完整命令
            while (true) {
                //查找换行符（命令以\n结尾）
                const std::size_t newline = connection.input.find('\n');

                if (newline == std::string::npos) { //没找到换行符
                    // 检查是否命令太长
                    if (connection.input.size() > config_.max_line_length) {
                        connection.output += "ERR command line is too long\n";
                        connection.closing = true;      //太长就断开
                    }
                    break;     // 跳出内层循环，继续读取更多数据
                }

                // 提取一行完整命令
                std::string line = connection.input.substr(0,newline);
                connection.input.erase(0,newline+1);    //从缓冲区删除这一行

                // 处理 Windows 风格的 \r\n
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();    // 去掉末尾的 \r
                }

                // 解析并执行命令
                bool should_close = false;
                const core::Command command = core::parse_command(line);
                connection.output += execute_command(command,should_close);

                if (should_close) {     // 如果命令要求关闭（如EXIT）
                    connection.closing = true;
                    break;
                }
            }

            if (connection.closing) {
                break;
            }
        }

        // 如果连接要关闭且输出缓冲区为空，直接关闭
        if (connection.closing && connection.output.empty()) {
            close_connection(fd);
            return ;
        }

        // 更新 epoll 监控的事件
         refresh_interest(fd,connection);
    }

    // 发送响应数据 把响应数据发送给客户端，处理发送缓冲区满的情况
    void EpollServer::handle_write(int fd,Connection& connection) {
        //循环发送，直到输出缓冲区为空
        while (!connection.output.empty()) {
            // 系统调用 send：将数据发送给客户端
            const ssize_t  sent = ::send(fd,connection.output.data(),connection.output.size(),0);
            if (sent>0) {
                //发送成功，更新活跃时间
                connection.last_active = std::chrono::steady_clock::now();
                //从缓冲区删除已发送的数据
                connection.output.erase(0,static_cast<std::size_t>(sent));
                continue;   //继续发送剩余数据
            }

            // send 返回 -1 且错误是 would_block() ，说明发送缓冲区满了
            if (sent < 0 && would_block()) {
                break;      //下次 EPOLLOUT 就绪时再继续发送
            }

            // 真正的错误，关闭连接
            close_connection(fd);
            return ;
        }

        //如果数据发送完了且连接需要关闭，执行关闭
        if (connection.output.empty() && connection.closing) {
            close_connection(fd);
            return ;
        }

        //更新 epoll 监控的事件
        refresh_interest(fd,connection);
    }

    // 更新epoll 监控事件，有数据要发就监控EPOLLOUT，发完了就取消监控
    void EpollServer::refresh_interest(int fd,Connection& connection) {
        // 基础事件：可读、连接关闭、错误
        std::uint32_t events = base_connection_events();

        // 如果还有数据要发送，也关注可写事件
        if (!connection.output.empty()) {
            events |= EPOLLOUT;
        }

        // 更新 epoll 中的事件
        if (!update_fd(fd,events)) {
           logger_.warn("[epoll] failed to update fd events");
            close_connection(fd);
        }
    }

    //关闭连接 从epoll和连接表中移除
    void EpollServer::close_connection(int fd) {
        // 从 epoll 中移除这个fd
        ::epoll_ctl(epoll_fd_,EPOLL_CTL_DEL,fd,nullptr);
        // 从连接表中删除（Connection 的析构函数会自动关闭 socket）
        connections_.erase(fd);
    }

    //清理空闲连接 定时踢掉长时间不活动的连接，释放资源
    void EpollServer::expire_idle_connections() {
        // 如果没有配置超时时间，不清理
        if (config_.idle_timeout_seconds <= 0) {
            return ;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(config_.idle_timeout_seconds);
        std::vector<int> expired;   //用于收集超时的 fd

        //遍历所有链接，找出空闲超时的
        for (const auto& [fd,connection] : connections_) {
            if (now - connection.last_active > timeout) {
                expired.push_back(fd);
            }
        }

        //关闭所有超时连接
        for (const int fd:expired) {
            close_connection(fd);
        }
    }

    // 执行具体命令 根据命令类型执行相应操作，返回响应字符串。写操作（SET/DEL）会同步记录AOF日志
    std::string EpollServer::execute_command(const core::Command& command,bool& should_close) {
        switch (command.type) {
            case core::CommandType::Help:
                return core::help_message();    //返回帮助信息

            case core::CommandType::Ping:
                return "PONG\n";        //心跳检测

            case core::CommandType::Set: {
                //SET 命令：存储键值对
                if (aof_) {     //如果有 AOF持久化，先记录日志
                    std::string error;
                    if (!aof_->append_set(command.key,command.value,error)) {
                        logger_.error("[epoll] AOF append SET failed: "+error);
                        return "ERR persist failed\n";
                    }
                }
                store_.set(command.key,command.value);      //存入内存
                return "OK\n";
            }

            case core::CommandType::Get: {
                //GET命令 ：获取 值value
                const auto value = store_.get(command.key);
                if (!value.has_value()) {
                    return "NOT_FOUND\n";
                }
                return "VALUE " + *value +'\n';
            }

            case core::CommandType::Del: {
                //DEL命令: 删除 键key
                if (!store.get(command.key).has_value()) {
                    return "NOT_FOUND\n";
                }
                if (aof_) {     //先记录AOF日志
                    std::string error;
                    if (!aof_->append_del(command.key,error)) {
                        logger_.error("[epoll] AOF append DEL failed: "+error);
                        return "ERR persist failed\n";
                    }
                }
                store_.erase(command.key);      //从内存删除
                return "DELETED\n";
            }

            case core::CommandType::Keys:
                return join_keys(store_.keys());    //列出所有 键key

            case core::CommandType::Size:
                return "SIZE " + std::to_string(store_.size()) + '\n';  //键key 数量

            case core::CommandType::Exit:
                should_close = true;        //标记需要关闭连接
                return "BYE\n";

            case core::CommandType::Invalid:
                return "ERR " + command.error + '\n';       //命令解析错误
        }

        return "ERR unhandled command\n";       //未知命令
    }
}       // namespace minikv::server