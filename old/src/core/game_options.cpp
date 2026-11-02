#include "game_options.h"

#include <algorithm>

#include "core/fs.h"
#include "core/install.h"
#include "core/strings.h"

namespace fl {

std::wstring installedPlayerVersion(const std::wstring& exe) {
    std::string tag;
    if (exe.empty() || !fs::readText(fs::join(fs::parent(exe), kPlayerVersionFile), tag)) return {};
    std::wstring have = widen(tag);
    while (!have.empty() && (have.back() == L'\r' || have.back() == L'\n' || have.back() == L' ')) have.pop_back();
    if (!have.empty() && (have[0] == L'v' || have[0] == L'V')) have.erase(0, 1);
    return have;
}

std::map<std::string, std::vector<Setting>> cachedCatalogSettings(const std::wstring& dataDir) {
    std::map<std::string, std::vector<Setting>> out;
    std::string text;
    if (fs::readText(fs::join(dataDir, L"catalog.json"), text)) parseCatalogSettings(text, out);
    return out;
}

const PlayerSpec* GameOptions::coopSavePlayer() const {
    for (const auto& p : cfg_.players)
        if (!p.coopSaveArg.empty()) return &p;
    return nullptr;
}

bool GameOptions::coopSaveSupported() const {
    const PlayerSpec* p = coopSavePlayer();
    return p && playerAtLeast(p->coopSaveSince);
}

bool GameOptions::playerAtLeast(const std::wstring& version) const {
    const PlayerSpec* p = coopSavePlayer();
    const PlayerStatus* ps = p ? lib_.player(p->id) : nullptr;
    if (!ps || !ps->found()) return false;
    if (version.empty()) return true;
    std::wstring have = installedPlayerVersion(ps->exe);
    return !have.empty() && compareVersions(have, version) >= 0;
}

std::wstring GameOptions::coopSaveFile(const GameSpec& g) const {
    for (const auto& a : g.actions) {
        if (a.swf.empty()) continue;
        std::wstring name = fs::fileName(fs::normalize(lib_.vars().expand(a.swf.front())));
        if (name.empty()) continue;
        return fs::join(lib_.vars().expand(L"${folder:LocalAppData}\\ruffle\\SharedObjects\\coop"), name + L".save");
    }
    return {};
}

const std::vector<Setting>& GameOptions::settingsFor(const GameSpec& g) const {
    auto it = catalog_.find(g.id);
    return it != catalog_.end() && !it->second.empty() ? it->second : g.settings;
}

std::string GameOptions::settingValue(const GameSpec& g, const Setting& s) const {
    if (const GameStats* st = user_.find(g.id)) {
        if (auto it = st->settings.find(s.id); it != st->settings.end()) {
            if (s.type == "checkbox" && (it->second == "1" || it->second == "0")) return it->second;
            // A choice the catalog has since removed falls back to the default.
            if (s.type == "choice")
                for (const auto& c : s.choices)
                    if (c.id == it->second) return it->second;
        }
    }
    if (s.type == "checkbox") return s.def == "on" ? "1" : "0";
    for (const auto& c : s.choices)
        if (c.id == s.def) return c.id;
    return s.choices.empty() ? std::string() : s.choices.front().id;
}

std::vector<std::wstring> GameOptions::settingArgs(const GameSpec& g, const std::vector<std::wstring>& actionArgs) const {
    auto starts = [&](const wchar_t* prefix) {
        return std::any_of(actionArgs.begin(), actionArgs.end(), [&](const std::wstring& a) { return a.rfind(prefix, 0) == 0; });
    };
    bool hosting = starts(L"--coop-host"), joining = starts(L"--coop-join");
    std::vector<std::wstring> out;
    for (const auto& s : settingsFor(g)) {
        if (!(s.scope == "all" || (s.scope == "host" && hosting) || (s.scope == "join" && joining))) continue;
        if (!playerAtLeast(s.since)) continue;
        std::string v = settingValue(g, s);
        if (s.type == "checkbox") {
            if (v == "1") out.insert(out.end(), s.args.begin(), s.args.end());
        } else {
            for (const auto& c : s.choices)
                if (c.id == v) out.insert(out.end(), c.args.begin(), c.args.end());
        }
    }
    return out;
}

void GameOptions::addHostOptions(const GameSpec& g, std::vector<std::wstring>& args, int saveChoice) const {
    // "Continue" needs no option, and a player that doesn't know an option would refuse to start, so those wait
    // for their release.
    if (coopSaveGame(g)) {
        bool hosting = std::any_of(args.begin(), args.end(), [](const std::wstring& s) { return s.rfind(L"--coop-host", 0) == 0; });
        if (hosting && saveChoice > 0 && coopSaveSupported()) {
            args.push_back(coopSavePlayer()->coopSaveArg);
            args.push_back(saveChoice == 1 ? L"solo" : L"new");
        }
    }
    std::vector<std::wstring> more = settingArgs(g, args);
    args.insert(args.end(), more.begin(), more.end());
}

}  // namespace fl
