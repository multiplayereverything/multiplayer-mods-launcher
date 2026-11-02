// Discord Rich Presence through the Discord app's local pipe (discord-ipc-N):
// "Playing Gun Mayhem 2, Room VK7Q2". Nothing leaves the PC except through the
// Discord app itself. A worker thread owns the pipe; Discord not running is
// normal, and it is tried again now and then.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace fl {

struct Presence {
    std::wstring details;  // first line, e.g. "Playing Gun Mayhem 2"
    std::wstring state;    // second line, e.g. "Room VK7Q2"
    int64_t start = 0;     // unix time the game started; 0 = no timer
    bool operator==(const Presence&) const = default;
};

// The Rich Presence messages, exposed for tests.
std::string discordHandshake(const std::wstring& clientId);
std::string discordActivity(const Presence* p, unsigned long pid, int nonce);  // null p clears it

class DiscordPresence {
public:
    explicit DiscordPresence(std::wstring clientId);
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    void set(const Presence& p);
    void clear();

private:
    void run();
    std::wstring clientId_;
    std::mutex mu_;
    std::condition_variable cv_;
    Presence wanted_;
    bool hasWanted_ = false, dirty_ = false, stop_ = false;
    std::thread thread_;
};

}  // namespace fl
