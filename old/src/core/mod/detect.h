// Game folder detection (Steam, Epic, registry, fixed paths) and validation
// of a folder the user picked.
#pragma once
#include <string>
#include <vector>

#include "core/mod/spec.h"

namespace fl::mod {

struct Candidate {
    std::wstring path;
    std::wstring source;  // "Steam", "Epic Games", "Registry", "Default location"
    bool valid = false;   // validation files present
};

std::vector<Candidate> detectGame(const Spec& cfg, const Vars& vars);

// Exposed for tests.
std::vector<std::string> parseVdfValues(const std::string& text, const std::string& key);
std::vector<std::wstring> steamLibraries(const std::wstring& steamRoot);
std::wstring steamGamePath(const std::vector<std::wstring>& libraries, const std::wstring& appId,
                           const std::wstring& installDir);
std::wstring readRegistryPath(const std::wstring& key, const std::wstring& value);

struct PathCheck {
    enum class Level { Ok, Warning, Error } level = Level::Error;
    std::wstring message;
    std::wstring suggestion;     // a nearby folder that does look valid
    std::wstring missingFile;
};

bool looksLikeGameDir(const Spec& cfg, const Vars& vars, const std::wstring& dir, std::wstring* missing = nullptr);
PathCheck checkGameDir(const Spec& cfg, const Vars& vars, const std::wstring& dir);

}  // namespace fl::mod
