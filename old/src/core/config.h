// Launcher configuration: every game, player, path, color and visible string
// lives in launcher.json and is loaded into these structs. Defaults here are
// the documented defaults in CONFIG.md; keep both in sync.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/mod/spec.h"
#include "vars.h"

namespace fl {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    unsigned long ref() const { return r | (g << 8) | (b << 16); }  // COLORREF
};
bool parseColor(const std::wstring& text, Color& out);

struct ThemeColors {
    Color background, surface, surfaceHover, sidebar, sidebarText, sidebarTextMuted;
    Color primary, primaryHover, onPrimary, text, textMuted, border, focus;
    Color success, warning, error, online;
};
ThemeColors darkPalette();
ThemeColors lightPalette();
// Name/value pairs in declaration order, for docs and --dump-defaults.
std::vector<std::pair<const char*, Color*>> colorFields(ThemeColors& c);

struct WindowSpec {
    int width = 1180, height = 720, minWidth = 720, minHeight = 520;
};

struct Branding {
    std::wstring title = L"Multiplayer Everything";
    std::wstring subtitle = L"Multiplayer Everything Launcher";
    std::wstring website = L"https://github.com/multiplayereverything/multiplayer-mods-launcher";
    std::wstring windowTitle = L"${TITLE} ${SUBTITLE}";
    std::wstring theme = L"dark";  // dark | light
    ThemeColors colors;            // palette for `theme` with overrides applied
    std::wstring font = L"Segoe UI";
    // Titles, card names and buttons. Bahnschrift ships with Windows 10+; its
    // condensed scoreboard look suits arcade games and fits long titles.
    std::wstring displayFont = L"Bahnschrift";
    float fontSize = 10.0f;  // points
    // P1-P4, the colors the games give their players. Used only to mark player slots.
    std::vector<Color> playerColors = {{0xE5, 0x48, 0x4D, 255}, {0x3E, 0x7B, 0xFA, 255}, {0x2E, 0xBD, 0x6B, 255},
                                       {0xF5, 0xB8, 0x3D, 255}};
    WindowSpec window;
};

struct Paths {
    std::wstring games = L"${LAUNCHER_DIR}\\games";
    std::wstring players = L"${LAUNCHER_DIR}\\players";
    std::wstring data = L"${env:LOCALAPPDATA}\\MultiplayerEverything\\MultiplayerEverythingLauncher";
    // First folder that contains FPSoftware becomes ${FLASHPOINT}.
    std::vector<std::wstring> flashpoint = {L"C:\\Flashpoint", L"D:\\Flashpoint", L"${folder:Profile}\\Flashpoint"};
};

struct Rooms {
    // mods-info servers.json: the central server's current address.
    std::wstring servers;
    std::wstring server;  // "host:port" used when servers.json can't be read
    int refreshSeconds = 30;
};

struct Downloads {
    // catalog.json of the flash-games repository; empty = no game downloads.
    std::wstring catalog;
    // Folder of mods-info (mods/<id>.json: newest version and download of each PC mod).
    // Empty: the folder of rooms.servers.
    std::wstring modsInfo;
    // mods-info as one zip, which the app reads (src-tauri/src/launcher/mods_zip.rs); this exe only checks the address.
    std::wstring modsInfoZip;
    // The website's games.json: stage, quality grade, version and what stays in sync, shown next to each game.
    // Empty: the launcher never asks the website.
    std::wstring site = L"https://www.multiplayereverything.com/games.json";
};

struct Behavior {
    bool minimizeOnLaunch = false;
    bool stopRelayWithGame = true;
    int quickExitSeconds = 4;  // a player that exits faster than this with an error is reported
};

// Where to get a player that isn't installed. Either a GitHub repository whose
// latest release has an asset matching `asset`, or a fixed `url`.
struct InstallSpec {
    std::string github;         // "owner/repo"
    std::wstring api;           // GitHub API address; empty = https://api.github.com (tests use a local one)
    std::wstring asset;         // file name glob, e.g. "*windows-x86_64.zip"
    std::wstring url, sha256;   // fixed download instead of GitHub (sha256 optional)
    std::wstring dir;           // unpack here; replaced on reinstall
    std::wstring exe;           // the player inside the archive, e.g. "ruffle.exe"
    bool enabled() const { return !github.empty() || !url.empty(); }
};

struct PlayerSpec {
    std::string id;
    // "flash" plays .swf files; "browser" (Edge, Chrome) opens HTML5 games at ${URL}; "window" is the Tauri
    // launcher's own window (no paths: always there, and this launcher can't open it).
    std::string kind = "flash";
    bool isWeb() const { return kind == "browser" || kind == "window"; }
    std::wstring name, website;
    InstallSpec install;
    // Candidates, first match wins: absolute paths (a '*' is allowed in the file
    // name), or a bare file name looked up on PATH.
    std::vector<std::wstring> paths;
    std::vector<std::wstring> args;  // ${SWF} is appended when no arg mentions it
    // How this player passes a value into the game, e.g. "-P${KEY}=${VALUE}"
    // (Ruffle's FlashVars). Empty: the player can't, and the value is skipped.
    std::wstring flashvarArg;
    // Ruffle Together's host option that picks the save a co-op game starts from ("--coop-save solo|new"),
    // and the first release that knows it. An older player fails on an option it doesn't know, so the
    // launcher adds the option only from that version on. Empty arg: the player has no such option.
    std::wstring coopSaveArg, coopSaveSince;
    // TV mode (--launch --tv, the Tauri window's full-screen controller view): added to the command line and
    // the player's environment. An option an older player would refuse belongs in the environment, which old
    // builds ignore.
    std::vector<std::wstring> tvArgs;
    std::vector<std::pair<std::wstring, std::wstring>> tvEnv;
};

struct PlayerOverride {
    std::vector<std::wstring> swf;
    std::vector<std::wstring> args;
    bool hasSwf = false, hasArgs = false;
};

struct RelaySpec {
    bool enabled = false;
    int port = 7777;
};

// A program an HTML5 action runs next to the game, such as a mod's co-op server:
// started hidden, its output read for links to share, stopped with the game.
struct ServerSpec {
    std::wstring name = L"Server";  // shown when the program is missing ("Node.js")
    std::wstring website;
    std::vector<std::wstring> exe;   // candidates, like a player's paths
    std::vector<std::wstring> args;
    std::wstring cwd;                // default: the folder of the action's file
    std::wstring url;                // the game's address once the server is up
    std::wstring share, lan;         // regexes: links in the output to show friends
    std::string runtime;             // a runtimes[] id: its paths are tried before exe
    // Regexes for the lines it prints: the room code a host shows (group 1), "ready to open", and the reason it failed.
    std::wstring linesCode, linesReady, linesError;
    bool enabled() const { return !exe.empty() || !runtime.empty(); }
    bool joinsByCode() const {
        for (const auto& a : args)
            if (a.find(L"${CODE}") != std::wstring::npos) return true;
        return false;
    }
};

// A program a game's server runs on (Node.js): found like a player, installed like one when missing.
struct RuntimeSpec {
    std::string id;
    std::wstring name, website;
    std::vector<std::wstring> paths;
    InstallSpec install;
};

struct ActionSpec {
    std::string id;
    std::wstring label, description, hint;
    // Flash: the .swf candidates (http(s):// URLs are passed as-is). HTML5: any
    // file the game needs ("file" in launcher.json), such as the server script.
    std::vector<std::wstring> swf;
    // HTML5: the page to open, a local .html (served by the launcher) or a URL.
    std::vector<std::wstring> page;
    ServerSpec server;
    bool joinLink = false;  // HTML5: open the http(s) link on the clipboard (a friend's game)
    int windowWidth = 0, windowHeight = 0;  // the size it asks for in the launcher's own window (0: default)
    std::vector<std::wstring> args;     // added after the player's own args
    // Used instead of `args` when a room code comes with the launch (Rooms list, join by code).
    // ${CODE}, ${SERVER} (the central server), ${ADDRESS} (host:port copied to the clipboard) and
    // ${PLAYER_NAME} are filled in when the game starts.
    std::vector<std::wstring> joinArgs;
    std::vector<std::string> players;  // allowed players in order of preference; empty = all
    std::map<std::string, PlayerOverride> overrides;  // per player id
    RelaySpec relay;                    // "host" actions start the built-in relay
    // Values handed to the game at start through the player's flashvarArg, such
    // as {"online": "1", "name": "${PLAYER_NAME}"}. A mod that doesn't read them ignores them.
    std::vector<std::pair<std::wstring, std::wstring>> flashvars;
    // PC game mods: "install" | "update" | "repair" | "uninstall" | "launch". Such an
    // action is made by the launcher, has no file of its own, and its label comes from the texts.
    std::string modOp;
    bool more = false;      // listed under "Other ways to connect" (direct IP, LAN)
    bool offline = false;   // solo / one keyboard: shown only when Settings asks for offline play
    std::wstring icon;      // one Segoe Fluent Icons glyph before the label ("icon": "E774")
    bool original = false;  // "edition": "original": the unmodded game, shown only when Settings asks
    bool isWeb() const { return !page.empty() || server.enabled() || joinLink; }
    bool isMod() const { return !modOp.empty(); }
    // A button that opens an https page (a mod made by someone else, installed with its own setup).
    // It needs no player and no file.
    std::wstring link;
    bool isLink() const { return !link.empty(); }
};

// The launcher's own lobby (core/lobby.h) for a mod that talks to the localhost
// bridge instead of the server: rooms, ready-up and start happen in the launcher.
struct LobbySpec {
    bool enabled = false;
    std::wstring version = L"1";  // the mod's version; the game file's hash is added to it
    int port = 7790;              // Direct IP hosting listens here
};

// A link on a game's guide page (a button at the bottom); always https://.
struct GuideLink {
    std::wstring label, url;
};
constexpr size_t kGuideMaxLinks = 4;

// What a game's guide page says, written in launcher.json. Anything left out is filled in from a default for
// that kind of game (core/guide.h), so a game needs no `guide` at all.
struct GuideSpec {
    std::vector<std::wstring> needs, steps, tips;
    std::vector<std::pair<std::wstring, std::wstring>> keys;  // key -> what it does
    std::vector<GuideLink> links;
};

// One game setting the player can change in the launcher ("Friendly fire", "Difficulty"). The launcher draws a
// checkbox or a dropdown for it and adds its `args` to the command line. Settings come from the game catalog
// (flash-games catalog.json, "settings") so a game can add one without a launcher release; the ones in
// launcher.json are the fallback when the catalog has none for that game.
struct SettingChoice {
    std::string id;
    std::wstring label;
    std::vector<std::wstring> args;
};
struct Setting {
    std::string id;
    std::string type = "checkbox";  // "checkbox" (args added when ticked) or "choice" (the picked choice's args)
    std::string scope = "host";     // which actions get the args: "host" (rooms you host), "join", or "all"
    std::wstring label;
    // The first Ruffle Together release that knows the option; before it the setting is disabled, because an
    // unknown option stops the player from starting.
    std::wstring since;
    std::vector<std::wstring> args;       // checkbox
    std::vector<SettingChoice> choices;   // choice; the first one is the default unless `def` names another
    std::string def;                      // checkbox: "on" starts ticked; choice: the default choice's id
};
inline constexpr size_t kMaxSettings = 4;

namespace json {
class Value;
}
struct ConfigIssue;
// Reads a "settings" array (launcher.json or the catalog). Bad entries are reported and left out.
std::vector<Setting> readSettings(const json::Value& list, const std::wstring& where, std::vector<ConfigIssue>& issues);

struct GameSpec {
    std::string id;
    std::wstring name, description, playerCount, controls, image;
    // Which part of the cover stays in view when it is cropped: 0 top, 1 bottom.
    float imageFocus = 0.5f;
    std::string author;     // someone else's mod in launcher.json: who made it
    std::string authorUrl;
    int maxPlayers = 0;  // player slots to light; 0 = highest number in playerCount
    // Rooms on the central server: the mod's id there ("gm2") and the action
    // that joins a room. Empty id: the game has no online rooms.
    std::string onlineId, joinAction;
    std::string siteId;  // the game's id on the website when it differs from `id` ("baldi" for "baldi-together")
    LobbySpec lobby;
    GuideSpec guide;
    std::vector<std::string> categories;
    bool comingSoon = false;  // listed with "Coming soon" and no buttons (its mod isn't built yet)
    std::vector<Setting> settings;  // fallback; the catalog's settings for this game win
    // A PC game mod: installed into the game's folder instead of played in Ruffle.
    std::shared_ptr<const mod::Spec> mod;
    Color color{0x4F, 0x8B, 0xFF, 255}, colorEnd{0x9C, 0x6B, 0xFF, 255};
    std::vector<ActionSpec> actions;
};

struct Category {
    std::string id;
    std::wstring name;
    std::wstring icon;  // one Segoe Fluent Icons glyph, written as hex in launcher.json ("E774")
    // Categories with the same group sit together in the sidebar; a line separates groups.
    std::string group;
};

// Discord Rich Presence ("Playing Gun Mayhem 2"): the application id from
// discord.com/developers. Empty: the launcher never talks to Discord.
struct Discord {
    std::wstring clientId;
};

// The launcher updating itself: the newest release of its own GitHub
// repository, the file `asset` in it (checked against GitHub's SHA-256).
struct Updates {
    InstallSpec source;  // github, asset, api
    // The app that replaces this window: its installer in the same releases (a file-name pattern) and where it
    // installs. With both set, this launcher hands over: it opens the app when it is installed, else installs it.
    std::wstring app, appExe;
    bool enabled() const { return !source.github.empty() && !source.asset.empty(); }
    bool handsOver() const { return enabled() && !app.empty() && !appExe.empty(); }
};

struct Config {
    int schemaVersion = 1;
    Branding branding;
    Paths paths;
    Behavior behavior;
    Downloads downloads;
    Rooms rooms;
    Discord discord;
    Updates updates;
    std::vector<PlayerSpec> players;
    std::vector<RuntimeSpec> runtimes;
    std::vector<Category> categories;
    std::vector<GameSpec> games;
    std::map<std::string, std::wstring> text;  // UI strings, defaults + overrides
    std::wstring configDir;                     // folder of launcher.json

    // UI text with ${VAR} substitution from `vars` (pairs of name, value).
    std::wstring t(const std::string& key,
                   std::initializer_list<std::pair<std::wstring_view, std::wstring_view>> vars = {}) const;
    // Folder of mods-info: downloads.modsInfo, else the folder of rooms.servers.
    std::wstring modsInfoBase() const;
    const GameSpec* findGame(std::string_view id) const;
    const PlayerSpec* findPlayer(std::string_view id) const;
    const RuntimeSpec* findRuntime(std::string_view id) const;
    const Category* findCategory(std::string_view id) const;
};

const std::vector<std::pair<std::string, std::wstring>>& defaultTexts();

struct ConfigIssue {
    enum class Severity { Warning, Error } severity;
    std::wstring where, message;
};

struct LoadResult {
    Config config;
    std::vector<ConfigIssue> issues;
    bool ok() const;
};

LoadResult loadConfig(std::string_view jsonText, const std::wstring& configDir);
LoadResult loadConfigFile(const std::wstring& path);
// Cross-references: unique ids, known players and categories, variables.
void validateConfig(const Config& cfg, std::vector<ConfigIssue>& issues);
std::wstring formatIssues(const std::vector<ConfigIssue>& issues);

// A community mod the owner approved into mods-info (records/<id>.json, docs/COMMUNITY_MODS.md): its pinned release
// and a launcher.json "mod" recipe limited to what can't reach outside the game folder. It is checked by those rules
// and then like any launcher.json mod; a record that fails is refused whole. An id launcher.json already has is
// refused too. `localRelease`: the release may come from a local test server (only when mods-info is one).
bool readCommunityMod(std::string_view json, const Config& cfg, bool localRelease, GameSpec& out, std::wstring& error);
// One of the team's own mods (records/<id>.json): a launcher.json games[] entry with the record's keys, from a
// multiplayereverything repository, downloading only from the team's repositories; then checked like launcher.json.
// A record's repository decides which rules read it (records/ holds both kinds): the team's owner, or anyone else.
bool isTeamRecord(std::string_view json);
bool readTeamMod(std::string_view json, const Config& cfg, bool localRelease, GameSpec& out, std::wstring& error);

}  // namespace fl
