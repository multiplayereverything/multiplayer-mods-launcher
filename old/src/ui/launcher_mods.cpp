// The launcher window: PC game mods (find the game, install, update, repair,
// uninstall, launch). The work itself is core/mod; this file is the window's side.
#include "launcher.h"

#include "core/fs.h"
#include "core/log.h"
#include "core/strings.h"
#include "dialogs.h"
#include "launcher_internal.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>

namespace fl::ui {

using namespace detail;

mod::Spec Launcher::modSpec(int gi) const {
    mod::Spec s = *cfg_.games[gi].mod;
    s.text = &cfg_.text;
    return s;
}

// The mod's own variables plus what its texts mention (VERSION is the newest, INSTALLED what is there).
static Vars modVars(const Vars& base, const mod::Spec& spec, const std::wstring& installed, const std::wstring& newest) {
    Vars v = mod::withSpec(base, spec);
    v.set(L"INSTALLED", installed);
    v.set(L"VERSION", newest);
    return v;
}

const Launcher::ModView* Launcher::modView(int gi) const {
    auto it = mods_.find(gi);
    return it == mods_.end() ? nullptr : &it->second;
}

bool Launcher::gameReady(int gi) const {
    if (cfg_.games[gi].mod) {
        const ModView* v = modView(gi);
        return v && v->status.installed;
    }
    return lib_.gameState(cfg_.games[gi], user_) == ActionState::Ready;
}

void Launcher::pollModGames() {
    if (snapshot_ || installing_) return;
    bool selectedChanged = false, played = false, stopped = false;
    for (auto& [gi, view] : mods_) {
        if (!view.status.game.found()) continue;  // nothing to watch for a game that isn't on this PC
        bool running = sys::isProcessRunning(modSpec(gi).game.processNames);
        if (running) {
            user_.stats(cfg_.games[gi].id).seconds += 2;  // this poll runs every two seconds
            played = true;
        }
        if (running == view.status.running) continue;
        view.status.running = running;
        if (gi == selected_) selectedChanged = true;
        if (!running) stopped = true;
    }
    if (stopped || (played && ++modSaveTick_ % 15 == 0)) save();  // when it closes, else about every half minute
    if (selectedChanged) select(selected_);          // the buttons say whether the game has to be closed
    else if (played) InvalidateRect(hwnd_, &detail_, FALSE);
}

void Launcher::refreshMods() {
    for (size_t i = 0; i < cfg_.games.size(); ++i) {
        if (!cfg_.games[i].mod) continue;
        const GameStats* st = user_.find(cfg_.games[i].id);
        mods_[static_cast<int>(i)].status = mod::inspect(modSpec(static_cast<int>(i)), lib_.vars(), st ? st->gameDir : L"");
    }
}

// What mods-info says about each mod's newest version: asked on a worker thread.
void Launcher::startModsFetch() {
    if (snapshot_ || !onlineOn_) return;
    bool any = std::any_of(cfg_.games.begin(), cfg_.games.end(), [](const GameSpec& g) { return g.mod != nullptr; });
    if (!any) return;
    if (modsThread_.joinable()) modsThread_.join();
    HWND wnd = hwnd_;
    std::wstring base = cfg_.modsInfoBase();
    bool fresh = forceModsFetch_;  // F5 asks GitHub again, whatever was heard a few minutes ago
    forceModsFetch_ = false;
    modsThread_ = std::thread([this, wnd, base, fresh] {
        std::map<std::string, ModSource> found;
        for (const auto& g : cfg_.games) {
            if (!g.mod) continue;
            mod::Spec spec = *g.mod;
            ModSource s;
            std::wstring err;
            s.known = mod::resolveSource(spec, base, s.source, err, cfg_.updates.source.api, fs::join(lib_.dataDir(), L"mods"), fresh);
            if (!s.known) logger().warn(L"mods-info: " + g.name + L": " + err);
            found[g.id] = std::move(s);
        }
        {
            std::lock_guard<std::mutex> lock(installMu_);
            pendingSources_ = std::move(found);
        }
        PostMessageW(wnd, WM_APP_MODS, 0, 0);
    });
}

void Launcher::onModsFetched() {
    std::map<std::string, ModSource> found;
    {
        std::lock_guard<std::mutex> lock(installMu_);
        found.swap(pendingSources_);
    }
    for (size_t i = 0; i < cfg_.games.size(); ++i) {
        auto it = found.find(cfg_.games[i].id);
        if (it == found.end()) continue;
        ModView& v = mods_[static_cast<int>(i)];
        v.source = it->second.source;
        v.sourceKnown = it->second.known;
    }
    buildNav();
    InvalidateRect(grid_, nullptr, FALSE);
    select(selected_);
}

bool Launcher::modUpdateAvailable(int gi) const {
    const ModView* v = modView(gi);
    return v && v->status.installed && v->source.published() &&
           compareVersions(v->source.version, v->status.installedVersion) > 0;
}

std::wstring Launcher::modStateText(int gi, Color& color) const {
    const ModView* v = modView(gi);
    mod::Spec spec = modSpec(gi);
    if (!v) {
        color = theme_.c.textMuted;
        return {};
    }
    Vars vars = modVars(lib_.vars(), spec, v->status.installedVersion, v->source.version);
    if (!v->status.problem.empty()) {
        color = theme_.c.error;
        return spec.t("mod.status.problem", vars);
    }
    if (!v->status.game.found()) {
        color = theme_.c.warning;
        return spec.t("mod.status.noGame", vars);
    }
    if (v->status.installed) {
        if (modUpdateAvailable(gi)) {
            color = theme_.c.online;
            return spec.t("mod.status.update", vars);
        }
        // GitHub refused the lookup (its hourly limit, or no connection): say so, the newest version shown may be old.
        if (v->source.stale) {
            color = theme_.c.warning;
            return spec.t("mod.status.installedStale", vars);
        }
        color = theme_.c.success;
        return spec.t("mod.status.installed", vars);
    }
    if (!v->status.missing.empty()) {
        color = theme_.c.warning;
        return v->status.missing;
    }
    color = theme_.c.textMuted;
    return spec.t(v->sourceKnown && !v->source.published() ? "mod.status.notPublished" : "mod.status.notInstalled", vars);
}

bool Launcher::modActionVisible(int gi, const ActionSpec& a) const {
    const ModView* v = modView(gi);
    if (!v) return false;
    bool installed = v->status.installed;
    if (a.modOp == "install") return !installed;
    if (a.modOp == "update") return installed && modUpdateAvailable(gi);
    return installed;  // launch, repair, uninstall
}

bool Launcher::actionVisible(int gi, const ActionSpec& a) const {
    return a.isMod() ? modActionVisible(gi, a) : actionShown(a);
}

// Label, second line, icon and look of a mod's button for the state it is in.
void Launcher::applyModAction(int gi, const ActionSpec& a, HWND b) {
    const ModView* v = modView(gi);
    if (!v) return;
    mod::Spec spec = modSpec(gi);
    Vars vars = modVars(lib_.vars(), spec, v->status.installedVersion, v->source.version);
    BtnInfo& info = btnInfo_[b];
    const std::string& op = a.modOp;
    info.ready = true;
    info.tag.clear();
    info.style = BtnStyle::Secondary;
    std::wstring label = spec.t("mod." + op, vars), sub = spec.t("mod." + op + ".sub", vars);
    if (op == "launch") {
        info.icon = kIconReady;
        info.style = modUpdateAvailable(gi) ? BtnStyle::Secondary : BtnStyle::Primary;
    } else if (op == "update") {
        info.icon = kIconUpdate;
        info.style = BtnStyle::Primary;
        info.ready = !v->status.running && v->status.problem.empty();
        if (v->status.running) sub = spec.t("mod.status.running", vars);
    } else if (op == "install") {
        info.icon = kIconInstall;
        info.style = BtnStyle::Primary;
        if (!v->status.game.found()) {
            info.ready = false;
            sub = spec.t("mod.status.noGame", vars);
        } else if (!v->status.problem.empty()) {
            info.ready = false;
            sub = spec.t("mod.status.problem", vars);
        } else if (!v->status.missing.empty()) {
            info.ready = false;  // the loader this mod needs isn't in the game folder
            sub = v->status.missing;
        } else if (v->sourceKnown && !v->source.published()) {
            info.ready = false;
            sub = spec.t("mod.status.notPublished", vars);
        } else if (v->status.running) {
            info.ready = false;
            sub = spec.t("mod.status.running", vars);
        } else if (!v->source.published()) {
            sub = spec.t("mod.install.subUnknown", vars);  // mods-info hasn't answered yet
        }
    } else if (op == "repair") {
        info.icon = kIconRepair;
        info.ready = v->source.published() && !v->status.running;
        if (v->status.running) sub = spec.t("mod.status.running", vars);
        else if (!v->source.published()) sub = spec.t("mod.repair.subOffline", vars);
    } else {  // uninstall
        info.icon = kIconUninstall;
        info.ready = !v->status.running;
        if (v->status.running) sub = spec.t("mod.status.running", vars);
    }
    info.sub = sub;
    SetWindowTextW(b, label.c_str());
}

void Launcher::chooseGameFolder() {
    if (selected_ < 0 || !cfg_.games[selected_].mod) return;
    const GameSpec& g = cfg_.games[selected_];
    mod::Spec spec = modSpec(selected_);
    Vars vars = mod::withSpec(lib_.vars(), spec);
    std::wstring start;
    if (const ModView* v = modView(selected_); v && v->status.game.found()) start = v->status.game.dir;
    std::wstring dir = pickFolder(hwnd_, spec.t("mod.pickFolder", vars), start);
    if (dir.empty()) return;
    mod::PathCheck check = mod::checkGameDir(spec, vars, dir);
    // Picked a sub-folder or the folder above it: the check finds the real one nearby.
    if (check.level != mod::PathCheck::Level::Ok && !check.suggestion.empty()) dir = check.suggestion;
    else if (check.level == mod::PathCheck::Level::Error) {
        notice(check.message, theme_.c.warning);
        return;
    }
    user_.stats(g.id).gameDir = fs::normalize(dir);
    save();
    refreshMods();
    buildNav();
    rebuildGrid();
    select(selected_);
    notice(spec.t("path.ok", vars) + L" " + fs::normalize(dir), theme_.c.success);
}

bool Launcher::runModAction(int gi, const ActionSpec& a) {
    const GameSpec& g = cfg_.games[gi];
    mod::Spec spec = modSpec(gi);
    const GameStats* saved = user_.find(g.id);
    // The game may have been started or moved since the window last looked.
    mod::Status fresh = mod::inspect(spec, lib_.vars(), saved ? saved->gameDir : L"");
    ModView& v = mods_[gi];
    v.status = fresh;
    Vars vars = modVars(lib_.vars(), spec, v.status.installedVersion, v.source.version);
    auto refuse = [&](const std::wstring& text) {
        notice(text, theme_.c.warning);
        buildNav();
        select(selected_);
        return false;
    };
    if (installing_) return refuse(spec.t("mod.busy", vars));
    if (!v.status.game.found()) {
        chooseGameFolder();
        return false;
    }
    if (!v.status.problem.empty()) return refuse(v.status.problem);

    if (a.modOp == "launch") {
        if (!v.status.installed) return refuse(spec.t("mod.status.notInstalled", vars));
        std::wstring err;
        if (!mod::launchGame(spec, lib_.vars(), v.status.game.dir, err)) {
            notice(spec.t("notice.launchFailed", vars, {{L"PLAYER", g.name}, {L"ERROR", err}}), theme_.c.error);
            return false;
        }
        GameStats& st = user_.stats(g.id);
        ++st.plays;
        st.lastPlayed = unixNow();
        save();
        notice(spec.launch.hint.empty() ? spec.t("mod.launched", vars) : vars.expand(spec.launch.hint), theme_.c.success);
        return true;
    }

    if (v.status.running) return refuse(spec.t("mod.status.running", vars));
    mod::Operation op = a.modOp == "uninstall" ? mod::Operation::Uninstall
                        : a.modOp == "repair"  ? mod::Operation::Repair
                        : a.modOp == "update"  ? mod::Operation::Update
                                               : mod::Operation::Install;
    // mods-info may not have answered yet (the job asks again); a definite "no download" is final.
    if (op != mod::Operation::Uninstall && v.sourceKnown && !v.source.published())
        return refuse(spec.t("mod.notPublished", vars));

    if (op == mod::Operation::Uninstall) {
        std::wstring title = spec.t("mod.uninstall.title", vars), body = spec.t("mod.uninstall.body", vars);
        std::wstring yes = spec.t("mod.uninstall.yes", vars), no = spec.t("mod.uninstall.no", vars);
        TASKDIALOG_BUTTON buttons[] = {{IDYES, yes.c_str()}, {IDNO, no.c_str()}};
        TASKDIALOGCONFIG tdc{sizeof tdc};
        tdc.hwndParent = hwnd_;
        tdc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
        tdc.pszWindowTitle = cfg_.branding.subtitle.c_str();
        tdc.pszMainInstruction = title.c_str();
        tdc.pszContent = body.c_str();
        tdc.cButtons = 2;
        tdc.pButtons = buttons;
        tdc.nDefaultButton = IDNO;
        int choice = 0;
        if (FAILED(TaskDialogIndirect(&tdc, &choice, nullptr, nullptr)) || choice != IDYES) return false;
    }

    // Everything below runs on a worker thread; it only posts messages back.
    if (installThread_.joinable()) installThread_.join();
    installing_ = true;
    installCancel_ = false;
    modCancel_.reset();
    jobIsGame_ = false;
    jobName_ = g.name;
    modJobGame_ = gi;
    modJobOp_ = a.modOp;
    {
        std::lock_guard<std::mutex> lock(installMu_);
        modPhase_ = spec.t("phase.preparing", vars);
        modFraction_ = 0;
        modItem_.clear();
    }
    mod::RunRequest req;
    req.op = op;
    req.gameDir = v.status.game.dir;
    req.source = v.source;
    req.cacheDir = fs::join(lib_.dataDir(), L"mods");
    req.githubApi = cfg_.updates.source.api;
    Vars base = lib_.vars();
    HWND wnd = hwnd_;
    std::wstring modsInfo = cfg_.modsInfoBase();
    std::string opName = a.modOp;
    auto lastPosted = std::make_shared<std::atomic<size_t>>(~size_t{0});
    installThread_ = std::thread([this, spec, base, req, wnd, gi, opName, lastPosted, modsInfo]() mutable {
        if (req.op != mod::Operation::Uninstall && !req.source.published()) {
            std::wstring err;
            if (!mod::resolveSource(spec, modsInfo, req.source, err, req.githubApi, req.cacheDir)) logger().warn(L"mods-info: " + err);
        }
        mod::Outcome o = mod::run(spec, base, req, [&, this](const std::wstring& phase, double f, const std::wstring& item) {
            {
                std::lock_guard<std::mutex> lock(installMu_);
                modPhase_ = phase;
                modFraction_ = f;
                modItem_ = item;
            }
            // About every percent, not every file.
            size_t key = std::hash<std::wstring>{}(phase) ^ static_cast<size_t>(f * 100);
            if (lastPosted->exchange(key) != key) PostMessageW(wnd, WM_APP_MODPROGRESS, 0, 0);
        }, modCancel_);
        auto* done = new ModDone{gi, opName, o.result == mod::Outcome::Result::Success, o.message, o.detail,
                                 req.source.version, o.rollbackComplete};
        PostMessageW(wnd, WM_APP_MODDONE, 0, reinterpret_cast<LPARAM>(done));
    });
    InvalidateRect(hwnd_, &status_, FALSE);
    select(selected_);  // the button shows its working light
    return true;
}

void Launcher::onModDone(ModDone* raw) {
    std::unique_ptr<ModDone> d(raw);
    if (installThread_.joinable()) installThread_.join();
    installing_ = false;
    modJobGame_ = -1;
    modJobOp_.clear();
    mod::Spec spec = modSpec(d->game);
    refreshMods();
    ModView& v = mods_[d->game];
    Vars vars = modVars(lib_.vars(), spec, v.status.installedVersion, d->version);
    if (d->ok) {
        toast(spec.t("mod.done." + d->op, vars), theme_.c.success);
    } else {
        std::wstring text = spec.t("mod.failed", vars, {{L"ERROR", d->message}});
        if (!d->rollbackComplete) text += L" " + spec.t("mod.rollbackIncomplete", vars);
        notice(text, theme_.c.error);
        logger().error(spec.mod.name + L": " + d->message + (d->detail.empty() ? L"" : L" | " + d->detail));
    }
    buildNav();
    rebuildGrid();
    select(selected_);
    InvalidateRect(hwnd_, &status_, FALSE);
}

// "Installing Spooky Together: Copying files 42%".
std::wstring Launcher::modJobText() const {
    std::lock_guard<std::mutex> lock(installMu_);
    return jobName_ + L": " + modPhase_ + L" " + std::to_wstring(static_cast<int>(modFraction_ * 100)) + L"%";
}

}  // namespace fl::ui
