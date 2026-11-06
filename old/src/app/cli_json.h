// Machine-readable forms of --list, --mods and --rooms (--json), for front ends that drive this exe: the Tauri
// prototype reads the library from here and starts everything through --launch and the mod commands.
#pragma once
#include <string>
#include <vector>

#include "core/config.h"
#include "core/library.h"
#include "core/rooms.h"
#include "core/settings.h"
#include "core/site.h"

namespace fl {

// The players found, the categories, every game with each action's state, the UI texts and the user's stats.
// Covers built into the exe are written under the data folder so the caller can show them. With `site`, each game
// also carries the website's grade, version and what stays in sync.
std::string libraryJson(const Config& cfg, const Library& lib, const UserData& user,
                        const std::vector<SiteGame>* site = nullptr);

struct ModReport {
    std::string id;
    bool gameFound = false, installed = false, running = false, known = false, published = false, stale = false;
    std::wstring gameDir, gameSource, installedVersion, problem, missing, missingUrl, warning, warningUrl, newest, minVersion, notice, error;
};
std::string modsJson(const std::vector<ModReport>& mods);

std::string roomsJson(const std::wstring& server, bool onServer, const std::vector<RoomInfo>& rooms, const std::wstring& error);

}  // namespace fl
