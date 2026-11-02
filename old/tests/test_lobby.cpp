// The lobby and its localhost bridge over real loopback sockets: Direct mode
// against the host's own listener, Online mode against a small stand-in for
// the central server's star rooms (RELAY|2, and RELAY|1 for an older server).
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "core/json.h"
#include "core/lobby.h"
#include "core/net.h"
#include "core/rooms.h"
#include "core/win.h"
#include "test.h"
// win.h first (WIN32_LEAN_AND_MEAN keeps the old winsock.h out).
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace fl;
using namespace fl::lobby;

namespace {

int freeLobbyPort() {
    static int next = 48300 + static_cast<int>(GetCurrentProcessId() % 300) * 2;
    return next++;
}

bool waitUntil(const std::function<bool()>& cond, int timeoutMs = 5000) {
    for (int i = 0; i < timeoutMs / 20; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}

// A NUL-framed TCP client: the game on the bridge, or a raw peer.
class Wire {
public:
    explicit Wire(int port) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        DWORD timeout = 3000;
        setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        ok_ = connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    }
    ~Wire() {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        WSACleanup();
    }
    bool ok() const { return ok_; }
    void send(const std::string& msg) {
        std::string d = msg + std::string(1, '\0');
        ::send(s_, d.data(), static_cast<int>(d.size()), 0);
    }
    std::string recv() {
        for (;;) {
            size_t nul = buf_.find('\0');
            if (nul != std::string::npos) {
                std::string m = buf_.substr(0, nul);
                buf_.erase(0, nul + 1);
                return m;
            }
            char tmp[4096];
            int n = ::recv(s_, tmp, sizeof tmp, 0);
            if (n == 0) return "<closed>";
            if (n < 0) return WSAGetLastError() == WSAETIMEDOUT ? "<timeout>" : "<closed>";
            buf_.append(tmp, n);
        }
    }
    // Next message whose "t" is `type` (skipping others), parsed.
    json::Value expect(const std::string& type) {
        for (int i = 0; i < 50; ++i) {
            std::string m = recv();
            if (m == "<closed>" || m == "<timeout>") return json::Value();
            std::string err;
            json::Value v = json::parse(m, &err);
            const json::Value* t = v.isObject() ? v.find("t") : nullptr;
            if (t && t->isString() && t->str() == type) return v;
        }
        return json::Value();
    }
    void close() {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        s_ = INVALID_SOCKET;
    }

private:
    SOCKET s_ = INVALID_SOCKET;
    bool ok_ = false;
    std::string buf_;
};

int num(const json::Value& v, const char* key) {
    const json::Value* x = v.isObject() ? v.find(key) : nullptr;
    return x && x->isNumber() ? static_cast<int>(x->number()) : -999;
}
std::string text(const json::Value& v, const char* key) {
    const json::Value* x = v.isObject() ? v.find(key) : nullptr;
    return x && x->isString() ? x->str() : "";
}

// The game connecting to its launcher's bridge, as a mod would.
std::unique_ptr<Wire> gameOn(Session& s) {
    Snapshot snap = s.snapshot();
    auto w = std::make_unique<Wire>(snap.bridgePort);
    w->send("{\"t\":\"hello\",\"token\":\"" + narrow(snap.token) + "\"}");
    return w;
}

Options directHost(int port, const std::wstring& name = L"Hana") {
    Options o;
    o.transport = Transport::Direct;
    o.host = true;
    o.directPort = port;
    o.gameId = "sh2";
    o.version = versionTag("1.0", "0123456789abcdef0123");
    o.playerName = name;
    o.maxPlayers = 4;
    return o;
}

Options directGuest(int port, const std::wstring& name) {
    Options o = directHost(port, name);
    o.host = false;
    o.server = L"127.0.0.1:" + std::to_wstring(port);
    return o;
}

int playersIn(Session& s) {
    int n = 0;
    for (const auto& p : s.snapshot().players)
        if (p.connected) ++n;
    return n;
}

// A tiny stand-in for the central server's star rooms.
class StarServer {
public:
    explicit StarServer(bool v1Only = false) : v1Only_(v1Only) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(listener_, 16);
        int len = sizeof a;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { loop(); });
    }
    ~StarServer() {
        stop_ = true;
        thread_.join();
        for (auto& [s, c] : conns_) closesocket(s);
        closesocket(listener_);
        WSACleanup();
    }
    std::wstring address() const { return L"127.0.0.1:" + std::to_wstring(port_); }
    std::string lastMeta() {
        std::lock_guard<std::mutex> l(mu_);
        return meta_;
    }
    std::string password() {
        std::lock_guard<std::mutex> l(mu_);
        return password_;
    }

private:
    struct C {
        std::string in;
        int id = -1;  // 0 host, >0 guest, -1 before the handshake
        std::string name;
    };
    void out(SOCKET s, const std::string& m) {
        std::string d = m + std::string(1, '\0');
        ::send(s, d.data(), static_cast<int>(d.size()), 0);
    }
    void drop(SOCKET s) {
        C c = conns_[s];
        conns_.erase(s);
        closesocket(s);
        if (c.id == 0) {
            for (auto& [gs, g] : conns_)
                if (g.id > 0) out(gs, "X");
            host_ = INVALID_SOCKET;
        } else if (c.id > 0 && host_ != INVALID_SOCKET) {
            out(host_, "L|" + std::to_string(c.id));
        }
    }
    void line(SOCKET s, C& c, const std::string& m) {
        if (c.id < 0) {
            std::vector<std::string> p;
            size_t a = 0;
            for (;;) {
                size_t b = m.find('|', a);
                p.push_back(m.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            if (p.size() < 4 || p[0] != "RELAY") return out(s, "E|Bad handshake");
            if (p[1] == "2" && v1Only_) return out(s, "E|Unsupported relay handshake version (this server speaks RELAY|1)");
            bool v2 = p[1] == "2";
            if (p[2] == "HOST") {
                version_ = p[4];
                {
                    std::lock_guard<std::mutex> l(mu_);
                    password_ = v2 ? p[7] : "";
                }
                c.id = 0;
                host_ = s;
                out(s, "ROOM|ABCDE|4");
                out(s, "W|0");
            } else if (p[2] == "JOIN") {
                std::string code = p[5], version = p[4], pw = v2 ? p[6] : "";
                c.name = p.back();
                if (host_ == INVALID_SOCKET || code != "ABCDE") return out(s, "E|No room with code " + code);
                if (version != version_)
                    return out(s, "E|Version mismatch: the host runs '" + version_ + "', you run '" + version + "'.");
                {
                    std::lock_guard<std::mutex> l(mu_);
                    if (pw != password_) return out(s, password_.empty() ? "E|Wrong password" : "E|This room needs a password");
                }
                c.id = nextId_++;
                out(s, "W|" + std::to_string(c.id));
                out(host_, "J|" + std::to_string(c.id) + "|" + c.name);
            }
            return;
        }
        if (c.id == 0) {
            if (m.rfind("@META|", 0) == 0) {
                std::lock_guard<std::mutex> l(mu_);
                meta_ = m;
                return;
            }
            for (auto& [gs, g] : conns_)
                if (g.id > 0) out(gs, m);
        } else if (host_ != INVALID_SOCKET) {
            out(host_, "G|" + std::to_string(c.id) + "|" + m);
        }
    }
    void loop() {
        while (!stop_) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listener_, &rd);
            for (auto& [s, c] : conns_) FD_SET(s, &rd);
            timeval tv{0, 30000};
            if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
            if (FD_ISSET(listener_, &rd)) {
                SOCKET s = accept(listener_, nullptr, nullptr);
                if (s != INVALID_SOCKET) conns_[s] = C{};
            }
            std::vector<SOCKET> gone;
            for (auto& [s, c] : conns_) {
                if (!FD_ISSET(s, &rd)) continue;
                char buf[8192];
                int n = ::recv(s, buf, sizeof buf, 0);
                if (n <= 0) {
                    gone.push_back(s);
                    continue;
                }
                c.in.append(buf, n);
                size_t nul;
                while ((nul = c.in.find('\0')) != std::string::npos) {
                    std::string m = c.in.substr(0, nul);
                    c.in.erase(0, nul + 1);
                    line(s, c, m);
                }
            }
            for (SOCKET s : gone)
                if (conns_.count(s)) drop(s);
        }
    }
    bool v1Only_;
    SOCKET listener_ = INVALID_SOCKET, host_ = INVALID_SOCKET;
    int port_ = 0, nextId_ = 1;
    std::string version_, meta_, password_;
    std::map<SOCKET, C> conns_;
    std::mutex mu_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace

TEST(lobby_version_tag_and_password_hash) {
    CHECK(versionTag("1.2", "0123456789abcdef0123456789") == "1.2#0123456789abcdef");
    CHECK(versionTag("1.2", "") == "1.2");
    CHECK(versionName("1.2#0123456789abcdef") == "1.2");
    CHECK(hashPassword(L"") == "");
    CHECK(hashPassword(L"password") == "5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8");
}

TEST(lobby_bridge_answers_flash_policy_and_refuses_wrong_token) {
    int port = freeLobbyPort();
    Session host;
    std::wstring err;
    REQUIRE(host.start(directHost(port), err));
    Wire policy(host.snapshot().bridgePort);
    REQUIRE(policy.ok());
    policy.send("<policy-file-request/>");
    CHECK(policy.recv().find("cross-domain-policy") != std::string::npos);
    CHECK(policy.recv() == "<closed>");
    Wire wrong(host.snapshot().bridgePort);
    wrong.send("{\"t\":\"hello\",\"token\":\"not-mine\"}");
    CHECK(wrong.recv() == "<closed>");
    CHECK(!host.snapshot().gameConnected);
}

TEST(lobby_direct_round_start_barrier_and_routing) {
    int port = freeLobbyPort();
    Session host, ann, bob;
    std::wstring err;
    REQUIRE(host.start(directHost(port), err));
    REQUIRE(ann.start(directGuest(port, L"Ann"), err));
    REQUIRE(waitUntil([&] { return ann.snapshot().mySlot == 1; }));
    REQUIRE(bob.start(directGuest(port, L"Bob"), err));
    REQUIRE(waitUntil([&] { return bob.snapshot().mySlot == 2; }));
    REQUIRE(waitUntil([&] { return playersIn(host) == 3 && playersIn(bob) == 3; }));
    CHECK(bob.snapshot().players[1].name == L"Ann");
    CHECK(ann.snapshot().phase == Phase::Lobby);

    // Not everyone ready: the host can't start.
    host.startGame();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(host.snapshot().phase == Phase::Lobby);
    ann.setReady(true);
    bob.setReady(true);
    REQUIRE(waitUntil([&] {
        auto ps = host.snapshot().players;
        return ps.size() == 3 && ps[1].ready && ps[2].ready;
    }));

    ann.sendChat(L"hi all");
    REQUIRE(waitUntil([&] {
        auto c = bob.snapshot().chat;
        return !c.empty() && c.back().text == L"hi all" && c.back().name == L"Ann";
    }));

    host.startGame();
    REQUIRE(waitUntil([&] { return host.snapshot().startNow && ann.snapshot().startNow && bob.snapshot().startNow; }));
    int seed = host.snapshot().seed;
    CHECK(ann.snapshot().seed == seed && bob.snapshot().seed == seed);

    auto gh = gameOn(host), ga = gameOn(ann), gb = gameOn(bob);
    json::Value wh = gh->expect("welcome"), wa = ga->expect("welcome"), wb = gb->expect("welcome");
    CHECK(num(wh, "slot") == 0 && text(wh, "role") == "host");
    CHECK(num(wa, "slot") == 1 && text(wa, "role") == "client");
    CHECK(num(wb, "slot") == 2 && num(wb, "seed") == seed);
    const json::Value* players = wb.find("players");
    CHECK(players && players->isArray() && players->size() == 3);

    // The barrier: nobody gets "go" until every game is loaded.
    gh->send("{\"t\":\"loaded\"}");
    ga->send("{\"t\":\"loaded\"}");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(host.snapshot().phase == Phase::Starting);
    gb->send("{\"t\":\"loaded\"}");
    CHECK(!gh->expect("go").isNull());
    CHECK(!ga->expect("go").isNull());
    CHECK(!gb->expect("go").isNull());
    CHECK(host.snapshot().phase == Phase::InGame);

    gh->send("{\"t\":\"send\",\"to\":-1,\"d\":{\"x\":1}}");
    json::Value ma = ga->expect("msg"), mb = gb->expect("msg");
    CHECK(num(ma, "from") == 0 && num(*ma.find("d"), "x") == 1);
    CHECK(num(mb, "from") == 0);
    ga->send("{\"t\":\"send\",\"to\":0,\"d\":\"to host\"}");
    json::Value mh = gh->expect("msg");
    CHECK(num(mh, "from") == 1 && text(mh, "d") == "to host");
    ga->send("{\"t\":\"send\",\"to\":2,\"d\":\"to bob\"}");
    json::Value mb2 = gb->expect("msg");
    CHECK(num(mb2, "from") == 1 && text(mb2, "d") == "to bob");
    gb->send("{\"t\":\"send\",\"to\":-1,\"d\":\"everyone\"}");
    CHECK(text(gh->expect("msg"), "d") == "everyone");
    CHECK(text(ga->expect("msg"), "d") == "everyone");

    // The host's game closing ends the round for everyone.
    host.gameExited();
    CHECK(!ga->expect("end").isNull());
    REQUIRE(waitUntil([&] { return ann.snapshot().phase == Phase::Lobby && bob.snapshot().phase == Phase::Lobby; }));
    CHECK(!host.snapshot().players[1].ready);
}

TEST(lobby_direct_refuses_other_versions_and_wrong_passwords) {
    int port = freeLobbyPort();
    Options h = directHost(port);
    h.password = L"secret";
    Session host;
    std::wstring err;
    REQUIRE(host.start(h, err));

    Options other = directGuest(port, L"Old");
    other.version = versionTag("1.0", "ffffffffffffffffffff");
    other.password = L"secret";
    Session old;
    REQUIRE(old.start(other, err));
    REQUIRE(waitUntil([&] { return old.snapshot().phase == Phase::Closed; }));
    CHECK(old.snapshot().reason == Reason::Version);
    CHECK(narrow(old.snapshot().hostVersion) == h.version);

    Options bad = directGuest(port, L"Guess");
    bad.password = L"wrong";
    Session guess;
    REQUIRE(guess.start(bad, err));
    REQUIRE(waitUntil([&] { return guess.snapshot().phase == Phase::Closed; }));
    CHECK(guess.snapshot().reason == Reason::Password);

    Options good = directGuest(port, L"Pat");
    good.password = L"secret";
    Session pat;
    REQUIRE(pat.start(good, err));
    REQUIRE(waitUntil([&] { return pat.snapshot().mySlot == 1; }));
    CHECK(playersIn(host) == 2);
}

TEST(lobby_direct_room_fills_up) {
    int port = freeLobbyPort();
    Options h = directHost(port);
    h.maxPlayers = 2;
    Session host, a, b;
    std::wstring err;
    REQUIRE(host.start(h, err));
    REQUIRE(a.start(directGuest(port, L"A"), err));
    REQUIRE(waitUntil([&] { return a.snapshot().mySlot == 1; }));
    REQUIRE(b.start(directGuest(port, L"B"), err));
    REQUIRE(waitUntil([&] { return b.snapshot().phase == Phase::Closed; }));
    CHECK(b.snapshot().reason == Reason::Full);
}

TEST(lobby_reconnect_keeps_the_slot) {
    int port = freeLobbyPort();
    Session host, ann, bob;
    std::wstring err;
    REQUIRE(host.start(directHost(port), err));
    REQUIRE(ann.start(directGuest(port, L"Ann"), err));
    REQUIRE(waitUntil([&] { return ann.snapshot().mySlot == 1; }));
    REQUIRE(bob.start(directGuest(port, L"Bob"), err));
    REQUIRE(waitUntil([&] { return bob.snapshot().mySlot == 2; }));
    std::wstring token = ann.snapshot().token;
    ann.testDropLink();
    REQUIRE(waitUntil([&] {
        for (const auto& p : host.snapshot().chat)
            if (p.text.find(L"lost connection") != std::wstring::npos) return true;
        return false;
    }));
    // Back within the grace window: same slot, same token, nobody took the place.
    REQUIRE(waitUntil([&] { return !ann.snapshot().reconnecting && ann.snapshot().mySlot == 1; }, 8000));
    CHECK(ann.snapshot().token == token);
    REQUIRE(waitUntil([&] { return playersIn(host) == 3; }));
    CHECK(bob.snapshot().mySlot == 2);
}

TEST(lobby_kick_and_ban) {
    int port = freeLobbyPort();
    Session host, ann, bob;
    std::wstring err;
    REQUIRE(host.start(directHost(port), err));
    REQUIRE(ann.start(directGuest(port, L"Ann"), err));
    REQUIRE(waitUntil([&] { return ann.snapshot().mySlot == 1; }));
    host.kick(1, false);
    REQUIRE(waitUntil([&] { return ann.snapshot().phase == Phase::Closed; }));
    CHECK(ann.snapshot().reason == Reason::Kicked);
    REQUIRE(waitUntil([&] { return playersIn(host) == 1; }));

    REQUIRE(bob.start(directGuest(port, L"Bob"), err));
    REQUIRE(waitUntil([&] { return bob.snapshot().mySlot == 1; }));
    std::wstring token = bob.snapshot().token;
    host.kick(1, true);
    REQUIRE(waitUntil([&] { return bob.snapshot().phase == Phase::Closed; }));
    CHECK(bob.snapshot().reason == Reason::Banned);
    Options again = directGuest(port, L"Bob2");
    again.token = token;
    Session back;
    REQUIRE(back.start(again, err));
    REQUIRE(waitUntil([&] { return back.snapshot().phase == Phase::Closed; }));
    CHECK(back.snapshot().reason == Reason::Banned);
}

TEST(lobby_host_leaving_closes_the_room) {
    int port = freeLobbyPort();
    auto host = std::make_unique<Session>();
    Session ann;
    std::wstring err;
    REQUIRE(host->start(directHost(port), err));
    REQUIRE(ann.start(directGuest(port, L"Ann"), err));
    REQUIRE(waitUntil([&] { return ann.snapshot().mySlot == 1; }));
    host->stop();
    REQUIRE(waitUntil([&] { return ann.snapshot().phase == Phase::Closed; }));
    CHECK(ann.snapshot().reason == Reason::HostLeft);
}

TEST(lobby_online_room_through_the_server) {
    StarServer server;
    Options h;
    h.transport = Transport::Online;
    h.host = true;
    h.server = server.address();
    h.gameId = "sh2";
    h.version = versionTag("1.0", "0123456789abcdef0123");
    h.playerName = L"Hana";
    h.roomName = L"Hana's room";
    h.password = L"pw";
    h.region = L"NL";
    Session host, ann;
    std::wstring err;
    REQUIRE(host.start(h, err));
    REQUIRE(waitUntil([&] { return host.snapshot().code == L"ABCDE" && host.snapshot().phase == Phase::Lobby; }));
    CHECK(server.password() == hashPassword(L"pw"));  // checked by the server, never sent to other players
    REQUIRE(waitUntil([&] { return server.lastMeta().find("state=lobby") != std::string::npos; }));
    CHECK(server.lastMeta().find("region=NL") != std::string::npos);

    Options g = h;
    g.host = false;
    g.code = L"ABCDE";
    g.playerName = L"Ann";
    Session wrongPw;
    Options gw = g;
    gw.password = L"nope";
    REQUIRE(wrongPw.start(gw, err));
    REQUIRE(waitUntil([&] { return wrongPw.snapshot().phase == Phase::Closed; }));
    CHECK(wrongPw.snapshot().reason == Reason::Password);

    REQUIRE(ann.start(g, err));
    REQUIRE(waitUntil([&] { return ann.snapshot().mySlot == 1 && playersIn(host) == 2; }));
    ann.setReady(true);
    REQUIRE(waitUntil([&] { return host.snapshot().players.size() == 2 && host.snapshot().players[1].ready; }));
    host.startGame();
    REQUIRE(waitUntil([&] { return ann.snapshot().startNow; }));
    REQUIRE(waitUntil([&] { return server.lastMeta().find("state=ingame") != std::string::npos; }));
    auto gh = gameOn(host), ga = gameOn(ann);
    CHECK(num(ga->expect("welcome"), "slot") == 1);
    gh->expect("welcome");
    gh->send("{\"t\":\"loaded\"}");
    ga->send("{\"t\":\"loaded\"}");
    CHECK(!ga->expect("go").isNull());
    ga->send("{\"t\":\"send\",\"to\":0,\"d\":[1,2,3]}");
    json::Value m = gh->expect("msg");
    CHECK(num(m, "from") == 1 && m.find("d") && m.find("d")->size() == 3);

    Options other = g;
    other.version = versionTag("1.1", "ffffffffffffffffffff");
    Session old;
    REQUIRE(old.start(other, err));
    REQUIRE(waitUntil([&] { return old.snapshot().phase == Phase::Closed; }));
    CHECK(old.snapshot().reason == Reason::Version);
    CHECK(narrow(old.snapshot().hostVersion) == h.version);

    host.stop();
    REQUIRE(waitUntil([&] { return ann.snapshot().phase == Phase::Closed; }));
    CHECK(ann.snapshot().reason == Reason::HostLeft);
}

TEST(lobby_online_falls_back_to_an_older_server) {
    StarServer server(true);
    Options h;
    h.transport = Transport::Online;
    h.host = true;
    h.server = server.address();
    h.gameId = "sh2";
    h.version = "1.0";
    h.playerName = L"Hana";
    h.password = L"pw";  // the old server can't check it: the host does
    Session host, ann, bad;
    std::wstring err;
    REQUIRE(host.start(h, err));
    REQUIRE(waitUntil([&] { return host.snapshot().phase == Phase::Lobby; }));
    CHECK(!host.snapshot().serverV2);
    Options g = h;
    g.host = false;
    g.code = L"ABCDE";
    g.playerName = L"Ann";
    Options gb = g;
    gb.password = L"bad";
    REQUIRE(bad.start(gb, err));
    REQUIRE(waitUntil([&] { return bad.snapshot().phase == Phase::Closed; }));
    CHECK(bad.snapshot().reason == Reason::Password);
    REQUIRE(ann.start(g, err));
    REQUIRE(waitUntil([&] { return ann.snapshot().mySlot == 1; }));
}

TEST(lobby_online_wrong_code) {
    StarServer server;
    Options g;
    g.transport = Transport::Online;
    g.server = server.address();
    g.gameId = "sh2";
    g.version = "1.0";
    g.code = L"ZZZZZ";
    g.playerName = L"Ann";
    Session ann;
    std::wstring err;
    REQUIRE(ann.start(g, err));
    REQUIRE(waitUntil([&] { return ann.snapshot().phase == Phase::Closed; }));
    CHECK(ann.snapshot().reason == Reason::NoRoom);
}

// Against the real relay (global-mods-server, run locally): set FL_TEST_RELAY=127.0.0.1:<port>.
TEST(lobby_against_a_local_relay) {
    char addr[64] = {};
    if (!GetEnvironmentVariableA("FL_TEST_RELAY", addr, sizeof addr)) return;  // skipped
    Options h;
    h.transport = Transport::Online;
    h.host = true;
    h.server = widen(addr);
    h.gameId = "sh2";
    h.version = versionTag("1.0", "0123456789abcdef0123");
    h.playerName = L"Hana";
    h.roomName = L"Relay test";
    h.password = L"pw";
    h.region = L"NL";
    Session host, ann;
    std::wstring err;
    REQUIRE(host.start(h, err));
    REQUIRE(waitUntil([&] { return host.snapshot().phase == Phase::Lobby; }));
    CHECK(host.snapshot().serverV2);
    std::wstring code = host.snapshot().code;
    CHECK(code.size() == 5);
    host.setServerPing(33);
    std::vector<RoomInfo> rooms;
    bool listed = waitUntil([&] {
        std::wstring e;
        listRooms(widen(addr), "sh2", rooms, e);
        for (const auto& r : rooms)
            if (r.code == code && r.locked && r.state == L"lobby" && r.region == L"NL") return true;
        return false;
    }, 8000);
    CHECK(listed);
    Options g = h;
    g.host = false;
    g.code = code;
    g.playerName = L"Ann";
    REQUIRE(ann.start(g, err));
    REQUIRE(waitUntil([&] { return ann.snapshot().mySlot == 1; }));
    ann.setReady(true);
    REQUIRE(waitUntil([&] { return host.snapshot().players.size() == 2 && host.snapshot().players[1].ready; }));
    host.startGame();
    REQUIRE(waitUntil([&] { return ann.snapshot().startNow; }));
    auto gh = gameOn(host), ga = gameOn(ann);
    gh->expect("welcome");
    ga->expect("welcome");
    gh->send("{\"t\":\"loaded\"}");
    ga->send("{\"t\":\"loaded\"}");
    CHECK(!ga->expect("go").isNull());
    gh->send("{\"t\":\"send\",\"to\":1,\"d\":\"via the relay\"}");
    CHECK(text(ga->expect("msg"), "d") == "via the relay");
    CHECK(waitUntil([&] {
        std::wstring e;
        listRooms(widen(addr), "sh2", rooms, e);
        for (const auto& r : rooms)
            if (r.code == code) return r.state == L"ingame";
        return false;
    }, 8000));
    std::wstring e;
    CHECK(!probePort(widen(addr), "sh2", host.snapshot().bridgePort, e).empty());
}

TEST(direct_addresses_put_vpns_first) {
    std::vector<sys::NetAddress> all = {{L"192.168.1.20", L"Wi-Fi"},
                                        {L"26.12.0.5", L"Radmin VPN"},
                                        {L"10.147.17.3", L"ZeroTier One [8056c2e21c000001]"},
                                        {L"25.1.2.3", L"Hamachi"},
                                        {L"100.101.5.6", L"Tailscale"},
                                        {L"8.8.4.4", L"Odd adapter"}};
    auto list = net::shareAddresses(all);
    REQUIRE(list.size() == 6);
    CHECK(list[0].vpn == L"Radmin VPN" && list[1].vpn == L"ZeroTier" && list[2].vpn == L"Hamachi" && list[3].vpn == L"Tailscale");
    CHECK(list[4].address == L"192.168.1.20" && list[4].kind == net::AddressKind::Lan);
    CHECK(list[5].kind == net::AddressKind::Other);
}
