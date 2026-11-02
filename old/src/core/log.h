// Thread-safe launcher log. Writes UTF-8 lines to a file and keeps the
// most recent lines in memory for the "Show details" view.
#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

enum class LogLevel { Debug, Info, Warn, Error };

class Logger {
public:
    static Logger& get();

    bool open(const std::wstring& path);  // truncates; creates the folder
    void close();
    void setLevel(LogLevel level) { level_ = level; }
    void setEcho(bool echo) { echo_ = echo; }  // mirror to stderr (CLI modes)
    std::wstring path() const;

    void log(LogLevel level, std::wstring_view msg);
    void debug(std::wstring_view m) { log(LogLevel::Debug, m); }
    void info(std::wstring_view m) { log(LogLevel::Info, m); }
    void warn(std::wstring_view m) { log(LogLevel::Warn, m); }
    void error(std::wstring_view m) { log(LogLevel::Error, m); }

    // Lines with sequence number >= `from`; `next` receives the next sequence.
    std::vector<std::wstring> since(uint64_t from, uint64_t& next) const;
    bool copyTo(const std::wstring& dest) const;

private:
    Logger() = default;
    mutable std::mutex mu_;
    FILE* file_ = nullptr;
    std::wstring path_;
    LogLevel level_ = LogLevel::Info;
    bool echo_ = false;
    std::deque<std::wstring> recent_;
    uint64_t seq_ = 0;
};

inline Logger& logger() { return Logger::get(); }

}  // namespace fl
