// Starting players, watching them exit, and other OS integration that has no
// UI: opening folders/URLs and listing this PC's network addresses.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "win.h"

namespace fl::sys {

std::wstring quoteArg(const std::wstring& arg);
std::wstring buildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args);

struct Started {
    HANDLE process = nullptr;  // owned by the caller (or handed to ProcessWatcher)
    DWORD pid = 0;
};
using EnvVars = std::vector<std::pair<std::wstring, std::wstring>>;
// This process's environment with `extra` set on top (a name already there is replaced, ignoring case), as the
// NUL-separated block CreateProcess takes.
std::wstring environmentBlock(const wchar_t* current, const EnvVars& extra);

// `hidden`: no console window (for helper tools such as tar.exe). `env`: variables set for the new process only.
bool startProcess(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& workDir,
                  Started& out, std::wstring& error, bool hidden = false, const EnvVars& env = {});

bool openPath(const std::wstring& pathOrUrl);
// True when a process with one of these executable names is running (`which` gets its name).
bool isProcessRunning(const std::vector<std::wstring>& exeNames, std::wstring* which = nullptr);
// Opens Explorer with `file` selected (or the folder itself).
bool revealInExplorer(const std::wstring& file);

struct NetAddress {
    std::wstring address, adapter;  // "192.168.1.20", "Wi-Fi" / "ZeroTier One"
};
// Up, non-loopback IPv4 addresses, physical adapters first.
std::vector<NetAddress> localAddresses();

// A background program next to a game, such as an HTML5 mod's co-op server:
// started hidden inside a job object, so it and everything it starts (a tunnel)
// end together, even if the launcher crashes. Every line it prints goes to
// `onLine` on a reader thread.
class BackgroundProcess {
public:
    using LineFn = std::function<void(const std::wstring&)>;
    BackgroundProcess() = default;
    ~BackgroundProcess();
    BackgroundProcess(const BackgroundProcess&) = delete;
    BackgroundProcess& operator=(const BackgroundProcess&) = delete;

    bool start(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& workDir,
               LineFn onLine, std::wstring& error);
    void stop();  // ends the whole process tree
    bool running() const;
    DWORD pid() const { return pid_; }

private:
    HANDLE job_ = nullptr, process_ = nullptr, read_ = nullptr;
    DWORD pid_ = 0;
    std::thread reader_;
};

// Polls until something accepts connections at `url`'s host and port (an
// http(s) address), up to `timeoutMs`. False on timeout or when `alive` says
// the server already quit.
bool waitForServer(const std::wstring& url, int timeoutMs, const std::function<bool()>& alive);

// Waits for any number of processes on one thread and calls `onExit` there
// (pid, exit code). Closes the handles it was given.
class ProcessWatcher {
public:
    using ExitFn = std::function<void(DWORD pid, DWORD exitCode)>;
    explicit ProcessWatcher(ExitFn onExit);
    ~ProcessWatcher();
    ProcessWatcher(const ProcessWatcher&) = delete;
    ProcessWatcher& operator=(const ProcessWatcher&) = delete;

    void watch(HANDLE process, DWORD pid);
    size_t count() const;

private:
    void loop();
    struct Item {
        HANDLE h;
        DWORD pid;
    };
    ExitFn onExit_;
    mutable std::mutex mu_;
    std::vector<Item> items_;
    HANDLE wake_ = nullptr;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace fl::sys
