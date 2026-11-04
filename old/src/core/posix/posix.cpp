// The Win32 calls of win32.h, done with POSIX. Handles are small objects (a file descriptor, an event, a process, a
// directory listing); error codes are the Win32 ones the engine checks, turned from errno.
#include "core/win.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

thread_local DWORD lastError = 0;

// A dead peer must not kill the process when a socket is written to (Winsock just returns an error), and a game
// started by the launcher must not be left a zombie the launcher never waits for: both set up once at start.
struct Startup {
    Startup() { signal(SIGPIPE, SIG_IGN); }
} startup;

// ---- UTF-8 <-> UTF-32 ------------------------------------------------------------------------------------------
std::wstring fromUtf8(const char* s, size_t n) {
    std::wstring out;
    out.reserve(n);
    for (size_t i = 0; i < n;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) cp = c, len = 1;
        else if ((c >> 5) == 6) cp = c & 0x1f, len = 2;
        else if ((c >> 4) == 14) cp = c & 0x0f, len = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, len = 4;
        else {
            out += L'\xFFFD';
            ++i;
            continue;
        }
        if (i + len > n) {
            out += L'\xFFFD';
            break;
        }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 2) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (!ok) {
            out += L'\xFFFD';
            ++i;
            continue;
        }
        out += static_cast<wchar_t>(cp);
        i += len;
    }
    return out;
}

std::string toUtf8(const wchar_t* s, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x110000) {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += "\xEF\xBF\xBD";
        }
    }
    return out;
}

std::string u8(const wchar_t* s) { return s ? toUtf8(s, wcslen(s)) : std::string(); }
std::wstring w(const std::string& s) { return fromUtf8(s.data(), s.size()); }

bool fail(DWORD code) {
    lastError = code;
    return false;
}
BOOL failErrno() {
    lastError = flWin32FromErrno(errno);
    return FALSE;
}

bool ieq(const std::string& a, const std::string& b) {
    std::wstring x = w(a), y = w(b);
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (towlower(x[i]) != towlower(y[i])) return false;
    return true;
}

// ENOENT is "file not found" on Windows when the folder exists and "path not found" when it does not.
DWORD notFound(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return ERROR_FILE_NOT_FOUND;
    struct stat st;
    return stat(path.substr(0, slash).c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? ERROR_FILE_NOT_FOUND
                                                                                 : ERROR_PATH_NOT_FOUND;
}

void setErrorFor(const std::string& path) {
    lastError = errno == ENOENT ? notFound(path) : flWin32FromErrno(errno);
}

// ---- handles ---------------------------------------------------------------------------------------------------
enum class Kind { File, Event, Process, Find };
struct Object {
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
    Kind kind;
};

struct FileObject : Object {
    FileObject(int f, bool owned) : Object(Kind::File), fd(f), own(owned) {}
    ~FileObject() override {
        if (own && fd >= 0) close(fd);
        if (!deleteOnClose.empty()) unlink(deleteOnClose.c_str());
    }
    int fd;
    bool own;
    std::string deleteOnClose;
};

struct EventObject : Object {
    EventObject(bool manual, bool initial) : Object(Kind::Event), manual(manual), signaled(initial) {}
    std::mutex mu;
    std::condition_variable cv;
    bool manual, signaled;
};

// One per process id, shared by every handle to it, so only one waiter reaps it and all see the exit code.
struct ProcessState {
    std::mutex mu;
    pid_t pid = 0;
    bool done = false, child = true;
    DWORD code = STILL_ACTIVE;
    int forcedCode = -1;  // TerminateProcess's code, reported instead of the signal
};
std::mutex processesMu;
std::map<pid_t, std::weak_ptr<ProcessState>> processes;

struct ProcessObject : Object {
    explicit ProcessObject(std::shared_ptr<ProcessState> s) : Object(Kind::Process), state(std::move(s)) {}
    std::shared_ptr<ProcessState> state;
};

struct FindObject : Object {
    FindObject() : Object(Kind::Find) {}
    std::vector<WIN32_FIND_DATAW> entries;
    size_t next = 0;
};

Object* obj(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
    return static_cast<Object*>(h);
}
FileObject* file(HANDLE h) {
    Object* o = obj(h);
    return o && o->kind == Kind::File ? static_cast<FileObject*>(o) : nullptr;
}

FileObject stdIn(0, false), stdOut(1, false), stdErr(2, false);
HANDLE stdHandles[3] = {&stdIn, &stdOut, &stdErr};
bool isStd(HANDLE h) { return h == &stdIn || h == &stdOut || h == &stdErr; }

// Polls a process for its end without blocking; true once it has ended.
bool processEnded(ProcessState& s) {
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.done) return true;
    if (s.child) {
        int status = 0;
        pid_t r = waitpid(s.pid, &status, WNOHANG);
        if (r == s.pid) {
            s.done = true;
            if (s.forcedCode >= 0) s.code = static_cast<DWORD>(s.forcedCode);
            else if (WIFEXITED(status)) s.code = static_cast<DWORD>(WEXITSTATUS(status));
            else if (WIFSIGNALED(status)) s.code = 128u + static_cast<DWORD>(WTERMSIG(status));
            else s.code = 1;
            return true;
        }
        if (r == 0) return false;
        s.child = false;  // ECHILD: someone else's process, or already reaped
    }
    if (kill(s.pid, 0) == 0 || errno == EPERM) {
        // A zombie still answers kill(); its /proc state says it is dead.
        char path[64], buf[512];
        snprintf(path, sizeof path, "/proc/%d/stat", static_cast<int>(s.pid));
        FILE* f = fopen(path, "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[n] = 0;
            const char* close = strrchr(buf, ')');
            if (!close || (close[2] != 'Z' && close[2] != 'X')) return false;
        } else {
            return false;
        }
    }
    s.done = true;
    if (s.code == STILL_ACTIVE) s.code = s.forcedCode >= 0 ? static_cast<DWORD>(s.forcedCode) : 0;
    return true;
}

// True when `h` is signaled now; an auto-reset event is reset by the check.
bool signaledNow(HANDLE h, bool consume) {
    Object* o = obj(h);
    if (!o) return false;
    if (o->kind == Kind::Event) {
        auto* e = static_cast<EventObject*>(o);
        std::lock_guard<std::mutex> lock(e->mu);
        if (!e->signaled) return false;
        if (consume && !e->manual) e->signaled = false;
        return true;
    }
    if (o->kind == Kind::Process) return processEnded(*static_cast<ProcessObject*>(o)->state);
    return true;  // files are always signaled
}

// ---- paths -----------------------------------------------------------------------------------------------------
bool lexists(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

// The real name in `dir` that matches `name` ignoring case, or empty.
std::string matchInDir(const std::string& dir, const std::string& name) {
    DIR* d = opendir(dir.empty() ? "/" : dir.c_str());
    if (!d) return {};
    std::string found;
    while (dirent* e = readdir(d)) {
        if (ieq(e->d_name, name)) {
            found = e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string homeDir() {
    const char* h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

std::string xdg(const char* var, const char* fallback) {
    const char* v = getenv(var);
    return v && *v == '/' ? std::string(v) : homeDir() + "/" + fallback;
}

}  // namespace

std::string flOsPath(const std::wstring& path) {
    std::string p = toUtf8(path.data(), path.size());
    for (auto& c : p)
        if (c == '\\') c = '/';
    if (p.rfind("//?/", 0) == 0) p.erase(0, 3);
    if (p.empty() || lexists(p)) return p;
    if (ieq(p, "NUL")) return "/dev/null";
    // Walk down from the root, fixing the case of each part that does not exist as written.
    std::string out;
    size_t i = 0;
    if (p[0] == '/') {
        out = "";
        while (i < p.size() && p[i] == '/') ++i;
    } else {
        out = ".";
    }
    bool missing = false;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        if (!part.empty()) {
            std::string next = out + "/" + part;
            if (!missing && !lexists(next) && part != "." && part != "..") {
                std::string real = matchInDir(out.empty() ? "/" : out, part);
                if (!real.empty()) next = out + "/" + real;
                else missing = true;
            }
            out = next;
        }
        i = j + 1;
    }
    if (p[0] != '/') out.erase(0, 2);  // "./"
    if (p.back() == '/' && (out.empty() || out.back() != '/')) out += '/';
    return out;
}

std::string flOsPath(const wchar_t* path) { return path ? flOsPath(std::wstring(path)) : std::string(); }

DWORD flWin32FromErrno(int e) {
    switch (e) {
        case 0: return ERROR_SUCCESS;
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:
        case EISDIR: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_ALREADY_EXISTS;
        case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
        case ENOSPC:
        case EDQUOT: return ERROR_DISK_FULL;
        case EROFS: return ERROR_WRITE_PROTECT;
        case EBUSY:
        case ETXTBSY: return ERROR_SHARING_VIOLATION;
        case EXDEV: return ERROR_NOT_SAME_DEVICE;
        case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
        case EMFILE:
        case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        case EINVAL: return ERROR_INVALID_PARAMETER;
        case EBADF: return ERROR_INVALID_HANDLE;
        case EPIPE: return ERROR_BROKEN_PIPE;
        case ETIMEDOUT: return ERROR_TIMEOUT;
        default: return ERROR_NOT_SUPPORTED;
    }
}

DWORD GetLastError() { return lastError; }
void SetLastError(DWORD code) { lastError = code; }

// ---- files -----------------------------------------------------------------------------------------------------
namespace {

// "\\.\pipe\discord-ipc-0": on Linux Discord listens on a Unix socket of that name in the runtime folder (or a
// Flatpak/Snap one under it).
HANDLE openPipe(const std::wstring& name) {
    std::string n = u8(name.c_str());
    std::vector<std::string> dirs;
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
        if (const char* v = getenv(var); v && *v) dirs.push_back(v);
    dirs.push_back("/tmp");
    std::vector<std::string> candidates;
    for (const auto& d : dirs) {
        candidates.push_back(d + "/" + n);
        candidates.push_back(d + "/app/com.discordapp.Discord/" + n);
        candidates.push_back(d + "/app/dev.vencord.Vesktop/" + n);
        candidates.push_back(d + "/snap.discord/" + n);
    }
    for (const auto& path : candidates) {
        if (path.size() >= sizeof(sockaddr_un::sun_path)) continue;
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, path.c_str());
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) return new FileObject(fd, true);
        close(fd);
    }
    lastError = ERROR_FILE_NOT_FOUND;
    return INVALID_HANDLE_VALUE;
}

}  // namespace

HANDLE CreateFileW(LPCWSTR wpath, DWORD access, DWORD, SECURITY_ATTRIBUTES* sa, DWORD disposition, DWORD flags, HANDLE) {
    std::wstring raw = wpath ? wpath : L"";
    if (raw.rfind(L"\\\\.\\pipe\\", 0) == 0) return openPipe(raw.substr(9));
    std::string path = flOsPath(raw);
    if (path.empty()) {
        lastError = ERROR_PATH_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    int mode = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
        case CREATE_NEW: mode |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: mode |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: mode |= O_CREAT; break;
        case TRUNCATE_EXISTING: mode |= O_TRUNC; break;
        default: break;
    }
    if (!(sa && sa->bInheritHandle)) mode |= O_CLOEXEC;
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
        lastError = ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    int fd = open(path.c_str(), mode, 0666);
    if (fd < 0) {
        if (errno == EEXIST) lastError = ERROR_FILE_EXISTS;
        else setErrorFor(path);
        return INVALID_HANDLE_VALUE;
    }
    auto* f = new FileObject(fd, true);
    if (flags & FILE_FLAG_DELETE_ON_CLOSE) f->deleteOnClose = path;
    lastError = 0;
    return f;
}

BOOL ReadFile(HANDLE h, void* buf, DWORD size, DWORD* read, void*) {
    FileObject* f = file(h);
    if (read) *read = 0;
    if (!f) return fail(ERROR_INVALID_HANDLE);
    for (;;) {
        ssize_t n = ::read(f->fd, buf, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            if (errno == ECONNRESET) return fail(ERROR_BROKEN_PIPE);
            return failErrno();
        }
        if (read) *read = static_cast<DWORD>(n);
        return TRUE;
    }
}

BOOL WriteFile(HANDLE h, const void* buf, DWORD size, DWORD* written, void*) {
    FileObject* f = file(h);
    if (written) *written = 0;
    if (!f) return fail(ERROR_INVALID_HANDLE);
    const char* p = static_cast<const char*>(buf);
    DWORD done = 0;
    while (done < size) {
        ssize_t n = ::write(f->fd, p + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            if (written) *written = done;
            return failErrno();
        }
        done += static_cast<DWORD>(n);
    }
    if (written) *written = done;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h) {
    FileObject* f = file(h);
    if (!f) return fail(ERROR_INVALID_HANDLE);
    return fsync(f->fd) == 0 || errno == EINVAL ? TRUE : failErrno();
}

BOOL CloseHandle(HANDLE h) {
    Object* o = obj(h);
    if (!o) return fail(ERROR_INVALID_HANDLE);
    if (isStd(h)) {
        // Closing a standard handle closes the descriptor, as on Windows; the object stays.
        auto* f = static_cast<FileObject*>(o);
        if (f->fd >= 0) close(f->fd);
        f->fd = -1;
        return TRUE;
    }
    delete o;
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    FileObject* f = file(h);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return fail(ERROR_INVALID_HANDLE);
    size->QuadPart = st.st_size;
    return TRUE;
}

DWORD GetFileType(HANDLE h) {
    FileObject* f = file(h);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return FILE_TYPE_UNKNOWN;
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return FILE_TYPE_DISK;
    if (S_ISCHR(st.st_mode)) return FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return FILE_TYPE_PIPE;
    return FILE_TYPE_UNKNOWN;
}

BOOL PeekNamedPipe(HANDLE h, void* buf, DWORD size, DWORD* read, DWORD* avail, DWORD* left) {
    FileObject* f = file(h);
    if (!f) return fail(ERROR_INVALID_HANDLE);
    int n = 0;
    if (ioctl(f->fd, FIONREAD, &n) != 0) return failErrno();
    if (n == 0) {
        // Nothing waiting: a closed peer reads as end of file, which Windows reports as a broken pipe.
        pollfd p{f->fd, POLLIN, 0};
        if (poll(&p, 1, 0) == 1 && (p.revents & (POLLHUP | POLLERR | POLLIN))) {
            char c;
            if (recv(f->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0 || (p.revents & (POLLHUP | POLLERR)))
                return fail(ERROR_BROKEN_PIPE);
        }
    }
    if (buf && size && n > 0) {
        ssize_t got = recv(f->fd, buf, size, MSG_PEEK | MSG_DONTWAIT);
        if (read) *read = got > 0 ? static_cast<DWORD>(got) : 0;
    } else if (read) {
        *read = 0;
    }
    if (avail) *avail = static_cast<DWORD>(n);
    if (left) *left = 0;
    return TRUE;
}

HANDLE GetStdHandle(DWORD which) {
    if (which == STD_INPUT_HANDLE) return stdHandles[0];
    if (which == STD_OUTPUT_HANDLE) return stdHandles[1];
    if (which == STD_ERROR_HANDLE) return stdHandles[2];
    return INVALID_HANDLE_VALUE;
}

BOOL SetStdHandle(DWORD which, HANDLE h) {
    int slot = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
    FileObject* f = file(h);
    if (slot < 0 || !f) return fail(ERROR_INVALID_PARAMETER);
    if (dup2(f->fd, slot) < 0) return failErrno();
    return TRUE;
}

namespace {

uint64_t fileTime(const timespec& t) {
    return (static_cast<uint64_t>(t.tv_sec) + 11644473600ull) * 10000000ull + static_cast<uint64_t>(t.tv_nsec) / 100;
}
void setFileTime(FILETIME& ft, const timespec& t) {
    uint64_t v = fileTime(t);
    ft.dwLowDateTime = static_cast<DWORD>(v);
    ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
}

DWORD attributesOf(const struct stat& st) {
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

}  // namespace

DWORD GetFileAttributesW(LPCWSTR p) {
    std::string path = flOsPath(p);
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0) {
        if (path.empty()) lastError = ERROR_PATH_NOT_FOUND;
        else setErrorFor(path);
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributesOf(st);
}

BOOL GetFileAttributesExW(LPCWSTR p, GET_FILEEX_INFO_LEVELS, void* out) {
    std::string path = flOsPath(p);
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0) {
        setErrorFor(path);
        return FALSE;
    }
    auto* d = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(out);
    d->dwFileAttributes = attributesOf(st);
    setFileTime(d->ftCreationTime, st.st_ctim);
    setFileTime(d->ftLastAccessTime, st.st_atim);
    setFileTime(d->ftLastWriteTime, st.st_mtim);
    uint64_t size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
    d->nFileSizeHigh = static_cast<DWORD>(size >> 32);
    d->nFileSizeLow = static_cast<DWORD>(size);
    return TRUE;
}

BOOL SetFileAttributesW(LPCWSTR p, DWORD attrs) {
    std::string path = flOsPath(p);
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        setErrorFor(path);
        return FALSE;
    }
    mode_t mode = st.st_mode & 07777;
    if (attrs & FILE_ATTRIBUTE_READONLY) mode &= ~static_cast<mode_t>(S_IWUSR | S_IWGRP | S_IWOTH);
    else mode |= S_IWUSR;
    return chmod(path.c_str(), mode) == 0 ? TRUE : failErrno();
}

BOOL CreateDirectoryW(LPCWSTR p, SECURITY_ATTRIBUTES*) {
    std::string path = flOsPath(p);
    if (mkdir(path.c_str(), 0777) == 0) return TRUE;
    setErrorFor(path);
    return FALSE;
}

BOOL RemoveDirectoryW(LPCWSTR p) {
    std::string path = flOsPath(p);
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return unlink(path.c_str()) == 0 ? TRUE : failErrno();
    if (rmdir(path.c_str()) == 0) return TRUE;
    if (errno == EEXIST) errno = ENOTEMPTY;
    setErrorFor(path);
    return FALSE;
}

BOOL DeleteFileW(LPCWSTR p) {
    std::string path = flOsPath(p);
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return fail(ERROR_ACCESS_DENIED);
    if (unlink(path.c_str()) == 0) return TRUE;
    setErrorFor(path);
    return FALSE;
}

namespace {

bool copyFile(const std::string& from, const std::string& to, bool exclusive) {
    int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        setErrorFor(from);
        return false;
    }
    struct stat st;
    fstat(in, &st);
    int out = open(to.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC), st.st_mode & 0777);
    if (out < 0) {
        if (errno == EEXIST) lastError = ERROR_FILE_EXISTS;
        else setErrorFor(to);
        close(in);
        return false;
    }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        for (ssize_t done = 0; done < n;) {
            ssize_t m = write(out, buf.data() + done, static_cast<size_t>(n - done));
            if (m < 0 && errno == EINTR) continue;
            if (m < 0) {
                ok = false;
                break;
            }
            done += m;
        }
        if (!ok) break;
    }
    if (!ok) lastError = flWin32FromErrno(errno);
    fchmod(out, st.st_mode & 0777);
    timespec times[2] = {st.st_atim, st.st_mtim};  // CopyFile keeps the last-write time
    futimens(out, times);
    if (close(out) != 0 && ok) {
        ok = false;
        lastError = flWin32FromErrno(errno);
    }
    close(in);
    if (!ok) unlink(to.c_str());
    return ok;
}

}  // namespace

BOOL MoveFileExW(LPCWSTR f, LPCWSTR t, DWORD flags) {
    std::string from = flOsPath(f), to = flOsPath(t);
    struct stat src, dst;
    if (lstat(from.c_str(), &src) != 0) {
        setErrorFor(from);
        return FALSE;
    }
    bool targetExists = lstat(to.c_str(), &dst) == 0;
    // The same file under another case of its name is a rename on Windows, not a replace.
    bool sameFile = targetExists && src.st_dev == dst.st_dev && src.st_ino == dst.st_ino;
    if (targetExists && !sameFile) {
        if (!(flags & MOVEFILE_REPLACE_EXISTING)) return fail(ERROR_ALREADY_EXISTS);
        if (S_ISDIR(dst.st_mode)) return fail(ERROR_ACCESS_DENIED);
    }
    if (sameFile) {
        std::string asked = u8(t);
        to = to.substr(0, to.find_last_of('/') + 1) + asked.substr(asked.find_last_of("\\/") + 1);
    }
    if (rename(from.c_str(), to.c_str()) == 0) return TRUE;
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED) && S_ISREG(src.st_mode)) {
        if (!copyFile(from, to, false)) return FALSE;
        unlink(from.c_str());
        return TRUE;
    }
    if (errno == ENOENT) lastError = notFound(to);
    else lastError = flWin32FromErrno(errno);
    return FALSE;
}

BOOL CopyFileW(LPCWSTR f, LPCWSTR t, BOOL failIfExists) {
    return copyFile(flOsPath(f), flOsPath(t), failIfExists) ? TRUE : FALSE;
}

namespace {

bool wildMatch(const wchar_t* p, const wchar_t* s) {
    if (*p == 0) return *s == 0;
    if (*p == L'*') {
        while (p[1] == L'*') ++p;
        for (const wchar_t* t = s;; ++t) {
            if (wildMatch(p + 1, t)) return true;
            if (*t == 0) return false;
        }
    }
    if (*s == 0) return false;
    if (*p == L'?' || towlower(*p) == towlower(*s)) return wildMatch(p + 1, s + 1);
    return false;
}

}  // namespace

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* out) {
    std::wstring pat = pattern ? pattern : L"";
    size_t slash = pat.find_last_of(L"\\/");
    std::wstring dirW = slash == std::wstring::npos ? L"." : pat.substr(0, slash);
    std::wstring mask = slash == std::wstring::npos ? pat : pat.substr(slash + 1);
    if (dirW.empty()) dirW = L"/";
    std::string dir = flOsPath(dirW);
    DIR* d = opendir(dir.c_str());
    if (!d) {
        lastError = errno == ENOENT || errno == ENOTDIR ? ERROR_PATH_NOT_FOUND : flWin32FromErrno(errno);
        return INVALID_HANDLE_VALUE;
    }
    auto* f = new FindObject;
    bool wild = mask.find_first_of(L"*?") != std::wstring::npos;
    while (dirent* e = readdir(d)) {
        std::wstring name = w(e->d_name);
        if (wild ? !wildMatch(mask.c_str(), name.c_str()) : !ieq(e->d_name, u8(mask.c_str()))) continue;
        struct stat st;
        std::string full = dir + "/" + e->d_name;
        if (stat(full.c_str(), &st) != 0 && lstat(full.c_str(), &st) != 0) continue;
        WIN32_FIND_DATAW fd{};
        fd.dwFileAttributes = attributesOf(st);
        setFileTime(fd.ftCreationTime, st.st_ctim);
        setFileTime(fd.ftLastAccessTime, st.st_atim);
        setFileTime(fd.ftLastWriteTime, st.st_mtim);
        uint64_t size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
        fd.nFileSizeHigh = static_cast<DWORD>(size >> 32);
        fd.nFileSizeLow = static_cast<DWORD>(size);
        wcsncpy(fd.cFileName, name.c_str(), MAX_PATH * 4 - 1);
        f->entries.push_back(fd);
    }
    closedir(d);
    if (f->entries.empty()) {
        delete f;
        lastError = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    *out = f->entries[f->next++];
    return f;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out) {
    Object* o = obj(h);
    if (!o || o->kind != Kind::Find) return fail(ERROR_INVALID_HANDLE);
    auto* f = static_cast<FindObject*>(o);
    if (f->next >= f->entries.size()) return fail(ERROR_NO_MORE_FILES);
    *out = f->entries[f->next++];
    return TRUE;
}

BOOL FindClose(HANDLE h) { return CloseHandle(h); }

namespace {
DWORD copyOut(const std::wstring& s, wchar_t* buf, DWORD size) {
    if (!buf || size <= s.size()) return static_cast<DWORD>(s.size() + 1);
    wmemcpy(buf, s.c_str(), s.size() + 1);
    return static_cast<DWORD>(s.size());
}
}  // namespace

DWORD GetTempPathW(DWORD size, wchar_t* buf) {
    std::string t = "/tmp";
    for (const char* var : {"TMPDIR", "TMP", "TEMP"})
        if (const char* v = getenv(var); v && *v == '/') {
            t = v;
            break;
        }
    if (t.back() != '/') t += '/';
    return copyOut(w(t), buf, size);
}

DWORD GetModuleFileNameW(HMODULE, wchar_t* buf, DWORD size) {
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    std::wstring p = w(path);
    if (size == 0) return 0;
    size_t len = std::min<size_t>(p.size(), size - 1);
    wmemcpy(buf, p.c_str(), len);
    buf[len] = 0;
    if (len < p.size()) {
        lastError = ERROR_INSUFFICIENT_BUFFER;
        return size;
    }
    return static_cast<DWORD>(len);
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD size, wchar_t* buf, wchar_t** filePart) {
    std::wstring p = path ? path : L"";
    for (auto& c : p)
        if (c == L'\\') c = L'/';
    if (p.empty() || p[0] != L'/') {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        p = w(cwd) + L"/" + p;
    }
    // Collapse "." and ".." the way Windows does, without touching the disk.
    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(L'/', i);
        if (j == std::wstring::npos) j = p.size();
        std::wstring part = p.substr(i, j - i);
        if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::wstring out;
    for (const auto& part : parts) out += L"/" + part;
    if (out.empty()) out = L"/";
    DWORD r = copyOut(out, buf, size);
    if (filePart && buf && r < size) {
        size_t slash = out.find_last_of(L'/');
        *filePart = buf + slash + 1;
    }
    return r;
}

BOOL GetDiskFreeSpaceExW(LPCWSTR dir, ULARGE_INTEGER* avail, ULARGE_INTEGER* total, ULARGE_INTEGER* freeBytes) {
    struct statvfs s;
    if (statvfs(flOsPath(dir).c_str(), &s) != 0) return failErrno();
    if (avail) avail->QuadPart = static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
    if (total) total->QuadPart = static_cast<uint64_t>(s.f_blocks) * s.f_frsize;
    if (freeBytes) freeBytes->QuadPart = static_cast<uint64_t>(s.f_bfree) * s.f_frsize;
    return TRUE;
}

DWORD SearchPathW(LPCWSTR path, LPCWSTR name, LPCWSTR, DWORD size, wchar_t* buf, wchar_t** filePart) {
    std::string n = u8(name);
    std::vector<std::string> names{n};
    // "node.exe" is "node" here.
    if (n.size() > 4 && ieq(n.substr(n.size() - 4), ".exe")) names.push_back(n.substr(0, n.size() - 4));
    std::string dirs = path ? u8(path) : (getenv("PATH") ? getenv("PATH") : "/usr/local/bin:/usr/bin:/bin");
    for (char& c : dirs)
        if (c == ';') c = ':';
    size_t i = 0;
    while (i <= dirs.size()) {
        size_t j = dirs.find(':', i);
        if (j == std::string::npos) j = dirs.size();
        std::string d = dirs.substr(i, j - i);
        i = j + 1;
        if (d.empty()) continue;
        for (const auto& candidate : names) {
            std::string full = d + "/" + candidate;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) {
                DWORD r = copyOut(w(full), buf, size);
                if (filePart && buf && r < size) *filePart = buf + w(d).size() + 1;
                return r;
            }
        }
    }
    lastError = ERROR_FILE_NOT_FOUND;
    return 0;
}

// ---- events, processes, waiting -----------------------------------------------------------------------------
HANDLE CreateEventW(SECURITY_ATTRIBUTES*, BOOL manualReset, BOOL initialState, LPCWSTR) {
    return new EventObject(manualReset != FALSE, initialState != FALSE);
}

BOOL SetEvent(HANDLE h) {
    Object* o = obj(h);
    if (!o || o->kind != Kind::Event) return fail(ERROR_INVALID_HANDLE);
    auto* e = static_cast<EventObject*>(o);
    {
        std::lock_guard<std::mutex> lock(e->mu);
        e->signaled = true;
    }
    e->cv.notify_all();
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    Object* o = obj(h);
    if (!o || o->kind != Kind::Event) return fail(ERROR_INVALID_HANDLE);
    auto* e = static_cast<EventObject*>(o);
    std::lock_guard<std::mutex> lock(e->mu);
    e->signaled = false;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    Object* o = obj(h);
    if (!o) {
        lastError = ERROR_INVALID_HANDLE;
        return WAIT_FAILED;
    }
    if (o->kind == Kind::Event) {
        auto* e = static_cast<EventObject*>(o);
        std::unique_lock<std::mutex> lock(e->mu);
        auto ready = [&] { return e->signaled; };
        if (ms == INFINITE) e->cv.wait(lock, ready);
        else if (!e->cv.wait_for(lock, std::chrono::milliseconds(ms), ready)) return WAIT_TIMEOUT;
        if (!e->manual) e->signaled = false;
        return WAIT_OBJECT_0;
    }
    return WaitForMultipleObjects(1, &h, FALSE, ms);
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms) {
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS) {
        lastError = ERROR_INVALID_PARAMETER;
        return WAIT_FAILED;
    }
    for (DWORD i = 0; i < count; ++i)
        if (!obj(handles[i])) {
            lastError = ERROR_INVALID_HANDLE;
            return WAIT_FAILED;
        }
    auto start = std::chrono::steady_clock::now();
    // Processes have no descriptor to wait on in every kernel this runs on, so poll; 10 ms is well under what any
    // caller notices.
    for (;;) {
        if (waitAll) {
            bool all = true;
            for (DWORD i = 0; i < count && all; ++i) all = signaledNow(handles[i], false);
            if (all) {
                for (DWORD i = 0; i < count; ++i) signaledNow(handles[i], true);
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < count; ++i)
                if (signaledNow(handles[i], true)) return WAIT_OBJECT_0 + i;
        }
        if (ms != INFINITE &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(ms))
            return WAIT_TIMEOUT;
        // A lone event: sleep on it rather than poll.
        Object* first = obj(handles[0]);
        if (count == 1 && first->kind == Kind::Event) return WaitForSingleObject(handles[0], ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

HANDLE flProcessHandle(pid_t pid) {
    std::lock_guard<std::mutex> lock(processesMu);
    std::shared_ptr<ProcessState> s = processes[pid].lock();
    if (!s) {
        s = std::make_shared<ProcessState>();
        s->pid = pid;
        processes[pid] = s;
    }
    for (auto it = processes.begin(); it != processes.end();) it = it->second.expired() ? processes.erase(it) : ++it;
    return new ProcessObject(s);
}

HANDLE flFileHandle(int fd) { return fd < 0 ? INVALID_HANDLE_VALUE : new FileObject(fd, true); }

pid_t flProcessId(HANDLE h) {
    Object* o = obj(h);
    return o && o->kind == Kind::Process ? static_cast<ProcessObject*>(o)->state->pid : 0;
}

BOOL GetExitCodeProcess(HANDLE h, DWORD* code) {
    Object* o = obj(h);
    if (!o || o->kind != Kind::Process) return fail(ERROR_INVALID_HANDLE);
    auto& s = *static_cast<ProcessObject*>(o)->state;
    processEnded(s);
    std::lock_guard<std::mutex> lock(s.mu);
    *code = s.done ? s.code : STILL_ACTIVE;
    return TRUE;
}

BOOL TerminateProcess(HANDLE h, UINT code) {
    Object* o = obj(h);
    if (!o || o->kind != Kind::Process) return fail(ERROR_INVALID_HANDLE);
    auto& s = *static_cast<ProcessObject*>(o)->state;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.done) return fail(ERROR_ACCESS_DENIED);
        s.forcedCode = static_cast<int>(code);
    }
    return kill(s.pid, SIGKILL) == 0 ? TRUE : failErrno();
}

DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }

void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

ULONGLONG GetTickCount64() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<ULONGLONG>(t.tv_sec) * 1000ull + static_cast<ULONGLONG>(t.tv_nsec) / 1000000ull;
}

namespace {
void systemTime(SYSTEMTIME* out, bool local) {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    tm parts;
    if (local) localtime_r(&t.tv_sec, &parts);
    else gmtime_r(&t.tv_sec, &parts);
    out->wYear = static_cast<WORD>(parts.tm_year + 1900);
    out->wMonth = static_cast<WORD>(parts.tm_mon + 1);
    out->wDayOfWeek = static_cast<WORD>(parts.tm_wday);
    out->wDay = static_cast<WORD>(parts.tm_mday);
    out->wHour = static_cast<WORD>(parts.tm_hour);
    out->wMinute = static_cast<WORD>(parts.tm_min);
    out->wSecond = static_cast<WORD>(parts.tm_sec);
    out->wMilliseconds = static_cast<WORD>(t.tv_nsec / 1000000);
}
}  // namespace

void GetSystemTime(SYSTEMTIME* out) { systemTime(out, false); }
void GetLocalTime(SYSTEMTIME* out) { systemTime(out, true); }

// ---- environment, text -------------------------------------------------------------------------------------------
namespace {

// Windows variables launcher.json uses (${env:LOCALAPPDATA}...), with their XDG counterparts when unset, so one
// config file serves both.
std::string windowsVariable(const std::string& name) {
    std::string up;
    for (char c : name) up += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (up == "LOCALAPPDATA") return xdg("XDG_DATA_HOME", ".local/share");
    if (up == "APPDATA") return xdg("XDG_CONFIG_HOME", ".config");
    if (up == "USERPROFILE" || up == "HOMEPATH") return homeDir();
    if (up == "TEMP" || up == "TMP") {
        const char* t = getenv("TMPDIR");
        return t && *t ? t : "/tmp";
    }
    if (up == "USERNAME") {
        const char* u = getenv("USER");
        return u ? u : "";
    }
    if (up == "COMPUTERNAME") {
        char host[256] = "";
        gethostname(host, sizeof host - 1);
        return host;
    }
    return {};
}

}  // namespace

DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* buf, DWORD size) {
    std::string n = u8(name);
    const char* v = getenv(n.c_str());
    std::string value = v ? v : windowsVariable(n);
    if (!v && value.empty()) {
        lastError = 203;  // ERROR_ENVVAR_NOT_FOUND
        return 0;
    }
    return copyOut(w(value), buf, size);
}

DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD size) {
    const char* v = getenv(name);
    std::string value = v ? v : windowsVariable(name);
    if (!v && value.empty()) {
        lastError = 203;
        return 0;
    }
    if (!buf || size <= value.size()) return static_cast<DWORD>(value.size() + 1);
    memcpy(buf, value.c_str(), value.size() + 1);
    return static_cast<DWORD>(value.size());
}

wchar_t* GetEnvironmentStringsW() {
    std::wstring block;
    for (char** e = environ; e && *e; ++e) block += w(*e) + L'\0';
    block += L'\0';
    auto* out = static_cast<wchar_t*>(malloc(block.size() * sizeof(wchar_t)));
    wmemcpy(out, block.data(), block.size());
    return out;
}

BOOL FreeEnvironmentStringsW(wchar_t* block) {
    free(block);
    return TRUE;
}

int MultiByteToWideChar(UINT, DWORD, const char* s, int n, wchar_t* out, int outSize) {
    size_t len = n < 0 ? strlen(s) + 1 : static_cast<size_t>(n);
    std::wstring r = fromUtf8(s, len);
    if (!out || outSize == 0) return static_cast<int>(r.size());
    if (static_cast<int>(r.size()) > outSize) {
        lastError = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    wmemcpy(out, r.data(), r.size());
    return static_cast<int>(r.size());
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* s, int n, char* out, int outSize, const char*, BOOL* usedDef) {
    size_t len = n < 0 ? wcslen(s) + 1 : static_cast<size_t>(n);
    std::string r = toUtf8(s, len);
    if (usedDef) *usedDef = FALSE;
    if (!out || outSize == 0) return static_cast<int>(r.size());
    if (static_cast<int>(r.size()) > outSize) {
        lastError = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    memcpy(out, r.data(), r.size());
    return static_cast<int>(r.size());
}

DWORD CharLowerBuffW(wchar_t* s, DWORD n) {
    for (DWORD i = 0; i < n; ++i) s[i] = static_cast<wchar_t>(towlower(static_cast<wint_t>(s[i])));
    return n;
}

DWORD CharUpperBuffW(wchar_t* s, DWORD n) {
    for (DWORD i = 0; i < n; ++i) s[i] = static_cast<wchar_t>(towupper(static_cast<wint_t>(s[i])));
    return n;
}

int CompareStringOrdinal(const wchar_t* a, int na, const wchar_t* b, int nb, BOOL ignoreCase) {
    size_t la = na < 0 ? wcslen(a) : static_cast<size_t>(na), lb = nb < 0 ? wcslen(b) : static_cast<size_t>(nb);
    for (size_t i = 0; i < la && i < lb; ++i) {
        wint_t x = static_cast<wint_t>(a[i]), y = static_cast<wint_t>(b[i]);
        if (ignoreCase) x = towupper(x), y = towupper(y);
        if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    return la == lb ? CSTR_EQUAL : la < lb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

DWORD FormatMessageW(DWORD flags, const void*, DWORD code, DWORD, wchar_t* buf, DWORD size, void*) {
    // The texts Windows gives for the codes this layer reports.
    const wchar_t* text = nullptr;
    switch (code) {
        case ERROR_SUCCESS: text = L"The operation completed successfully."; break;
        case ERROR_FILE_NOT_FOUND: text = L"The system cannot find the file specified."; break;
        case ERROR_PATH_NOT_FOUND: text = L"The system cannot find the path specified."; break;
        case ERROR_TOO_MANY_OPEN_FILES: text = L"The system cannot open the file."; break;
        case ERROR_ACCESS_DENIED: text = L"Access is denied."; break;
        case ERROR_INVALID_HANDLE: text = L"The handle is invalid."; break;
        case ERROR_NOT_ENOUGH_MEMORY: text = L"Not enough memory resources are available to process this command."; break;
        case ERROR_WRITE_PROTECT: text = L"The media is write protected."; break;
        case ERROR_NOT_SAME_DEVICE: text = L"The system cannot move the file to a different disk drive."; break;
        case ERROR_SHARING_VIOLATION: text = L"The process cannot access the file because it is being used by another process."; break;
        case ERROR_LOCK_VIOLATION: text = L"The process cannot access the file because another process has locked a portion of the file."; break;
        case ERROR_NOT_SUPPORTED: text = L"The request is not supported."; break;
        case ERROR_FILE_EXISTS: text = L"The file exists."; break;
        case ERROR_INVALID_PARAMETER: text = L"The parameter is incorrect."; break;
        case ERROR_BROKEN_PIPE: text = L"The pipe has been ended."; break;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: text = L"There is not enough space on the disk."; break;
        case ERROR_INSUFFICIENT_BUFFER: text = L"The data area passed to a system call is too small."; break;
        case ERROR_DIR_NOT_EMPTY: text = L"The directory is not empty."; break;
        case ERROR_ALREADY_EXISTS: text = L"Cannot create a file when that file already exists."; break;
        case ERROR_FILENAME_EXCED_RANGE: text = L"The filename or extension is too long."; break;
        case ERROR_DIRECTORY: text = L"The directory name is invalid."; break;
        case ERROR_TIMEOUT: text = L"This operation returned because the timeout period expired."; break;
        default: break;
    }
    if (!text) return 0;
    size_t n = wcslen(text);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        auto* out = static_cast<wchar_t*>(malloc((n + 1) * sizeof(wchar_t)));
        wmemcpy(out, text, n + 1);
        *reinterpret_cast<wchar_t**>(buf) = out;
        return static_cast<DWORD>(n);
    }
    if (!buf || size <= n) return 0;
    wmemcpy(buf, text, n + 1);
    return static_cast<DWORD>(n);
}

void* LocalFree(void* p) {
    free(p);
    return nullptr;
}

long BCryptGenRandom(void*, PUCHAR buf, ULONG size, ULONG) {
    for (ULONG done = 0; done < size;) {
        ssize_t n = getrandom(buf + done, size - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += static_cast<ULONG>(n);
    }
    return 0;
}

int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    for (;; ++a, ++b) {
        wint_t x = towlower(static_cast<wint_t>(*a)), y = towlower(static_cast<wint_t>(*b));
        if (x != y || !x) return static_cast<int>(x) - static_cast<int>(y);
    }
}

int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; ++i, ++a, ++b) {
        wint_t x = towlower(static_cast<wint_t>(*a)), y = towlower(static_cast<wint_t>(*b));
        if (x != y || !x) return static_cast<int>(x) - static_cast<int>(y);
    }
    return 0;
}

int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }

FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
    std::string m = u8(mode);
    std::string clean;
    for (char c : m)
        if (c != 't' && c != ',' && c != ' ') clean += c;  // "rt", "w, ccs=UTF-8"
    if (size_t ccs = clean.find("ccs="); ccs != std::string::npos) clean.erase(ccs);
    return fopen(flOsPath(path).c_str(), (clean + "e").c_str());
}

// ---- sockets -------------------------------------------------------------------------------------------------
int ioctlsocket(SOCKET s, long cmd, u_long* arg) {
    if (cmd == static_cast<long>(FIONBIO)) {
        int flags = fcntl(s, F_GETFL, 0);
        if (flags < 0) return SOCKET_ERROR;
        flags = *arg ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        return fcntl(s, F_SETFL, flags) == 0 ? 0 : SOCKET_ERROR;
    }
    if (cmd == static_cast<long>(FIONREAD)) {
        int n = 0;
        if (ioctl(s, FIONREAD, &n) != 0) return SOCKET_ERROR;
        *arg = static_cast<u_long>(n);
        return 0;
    }
    errno = EINVAL;
    return SOCKET_ERROR;
}

#undef setsockopt
int flSetsockopt(SOCKET s, int level, int name, const void* value, socklen_t size) {
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && size == sizeof(DWORD)) {
        DWORD ms = *static_cast<const DWORD*>(value);
        timeval tv{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
        return ::setsockopt(s, level, name, &tv, sizeof tv);
    }
    if (level == SOL_SOCKET && (name == SO_REUSEADDR) && size == sizeof(BOOL)) {
        int on = *static_cast<const BOOL*>(value) ? 1 : 0;
        return ::setsockopt(s, level, name, &on, sizeof on);
    }
    return ::setsockopt(s, level, name, value, size);
}

const wchar_t* InetNtopW(int family, const void* addr, wchar_t* buf, size_t size) {
    char text[INET6_ADDRSTRLEN] = "";
    if (!inet_ntop(family, addr, text, sizeof text)) return nullptr;
    std::wstring t = w(text);
    if (t.size() >= size) return nullptr;
    wmemcpy(buf, t.c_str(), t.size() + 1);
    return buf;
}

// ---- command line ------------------------------------------------------------------------------------------------
wchar_t* GetCommandLineW() {
    static std::wstring line = [] {
        std::string raw;
        if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.append(buf, n);
            fclose(f);
        }
        std::wstring out;
        size_t i = 0;
        while (i < raw.size()) {
            size_t z = raw.find('\0', i);
            if (z == std::string::npos) z = raw.size();
            std::wstring arg = w(raw.substr(i, z - i));
            i = z + 1;
            if (!out.empty()) out += L' ';
            // The Windows quoting rules, which CommandLineToArgvW undoes.
            if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
                out += arg;
                continue;
            }
            out += L'"';
            size_t slashes = 0;
            for (wchar_t c : arg) {
                if (c == L'\\') {
                    ++slashes;
                    continue;
                }
                out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                slashes = 0;
                out += c;
            }
            out.append(slashes * 2, L'\\');
            out += L'"';
        }
        return out;
    }();
    return line.data();
}

wchar_t** CommandLineToArgvW(const wchar_t* line, int* argc) {
    std::vector<std::wstring> args;
    std::wstring cur;
    bool inArg = false, quoted = false;
    for (const wchar_t* p = line; p && *p;) {
        if (!quoted && (*p == L' ' || *p == L'\t')) {
            if (inArg) args.push_back(cur), cur.clear(), inArg = false;
            ++p;
            continue;
        }
        inArg = true;
        if (*p == L'\\') {
            size_t n = 0;
            while (*p == L'\\') ++n, ++p;
            if (*p == L'"') {
                cur.append(n / 2, L'\\');
                if (n % 2) cur += L'"', ++p;
            } else {
                cur.append(n, L'\\');
            }
            continue;
        }
        if (*p == L'"') {
            if (quoted && p[1] == L'"') cur += L'"', ++p;
            else quoted = !quoted;
            ++p;
            continue;
        }
        cur += *p++;
    }
    if (inArg) args.push_back(cur);
    // One block, freed with LocalFree: the pointers, then the strings.
    size_t bytes = (args.size() + 1) * sizeof(wchar_t*);
    for (const auto& a : args) bytes += (a.size() + 1) * sizeof(wchar_t);
    auto** out = static_cast<wchar_t**>(malloc(bytes));
    auto* text = reinterpret_cast<wchar_t*>(out + args.size() + 1);
    for (size_t i = 0; i < args.size(); ++i) {
        out[i] = text;
        wmemcpy(text, args[i].c_str(), args[i].size() + 1);
        text += args[i].size() + 1;
    }
    out[args.size()] = nullptr;
    if (argc) *argc = static_cast<int>(args.size());
    return out;
}
