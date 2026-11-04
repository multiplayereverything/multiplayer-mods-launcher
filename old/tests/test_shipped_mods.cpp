// The PC mods the launcher ships, installed from the layouts of their real GitHub
// release zips (file names only), into fake game folders: files land where the
// mod needs them, and an uninstall leaves the game folder exactly as it was.
#include <atomic>
#include <filesystem>
#include <thread>

#include "core/config.h"
#include "core/community.h"
#include "core/install.h"
#include "core/json.h"
#include "core/mod/manager.h"
#include "core/mod/package.h"
#include "core/win.h"
#include "shipped.h"
#include "test.h"
// win.h first (WIN32_LEAN_AND_MEAN keeps the old winsock.h out).
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace fl;
using namespace fl::mod;

namespace {

// A stand-in for api.github.com that counts its questions: every request gets the release list whose newest
// release is `version` (assets "Mod-<version>.zip") or, for a tag, the loader release.
class FakeGithub {
public:
    FakeGithub() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(listener_, 8);
        int len = sizeof a;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] {
            while (!stop_) {
                fd_set rd;
                FD_ZERO(&rd);
                FD_SET(listener_, &rd);
                timeval tv{0, 50000};
                if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
                SOCKET c = accept(listener_, nullptr, nullptr);
                char buf[4096];
                int n = recv(c, buf, sizeof buf - 1, 0);
                std::string req(buf, n > 0 ? n : 0);
                bool tag = req.find("/releases/tags/") != std::string::npos;
                ++(tag ? tagHits : listHits);
                std::string asset = tag ? "Loader.zip" : "Mod-" + narrow(version) + ".zip";
                std::string rel = std::string("{\"tag_name\":\"") + (tag ? "loader-1" : "v" + narrow(version)) +
                                  "\",\"draft\":false,\"assets\":[{\"name\":\"" + asset +
                                  "\",\"browser_download_url\":\"https://example.invalid/" + asset +
                                  "\",\"digest\":\"sha256:" + std::string(64, 'a') + "\"}]}";
                bool latest = req.find("/releases/latest") != std::string::npos;  // a player's lookup: one release, not a list
                std::string body = tag || latest ? rel : "[" + rel + "]";
                std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                    "\r\nConnection: close\r\n\r\n" + body;
                send(c, reply.data(), static_cast<int>(reply.size()), 0);
                closesocket(c);
            }
        });
    }
    ~FakeGithub() {
        stop_ = true;
        thread_.join();
        closesocket(listener_);
        WSACleanup();
    }
    std::wstring address() const { return L"http://127.0.0.1:" + std::to_wstring(port_); }
    std::atomic<int> listHits{0}, tagHits{0};
    std::wstring version = L"1.0.0";

private:
    SOCKET listener_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

Config loadShipped() {
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    if (!r.ok()) throw std::runtime_error(narrow(formatIssues(r.issues)));
    test::addTeamFixtures(r.config);
    return r.config;
}

const GameSpec* shippedMod(const Config& c, const char* id) {
    const GameSpec* g = c.findGame(id);
    return g && g->mod ? g : nullptr;
}

// What is in the game folder, as "relative path size" lines.
std::string folderSnapshot(const std::wstring& dir) {
    std::string out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(fs::longPath(dir), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec))
            out += narrow(it->path().wstring().substr(dir.size())) + " " + std::to_string(it->file_size(ec)) + "\n";
    return out;
}

Outcome installShipped(const Config& cfg, const GameSpec& g, const std::wstring& gameDir, const std::wstring& payload,
                       const char* version, Operation op = Operation::Install) {
    Spec spec = *g.mod;
    spec.mod.version = widen(version);
    spec.text = &cfg.text;
    Vars base;
    CancelToken cancel;
    return install(spec, base, op, gameDir, payload, [](const Progress&) {}, cancel);
}

Outcome uninstallShipped(const Config& cfg, const GameSpec& g, const std::wstring& gameDir) {
    Spec spec = *g.mod;
    spec.text = &cfg.text;
    Vars base;
    CancelToken cancel;
    return uninstall(spec, base, gameDir, [](const Progress&) {}, cancel);
}

}  // namespace

TEST(shipped_spooky_installs_from_its_release_layout_and_uninstalls_cleanly) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "spooky-together");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/spooky-together");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].github == L"multiplayereverything/spooky-together");
    CHECK(g->mod->package.extras[0].tag == L"bepinex-5.4.23.5");
    TempDir root("spooky-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\SpookyUnity.exe", "game");
    writeText(game + L"\\SpookyUnity_Data\\data", "d");
    writeText(game + L"\\UnityPlayer.dll", "u");
    // SpookyTogether-BepInEx-0.2.9.zip is the game-folder layout (its own copy of the loader is ignored);
    // the loader comes from BepInEx_win_x64_5.4.23.5.zip, unpacked as "loader".
    for (const char* f : {"winhttp.dll", "changelog.txt", "README.md", "BepInEx\\core\\BepInEx.dll",
                          "BepInEx\\plugins\\SpookyTogether\\SpookyTogether.BepInEx.dll"})
        writeText(payload + L"\\" + widen(f), std::string("zip ") + f);
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt",
                          "BepInEx\\core\\BepInEx.dll", "BepInEx\\core\\0Harmony.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.2.9");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\doorstop_config.ini") && exists(game + L"\\.doorstop_version"));
    CHECK(exists(game + L"\\BepInEx\\core\\BepInEx.dll") && exists(game + L"\\BepInEx\\core\\0Harmony.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\SpookyTogether\\SpookyTogether.BepInEx.dll"));
    CHECK(!exists(game + L"\\README.md") && !exists(game + L"\\changelog.txt"));  // the zip's own notes stay out of the game
    Outcome again = installShipped(cfg, *g, game, payload, "0.2.9", Operation::Repair);
    CHECK(again.result == Outcome::Result::Success);
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_baldi_installs_the_plugin_and_the_loader_extra) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "baldi-together");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/baldi-plus-coop");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].dir == L"loader" && g->mod->package.extras[0].tag == L"bepinex-5.4.23.5");
    CHECK(g->mod->package.extras[0].github == L"multiplayereverything/baldi-plus-coop");
    TempDir root("baldi-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\BALDI.exe", "game");
    writeText(game + L"\\BALDI_Data\\data", "d");
    writeText(game + L"\\UnityPlayer.dll", "u");
    writeText(game + L"\\UserData\\BaldiTogether.ini", "keep me");
    // BaldiTogether-BepInEx-0.9.0.zip: the plugin and notes; BepInEx_win_x64_5.4.23.5.zip unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\BaldiTogether\\BaldiTogether.BepInEx.dll", "plugin");
    writeText(payload + L"\\README.md", "notes");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt", "BepInEx\\core\\BepInEx.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.9.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\BepInEx\\core\\BepInEx.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\BaldiTogether\\BaldiTogether.BepInEx.dll"));
    CHECK(!exists(game + L"\\changelog.txt") && !exists(game + L"\\README.md"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);  // including the player's own UserData settings
}

TEST(shipped_miside_installs_bepinex_6_and_the_plugin_and_uninstalls_cleanly) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "miside-together");
    REQUIRE(g != nullptr);
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].github == L"multiplayereverything/miside-coop");
    CHECK(g->mod->package.extras[0].tag == L"bepinex-6.0.0-be.790");
    TempDir root("miside-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\MiSideFull.exe", "game");
    writeText(game + L"\\MiSideFull_Data\\data", "d");
    writeText(game + L"\\GameAssembly.dll", "g");
    // MiSideTogether-0.3.1.zip is the plugin only; the BepInEx-Unity.IL2CPP-win-x64 be.790 zip is unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\MiSideTogether\\MiSideTogether.dll", "plugin");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt",
                          "BepInEx\\core\\BepInEx.Unity.IL2CPP.dll", "BepInEx\\core\\BepInEx.Core.dll",
                          "dotnet\\System.dll", "dotnet\\clrjit.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.3.1");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\BepInEx\\core\\BepInEx.Unity.IL2CPP.dll") && exists(game + L"\\dotnet\\clrjit.dll"));
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\doorstop_config.ini") && exists(game + L"\\.doorstop_version"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\MiSideTogether\\MiSideTogether.dll"));
    CHECK(!exists(game + L"\\changelog.txt"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_gta_keeps_the_games_eax_dll_next_to_the_wrapper_and_puts_it_back) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "gta-coop");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/gta-coop" && g->siteId == "gta");
    TempDir root("gta-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\gta_sa.exe", "game");
    writeText(game + L"\\eax.dll", "THE GAMES EAX");
    writeText(game + L"\\models\\gta3.img", "img");
    // GTACoop-1.0.0.zip: game\ (the files that go next to gta_sa.exe) and server\.
    writeText(payload + L"\\game\\coop_eax.dll", "COOP WRAPPER");
    writeText(payload + L"\\game\\CoopAndreasSA.dll", "plugin");
    writeText(payload + L"\\game\\LaunchCoopAndreas.exe", "launcher");
    writeText(payload + L"\\game\\LaunchCoopAndreas.exe.manifest", "manifest");
    writeText(payload + L"\\game\\install.bat", "not run by the launcher");
    writeText(payload + L"\\game\\CoopAndreas\\main.scm", "scm");
    writeText(payload + L"\\game\\CoopAndreas\\script.img", "img");
    writeText(payload + L"\\server\\server.exe", "server");
    writeText(payload + L"\\server\\server-config.ini", "port = 6767");
    writeText(payload + L"\\source\\0001.patch", "source");
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "1.0.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(readText(game + L"\\eax.dll") == "COOP WRAPPER");    // what install.bat does: coop_eax.dll becomes eax.dll
    CHECK(readText(game + L"\\eax_orig.dll") == "THE GAMES EAX");  // and the game's own is kept beside it
    CHECK(exists(game + L"\\CoopAndreasSA.dll") && exists(game + L"\\LaunchCoopAndreas.exe"));
    CHECK(exists(game + L"\\CoopAndreas\\main.scm") && exists(game + L"\\CoopAndreas\\server\\server.exe"));
    CHECK(!exists(game + L"\\install.bat") && !exists(game + L"\\coop_eax.dll") && !exists(game + L"\\CoopAndreas\\source"));
    // The host edits the server's settings; an update keeps them, and the copy of the original eax.dll stays the
    // original (the wrapper is not copied over it).
    writeText(game + L"\\CoopAndreas\\server\\server-config.ini", "port = 7000");
    Outcome again = installShipped(cfg, *g, game, payload, "1.0.0", Operation::Update);
    REQUIRE(again.result == Outcome::Result::Success);
    CHECK(readText(game + L"\\eax_orig.dll") == "THE GAMES EAX" && readText(game + L"\\eax.dll") == "COOP WRAPPER");
    CHECK(readText(game + L"\\CoopAndreas\\server\\server-config.ini") == "port = 7000");
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(readText(game + L"\\eax.dll") == "THE GAMES EAX");
    // Only the host's edited settings may be left behind (the engine never deletes a file the player changed).
    std::string left = folderSnapshot(game);
    CHECK(left.find("eax_orig.dll") == std::string::npos && left.find("CoopAndreasSA.dll") == std::string::npos);
    (void)before;
}

TEST(a_package_that_lost_a_file_is_reported_as_damaged_and_can_be_discarded) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "gta-coop");
    REQUIRE(g != nullptr);
    TempDir root("av");
    std::wstring game = root / L"Game", payload = root / L"cache\\gta-coop\\1.0.0\\payload";
    writeText(game + L"\\gta_sa.exe", "game");
    writeText(game + L"\\eax.dll", "THE GAMES EAX");
    writeText(game + L"\\models\\gta3.img", "img");
    // An antivirus program took LaunchCoopAndreas.exe out of the unpacked package.
    writeText(payload + L"\\game\\coop_eax.dll", "wrapper");
    writeText(payload + L"\\game\\CoopAndreasSA.dll", "plugin");
    writeText(payload + L"\\game\\CoopAndreas\\main.scm", "scm");
    writeText(payload + L"\\server\\server.exe", "server");
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "1.0.0");
    CHECK(o.result == Outcome::Result::Failed && o.packageDamaged);
    CHECK(o.message.find(L"LaunchCoopAndreas.exe") != std::wstring::npos && o.message.find(L"ntivirus") != std::wstring::npos);
    CHECK(folderSnapshot(game) == before);  // nothing was changed in the game folder
    std::wstring cache = root / L"cache";
    REQUIRE(exists(payload + L"\\game\\coop_eax.dll"));
    discardPackage(cache, L"gta-coop", L"1.0.0");
    CHECK(!exists(payload + L"\\game\\coop_eax.dll"));
}

TEST(a_required_file_blocks_the_install_until_it_is_there) {
    LoadResult r = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                       "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "games": [ { "id": "g", "name": "G", "mod": { "game": {"name": "G", "executable": "G.exe",
        "detection": [{"type": "path", "path": "C:\\x"}]}, "components": [{"id": "m", "source": "a"}],
        "package": {"url": "https://example.invalid/p.zip"},
        "install": {"requires": [{"path": "Loader/core/loader.dll", "message": "Install the loader first."}]} } } ] })",
                              L"C:\\x");
    REQUIRE(r.ok());
    Spec spec = *r.config.games[0].mod;
    spec.text = &r.config.text;
    TempDir root("requires");
    Vars base;
    CHECK(missingRequirement(spec, base, root / L"Game") == L"Install the loader first.");
    writeText(root / L"Game\\Loader\\core\\loader.dll", "x");
    CHECK(missingRequirement(spec, base, root / L"Game").empty());
}

TEST(shipped_cupheads_installs_the_plugin_and_bepinex_5_and_uninstalls_cleanly) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "cupheads");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/CupHeads");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].tag == L"bepinex-5.4.23.5");
    TempDir root("cup-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\Cuphead.exe", "game");
    writeText(game + L"\\Cuphead_Data\\data", "d");
    // CupHeads-BepInEx-1.7.0.zip is the plugin only; BepInEx_win_x64_5.4.23.5.zip is unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\CupheadOnline\\CupheadOnline.dll", "plugin");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt", "BepInEx\\core\\BepInEx.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "1.7.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\BepInEx\\core\\BepInEx.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\CupheadOnline\\CupheadOnline.dll"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_rick_and_morty_installs_its_own_bepinex_cfg) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "rick-and-morty-together");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/rick-and-morty-coop");
    TempDir root("rick-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\RickAndMortyVR.exe", "game");
    writeText(game + L"\\RickAndMortyVR_Data\\data", "d");
    // RickAndMortyTogether-BepInEx-0.1.0.zip: the plugin, the settings this game needs, and a readme.
    writeText(payload + L"\\BepInEx\\plugins\\RickAndMortyTogether\\RickAndMortyTogether.dll", "plugin");
    writeText(payload + L"\\BepInEx\\config\\BepInEx.cfg", "[Preloader.Entrypoint]");
    writeText(payload + L"\\RickAndMortyTogether-README.md", "notes");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "BepInEx\\core\\BepInEx.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.1.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(readText(game + L"\\BepInEx\\config\\BepInEx.cfg") == "[Preloader.Entrypoint]");  // the mod's settings, not a default
    CHECK(!exists(game + L"\\RickAndMortyTogether-README.md"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_dave_the_diver_installs_bepinex_6_and_the_plugin) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "dave-the-diver-multiplayer");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/Dave-The-Divers-Coop");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].tag == L"bepinex-6.0.0-be.790");
    TempDir root("dave-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\DaveTheDiver.exe", "game");
    writeText(game + L"\\DaveTheDiver_Data\\data", "d");
    writeText(game + L"\\GameAssembly.dll", "g");
    // DaveTheDiverMP-0.19.0.zip: the plugin and its license; the be.790 IL2CPP zip is unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\DaveTheDiverMP\\DaveTheDiverMP.dll", "plugin");
    writeText(payload + L"\\BepInEx\\plugins\\DaveTheDiverMP\\LICENSE.txt", "MIT");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "BepInEx\\core\\BepInEx.Unity.IL2CPP.dll",
                          "dotnet\\clrjit.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.19.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\BepInEx\\core\\BepInEx.Unity.IL2CPP.dll") && exists(game + L"\\dotnet\\clrjit.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\DaveTheDiverMP\\DaveTheDiverMP.dll"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(a_new_release_shows_within_ten_minutes_and_a_pinned_loader_is_asked_once) {
    LoadResult r = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                       "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "games": [ { "id": "g", "name": "G", "mod": { "id": "g-mod", "game": {"name": "G", "executable": "G.exe",
        "detection": [{"type": "path", "path": "C:\\x"}]}, "components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r", "asset": "Mod-*.zip",
                    "extras": [{"dir": "loader", "github": "o/r", "tag": "loader-1", "asset": "Loader.zip"}]} } } ] })",
                              L"C:\\x");
    REQUIRE(r.ok());
    Spec spec = *r.config.games[0].mod;
    TempDir cache("sources-cache");
    FakeGithub api;
    std::wstring err;
    Source s;

    REQUIRE(resolveSource(spec, L"", s, err, api.address(), cache.path()));
    CHECK(s.version == L"1.0.0" && s.extras.size() == 1);
    CHECK(api.listHits == 1 && api.tagHits == 1);  // the first time: the release list and the loader

    api.version = L"1.0.1";  // a new release appears; the answer heard a moment ago is still used...
    REQUIRE(resolveSource(spec, L"", s, err, api.address(), cache.path()));
    CHECK(s.version == L"1.0.0" && api.listHits == 1);
    // ...but F5 asks again, and the loader (pinned by tag) is not asked about a second time
    REQUIRE(resolveSource(spec, L"", s, err, api.address(), cache.path(), true));
    CHECK(s.version == L"1.0.1");
    CHECK(api.listHits == 2 && api.tagHits == 1);
    CHECK(s.extras.size() == 1 && s.extras[0].url.find(L"Loader.zip") != std::wstring::npos);

    // An answer older than ten minutes is asked again without F5.
    api.version = L"1.0.2";
    std::string text = readText(cache / L"sources.json");
    std::string perr;
    json::Value all = json::parse(text, &perr);
    REQUIRE(all.isObject() && all.find("g-mod"));
    json::Value entry = *all.find("g-mod");
    entry.set("at", 1000);
    all.set("g-mod", entry);
    std::string aged = json::serialize(all, true);
    writeText(cache / L"sources.json", aged);
    REQUIRE(resolveSource(spec, L"", s, err, api.address(), cache.path()));
    CHECK(s.version == L"1.0.2" && api.listHits == 3 && api.tagHits == 1);
}

TEST(mod_config_github_extras_and_requires_are_validated) {
    auto load = [](const std::string& mod) {
        return loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                       "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
                       "games": [ { "id": "g", "name": "G", "mod": { "game": {"name": "G", "executable": "G.exe",
                       "detection": [{"type": "path", "path": "C:\\x"}]}, )" +
                              mod + "} } ] }",
                          L"C:\\x");
    };
    LoadResult ok = load(R"("components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r", "asset": "*.zip", "extras": [{"dir": "loader", "github": "o/l", "tag": "v1", "asset": "l.zip"}]},
        "install": {"requires": [{"path": "BepInEx/core/x.dll", "message": "install it"}]})");
    if (!ok.ok()) fprintf(stderr, "%ls\n", formatIssues(ok.issues).c_str());
    REQUIRE(ok.ok());
    const Spec& s = *ok.config.games[0].mod;
    CHECK(s.package.github == L"o/r" && s.package.extras.size() == 1 && s.package.extras[0].tag == L"v1");
    CHECK(s.install.requires_.size() == 1 && s.install.requires_[0].message == L"install it");
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}], "package": {"github": "justarepo"})").ok());
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r", "extras": [{"dir": "a/b", "github": "o/l", "asset": "l.zip"}]})")
               .ok());
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r", "extras": [{"dir": "x"}]})")
               .ok());
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}], "package": {"github": "o/r"},
        "install": {"requires": [{"message": "no path"}]})")
               .ok());
}

TEST(a_player_the_launcher_installed_is_updated_only_when_a_newer_release_exists) {
    TempDir dir("player-update");
    FakeGithub api;
    std::string player = R"([{ "id": "rt", "name": "Ruffle Together", "paths": ["rt.exe"],
        "install": { "github": "o/r", "asset": "Mod-*.zip", "api": ")" + narrow(api.address()) +
                         R"(", "dir": "${PLAYERS_DIR}\\rt", "exe": "rt.exe" } }])";
    LoadResult r = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                       "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": )" + player + R"(, "games": [{ "id": "g", "name": "G", "actions": [{ "id": "p", "swf": "a.swf" }] }] })",
                              dir.path());
    REQUIRE(r.ok());
    Vars vars;
    vars.set(L"PLAYERS_DIR", dir.path() + L"\\players");
    const PlayerSpec& p = r.config.players[0];
    const std::wstring exe = dir.path() + L"\\players\\rt\\rt.exe";
    const std::wstring marker = dir.path() + L"\\players\\rt\\.version";

    // Not installed by the launcher (no file in its folder): left alone, GitHub isn't even asked.
    PlayerUpdate none = updateInstalledPlayer(p, vars, nullptr);
    CHECK(!none.checked && !none.updated && none.error.empty() && api.listHits == 0);

    // The newest release is the one installed: nothing to do.
    writeText(exe, "old");
    writeText(marker, "v1.0.1");
    api.version = L"1.0.1";
    PlayerUpdate same = updateInstalledPlayer(p, vars, nullptr);
    CHECK(same.checked && !same.available && !same.updated && same.error.empty() && same.installed == L"v1.0.1");
    CHECK(readText(exe) == "old");

    // Only asked (a front end asks the player first): the newer release is reported, nothing is downloaded.
    api.version = L"1.0.2";
    PlayerUpdate asked = updateInstalledPlayer(p, vars, nullptr, false);
    CHECK(asked.checked && asked.available && !asked.updated && asked.error.empty() && !asked.version.empty());
    CHECK(readText(exe) == "old");
    CHECK(readText(marker) == "v1.0.1");

    // A newer release exists but can't be downloaded here: the installed copy stays exactly as it was.
    api.version = L"1.0.2";
    PlayerUpdate newer = updateInstalledPlayer(p, vars, nullptr);
    CHECK(newer.checked && !newer.updated && !newer.error.empty());
    CHECK(readText(exe) == "old");
    CHECK(readText(marker) == "v1.0.1");

    // A copy from before the version was recorded counts as older, so even the same release is installed over it.
    api.version = L"1.0.1";
    std::filesystem::remove(marker);
    PlayerUpdate unmarked = updateInstalledPlayer(p, vars, nullptr);
    CHECK(unmarked.checked && !unmarked.updated && !unmarked.error.empty());  // it tried (the download fails here)
    CHECK(readText(exe) == "old");
}
