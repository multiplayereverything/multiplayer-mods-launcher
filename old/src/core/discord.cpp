#include "discord.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "json.h"
#include "log.h"
#include "strings.h"
#include "win.h"

namespace fl {

namespace {

// Frames are: opcode (0 handshake, 1 frame), payload length, JSON. Little-endian.
bool writeFrame(HANDLE pipe, uint32_t op, const std::string& json) {
    std::string buf(8, '\0');
    uint32_t len = static_cast<uint32_t>(json.size());
    memcpy(buf.data(), &op, 4);
    memcpy(buf.data() + 4, &len, 4);
    buf += json;
    DWORD done = 0;
    return WriteFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), &done, nullptr) && done == buf.size();
}

// Reads one reply if one arrives within `ms`, without ever blocking longer.
bool readFrame(HANDLE pipe, int ms, uint32_t& op, std::string& json) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    DWORD avail = 0;
    while (true) {
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail >= 8) break;
        if (std::chrono::steady_clock::now() > until) return false;
        Sleep(20);
    }
    char head[8];
    DWORD got = 0;
    if (!ReadFile(pipe, head, 8, &got, nullptr) || got != 8) return false;
    uint32_t len;
    memcpy(&op, head, 4);
    memcpy(&len, head + 4, 4);
    if (len > 1 << 20) return false;
    json.assign(len, '\0');
    for (DWORD off = 0; off < len; off += got)
        if (!ReadFile(pipe, json.data() + off, len - off, &got, nullptr) || !got) return false;
    return true;
}

HANDLE connectPipe() {
    for (int i = 0; i < 10; ++i) {
        std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return INVALID_HANDLE_VALUE;
}

}  // namespace

std::string discordHandshake(const std::wstring& clientId) {
    json::Value v = json::Value::object();
    v.set("v", 1);
    v.set("client_id", narrow(clientId));
    return json::serialize(v, false);
}

std::string discordActivity(const Presence* p, unsigned long pid, int nonce) {
    json::Value args = json::Value::object();
    args.set("pid", static_cast<int64_t>(pid));
    if (p) {
        json::Value a = json::Value::object();
        // Discord rejects fields shorter than 2 characters.
        if (p->details.size() >= 2) a.set("details", narrow(p->details.substr(0, 120)));
        if (p->state.size() >= 2) a.set("state", narrow(p->state.substr(0, 120)));
        if (p->start > 0) {
            json::Value ts = json::Value::object();
            ts.set("start", p->start);
            a.set("timestamps", std::move(ts));
        }
        args.set("activity", std::move(a));
    } else {
        args.set("activity", json::Value(nullptr));
    }
    json::Value v = json::Value::object();
    v.set("cmd", "SET_ACTIVITY");
    v.set("args", std::move(args));
    v.set("nonce", std::to_string(nonce));
    return json::serialize(v, false);
}

DiscordPresence::DiscordPresence(std::wstring clientId) : clientId_(std::move(clientId)) {
    thread_ = std::thread([this] { run(); });
}

DiscordPresence::~DiscordPresence() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DiscordPresence::set(const Presence& p) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (hasWanted_ && wanted_ == p) return;
        wanted_ = p;
        hasWanted_ = dirty_ = true;
    }
    cv_.notify_all();
}

void DiscordPresence::clear() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!hasWanted_) return;
        hasWanted_ = false;
        dirty_ = true;
    }
    cv_.notify_all();
}

void DiscordPresence::run() {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    int nonce = 0;
    auto drop = [&] {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    };
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
        // Wake on a change; retry a lost connection every 20 seconds while there is something to show.
        cv_.wait_for(lock, std::chrono::seconds(20), [this] { return stop_ || dirty_; });
        if (stop_) break;
        if (!dirty_ && (pipe != INVALID_HANDLE_VALUE || !hasWanted_)) continue;
        Presence p = wanted_;
        bool has = hasWanted_;
        dirty_ = false;
        lock.unlock();
        if (pipe == INVALID_HANDLE_VALUE && has) {
            pipe = connectPipe();
            uint32_t op = 0;
            std::string reply;
            if (pipe != INVALID_HANDLE_VALUE &&
                !(writeFrame(pipe, 0, discordHandshake(clientId_)) && readFrame(pipe, 3000, op, reply) && op == 1)) {
                logger().debug(L"Discord: handshake refused: " + widen(reply));
                drop();
            }
        }
        if (pipe != INVALID_HANDLE_VALUE) {
            uint32_t op = 0;
            std::string reply;
            if (!writeFrame(pipe, 1, discordActivity(has ? &p : nullptr, GetCurrentProcessId(), ++nonce))) {
                drop();
                lock.lock();
                dirty_ = has;  // Discord restarted: send it again on the next try
                continue;
            }
            readFrame(pipe, 2000, op, reply);  // drain the answer; an error is only logged
            if (reply.find("\"evt\":\"ERROR\"") != std::string::npos) logger().debug(L"Discord: " + widen(reply));
            if (!has) drop();  // nothing to show: let go of the pipe
        }
        lock.lock();
    }
    lock.unlock();
    if (pipe != INVALID_HANDLE_VALUE) {
        writeFrame(pipe, 1, discordActivity(nullptr, GetCurrentProcessId(), ++nonce));
        drop();
    }
}

}  // namespace fl
