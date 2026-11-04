#include "process.h"

#include <algorithm>

#include "fs.h"
#include "log.h"
#include "strings.h"
// win.h first: these need the full Windows types.
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _WIN32
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <net/if.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace fl::sys {

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L"\"";
    return out;
}

std::wstring buildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    // argv[0] is always quoted: CreateProcess otherwise tries "C:\Program.exe" first.
    std::wstring cmd = L"\"" + exe + L"\"";
    for (const auto& a : args) cmd += L" " + quoteArg(a);
    return cmd;
}

std::wstring environmentBlock(const wchar_t* current, const EnvVars& extra) {
    std::wstring block;
    for (const wchar_t* p = current; p && *p; p += wcslen(p) + 1) {
        std::wstring entry = p;
        // "=C:=C:\dir" entries (per-drive folders) start with '=': kept as they are.
        size_t eq = entry.find(L'=', 1);
        std::wstring name = eq == std::wstring::npos ? entry : entry.substr(0, eq);
        bool replaced = std::any_of(extra.begin(), extra.end(), [&](const auto& e) { return _wcsicmp(e.first.c_str(), name.c_str()) == 0; });
        if (!replaced) block += entry + L'\0';
    }
    for (const auto& [name, value] : extra) block += name + L'=' + value + L'\0';
    block += L'\0';
    return block;
}

#ifdef _WIN32
bool startProcess(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& workDir,
                  Started& out, std::wstring& error, bool hidden, const EnvVars& env) {
    std::wstring cmd = buildCommandLine(exe, args);
    std::wstring envText;
    for (const auto& [name, value] : env) envText += L" " + name + L"=" + value;
    logger().info(L"Starting: " + cmd + (workDir.empty() ? L"" : L"  (in " + workDir + L")") +
                  (envText.empty() ? L"" : L"  (with" + envText + L")"));
    std::wstring block;
    if (!env.empty()) {
        wchar_t* current = GetEnvironmentStringsW();
        block = environmentBlock(current, env);
        FreeEnvironmentStringsW(current);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    DWORD flags = (hidden ? CREATE_NO_WINDOW : 0) | (block.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, block.empty() ? nullptr : block.data(),
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        error = win32ErrorMessage(GetLastError());
        logger().error(L"CreateProcess failed: " + error);
        return false;
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.pid = pi.dwProcessId;
    return true;
}

bool openPath(const std::wstring& p) {
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
}

bool revealInExplorer(const std::wstring& file) {
    if (fs::isDir(file)) return openPath(file);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str());
    if (!pidl) return openPath(fs::parent(file));
    HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return SUCCEEDED(hr);
}

std::vector<NetAddress> localAddresses() {
    std::vector<NetAddress> out, virtualOnes;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET) continue;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&sin->sin_addr);
            if (b[0] == 169 && b[1] == 254) continue;  // link-local: nobody can reach it
            wchar_t text[32];
            InetNtopW(AF_INET, &sin->sin_addr, text, 32);
            NetAddress n{text, a->FriendlyName ? a->FriendlyName : L""};
            // Hyper-V/WSL switches are noise; VPN adapters (ZeroTier, Radmin, Tailscale) are
            // exactly what friends over the internet need, so keep those.
            std::wstring lower = toLower(n.adapter);
            if (lower.find(L"vethernet") != std::wstring::npos || lower.find(L"virtualbox") != std::wstring::npos ||
                lower.find(L"vmware") != std::wstring::npos)
                continue;
            (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211 ? out : virtualOnes).push_back(n);
        }
    }
    out.insert(out.end(), virtualOnes.begin(), virtualOnes.end());
    return out;
}

BackgroundProcess::~BackgroundProcess() { stop(); }

bool BackgroundProcess::start(const std::wstring& exe, const std::vector<std::wstring>& args,
                              const std::wstring& workDir, LineFn onLine, std::wstring& error) {
    stop();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE write = nullptr;
    if (!CreatePipe(&read_, &write, &sa, 0)) {
        error = win32ErrorMessage(GetLastError());
        return false;
    }
    SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);  // only the child's end is inherited
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    std::wstring cmd = buildCommandLine(exe, args);
    logger().info(L"Starting server: " + cmd + L"  (in " + workDir + L")");
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    // Suspended until it is in the job, so nothing it starts can escape the job.
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    DWORD code = GetLastError();
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        error = win32ErrorMessage(code);
        stop();
        return false;
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    HANDLE read = read_;
    reader_ = std::thread([read, onLine] {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (onLine) onLine(widen(line));
            }
        }
        if (!pending.empty() && onLine) onLine(widen(pending));
    });
    return true;
}

void BackgroundProcess::stop() {
    if (job_) {
        TerminateJobObject(job_, 1);
        CloseHandle(job_);
        job_ = nullptr;
    } else if (process_) {
        TerminateProcess(process_, 1);
    }
    // The pipe closes once every process holding it is gone; the reader ends then.
    if (reader_.joinable()) reader_.join();
    if (read_) CloseHandle(read_);
    if (process_) CloseHandle(process_);
    read_ = process_ = nullptr;
    pid_ = 0;
}
#else  // Linux twins of startProcess .. BackgroundProcess::stop (docs/LINUX.md)

namespace {

// argv[0] and the rest, as UTF-8; a Windows program ("MZ") runs through Wine.
bool commandFor(const std::wstring& exe, const std::vector<std::wstring>& args, std::vector<std::string>& argv,
                std::wstring& error) {
    std::string path = flOsPath(exe);
    char magic[2] = {};
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = win32ErrorMessage(flWin32FromErrno(errno));
        return false;
    }
    bool windows = read(fd, magic, 2) == 2 && magic[0] == 'M' && magic[1] == 'Z';
    close(fd);
    if (windows) {
        std::wstring wine(MAX_PATH, L'\0');
        DWORD n = SearchPathW(nullptr, L"wine", nullptr, MAX_PATH, wine.data(), nullptr);
        if (!n || n >= MAX_PATH) {
            error = L"This is a Windows program and Wine isn't installed (sudo apt install wine).";
            return false;
        }
        wine.resize(n);
        argv.push_back(flOsPath(wine));
    }
    argv.push_back(path);
    for (const auto& a : args) argv.push_back(narrow(a));
    return true;
}

std::vector<std::string> environmentFor(const EnvVars& extra) {
    std::vector<std::string> out;
    for (char** e = environ; e && *e; ++e) {
        std::string entry = *e;
        std::string name = entry.substr(0, entry.find('='));
        bool replaced = std::any_of(extra.begin(), extra.end(), [&](const auto& x) { return narrow(x.first) == name; });
        if (!replaced) out.push_back(entry);
    }
    for (const auto& [name, value] : extra) out.push_back(narrow(name) + "=" + narrow(value));
    return out;
}

std::vector<char*> pointers(std::vector<std::string>& v) {
    std::vector<char*> p;
    for (auto& s : v) p.push_back(s.data());
    p.push_back(nullptr);
    return p;
}

void closeFrom(int lowest) {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, lowest, ~0u, 0) == 0) return;
#endif
    for (int fd = lowest; fd < 4096; ++fd) close(fd);
}

// fork + exec with an error pipe, so a failed exec is reported like a failed CreateProcess. Between fork and exec
// the child makes only async-signal-safe calls. Returns the pid, or 0 with `error` set.
pid_t spawn(std::vector<std::string>& argv, std::vector<std::string>* env, const std::string& dir, int out,
            bool ownGroup, bool newSession, std::wstring& error) {
    std::vector<char*> args = pointers(argv);
    std::vector<char*> envp;
    if (env) envp = pointers(*env);
    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0) {
        error = win32ErrorMessage(flWin32FromErrno(errno));
        return 0;
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        error = win32ErrorMessage(flWin32FromErrno(errno));
        close(report[0]);
        close(report[1]);
        return 0;
    }
    if (pid == 0) {
        if (newSession) setsid();
        else if (ownGroup) setpgid(0, 0);
        if (ownGroup) {
            // A server dies with the launcher, as the Windows job object makes it.
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(127);
        }
        signal(SIGPIPE, SIG_DFL);
        int nul = open("/dev/null", O_RDWR);
        // Moved to 3 first, so the dup2s below can't overwrite it.
        int keep = fcntl(report[1], F_DUPFD_CLOEXEC, 10);
        int sink = out >= 0 ? fcntl(out, F_DUPFD, 10) : nul;
        dup2(nul, 0);
        dup2(sink, 1);
        dup2(sink, 2);
        dup2(keep, 3);
        fcntl(3, F_SETFD, FD_CLOEXEC);
        closeFrom(4);
        if (!dir.empty() && chdir(dir.c_str()) != 0) {
            int e = errno;
            (void)!write(3, &e, sizeof e);
            _exit(127);
        }
        if (env) execve(args[0], args.data(), envp.data());
        else execv(args[0], args.data());
        int e = errno;
        (void)!write(3, &e, sizeof e);
        _exit(127);
    }
    close(report[1]);
    int e = 0;
    ssize_t n;
    do n = read(report[0], &e, sizeof e);
    while (n < 0 && errno == EINTR);
    close(report[0]);
    if (n == sizeof e) {
        waitpid(pid, nullptr, 0);
        error = win32ErrorMessage(flWin32FromErrno(e));
        return 0;
    }
    return pid;
}

// Runs a desktop helper (xdg-open, dbus-send) detached; true when it ran and succeeded within a few seconds. A double
// fork leaves no zombie behind.
bool detached(std::vector<std::string> argv) {
    std::vector<char*> args = pointers(argv);
    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(report[0]);
        close(report[1]);
        return false;
    }
    if (pid == 0) {
        setsid();
        pid_t helper = fork();
        if (helper != 0) _exit(helper > 0 ? 0 : 1);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
        int keep = fcntl(report[1], F_DUPFD, 3);
        (void)keep;
        // The grandchild runs the helper and writes its result; the launcher doesn't wait for long-running ones.
        pid_t tool = fork();
        if (tool == 0) {
            closeFrom(3);
            execvp(args[0], args.data());
            _exit(127);
        }
        int status = 0;
        waitpid(tool, &status, 0);
        char ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 : 0;
        (void)!write(keep, &ok, 1);
        _exit(0);
    }
    close(report[1]);
    waitpid(pid, nullptr, 0);
    pollfd p{report[0], POLLIN, 0};
    char ok = 0;
    bool done = poll(&p, 1, 5000) > 0 && read(report[0], &ok, 1) == 1;
    close(report[0]);
    return done ? ok == 1 : true;  // still running after 5 s: it opened something and stayed
}

std::string uriFor(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if (isalnum(c) || strchr("/-._~", c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}  // namespace

bool startProcess(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& workDir,
                  Started& out, std::wstring& error, bool hidden, const EnvVars& env) {
    std::wstring cmd = buildCommandLine(exe, args);
    std::wstring envText;
    for (const auto& [name, value] : env) envText += L" " + name + L"=" + value;
    logger().info(L"Starting: " + cmd + (workDir.empty() ? L"" : L"  (in " + workDir + L")") +
                  (envText.empty() ? L"" : L"  (with" + envText + L")"));
    std::vector<std::string> argv;
    if (!commandFor(exe, args, argv, error)) {
        logger().error(L"Starting failed: " + error);
        return false;
    }
    // BepInEx (any Doorstop loader) is a winhttp.dll beside the game, which Wine passes over for its own builtin one
    // unless told.
    EnvVars vars = env;
    bool viaWine = argv.size() == args.size() + 2;
    bool overridden = getenv("WINEDLLOVERRIDES") != nullptr ||
                      std::any_of(env.begin(), env.end(), [](const auto& x) { return x.first == L"WINEDLLOVERRIDES"; });
    if (viaWine && !overridden && fs::isFile(fs::join(fs::parent(exe), L"winhttp.dll"))) {
        logger().info(L"A winhttp.dll loader (BepInEx) is beside it: WINEDLLOVERRIDES=winhttp=n,b");
        vars.push_back({L"WINEDLLOVERRIDES", L"winhttp=n,b"});
    }
    std::vector<std::string> block;
    if (!vars.empty()) block = environmentFor(vars);
    // Its own session: the game outlives the launcher, as on Windows.
    pid_t pid = spawn(argv, vars.empty() ? nullptr : &block, workDir.empty() ? "" : flOsPath(workDir), -1, false, true,
                      error);
    if (!pid) {
        logger().error(L"Starting failed: " + error);
        return false;
    }
    out.process = flProcessHandle(pid);
    out.pid = static_cast<DWORD>(pid);
    return true;
}

bool openPath(const std::wstring& p) {
    bool url = p.find(L"://") != std::wstring::npos || startsWithI(p, L"mailto:");
    return detached({"xdg-open", url ? narrow(p) : flOsPath(p)});
}

bool revealInExplorer(const std::wstring& file) {
    if (fs::isDir(file)) return openPath(file);
    // The freedesktop file-manager interface selects the file (Nautilus, Dolphin, Nemo, Thunar); else the folder.
    if (detached({"dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.FileManager1",
                  "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
                  "array:string:" + uriFor(flOsPath(file)), "string:"}))
        return true;
    return openPath(fs::parent(file));
}

std::vector<NetAddress> localAddresses() {
    std::vector<NetAddress> out, virtualOnes;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&sin->sin_addr);
        if (b[0] == 169 && b[1] == 254) continue;  // link-local: nobody can reach it
        std::string name = a->ifa_name;
        // Container and VM bridges are noise; VPN adapters (ZeroTier, Tailscale, Hamachi) are what friends need.
        static const char* noise[] = {"docker", "veth", "br-", "virbr", "vmnet", "vboxnet", "lxcbr", "lxdbr", "podman", "cni"};
        if (std::any_of(std::begin(noise), std::end(noise), [&](const char* n) { return name.rfind(n, 0) == 0; }))
            continue;
        wchar_t text[32];
        InetNtopW(AF_INET, &sin->sin_addr, text, 32);
        std::wstring label = widen(name);
        if (name.rfind("zt", 0) == 0) label = L"ZeroTier (" + label + L")";
        else if (name.rfind("ham", 0) == 0) label = L"Hamachi (" + label + L")";
        else if (name.rfind("tailscale", 0) == 0) label = L"Tailscale (" + label + L")";
        NetAddress n{text, label};
        // A real network card has a device behind it; tunnels and bridges don't.
        bool physical = access(("/sys/class/net/" + name + "/device").c_str(), F_OK) == 0;
        (physical ? out : virtualOnes).push_back(n);
    }
    freeifaddrs(list);
    out.insert(out.end(), virtualOnes.begin(), virtualOnes.end());
    return out;
}

BackgroundProcess::~BackgroundProcess() { stop(); }

bool BackgroundProcess::start(const std::wstring& exe, const std::vector<std::wstring>& args,
                              const std::wstring& workDir, LineFn onLine, std::wstring& error) {
    stop();
    std::vector<std::string> argv;
    if (!commandFor(exe, args, argv, error)) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        error = win32ErrorMessage(flWin32FromErrno(errno));
        return false;
    }
    std::wstring cmd = buildCommandLine(exe, args);
    logger().info(L"Starting server: " + cmd + L"  (in " + workDir + L")");
    // Its own process group, which stop() ends whole: what the job object does on Windows.
    pid_t pid = spawn(argv, nullptr, workDir.empty() ? "" : flOsPath(workDir), fds[1], true, false, error);
    close(fds[1]);
    if (!pid) {
        close(fds[0]);
        return false;
    }
    process_ = flProcessHandle(pid);
    pid_ = static_cast<DWORD>(pid);
    read_ = flFileHandle(fds[0]);
    HANDLE read = read_;
    reader_ = std::thread([read, onLine] {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (onLine) onLine(widen(line));
            }
        }
        if (!pending.empty() && onLine) onLine(widen(pending));
    });
    return true;
}

void BackgroundProcess::stop() {
    if (pid_) {
        kill(-static_cast<pid_t>(pid_), SIGKILL);  // the group: the server and whatever it started
        if (process_) TerminateProcess(process_, 1);
    }
    // The pipe closes once every process holding it is gone; the reader ends then.
    if (reader_.joinable()) reader_.join();
    if (process_) WaitForSingleObject(process_, 2000);  // reaped, so it leaves no zombie
    if (read_) CloseHandle(read_);
    if (process_) CloseHandle(process_);
    read_ = process_ = nullptr;
    pid_ = 0;
}
#endif

bool BackgroundProcess::running() const {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool waitForServer(const std::wstring& url, int timeoutMs, const std::function<bool()>& alive) {
    std::wstring rest = url.substr(url.find(L"://") == std::wstring::npos ? 0 : url.find(L"://") + 3);
    std::wstring hostPort = rest.substr(0, rest.find(L'/'));
    std::wstring host = hostPort, port = startsWithI(url, L"https://") ? L"443" : L"80";
    if (size_t colon = hostPort.rfind(L':'); colon != std::wstring::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    bool up = false;
    ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (!up && GetTickCount64() < until && (!alive || alive())) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(narrow(host).c_str(), narrow(port).c_str(), &hints, &res) == 0 && res) {
            SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            up = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
            closesocket(s);
            freeaddrinfo(res);
        }
        if (!up) Sleep(250);
    }
    WSACleanup();
    return up;
}

ProcessWatcher::ProcessWatcher(ExitFn onExit) : onExit_(std::move(onExit)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { loop(); });
}

ProcessWatcher::~ProcessWatcher() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    for (auto& i : items_) CloseHandle(i.h);  // the games keep running; we only stop watching
    CloseHandle(wake_);
}

void ProcessWatcher::watch(HANDLE process, DWORD pid) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        items_.push_back({process, pid});
    }
    SetEvent(wake_);
}

size_t ProcessWatcher::count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return items_.size();
}

void ProcessWatcher::loop() {
    for (;;) {
        std::vector<HANDLE> handles{wake_};
        std::vector<Item> snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            // WaitForMultipleObjects is limited to 64 handles; nobody runs 63 games at once.
            for (size_t i = 0; i < items_.size() && handles.size() < MAXIMUM_WAIT_OBJECTS; ++i) {
                handles.push_back(items_[i].h);
                snapshot.push_back(items_[i]);
            }
        }
        DWORD r = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) {
            if (r == WAIT_FAILED) Sleep(50);
            continue;
        }
        size_t idx = r - WAIT_OBJECT_0 - 1;
        if (idx >= snapshot.size()) continue;
        Item done = snapshot[idx];
        DWORD code = 0;
        GetExitCodeProcess(done.h, &code);
        {
            std::lock_guard<std::mutex> lock(mu_);
            items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& i) { return i.h == done.h; }),
                         items_.end());
        }
        CloseHandle(done.h);
        if (onExit_) onExit_(done.pid, code);
    }
}

#ifdef _WIN32
bool isProcessRunning(const std::vector<std::wstring>& exeNames, std::wstring* which) {
    if (exeNames.empty()) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        for (const auto& name : exeNames) {
            if (iequals(pe.szExeFile, name)) {
                found = true;
                if (which) *which = pe.szExeFile;
                break;
            }
        }
    }
    CloseHandle(snap);
    return found;
}
#else
// Linux: a process's name is the file name of its argv[0] (a Wine/Proton game shows "C:\...\Game.exe" there), or
// its comm; matched ignoring case, with or without ".exe".
bool isProcessRunning(const std::vector<std::wstring>& exeNames, std::wstring* which) {
    if (exeNames.empty()) return false;
    auto bare = [](std::wstring s) {
        s = toLower(s);
        if (s.size() > 4 && s.compare(s.size() - 4, 4, L".exe") == 0) s.resize(s.size() - 4);
        return s;
    };
    std::vector<std::wstring> wanted;
    for (const auto& n : exeNames) wanted.push_back(bare(n));
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    bool found = false;
    while (dirent* e = readdir(proc)) {
        if (!isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
        std::vector<std::wstring> names;
        std::string base = std::string("/proc/") + e->d_name;
        if (FILE* f = fopen((base + "/cmdline").c_str(), "rb")) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[n] = 0;
            std::string arg0 = buf;
            size_t cut = arg0.find_last_of("/\\");
            if (!arg0.empty()) names.push_back(widen(cut == std::string::npos ? arg0 : arg0.substr(cut + 1)));
        }
        if (FILE* f = fopen((base + "/comm").c_str(), "rb")) {
            char buf[64] = {};
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            std::string comm(buf, n);
            while (!comm.empty() && comm.back() == '\n') comm.pop_back();
            if (!comm.empty()) names.push_back(widen(comm));
        }
        for (const auto& name : names) {
            std::wstring b = bare(name);
            if (std::find(wanted.begin(), wanted.end(), b) != wanted.end()) {
                found = true;
                if (which) *which = name;
                break;
            }
        }
        if (found) break;
    }
    closedir(proc);
    return found;
}
#endif

}  // namespace fl::sys
