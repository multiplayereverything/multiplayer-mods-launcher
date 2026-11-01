// Command-line parsing for the launcher executable.
#pragma once
#include <string>
#include <vector>

namespace fl {

struct Args {
    std::wstring config, log, snapshot, launch;  // launch: "game" or "game:action"
    std::string installPlayer;                   // player id for --install-player
    std::string downloadGame;                    // game id or "all" for --download-game
    std::string rooms;                           // game id for --rooms
    std::string installMod, repairMod, uninstallMod;  // mod game ids
    std::wstring gameDir;                        // --game-dir: the game's folder, for the mod commands
    bool mods = false;                           // --mods: list the PC mods
    std::string select;
    int width = 0, height = 0, relayPort = 0;
    bool list = false, validate = false, help = false, demoHost = false, version = false, update = false;
    int guideScroll = 0;  // --guide-scroll <pixels>: scroll that far down in the snapshot
    bool guide = false;   // --guide: snapshot with the selected game's guide page open
    std::wstring lobby;  // --lobby setup|room: snapshot the lobby window
    bool json = false;   // --json: --list, --mods and --rooms print JSON for programs (the Tauri front end)
    std::wstring join, address;  // --join <code> / --address <ip:port> for --launch
    std::wstring error;
};

Args parseArgs(int argc, wchar_t** argv);
std::wstring usageText();

}  // namespace fl
