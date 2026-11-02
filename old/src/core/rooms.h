// Public rooms on the Multiplayer Everything central server (global-mods-server
// relay.py): "RELAY|2|ROOMS|<game>" answers "ROOMS|<code>,<players>,<max>,
// <locked>,<state>,<region>,<ping>,<version>,<name>;..." (an older server only
// knows "RELAY|1|LIST|<game>" -> "LIST|<code>,<players>,<max>,<name>;...") as
// NUL-terminated XMLSocket text. Also resolves which server to ask, the same
// way the mods do: mods-info servers.json first, then a built-in address.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct RoomInfo {
    std::wstring code, name;
    int players = 0, max = 0;
    // From RELAY|2|ROOMS (rooms the launcher's lobby hosts); empty/-1 on an older server.
    bool locked = false;
    std::wstring state;    // "lobby" | "ingame" | "" (unknown)
    std::wstring region;   // host's country, "NL"
    int hostPing = -1;     // host's round trip to the server, ms
    std::wstring version;  // "1.2#0123456789abcdef"
    bool inGame() const { return state == L"ingame"; }
};

// Parses a LIST or ROOMS reply. Room names may contain commas (they're the last field).
bool parseRoomList(std::string_view reply, std::vector<RoomInfo>& out);

// One short request on its own connection. `server` is "host:port".
// `rejected` (optional) is set when the server answered but doesn't run this
// game yet ("E|Unknown game ..."): the mod's mods-info file isn't merged.
bool listRooms(const std::wstring& server, const std::string& gameId, std::vector<RoomInfo>& out, std::wstring& error,
               int timeoutMs = 4000, bool* rejected = nullptr);

// One request/answer exchange: the first NUL-terminated answer. False when the
// server can't be reached or says nothing.
bool askServer(const std::wstring& server, const std::string& request, std::string& reply, std::wstring& error,
               int timeoutMs = 4000);

// Can the internet reach `port` on this PC? The server connects back to the
// address it sees us from. "open", "closed", "other" (something else answers
// there), or "" when the server can't test it (an older server, unreachable).
std::string probePort(const std::wstring& server, const std::string& gameId, int port, std::wstring& error);

// First "online" server in mods-info servers.json at `serversUrl`; `fallback`
// when that can't be read. Returns "host:port" or empty.
std::wstring resolveRoomServer(const std::wstring& serversUrl, const std::wstring& fallback, std::wstring* error);

}  // namespace fl
