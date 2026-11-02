#include "core/mod/detect.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::mod {

std::vector<std::string> parseVdfValues(const std::string& text, const std::string& key) {
    std::vector<std::string> out;
    std::string pending;
    bool havePending = false;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '{' || c == '}') {
            havePending = false;
            ++i;
            continue;
        }
        std::string tok;
        if (c == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    char e = text[++i];
                    tok += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else {
                    tok += text[i];
                }
                ++i;
            }
            ++i;
        } else {
            while (i < text.size() && !isspace(static_cast<unsigned char>(text[i])) && text[i] != '{' && text[i] != '}')
                tok += text[i++];
        }
        if (havePending) {
            if (_stricmp(pending.c_str(), key.c_str()) == 0) out.push_back(tok);
            havePending = false;
        } else {
            pending = tok;
            havePending = true;
        }
    }
    return out;
}

static std::vector<std::wstring> steamRoots() {
    std::vector<std::wstring> roots;
    auto add = [&](const std::wstring& p) {
        std::wstring n = fs::normalize(p);
        if (n.empty() || !fs::isDir(n)) return;
        for (const auto& r : roots)
            if (iequals(r, n)) return;
        roots.push_back(n);
    };
    add(readRegistryPath(L"HKCU\\Software\\Valve\\Steam", L"SteamPath"));
    add(readRegistryPath(L"HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
    add(readRegistryPath(L"HKLM\\SOFTWARE\\Valve\\Steam", L"InstallPath"));
    add(fs::join(fs::knownFolder(L"ProgramFilesX86"), L"Steam"));
    return roots;
}

std::vector<std::wstring> steamLibraries(const std::wstring& steamRoot) {
    std::vector<std::wstring> libs{fs::normalize(steamRoot)};
    for (const wchar_t* rel : {L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf"}) {
        std::string text;
        if (!fs::readText(fs::join(steamRoot, rel), text)) continue;
        std::vector<std::string> paths = parseVdfValues(text, "path");
        // Old format: "1" "D:\\SteamLibrary"
        for (int n = 1; n < 32; ++n)
            for (auto& v : parseVdfValues(text, std::to_string(n)))
                if (v.size() > 2 && v[1] == ':') paths.push_back(v);
        for (const auto& p : paths) {
            std::wstring w = fs::normalize(widen(p));
            bool dup = false;
            for (const auto& l : libs) dup = dup || iequals(l, w);
            if (!dup && !w.empty()) libs.push_back(w);
        }
    }
    return libs;
}

std::wstring steamGamePath(const std::vector<std::wstring>& libraries, const std::wstring& appId,
                           const std::wstring& installDir) {
    for (const auto& lib : libraries) {
        std::wstring common = fs::join(lib, L"steamapps\\common");
        if (!appId.empty()) {
            std::string acf;
            if (fs::readText(fs::join(lib, L"steamapps\\appmanifest_" + appId + L".acf"), acf)) {
                auto dirs = parseVdfValues(acf, "installdir");
                if (!dirs.empty()) {
                    std::wstring p = fs::join(common, widen(dirs[0]));
                    if (fs::isDir(p)) return p;
                }
            }
        }
        if (!installDir.empty()) {
            std::wstring p = fs::join(common, installDir);
            if (fs::isDir(p)) return p;
        }
    }
    return {};
}

std::wstring readRegistryPath(const std::wstring& key, const std::wstring& value) {
    HKEY root = nullptr;
    std::wstring sub;
    struct Hive {
        const wchar_t* name;
        HKEY key;
    };
    static const Hive hives[] = {{L"HKLM\\", HKEY_LOCAL_MACHINE},        {L"HKEY_LOCAL_MACHINE\\", HKEY_LOCAL_MACHINE},
                                 {L"HKCU\\", HKEY_CURRENT_USER},         {L"HKEY_CURRENT_USER\\", HKEY_CURRENT_USER},
                                 {L"HKCR\\", HKEY_CLASSES_ROOT}};
    for (const auto& h : hives) {
        if (startsWithI(key, h.name)) {
            root = h.key;
            sub = key.substr(wcslen(h.name));
        }
    }
    if (!root) return {};
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        wchar_t buf[2048];
        DWORD size = sizeof buf;
        DWORD type = 0;
        if (RegGetValueW(root, sub.c_str(), value.empty() ? nullptr : value.c_str(),
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | (view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : RRF_SUBKEY_WOW6432KEY),
                         &type, buf, &size) == ERROR_SUCCESS)
            return buf;
    }
    return {};
}

static std::vector<std::wstring> epicLocations(const std::wstring& appName) {
    std::vector<std::wstring> out;
    std::wstring dir = fs::join(fs::knownFolder(L"ProgramData"), L"Epic\\EpicGamesLauncher\\Data\\Manifests");
    std::error_code ec;
    if (!fs::isDir(dir)) return out;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        if (!iequals(it->path().extension().wstring(), L".item")) continue;
        std::string text;
        if (!fs::readText(it->path().wstring(), text)) continue;
        json::Value v = json::parse(text, nullptr);
        auto name = v.find("AppName"), display = v.find("DisplayName"), loc = v.find("InstallLocation");
        if (!loc) continue;
        if ((name && iequals(widen(name->str()), appName)) || (display && iequals(widen(display->str()), appName)))
            out.push_back(widen(loc->str()));
    }
    return out;
}

std::vector<Candidate> detectGame(const Spec& cfg, const Vars& vars) {
    std::vector<Candidate> found;
    auto add = [&](const std::wstring& path, const wchar_t* source) {
        std::wstring n = fs::normalize(path);
        if (n.empty() || !fs::isDir(n)) return;
        for (const auto& c : found)
            if (iequals(c.path, n)) return;
        Candidate c{n, source, looksLikeGameDir(cfg, vars, n)};
        logger().info(L"Detected " + n + L" via " + source + (c.valid ? L"" : L" (validation files missing)"));
        found.push_back(c);
    };

    std::vector<std::wstring> libs;
    bool libsLoaded = false;
    for (const auto& rule : cfg.game.detection) {
        if (iequals(rule.type, L"steam")) {
            if (!libsLoaded) {
                for (const auto& root : steamRoots())
                    for (const auto& lib : steamLibraries(root)) libs.push_back(lib);
                libsLoaded = true;
            }
            std::wstring appId = rule.appId.empty() ? cfg.game.steamAppId : vars.expand(rule.appId);
            add(steamGamePath(libs, appId, vars.expand(rule.installDir)), L"Steam");
        } else if (iequals(rule.type, L"epic")) {
            for (const auto& p : epicLocations(vars.expand(rule.appName))) add(p, L"Epic Games");
        } else if (iequals(rule.type, L"registry")) {
            std::wstring p = readRegistryPath(vars.expand(rule.key), vars.expand(rule.value));
            if (!p.empty() && !rule.subPath.empty()) p = fs::join(p, vars.expand(rule.subPath));
            add(p, L"Registry");
        } else if (iequals(rule.type, L"path")) {
            add(vars.expand(rule.path), L"Default location");
        }
    }
    std::stable_partition(found.begin(), found.end(), [](const Candidate& c) { return c.valid; });
    return found;
}

bool looksLikeGameDir(const Spec& cfg, const Vars& vars, const std::wstring& dir, std::wstring* missing) {
    std::vector<std::wstring> files = cfg.game.validationFiles;
    if (files.empty() && !cfg.game.executable.empty()) files.push_back(cfg.game.executable);
    for (const auto& f : files) {
        std::wstring rel = vars.expand(f);
        if (!fs::exists(fs::join(dir, rel))) {
            if (missing) *missing = rel;
            return false;
        }
    }
    return true;
}

PathCheck checkGameDir(const Spec& cfg, const Vars& vars, const std::wstring& input) {
    PathCheck r;
    std::wstring dir = fs::normalize(input);
    auto fail = [&](const char* key) {
        r.level = PathCheck::Level::Error;
        r.message = cfg.t(key, vars);
        return r;
    };
    if (dir.empty()) return fail("path.empty");
    if (fs::hasInvalidChars(dir)) return fail("path.invalidChars");
    if (!fs::isAbsolute(dir)) return fail("path.notAbsolute");
    if (fs::isFile(dir)) return fail("path.isFile");
    if (!fs::isDir(dir)) return fail("path.notFound");
    if (fs::isDangerousRoot(dir)) return fail("path.unsafe");

    std::wstring missing;
    if (looksLikeGameDir(cfg, vars, dir, &missing)) {
        r.level = PathCheck::Level::Ok;
        r.message = cfg.t("path.ok", vars);
        return r;
    }
    r.missingFile = missing;

    // Picked a sub-folder (bin\) or the parent (steamapps\common\)? Look nearby.
    std::wstring up = fs::parent(dir);
    for (int i = 0; i < 2 && !up.empty() && r.suggestion.empty(); ++i, up = fs::parent(up))
        if (!fs::isDangerousRoot(up) && looksLikeGameDir(cfg, vars, up)) r.suggestion = up;
    if (r.suggestion.empty()) {
        std::error_code ec;
        int checked = 0;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator() && checked < 200; it.increment(ec), ++checked) {
            if (it->is_directory(ec) && looksLikeGameDir(cfg, vars, it->path().wstring())) {
                r.suggestion = it->path().wstring();
                break;
            }
        }
    }

    bool soft = cfg.game.allowUnvalidated;
    r.level = soft ? PathCheck::Level::Warning : PathCheck::Level::Error;
    r.message = cfg.t(soft ? "path.unvalidated" : "path.missingFile", vars, {{L"FILE", missing}});
    if (!r.suggestion.empty()) r.message += L"\n" + cfg.t("path.suggest", vars, {{L"SUGGESTION", r.suggestion}});
    return r;
}

}  // namespace fl::mod
