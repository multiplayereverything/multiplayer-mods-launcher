// The built-in relay against real loopback sockets, checked message by
// message against the protocol of the mods' gm2_relay.py / sh2_relay.py.
#include <chrono>
#include <thread>

#include "core/relay.h"
#include "core/win.h"
#include "test.h"
// win.h first (WIN32_LEAN_AND_MEAN keeps the old winsock.h out).
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace fl;

namespace {

int freePort() {
    static int next = 47700 + static_cast<int>(GetCurrentProcessId() % 400);
    return next++;
}

class Peer {
public:
    explicit Peer(int port) {
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
    ~Peer() {
        close();
        WSACleanup();
    }
    bool ok() const { return ok_; }
    void sendRaw(const std::string& data) { ::send(s_, data.data(), static_cast<int>(data.size()), 0); }
    void sendMsg(const std::string& msg) { sendRaw(msg + std::string(1, '\0')); }
    // Next NUL-terminated message; "<closed>" or "<timeout>" otherwise.
    std::string recvMsg() {
        for (;;) {
            size_t nul = buf_.find('\0');
            if (nul != std::string::npos) {
                std::string m = buf_.substr(0, nul);
                buf_.erase(0, nul + 1);
                return m;
            }
            char tmp[4096];
            int n = recv(s_, tmp, sizeof tmp, 0);
            if (n == 0) return "<closed>";
            if (n < 0) return WSAGetLastError() == WSAETIMEDOUT ? "<timeout>" : "<closed>";
            buf_.append(tmp, n);
        }
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

bool waitFor(Relay& r, size_t peers) {
    for (int i = 0; i < 100; ++i) {
        if (r.snapshot().peers.size() == peers) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

TEST(relay_answers_flash_policy_request) {
    Relay relay;
    std::wstring err;
    int port = freePort();
    REQUIRE(relay.start(port, err));
    Peer p(port);
    REQUIRE(p.ok());
    p.sendRaw(std::string("<policy-file-request/>") + '\0');
    std::string policy = p.recvMsg();
    CHECK(policy.find("<cross-domain-policy>") != std::string::npos);
    CHECK(policy.find("to-ports=\"*\"") != std::string::npos);
    CHECK(p.recvMsg() == "<closed>");
}

TEST(relay_policy_request_split_across_packets) {
    Relay relay;
    std::wstring err;
    int port = freePort();
    REQUIRE(relay.start(port, err));
    Peer p(port);
    REQUIRE(p.ok());
    p.sendRaw("<policy-fi");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    p.sendRaw(std::string("le-request/>") + '\0');
    CHECK(p.recvMsg().find("<cross-domain-policy>") != std::string::npos);
    CHECK(p.recvMsg() == "<closed>");
}

TEST(relay_routes_host_and_guests) {
    Relay relay;
    std::wstring err;
    int port = freePort();
    REQUIRE(relay.start(port, err));

    Peer guestEarly(port);  // a friend may connect before the host
    REQUIRE(guestEarly.ok());
    guestEarly.sendMsg("J|Early");
    std::string w1 = guestEarly.recvMsg();
    CHECK(w1.rfind("W|", 0) == 0);
    std::string earlyId = w1.substr(2);

    Peer host(port);
    REQUIRE(host.ok());
    host.sendMsg("H|Alice");
    CHECK(host.recvMsg() == "W|0");
    CHECK(host.recvMsg() == "J|" + earlyId + "|Early");  // told about guests already there

    Peer guest(port);
    REQUIRE(guest.ok());
    guest.sendMsg("\r\nJ|Bob");  // newlines around a message are ignored
    std::string w2 = guest.recvMsg();
    REQUIRE(w2.rfind("W|", 0) == 0);
    std::string bobId = w2.substr(2);
    CHECK(host.recvMsg() == "J|" + bobId + "|Bob");
    CHECK(waitFor(relay, 3));
    RelaySnapshot snap = relay.snapshot();
    REQUIRE(snap.peers.size() == 3);
    CHECK(snap.peers[0].host && snap.peers[0].name == L"Alice");

    host.sendMsg("F|12|keys");  // host -> every guest, unchanged
    CHECK(guest.recvMsg() == "F|12|keys");
    CHECK(guestEarly.recvMsg() == "F|12|keys");

    guest.sendMsg("I|3|left");  // guest -> host, tagged with the guest id
    CHECK(host.recvMsg() == "G|" + bobId + "|I|3|left");

    guest.sendMsg("D|debug line");  // debug lines go to the log only
    guest.sendMsg("I|4");
    CHECK(host.recvMsg() == "G|" + bobId + "|I|4");

    Peer second(port);
    REQUIRE(second.ok());
    second.sendMsg("H|Mallory");
    CHECK(second.recvMsg() == "E|A host is already connected to this server");

    guest.close();
    CHECK(host.recvMsg() == "L|" + bobId);
    host.close();
    CHECK(guestEarly.recvMsg() == "X");
    CHECK(waitFor(relay, 1));
}

TEST(relay_keeps_message_order_under_load) {
    Relay relay;
    std::wstring err;
    int port = freePort();
    REQUIRE(relay.start(port, err));
    Peer host(port), guest(port);
    REQUIRE(host.ok() && guest.ok());
    host.sendMsg("H|h");
    CHECK(host.recvMsg() == "W|0");
    guest.sendMsg("J|g");
    CHECK(guest.recvMsg().rfind("W|", 0) == 0);
    host.recvMsg();  // J|id|g
    std::string batch;
    for (int i = 0; i < 2000; ++i) batch += "F|" + std::to_string(i) + std::string(1, '\0');
    host.sendRaw(batch);  // many messages in few TCP segments
    bool inOrder = true;
    for (int i = 0; i < 2000 && inOrder; ++i) inOrder = guest.recvMsg() == "F|" + std::to_string(i);
    CHECK(inOrder);
}

TEST(relay_reports_port_in_use) {
    Relay a, b;
    std::wstring err;
    int port = freePort();
    REQUIRE(a.start(port, err));
    bool inUse = false;
    CHECK(!b.start(port, err, &inUse));
    CHECK(inUse);
    a.stop();
    CHECK(!a.running());
    CHECK(b.start(port, err));  // free again after stop
}
