// The whole life of a PC game mod in one place: find the game, see what is
// installed, install / update / repair / uninstall it through the safe-install
// engine, start the game. No UI; the window and the command line both call this.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "core/mod/detect.h"
#include "core/mod/engine.h"
#include "core/mod/package.h"
#include "core/mod/spec.h"
#include "core/mod/state.h"

namespace fl::mod {

// `base` (the launcher's variables) plus this mod's MOD_* / GAME_* names.
Vars withSpec(const Vars& base, const Spec& spec);

struct GameLocation {
    std::wstring dir, source;  // source: "Steam", "Epic Games", "Chosen folder"...
    bool found() const { return !dir.empty(); }
};
// A folder the player chose wins when it looks like the game; otherwise the first detected install.
GameLocation findGame(const Spec& spec, const Vars& base, const std::wstring& chosenDir);

struct Status {
    GameLocation game;
    bool installed = false;
    std::wstring installedVersion;
    bool running = false;   // the game's process is running: files may be locked
    std::wstring problem;   // an install record that can't be used
    std::wstring missing;   // what the game folder lacks before this mod can go in (install.requires)
};
Status inspect(const Spec& spec, const Vars& base, const std::wstring& chosenDir);
// The first install.requires entry whose file the game folder lacks, as its message; empty when all are there.
std::wstring missingRequirement(const Spec& spec, const Vars& base, const std::wstring& gameDir);

// Runs install / update / repair against the unpacked package in `payloadDir`.
// `spec.mod.version` must be the version being installed.
Outcome install(const Spec& spec, const Vars& base, Operation op, const std::wstring& gameDir,
                const std::wstring& payloadDir, const ProgressFn& progress, CancelToken& cancel,
                EngineOptions options = {});
// Removes the mod from the game folder it was installed in (needs no package).
Outcome uninstall(const Spec& spec, const Vars& base, const std::wstring& gameDir, const ProgressFn& progress,
                  CancelToken& cancel, EngineOptions options = {});

// What to install: the version, its package address and (when mods-info or the spec states one) its SHA-256.
struct Source {
    std::wstring version, url, sha256;
    std::vector<ExtraDownload> extras;  // loader packages etc., unpacked beside it
    std::wstring minVersion, notice;  // from mods-info, for the window
    // The newest version couldn't be looked up just now (GitHub's hourly limit, no connection): this is the
    // last answer heard, which may be old.
    bool stale = false;
    bool published() const { return !url.empty() && !version.empty(); }
};
// The newest package: the spec's own pinned package, else what mods-info says.
// `githubApi` empty = api.github.com. A mod that comes from GitHub is asked about at most once every ten minutes
// per `cacheDir` (and not at all while GitHub's rate limit is used up: the last answer is used), because GitHub
// answers 60 questions an hour per address without a login. `forceFresh` skips the ten minutes (the F5 rescan).
// A loader pinned by tag is remembered for good: it costs no question after the first.
bool resolveSource(const Spec& spec, const std::wstring& modsInfoBase, Source& out, std::wstring& error,
                   const std::wstring& githubApi = L"", const std::wstring& cacheDir = L"", bool forceFresh = false);

// Phase text, 0..1 fraction of the current phase, and the current item (a file).
using Report = std::function<void(const std::wstring& phase, double fraction, const std::wstring& item)>;

struct RunRequest {
    Operation op = Operation::Install;
    std::wstring gameDir;
    Source source;                   // not needed to uninstall
    std::wstring cacheDir;           // where packages are kept
    std::wstring githubApi;          // empty: api.github.com (tests use a local server)
};
// Install, update, repair or uninstall start to finish: download and check the package, then the engine.
Outcome run(const Spec& spec, const Vars& base, const RunRequest& req, const Report& report, CancelToken& cancel,
            EngineOptions options = {});

// Starts the game the way the mod's launch settings say (a steam:// address or the executable).
bool launchGame(const Spec& spec, const Vars& base, const std::wstring& gameDir, std::wstring& error);

}  // namespace fl::mod
