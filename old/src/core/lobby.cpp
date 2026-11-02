#include "lobby.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "fs.h"
#include "json.h"
#include "log.h"
#include "strings.h"
#include "win.h"
// win.h first (WIN32_LEAN_AND_MEAN keeps the old winsock.h out).
#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace fl::lobby {

namespace {

using Clock = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;

constexpr char kPolicyRequest[] = "<policy-file-request/>";
constexpr char kPolicy[] =
    "<?xml version=\"1.0\"?><cross-domain-policy><allow-access-from domain=\"*\" to-ports=\"*\"/>"
    "</cross-domain-policy>";
constexpr size_t kMaxBuffered = 1024 * 1024;  // a peer that sends this much without a NUL is broken
constexpr size_t kMaxLinks = 16;
constexpr int kConnectTimeoutMs = 4000;
constexpr int kReconnectEveryMs = 2000;
constexpr int kPasswordTries = 5;  // per minute and address, Direct mode (the server counts its own)

int64_t sinceMs(Clock::time_point t) { return std::chrono::duration_cast<ms>(Clock::now() - t).count(); }

int64_t wallMs() {
    return std::chrono::duration_cast<ms>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string randomHex(size_t bytes) {
    std::vector<uint8_t> b(bytes);
    BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return narrow(hexEncode(b.data(), b.size()));
}

int randomSeed() {
    uint32_t v = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof v, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return static_cast<int>(v & 0x7fffffff);
}

// Names and labels the server's clean() keeps: no separators or control characters.
std::wstring cleanName(const std::wstring& s, size_t limit = 24) {
    std::wstring out;
    for (wchar_t ch : s)
        if (ch >= 32 && ch != L'|' && ch != L';' && ch != L',' && ch != 127) out += ch;
    out = trim(out);
    if (out.size() > limit) out.resize(limit);
    return out;
}

void setNonBlocking(SOCKET s) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

void setNoDelay(SOCKET s) {
    BOOL yes = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
}

SOCKET listenOn(bool loopbackOnly, int port, int& actualPort, int& error) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = WSAGetLastError();
        return s;
    }
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR || listen(s, SOMAXCONN) == SOCKET_ERROR) {
        error = WSAGetLastError();
        closesocket(s);
        return INVALID_SOCKET;
    }
    int len = sizeof addr;
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    actualPort = ntohs(addr.sin_port);
    setNonBlocking(s);
    return s;
}

// Blocking connect with a timeout; the socket comes back non-blocking.
SOCKET connectTo(const std::wstring& hostPort, std::wstring& error) {
    size_t colon = hostPort.rfind(L':');
    if (colon == std::wstring::npos) {
        error = L"The address needs a port: " + hostPort;
        return INVALID_SOCKET;
    }
    std::string host = narrow(trim(hostPort.substr(0, colon))), port = narrow(trim(hostPort.substr(colon + 1)));
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        error = L"Cannot find " + hostPort;
        return INVALID_SOCKET;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    setNonBlocking(s);
    connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000};
    if (select(0, nullptr, &w, &e, &tv) != 1 || !FD_ISSET(s, &w)) {
        error = L"Cannot reach " + hostPort;
        closesocket(s);
        return INVALID_SOCKET;
    }
    setNoDelay(s);
    return s;
}

std::string str(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return v && v->isString() ? v->str() : std::string();
}
int num(const json::Value& o, const char* key, int def = 0) {
    const json::Value* v = o.find(key);
    return v && v->isNumber() ? static_cast<int>(v->number()) : def;
}
bool flag(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return v && v->boolean();
}

std::string line(const json::Value& v) { return json::serialize(v, false); }

json::Value obj(const char* field, const char* kind) {
    json::Value o = json::Value::object();
    o.set(field, kind);
    return o;
}

// One TCP connection carrying NUL-terminated messages.
struct Conn {
    SOCKET s = INVALID_SOCKET;
    std::string in, out;
    bool policyChecked = false;  // first bytes looked at (game and direct listeners answer the Flash policy request)
    bool closeAfterFlush = false;
    bool dead = false;
    int link = 0;                // Direct host: this guest's link id
    std::string ip;
    Clock::time_point since = Clock::now(), lastRecv = Clock::now();

    ~Conn() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
    void send(const std::string& msg) {
        if (dead || closeAfterFlush) return;
        out += msg;
        out.push_back('\0');
    }
    void flush() {
        while (!out.empty() && !dead) {
            int n = ::send(s, out.data(), static_cast<int>(std::min<size_t>(out.size(), 1 << 20)), 0);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) dead = true;
                return;
            }
            out.erase(0, static_cast<size_t>(n));
        }
        if (out.empty() && closeAfterFlush) dead = true;
    }
    // Reads what's there; false when the peer closed or the buffer overflowed.
    bool read() {
        char buf[16384];
        for (;;) {
            int n = recv(s, buf, sizeof buf, 0);
            if (n == 0) return false;
            if (n == SOCKET_ERROR) return WSAGetLastError() == WSAEWOULDBLOCK;
            in.append(buf, static_cast<size_t>(n));
            lastRecv = Clock::now();
            if (in.size() > kMaxBuffered && in.find('\0') == std::string::npos) return false;
        }
    }
    // Answers a Flash policy request if that's what came first. True when it did.
    bool policy() {
        if (policyChecked) return false;
        size_t need = sizeof kPolicyRequest - 1;
        if (in.size() < need && std::string_view(kPolicyRequest).substr(0, in.size()) == in) return false;
        policyChecked = true;
        if (in.compare(0, need, kPolicyRequest) != 0) return false;
        send(kPolicy);
        closeAfterFlush = true;
        in.clear();
        return true;
    }
    bool next(std::string& msg) {
        size_t nul = in.find('\0');
        if (nul == std::string::npos) return false;
        msg = in.substr(0, nul);
        in.erase(0, nul + 1);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.pop_back();
        while (!msg.empty() && (msg.front() == '\r' || msg.front() == '\n')) msg.erase(0, 1);
        return true;
    }
};

std::wstring addressOf(SOCKET s) {
    sockaddr_in a{};
    int len = sizeof a;
    if (getpeername(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) return L"";
    wchar_t buf[64] = {};
    InetNtopW(AF_INET, &a.sin_addr, buf, 64);
    return buf;
}

}  // namespace

std::string versionTag(const std::string& modVersion, const std::string& swfSha256) {
    std::string v = modVersion.empty() ? "1" : modVersion;
    if (swfSha256.size() >= 16) v += "#" + swfSha256.substr(0, 16);
    return v;
}

std::string versionName(const std::string& tag) { return tag.substr(0, tag.find('#')); }

std::string hashPassword(const std::wstring& password) {
    if (password.empty()) return "";
    std::string utf8 = narrow(password);
    fs::Sha256 h;
    h.update(utf8.data(), utf8.size());
    return h.hex();
}

// ---------------------------------------------------------------------------

struct Session::Impl {
    Options opt;
    std::atomic<bool> stop{false};
    std::function<void()>* onChange = nullptr;

    mutable std::mutex mu;
    Snapshot pub;  // what snapshot() returns
    std::vector<std::function<void()>> cmds;

    Snapshot st;  // the session thread's working copy
    bool dirty = true;

    SOCKET bridgeListener = INVALID_SOCKET, directListener = INVALID_SOCKET;
    std::vector<std::unique_ptr<Conn>> gameConns;  // waiting for hello, or answering a policy request
    std::unique_ptr<Conn> game;
    std::unique_ptr<Conn> up;                      // Online (both roles) and Direct guest
    std::vector<std::unique_ptr<Conn>> links;      // Direct host: one per guest launcher
    bool upReady = false;                          // handshake done
    bool tryV1 = false;                            // the server is older: RELAY|1
    int myLink = -1;                               // Online guest: our id in the server's star room
    int nextLink = 1;

    struct Peer {
        int link = 0, slot = 0;
        std::string token, ip;
        std::wstring name;
        bool welcomed = false, ready = false, connected = true, loaded = false;
        int ping = -1;
        Clock::time_point lastSeen = Clock::now(), graceUntil{};
    };
    std::vector<Peer> peers;                       // host: guests by link
    std::map<int, std::wstring> pendingNames;      // host, Online: J| seen, hello not yet
    std::set<std::string> banned;                  // host: tokens
    std::map<std::string, std::deque<Clock::time_point>> pwFails;
    std::set<int> ignored;                         // host: kicked links whose messages are dropped

    bool myLoaded = false, goSent = false;
    Clock::time_point loadDeadline{}, lastBeat = Clock::now(), lastFromHost = Clock::now(), lastPing{};
    Clock::time_point reconnectUntil{}, nextReconnect{};
    int64_t pingSentAt = 0;
    int myRtt = -1, serverPing = -1;
    std::vector<json::Value> pendingGameMsgs;      // for the game before it connects (go, events)

    // ---- publishing ----
    void changed() { dirty = true; }
    void publish() {
        if (!dirty) return;
        dirty = false;
        st.players = playerList();
        {
            std::lock_guard<std::mutex> lock(mu);
            st.revision = pub.revision + 1;
            pub = st;
        }
        if (onChange && *onChange) (*onChange)();
    }
    void note(const std::wstring& text) {
        st.chat.push_back({-1, L"", text});
        while (st.chat.size() > 100) st.chat.pop_front();
        changed();
    }

    std::vector<Player> playerList() const {
        if (!st.host) {
            std::vector<Player> list = st.players;
            for (auto& p : list) p.you = p.slot == st.mySlot;
            return list;
        }
        std::vector<Player> list;
        Player me;
        me.slot = 0;
        me.name = opt.playerName;
        me.host = me.ready = me.you = true;
        me.loaded = myLoaded;
        list.push_back(me);
        for (const auto& p : peers) {
            if (!p.welcomed) continue;
            Player x;
            x.slot = p.slot;
            x.name = p.name;
            x.ready = p.ready;
            x.connected = p.connected;
            x.loaded = p.loaded;
            x.pingMs = p.ping;
            list.push_back(x);
        }
        std::sort(list.begin(), list.end(), [](const Player& a, const Player& b) { return a.slot < b.slot; });
        return list;
    }

    // ---- sending ----
    // Host -> every guest launcher.
    void toAll(const json::Value& v) {
        std::string m = line(v);
        if (opt.transport == Transport::Direct) {
            for (auto& c : links)
                if (!ignored.count(c->link)) c->send(m);
        } else if (up && upReady) {
            up->send(m);
        }
    }
    // Host -> one guest launcher: Online rooms only broadcast, so the message names its link.
    void toLink(int link, json::Value v) {
        v.set("to", link);
        std::string m = line(v);
        if (opt.transport == Transport::Direct) {
            for (auto& c : links)
                if (c->link == link) c->send(m);
        } else if (up && upReady) {
            up->send(m);
        }
    }
    // Guest -> host.
    void toHost(const json::Value& v) {
        if (up && upReady) up->send(line(v));
    }
    void toGame(const json::Value& v) {
        if (game) game->send(line(v));
        else pendingGameMsgs.push_back(v);
    }
    void meta(const std::string& state) {
        if (opt.transport != Transport::Online || !up || !upReady || !st.serverV2) return;
        std::string m = "@META|state=" + state;
        if (!opt.region.empty()) m += "|region=" + narrow(opt.region);
        if (serverPing >= 0) m += "|ping=" + std::to_string(serverPing);
        up->send(m);
    }

    // ---- lobby state (host) ----
    json::Value lobbyMessage() const {
        json::Value o = obj("k", "lobby");
        o.set("phase", st.phase == Phase::InGame ? "ingame" : st.phase == Phase::Starting ? "starting" : "lobby");
        o.set("max", st.maxPlayers);
        o.set("name", narrow(st.roomName));
        o.set("locked", st.locked);
        json::Value list = json::Value::array();
        for (const auto& p : playerList()) {
            json::Value x = json::Value::object();
            x.set("slot", p.slot);
            x.set("name", narrow(p.name));
            x.set("host", p.host);
            x.set("ready", p.ready);
            x.set("connected", p.connected);
            x.set("loaded", p.loaded);
            x.set("ping", p.pingMs);
            list.push(x);
        }
        o.set("players", list);
        return o;
    }
    void broadcastLobby() {
        if (st.host) toAll(lobbyMessage());
        changed();
    }
    Peer* peerByLink(int link) {
        for (auto& p : peers)
            if (p.link == link) return &p;
        return nullptr;
    }
    Peer* peerBySlot(int slot) {
        for (auto& p : peers)
            if (p.welcomed && p.slot == slot) return &p;
        return nullptr;
    }
    int freeSlot() const {
        for (int s = 1; s < st.maxPlayers; ++s) {
            bool used = false;
            for (const auto& p : peers)
                if (p.welcomed && p.slot == s) used = true;
            if (!used) return s;
        }
        return -1;
    }
    json::Value playersArray() const {
        json::Value list = json::Value::array();
        for (const auto& p : playerList()) {
            if (!p.connected) continue;
            json::Value x = json::Value::object();
            x.set("slot", p.slot);
            x.set("name", narrow(p.name));
            list.push(x);
        }
        return list;
    }
    void gameEvent(const char* kind, int slot, const std::wstring& name = L"") {
        json::Value e = obj("t", kind);
        e.set("slot", slot);
        if (!name.empty()) e.set("name", narrow(name));
        if (game || st.phase == Phase::Starting || st.phase == Phase::InGame) toGame(e);
    }
    void reject(int link, Reason why, const std::string& code) {
        json::Value r = obj("k", "reject");
        r.set("code", code);
        if (why == Reason::Version) r.set("hostVersion", opt.version);
        toLink(link, r);
        ignored.insert(link);
        if (opt.transport == Transport::Direct)
            for (auto& c : links)
                if (c->link == link) c->closeAfterFlush = true;
        peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const Peer& p) { return p.link == link && !p.welcomed; }),
                    peers.end());
    }
    bool passwordOk(const std::string& ip, const std::string& given, int link) {
        // Online on a current server the room's password was checked by the server before the guest got in.
        if (opt.password.empty() || (opt.transport == Transport::Online && st.serverV2)) return true;
        auto& q = pwFails[ip];
        while (!q.empty() && sinceMs(q.front()) > 60000) q.pop_front();
        if (q.size() >= static_cast<size_t>(kPasswordTries)) {
            reject(link, Reason::TooManyTries, "tries");
            return false;
        }
        if (given == hashPassword(opt.password)) return true;
        q.push_back(Clock::now());
        reject(link, Reason::Password, "password");
        return false;
    }

    void onHello(int link, const std::string& ip, const json::Value& m) {
        std::string token = str(m, "token");
        std::wstring name = cleanName(widen(str(m, "name")));
        if (name.empty()) name = L"Player";
        if (!token.empty() && banned.count(token)) return reject(link, Reason::Banned, "banned");
        if (num(m, "proto") != kProtocol || str(m, "version") != opt.version) return reject(link, Reason::Version, "version");
        if (!passwordOk(ip, str(m, "pw"), link)) return;
        // A player coming back within the grace window keeps their slot.
        if (!token.empty())
            for (auto& p : peers)
                if (p.welcomed && p.token == token) {
                    int old = p.link;
                    p.link = link;
                    p.ip = ip;
                    p.lastSeen = Clock::now();
                    bool wasGone = !p.connected;
                    p.connected = true;
                    if (old != link && opt.transport == Transport::Direct)
                        for (auto& c : links)
                            if (c->link == old) c->closeAfterFlush = true;
                    welcome(p);
                    if (wasGone) {
                        gameEvent("back", p.slot, p.name);
                        json::Value b = obj("k", "back");
                        b.set("slot", p.slot);
                        toAll(b);
                        note(p.name + L" is back");
                    }
                    broadcastLobby();
                    return;
                }
        if (st.phase != Phase::Lobby) return reject(link, Reason::InGame, "ingame");
        int slot = freeSlot();
        if (slot < 0) return reject(link, Reason::Full, "full");
        Peer* p = peerByLink(link);
        if (!p) {
            peers.push_back({});
            p = &peers.back();
        }
        p->link = link;
        p->slot = slot;
        p->ip = ip;
        p->name = name;
        p->token = token.size() >= 16 ? token : randomHex(16);
        p->welcomed = true;
        p->lastSeen = Clock::now();
        welcome(*p);
        note(name + L" joined");
        broadcastLobby();
    }
    void welcome(const Peer& p) {
        json::Value w = obj("k", "welcome");
        w.set("slot", p.slot);
        w.set("token", p.token);
        w.set("link", p.link);
        toLink(p.link, w);
        toLink(p.link, lobbyMessage());
    }
    void peerGone(Peer& p, const std::wstring& why) {
        int slot = p.slot;
        std::wstring name = p.name;
        bool welcomed = p.welcomed;
        int link = p.link;
        peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const Peer& x) { return x.link == link; }), peers.end());
        if (!welcomed) return;
        gameEvent("leave", slot, name);
        json::Value g = obj("k", "gone");
        g.set("slot", slot);
        toAll(g);
        note(why);
        broadcastLobby();
        checkBarrier();
    }
    void linkLeft(int link) {
        pendingNames.erase(link);
        Peer* p = peerByLink(link);
        if (!p) return;
        if (!p->welcomed) {
            peerGone(*p, L"");
            return;
        }
        if (!p->connected) return;
        p->connected = false;
        p->graceUntil = Clock::now() + ms(kGraceMs);
        gameEvent("drop", p->slot, p->name);
        json::Value d = obj("k", "drop");
        d.set("slot", p->slot);
        toAll(d);
        note(p->name + L" lost connection; keeping their slot for 30 seconds");
        broadcastLobby();
        checkBarrier();
    }

    void onFromGuest(int link, const std::string& ip, const std::string& text) {
        if (ignored.count(link)) return;
        std::string err;
        json::Value m = json::parse(text, &err);
        if (!m.isObject()) return;
        std::string k = str(m, "k");
        if (k == "hello") return onHello(link, ip, m);
        Peer* p = peerByLink(link);
        if (!p || !p->welcomed) return;
        p->lastSeen = Clock::now();
        if (k == "g") {
            const json::Value* d = m.find("d");
            route(p->slot, num(m, "dst", -1), d ? *d : json::Value());
        } else if (k == "ping") {
            json::Value r = obj("k", "pong");
            r.set("t", m.find("t") ? *m.find("t") : json::Value(0));
            toLink(link, r);
            int rtt = num(m, "rtt", -1);
            if (rtt != p->ping) {
                p->ping = rtt;
                changed();
            }
        } else if (k == "ready") {
            if (st.phase == Phase::Lobby) {
                p->ready = flag(m, "v");
                broadcastLobby();
            }
        } else if (k == "chat") {
            relayChat(p->slot, p->name, widen(str(m, "text")));
        } else if (k == "loaded") {
            if (st.phase == Phase::Starting && !p->loaded) {
                p->loaded = true;
                broadcastLobby();
                checkBarrier();
            }
        } else if (k == "gameOver") {
            if (p->loaded) {
                p->loaded = false;
                gameEvent("leave", p->slot, p->name);
                json::Value g = obj("k", "gone");
                g.set("slot", p->slot);
                toAll(g);
                note(p->name + L"'s game closed");
                broadcastLobby();
            }
        } else if (k == "bye") {
            peerGone(*p, p->name + L" left");
        }
    }
    void relayChat(int slot, const std::wstring& name, std::wstring text) {
        text = trim(text);
        for (auto& ch : text)
            if (ch < 32) ch = L' ';
        if (text.empty()) return;
        if (text.size() > kMaxChat) text.resize(kMaxChat);
        json::Value c = obj("k", "chat");
        c.set("slot", slot);
        c.set("name", narrow(name));
        c.set("text", narrow(text));
        toAll(c);
        st.chat.push_back({slot, name, text});
        while (st.chat.size() > 100) st.chat.pop_front();
        changed();
    }
    // Game traffic, host side: `from` slot to `dst` (-1 everyone else).
    void route(int from, int dst, const json::Value& d) {
        if (st.phase != Phase::InGame && st.phase != Phase::Starting) return;
        if (from != 0 && (dst == -1 || dst == 0)) {
            json::Value m = obj("t", "msg");
            m.set("from", from);
            m.set("d", d);
            toGame(m);
        }
        if (dst == 0) return;
        json::Value g = obj("k", "g");
        g.set("from", from);
        g.set("dst", dst);
        g.set("d", d);
        if (dst > 0 && opt.transport == Transport::Direct) {
            if (Peer* p = peerBySlot(dst)) toLink(p->link, g);
        } else {
            toAll(g);
        }
    }
    void checkBarrier() {
        if (!st.host || st.phase != Phase::Starting) return;
        bool all = myLoaded;
        for (const auto& p : peers)
            if (p.welcomed && p.connected && !p.loaded) all = false;
        bool late = Clock::now() > loadDeadline;
        if (!all && !late) return;
        if (!all) note(L"Starting without the players whose game didn't load in time");
        st.phase = Phase::InGame;
        toAll(obj("k", "go"));
        goSent = true;
        toGame(obj("t", "go"));
        broadcastLobby();
    }
    void doStart() {
        if (!st.host || st.phase != Phase::Lobby) return;
        for (const auto& p : peers)
            if (p.welcomed && p.connected && !p.ready) {
                note(L"Everyone must be ready first");
                return;
            }
        st.seed = randomSeed();
        st.phase = Phase::Starting;
        st.startNow = true;
        myLoaded = false;
        goSent = false;
        pendingGameMsgs.clear();
        for (auto& p : peers) p.loaded = false;
        loadDeadline = Clock::now() + ms(kLoadTimeoutMs);
        json::Value s = obj("k", "start");
        s.set("seed", st.seed);
        s.set("players", playersArray());
        toAll(s);
        meta("ingame");
        note(L"Starting the game");
        broadcastLobby();
    }
    void backToLobby() {
        st.phase = Phase::Lobby;
        st.startNow = false;
        myLoaded = false;
        goSent = false;
        for (auto& p : peers) {
            p.ready = false;
            p.loaded = false;
        }
        toAll(obj("k", "end"));
        meta("lobby");
        note(L"Back in the lobby");
        broadcastLobby();
    }

    // ---- guest side ----
    void onFromHost(const std::string& text) {
        lastFromHost = Clock::now();
        if (text == "X") return close(Reason::HostLeft, L"");
        if (text.rfind("E|", 0) == 0) return close(Reason::Server, widen(text.substr(2)));
        std::string err;
        json::Value m = json::parse(text, &err);
        if (!m.isObject()) return;
        if (const json::Value* to = m.find("to"); to && to->isNumber() && myLink >= 0 &&
                                                  static_cast<int>(to->number()) != myLink)
            return;
        std::string k = str(m, "k");
        if (k == "welcome") {
            st.mySlot = num(m, "slot");
            st.token = widen(str(m, "token"));
            if (myLink < 0) myLink = num(m, "link", -1);
            if (st.reconnecting) note(L"Reconnected");
            st.reconnecting = false;
            changed();
        } else if (k == "reject") {
            std::string code = str(m, "code");
            Reason r = code == "version"  ? Reason::Version
                       : code == "full"   ? Reason::Full
                       : code == "ingame" ? Reason::InGame
                       : code == "password" ? Reason::Password
                       : code == "tries"  ? Reason::TooManyTries
                       : code == "banned" ? Reason::Banned
                       : code == "kicked" ? Reason::Kicked
                                          : Reason::Server;
            st.hostVersion = widen(str(m, "hostVersion"));
            close(r, L"");
        } else if (k == "lobby") {
            std::string ph = str(m, "phase");
            Phase was = st.phase;
            if (ph == "lobby") st.phase = Phase::Lobby;
            else if (ph == "starting" && was != Phase::InGame) st.phase = Phase::Starting;
            else if (ph == "ingame") st.phase = Phase::InGame;
            if (was == Phase::Connecting && st.phase != Phase::Lobby) st.phase = Phase::Lobby;  // joined mid-round: wait
            st.maxPlayers = num(m, "max", st.maxPlayers);
            st.roomName = widen(str(m, "name"));
            st.locked = flag(m, "locked");
            std::vector<Player> list;
            if (const json::Value* ps = m.find("players"); ps && ps->isArray())
                for (size_t i = 0; i < ps->size(); ++i) {
                    const json::Value& x = ps->at(i);
                    Player p;
                    p.slot = num(x, "slot");
                    p.name = widen(str(x, "name"));
                    p.host = flag(x, "host");
                    p.ready = flag(x, "ready");
                    p.connected = flag(x, "connected");
                    p.loaded = flag(x, "loaded");
                    p.pingMs = num(x, "ping", -1);
                    if (p.slot == st.mySlot && myRtt >= 0) p.pingMs = myRtt;
                    list.push_back(p);
                }
            st.players = list;
            changed();
        } else if (k == "chat") {
            st.chat.push_back({num(m, "slot"), widen(str(m, "name")), widen(str(m, "text"))});
            while (st.chat.size() > 100) st.chat.pop_front();
            changed();
        } else if (k == "pong") {
            if (pingSentAt) myRtt = static_cast<int>(wallMs() - pingSentAt);
            pingSentAt = 0;
        } else if (k == "start") {
            st.seed = num(m, "seed");
            st.phase = Phase::Starting;
            st.startNow = true;
            myLoaded = false;
            goSent = false;
            pendingGameMsgs.clear();
            startPlayers = m.find("players") ? *m.find("players") : json::Value::array();
            note(L"The host started the game");
        } else if (k == "go") {
            st.phase = Phase::InGame;
            goSent = true;
            toGame(obj("t", "go"));
            changed();
        } else if (k == "g") {
            int dst = num(m, "dst", -1), from = num(m, "from");
            if ((dst == -1 || dst == st.mySlot) && from != st.mySlot) {
                json::Value g = obj("t", "msg");
                g.set("from", from);
                g.set("d", m.find("d") ? *m.find("d") : json::Value());
                toGame(g);
            }
        } else if (k == "drop" || k == "back" || k == "gone") {
            int slot = num(m, "slot");
            if (slot != st.mySlot) gameEvent(k == "gone" ? "leave" : k.c_str(), slot);
        } else if (k == "end") {
            st.phase = Phase::Lobby;
            st.startNow = false;
            goSent = false;
            toGame(obj("t", "end"));
            note(L"The host ended the round");
            changed();
        } else if (k == "closed") {
            close(Reason::HostLeft, L"");
        }
    }
    json::Value startPlayers = json::Value::array();

    void sendHello() {
        json::Value h = obj("k", "hello");
        h.set("proto", kProtocol);
        h.set("name", narrow(opt.playerName));
        h.set("token", narrow(st.token));
        h.set("version", opt.version);
        // Only where the host itself checks it: the server checks it otherwise and the host never sees it.
        if (!opt.password.empty() && (opt.transport == Transport::Direct || !st.serverV2))
            h.set("pw", hashPassword(opt.password));
        toHost(h);
    }

    // ---- connections ----
    std::string handshake() const {
        std::string name = narrow(cleanName(opt.host ? (opt.roomName.empty() ? opt.playerName : opt.roomName) : opt.playerName));
        std::string pw = hashPassword(opt.password);
        std::string code = narrow(opt.code);
        if (opt.host)
            return tryV1 ? "RELAY|1|HOST|" + opt.gameId + "|" + opt.version + "|" + (opt.isPublic ? "1" : "0") + "|" + name
                         : "RELAY|2|HOST|" + opt.gameId + "|" + opt.version + "|" + (opt.isPublic ? "1" : "0") + "|" +
                               std::to_string(opt.maxPlayers) + "|" + pw + "||" + name;
        return tryV1 ? "RELAY|1|JOIN|" + opt.gameId + "|" + opt.version + "|" + code + "|" + name
                     : "RELAY|2|JOIN|" + opt.gameId + "|" + opt.version + "|" + code + "|" + pw + "|" + name;
    }
    bool connectUp(std::wstring& error) {
        up.reset();
        upReady = false;
        SOCKET s = connectTo(opt.server, error);
        if (s == INVALID_SOCKET) return false;
        up = std::make_unique<Conn>();
        up->s = s;
        up->policyChecked = true;
        lastFromHost = Clock::now();
        if (opt.transport == Transport::Online) {
            up->send(handshake());
        } else {
            upReady = true;  // Direct: the host's listener talks lobby JSON at once
            sendHello();
        }
        return true;
    }
    // Online: replies to the handshake, then the star protocol.
    void onUpLine(const std::string& text) {
        if (opt.transport == Transport::Online && !upReady) {
            if (text.rfind("E|", 0) == 0) {
                std::string why = text.substr(2);
                if (!tryV1 && (why.find("RELAY|2") != std::string::npos || why.find("handshake version") != std::string::npos ||
                               why.find("Bad handshake") != std::string::npos)) {
                    // An older server: retry the plain handshake once (no password, no room state).
                    tryV1 = true;
                    st.serverV2 = false;
                    up->dead = true;
                    nextReconnect = Clock::now();
                    retryNow = true;
                    return;
                }
                return refused(why);
            }
            if (opt.host) {
                if (text.rfind("ROOM|", 0) == 0) {
                    size_t bar = text.find('|', 5);
                    st.code = widen(text.substr(5, bar == std::string::npos ? std::string::npos : bar - 5));
                    if (bar != std::string::npos) st.maxPlayers = std::min(st.maxPlayers, atoi(text.c_str() + bar + 1));
                    changed();
                } else if (text == "W|0") {
                    upReady = true;
                    st.phase = Phase::Lobby;
                    meta("lobby");
                    note(L"Room " + st.code + L" is open");
                    broadcastLobby();
                }
            } else if (text.rfind("W|", 0) == 0) {
                upReady = true;
                myLink = atoi(text.c_str() + 2);
                sendHello();
            }
            return;
        }
        if (!st.host) return onFromHost(text);
        // Host, Online: the star protocol around guests' messages.
        if (text.rfind("G|", 0) == 0) {
            size_t bar = text.find('|', 2);
            if (bar == std::string::npos) return;
            onFromGuest(atoi(text.c_str() + 2), "", text.substr(bar + 1));
        } else if (text.rfind("J|", 0) == 0) {
            size_t bar = text.find('|', 2);
            pendingNames[atoi(text.c_str() + 2)] = bar == std::string::npos ? L"" : widen(text.substr(bar + 1));
        } else if (text.rfind("L|", 0) == 0) {
            linkLeft(atoi(text.c_str() + 2));
        } else if (text.rfind("E|", 0) == 0) {
            close(Reason::Server, widen(text.substr(2)));
        }
    }
    bool retryNow = false;
    void refused(const std::string& why) {
        std::wstring w = widen(why);
        Reason r = Reason::Server;
        if (why.find("No room") != std::string::npos || why.find("another game") != std::string::npos) r = Reason::NoRoom;
        else if (why.find("full") != std::string::npos) r = Reason::Full;
        else if (why.find("Too many wrong") != std::string::npos) r = Reason::TooManyTries;
        else if (why.find("password") != std::string::npos || why.find("Wrong password") != std::string::npos) r = Reason::Password;
        else if (why.find("Version mismatch") != std::string::npos || why.find("doesn't support version") != std::string::npos) {
            r = Reason::Version;
            size_t a = why.find("runs '");
            if (a != std::string::npos) {
                size_t b = why.find('\'', a + 6);
                st.hostVersion = widen(why.substr(a + 6, b == std::string::npos ? std::string::npos : b - a - 6));
            }
        }
        // Coming back to a room that closed meanwhile: the connection was lost, the code isn't wrong.
        if (st.reconnecting && (r == Reason::NoRoom || r == Reason::Server)) r = Reason::Lost;
        close(r, w);
    }
    void close(Reason r, const std::wstring& detail) {
        if (st.phase == Phase::Closed) return;
        if (st.host) {
            json::Value c = obj("k", "closed");
            toAll(c);
            for (auto& l : links) l->closeAfterFlush = true;
        } else if (up && upReady && r == Reason::None) {
            toHost(obj("k", "bye"));
        }
        if (up) {
            up->closeAfterFlush = true;
            up->flush();
        }
        st.phase = Phase::Closed;
        st.reason = r;
        st.detail = detail;
        st.reconnecting = false;
        st.startNow = false;
        if (game) toGame(obj("t", "closed"));
        changed();
    }
    void linkLost() {
        // A guest whose link dropped without the host saying goodbye tries again, keeping its token.
        if (st.phase == Phase::Closed) return;
        up.reset();
        upReady = false;
        if (st.host) return close(Reason::Unreachable, L"Lost the connection to the server");
        if (st.mySlot < 0 || st.token.empty()) return close(Reason::Unreachable, L"");
        if (!st.reconnecting) {
            st.reconnecting = true;
            reconnectUntil = Clock::now() + ms(kGraceMs);
            nextReconnect = Clock::now();
            note(L"Connection lost; reconnecting");
        }
    }

    // ---- the game on the bridge ----
    void onFromGame(const std::string& text) {
        std::string err;
        json::Value m = json::parse(text, &err);
        if (!m.isObject()) return;
        std::string t = str(m, "t");
        if (t == "send") {
            const json::Value* d = m.find("d");
            int dst = num(m, "to", -1);
            if (st.host) {
                route(0, dst, d ? *d : json::Value());
            } else if (st.phase == Phase::Starting || st.phase == Phase::InGame) {
                json::Value g = obj("k", "g");
                g.set("dst", dst);
                g.set("d", d ? *d : json::Value());
                toHost(g);
            }
        } else if (t == "loaded") {
            if (!myLoaded && (st.phase == Phase::Starting || st.phase == Phase::InGame)) {
                myLoaded = true;
                if (st.host) checkBarrier();
                else toHost(obj("k", "loaded"));
                changed();
            }
        } else if (t == "chat") {
            doChat(widen(str(m, "text")));
        } else if (t == "log") {
            logger().info(L"Game: " + widen(str(m, "text")));
        }
    }
    void gameHello(std::unique_ptr<Conn> c, const json::Value& m) {
        if (widen(str(m, "token")) != st.token) {
            c->closeAfterFlush = true;
            return;
        }
        game = std::move(c);
        st.gameConnected = true;
        json::Value w = obj("t", "welcome");
        w.set("slot", st.mySlot);
        w.set("role", st.host ? "host" : "client");
        w.set("room", narrow(st.code));
        w.set("seed", st.seed);
        w.set("name", narrow(opt.playerName));
        w.set("players", st.host ? playersArray() : startPlayers);
        game->send(line(w));
        for (auto& v : pendingGameMsgs) game->send(line(v));
        pendingGameMsgs.clear();
        changed();
    }

    void doChat(const std::wstring& text) {
        if (st.host) relayChat(0, opt.playerName, text);
        else {
            json::Value c = obj("k", "chat");
            c.set("text", narrow(text.substr(0, kMaxChat)));
            toHost(c);
        }
    }

    // ---- the loop ----
    void tick() {
        auto now = Clock::now();
        if (st.host) {
            for (size_t i = 0; i < peers.size(); ++i) {
                Peer& p = peers[i];
                if (p.welcomed && p.connected && sinceMs(p.lastSeen) > kTimeoutMs) {
                    if (opt.transport == Transport::Direct)
                        for (auto& c : links)
                            if (c->link == p.link) c->dead = true;
                    linkLeft(p.link);
                }
                if (p.welcomed && !p.connected && now > p.graceUntil) {
                    peerGone(p, p.name + L" left (connection lost)");
                    i = static_cast<size_t>(-1);  // the list changed
                }
            }
            if (st.phase == Phase::Starting) checkBarrier();
            if (sinceMs(lastBeat) > kHeartbeatMs && st.phase != Phase::Connecting && st.phase != Phase::Closed) {
                lastBeat = now;
                broadcastLobby();
                meta(st.phase == Phase::Lobby ? "lobby" : "ingame");
            }
        } else if (st.phase != Phase::Closed) {
            if (up && upReady && sinceMs(lastFromHost) > kTimeoutMs) {
                logger().warn(L"Lobby: no word from the host for 15 s");
                linkLost();
            }
            if (up && upReady && sinceMs(lastPing) > kHeartbeatMs) {
                lastPing = now;
                pingSentAt = wallMs();
                json::Value p = obj("k", "ping");
                p.set("t", static_cast<double>(pingSentAt));
                p.set("rtt", myRtt);
                toHost(p);
            }
            if (st.reconnecting && !up) {
                if (now > reconnectUntil) return close(Reason::Lost, L"");
                if (now >= nextReconnect) {
                    nextReconnect = now + ms(kReconnectEveryMs);
                    std::wstring err;
                    if (connectUp(err)) logger().info(L"Lobby: reconnecting");
                }
            }
        }
    }

    void loop() {
        while (!stop) {
            std::vector<std::function<void()>> todo;
            {
                std::lock_guard<std::mutex> lock(mu);
                todo.swap(cmds);
            }
            for (auto& f : todo) f();
            if (retryNow) {
                retryNow = false;
                up.reset();
                upReady = false;
                std::wstring err;
                if (!connectUp(err)) close(Reason::Unreachable, err);
            }
            tick();

            fd_set rd, wr;
            FD_ZERO(&rd);
            FD_ZERO(&wr);
            auto add = [&](Conn* c) {
                if (!c || c->dead) return;
                FD_SET(c->s, &rd);
                if (!c->out.empty()) FD_SET(c->s, &wr);
            };
            if (bridgeListener != INVALID_SOCKET) FD_SET(bridgeListener, &rd);
            if (directListener != INVALID_SOCKET) FD_SET(directListener, &rd);
            for (auto& c : gameConns) add(c.get());
            add(game.get());
            add(up.get());
            for (auto& c : links) add(c.get());
            timeval tv{0, 50 * 1000};
            if (rd.fd_count == 0) {
                Sleep(50);
            } else if (select(0, &rd, &wr, nullptr, &tv) == SOCKET_ERROR) {
                Sleep(20);
                continue;
            }

            if (bridgeListener != INVALID_SOCKET && FD_ISSET(bridgeListener, &rd)) acceptOn(bridgeListener, true);
            if (directListener != INVALID_SOCKET && FD_ISSET(directListener, &rd)) acceptOn(directListener, false);

            for (auto& c : gameConns) {
                if (c->dead || !FD_ISSET(c->s, &rd)) continue;
                if (!c->read()) c->dead = true;
                if (c->policy()) continue;
                std::string msg;
                if (c->next(msg)) {
                    std::string err;
                    json::Value m = json::parse(msg, &err);
                    if (m.isObject() && str(m, "t") == "hello") gameHello(std::move(c), m);
                    else c->dead = true;
                }
                if (c && sinceMs(c->since) > 10000) c->dead = true;
            }
            gameConns.erase(std::remove_if(gameConns.begin(), gameConns.end(),
                                           [](const std::unique_ptr<Conn>& c) { return !c || (c->dead && c->out.empty()); }),
                            gameConns.end());

            if (game && FD_ISSET(game->s, &rd)) {
                if (!game->read()) game->dead = true;
                std::string msg;
                while (game && game->next(msg)) onFromGame(msg);
            }
            if (up && FD_ISSET(up->s, &rd)) {
                bool alive = up->read();
                std::string msg;
                while (up && !up->dead && up->next(msg)) onUpLine(msg);
                if (!alive && up) up->dead = true;
            }
            for (auto& c : links) {
                if (c->dead || !FD_ISSET(c->s, &rd)) continue;
                if (!c->read()) c->dead = true;
                if (c->policy()) continue;
                std::string msg;
                while (c->next(msg)) onFromGuest(c->link, c->ip, msg);
            }

            for (auto& c : gameConns) c->flush();
            if (game) game->flush();
            if (up) up->flush();
            for (auto& c : links) c->flush();

            if (game && game->dead) {
                game.reset();
                st.gameConnected = false;
                changed();
            }
            if (up && up->dead && !retryNow) {
                bool closing = up->closeAfterFlush || st.phase == Phase::Closed;
                up.reset();
                upReady = false;
                if (!closing) {
                    if (st.phase == Phase::Connecting && !st.host && !st.reconnecting) close(Reason::Unreachable, L"");
                    else linkLost();
                }
            }
            std::vector<int> closed;
            for (auto& c : links)
                if (c->dead) closed.push_back(c->link);
            links.erase(std::remove_if(links.begin(), links.end(), [](const std::unique_ptr<Conn>& c) { return c->dead; }),
                        links.end());
            for (int link : closed) linkLeft(link);
            publish();
        }
        // Leaving: say goodbye so others don't wait out the grace window.
        if (st.phase != Phase::Closed) close(Reason::None, L"");
        auto deadline = Clock::now() + ms(500);
        while (Clock::now() < deadline) {
            bool pending = false;
            for (Conn* c : {up.get(), game.get()})
                if (c && !c->dead && !c->out.empty()) {
                    c->closeAfterFlush = false;
                    c->flush();
                    pending = pending || !c->out.empty();
                }
            for (auto& c : links)
                if (!c->dead && !c->out.empty()) {
                    c->closeAfterFlush = false;
                    c->flush();
                    pending = pending || !c->out.empty();
                }
            if (!pending) break;
            Sleep(10);
        }
        publish();
    }

    void acceptOn(SOCKET listener, bool bridge) {
        for (;;) {
            SOCKET s = ::accept(listener, nullptr, nullptr);
            if (s == INVALID_SOCKET) return;
            setNonBlocking(s);
            setNoDelay(s);
            auto c = std::make_unique<Conn>();
            c->s = s;
            c->ip = narrow(addressOf(s));
            if (bridge) {
                if (gameConns.size() >= 8) continue;
                gameConns.push_back(std::move(c));
            } else {
                if (links.size() >= kMaxLinks) {
                    c->send(line(obj("k", "reject")));
                    c->closeAfterFlush = true;
                    c->flush();
                    continue;
                }
                c->link = nextLink++;
                links.push_back(std::move(c));
            }
        }
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) { impl_->onChange = &onChange_; }
Session::~Session() { stop(); }

bool Session::start(const Options& opt, std::wstring& error) {
    stop();
    impl_ = std::make_unique<Impl>();
    impl_->onChange = &onChange_;
    Impl& d = *impl_;
    d.opt = opt;
    d.opt.playerName = cleanName(opt.playerName);
    if (d.opt.playerName.empty()) d.opt.playerName = L"Player";
    d.opt.maxPlayers = std::clamp(opt.maxPlayers, 2, 16);
    WSADATA wsa;
    if (int rc = WSAStartup(MAKEWORD(2, 2), &wsa)) {
        error = win32ErrorMessage(rc);
        return false;
    }
    int code = 0, port = 0;
    d.bridgeListener = listenOn(true, 0, port, code);
    if (d.bridgeListener == INVALID_SOCKET) {
        error = win32ErrorMessage(code);
        WSACleanup();
        return false;
    }
    Snapshot& st = d.st;
    st.bridgePort = port;
    st.transport = opt.transport;
    st.host = opt.host;
    st.maxPlayers = d.opt.maxPlayers;
    st.isPublic = opt.isPublic;
    st.locked = !opt.password.empty();
    st.roomName = opt.host ? (opt.roomName.empty() ? d.opt.playerName : cleanName(opt.roomName)) : L"";
    st.token = opt.token.empty() ? widen(randomHex(16)) : opt.token;
    st.mySlot = opt.host ? 0 : -1;
    if (opt.transport == Transport::Direct && opt.host) {
        int actual = 0;
        d.directListener = listenOn(false, opt.directPort, actual, code);
        if (d.directListener == INVALID_SOCKET) {
            error = win32ErrorMessage(code);
            closesocket(d.bridgeListener);
            WSACleanup();
            return false;
        }
        st.code = L"port " + std::to_wstring(actual);
        st.phase = Phase::Lobby;
        st.chat.push_back({-1, L"", L"Waiting for friends on port " + std::to_wstring(actual)});
    } else if (opt.transport == Transport::Direct) {
        st.code = opt.server;
    } else if (!opt.host) {
        st.code = opt.code;
    }
    d.stop = false;
    running_ = true;
    d.publish();
    thread_ = std::thread([this] {
        Impl& x = *impl_;
        if (!(x.opt.transport == Transport::Direct && x.opt.host)) {
            std::wstring err;
            if (!x.connectUp(err)) x.close(Reason::Unreachable, err);
            x.publish();
        }
        x.loop();
    });
    return true;
}

void Session::stop() {
    if (!impl_) return;
    if (thread_.joinable()) {
        impl_->stop = true;
        thread_.join();
    }
    if (running_) {
        Impl& d = *impl_;
        d.game.reset();
        d.up.reset();
        d.links.clear();
        d.gameConns.clear();
        if (d.bridgeListener != INVALID_SOCKET) closesocket(d.bridgeListener);
        if (d.directListener != INVALID_SOCKET) closesocket(d.directListener);
        d.bridgeListener = d.directListener = INVALID_SOCKET;
        WSACleanup();
        running_ = false;
    }
}

Snapshot Session::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->pub;
}

void Session::setReady(bool ready) {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d, ready] {
        if (d->st.host || d->st.phase != Phase::Lobby) return;
        json::Value r = obj("k", "ready");
        r.set("v", ready);
        d->toHost(r);
    });
}

void Session::startGame() {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d] { d->doStart(); });
}

void Session::sendChat(const std::wstring& text) {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d, text] { d->doChat(text); });
}

void Session::kick(int slot, bool ban) {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d, slot, ban] {
        if (!d->st.host) return;
        Impl::Peer* p = d->peerBySlot(slot);
        if (!p) return;
        if (ban) d->banned.insert(p->token);
        int link = p->link;
        std::wstring name = p->name;
        json::Value r = obj("k", "reject");
        r.set("code", ban ? "banned" : "kicked");
        d->toLink(link, r);
        d->ignored.insert(link);
        if (d->opt.transport == Transport::Direct)
            for (auto& c : d->links)
                if (c->link == link) c->closeAfterFlush = true;
        d->peerGone(*p, name + (ban ? L" was banned" : L" was kicked"));
    });
}

void Session::gameLaunched() {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d] {
        d->st.startNow = false;
        d->changed();
    });
}

void Session::gameExited() {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d] {
        d->game.reset();
        d->st.gameConnected = false;
        d->st.startNow = false;
        if (d->st.host) {
            if (d->st.phase == Phase::Starting || d->st.phase == Phase::InGame) d->backToLobby();
        } else if (d->myLoaded) {
            d->myLoaded = false;
            d->toHost(obj("k", "gameOver"));
        }
        d->changed();
    });
}

void Session::setServerPing(int ms) {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d, ms] { d->serverPing = ms; });
}

void Session::testDropLink() {
    Impl* d = impl_.get();
    std::lock_guard<std::mutex> lock(d->mu);
    d->cmds.push_back([d] {
        if (d->up) {
            shutdown(d->up->s, SD_BOTH);
            d->up->dead = true;
        }
    });
}

}  // namespace fl::lobby
