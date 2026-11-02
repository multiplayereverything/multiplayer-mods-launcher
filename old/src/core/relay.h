// Built-in netplay relay for the online mods (Gun Mayhem 2 Online, Stealth
// Hunter 2 Online...). Same wire protocol as the mods' gm2_relay.py /
// sh2_relay.py, so a game can't tell the difference:
//
//   messages are NUL-terminated UTF-8 text (Flash XMLSocket)
//   first message "H|name" makes a client the host, "J|name" a guest
//   host  -> relay : anything, forwarded to every guest unchanged
//   guest -> relay : "<msg>", forwarded to the host as "G|<guest id>|<msg>"
//   relay -> host  : "W|0" welcome, "J|<id>|<name>" join, "L|<id>" leave
//   relay -> guest : "W|<id>" welcome, "X" when the host leaves
//   "D|text" from anyone is a debug line for the log
//
// It also answers Flash's <policy-file-request/> on the game port (and on
// 843 when that port is free). One thread, non-blocking sockets, TCP_NODELAY:
// per-frame input messages are tiny and must not wait for Nagle.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fl {

struct RelayPeer {
    int id = 0;
    std::wstring name, address;
    bool host = false;
};

struct RelaySnapshot {
    bool running = false;
    int port = 0;
    std::vector<RelayPeer> peers;  // host first
    uint64_t messages = 0;         // relayed so far
};

class Relay {
public:
    Relay();
    ~Relay();
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    // Binds synchronously so the caller learns about "port in use" at once.
    // `addressInUse` is set when the port is taken (likely another relay).
    bool start(int port, std::wstring& error, bool* addressInUse = nullptr);
    void stop();
    bool running() const { return running_; }
    int port() const { return port_; }
    RelaySnapshot snapshot() const;
    // Called on the relay thread whenever peers join or leave.
    void setOnChange(std::function<void()> fn) { onChange_ = std::move(fn); }

private:
    struct Client;
    void loop();
    void accept(uintptr_t listener, bool policyOnly);
    void onData(Client& c);
    void onMessage(Client& c, const std::string& msg);
    void send(Client& c, const std::string& msg);
    void flush(Client& c);
    void drop(Client& c);
    Client* find(int id);
    void publish();

    std::atomic<bool> running_{false}, stop_{false};
    int port_ = 0;
    uintptr_t listener_ = ~uintptr_t(0), policyListener_ = ~uintptr_t(0);
    std::vector<std::unique_ptr<Client>> clients_;
    int nextId_ = 1;
    int hostId_ = 0;
    uint64_t messages_ = 0;
    std::thread thread_;
    mutable std::mutex mu_;
    RelaySnapshot snap_;
    std::function<void()> onChange_;
};

}  // namespace fl
