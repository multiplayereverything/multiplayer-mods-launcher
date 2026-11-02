// What the host chooses before a Flash game starts, shared by the window and the command line (--launch): which
// Ruffle Together co-op save to start from, and the game's own settings (checkboxes and choices that become
// command-line options). Read-only over the config, the players found and the user's saved choices.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/library.h"
#include "core/settings.h"

namespace fl {

// Ruffle Together's lockstep co-op games keep one co-op save per game file.
inline bool coopSaveGame(const GameSpec& g) { return g.onlineId == "ruffle-coop"; }

// The release tag the launcher wrote next to a player it installed ("1.0.3.6"), empty for a copy it didn't install.
std::wstring installedPlayerVersion(const std::wstring& exe);

// The catalog's per-game settings from the copy the last session downloaded (catalog.json in the data folder).
std::map<std::string, std::vector<Setting>> cachedCatalogSettings(const std::wstring& dataDir);

class GameOptions {
public:
    GameOptions(const Config& cfg, const Library& lib, const UserData& user,
                const std::map<std::string, std::vector<Setting>>& catalogSettings)
        : cfg_(cfg), lib_(lib), user_(user), catalog_(catalogSettings) {}

    const PlayerSpec* coopSavePlayer() const;  // the player that takes a co-op save option, if any
    bool coopSaveSupported() const;            // that player is installed and new enough
    bool playerAtLeast(const std::wstring& version) const;
    std::wstring coopSaveFile(const GameSpec& g) const;  // where Ruffle Together keeps this game's co-op save

    const std::vector<Setting>& settingsFor(const GameSpec& g) const;  // the catalog's, else launcher.json's
    std::string settingValue(const GameSpec& g, const Setting& s) const;  // "1"/"0" or the choice id
    std::vector<std::wstring> settingArgs(const GameSpec& g, const std::vector<std::wstring>& actionArgs) const;

    // Adds the host's save choice (0 continue, 1 from the solo save, 2 new game) and the game's settings to an
    // action's arguments, as the window does before it starts the player.
    void addHostOptions(const GameSpec& g, std::vector<std::wstring>& args, int saveChoice) const;

private:
    const Config& cfg_;
    const Library& lib_;
    const UserData& user_;
    const std::map<std::string, std::vector<Setting>>& catalog_;
};

}  // namespace fl
