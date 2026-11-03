// Config parsing/validation, file and player lookup, command lines, settings.
#include <cmath>
#include <cstring>
#include <thread>

#include "core/community.h"
#include "core/config.h"
#include "core/discord.h"
#include "core/embedded.h"
#include "core/install.h"
#include "core/library.h"
#include "core/motion.h"
#include "core/process.h"
#include "core/rooms.h"
#include "core/webserver.h"
#include "core/settings.h"
#include "test.h"
// win.h first.
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace fl;

namespace {

std::string minimalConfig(const std::string& games, const std::string& players = "") {
    std::string p = players.empty() ? R"([{ "id": "ruffle", "paths": ["${PLAYERS_DIR}\\ruffle.exe"] },
                                          { "id": "flash", "paths": ["${PLAYERS_DIR}\\flash*.exe"], "args": ["-x"] }])"
                                    : players;
    return R"({ "paths": { "games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                           "data": "${LAUNCHER_DIR}\\data", "flashpoint": ["${LAUNCHER_DIR}\\fp"] },
               "categories": [{ "id": "online" }],
               "players": )" +
           p + R"(, "games": )" + games + "}";
}

bool hasIssue(const LoadResult& r, const std::wstring& where, ConfigIssue::Severity sev) {
    for (const auto& i : r.issues)
        if (i.severity == sev && i.where.find(where) != std::wstring::npos) return true;
    return false;
}

Vars baseVars(const TempDir& dir) {
    Vars v;
    v.set(L"LAUNCHER_DIR", dir.path());
    return v;
}

}  // namespace

TEST(config_loads_shipped_launcher_json) {
    // The real config must stay valid: it is what users run.
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    if (!r.ok()) fprintf(stderr, "%ls\n", formatIssues(r.issues).c_str());
    REQUIRE(r.ok());
    CHECK(r.issues.empty());
    CHECK(r.config.findGame("gunmayhem2") != nullptr);
    CHECK(r.config.findGame("stealthhunter2") != nullptr);
    const GameSpec* gm2 = r.config.findGame("gunmayhem2");
    // Gun Mayhem 2 online is Ruffle Together's lockstep co-op: options on the command line, no relay of ours,
    // and no FlashVars (every PC must run with identical settings).
    bool online = false, host4 = false;
    for (const auto& a : gm2->actions) {
        CHECK(!a.relay.enabled && a.flashvars.empty());
        if (a.id == "online")
            online = !a.joinArgs.empty() && a.args.size() > 1 && a.args[0] == L"--coop-host-relay" &&
                     std::find(a.joinArgs.begin(), a.joinArgs.end(), L"${CODE}") != a.joinArgs.end();
        if (a.id == "host4")
            host4 = std::find(a.args.begin(), a.args.end(), L"--coop-players") != a.args.end() && a.args.back() == L"4";
    }
    CHECK(online && host4);
}

TEST(config_reports_typos_and_bad_references) {
    LoadResult r = loadConfig(minimalConfig(R"([
        { "id": "a", "nmae": "typo", "categories": ["nope"],
          "actions": [{ "id": "play", "swf": "x.swf", "players": ["ghost"] }] },
        { "id": "a", "actions": [] }
    ])"),
                              L"C:\\x");
    CHECK(!r.ok());
    CHECK(hasIssue(r, L"games[0].nmae", ConfigIssue::Severity::Warning));
    CHECK(hasIssue(r, L"games[0].categories", ConfigIssue::Severity::Error));
    CHECK(hasIssue(r, L"games[0].actions[0].players", ConfigIssue::Severity::Error));
    CHECK(hasIssue(r, L"games[1].id", ConfigIssue::Severity::Error));       // duplicate
    CHECK(hasIssue(r, L"games[1].actions", ConfigIssue::Severity::Error));  // no actions
}

TEST(config_rejects_bad_json_with_position) {
    LoadResult r = loadConfig("{ \"games\": [ }", L"C:\\x");
    CHECK(!r.ok());
    REQUIRE(!r.issues.empty());
    CHECK(r.issues[0].message.find(L"line") != std::wstring::npos);
}

TEST(config_text_substitution) {
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf" }] }])"), L"C:\\x");
    REQUIRE(r.ok());
    CHECK(r.config.t("bar.found", {{L"PLAYER", L"Ruffle"}}).find(L"Ruffle") == 0);
}

TEST(library_uses_join_args_when_a_room_code_comes_with_the_launch) {
    TempDir dir("lib-join-args");
    writeText(dir / L"players\\ruffle.exe", "x");
    writeText(dir / L"games\\game.swf", "x");
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "name": "Game",
        "actions": [{ "id": "online", "swf": ["${GAMES_DIR}\\game.swf"],
                      "args": ["--host", "${SERVER}"], "joinArgs": ["--join", "${SERVER}", "--code", "${CODE}"] }] }])"),
                              dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    const GameSpec& g = r.config.games[0];
    ResolvedAction host = lib.resolve(g, g.actions[0], user);
    REQUIRE(host.state == ActionState::Ready && host.args.size() == 3);
    CHECK(host.args[0] == L"--host");
    CHECK(host.args[1] == L"${SERVER}");  // filled in by the launcher at start
    ResolvedAction join = lib.resolve(g, g.actions[0], user, true);
    REQUIRE(join.args.size() == 5);
    CHECK(join.args[0] == L"--join");
    CHECK(join.args[3] == L"${CODE}");
    CHECK(join.args[4] == join.swf);
}

TEST(library_resolves_player_swf_and_args) {
    TempDir dir("lib-resolve");
    writeText(dir / L"players\\ruffle.exe", "x");
    writeText(dir / L"games\\game.swf", "x");
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "name": "Game",
        "actions": [{ "id": "host", "swf": ["${GAMES_DIR}\\missing.swf", "${GAMES_DIR}\\game.swf"],
                      "overrides": { "ruffle": { "args": ["--tcp-connections", "allow"] } },
                      "relay": { "port": 7777 } }] }])"),
                              dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    ResolvedAction a = lib.resolve(r.config.games[0], r.config.games[0].actions[0], user);
    REQUIRE(a.state == ActionState::Ready);
    CHECK(a.player->id == "ruffle");
    CHECK(fs::fileName(a.swf) == L"game.swf");
    REQUIRE(a.args.size() == 3);  // override args, then the SWF appended
    CHECK(a.args[0] == L"--tcp-connections");
    CHECK(a.args[2] == a.swf);
    CHECK(fs::normalize(a.workDir) == fs::normalize(dir / L"games"));
}

TEST(library_falls_back_and_honours_preferred_player) {
    TempDir dir("lib-fallback");
    writeText(dir / L"players\\flash_32_sa.exe", "x");
    writeText(dir / L"players\\flash_9_sa.exe", "x");
    writeText(dir / L"games\\game.swf", "x");
    LoadResult r = loadConfig(
        minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\game.swf" }] }])"), dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    const auto& g = r.config.games[0];
    ResolvedAction a = lib.resolve(g, g.actions[0], user);
    REQUIRE(a.state == ActionState::Ready);
    CHECK(a.player->id == "flash");  // Ruffle missing -> next player
    CHECK(fs::fileName(a.playerExe) == L"flash_32_sa.exe");  // natural order: 32 > 9
    REQUIRE(a.args.size() == 2);
    CHECK(a.args[0] == L"-x");

    writeText(dir / L"players\\ruffle.exe", "x");
    lib.refresh();
    CHECK(lib.resolve(g, g.actions[0], user).player->id == "ruffle");
    user.stats("g").player = "flash";
    CHECK(lib.resolve(g, g.actions[0], user).player->id == "flash");
}

TEST(library_reports_missing_pieces) {
    TempDir dir("lib-missing");
    LoadResult r = loadConfig(
        minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\game.swf" }] }])"), dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    const auto& g = r.config.games[0];
    CHECK(lib.resolve(g, g.actions[0], user).state == ActionState::NoSwf);
    writeText(dir / L"games\\game.swf", "x");
    CHECK(lib.resolve(g, g.actions[0], user).state == ActionState::NoPlayer);
    CHECK(lib.gameState(g, user) == ActionState::NoPlayer);
}

TEST(library_uses_located_file_and_skips_missing_flashpoint) {
    TempDir dir("lib-located");
    writeText(dir / L"players\\ruffle.exe", "x");
    writeText(dir / L"elsewhere\\my game.swf", "x");
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g",
        "actions": [{ "id": "p", "swf": ["${FLASHPOINT}\\game.swf", "${GAMES_DIR}\\game.swf"] }] }])"),
                              dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    CHECK(lib.flashpointDir().empty());
    UserData user;
    const auto& g = r.config.games[0];
    CHECK(lib.resolve(g, g.actions[0], user).state == ActionState::NoSwf);
    user.stats("g").swf["p"] = dir / L"elsewhere\\my game.swf";
    ResolvedAction a = lib.resolve(g, g.actions[0], user);
    CHECK(a.state == ActionState::Ready);
    CHECK(a.userSwf);

    // With a Flashpoint folder present, ${FLASHPOINT} candidates count.
    writeText(dir / L"fp\\FPSoftware\\.keep", "");
    writeText(dir / L"fp\\game.swf", "x");
    lib.refresh();
    CHECK(fs::normalize(lib.flashpointDir()) == fs::normalize(dir / L"fp"));
    user.stats("g").swf.clear();
    CHECK(fs::normalize(lib.resolve(g, g.actions[0], user).swf) == fs::normalize(dir / L"fp\\game.swf"));
}

TEST(library_passes_urls_through) {
    TempDir dir("lib-url");
    writeText(dir / L"players\\ruffle.exe", "x");
    LoadResult r = loadConfig(
        minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "http://localflash/x/game.swf" }] }])"),
        dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    ResolvedAction a = lib.resolve(r.config.games[0], r.config.games[0].actions[0], user);
    CHECK(a.state == ActionState::Ready);
    CHECK(a.swfIsUrl);
    CHECK(a.args.back() == L"http://localflash/x/game.swf");
}

TEST(command_line_quoting) {
    CHECK(sys::quoteArg(L"plain") == L"plain");
    CHECK(sys::quoteArg(L"has space") == L"\"has space\"");
    CHECK(sys::quoteArg(L"") == L"\"\"");
    CHECK(sys::quoteArg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
    CHECK(sys::quoteArg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(sys::buildCommandLine(L"C:\\Program Files\\r.exe", {L"--a", L"C:\\My Games\\g.swf"}) ==
          L"\"C:\\Program Files\\r.exe\" --a \"C:\\My Games\\g.swf\"");
}

TEST(config_player_tv_block) {
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf" }] }])",
                                            R"([{ "id": "ruffle", "paths": ["r.exe"],
                                                  "tv": { "args": ["--fullscreen"], "env": { "RUFFLE_TOGETHER_TV": "1" } } }])"),
                              L"C:\\x");
    REQUIRE(r.ok());
    CHECK(r.issues.empty());
    const PlayerSpec& p = r.config.players[0];
    CHECK(p.tvArgs.size() == 1 && p.tvArgs[0] == L"--fullscreen");
    CHECK(p.tvEnv.size() == 1 && p.tvEnv[0].first == L"RUFFLE_TOGETHER_TV" && p.tvEnv[0].second == L"1");

    LoadResult bad = loadConfig(minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf" }] }])",
                                              R"([{ "id": "ruffle", "paths": ["r.exe"], "tv": { "env": { "X": 1 }, "argz": [] } }])"),
                                L"C:\\x");
    CHECK(hasIssue(bad, L"players[0].tv.env", ConfigIssue::Severity::Error));
    CHECK(hasIssue(bad, L"players[0].tv.argz", ConfigIssue::Severity::Warning));
}

TEST(environment_block_sets_and_replaces) {
    // "A=1", "=C:=C:\\d", "Path=x" as Windows hands them over, NUL-separated and NUL-ended.
    std::wstring current = std::wstring(L"A=1") + L'\0' + L"=C:=C:\\d" + L'\0' + L"Path=x" + L'\0' + L'\0';
    std::wstring block = sys::environmentBlock(current.c_str(), {{L"PATH", L"y"}, {L"TV", L"1"}});
    std::wstring want = std::wstring(L"A=1") + L'\0' + L"=C:=C:\\d" + L'\0' + L"PATH=y" + L'\0' + L"TV=1" + L'\0' + L'\0';
    CHECK(block == want);
}

TEST(settings_round_trip_and_damaged_file) {
    TempDir dir("settings");
    std::wstring path = dir / L"data\\settings.json";
    UserData d;
    d.selectedGame = "gunmayhem2";
    d.category = "cat:online";
    d.width = 1200;
    d.maximized = true;
    GameStats& s = d.stats("gunmayhem2");
    s.favorite = true;
    s.plays = 3;
    s.seconds = 4000;
    s.lastPlayed = 1700000000;
    s.player = "ruffle";
    s.swf["host"] = L"C:\\Games\\\u00E9t\u00E9.swf";
    REQUIRE(saveUserData(d, path));
    UserData back = loadUserData(path);
    CHECK(back.selectedGame == "gunmayhem2");
    CHECK(back.category == "cat:online");
    CHECK(back.width == 1200 && back.maximized);
    const GameStats* b = back.find("gunmayhem2");
    REQUIRE(b != nullptr);
    CHECK(b->favorite && b->plays == 3 && b->seconds == 4000 && b->lastPlayed == 1700000000);
    CHECK(b->player == "ruffle");
    CHECK(b->swf.at("host") == L"C:\\Games\\\u00E9t\u00E9.swf");

    writeText(path, "{ not json");
    UserData fresh = loadUserData(path);
    CHECK(fresh.games.empty());
    CHECK(exists(path + L".bad"));  // kept for inspection, not overwritten
}

TEST(catalog_parsing_and_unsafe_names) {
    std::string json = R"({ "schemaVersion": 1, "files": [
        { "name": "gm2.swf", "game": "gunmayhem2", "path": "games/gunmayhem2/gm2.swf", "size": 10, "sha256": "ABCDEF" },
        { "name": "../evil.swf", "game": "x", "path": "games/x/evil.swf", "size": 1, "sha256": "00" },
        { "name": "sub/evil.swf", "game": "x", "path": "games/x/evil.swf", "size": 1, "sha256": "00" },
        { "name": "abs.swf", "game": "y", "path": "https://example.org/abs.swf", "size": 2, "sha256": "11" }
    ] })";
    std::vector<CatalogFile> files;
    std::wstring err;
    REQUIRE(parseCatalog(json, L"https://raw.githubusercontent.com/o/r/main/catalog.json", files, err));
    REQUIRE(files.size() == 2);  // names that could escape the games folder are dropped
    CHECK(files[0].url == L"https://raw.githubusercontent.com/o/r/main/games/gunmayhem2/gm2.swf");
    CHECK(files[0].sha256 == L"abcdef");
    CHECK(files[0].size == 10);
    CHECK(files[1].url == L"https://example.org/abs.swf");
    CHECK(!parseCatalog("{ nope", L"https://x/catalog.json", files, err));
}

TEST(library_blames_the_missing_file_when_a_player_is_installed) {
    // Ruffle is installed but its file is missing; Flash Player's copy is a web
    // address. The fix is the file, so the state must say so.
    TempDir dir("lib-blame");
    writeText(dir / L"players\\ruffle.exe", "x");
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\g.swf",
        "overrides": { "flash": { "swf": ["http://localflash/g.swf"] } } }] }])"),
                              dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    ResolvedAction a = lib.resolve(r.config.games[0], r.config.games[0].actions[0], user);
    CHECK(a.state == ActionState::NoSwf);
    CHECK(a.player && a.player->id == "ruffle");
}

TEST(room_list_parsing) {
    std::vector<RoomInfo> rooms;
    REQUIRE(parseRoomList("LIST|VK7Q2,2,4,Liam's room;M2NN5,4,4,Friday, late;bad;QX3TB,1,4,", rooms));
    REQUIRE(rooms.size() == 3);  // the malformed "bad" row is skipped
    CHECK(rooms[0].code == L"VK7Q2" && rooms[0].players == 2 && rooms[0].max == 4 && rooms[0].name == L"Liam's room");
    CHECK(rooms[1].name == L"Friday, late");  // the name is the last field and may contain commas
    CHECK(rooms[2].code == L"QX3TB" && rooms[2].name.empty());
    CHECK(parseRoomList("LIST|", rooms) && rooms.empty());  // no rooms open
    CHECK(!parseRoomList("E|unknown game", rooms));
}

TEST(room_list_v2_carries_lobby_details) {
    std::vector<RoomInfo> rooms;
    REQUIRE(parseRoomList("ROOMS|SKT75,2,3,1,ingame,NL,42,1.0#abc,Friday, late;OLD11,1,4,0,,,-1,v1,Old room", rooms));
    REQUIRE(rooms.size() == 2);
    CHECK(rooms[0].code == L"SKT75" && rooms[0].players == 2 && rooms[0].max == 3 && rooms[0].locked);
    CHECK(rooms[0].inGame() && rooms[0].region == L"NL" && rooms[0].hostPing == 42 && rooms[0].version == L"1.0#abc");
    CHECK(rooms[0].name == L"Friday, late");
    CHECK(!rooms[1].locked && !rooms[1].inGame() && rooms[1].hostPing == -1 && rooms[1].name == L"Old room");
}

TEST(config_lobby_block) {
    LoadResult r = loadConfig(minimalConfig(R"([
        { "id": "a", "online": { "id": "gm2", "join": "p", "lobby": { "version": "1.3", "port": 7800 } },
          "actions": [{ "id": "p", "swf": "a.swf" }] },
        { "id": "b", "online": { "id": "sh2", "join": "p", "lobby": { "enabled": false } }, "actions": [{ "id": "p", "swf": "b.swf" }] },
        { "id": "c", "actions": [{ "id": "p", "swf": "c.swf" }] }
    ])"), L"C:\\x");
    REQUIRE(r.ok());
    CHECK(r.config.games[0].lobby.enabled && r.config.games[0].lobby.version == L"1.3" && r.config.games[0].lobby.port == 7800);
    CHECK(!r.config.games[1].lobby.enabled);
    CHECK(!r.config.games[2].lobby.enabled && r.config.games[2].lobby.port == 7790);
    LoadResult bad = loadConfig(minimalConfig(R"([
        { "id": "a", "online": { "join": "p", "lobby": {} }, "actions": [{ "id": "p", "swf": "a.swf" }] }
    ])"), L"C:\\x");
    CHECK(!bad.ok());
}

TEST(html5_action_resolves_to_a_browser) {
    TempDir dir("lib-web");
    writeText(dir / L"players\\edge.exe", "x");
    writeText(dir / L"players\\ruffle.exe", "x");
    writeText(dir / L"mod\\server\\server.js", "x");
    writeText(dir / L"node\\node.exe", "x");
    std::string players = R"([{ "id": "ruffle", "paths": ["${PLAYERS_DIR}\\ruffle.exe"] },
        { "id": "edge", "kind": "browser", "paths": ["${PLAYERS_DIR}\\edge.exe"], "args": ["--app=${URL}"] }])";
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "pb2", "actions": [
        { "id": "host", "file": "${LAUNCHER_DIR}\\mod\\server\\server.js",
          "server": { "name": "Node.js", "exe": ["${FILE_DIR}\\..\\..\\node\\node.exe"], "args": ["server\\server.js", "2053"],
                      "cwd": "${FILE_DIR}\\..", "url": "http://localhost:2053/", "share": "https://[a-z]+\\.example" } },
        { "id": "join", "joinLink": true },
        { "id": "page", "page": "${LAUNCHER_DIR}\\web\\index.html" } ] }])", players),
                              dir.path());
    if (!r.ok()) fprintf(stderr, "%ls\n", formatIssues(r.issues).c_str());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    const auto& g = r.config.games[0];
    ResolvedAction host = lib.resolve(g, g.actions[0], user);
    REQUIRE(host.state == ActionState::Ready);
    CHECK(host.web && host.player->id == "edge");  // never Ruffle for an HTML5 action
    CHECK(fs::fileName(host.serverExe) == L"node.exe");
    CHECK(fs::normalize(host.serverCwd) == fs::normalize(dir / L"mod"));
    CHECK(host.url == L"http://localhost:2053/");
    REQUIRE(host.args.size() == 1);
    CHECK(host.args[0] == L"--app=${URL}");  // filled in when the game starts
    CHECK(lib.resolve(g, g.actions[1], user).state == ActionState::Ready);
    CHECK(lib.resolve(g, g.actions[2], user).state == ActionState::NoSwf);  // no index.html yet
    writeText(dir / L"web\\index.html", "<p>hi</p>");
    ResolvedAction page = lib.resolve(g, g.actions[2], user);
    CHECK(page.state == ActionState::Ready && page.pageLocal);

    DeleteFileW((dir / L"node\\node.exe").c_str());
    ResolvedAction noNode = lib.resolve(g, g.actions[0], user);
    CHECK(noNode.state == ActionState::NoRuntime);
}

TEST(config_rejects_html5_action_with_flash_player) {
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "actions": [ { "id": "p", "page": "https://x.example/", "players": ["ruffle"] } ] }])",
                                            R"([{ "id": "ruffle", "paths": ["r.exe"] }, { "id": "edge", "kind": "browser", "paths": ["e.exe"] }])"),
                              L"C:\\x");
    CHECK(!r.ok());
}

static std::string httpGet(int port, const std::string& path, const std::string& extra = "") {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\n" + extra + "\r\n";
        send(s, req.data(), static_cast<int>(req.size()), 0);
        char buf[4096];
        int n;
        while ((n = recv(s, buf, sizeof buf, 0)) > 0) out.append(buf, n);
    }
    closesocket(s);
    WSACleanup();
    return out;
}

TEST(web_server_serves_only_its_folder) {
    TempDir dir("web");
    writeText(dir / L"game\\index.html", "<h1>game</h1>");
    writeText(dir / L"game\\data\\level 1.json", "0123456789");
    writeText(dir / L"secret.txt", "no");
    WebServer web;
    std::wstring err;
    REQUIRE(web.start(dir / L"game", 0, err));
    std::string page = httpGet(web.port(), "/");
    CHECK(page.find("200 OK") != std::string::npos && page.find("text/html") != std::string::npos &&
          page.find("<h1>game</h1>") != std::string::npos);
    CHECK(httpGet(web.port(), "/data/level%201.json").find("0123456789") != std::string::npos);
    std::string range = httpGet(web.port(), "/data/level%201.json", "Range: bytes=2-4\r\n");
    CHECK(range.find("206 Partial") != std::string::npos && range.find("\r\n\r\n234") != std::string::npos);
    CHECK(httpGet(web.port(), "/../secret.txt").find("404") != std::string::npos);
    CHECK(httpGet(web.port(), "/%2e%2e/secret.txt").find("404") != std::string::npos);
    CHECK(httpGet(web.port(), "/..%5csecret.txt").find("404") != std::string::npos);
    web.stop();
    CHECK(mimeType(L"x.wasm") == "application/wasm");
    CHECK(mimeType(L"x.JS").rfind("text/javascript", 0) == 0);
}

TEST(motion_curves_start_and_end_on_their_values) {
    using namespace fl::motion;
    CHECK(smoothstep(0) == 0 && smoothstep(1) == 1 && smoothstep(-3) == 0 && smoothstep(9) == 1);
    CHECK(easeOutCubic(0) == 0 && easeOutCubic(1) == 1 && easeOutCubic(0.5f) > 0.5f);
    CHECK(singleBounce(0) == 0 && std::abs(singleBounce(1) - 1) < 1e-6f);
    CHECK(std::abs(singleBounce(0.7f) - 1.25f) < 1e-6f);  // the overshoot peak
    for (int i = 0; i < 4; ++i)
        for (uint64_t ms = 0; ms < 3200; ms += 100) {
            float w = wave(ms, i);
            CHECK(w >= 0 && w <= 1);
        }
    CHECK(wave(0, 0) != wave(0, 1));  // neighbours are out of step
}

TEST(scramble_keeps_shape_and_locks_left_to_right) {
    using namespace fl::motion;
    std::wstring s = scramble(L"Gun Mayhem 2", 0, 7);
    REQUIRE(s.size() == 12);
    CHECK(s[3] == L' ' && s[10] == L' ');
    CHECK(s[0] >= L'A' && s[0] <= L'Z' && s[1] >= L'a' && s[1] <= L'z' && s[11] >= L'0' && s[11] <= L'9');
    CHECK(scramble(L"Gun Mayhem 2", 1, 7) == L"Gun Mayhem 2");
    CHECK(scramble(L"VK7Q2", 0.6f, 3).substr(0, 3) == L"VK7");
    CHECK(scramble(L"VK7Q2", 0, 1) != scramble(L"VK7Q2", 0, 2));
}

TEST(online_first_action_options_parse) {
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "online": { "id": "g1", "join": "online" }, "actions": [
        { "id": "online", "swf": "a.swf", "flashvars": { "online": "1", "name": "${PLAYER_NAME}" } },
        { "id": "host", "swf": "a.swf", "more": true },
        { "id": "play", "swf": "b.swf", "edition": "original" } ] }])"),
                              L"C:\\x");
    if (!r.ok()) fprintf(stderr, "%ls\n", formatIssues(r.issues).c_str());
    REQUIRE(r.ok());
    const auto& a = r.config.games[0].actions;
    REQUIRE(a[0].flashvars.size() == 2);
    CHECK(a[0].flashvars[0].first == L"online" && a[0].flashvars[1].second == L"${PLAYER_NAME}");
    CHECK(!a[0].more && a[1].more && !a[1].original && a[2].original);
    CHECK(r.config.games[0].joinAction == "online");
    for (const auto& i : r.issues) CHECK(i.message.find(L"unknown") == std::wstring::npos);  // PLAYER_NAME is known
}

TEST(category_icons_and_discord_are_checked) {
    std::string base = minimalConfig(R"([{ "id": "g", "actions": [ { "id": "p", "swf": "a.swf" } ] }])");
    // `cats` replaces the helper's own category list; `extra` goes in front.
    auto with = [&](const std::string& cats, const std::string& extra) {
        std::string j = base;
        std::string mine = R"("categories": [{ "id": "online" }])";
        j.replace(j.find(mine), mine.size(), R"("categories": )" + cats);
        j.insert(j.find('{') + 1, extra);
        return loadConfig(j, L"C:\\x");
    };
    LoadResult ok = with(R"([ { "id": "online", "icon": "E774" } ])", R"("discord": { "clientId": "1234567890" },)");
    if (!ok.ok()) fprintf(stderr, "%ls\n", formatIssues(ok.issues).c_str());
    REQUIRE(ok.ok());
    CHECK(ok.config.categories[0].icon == std::wstring(1, static_cast<wchar_t>(0xE774)));
    CHECK(ok.config.discord.clientId == L"1234567890");
    CHECK(!with(R"([ { "id": "online", "icon": "star" } ])", "").ok());
    CHECK(!with(R"([ { "id": "online" } ])", R"("discord": { "clientId": "abc" },)").ok());
}

TEST(discord_presence_messages) {
    std::string hello = discordHandshake(L"42");
    CHECK(hello.find("\"v\":1") != std::string::npos && hello.find("\"client_id\":\"42\"") != std::string::npos);
    Presence p;
    p.details = L"Playing Gun Mayhem 2";
    p.state = L"Room VK7Q2";
    p.start = 1700000000;
    std::string set = discordActivity(&p, 99, 3);
    CHECK(set.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos);
    CHECK(set.find("\"pid\":99") != std::string::npos && set.find("\"nonce\":\"3\"") != std::string::npos);
    CHECK(set.find("\"details\":\"Playing Gun Mayhem 2\"") != std::string::npos);
    CHECK(set.find("\"start\":1700000000") != std::string::npos);
    std::string clear = discordActivity(nullptr, 99, 4);
    CHECK(clear.find("\"activity\":null") != std::string::npos);
}

TEST(embedded_names_match_the_resource_script) {
    // CMakeLists.txt names the resources the same way.
    CHECK(embeddedName(L"launcher.json") == L"F_LAUNCHER_JSON");
    CHECK(embeddedName(L"covers\\gunmayhem2.png") == L"F_COVERS_GUNMAYHEM2_PNG");
    CHECK(embeddedName(L"covers/Pb2.jpg") == L"F_COVERS_PB2_JPG");
    std::string none;
    CHECK(!readEmbedded(L"covers\\nope.png", none));  // the test exe has no resources
}

TEST(shipped_config_keeps_stealth_hunter_2_on_ruffle) {
    // Flash Player is never an option for Stealth Hunter 2: its online actions are Ruffle
    // Together's own options, which only that player understands.
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    REQUIRE(r.ok());
    const GameSpec* g = r.config.findGame("stealthhunter2");
    REQUIRE(g != nullptr);
    for (const auto& a : g->actions) {
        bool coop = false;
        for (const auto& arg : a.args) coop = coop || arg == L"--coop-keys";
        CHECK(coop || a.original || a.offline);
        CHECK(!a.overrides.count("flash"));
    }
    CHECK(r.config.updates.enabled());
}

TEST(shipped_config_gives_dad_n_me_a_friendly_fire_host_option) {
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    REQUIRE(r.ok());
    const GameSpec* g = r.config.findGame("dadnme");
    REQUIRE(g != nullptr);
    REQUIRE(g->settings.size() == 1);
    CHECK(g->settings[0].id == "friendlyfire");
    CHECK(g->settings[0].type == "checkbox" && g->settings[0].scope == "host");
    CHECK(g->settings[0].since == L"1.0.3.7");
    CHECK(g->settings[0].args.size() == 2 && g->settings[0].args[1] == L"friendlyfire=1");
}

TEST(shipped_config_gives_zombotron_four_host_options_and_up_to_four_players) {
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    REQUIRE(r.ok());
    const GameSpec* g = r.config.findGame("zombotron");
    REQUIRE(g != nullptr);
    CHECK(g->maxPlayers == 4);
    REQUIRE(g->settings.size() == 4);
    CHECK(g->settings[0].id == "camera" && g->settings[0].type == "choice" && g->settings[0].choices.size() == 2);
    CHECK(g->settings[1].id == "zoom" && g->settings[1].choices.size() == 3);
    CHECK(g->settings[2].id == "friendlyfire" && g->settings[3].id == "bump");
}

TEST(catalog_settings_accept_the_live_dad_n_me_camera_choice) {
    std::map<std::string, std::vector<Setting>> out;
    parseCatalogSettings(R"({ "files": [], "settings": { "dadnme": [
      { "id": "friendlyfire", "type": "checkbox", "scope": "host", "label": "Friendly fire", "since": "1.0.3.7", "args": ["--coop-option", "friendlyfire=1"] },
      { "id": "camera", "type": "choice", "scope": "host", "label": "Camera", "since": "1.0.3.7", "default": "shared", "choices": [
        { "id": "shared", "label": "Shared screen", "args": [] },
        { "id": "follow", "label": "Follow each player", "args": ["--coop-option", "camera=follow"] },
        { "id": "zoom", "label": "Follow each player, zoomed in", "args": ["--coop-option", "camera=zoom"] } ] } ] } })", out);
    REQUIRE(out["dadnme"].size() == 2);
    const Setting& cam = out["dadnme"][1];
    CHECK(cam.type == "choice" && cam.def == "shared" && cam.choices.size() == 3);
    CHECK(cam.choices[0].args.empty());
    CHECK(cam.choices[2].args.size() == 2 && cam.choices[2].args[1] == L"camera=zoom");
}

TEST(shipped_config_keeps_chaos_faction_2_to_six_players_on_its_key_map) {
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    REQUIRE(r.ok());
    const GameSpec* g = r.config.findGame("chaosfaction2");
    REQUIRE(g != nullptr);
    CHECK(g->maxPlayers == 6);
    int rooms = 0;
    for (const auto& a : g->actions) {
        if (a.id.rfind("room", 0) == 0 || (a.id.rfind("host", 0) == 0 && a.id.size() > 4)) ++rooms;
        bool keyed = false;
        for (size_t i = 0; i + 1 < a.args.size(); ++i)
            if (a.args[i] == L"--coop-keys") keyed = a.args[i + 1] == L"chaos-faction-2";
        if (!a.args.empty()) CHECK(keyed);
    }
    CHECK(rooms == 8);  // a room and an IP host for 3, 4, 5 and 6 players
}

TEST(rate_limit_message_says_how_long_is_left) {
    CHECK(rateLimitMessage(20).find(L"about a minute") != std::wstring::npos);
    CHECK(rateLimitMessage(61).find(L"2 minutes") != std::wstring::npos);
    CHECK(rateLimitMessage(23 * 60).find(L"23 minutes") != std::wstring::npos);
    CHECK(rateLimitMessage(3600).find(L"1 h 0 min") != std::wstring::npos);
    CHECK(rateLimitMessage(-5).find(L"about a minute") != std::wstring::npos);
    CHECK(isRateLimitMessage(rateLimitMessage(90)) && !isRateLimitMessage(L"api.github.com answered HTTP 403"));
}

TEST(catalog_settings_checkboxes_and_choices) {
    std::map<std::string, std::vector<Setting>> out;
    parseCatalogSettings(R"({ "files": [], "settings": {
        "dadnme": [
          { "id": "ff", "label": "Friendly fire", "since": "1.0.3.7", "args": ["--coop-option", "friendlyfire=1"] },
          { "id": "diff", "type": "choice", "label": "Difficulty", "default": "hard", "scope": "all",
            "choices": [ { "id": "easy", "label": "Easy", "args": ["--coop-option", "d=0"] },
                         { "id": "hard", "label": "Hard", "args": ["--coop-option", "d=2"] } ] },
          { "id": "bad", "type": "choice", "label": "One choice", "choices": [ { "id": "a", "label": "A" } ] },
          { "id": "bad2", "label": "No args" }
        ],
        "other": 5 } })", out);
    REQUIRE(out.count("dadnme") == 1);
    REQUIRE(out["dadnme"].size() == 2);  // the two broken ones are left out
    CHECK(out["dadnme"][0].scope == "host");
    CHECK(out["dadnme"][1].type == "choice" && out["dadnme"][1].choices.size() == 2 && out["dadnme"][1].def == "hard");
    CHECK(out.count("other") == 0);
    std::map<std::string, std::vector<Setting>> none;
    parseCatalogSettings("not json", none);
    CHECK(none.empty());
}

TEST(rooms_a_server_without_the_game_is_told_apart) {
    // The relay answers "E|Unknown game ..." until the mod's mods-info file is merged.
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(l, 4) == 0);
    int len = sizeof a;
    getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
    std::thread server([l] {
        for (const char* reply : {"E|Unknown game 'tw' on this server", "LIST|ABCDE,1,4,Room"}) {
            SOCKET c = accept(l, nullptr, nullptr);
            char buf[256];
            recv(c, buf, sizeof buf, 0);
            send(c, reply, static_cast<int>(strlen(reply)) + 1, 0);  // with its NUL
            closesocket(c);
        }
    });
    std::wstring addr = L"127.0.0.1:" + std::to_wstring(ntohs(a.sin_port)), err;
    std::vector<RoomInfo> rooms;
    bool rejected = false;
    CHECK(!listRooms(addr, "tw", rooms, err, 3000, &rejected));
    CHECK(rejected && err.find(L"Unknown game") != std::wstring::npos);
    CHECK(listRooms(addr, "gm2", rooms, err, 3000, &rejected));
    CHECK(!rejected && rooms.size() == 1);
    server.join();
    closesocket(l);
    WSACleanup();
}

// ---- community mods from mods-info (core/community.cpp, readCommunityMod) -------------------------------------------
// The same rules as the app's src-tauri/src/launcher/community.rs, whose tests try every refusal; here, that the mod
// commands accept an approved record, refuse one that reaches further, and read the app's saved copy.

namespace {

std::string communityRecord(const std::string& extraMod = "", const std::string& url = "") {
    std::string u = url.empty() ? "https://github.com/someone/lab-coop/releases/download/v1.2.0/LabCoop-1.2.0.zip" : url;
    return R"({ "id": "lab-coop", "repo": "someone/lab-coop", "tag": "v1.2.0", "version": "1.2.0", "asset": "LabCoop-1.2.0.zip",
      "url": ")" + u + R"(", "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "author": "someone",
      "mod": { "id": "lab-coop", )" + extraMod + R"(
        "game": { "name": "Lab Game", "executable": "LabGame.exe", "validationFiles": ["${GAME_EXE}"],
                  "detection": [{ "type": "steam", "appId": "123456", "installDir": "Lab Game" }] },
        "install": { "targetDir": "${GAME_DIR}/BepInEx/plugins/LabCoop" },
        "components": [{ "id": "mod", "name": "LabCoop", "source": "LabCoop.dll", "target": "${INSTALL_DIR}", "required": true }],
        "launch": { "uri": "steam://rungameid/123456" } } })";
}

Config shippedConfig() {
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    return loadConfigFile(path).config;
}

}  // namespace

TEST(community_record_becomes_a_pinned_mod) {
    Config cfg = shippedConfig();
    GameSpec g;
    std::wstring why;
    REQUIRE(readCommunityMod(communityRecord(), cfg, false, g, why));
    CHECK(g.id == "lab-coop");
    CHECK(g.name == L"Lab Game");
    REQUIRE(g.mod != nullptr);
    CHECK(g.mod->package.version == L"1.2.0");
    CHECK(g.mod->package.sha256.size() == 64);
    CHECK(g.mod->mod.publisher == L"someone");
    CHECK(g.categories.size() == 1 && g.categories[0] == "community");
}

TEST(community_record_that_reaches_further_is_refused) {
    Config cfg = shippedConfig();
    GameSpec g;
    std::wstring why;
    CHECK(!readCommunityMod(communityRecord(R"("package": { "github": "x/y" },)"), cfg, false, g, why));
    CHECK(why.find(L"not allowed") != std::wstring::npos);
    CHECK(!readCommunityMod(communityRecord("", "https://evil.example/LabCoop-1.2.0.zip"), cfg, false, g, why));
    CHECK(why.find(L"url must be") != std::wstring::npos);
    // A loopback release only when mods-info is loopback too (the tests).
    std::string local = "http://127.0.0.1:9/someone/lab-coop/releases/download/v1.2.0/LabCoop-1.2.0.zip";
    CHECK(!readCommunityMod(communityRecord("", local), cfg, false, g, why));
    CHECK(readCommunityMod(communityRecord("", local), cfg, true, g, why));
}

TEST(community_mods_come_from_the_apps_saved_copy) {
    TempDir dir("community");
    std::wstring path = fs::join(dir.path(), L"community.json");
    CommunityCache c;
    c.fetchedAt = 1700000000;
    c.raw = {communityRecord(), R"({ "id": "broken" })"};
    REQUIRE(saveCommunityCache(path, c));
    CommunityCache back;
    REQUIRE(loadCommunityCache(path, back));
    CHECK(back.raw.size() == 2 && back.fetchedAt == 1700000000);
    Config cfg = shippedConfig();
    size_t before = cfg.games.size();
    CHECK(mergeCommunity(cfg, back) == 1);  // the broken one is left out
    CHECK(cfg.games.size() == before + 1);
    CHECK(cfg.findGame("lab-coop") != nullptr);
}
