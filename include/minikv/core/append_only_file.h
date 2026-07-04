#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "minikv/core/key_value_store.h"

namespace minikv::core {

    class AppendOnlyFile {
    public:
        explicit AppendOnlyFile(std::filesystem::path path);

        AppendOnlyFile(const AppendOnlyFile&) = delete;
        AppendOnlyFile& operator=(const AppendOnlyFile&) = delete;

        bool replay(KeyValueStore& store,std::string& error);
        bool append_set(const std::string& key,const std::string& value,std::string& error);
        bool append_del(const std::string& key,std::string& error);
        void flush();

    private:
        bool open_for_append(std::string& error);
        bool append_line(const std::string& line,std::string& error);
        static bool is_safe_record_text(std::string_view text);

        std::filesystem::path path_;
        std::ofstream output_;
        std::mutex mutex_;
    };
}   //namespace minikv::core