#include "core/posix/http.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/strings.h"
#include "core/win.h"

namespace fl::sys {

namespace {

// curl's exit codes worth putting in words; the rest show curl's own message.
std::wstring curlProblem(int code, const std::string& stderrText) {
    std::string msg = stderrText;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    if (msg.rfind("curl: ", 0) == 0) msg.erase(0, 6);
    switch (code) {
    case 6: return L"Couldn't find the server (no internet connection?)";
    case 7: return L"Couldn't connect to the server";
    case 28: return L"The server took too long to answer";
    case 35:
    case 60: return L"The secure connection failed: " + widen(msg);
    case 127: return L"curl isn't installed; the launcher needs it to download (sudo apt install curl)";
    default: return msg.empty() ? L"curl failed with code " + std::to_wstring(code) : widen(msg);
    }
}

// One "HTTP/x 200 ..." block, ended by an empty line.
bool parseHead(const std::string& block, HttpHead& out) {
    size_t eol = block.find("\r\n");
    std::string first = block.substr(0, eol);
    if (first.rfind("HTTP/", 0) != 0) return false;
    size_t sp = first.find(' ');
    out.status = sp == std::string::npos ? 0 : atoi(first.c_str() + sp + 1);
    out.headers.clear();
    for (size_t p = eol == std::string::npos ? block.size() : eol + 2; p < block.size();) {
        size_t e = block.find("\r\n", p);
        if (e == std::string::npos) e = block.size();
        std::string line = block.substr(p, e - p);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon), value = line.substr(colon + 1);
            for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            size_t a = value.find_first_not_of(" \t");
            value = a == std::string::npos ? "" : value.substr(a);
            out.headers[name] = value;
        }
        p = e + 2;
    }
    return true;
}

}  // namespace

bool curlGet(const std::wstring& url, const std::string& userAgent, const std::string& accept,
             const std::function<bool(const HttpHead&)>& onHeaders, const std::function<bool(const char*, size_t)>& sink,
             const std::atomic<bool>* cancel, std::wstring& error) {
    std::string u = narrow(url);
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) {
        error = L"Bad download address: " + url;
        return false;
    }
    // Headers and body on one stream (-D -), so the headers are known before the first body byte.
    std::vector<std::string> args = {"curl", "-sS", "-L", "--max-redirs", "20", "--connect-timeout", "30",
                                     "--suppress-connect-headers", "-A", userAgent, "-H", "Accept: " + accept, "-D", "-",
                                     "-o", "-", "--", u};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    int out[2], err[2];
    if (pipe2(out, O_CLOEXEC) != 0) return false;
    if (pipe2(err, O_CLOEXEC) != 0) {
        close(out[0]);
        close(out[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        signal(SIGPIPE, SIG_DFL);
        execvp("curl", argv.data());
        _exit(127);
    }
    close(out[1]);
    close(err[1]);
    if (pid < 0) {
        close(out[0]);
        close(err[0]);
        error = L"Cannot start curl";
        return false;
    }

    std::string head, errText;
    HttpHead final;
    bool inBody = false, ok = true, stopped = false;
    std::vector<char> buf(256 * 1024);
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    int live = 2;
    while (live > 0) {
        if (cancel && *cancel) {
            error = L"Cancelled";
            ok = false;
            stopped = true;
            break;
        }
        int r = poll(fds, 2, 200);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (fds[1].revents) {
            ssize_t n = read(err[0], buf.data(), buf.size());
            if (n > 0) errText.append(buf.data(), static_cast<size_t>(n));
            else {
                fds[1].fd = -1;
                --live;
            }
        }
        if (!fds[0].revents) continue;
        ssize_t n = read(out[0], buf.data(), buf.size());
        if (n <= 0) {
            fds[0].fd = -1;
            --live;
            continue;
        }
        const char* data = buf.data();
        size_t len = static_cast<size_t>(n);
        while (!inBody && len) {
            head.append(data, len);
            len = 0;
            size_t end;
            // Every block before the last one is a redirect or a 1xx; the body starts after the last.
            while (!inBody && (end = head.find("\r\n\r\n")) != std::string::npos) {
                HttpHead h;
                if (!parseHead(head.substr(0, end), h)) {
                    error = L"The server's answer couldn't be read";
                    ok = false;
                    stopped = true;
                    break;
                }
                std::string rest = head.substr(end + 4);
                bool more = (h.status >= 100 && h.status < 200) ||
                            (h.status >= 300 && h.status < 400 && h.headers.count("location"));
                if (more) {
                    head = rest;
                    continue;
                }
                final = h;
                inBody = true;
                if (onHeaders && !onHeaders(final)) {
                    stopped = true;
                    break;
                }
                if (!rest.empty() && !sink(rest.data(), rest.size())) {
                    error = L"Could not write the download to disk";
                    ok = false;
                    stopped = true;
                }
                head.clear();
            }
            if (stopped) break;
        }
        if (stopped) break;
        if (inBody && len && !sink(data, len)) {
            error = L"Could not write the download to disk";
            ok = false;
            stopped = true;
            break;
        }
    }
    if (stopped) kill(pid, SIGKILL);
    close(out[0]);
    close(err[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (stopped) return ok && inBody;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code != 0) {
        error = curlProblem(code, errText);
        return false;
    }
    if (!inBody) {
        // A body-less answer ends with the header block and nothing after it.
        HttpHead h;
        if (!parseHead(head.substr(0, head.find("\r\n\r\n")), h)) {
            error = L"The server's answer couldn't be read";
            return false;
        }
        if (onHeaders && !onHeaders(h)) return true;
    }
    return true;
}

}  // namespace fl::sys
