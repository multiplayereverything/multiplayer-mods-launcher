#include "webserver.h"

#include "fs.h"
#include "log.h"
#include "strings.h"
#include "win.h"
// win.h first (WIN32_LEAN_AND_MEAN keeps the old winsock.h out).
#include <winsock2.h>
#include <ws2tcpip.h>

namespace fl {

namespace {

// Percent-decoding of a URL path; '+' stays '+' (it only means space in queries).
std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

void sendAll(SOCKET s, const char* data, size_t n) {
    while (n) {
        int sent = send(s, data, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
        if (sent <= 0) return;
        data += sent;
        n -= static_cast<size_t>(sent);
    }
}

void sendStatus(SOCKET s, const char* status) {
    std::string r = std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sendAll(s, r.data(), r.size());
}

}  // namespace

std::string mimeType(const std::wstring& fileName) {
    std::wstring ext = toLower(fileName.substr(fileName.find_last_of(L'.') == std::wstring::npos
                                                   ? fileName.size()
                                                   : fileName.find_last_of(L'.') + 1));
    static const std::pair<const wchar_t*, const char*> types[] = {
        {L"html", "text/html; charset=utf-8"}, {L"htm", "text/html; charset=utf-8"},
        {L"js", "text/javascript; charset=utf-8"}, {L"mjs", "text/javascript; charset=utf-8"},
        {L"css", "text/css; charset=utf-8"}, {L"json", "application/json"}, {L"wasm", "application/wasm"},
        {L"png", "image/png"}, {L"jpg", "image/jpeg"}, {L"jpeg", "image/jpeg"}, {L"gif", "image/gif"},
        {L"webp", "image/webp"}, {L"svg", "image/svg+xml"}, {L"ico", "image/x-icon"},
        {L"mp3", "audio/mpeg"}, {L"ogg", "audio/ogg"}, {L"wav", "audio/wav"}, {L"m4a", "audio/mp4"},
        {L"mp4", "video/mp4"}, {L"webm", "video/webm"}, {L"woff", "font/woff"}, {L"woff2", "font/woff2"},
        {L"ttf", "font/ttf"}, {L"otf", "font/otf"}, {L"xml", "application/xml"}, {L"txt", "text/plain; charset=utf-8"},
        {L"swf", "application/x-shockwave-flash"},
    };
    for (const auto& [e, t] : types)
        if (ext == e) return t;
    return "application/octet-stream";
}

WebServer::~WebServer() { stop(); }

bool WebServer::start(const std::wstring& root, int port, std::wstring& error) {
    stop();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = L"Networking is not available";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from other PCs
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR || listen(s, SOMAXCONN) == SOCKET_ERROR) {
        error = win32ErrorMessage(WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return false;
    }
    int len = sizeof addr;
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    root_ = fs::normalize(root);
    listener_ = s;
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
    logger().info(L"Serving " + root_ + L" at http://127.0.0.1:" + std::to_wstring(port_) + L"/");
    return true;
}

void WebServer::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    closesocket(static_cast<SOCKET>(listener_));  // wakes accept()
    thread_.join();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(mu_);
        workers.swap(workers_);
    }
    for (auto& w : workers)
        if (w.joinable()) w.join();
    listener_ = ~uintptr_t(0);
    WSACleanup();
}

void WebServer::loop() {
    while (!stop_) {
        SOCKET c = accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        // Browsers open several connections at once; each gets a short-lived thread.
        std::lock_guard<std::mutex> lock(mu_);
        workers_.emplace_back([this, c] { serve(c); });
    }
}

void WebServer::serve(uintptr_t client) {
    SOCKET c = static_cast<SOCKET>(client);
    DWORD timeout = 10000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    std::string req;
    char buf[8192];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 64 * 1024) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, n);
    }
    std::string line = req.substr(0, req.find("\r\n"));
    size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    std::string method = line.substr(0, sp1);
    std::string target = sp1 == std::string::npos ? "" : line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method != "GET" && method != "HEAD") {
        sendStatus(c, "405 Method Not Allowed");
        closesocket(c);
        return;
    }
    std::string path = urlDecode(target.substr(0, target.find_first_of("?#")));
    if (path.empty() || path.back() == '/') path += "index.html";
    std::wstring rel = widen(path.substr(1));
    for (auto& ch : rel)
        if (ch == L'/') ch = L'\\';
    std::wstring file = fs::normalize(fs::join(root_, rel));
    // "..", drive letters or encoded slashes can't leave the game's folder.
    if (!fs::isUnder(file, root_) || !fs::isFile(file)) {
        sendStatus(c, "404 Not Found");
        closesocket(c);
        return;
    }
    std::vector<uint8_t> body;
    fs::readFile(file, body);
    size_t from = 0, to = body.empty() ? 0 : body.size() - 1;
    bool partial = false;
    // Audio and video elements ask for byte ranges ("Range: bytes=100-").
    std::string lower = req;
    for (auto& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (size_t r = lower.find("\r\nrange: bytes="); r != std::string::npos && !body.empty()) {
        std::string spec = req.substr(r + 15, req.find("\r\n", r + 2) - r - 15);
        size_t dash = spec.find('-');
        if (dash != std::string::npos) {
            std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
            if (a.empty() && !b.empty()) {
                size_t n = std::min<size_t>(strtoull(b.c_str(), nullptr, 10), body.size());
                from = body.size() - n;
            } else {
                from = strtoull(a.c_str(), nullptr, 10);
                if (!b.empty()) to = std::min<size_t>(strtoull(b.c_str(), nullptr, 10), body.size() - 1);
            }
            if (from > to || from >= body.size()) {
                sendStatus(c, "416 Range Not Satisfiable");
                closesocket(c);
                return;
            }
            partial = true;
        }
    }
    size_t length = body.empty() ? 0 : to - from + 1;
    std::string head = std::string("HTTP/1.1 ") + (partial ? "206 Partial Content" : "200 OK") +
                       "\r\nContent-Type: " + mimeType(file) + "\r\nContent-Length: " + std::to_string(length) +
                       "\r\nAccept-Ranges: bytes\r\nCache-Control: no-cache\r\nConnection: close\r\n";
    if (partial)
        head += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" +
                std::to_string(body.size()) + "\r\n";
    head += "\r\n";
    sendAll(c, head.data(), head.size());
    if (method == "GET" && length) sendAll(c, reinterpret_cast<const char*>(body.data()) + from, length);
    shutdown(c, SD_SEND);
    closesocket(c);
}

}  // namespace fl
