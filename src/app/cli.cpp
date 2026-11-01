#include "cli.h"

#include "core/strings.h"

namespace fl {

Args parseArgs(int argc, wchar_t** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::wstring k = argv[i];
        if (k.size() > 1 && k[0] == L'/') k = L"--" + k.substr(1);  // accept /list style
        auto value = [&](std::wstring& out) {
            if (i + 1 >= argc) {
                a.error = L"Missing value after " + k;
                return;
            }
            out = argv[++i];
        };
        std::wstring tmp;
        if (k == L"--config") value(a.config);
        else if (k == L"--log") value(a.log);
        else if (k == L"--snapshot") value(a.snapshot);
        else if (k == L"--launch") value(a.launch);
        else if (k == L"--install-player") {
            value(tmp);
            a.installPlayer = narrow(tmp);
        } else if (k == L"--install-mod") {
            value(tmp);
            a.installMod = narrow(tmp);
        } else if (k == L"--repair-mod") {
            value(tmp);
            a.repairMod = narrow(tmp);
        } else if (k == L"--uninstall-mod") {
            value(tmp);
            a.uninstallMod = narrow(tmp);
        } else if (k == L"--game-dir") {
            value(a.gameDir);
        } else if (k == L"--rooms") {
            value(tmp);
            a.rooms = narrow(tmp);
        } else if (k == L"--download-game") {
            value(tmp);
            a.downloadGame = narrow(tmp);
        } else if (k == L"--select") {
            value(tmp);
            a.select = narrow(tmp);
        } else if (k == L"--size") {
            value(tmp);
            if (swscanf_s(tmp.c_str(), L"%dx%d", &a.width, &a.height) != 2 || a.width < 200 || a.height < 200)
                a.error = L"--size must look like 1280x720";
        } else if (k == L"--relay") {
            value(tmp);
            a.relayPort = _wtoi(tmp.c_str());
            if (a.relayPort < 1 || a.relayPort > 65535) a.error = L"--relay needs a port between 1 and 65535";
        } else if (k == L"--demo-host") a.demoHost = true;
        else if (k == L"--guide") a.guide = true;
        else if (k == L"--guide-scroll") {
            value(tmp);
            a.guideScroll = _wtoi(tmp.c_str());
        }
        else if (k == L"--lobby") value(a.lobby);
        else if (k == L"--list") a.list = true;
        else if (k == L"--json") a.json = true;
        else if (k == L"--join") value(a.join);
        else if (k == L"--address") value(a.address);
        else if (k == L"--version") a.version = true;
        else if (k == L"--update") a.update = true;
        else if (k == L"--mods") a.mods = true;
        else if (k == L"--validate") a.validate = true;
        else if (k == L"--help" || k == L"-h" || k == L"--?" || k == L"-?") a.help = true;
        else a.error = L"Unknown option: " + std::wstring(argv[i]);
        if (!a.error.empty()) break;
    }
    return a;
}

std::wstring usageText() {
    return LR"(Usage: MultiplayerEverythingLauncher.exe [options]

Without options the launcher window opens. The configuration is
launcher.json next to the exe.

  --config <file>        Use this launcher.json instead
  --list                 Print every game, its actions and whether each can start
  --launch <game[:action]>
                         Start a game without the window (default action: the first).
                         Host actions keep the relay running until the game exits.
    --join <code>        ...joining this room (the action's joinArgs, ${CODE})
    --address <ip:port>  ...connecting to this address (${ADDRESS})
  --json                 With --list, --mods or --rooms: print JSON for programs
  --relay <port>         Run only the netplay relay (a dedicated relay PC); Ctrl+C stops
  --install-player <id>  Download and install a player that has an "install" entry
                         (e.g. ruffle) into its folder, then exit
  --download-game <id|all>
                         Download a game's files from the game catalog into the games folder
  --validate             Check launcher.json, print problems, exit
  --log <file>           Write the log here instead of the data folder
  --snapshot <png>       Render the window to a PNG and exit (visual checks)
    --select <game>      ...with this game selected
    --size <WxH>         ...at this window size in pixels
    --demo-host          ...with the selected game's relay running
  --rooms <game>         List the open rooms for a game on the central server
  --update               Install the newest launcher release now, if there is one
  --mods                 List the PC game mods: game found, installed version, newest version
  --install-mod <id>     Install (or update) a PC game mod into its game's folder
  --repair-mod <id>      Put back missing or damaged files of an installed mod
  --uninstall-mod <id>   Remove a mod and restore the files it replaced
    --game-dir <folder>  ...use this game folder instead of detecting one
  --version              Print the launcher version
  --help                 Show this help

Exit codes: 0 success, 1 failed, 3 bad configuration or arguments,
4 game cannot start (missing player or game file).
)";
}

}  // namespace fl
