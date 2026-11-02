#include "vars.h"

#include "fs.h"
#include "strings.h"
#include "win.h"

namespace fl {

void Vars::set(const std::wstring& name, const std::wstring& value) { values_[toUpper(name)] = value; }

std::wstring Vars::get(const std::wstring& name) const {
    auto it = values_.find(toUpper(name));
    return it == values_.end() ? std::wstring() : it->second;
}

bool Vars::has(const std::wstring& name) const { return values_.count(toUpper(name)) != 0; }

std::wstring Vars::expand(std::wstring_view text, std::vector<std::wstring>* unknown) const {
    return expandDepth(text, 0, unknown);
}

std::wstring Vars::expandDepth(std::wstring_view text, int depth, std::vector<std::wstring>* unknown) const {
    std::wstring out;
    size_t i = 0;
    while (i < text.size()) {
        size_t start = text.find(L"${", i);
        if (start == std::wstring_view::npos) {
            out += text.substr(i);
            break;
        }
        size_t end = text.find(L'}', start + 2);
        if (end == std::wstring_view::npos) {
            out += text.substr(i);
            break;
        }
        out += text.substr(i, start - i);
        std::wstring name(text.substr(start + 2, end - start - 2));
        std::wstring value;
        bool found = true;
        if (startsWithI(name, L"env:")) {
            std::wstring var = name.substr(4);
            DWORD n = GetEnvironmentVariableW(var.c_str(), nullptr, 0);
            if (n) {
                value.resize(n);
                n = GetEnvironmentVariableW(var.c_str(), value.data(), n);
                value.resize(n);
            } else {
                found = false;
            }
        } else if (startsWithI(name, L"folder:")) {
            value = fs::knownFolder(name.substr(7));
            found = !value.empty();
        } else {
            auto it = values_.find(toUpper(name));
            found = it != values_.end();
            if (found) value = depth < 8 ? expandDepth(it->second, depth + 1, unknown) : it->second;
        }
        if (found) {
            out += value;
        } else {
            out += text.substr(start, end - start + 1);
            if (unknown) unknown->push_back(name);
        }
        i = end + 1;
    }
    return out;
}

const std::vector<std::wstring>& knownVariableNames() {
    static const std::vector<std::wstring> names = {
        L"LAUNCHER_DIR", L"GAMES_DIR", L"PLAYERS_DIR", L"DATA_DIR", L"FLASHPOINT",
        L"GAME_ID",      L"GAME_NAME", L"SWF",         L"SWF_DIR",  L"PORT",
        L"TITLE",        L"SUBTITLE",  L"URL",       L"FILE",     L"FILE_DIR",
        L"PLAYER_NAME", L"CODE", L"SERVER", L"ADDRESS",
        L"MOD_ID",     L"MOD_NAME",   L"MOD_VERSION", L"MOD_PUBLISHER", L"MOD_WEBSITE",
        L"MOD_SUPPORT_URL", L"MOD_DESCRIPTION",
        L"GAME_EXE",   L"STEAM_APP_ID", L"GAME_DIR", L"INSTALL_DIR", L"STATE_DIR", L"INSTALLED_VERSION",
    };
    return names;
}

}  // namespace fl
