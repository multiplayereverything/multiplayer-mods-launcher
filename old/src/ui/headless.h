// Starting an action without the window (command line --launch, the lobby session): what launcher_headless.cpp
// needs, with no window types, so the Linux build (which has no window) compiles it as well.
#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/library.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::ui {

struct HeadlessOptions {
    // ${CODE} / ${ADDRESS} (--join, --address); a code also picks the action's joinArgs.
    std::wstring joinCode, address;
    int coopSave = 0;  // the host's co-op save: 0 continue, 1 from the solo save, 2 new game
    bool wait = false;  // stay until the game exits (counts its time), not only while hosting
    bool tv = false;    // started from TV mode: the player's "tv" options and environment
    std::vector<std::pair<std::wstring, std::wstring>> flashVars;  // more values for the game (the lobby's room)
    // While a host action keeps the relay up: a JSON line each time who is connected changes.
    std::function<void(const std::string&)> report;
    // Set: the player's process is handed back here at once instead of waited for (the lobby watches it).
    HANDLE* process = nullptr;
};

#ifndef _WIN32
// Linux has no launcher window: only its headless start (launcher_headless.cpp).
class Launcher {
public:
    using HeadlessOptions = ui::HeadlessOptions;
    static int launchHeadless(const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath,
                              const GameSpec& game, const ActionSpec& action, std::wstring& message,
                              const HeadlessOptions& opt = {});
};
#endif

namespace detail {

// A fixed port per game keeps HTML5 saves: browsers file storage by address.
inline int stablePort(const std::string& gameId) {
    unsigned h = 2166136261u;
    for (char ch : gameId) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    return 47000 + static_cast<int>(h % 2000);
}

using FlashVars = std::vector<std::pair<std::wstring, std::wstring>>;

// The action's own values first (online mode, your name), then this launch's
// (a room code). Empty values are left out.
inline FlashVars gameValues(const ActionSpec& a, const std::wstring& playerName, const FlashVars& given) {
    FlashVars out;
    for (const auto& [key, value] : a.flashvars) {
        std::wstring v = replaceAll(value, L"${PLAYER_NAME}", playerName);
        if (!v.empty()) out.emplace_back(key, v);
    }
    // An action that takes its room on the command line (joinArgs) must give the game the same settings on
    // every PC, so the room code and "host" are not passed as values too.
    if (a.joinArgs.empty()) out.insert(out.end(), given.begin(), given.end());
    return out;
}

// Values for the game go in before the game file, the way the player expects
// options; players without flashvarArg can't take them.
inline void addFlashVars(ResolvedAction& r, const FlashVars& vars) {
    if (vars.empty() || !r.player || r.player->flashvarArg.empty()) return;
    auto at = std::find(r.args.rbegin(), r.args.rend(), r.swf);
    auto pos = at == r.args.rend() ? r.args.end() : std::prev(at.base());
    std::vector<std::wstring> extra;
    for (const auto& [key, value] : vars)
        extra.push_back(replaceAll(replaceAll(r.player->flashvarArg, L"${KEY}", key), L"${VALUE}", value));
    r.args.insert(pos, extra.begin(), extra.end());
}

// Fills ${PLAYER_NAME}, ${CODE}, ${SERVER} and ${ADDRESS} in the final arguments (the game file and
// flashvars never contain them). Returns the name of a placeholder that has no value, or empty.
inline std::wstring fillPlaceholders(ResolvedAction& r, const std::wstring& name, const std::wstring& code,
                                     const std::wstring& server, const std::wstring& address) {
    const std::pair<const wchar_t*, const std::wstring*> values[] = {
        {L"PLAYER_NAME", &name}, {L"CODE", &code}, {L"SERVER", &server}, {L"ADDRESS", &address}};
    for (auto& arg : r.args)
        for (const auto& [key, value] : values) {
            std::wstring token = std::wstring(L"${") + key + L"}";
            if (arg.find(token) == std::wstring::npos) continue;
            if (value->empty() && std::wstring(key) != L"PLAYER_NAME") return key;
            arg = replaceAll(arg, token, *value);
        }
    return {};
}

inline std::wstring stateReason(const Config& cfg, const ResolvedAction& r) {
    if (r.state == ActionState::NoRuntime) return cfg.t("status.noRuntime", {{L"RUNTIME", r.action->server.name}});
    if (r.state == ActionState::NoSwf) return cfg.t("status.noSwf");
    if (r.state == ActionState::NoPlayer) return cfg.t("status.noPlayer");
    return cfg.t("status.ready");
}

}  // namespace detail
}  // namespace fl::ui
