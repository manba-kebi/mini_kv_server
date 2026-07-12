#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include "asynclogger/async_logger.h"
#include "minikv/net/socket_runtime.h"
#include "minikv/server/epoll_server.h"

namespace {

    //提示用户怎么用这个程序
    void print_usage(const char* program) {
        std::cout
            << "Usage: " <<program              // 程序名
            <<" [--port <port>]"                // 端口号
            <<" [--backlog <count>]"            // 等待队列长度
            <<" [--max-events <count>]"         // epoll 每次处理最大事件数
            <<" [--idle-timeout <seconds>]"     // 空闲超时时间
            <<" [--max-line <bytes>]"           //最大命令长度
            <<" [--aof <path>]\n"               // AOF 持久化文件路径
            <<"Example: " << program
            <<" --port 9000 --backlog 512 --max-events 128 --idle-timeout 60 --aof data/minikv.aof\n";
    }

    // 解析16位无符号整数
    bool parse_u16(const std::string& text, std::uint16_t& value) {
        char* end = nullptr;        // 后续用于指向第一个非数字字符的位置
        // strtoul: 字符串转 unsigned long
        const unsigned long parsed = std::strtoul(text.c_str(),&end,10);
        // 10 表示十进制

        const auto max_port = static_cast<unsigned long>((std::numeric_limits<std::uint16_t>::max)());      //65535

        // 检查是否解析成功
        if (end == text.c_str() || *end != '\0' || parsed > max_port) {
            //一个数字都没解析到     字符串后面还有非数字字符    超过了 65535
            return false;
        }
        value = static_cast<std::uint16_t>(parsed);
        return true;
    }

    // 解析普通整数
    bool parse_int(const std::string& text, int& value) {
        char* end = nullptr;
        // strtol: 字符串转 long
        const long parsed = std::strtol(text.c_str(),&end,10);

        if (end == text.c_str() || *end != '\0' || parsed < 0 || parsed>(std::numeric_limits<int>::max)()) {
            //没解析到数字           有多余字符        不允许负数       超过 int 范围
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }

    //解析 size_t 类型，解析最大行长度
    bool parse_size(const std::string& text, size_t& value) {
        char* end = nullptr;
        // strtoull: 字符串转 unsigned long long
        const unsigned long long parsed = std::strtoull(text.c_str(),&end,10);
        if (end == text.c_str() || *end != '\0') {
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return true;
    }

}   //namespace

int main(int argc, char* argv[]) {
    // 初始化 socket
    minikv::net::SocketRuntime runtime;
    if (!runtime.ok()) {
        std::cerr << "Failed to initialize socket runtime\n";
        return 1;
    }

    minikv::server::EpollServerConfig config;

    // 遍历命令行参数（argv[0] 是程序名，从 1 开始）
    for (int i = 1;i<argc;++i) {
        const std::string_view arg = argv[i];

        //帮助信息
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        //检查参数值是否存在。如果这是最后一个参数，那它没有对应的值
        if (i+1 >= argc) {
            std::cerr <<"Missing value for argument: " <<arg <<'\n';
            print_usage(argv[0]);
            return 1;
        }

        // 获取参数值
        const std::string value = argv[++i];

        //解析各个参数
        if (arg == "--port") {
            if (!parse_u16(value, config.port)) {
                std::cerr << "Invalid port: " <<value << '\n';
                return 1;
            }
        }else if (arg == "--backlog") {
            if (!parse_int(value,config.backlog)) {
                std::cerr << "Invalid backlog: " << value << '\n';
                return 1;
            }
        }else if (arg == "--max-events") {
            if (!parse_int(value,config.max_events) || config.max_events <= 0) {
                std::cerr << "Invalid max-events: " << value << '\n';
                return 1;
            }
        }else if (arg == "--idle-timeout") {
            if (!parse_int(value,config.idle_timeout_seconds)) {
                std::cerr << "Invalid idle-timeout: " << value << '\n';
                return 1;
            }
        }else if (arg == "--max-line") {
            if (!parse_size(value,config.max_line_length) || config.max_line_length == 0) {
                std::cerr << "Invalid max-line: " << value << '\n';
                return 1;
            }
        }else if (arg == "--aof") {
            config.aof_path = std::filesystem::path(value);
        }else {
            // 未知参数
            std::cerr <<"Unknown argument: "<< arg << '\n';
            print_usage(argv[0]);
            return 1;
        }
    }

    //创建日志配置
    asynclogger::LoggerConfig logger_config;
    logger_config.file_path = "logs/mini_kv_server_epoll.log";      //日志文件路径
    logger_config.max_queue_size = 4096;                            //日志队列最大长度
    logger_config.roll_size_bytes = 10 *1024 * 1024;
    logger_config.overflow_policy = asynclogger::OverflowPolicy::Drop;
    logger_config.auto_flush = true;                                //自动刷新

    //创建异步日志对象
    asynclogger::AsyncLogger logger(logger_config);
    //创建Epoll服务器
    minikv::server::EpollServer server(config,logger);

    //启动服务器
    if (!server.start()) {
        return 1;
    }

    //进入事件循环
    server.run();
    return 0;
}

