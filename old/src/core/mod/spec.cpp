#include "core/mod/spec.h"

#include "core/strings.h"

namespace fl::mod {

Vars Spec::baseVars() const {
    Vars v;
    v.set(L"MOD_ID", mod.id);
    v.set(L"MOD_NAME", mod.name);
    v.set(L"MOD_VERSION", mod.version);
    v.set(L"MOD_PUBLISHER", mod.publisher);
    v.set(L"MOD_WEBSITE", mod.website);
    v.set(L"MOD_SUPPORT_URL", mod.supportUrl);
    v.set(L"MOD_DESCRIPTION", mod.description);
    v.set(L"GAME_NAME", game.name);
    v.set(L"GAME_EXE", game.executable);
    v.set(L"STEAM_APP_ID", game.steamAppId);
    return v;
}

std::wstring Spec::t(const std::string& key, const Vars& vars,
                     std::initializer_list<std::pair<const wchar_t*, std::wstring>> extra) const {
    if (!text) return widen(key);
    auto it = text->find(key);
    if (it == text->end()) return widen(key);
    if (extra.size() == 0) return trim(vars.expand(it->second));
    Vars v = vars;
    for (const auto& [name, value] : extra) v.set(name, value);
    return trim(v.expand(it->second));
}

const Component* Spec::component(const std::string& id) const {
    for (const auto& c : components)
        if (c.id == id) return &c;
    return nullptr;
}

}  // namespace fl::mod
