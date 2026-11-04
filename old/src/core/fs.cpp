#include "fs.h"

#include <algorithm>
#include <filesystem>

#include "strings.h"
#include "win.h"
#ifdef _WIN32
// win.h first: these need the full Windows types.
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace fl::fs {

#ifdef _WIN32
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
#else
// Linux: '/' paths. A '\' from launcher.json or a mod spec ("${GAME_DIR}\Mods") is a separator here too, so the
// shared configs work unchanged; the result always uses '/'.
std::wstring normalize(const std::wstring& input) {
    std::wstring s = trim(input);
    while (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = trim(s.substr(1, s.size() - 2));
    if (s.empty()) return {};
    for (auto& c : s)
        if (c == L'\\') c = L'/';
    std::wstring out = std::filesystem::path(s).lexically_normal().wstring();
    while (out.size() > 1 && out.back() == L'/') out.pop_back();
    return out;
}

bool isAbsolute(const std::wstring& p) { return !p.empty() && (p[0] == L'/' || p[0] == L'\\'); }

bool isUnder(const std::wstring& child, const std::wstring& parentDir) {
    std::wstring c = normalize(child), p = normalize(parentDir);
    if (c.empty() || p.empty()) return false;
    if (c == p) return true;
    if (p.back() != L'/') p += L'/';
    return c.rfind(p, 0) == 0;
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r += L'/';
    size_t skip = 0;
    while (skip < b.size() && (b[skip] == L'\\' || b[skip] == L'/')) ++skip;
    r += b.substr(skip);
    for (auto& c : r)
        if (c == L'\\') c = L'/';
    return r;
}

std::wstring parent(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    if (pos == 0) return path.size() > 1 ? L"/" : std::wstring();
    return path.substr(0, pos);
}

std::wstring fileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring longPath(const std::wstring& p) { return p; }

// The same names are refused as on Windows, so a mod that installs here installs there too.
bool hasInvalidChars(const std::wstring& p) {
    for (wchar_t c : p)
        if (c < 32 || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*') return true;
    return false;
}
#endif

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

bool ensureDir(const std::wstring& dir, std::vector<std::wstring>* created, ErrorCode* error) {
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

#ifndef _WIN32
bool isDangerousRoot(const std::wstring& path) {
    std::wstring n = normalize(path);
    if (n.size() <= 1) return true;  // "/"
    for (const wchar_t* sys : {L"/usr", L"/etc", L"/bin", L"/sbin", L"/lib", L"/lib64", L"/boot", L"/dev", L"/proc",
                               L"/sys", L"/var", L"/opt", L"/home", L"/root", L"/run", L"/srv", L"/mnt", L"/media",
                               L"/tmp"})
        if (n == sys) return true;
    for (const wchar_t* r : {L"Profile", L"Desktop", L"Documents"}) {
        std::wstring k = knownFolder(r);
        if (!k.empty() && normalize(k) == n) return true;
    }
    for (const wchar_t* sys : {L"/usr", L"/etc", L"/bin", L"/sbin", L"/boot", L"/proc", L"/sys", L"/dev"})
        if (isUnder(n, sys)) return true;
    return false;
}
#else
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
#endif

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

bool canWrite(const std::wstring& dir, ErrorCode* error) {
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

#ifdef _WIN32
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
#else
namespace {

std::wstring envDir(const char* var, const std::wstring& fallback) {
    const char* v = getenv(var);
    return v && *v == '/' ? widen(v) : fallback;
}

// XDG_DESKTOP_DIR="$HOME/Desktop" in ~/.config/user-dirs.dirs, as xdg-user-dir reads it.
std::wstring userDir(const char* key, const wchar_t* fallback) {
    std::wstring home = envDir("HOME", L"/tmp");
    std::string text;
    if (readText(envDir("XDG_CONFIG_HOME", home + L"/.config") + L"/user-dirs.dirs", text)) {
        std::string prefix = std::string(key) + "=\"";
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (line.rfind(prefix, 0) != 0 || line.size() < prefix.size() + 1 || line.back() != '"') continue;
            std::string value = line.substr(prefix.size(), line.size() - prefix.size() - 1);
            if (value.rfind("$HOME", 0) == 0) return home + widen(value.substr(5));
            if (!value.empty() && value[0] == '/') return widen(value);
        }
    }
    return home + L"/" + fallback;
}

}  // namespace

// The Windows folder names launcher.json uses, mapped to their Linux places. Program Files and Windows have none:
// a Windows game's copy of them is inside its Proton prefix (mod/detect.cpp).
std::wstring knownFolder(const std::wstring& name) {
    std::wstring home = envDir("HOME", L"/tmp");
    if (iequals(name, L"Temp")) return tempDir();
    if (iequals(name, L"Profile")) return home;
    if (iequals(name, L"Desktop")) return userDir("XDG_DESKTOP_DIR", L"Desktop");
    if (iequals(name, L"Documents")) return userDir("XDG_DOCUMENTS_DIR", L"Documents");
    if (iequals(name, L"LocalAppData")) return envDir("XDG_DATA_HOME", home + L"/.local/share");
    if (iequals(name, L"RoamingAppData")) return envDir("XDG_CONFIG_HOME", home + L"/.config");
    if (iequals(name, L"SavedGames")) return envDir("XDG_DATA_HOME", home + L"/.local/share");
    if (iequals(name, L"StartMenu")) return envDir("XDG_DATA_HOME", home + L"/.local/share") + L"/applications";
    if (iequals(name, L"CommonStartMenu")) return L"/usr/share/applications";
    return {};
}
#endif

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

bool writeFile(const std::wstring& path, const void* data, size_t size, ErrorCode* error) {
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

#ifdef _WIN32
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
#else
// Linux: SHA-256 as FIPS 180-4 gives it (no crypto library to depend on). The state lives in object_.
namespace {

struct ShaState {
    uint32_t h[8];
    uint8_t block[64];
    uint64_t total;
    size_t used;
};

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(ShaState& s, const uint8_t* p) {
    uint32_t m[64];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + m[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s.h[0] += a, s.h[1] += b, s.h[2] += c, s.h[3] += d, s.h[4] += e, s.h[5] += f, s.h[6] += g, s.h[7] += h;
}

}  // namespace

Sha256::Sha256() {
    object_.resize(sizeof(ShaState));
    auto* s = reinterpret_cast<ShaState*>(object_.data());
    const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::copy(init, init + 8, s->h);
    s->total = 0;
    s->used = 0;
    hash_ = s;
}

Sha256::~Sha256() = default;

void Sha256::update(const void* data, size_t size) {
    auto& s = *static_cast<ShaState*>(hash_);
    const auto* p = static_cast<const uint8_t*>(data);
    s.total += size;
    while (size) {
        size_t take = std::min(size, 64 - s.used);
        std::copy(p, p + take, s.block + s.used);
        s.used += take, p += take, size -= take;
        if (s.used == 64) {
            compress(s, s.block);
            s.used = 0;
        }
    }
}

void Sha256::raw(uint8_t out[32]) {
    auto& s = *static_cast<ShaState*>(hash_);
    uint64_t bits = s.total * 8;
    uint8_t pad[72] = {0x80};
    size_t padLen = (s.used < 56 ? 56 : 120) - s.used;
    for (int i = 0; i < 8; ++i) pad[padLen + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(pad, padLen + 8);
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = static_cast<uint8_t>(s.h[i] >> (24 - 8 * k));
}
#endif

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
