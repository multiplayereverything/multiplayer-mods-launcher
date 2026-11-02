// The install record (STATE_DIR\manifest.json). It lists every file the mod
// owns so update, repair and uninstall never touch anything else.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl::mod {

struct FileRecord {
    std::wstring path;    // absolute
    uint64_t size = 0;
    std::string sha256;
    std::string component;
    std::wstring backup;  // where the original game file was moved, if any
    bool preserved = false;
};

struct InstalledState {
    int schema = 1;
    std::wstring modId, modName, version, gameDir, installDir, stateDir, installedAt, uninstallKey;
    std::vector<std::string> components, shortcutIds;
    std::vector<FileRecord> files;
    std::vector<std::wstring> shortcuts, createdDirs;
    std::vector<std::wstring> keepFiles;  // uninstallCleanup matches that existed before the first install
    bool registered = false;
};

constexpr const wchar_t* kManifestName = L"manifest.json";
constexpr const wchar_t* kUninstallerName = L"uninstall.exe";

std::optional<InstalledState> loadState(const std::wstring& stateDir, std::wstring* error = nullptr);
std::string serializeState(const InstalledState& s);
std::optional<InstalledState> parseState(const std::string& text, std::wstring* error);

}  // namespace fl::mod
