#include "relay.h"

#include <algorithm>
#include <chrono>

#include "log.h"
#include "strings.h"
#include "win.h"
// win.h first (WIN32_LEAN_AND_MEAN keeps the old winsock.h out).
#include <winsock2.h>
#include <ws2tcpip.h>

namespace fl {

namespace {

constexpr char kPolicyRequest[] = "<policy-file-request/>";
constexpr size_t kPolicyRequestLen = sizeof kPolicyRequest - 1;
constexpr char kPolicy[] =
    "<?xml version=\"1.0\"?><cross-domain-policy><allow-access-from domain=\"*\" to-ports=\"*\"/>"
    "</cross-domain-policy>";
constexpr size_t kMaxBuffered = 4 * 1024 * 1024;  // a peer that sends this much without a NUL is broken
constexpr size_t kMaxClients = 48;                // select() handles 64 sockets
constexpr auto kPolicyTimeout = std::chrono::seconds(5);

using Clock = std::chrono::steady_clock;

SOCKET listenOn(int port, int& error) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = WSAGetLastError();
        return s;
    }
    // Exclusive: two relays silently sharing a port would split the players.
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port));
    u_long nonBlocking = 1;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR || listen(s, SOMAXCONN) == SOCKET_ERROR ||
        ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        error = WSAGetLastError();
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

std::string stripNewlines(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

}  // namespace

struct Relay::Client {
    SOCKET s = INVALID_SOCKET;
    int id = 0;
    std::string in, out, name = "?";
    std::wstring address;
    enum class Role { None, Host, Guest } role = Role::None;
    bool policyOnly = false;    // connection on port 843
    bool policyChecked = false;  // first bytes were not a policy request
    bool closeAfterFlush = false;
    bool dead = false;
    Clock::time_point since = Clock::now();
};

Relay::Relay() = default;
Relay::~Relay() { stop(); }

bool Relay::start(int port, std::wstring& error, bool* addressInUse) {
    stop();
    if (addressInUse) *addressInUse = false;
    WSADATA wsa;
    if (int rc = WSAStartup(MAKEWORD(2, 2), &wsa)) {
        error = win32ErrorMessage(rc);
        return false;
    }
    int code = 0;
    SOCKET s = listenOn(port, code);
    if (s == INVALID_SOCKET) {
        error = win32ErrorMessage(code);
        if (addressInUse) *addressInUse = code == WSAEADDRINUSE || code == WSAEACCES;
        logger().warn(L"Relay: cannot listen on port " + std::to_wstring(port) + L": " + error);
        WSACleanup();
        return false;
    }
    listener_ = s;
    // Flash asks port 843 for the policy first; when it can't connect it falls back
    // to the game port, so failing here only costs the game a few seconds.
    int ignored = 0;
    policyListener_ = port == 843 ? INVALID_SOCKET : listenOn(843, ignored);
    port_ = port;
    nextId_ = 1;
    hostId_ = 0;
    messages_ = 0;
    stop_ = false;
    running_ = true;
    logger().info(L"Relay listening on port " + std::to_wstring(port) +
                  (policyListener_ != INVALID_SOCKET ? L" (policy server on 843)" : L" (policy served on the game port)"));
    publish();
    thread_ = std::thread([this] { loop(); });
    return true;
}

void Relay::stop() {
    if (!running_ && !thread_.joinable()) return;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    for (auto& c : clients_) closesocket(c->s);
    clients_.clear();
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    if (policyListener_ != INVALID_SOCKET) closesocket(policyListener_);
    listener_ = policyListener_ = INVALID_SOCKET;
    running_ = false;
    WSACleanup();
    logger().info(L"Relay stopped");
    publish();
}

RelaySnapshot Relay::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    RelaySnapshot s = snap_;
    s.messages = messages_;
    return s;
}

void Relay::publish() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = {};
        snap_.running = running_;
        snap_.port = port_;
        for (const auto& c : clients_) {
            if (c->dead || c->role == Client::Role::None) continue;
            RelayPeer p{c->id, widen(c->name), c->address, c->role == Client::Role::Host};
            if (p.host) snap_.peers.insert(snap_.peers.begin(), p);
            else snap_.peers.push_back(p);
        }
    }
    if (onChange_) onChange_();
}

Relay::Client* Relay::find(int id) {
    for (auto& c : clients_)
        if (c->id == id && !c->dead) return c.get();
    return nullptr;
}

void Relay::loop() {
    while (!stop_) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(listener_, &rd);
        if (policyListener_ != INVALID_SOCKET) FD_SET(policyListener_, &rd);
        for (auto& c : clients_) {
            FD_SET(c->s, &rd);
            if (!c->out.empty()) FD_SET(c->s, &wr);
        }
        // The timeout only bounds how fast stop() is noticed; data wakes select at once.
        timeval tv{0, 100 * 1000};
        int n = select(0, &rd, &wr, nullptr, &tv);
        if (n == SOCKET_ERROR) {
            logger().error(L"Relay: select failed: " + win32ErrorMessage(WSAGetLastError()));
            Sleep(50);
            continue;
        }
        if (FD_ISSET(listener_, &rd)) accept(listener_, false);
        if (policyListener_ != INVALID_SOCKET && FD_ISSET(policyListener_, &rd)) accept(policyListener_, true);
        // Index loop: handlers may add output to other clients but never add clients.
        for (size_t i = 0; i < clients_.size(); ++i) {
            Client& c = *clients_[i];
            if (c.dead) continue;
            if (FD_ISSET(c.s, &rd)) onData(c);
            if (!c.dead && FD_ISSET(c.s, &wr)) flush(c);
            if (!c.dead && c.policyOnly && Clock::now() - c.since > kPolicyTimeout) c.dead = true;
        }
        bool changed = false;
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->dead) {
                changed = changed || (*it)->role != Client::Role::None;
                closesocket((*it)->s);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        if (changed) publish();
    }
}

void Relay::accept(uintptr_t listener, bool policyOnly) {
    for (;;) {
        sockaddr_in addr{};
        int len = sizeof addr;
        SOCKET s = ::accept(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&addr), &len);
        if (s == INVALID_SOCKET) return;  // WSAEWOULDBLOCK: no more pending
        if (clients_.size() >= kMaxClients) {
            closesocket(s);
            continue;
        }
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        BOOL yes = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
        auto c = std::make_unique<Client>();
        c->s = s;
        c->id = nextId_++;
        c->policyOnly = policyOnly;
        wchar_t text[64] = L"?";
        InetNtopW(AF_INET, &addr.sin_addr, text, 64);
        c->address = text;
        clients_.push_back(std::move(c));
    }
}

void Relay::onData(Client& c) {
    char buf[64 * 1024];
    for (;;) {
        int n = recv(c.s, buf, sizeof buf, 0);
        if (n > 0) {
            c.in.append(buf, n);
            continue;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
            drop(c);
            return;
        }
        break;
    }
    if (c.policyOnly) {
        c.closeAfterFlush = true;  // before send(): it may flush everything right away
        send(c, kPolicy);
        c.in.clear();
        return;
    }
    if (!c.policyChecked && c.role == Client::Role::None) {
        size_t k = std::min(c.in.size(), kPolicyRequestLen);
        if (c.in.compare(0, k, kPolicyRequest, k) == 0) {
            if (c.in.size() < kPolicyRequestLen) return;  // could still become a policy request
            c.closeAfterFlush = true;
            send(c, kPolicy);
            c.in.clear();
            return;
        }
        c.policyChecked = true;
    }
    size_t start = 0, nul;
    while (!c.dead && (nul = c.in.find('\0', start)) != std::string::npos) {
        std::string msg = stripNewlines(c.in.substr(start, nul - start));
        start = nul + 1;
        if (!msg.empty()) onMessage(c, msg);
    }
    c.in.erase(0, start);
    if (c.in.size() > kMaxBuffered) {
        logger().warn(L"Relay: dropping " + c.address + L" (message too long)");
        drop(c);
    }
}

void Relay::onMessage(Client& c, const std::string& msg) {
    if (msg.rfind("D|", 0) == 0) {
        logger().debug(L"Relay [" + std::wstring(c.role == Client::Role::Host ? L"host" : L"guest") +
                       std::to_wstring(c.id) + L"] " + widen(msg.substr(2)));
        return;
    }
    if (c.role == Client::Role::None) {
        size_t bar = msg.find('|');
        std::string kind = msg.substr(0, bar);
        std::string name = bar == std::string::npos ? std::string() : msg.substr(bar + 1);
        c.name = name.empty() ? "Player" : name;
        if (kind == "H") {
            if (hostId_ && find(hostId_)) {
                send(c, "E|A host is already connected to this server");
                return;
            }
            c.role = Client::Role::Host;
            hostId_ = c.id;
            logger().info(L"Relay: host '" + widen(c.name) + L"' connected from " + c.address);
            send(c, "W|0");
            for (auto& g : clients_)
                if (!g->dead && g->role == Client::Role::Guest) send(c, "J|" + std::to_string(g->id) + "|" + g->name);
            publish();
        } else if (kind == "J") {
            c.role = Client::Role::Guest;
            logger().info(L"Relay: friend '" + widen(c.name) + L"' (id " + std::to_wstring(c.id) + L") connected from " +
                          c.address);
            send(c, "W|" + std::to_string(c.id));
            if (Client* h = find(hostId_)) send(*h, "J|" + std::to_string(c.id) + "|" + c.name);
            publish();
        }
        return;
    }
    ++messages_;
    if (c.role == Client::Role::Host) {
        for (auto& g : clients_)
            if (!g->dead && g->role == Client::Role::Guest) send(*g, msg);
    } else if (Client* h = find(hostId_)) {
        send(*h, "G|" + std::to_string(c.id) + "|" + msg);
    }
}

void Relay::send(Client& c, const std::string& msg) {
    if (c.dead) return;
    c.out += msg;
    c.out += '\0';
    if (c.out.size() > kMaxBuffered) {  // a peer that stopped reading entirely
        drop(c);
        return;
    }
    flush(c);
}

void Relay::flush(Client& c) {
    while (!c.out.empty()) {
        int n = ::send(c.s, c.out.data(), static_cast<int>(std::min<size_t>(c.out.size(), 1 << 20)), 0);
        if (n > 0) {
            c.out.erase(0, n);
            continue;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) return;  // select() tells us when it drains
        drop(c);
        return;
    }
    if (c.closeAfterFlush) c.dead = true;
}

void Relay::drop(Client& c) {
    if (c.dead) return;
    c.dead = true;
    if (c.role == Client::Role::Host && hostId_ == c.id) {
        hostId_ = 0;
        logger().info(L"Relay: host '" + widen(c.name) + L"' left");
        for (auto& g : clients_)
            if (!g->dead && g->role == Client::Role::Guest) send(*g, "X");
    } else if (c.role == Client::Role::Guest) {
        logger().info(L"Relay: friend '" + widen(c.name) + L"' left");
        if (Client* h = find(hostId_)) send(*h, "L|" + std::to_string(c.id));
    }
}

}  // namespace fl
