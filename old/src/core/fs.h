// Filesystem helpers. Every function takes and returns Windows paths with
// backslashes; long paths are handled internally via the \\?\ prefix.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl::fs {

std::wstring normalize(const std::wstring& path);  // trims quotes/space, collapses .., no trailing '\'
bool isAbsolute(const std::wstring& path);
bool isUnder(const std::wstring& child, const std::wstring& parent);  // true when equal, too
std::wstring join(const std::wstring& a, const std::wstring& b);
std::wstring parent(const std::wstring& path);
std::wstring fileName(const std::wstring& path);
std::wstring longPath(const std::wstring& path);
bool hasInvalidChars(const std::wstring& path);

bool exists(const std::wstring& path);
bool isDir(const std::wstring& path);
bool isFile(const std::wstring& path);
std::optional<uint64_t> fileSize(const std::wstring& path);

// Creates `dir` and missing parents; appends each created folder to `created`.
bool ensureDir(const std::wstring& dir, std::vector<std::wstring>* created, unsigned long* error);
// Drive roots, Windows, Program Files, the user profile root, Desktop...
bool isDangerousRoot(const std::wstring& path);
std::wstring nearestExisting(const std::wstring& path);
std::optional<uint64_t> freeSpace(const std::wstring& path);
bool canWrite(const std::wstring& dir, unsigned long* error);

std::wstring tempDir();
std::wstring selfExePath();
// Desktop, StartMenu, Documents, LocalAppData, RoamingAppData, ProgramFiles,
// ProgramFilesX86, SavedGames, Profile, Temp, Windows, ProgramData.
std::wstring knownFolder(const std::wstring& name);

bool readFile(const std::wstring& path, std::vector<uint8_t>& out);
bool readText(const std::wstring& path, std::string& out);
bool writeFile(const std::wstring& path, const void* data, size_t size, unsigned long* error);

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t size);
    std::string hex();  // finishes the hash
    void raw(uint8_t out[32]);

private:
    void* hash_ = nullptr;
    std::vector<uint8_t> object_;
};
std::string sha256File(const std::wstring& path);  // empty on error

}  // namespace fl::fs
