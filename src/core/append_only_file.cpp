#include "minikv/core/append_only_file.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "minikv/core/command.h"

namespace minikv::core {
    // 持久化（Persistences）核心模块 AOF（Append-Only File，追加写文件）
    // 把每个写操作（SET/DEL）以日志形式追加到磁盘文件里；服务器重启时，重放（Replay）这些日志，把数据恢复回来。

    // 记录日志文件的存放路径
    AppendOnlyFile::AppendOnlyFile(std::filesystem::path path): path_(std::move(path)) {
        if (path_.empty()) {
            throw std::invalid_argument("append-only file path must not be empty");
        }
    }

    // 服务器启动时调用，把磁盘上的历史数据加载回内存。
    bool AppendOnlyFile::replay(KeyValueStore& store,std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (std::filesystem::exists(path_)) {
            std::ifstream input(path_);
            if (!input) {
                error = "failed to open AOF for replay: " + path_.string();
                return false;
            }

            std::string line;
            std::size_t line_number = 0;
            while (std::getline(input, line)) {
                ++line_number;
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    continue;
                }

                const Command command = parse_command(line);
                switch (command.type) {
                    case CommandType::Set:
                        store.set(command.key, command.value);
                        break;
                    case CommandType::Del:
                        store.erase(command.key);
                        break;
                    default: {
                        std::ostringstream oss;
                        oss << "invalid AOF record at line " <<line_number << ": " << line;
                        error = oss.str();
                        return false;
                    }
                }
            }

            if (input.bad()) {
                error = "failed while reading AOF: " + path_.string();
                return false;
            }
        }

        return open_for_append(error);
    }

    // set del 实时记录写操作
    // 每当客户端发来一个 SET 或 DEL 命令，并且内存执行成功后，TCP 工作线程就会调用这两个函数，把命令立刻追加到磁盘日志里。
    bool AppendOnlyFile::append_set(const std::string& key,const std::string& value,std::string& error) {
        if (!is_safe_record_text(key) || !is_safe_record_text(value)) {
            error = "AOF record contains newline or carriage return";
            return false;
        }
        return append_line("SET " + key + " " + value,error);
    }

    bool AppendOnlyFile::append_del(const std::string& key,std::string& error) {
        if (!is_safe_record_text(key)) {
            error = "AOF record contains newline or carriage return";
            return false;
        }
        return append_line("DEL " + key,error);
    }

    // 把 C++ 标准库的缓冲区数据强制推到操作系统内核
    void AppendOnlyFile::flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_.is_open()) {
            output_.flush();
        }
    }

    // 内部工具函数，确保日志文件处于“可追加写入”的状态
    bool AppendOnlyFile::open_for_append(std::string& error) {
        if (output_.is_open()) {
            return true;
        }

        try {
            if (path_.has_parent_path()) {
                std::filesystem::create_directories(path_.parent_path());
            }
        }catch (const std::exception& exc) {
            error = "failed to create AOF directory: " + std::string(exc.what());
            return false;
        }

        output_.open(path_,std::ios::out | std::ios::app | std::ios::binary);
        if (!output_) {
            error = "failed to open AOF for append: " + path_.string();
            return false;
        }
        return true;
    }

    // 这是 append_set 和 append_del 的底层调用。
    bool AppendOnlyFile::append_line(const std::string& line,std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_for_append(error)) {
            return false;
        }

        output_ << line << '\n';
        output_.flush();

        if (!output_) {
            error = "failed to append AOF record: " + path_.string();
            return false;
        }
        return true;
    }

    // 输入合法性校验：检查字符串里有没有 \n（换行）或 \r（回车）。
    bool AppendOnlyFile::is_safe_record_text(std::string_view text) {
        return text.find('\n') == std::string_view::npos &&
            text.find('\r') == std::string_view::npos;
    }

}   // namespace minikv:core