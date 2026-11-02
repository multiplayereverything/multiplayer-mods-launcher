// What the launcher needs to know to install a PC game mod (a "mod game" in
// launcher.json): how to find the game, which files go where, how to start it.
// The shapes follow the Multiplayer Everything installer template, so a mod
// installed by its stand-alone setup shows up here as installed too.
#pragma once
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/vars.h"

namespace fl::mod {

struct Info {
    std::wstring id, name, version, publisher, website, supportUrl, description;
};

struct DetectionRule {
    std::wstring type;  // steam | epic | registry | path
    std::wstring appId, installDir, appName, key, value, subPath, path;
};

struct GameInfo {
    std::wstring name, executable, steamAppId;
    std::vector<std::wstring> validationFiles, processNames;
    std::vector<DetectionRule> detection;
    bool allowUnvalidated = false;
};

struct Component {
    std::string id;
    std::wstring name, description, source, target = L"${INSTALL_DIR}";
    // A single-file source can land under another name ("coop_eax.dll" installed as "eax.dll").
    std::wstring targetName;
    // Instead of `source`: a copy of a file the game folder already has, the first of these that exists (relative to
    // the game folder), installed under `targetName` or its own name. A loader that wraps one of the game's own
    // files needs the original kept beside it; listing the copy first ("eax_orig.dll", then "eax.dll") keeps an
    // update from copying the wrapper over the copy.
    std::vector<std::wstring> sourceGame;
    // Game-folder paths that mean "this is already there" (a mod loader the player installed): when one exists and
    // this mod did not put it there, the component is left alone, and uninstall leaves it too.
    std::vector<std::wstring> skipIfPresent;
    bool required = false, selected = true;
};

struct LaunchSpec {
    std::wstring executable = L"${GAME_DIR}\\${GAME_EXE}", arguments, workingDir = L"${GAME_DIR}";
    std::wstring uri;  // e.g. steam://rungameid/${STEAM_APP_ID}: opened instead of the executable
    std::wstring hint; // shown in the status bar after the game starts ("press F5 in the game")
};

// A file the game folder must already have (a loader the launcher can't install itself), with
// the sentence shown when it is missing.
struct Requirement {
    std::wstring path, message;  // path relative to the game folder
};

// Where the mod's files go. Paths may use ${GAME_DIR}, ${INSTALL_DIR}, ${STATE_DIR}.
struct Layout {
    std::wstring targetDir = L"${GAME_DIR}\\Mods\\${MOD_ID}";
    std::wstring stateDir = L"${INSTALL_DIR}\\.installer";
    std::wstring uninstallKey = L"${MOD_ID}";
    std::vector<std::wstring> allowedRoots;  // extra roots components may write to
    std::vector<std::wstring> preserve;      // globs relative to INSTALL_DIR kept on update/uninstall
    // Globs relative to GAME_DIR for files the mod creates while running (loader
    // caches, logs); removed on uninstall unless they existed before the install.
    std::vector<std::wstring> uninstallCleanup;
    // Run that cleanup only when this component was installed by the mod (not skipped because the loader was
    // already there): the loader's runtime files then belong to the player.
    std::string uninstallCleanupWith;
    std::vector<Requirement> requires_;  // checked before anything is downloaded
    bool backupOverwritten = true;
    int lockedFileRetries = 5;
    int extraSpaceMB = 20;
};

// Where the mod's files are downloaded from. The usual setup leaves `url` and
// `sha256` empty: mods-info (`modsInfo` is the mod's id there) gives the newest
// version and its download address.
// A second download the mod needs, unpacked beside the package under `dir` (components use
// "<dir>/..." as their source). Usually the mod loader, straight from the loader's own release.
struct ExtraPackage {
    std::wstring dir;           // folder in the unpacked package, e.g. "loader"
    std::wstring github, tag;   // "owner/repo" and the release's tag (the newest when empty)
    std::wstring asset;         // file name glob in that release
    std::wstring url, sha256;   // a fixed address instead (sha256 learned from GitHub when empty)
};

struct Package {
    // Where the newest version comes from, first that is set: a pinned url, a GitHub repository's
    // newest release (pre-releases count; its asset matching `asset`), or the mod's file in mods-info.
    std::wstring modsInfo, url, sha256;
    // What a pinned url is called (shown, and compared to see if an update is waiting): bump it with the url.
    std::wstring version;
    std::wstring github, asset;
    std::vector<ExtraPackage> extras;
};


struct Spec {
    Info mod;
    GameInfo game;
    Layout install;
    std::vector<Component> components;
    LaunchSpec launch;
    Package package;
    // The launcher's UI texts (Config::text); set by whoever uses the spec.
    const std::map<std::string, std::wstring>* text = nullptr;

    Vars baseVars() const;  // MOD_*, GAME_NAME, GAME_EXE, STEAM_APP_ID
    // Looks up a text key and expands variables; `extra` adds per-message values.
    std::wstring t(const std::string& key, const Vars& vars,
                   std::initializer_list<std::pair<const wchar_t*, std::wstring>> extra = {}) const;
    const Component* component(const std::string& id) const;
};

}  // namespace fl::mod
