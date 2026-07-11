#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "minikv/core/append_only_file.h"
#include "minikv/core/command.h"
#include "minikv/core/key_value_store.h"
#include "minikv/net/socket.h"

namespace asynclogger {
    class AsyncLogger;
}

namespace minikv::server {

    struct EpollServerConfig {
        std::uint16_t port{9000};       //端口
        int backlog{512};
        int max_events{128};        //最大事件数
        std::size_t max_line_length{4096};      //最大行长度
        int idle_timeout_seconds{60};       //空闲超时
        std::filesystem::path aof_path{};       //AOF路径
    };

    class EpollServer {
    public:
        EpollServer(EpollServerConfig config,asynclogger::AsyncLogger& logger);
        ~EpollServer();

        EpollServer(const EpollServer&) = delete;
        EpollServer& operator=(const EpollServer&) = delete;

        bool start();       //初始化监听socket、epoll、AOF回放
        void run();     //事件循环

    private:
        struct Connection {
            net::Socket socket;     //每个客户端自己的socket
            std::string input;      //输入缓冲区
            std::string output;     //输出缓冲区
            std::chrono::steady_clock::time_point last_active{};        //最后活跃时间
            bool closing{false};        //是否准备关闭
        };

        bool add_fd(int fd,std::uint32_t events);
        bool update_fd(int fd,std::uint32_t events);
        void accept_new_connection();
        void handle_connection_event(int fd,std::uint32_t events);
        void handle_read(int fd,Connection& connection);
        void handle_write(int fd,Connection& connection);
        void refresh_interest(int fd,Connection& connection);
        void close_connection(int fd);
        void expire_idle_connections();
        std::string execute_command(const core::Command& command,bool& should_close);

        EpollServerConfig config_;
        asynclogger::AsyncLogger& logger_;
        net::Socket listen_socket_;
        int epoll_fd_{-1};
        core::KeyValueStore store_;
        std::unique_ptr<core::AppendOnlyFile> aof_;
        std::unordered_map<int,Connection> connections_;
        bool started_{false};
    };
}   // namespace minikv::server