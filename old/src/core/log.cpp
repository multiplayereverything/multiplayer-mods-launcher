#include "log.h"

#include <share.h>

#include "fs.h"
#include "strings.h"
#include "win.h"

namespace fl {

Logger& Logger::get() {
    static Logger instance;
    return instance;
}

bool Logger::open(const std::wstring& path) {
    std::lock_guard lock(mu_);
    if (file_) fclose(file_);
    file_ = nullptr;
    fs::ensureDir(fs::parent(path), nullptr, nullptr);
    file_ = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);  // others may read the live log
    path_ = file_ ? path : L"";
    return file_ != nullptr;
}

void Logger::close() {
    std::lock_guard lock(mu_);
    if (file_) fclose(file_);
    file_ = nullptr;
}

std::wstring Logger::path() const {
    std::lock_guard lock(mu_);
    return path_;
}

void Logger::log(LogLevel level, std::wstring_view msg) {
    if (level < level_) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    static const wchar_t* names[] = {L"DEBUG", L"INFO ", L"WARN ", L"ERROR"};
    wchar_t stamp[48];
    swprintf(stamp, 48, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds, names[static_cast<int>(level)]);
    std::wstring line = stamp;
    line += msg;

    std::lock_guard lock(mu_);
    if (file_) {
        std::string utf8 = narrow(line);
        utf8 += "\r\n";
        fwrite(utf8.data(), 1, utf8.size(), file_);
        fflush(file_);
    }
    if (echo_) fwprintf(stderr, L"%s\n", line.c_str());
    recent_.push_back(line.substr(11));  // drop the date for the on-screen view
    if (recent_.size() > 500) recent_.pop_front();
    ++seq_;
}

std::vector<std::wstring> Logger::since(uint64_t from, uint64_t& next) const {
    std::lock_guard lock(mu_);
    std::vector<std::wstring> out;
    uint64_t first = seq_ - recent_.size();
    for (uint64_t s = from < first ? first : from; s < seq_; ++s) out.push_back(recent_[static_cast<size_t>(s - first)]);
    next = seq_;
    return out;
}

bool Logger::copyTo(const std::wstring& dest) const {
    std::wstring src;
    {
        std::lock_guard lock(mu_);
        if (file_) fflush(file_);
        src = path_;
    }
    if (src.empty()) return false;
    // The log is still open for writing, so read it with write sharing.
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    std::vector<char> data;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(in, buf, sizeof buf, &n, nullptr) && n) data.insert(data.end(), buf, buf + n);
    CloseHandle(in);
    return fs::writeFile(dest, data.data(), data.size(), nullptr);
}

}  // namespace fl
