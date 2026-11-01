// Per-user launcher data: favorites, play statistics, chosen player, located
// game files and window placement. Stored as UTF-8 JSON in paths.data and
// written atomically so a crash never leaves a half-written file.
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace fl {

struct GameStats {
    bool favorite = false;
    int plays = 0;
    int64_t seconds = 0;     // total play time
    int64_t lastPlayed = 0;  // unix time, 0 = never
    std::string player;      // preferred player id, empty = config order
    std::map<std::string, std::wstring> swf;  // action id -> file chosen with "Locate"
    std::wstring gameDir;                     // PC mods: the game folder the player chose
    // The game's settings the player changed: setting id -> "1"/"0" for a checkbox, the choice id for a choice.
    std::map<std::string, std::string> settings;
};

struct UserData {
    std::map<std::string, GameStats> games;
    std::string selectedGame, category;
    // Settings window. minimize: -1 = launcher.json's behavior.minimizeOnLaunch.
    int minimize = -1;
    bool online = true;          // room list and update checks
    std::string defaultPlayer;   // empty = automatic (config order)
    std::wstring gamesDir;       // empty = paths.games
    std::wstring playerName;     // passed to games as ${PLAYER_NAME}
    bool showOriginals = false;  // list "edition": "original" actions
    bool autoUpdate = true;      // install new launcher releases by itself
    int sortMode = 0;            // the grid's order: 0 ours, 1 recently played, 2 name, 3 most players
    bool compactCards = false;   // smaller cards in the grid
    bool showOffline = false;    // list "offline" actions (solo, one keyboard)
    int x = 0, y = 0, width = 0, height = 0;  // 0 = default placement
    bool maximized = false;

    GameStats& stats(const std::string& gameId) { return games[gameId]; }
    const GameStats* find(const std::string& gameId) const;
};

UserData loadUserData(const std::wstring& path);
bool saveUserData(const UserData& data, const std::wstring& path, std::wstring* error = nullptr);

int64_t unixNow();

}  // namespace fl
