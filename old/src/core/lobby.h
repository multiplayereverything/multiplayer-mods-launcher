// The launcher's own lobby for Flash games: rooms, ready-up, start, and a
// localhost bridge the game talks to, so a mod needs no network code of its
// own beyond one socket to 127.0.0.1 and no lobby screen.
//
// Launcher <-> launcher (the "link"): NUL-terminated JSON objects with a "k"
// field, carried either by the central server's star rooms (RELAY|2 HOST/JOIN,
// RELAY|1 on an older server) or, in DIRECT mode, by a TCP connection to the
// host's own listener. The host's launcher is the authority: it assigns slots,
// checks versions, runs the ready-up, the start and the "loaded" barrier, and
// routes game traffic between players.
//
// Game <-> launcher (the bridge): NUL-terminated JSON objects with a "t" field
// on 127.0.0.1:<bridgePort>, which also answers Flash's <policy-file-request/>.
// See docs/LOBBY_BRIDGE.md for both protocols.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fl::lobby {

constexpr int kProtocol = 1;
constexpr int kHeartbeatMs = 5000;      // pings and lobby refreshes
constexpr int kTimeoutMs = 15000;       // silence after which a peer counts as gone
constexpr int kGraceMs = 30000;         // a dropped player keeps their slot this long
constexpr int kLoadTimeoutMs = 90000;   // the "loaded" barrier gives up on slow players after this
constexpr size_t kMaxChat = 200;        // characters per chat line

enum class Transport { Online, Direct };
enum class Phase { Connecting, Lobby, Starting, InGame, Closed };

struct Player {
    int slot = 0;  // 0 = host, P1
    std::wstring name;
    bool host = false, ready = false, connected = true, loaded = false, you = false;
    int pingMs = -1;
};

struct ChatLine {
    int slot = -1;  // -1: a message from the launcher itself (joined, left...)
    std::wstring name, text;
};

// Why a session ended or a join failed; the window shows the matching text.
enum class Reason {
    None,
    HostLeft,        // the host closed the room or quit
    Kicked,
    Banned,
    Full,
    InGame,          // the room is already playing
    Version,         // the host runs another build: `hostVersion` says which
    Password,        // wrong or missing password
    TooManyTries,
    NoRoom,          // wrong code
    Unreachable,     // can't connect to the server or the host
    Lost,            // the connection dropped and didn't come back within the grace window
    Server,          // another refusal from the server: `detail` has its text
};

struct Snapshot {
    Phase phase = Phase::Connecting;
    Transport transport = Transport::Online;
    bool host = false, reconnecting = false, locked = false, isPublic = true;
    std::wstring code;      // room code (online) or "address:port" (direct)
    std::wstring roomName;
    int maxPlayers = 4, mySlot = -1, seed = 0;
    int bridgePort = 0;
    std::wstring token;     // this player's session token (FlashVars sessionToken)
    std::vector<Player> players;  // by slot
    std::deque<ChatLine> chat;
    Reason reason = Reason::None;
    std::wstring detail, hostVersion;
    bool gameConnected = false;   // this PC's game is on the bridge
    bool startNow = false;        // the host started: launch the game (set until gameLaunched())
    bool serverV2 = true;         // false: an older server (no passwords, no room state)
    uint64_t revision = 0;
};

struct Options {
    Transport transport = Transport::Online;
    bool host = false;
    std::wstring server;        // Online: "host:port" of the central server. Direct guest: "address:port" of the host
    int directPort = 7790;      // Direct host: where to listen
    std::string gameId;         // the mod's id on the server ("gm2")
    std::string version;        // "<mod version>#<first 16 hex of the swf's SHA-256>"
    std::wstring roomName, playerName, password;  // password: plain text, hashed before it leaves this PC
    bool isPublic = true;
    int maxPlayers = 4;
    std::wstring code;          // Online guest: the room to join
    std::wstring region;        // host's two-letter country, shown in room lists
    std::wstring token;         // resume with this token (empty = new player)
};

// Builds "1.2#0123456789abcdef" from a mod version and the game file's SHA-256 (hex).
std::string versionTag(const std::string& modVersion, const std::string& swfSha256);
// The part before '#'.
std::string versionName(const std::string& tag);
std::string hashPassword(const std::wstring& password);  // hex SHA-256 of the UTF-8 text, "" for ""

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Opens the bridge listener and (Direct host) the game listener at once,
    // then connects in the background. False with `error` when a port can't be opened.
    bool start(const Options& opt, std::wstring& error);
    void stop();  // leaves the room (the host closes it)
    bool running() const { return running_; }
    Snapshot snapshot() const;
    // Called on the session's thread whenever the snapshot changes.
    void setOnChange(std::function<void()> fn) { onChange_ = std::move(fn); }

    // Commands, safe from any thread; applied on the session's thread.
    void setReady(bool ready);
    void startGame();           // host: everyone connected is ready
    void sendChat(const std::wstring& text);
    void kick(int slot, bool ban);
    void gameLaunched();        // the launcher started the game for this start
    void gameExited();          // this PC's game closed
    void setServerPing(int ms); // host: shown to others in the room list

    // Tests only: cut the link without leaving, to exercise the reconnect path.
    void testDropLink();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::function<void()> onChange_;
};

}  // namespace fl::lobby
