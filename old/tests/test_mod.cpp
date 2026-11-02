// Integration tests for the PC-mod install engine against real temporary
// folders: install, update, repair, modify, uninstall, cancellation, injected
// failures, locked files and crash recovery. Ported from the installer
// template's engine tests; the engine is the same code.
#include "core/config.h"
#include "core/mod/engine.h"
#include "core/mod/manager.h"
#include "core/mod/resources.h"
#include "core/mod/state.h"
#include "core/win.h"
#include "test.h"

using namespace fl;
using namespace fl::mod;

namespace {

// A launcher.json with one PC mod game; `installExtra` goes inside "install".
std::string modGameConfig(const std::string& installExtra = "") {
    return R"({
      "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                "data": "${LAUNCHER_DIR}\\data", "flashpoint": []},
      "players": [ {"id": "ruffle", "paths": ["r.exe"]} ],
      "games": [ {
        "id": "test-game", "name": "Test Mod",
        "mod": {
          "id": "test-mod",
          "game": {"name": "Test Game", "executable": "Game.exe",
                   "detection": [{"type": "path", "path": "C:\\nowhere"}]},
          "install": {"lockedFileRetries": 1, "preserve": ["config/*.ini"])" + installExtra + R"(},
          "components": [
            {"id": "core", "name": "Core", "source": "payload/core", "required": true},
            {"id": "override", "name": "Override", "source": "payload/override", "target": "${GAME_DIR}"},
            {"id": "extras", "name": "Extras", "source": "payload/extras", "target": "${INSTALL_DIR}\\extras", "selected": false}
          ],
          "package": {"url": "https://example.invalid/package.zip"}
        } } ]
    })";
}

// A fake game folder plus a package folder with three components:
//   core     (required) payload/core     -> ${INSTALL_DIR}
//   override            payload/override -> ${GAME_DIR}   (replaces Data\original.dat)
//   extras   (optional, off by default) payload/extras -> ${INSTALL_DIR}\extras
struct Fixture {
    TempDir root;
    std::wstring game, mod;
    Config launcher;  // owns the texts the spec points at
    Spec cfg;
    std::unique_ptr<Resources> res;

    explicit Fixture(const std::string& name, const std::string& installExtra = "") : root(name) {
        game = root / L"Game";
        mod = root / L"Mod";
        writeText(game + L"\\Game.exe", "game binary");
        writeText(game + L"\\Data\\original.dat", "ORIGINAL");
        writeText(mod + L"\\payload\\core\\Mod.dll", "mod dll v1");
        writeText(mod + L"\\payload\\core\\config\\settings.ini", "default=1");
        writeText(mod + L"\\payload\\core\\data\\content.pak", std::string(200000, 'P'));
        writeText(mod + L"\\payload\\core\\data\\old.txt", "obsolete in v2");
        writeText(mod + L"\\payload\\override\\Data\\original.dat", "MODDED");
        writeText(mod + L"\\payload\\extras\\readme.txt", "extras");
        setConfig("1.0.0", installExtra);
    }

    void setConfig(const std::string& version, const std::string& installExtra = "") {
        LoadResult r = loadConfig(modGameConfig(installExtra), L"C:\\x");
        if (!r.ok()) throw std::runtime_error(narrow(formatIssues(r.issues)));
        launcher = r.config;
        cfg = *launcher.games[0].mod;
        cfg.mod.version = widen(version);
        cfg.text = &launcher.text;
        res = openDirectory(mod);
    }

    std::wstring installDir() const { return game + L"\\Mods\\test-mod"; }
    std::wstring stateDir() const { return installDir() + L"\\.installer"; }

    Selection selection(std::set<std::string> comps = {"core", "override"}) const {
        Selection s;
        s.gameDir = game;
        s.components = std::move(comps);
        return s;
    }

    PlanResult plan(Operation op, const Selection& sel) const { return buildPlan(cfg, *res, op, sel, cfg.baseVars()); }

    Outcome run(Operation op, const Selection& sel, EngineOptions eo = {}, CancelToken* token = nullptr) {
        PlanResult p = plan(op, sel);
        if (!p.ok) throw std::runtime_error("plan failed: " + narrow(p.error));
        CancelToken local;
        Engine e(cfg, res.get(), std::move(eo));
        return e.run(p.plan, [&](const Progress& pr) {
            if (pr.fraction < 0 || pr.fraction > 1) throw std::runtime_error("progress out of range");
        }, token ? *token : local);
    }

    Outcome uninstall(EngineOptions eo = {}) {
        PlanResult p = buildUninstallPlan(cfg, stateDir(), cfg.baseVars());
        if (!p.ok) throw std::runtime_error("uninstall plan failed: " + narrow(p.error));
        CancelToken token;
        Engine e(cfg, res.get(), std::move(eo));
        return e.run(p.plan, nullptr, token);
    }

    bool gameUntouched() const {
        return readText(game + L"\\Data\\original.dat") == "ORIGINAL" && !exists(game + L"\\Mods") &&
               readText(game + L"\\Game.exe") == "game binary";
    }
};

}  // namespace

TEST(install_writes_files_manifest_and_backup) {
    Fixture f("install");
    Outcome o = f.run(Operation::Install, f.selection());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(o.filesWritten == 5);
    CHECK(readText(f.installDir() + L"\\Mod.dll") == "mod dll v1");
    CHECK(readText(f.game + L"\\Data\\original.dat") == "MODDED");
    CHECK(!exists(f.installDir() + L"\\extras"));
    CHECK(!exists(f.stateDir() + L"\\.txn"));
    auto st = loadState(f.stateDir());
    REQUIRE(st.has_value());
    CHECK(st->version == L"1.0.0");
    CHECK(st->files.size() == 5);
    bool backedUp = false;
    for (const auto& r : st->files)
        if (r.path == f.game + L"\\Data\\original.dat") backedUp = readText(r.backup) == "ORIGINAL";
    CHECK(backedUp);
    CHECK(exists(f.stateDir() + L"\\install.log"));
}

TEST(uninstall_restores_originals_and_cleans_up) {
    Fixture f("uninstall");
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    writeText(f.installDir() + L"\\config\\settings.ini", "user edited");  // preserved by "config/*.ini"
    writeText(f.installDir() + L"\\user-notes.txt", "not ours");
    Outcome o = f.uninstall();
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(readText(f.game + L"\\Data\\original.dat") == "ORIGINAL");
    CHECK(!exists(f.installDir() + L"\\Mod.dll"));
    CHECK(!exists(f.stateDir()));
    CHECK(readText(f.installDir() + L"\\config\\settings.ini") == "user edited");
    CHECK(readText(f.installDir() + L"\\user-notes.txt") == "not ours");
    CHECK(!exists(f.installDir() + L"\\data"));  // empty folders we created are removed
}

TEST(full_uninstall_leaves_game_exactly_as_before) {
    Fixture f("roundtrip");
    REQUIRE(f.run(Operation::Install, f.selection({"core", "override", "extras"})).result == Outcome::Result::Success);
    REQUIRE(f.uninstall().result == Outcome::Result::Success);
    CHECK(f.gameUntouched());
}

TEST(cancel_mid_install_rolls_back) {
    Fixture f("cancel");
    CancelToken token;
    EngineOptions eo;
    eo.beforeFile = [&](size_t i, const std::wstring&) {
        if (i == 3) token.cancel();
    };
    Outcome o = f.run(Operation::Install, f.selection(), eo, &token);
    CHECK(o.result == Outcome::Result::Cancelled);
    CHECK(o.rollbackComplete);
    CHECK(f.gameUntouched());
}

TEST(injected_failure_rolls_back) {
    Fixture f("fail");
    EngineOptions eo;
    eo.beforeFile = [](size_t i, const std::wstring&) {
        if (i == 4) throw std::runtime_error("disk exploded");
    };
    Outcome o = f.run(Operation::Install, f.selection(), eo);
    CHECK(o.result == Outcome::Result::Failed);
    CHECK(o.rollbackComplete);
    CHECK(o.detail.find(L"disk exploded") != std::wstring::npos);
    CHECK(f.gameUntouched());
}

TEST(locked_game_file_fails_cleanly) {
    Fixture f("locked");
    HANDLE h = CreateFileW((f.game + L"\\Data\\original.dat").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    Outcome o = f.run(Operation::Install, f.selection());
    CloseHandle(h);
    CHECK(o.result == Outcome::Result::Failed);
    CHECK(o.message.find(L"in use") != std::wstring::npos);
    CHECK(o.rollbackComplete);
    CHECK(f.gameUntouched());
    // Retry after the game is closed succeeds.
    CHECK(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
}

TEST(failed_update_restores_previous_version) {
    Fixture f("update-fail");
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    writeText(f.mod + L"\\payload\\core\\Mod.dll", "mod dll v2");
    f.setConfig("2.0.0");
    EngineOptions eo;
    eo.beforeFile = [](size_t i, const std::wstring&) {
        if (i == 4) throw std::runtime_error("boom");
    };
    CHECK(f.run(Operation::Update, f.selection(), eo).result == Outcome::Result::Failed);
    CHECK(readText(f.installDir() + L"\\Mod.dll") == "mod dll v1");
    CHECK(loadState(f.stateDir())->version == L"1.0.0");
    CHECK(readText(f.game + L"\\Data\\original.dat") == "MODDED");
}

TEST(missing_payload_is_reported_before_any_change) {
    Fixture f("missing");
    std::filesystem::remove_all(f.mod + L"\\payload\\override");
    f.res = openDirectory(f.mod);
    PlanResult p = f.plan(Operation::Install, f.selection());
    CHECK(!p.ok);
    CHECK(p.error.find(L"payload/override") != std::wstring::npos);
    CHECK(f.gameUntouched());
}

TEST(payload_removed_after_planning_fails_and_rolls_back) {
    Fixture f("vanish");
    PlanResult p = f.plan(Operation::Install, f.selection());
    REQUIRE(p.ok);
    DeleteFileW((f.mod + L"\\payload\\core\\data\\old.txt").c_str());
    CancelToken token;
    Outcome o = Engine(f.cfg, f.res.get()).run(p.plan, nullptr, token);
    CHECK(o.result == Outcome::Result::Failed);
    CHECK(o.message.find(L"lost") != std::wstring::npos);
    CHECK(f.gameUntouched());
}

TEST(unsafe_layouts_are_rejected) {
    Fixture f("layout", R"(, "targetDir": "${GAME_DIR}\\..\\Elsewhere")");
    CHECK(!f.plan(Operation::Install, f.selection()).ok);
    Fixture g("layout2", R"(, "stateDir": "${INSTALL_DIR}")");
    CHECK(!g.plan(Operation::Install, g.selection()).ok);
    Fixture h("space", R"(, "extraSpaceMB": 1048576)");
    PlanResult p = h.plan(Operation::Install, h.selection());
    CHECK(!p.ok);
    CHECK(p.error.find(L"Not enough free space") != std::wstring::npos);
}

TEST(repair_restores_missing_and_damaged_files) {
    Fixture f("repair");
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    DeleteFileW((f.installDir() + L"\\Mod.dll").c_str());
    writeText(f.installDir() + L"\\data\\content.pak", std::string(200000, 'X'));
    Outcome o = f.run(Operation::Repair, f.selection());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(o.filesWritten == 2);
    CHECK(o.filesSkipped == 3);
    CHECK(readText(f.installDir() + L"\\Mod.dll") == "mod dll v1");
    CHECK(readText(f.installDir() + L"\\data\\content.pak") == std::string(200000, 'P'));
    // The original game file backup survives a repair and is still restored on uninstall.
    REQUIRE(f.uninstall().result == Outcome::Result::Success);
    CHECK(readText(f.game + L"\\Data\\original.dat") == "ORIGINAL");
}

TEST(update_replaces_changed_removes_obsolete_keeps_settings) {
    Fixture f("update");
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    writeText(f.installDir() + L"\\config\\settings.ini", "user=42");
    writeText(f.mod + L"\\payload\\core\\Mod.dll", "mod dll v2");
    writeText(f.mod + L"\\payload\\core\\config\\settings.ini", "default=2");
    DeleteFileW((f.mod + L"\\payload\\core\\data\\old.txt").c_str());
    writeText(f.mod + L"\\payload\\core\\data\\new.txt", "new in v2");
    f.setConfig("2.0.0");
    Outcome o = f.run(Operation::Update, f.selection());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(readText(f.installDir() + L"\\Mod.dll") == "mod dll v2");
    CHECK(readText(f.installDir() + L"\\data\\new.txt") == "new in v2");
    CHECK(!exists(f.installDir() + L"\\data\\old.txt"));
    CHECK(readText(f.installDir() + L"\\config\\settings.ini") == "user=42");
    CHECK(o.filesRemoved == 1);
    CHECK(loadState(f.stateDir())->version == L"2.0.0");
}

TEST(modify_removes_deselected_component_and_restores_original) {
    Fixture f("modify");
    REQUIRE(f.run(Operation::Install, f.selection({"core", "override", "extras"})).result == Outcome::Result::Success);
    CHECK(exists(f.installDir() + L"\\extras\\readme.txt"));
    Outcome o = f.run(Operation::Modify, f.selection({"core"}));
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(!exists(f.installDir() + L"\\extras\\readme.txt"));
    CHECK(readText(f.game + L"\\Data\\original.dat") == "ORIGINAL");
    auto st = loadState(f.stateDir());
    CHECK(st->components.size() == 1);
}

TEST(required_components_are_always_installed) {
    Fixture f("required");
    PlanResult p = f.plan(Operation::Install, f.selection({}));
    REQUIRE(p.ok);
    CHECK(p.plan.components.size() == 1 && p.plan.components[0] == "core");
    CHECK(!f.plan(Operation::Install, f.selection({"nope"})).ok);
}

TEST(component_target_outside_game_is_rejected) {
    Fixture f("escape");
    f.cfg.components[1].target = L"C:\\Windows\\System32";
    PlanResult p = f.plan(Operation::Install, f.selection());
    CHECK(!p.ok);
    CHECK(p.error.find(L"outside") != std::wstring::npos);
}

TEST(other_mods_install_record_is_not_touched) {
    Fixture f("foreign");
    writeText(f.stateDir() + L"\\manifest.json", R"({"modId": "someone-else", "modName": "Other Mod", "files": []})");
    PlanResult p = f.plan(Operation::Install, f.selection());
    CHECK(!p.ok);
    CHECK(p.error.find(L"Other Mod") != std::wstring::npos);
}

TEST(uninstall_cleanup_removes_runtime_files_but_keeps_older_ones) {
    Fixture f("cleanup", R"(, "uninstallCleanup": ["Loader/cache/**", "Loader/LogOutput.log"])");
    writeText(f.game + L"\\Loader\\cache\\before-install.bin", "player's own");
    CreateDirectoryW((f.game + L"\\Loader\\cache\\empty-before").c_str(), nullptr);
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    // Files the mod's loader creates while the game runs.
    writeText(f.game + L"\\Loader\\cache\\deep\\chainloader.dat", "cache");
    writeText(f.game + L"\\Loader\\LogOutput.log", "log");

    CancelToken token;
    EngineOptions eo;
    eo.beforeFile = [&](size_t i, const std::wstring&) {
        if (i == 4) token.cancel();
    };
    PlanResult p = buildUninstallPlan(f.cfg, f.stateDir(), f.cfg.baseVars());
    REQUIRE(p.ok);
    Engine cancelled(f.cfg, f.res.get(), eo);
    CHECK(cancelled.run(p.plan, nullptr, token).result == Outcome::Result::Cancelled);
    CHECK(readText(f.game + L"\\Loader\\LogOutput.log") == "log");

    REQUIRE(f.uninstall().result == Outcome::Result::Success);
    CHECK(!exists(f.game + L"\\Loader\\LogOutput.log"));
    CHECK(!exists(f.game + L"\\Loader\\cache\\deep"));
    CHECK(readText(f.game + L"\\Loader\\cache\\before-install.bin") == "player's own");
    CHECK(exists(f.game + L"\\Loader\\cache\\empty-before"));  // pre-existing empty folder survives
    CHECK(readText(f.game + L"\\Data\\original.dat") == "ORIGINAL");
}


// A loader the player already has is not ours: install leaves it alone, uninstall leaves it and its runtime files.
TEST(a_loader_that_is_already_there_is_neither_installed_over_nor_removed) {
    Fixture f("own-loader", R"(, "uninstallCleanup": ["Loader/cache/**"], "uninstallCleanupWith": "override")");
    f.cfg.components[1].skipIfPresent = {L"Loader/core"};
    writeText(f.game + L"\\Loader\\core\\their.dll", "theirs");
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    CHECK(readText(f.game + L"\\Data\\original.dat") == "ORIGINAL");  // the component was skipped
    CHECK(exists(f.installDir() + L"\\Mod.dll"));                      // the mod itself went in
    writeText(f.game + L"\\Loader\\cache\\theirs.bin", "player's cache");
    REQUIRE(f.uninstall().result == Outcome::Result::Success);
    CHECK(readText(f.game + L"\\Loader\\core\\their.dll") == "theirs");
    CHECK(readText(f.game + L"\\Loader\\cache\\theirs.bin") == "player's cache");

    // Without a loader there, the component goes in, is recorded, and its cleanup runs on uninstall.
    Fixture g("no-loader", R"(, "uninstallCleanup": ["Loader/cache/**"], "uninstallCleanupWith": "override")");
    g.cfg.components[1].skipIfPresent = {L"Loader/core"};
    REQUIRE(g.run(Operation::Install, g.selection()).result == Outcome::Result::Success);
    CHECK(readText(g.game + L"\\Data\\original.dat") == "MODDED");
    writeText(g.game + L"\\Loader\\cache\\made.bin", "x");
    REQUIRE(g.uninstall().result == Outcome::Result::Success);
    CHECK(!exists(g.game + L"\\Loader\\cache\\made.bin"));
    CHECK(readText(g.game + L"\\Data\\original.dat") == "ORIGINAL");
}

TEST(uninstall_cleanup_patterns_are_validated) {
    LoadResult r = loadConfig(modGameConfig(R"(, "uninstallCleanup": ["../outside/**", "**/*.log", "ok/cache/**"])"), L"C:\\x");
    CHECK(!r.ok());
    std::wstring all = formatIssues(r.issues);
    CHECK(all.find(L"uninstallCleanup[0]") != std::wstring::npos);
    CHECK(all.find(L"uninstallCleanup[1]") != std::wstring::npos);
    CHECK(all.find(L"uninstallCleanup[2]") == std::wstring::npos);
}

// A game folder copied together with its install record: the record lists the
// original folder's absolute paths, so acting on it would damage the original.
TEST(copied_install_record_never_touches_the_original_folder) {
    Fixture f("copied-record");
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    std::wstring copy = f.root / L"GameCopy";
    std::filesystem::copy(f.game, copy, std::filesystem::copy_options::recursive);
    std::wstring copyState = copy + L"\\Mods\\test-mod\\.installer";
    REQUIRE(exists(copyState + L"\\manifest.json"));

    Selection sel = f.selection();
    sel.gameDir = copy;
    PlanResult install = buildPlan(f.cfg, *f.res, Operation::Repair, sel, f.cfg.baseVars());
    CHECK(!install.ok);
    CHECK(install.error.find(L"copied or moved") != std::wstring::npos);
    PlanResult remove = buildUninstallPlan(f.cfg, copyState, f.cfg.baseVars());
    CHECK(!remove.ok);

    // The original install is intact and still works normally.
    CHECK(readText(f.installDir() + L"\\Mod.dll") == "mod dll v1");
    CHECK(readText(f.game + L"\\Data\\original.dat") == "MODDED");
    CHECK(f.run(Operation::Repair, f.selection()).result == Outcome::Result::Success);
    REQUIRE(f.uninstall().result == Outcome::Result::Success);
    CHECK(readText(f.game + L"\\Data\\original.dat") == "ORIGINAL");
}

TEST(change_events_describe_what_happened) {
    Fixture f("changes");
    std::vector<Change> seen;
    EngineOptions eo;
    eo.onChange = [&](const Change& c) { seen.push_back(c); };
    auto count = [&](Change::Kind k) {
        return std::count_if(seen.begin(), seen.end(), [&](const Change& c) { return c.kind == k; });
    };
    REQUIRE(f.run(Operation::Install, f.selection(), eo).result == Outcome::Result::Success);
    CHECK(count(Change::Kind::Installed) == 4);
    CHECK(count(Change::Kind::Replaced) == 1);  // Data\original.dat, original saved
    CHECK(std::any_of(seen.begin(), seen.end(), [&](const Change& c) {
        return c.kind == Change::Kind::Replaced && c.path == f.game + L"\\Data\\original.dat";
    }));

    seen.clear();
    DeleteFileW((f.installDir() + L"\\Mod.dll").c_str());
    REQUIRE(f.run(Operation::Repair, f.selection(), eo).result == Outcome::Result::Success);
    CHECK(count(Change::Kind::Installed) == 1);
    CHECK(count(Change::Kind::Unchanged) == 3);
    CHECK(count(Change::Kind::Kept) == 1);  // config/settings.ini is preserved

    // A failed update reports each game file it put back.
    seen.clear();
    writeText(f.mod + L"\\payload\\core\\Mod.dll", "mod dll v2");
    f.setConfig("2.0.0");
    eo.beforeFile = [](size_t i, const std::wstring&) {
        if (i == 4) throw std::runtime_error("boom");
    };
    REQUIRE(f.run(Operation::Update, f.selection(), eo).result == Outcome::Result::Failed);
    CHECK(count(Change::Kind::Updated) == 1);
    CHECK(count(Change::Kind::Undone) == 1);
    CHECK(std::all_of(seen.begin(), seen.end(), [&](const Change& c) { return !fs::isUnder(c.path, f.stateDir()); }));
}

TEST(interrupted_transaction_is_recovered) {
    Fixture f("recover");
    std::wstring stray = f.game + L"\\half-written.fl-new";
    std::wstring moved = f.stateDir() + L"\\.txn\\1.stash";
    writeText(stray, "partial");
    writeText(moved, "ORIGINAL-EXE");
    DeleteFileW((f.game + L"\\Game.exe").c_str());
    std::string journal = "C\t" + narrow(stray) + "\nM\t" + narrow(f.game + L"\\Game.exe") + "\t" + narrow(moved) + "\n";
    writeText(f.stateDir() + L"\\.txn\\journal.log", journal);
    std::wstring report;
    CHECK(Engine::recover(f.stateDir(), &report));
    CHECK(!exists(stray));
    CHECK(readText(f.game + L"\\Game.exe") == "ORIGINAL-EXE");
    CHECK(!exists(f.stateDir() + L"\\.txn"));
    CHECK(!Engine::recover(f.stateDir(), &report));
}


// A mod first set up by its stand-alone installer: its record lists shortcuts and
// an Apps & Features entry. Updating through the launcher keeps them; uninstalling
// removes them.
TEST(a_standalone_installers_record_is_adopted) {
    std::string key = "fl-test-" + std::to_string(GetCurrentProcessId());
    Fixture f("adopt");
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    TempDir links("adopt-links");
    std::wstring lnk = links / L"Play.lnk";
    writeText(lnk, "shortcut stand-in");
    HKEY h = nullptr;
    std::wstring regKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + widen(key);
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, regKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) == ERROR_SUCCESS);
    RegCloseKey(h);

    auto st = loadState(f.stateDir());
    REQUIRE(st.has_value());
    st->uninstallKey = widen(key);
    st->registered = true;
    st->shortcuts = {lnk};
    st->shortcutIds = {"play"};
    std::string text = serializeState(*st);
    writeText(f.stateDir() + L"\\manifest.json", text);

    REQUIRE(f.run(Operation::Repair, f.selection()).result == Outcome::Result::Success);
    auto after = loadState(f.stateDir());
    REQUIRE(after.has_value());
    CHECK(after->registered && after->uninstallKey == widen(key));
    CHECK(after->shortcuts.size() == 1 && after->shortcuts[0] == lnk);

    REQUIRE(f.uninstall().result == Outcome::Result::Success);
    CHECK(!exists(lnk));
    HKEY probe = nullptr;
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, regKey.c_str(), 0, KEY_READ, &probe) != ERROR_SUCCESS);
    if (probe) RegCloseKey(probe);
    CHECK(f.gameUntouched());
}

// inspect() reads the install record in the game folder the mod's own config points at.
TEST(inspect_reports_game_installed_version_and_problems) {
    Fixture f("inspect");
    f.cfg.game.detection.clear();
    f.cfg.game.validationFiles = {L"Game.exe"};
    Vars base;
    Status s = inspect(f.cfg, base, f.game);
    CHECK(s.game.found() && !s.installed && s.problem.empty());
    REQUIRE(f.run(Operation::Install, f.selection()).result == Outcome::Result::Success);
    s = inspect(f.cfg, base, f.game);
    CHECK(s.installed && s.installedVersion == L"1.0.0");
    writeText(f.stateDir() + L"\\manifest.json", "not json at all");
    s = inspect(f.cfg, base, f.game);
    CHECK(!s.installed && !s.problem.empty());
    // A folder that isn't the game is not accepted.
    CHECK(!inspect(f.cfg, base, f.root / L"Mod").game.found());
}
