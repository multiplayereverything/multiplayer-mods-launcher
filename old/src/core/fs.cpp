#include "fs.h"

#include <algorithm>
#include <filesystem>

#include "strings.h"
#include "win.h"
// win.h first: these need the full Windows types.
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace fl::fs {

std::wstring normalize(const std::wstring& input) {
    std::wstring s = trim(input);
    while (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = trim(s.substr(1, s.size() - 2));
    if (s.empty()) return {};
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) s = L"\\\\" + s.substr(8);
    else if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    std::wstring out = std::filesystem::path(s).lexically_normal().wstring();
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    if (out.size() == 2 && out[1] == L':') out += L'\\';
    if (out.size() >= 2 && out[1] == L':') out[0] = static_cast<wchar_t>(towupper(out[0]));
    return out;
}

bool isAbsolute(const std::wstring& p) {
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\') return true;
    return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\';
}

bool isUnder(const std::wstring& child, const std::wstring& parentDir) {
    std::wstring c = normalize(child), p = normalize(parentDir);
    if (c.empty() || p.empty()) return false;
    if (iequals(c, p)) return true;
    if (p.back() != L'\\') p += L'\\';
    return startsWithI(c, p);
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
    size_t skip = 0;
    while (skip < b.size() && (b[skip] == L'\\' || b[skip] == L'/')) ++skip;
    r += b.substr(skip);
    for (auto& c : r)
        if (c == L'/') c = L'\\';
    return r;
}

std::wstring parent(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    if (pos == 2 && path.size() > 1 && path[1] == L':') return path.size() > 3 ? path.substr(0, 3) : std::wstring();
    if (pos < 2 && path.rfind(L"\\\\", 0) == 0) return {};
    return path.substr(0, pos);
}

std::wstring fileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring longPath(const std::wstring& p) {
    if (p.size() < 240 || p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

bool hasInvalidChars(const std::wstring& p) {
    for (size_t i = 0; i < p.size(); ++i) {
        wchar_t c = p[i];
        if (c < 32 || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*') return true;
        if (c == L':' && i != 1) return true;
    }
    return false;
}

static DWORD attrs(const std::wstring& p) { return GetFileAttributesW(longPath(p).c_str()); }

bool exists(const std::wstring& p) { return !p.empty() && attrs(p) != INVALID_FILE_ATTRIBUTES; }
bool isDir(const std::wstring& p) {
    DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : attrs(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool isFile(const std::wstring& p) {
    DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : attrs(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::optional<uint64_t> fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(longPath(p).c_str(), GetFileExInfoStandard, &d)) return std::nullopt;
    return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool ensureDir(const std::wstring& dir, std::vector<std::wstring>* created, unsigned long* error) {
    std::vector<std::wstring> missing;
    std::wstring cur = dir;
    while (!cur.empty() && !isDir(cur)) {
        if (isFile(cur)) {
            if (error) *error = ERROR_DIRECTORY;
            return false;
        }
        missing.push_back(cur);
        std::wstring up = parent(cur);
        if (up == cur) break;
        cur = up;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (!CreateDirectoryW(longPath(*it).c_str(), nullptr)) {
            DWORD e = GetLastError();
            if (e == ERROR_ALREADY_EXISTS) continue;
            if (error) *error = e;
            return false;
        }
        if (created) created->push_back(*it);
    }
    return true;
}

bool isDangerousRoot(const std::wstring& path) {
    std::wstring n = normalize(path);
    if (n.size() <= 3) return true;  // "C:\" or shorter
    if (n.rfind(L"\\\\", 0) == 0 && std::count(n.begin(), n.end(), L'\\') <= 3) return true;  // \\server\share
    const wchar_t* roots[] = {L"Windows", L"ProgramFiles", L"ProgramFilesX86", L"Profile", L"Desktop", L"Documents",
                              L"ProgramData"};
    for (auto r : roots) {
        std::wstring k = knownFolder(r);
        if (!k.empty() && iequals(normalize(k), n)) return true;
    }
    std::wstring win = knownFolder(L"Windows");
    return !win.empty() && isUnder(n, win);
}

std::wstring nearestExisting(const std::wstring& path) {
    std::wstring cur = normalize(path);
    while (!cur.empty() && !isDir(cur)) {
        std::wstring up = parent(cur);
        if (up == cur) return {};
        cur = up;
    }
    return cur;
}

std::optional<uint64_t> freeSpace(const std::wstring& path) {
    std::wstring dir = nearestExisting(path);
    if (dir.empty()) return std::nullopt;
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &avail, nullptr, nullptr)) return std::nullopt;
    return avail.QuadPart;
}

bool canWrite(const std::wstring& dir, unsigned long* error) {
    std::wstring base = nearestExisting(dir);
    if (base.empty()) {
        if (error) *error = ERROR_PATH_NOT_FOUND;
        return false;
    }
    std::wstring probe = join(base, L".fl-write-test-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE h = CreateFileW(longPath(probe).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return false;
    }
    CloseHandle(h);
    return true;
}

std::wstring tempDir() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return normalize(std::wstring(buf, n));
}

std::wstring selfExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring knownFolder(const std::wstring& name) {
    struct Entry {
        const wchar_t* name;
        const KNOWNFOLDERID* id;
    };
    static const Entry table[] = {
        {L"Desktop", &FOLDERID_Desktop},         {L"StartMenu", &FOLDERID_Programs},
        {L"Documents", &FOLDERID_Documents},     {L"LocalAppData", &FOLDERID_LocalAppData},
        {L"RoamingAppData", &FOLDERID_RoamingAppData}, {L"ProgramFiles", &FOLDERID_ProgramFiles},
        {L"ProgramFilesX86", &FOLDERID_ProgramFilesX86}, {L"SavedGames", &FOLDERID_SavedGames},
        {L"Profile", &FOLDERID_Profile},         {L"Windows", &FOLDERID_Windows},
        {L"ProgramData", &FOLDERID_ProgramData}, {L"PublicDesktop", &FOLDERID_PublicDesktop},
        {L"CommonStartMenu", &FOLDERID_CommonPrograms},
    };
    if (iequals(name, L"Temp")) return tempDir();
    for (const auto& e : table) {
        if (!iequals(name, e.name)) continue;
        PWSTR p = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(*e.id, KF_FLAG_DEFAULT, nullptr, &p))) out = p;
        if (p) CoTaskMemFree(p);
        return out;
    }
    return {};
}

bool readFile(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool readText(const std::wstring& path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool writeFile(const std::wstring& path, const void* data, size_t size, unsigned long* error) {
    HANDLE h = CreateFileW(longPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return false;
    }
    DWORD written = 0;
    bool ok = size == 0 || (WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) && written == size);
    if (!ok && error) *error = GetLastError();
    CloseHandle(h);
    return ok;
}

static BCRYPT_ALG_HANDLE shaAlgorithm() {
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        return a;
    }();
    return alg;
}

Sha256::Sha256() {
    DWORD objLen = 0, got = 0;
    BCryptGetProperty(shaAlgorithm(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof objLen, &got, 0);
    object_.resize(objLen);
    BCRYPT_HASH_HANDLE h = nullptr;
    BCryptCreateHash(shaAlgorithm(), &h, object_.data(), objLen, nullptr, 0, 0);
    hash_ = h;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
}

void Sha256::update(const void* data, size_t size) {
    if (size) BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), (PUCHAR)data, static_cast<ULONG>(size), 0);
}

void Sha256::raw(uint8_t out[32]) { BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), out, 32, 0); }

std::string Sha256::hex() {
    uint8_t digest[32];
    raw(digest);
    return narrow(hexEncode(digest, 32));
}

std::string sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    Sha256 sha;
    std::vector<uint8_t> buf(1 << 20);
    DWORD read = 0;
    bool ok = true;
    while ((ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) != 0) && read > 0)
        sha.update(buf.data(), read);
    CloseHandle(h);
    return ok ? sha.hex() : std::string();
}

}  // namespace fl::fs
