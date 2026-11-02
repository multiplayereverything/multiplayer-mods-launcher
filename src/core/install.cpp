#include "install.h"

#include <algorithm>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <vector>

#include "fs.h"
#include "json.h"
#include "log.h"
#include "process.h"
#include "strings.h"
#include "win.h"
// win.h first: winhttp needs the full Windows types.
#include <winhttp.h>

namespace fl {

namespace {

class Handle {
public:
    explicit Handle(HINTERNET h = nullptr) : h_(h) {}
    ~Handle() {
        if (h_) WinHttpCloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_;
};

std::wstring lastError(const wchar_t* what) { return std::wstring(what) + L": " + win32ErrorMessage(GetLastError()); }

}  // namespace

bool isRateLimitMessage(const std::wstring& text) { return text.rfind(L"GitHub only allows 60 requests", 0) == 0; }

std::wstring rateLimitMessage(int64_t secondsLeft) {
    if (secondsLeft < 1) secondsLeft = 1;
    int64_t minutes = (secondsLeft + 59) / 60;
    std::wstring wait = minutes <= 1 ? std::wstring(L"about a minute")
                        : minutes < 60 ? std::to_wstring(minutes) + L" minutes"
                                       : std::to_wstring(minutes / 60) + L" h " + std::to_wstring(minutes % 60) + L" min";
    return L"GitHub only allows 60 requests an hour and that limit is used up. It answers again in " + wait + L".";
}

namespace {

// GET `url`, following redirects (GitHub sends downloads to its CDN). The body
// goes to `sink`; returns false with `error` set on failure or cancel.
bool httpGet(const std::wstring& url, const std::function<bool(const char*, size_t)>& sink, uint64_t* contentLength,
             const std::atomic<bool>* cancel, std::wstring& error) {
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256] = L"", path[4096] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[2048] = L"";
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = L"Bad download address: " + url;
        return false;
    }
    Handle session(WinHttpOpen(L"MultiplayerEverything-Launcher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = lastError(L"WinHttpOpen");
        return false;
    }
    Handle conn(WinHttpConnect(session.get(), host, uc.nPort, 0));
    std::wstring object = std::wstring(path) + extra;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle req(conn ? WinHttpOpenRequest(conn.get(), L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
                    : nullptr);
    if (!req) {
        error = lastError((L"Connecting to " + std::wstring(host)).c_str());
        return false;
    }
    // GitHub's API answers JSON only to clients that send an Accept header.
    const wchar_t* headers = L"Accept: application/vnd.github+json, */*\r\n";
    if (!WinHttpSendRequest(req.get(), headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        error = lastError((L"Downloading from " + std::wstring(host)).c_str());
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = std::wstring(host) + L" answered HTTP " + std::to_wstring(status);
        // GitHub allows 60 requests an hour without signing in: say how long until it answers again.
        if (status == 403 || status == 429) {
            auto header = [&](const wchar_t* name) {
                wchar_t v[64] = L"";
                DWORD vsize = sizeof v;
                std::wstring query = name;
                return WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CUSTOM, query.c_str(), v, &vsize, WINHTTP_NO_HEADER_INDEX)
                           ? std::wstring(v)
                           : std::wstring();
            };
            std::wstring remaining = header(L"X-RateLimit-Remaining"), reset = header(L"X-RateLimit-Reset"),
                         retry = header(L"Retry-After");
            int64_t now = static_cast<int64_t>(std::time(nullptr));
            int64_t resetAt = !reset.empty() ? _wcstoi64(reset.c_str(), nullptr, 10)
                              : !retry.empty() ? now + _wcstoi64(retry.c_str(), nullptr, 10)
                                               : 0;
            if (resetAt > 0 && (remaining == L"0" || !retry.empty()))
                error = rateLimitMessage(resetAt - now);
        }
        return false;
    }
    if (contentLength) {
        wchar_t len[32] = L"";
        DWORD lsize = sizeof len;
        *contentLength = WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len,
                                             &lsize, WINHTTP_NO_HEADER_INDEX)
                             ? _wcstoui64(len, nullptr, 10)
                             : 0;
    }
    std::vector<char> buf(256 * 1024);
    for (;;) {
        if (cancel && *cancel) {
            error = L"Cancelled";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req.get(), buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            error = lastError(L"Download interrupted");
            return false;
        }
        if (got == 0) return true;
        if (!sink(buf.data(), got)) {
            error = L"Could not write the download to disk";
            return false;
        }
    }
}

struct Asset {
    std::wstring url, name, version, sha256;
    uint64_t size = 0;
};

bool findAsset(const InstallSpec& spec, Asset& out, const std::atomic<bool>* cancel, std::wstring& error) {
    if (!spec.url.empty()) {
        out.url = spec.url;
        out.name = fs::fileName(replaceAll(spec.url, L"/", L"\\"));
        out.sha256 = toLower(spec.sha256);
        return true;
    }
    std::string body;
    std::wstring base = spec.api.empty() ? std::wstring(L"https://api.github.com") : spec.api;
    std::wstring api = base + L"/repos/" + widen(spec.github) + L"/releases/latest";
    if (!httpGet(api, [&](const char* d, size_t n) { body.append(d, n); return body.size() < 8 * 1024 * 1024; },
                 nullptr, cancel, error))
        return false;
    std::string perr;
    json::Value rel = json::parse(body, &perr);
    const json::Value* assets = rel.isObject() ? rel.find("assets") : nullptr;
    if (!assets || !assets->isArray()) {
        error = L"GitHub returned no release information for " + widen(spec.github);
        return false;
    }
    if (const json::Value* tag = rel.find("tag_name"); tag && tag->isString()) out.version = widen(tag->str());
    for (size_t i = 0; i < assets->size(); ++i) {
        const json::Value& a = assets->at(i);
        const json::Value* name = a.find("name");
        const json::Value* url = a.find("browser_download_url");
        if (!name || !name->isString() || !url || !url->isString()) continue;
        if (!globMatch(spec.asset, widen(name->str()))) continue;
        out.name = widen(name->str());
        out.url = widen(url->str());
        if (const json::Value* sz = a.find("size"); sz && sz->isNumber()) out.size = static_cast<uint64_t>(sz->number());
        if (const json::Value* dg = a.find("digest"); dg && dg->isString() && dg->str().rfind("sha256:", 0) == 0)
            out.sha256 = toLower(widen(dg->str().substr(7)));
        return true;
    }
    error = L"The latest " + widen(spec.github) + L" release has no file matching " + spec.asset;
    return false;
}

// Finds `exe` in `dir` or one folder down (some archives wrap everything in a
// top folder). Returns the folder that contains it, or empty.
std::wstring folderWith(const std::wstring& dir, const std::wstring& exe) {
    if (fs::isFile(fs::join(dir, exe))) return dir;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_directory() && fs::isFile(fs::join(e.path().wstring(), exe))) return e.path().wstring();
    return {};
}

void removeTree(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::remove_all(fs::longPath(dir), ec);
}

// Downloads `url` to `dest`, adding to st.done as bytes arrive. Checks the size
// and SHA-256 when known; on any failure `dest` is deleted.
bool downloadVerified(const std::wstring& url, const std::wstring& dest, const std::wstring& sha256, uint64_t size,
                      InstallStatus& st, const InstallProgress& progress, const std::atomic<bool>* cancel,
                      std::wstring& error) {
    FILE* f = _wfopen(fs::longPath(dest).c_str(), L"wb");
    if (!f) {
        error = L"Cannot write " + dest;
        return false;
    }
    fs::Sha256 hash;
    uint64_t got = 0, length = 0;
    bool ok = httpGet(
        url,
        [&](const char* d, size_t n) {
            hash.update(d, n);
            got += n;
            st.done += n;
            if (!st.total && length) st.total = length;
            if (progress) progress(st);
            return fwrite(d, 1, n, f) == n;
        },
        &length, cancel, error);
    fclose(f);
    if (ok && size && got != size) {
        error = L"The download is incomplete (" + formatBytes(got) + L" of " + formatBytes(size) + L"; " +
                std::to_wstring(got) + L" of " + std::to_wstring(size) + L" bytes)";
        ok = false;
    }
    if (ok && !sha256.empty() && widen(hash.hex()) != toLower(sha256)) {
        error = L"The download doesn't match its published checksum, so it was not installed.";
        ok = false;
    }
    if (!ok) DeleteFileW(fs::longPath(dest).c_str());
    return ok;
}

// Release tags are "v1.2.0"; the shared version compare wants "1.2.0".
std::wstring_view bareVersion(std::wstring_view v) {
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.remove_prefix(1);
    return v;
}

}  // namespace

bool downloadImage(const std::wstring& url, const std::wstring& dest, uint64_t maxBytes, std::wstring& error) {
    if (!startsWithI(url, L"https://") && !startsWithI(url, L"http://127.0.0.1")) {
        error = L"Pictures are only fetched over https";
        return false;
    }
    std::wstring part = dest + L".part";
    fs::ensureDir(fs::parent(dest), nullptr, nullptr);
    FILE* f = _wfopen(fs::longPath(part).c_str(), L"wb");
    if (!f) {
        error = L"Cannot write " + part;
        return false;
    }
    uint64_t got = 0, length = 0;
    bool ok = httpGet(
        url,
        [&](const char* d, size_t n) {
            got += n;
            return got <= maxBytes && fwrite(d, 1, n, f) == n;
        },
        &length, nullptr, error);
    fclose(f);
    if (ok && got > maxBytes) ok = false, error = L"The picture is too large";
    if (ok) {
        std::string text;
        fs::readText(part, text);
        static const unsigned char kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        bool png = text.size() > 8 && memcmp(text.data(), kPng, sizeof kPng) == 0;
        bool jpg = text.size() > 3 && static_cast<uint8_t>(text[0]) == 0xFF && static_cast<uint8_t>(text[1]) == 0xD8;
        bool webp = text.size() > 12 && memcmp(text.data(), "RIFF", 4) == 0 && memcmp(text.data() + 8, "WEBP", 4) == 0;
        if (!png && !jpg && !webp) ok = false, error = L"That isn't a PNG, JPEG or WebP picture";
    }
    if (ok && !MoveFileExW(fs::longPath(part).c_str(), fs::longPath(dest).c_str(), MOVEFILE_REPLACE_EXISTING))
        ok = false, error = lastError((L"Cannot save " + dest).c_str());
    if (!ok) DeleteFileW(fs::longPath(part).c_str());
    return ok;
}

InstallResult installPlayer(const PlayerSpec& player, const Vars& vars, const InstallProgress& progress,
                            const std::atomic<bool>* cancel) {
    InstallResult r;
    InstallStatus st;
    auto report = [&](InstallStatus::Stage s) {
        st.stage = s;
        if (progress) progress(st);
    };
    auto fail = [&](const std::wstring& msg) {
        r.error = msg;
        logger().error(L"Installing " + player.name + L" failed: " + msg);
        report(InstallStatus::Stage::Failed);
        return r;
    };
    const InstallSpec& spec = player.install;
    if (!spec.enabled()) return fail(L"No download is configured for " + player.name);
    std::wstring target = fs::normalize(vars.expand(spec.dir));
    std::wstring exe = spec.exe.empty() ? std::wstring(L"ruffle.exe") : spec.exe;
    if (target.empty() || fs::isDangerousRoot(target)) return fail(L"Unsafe install folder: " + target);

    report(InstallStatus::Stage::Finding);
    Asset asset;
    std::wstring error;
    if (!findAsset(spec, asset, cancel, error)) return fail(error);
    r.version = asset.version;
    logger().info(L"Installing " + player.name + L" " + asset.version + L" from " + asset.url);

    // Download next to the target so the final move is a rename on the same drive.
    std::wstring work = target + L".installing";
    removeTree(work);
    if (!fs::ensureDir(work, nullptr, nullptr)) return fail(L"Cannot create " + work);
    std::wstring archive = fs::join(work, asset.name.empty() ? L"download.zip" : asset.name);
    st.total = asset.size;
    report(InstallStatus::Stage::Downloading);
    if (!downloadVerified(asset.url, archive, asset.sha256, asset.size, st, progress, cancel, error)) {
        removeTree(work);
        return fail(error);
    }

    report(InstallStatus::Stage::Unpacking);
    // tar.exe ships with Windows 10 1803+ and unpacks .zip archives.
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring unpacked = fs::join(work, L"files");
    fs::ensureDir(unpacked, nullptr, nullptr);
    sys::Started tar;
    if (!sys::startProcess(fs::join(sys, L"tar.exe"), {L"-xf", archive, L"-C", unpacked}, work, tar, error, true)) {
        removeTree(work);
        return fail(L"Cannot unpack the download: " + error);
    }
    WaitForSingleObject(tar.process, 120000);
    DWORD code = 1;
    GetExitCodeProcess(tar.process, &code);
    CloseHandle(tar.process);
    std::wstring root = code == 0 ? folderWith(unpacked, exe) : std::wstring();
    if (root.empty()) {
        removeTree(work);
        return fail(code == 0 ? L"The download doesn't contain " + exe : L"Unpacking the download failed");
    }

    // Swap folders: keep the old install until the new one is in place.
    std::wstring old = target + L".old";
    removeTree(old);
    if (fs::exists(target) && !MoveFileExW(fs::longPath(target).c_str(), fs::longPath(old).c_str(), 0)) {
        removeTree(work);
        return fail(lastError((L"Cannot replace " + target).c_str()));
    }
    fs::ensureDir(fs::parent(target), nullptr, nullptr);
    if (!MoveFileExW(fs::longPath(root).c_str(), fs::longPath(target).c_str(), 0)) {
        std::wstring msg = lastError((L"Cannot move the player into " + target).c_str());
        if (fs::exists(old)) MoveFileExW(fs::longPath(old).c_str(), fs::longPath(target).c_str(), 0);
        removeTree(work);
        return fail(msg);
    }
    removeTree(old);
    removeTree(work);
    r.ok = true;
    r.exe = fs::join(target, exe);
    // Remember which release this is, so a newer one can be told apart later.
    if (!asset.version.empty()) {
        std::string tag = narrow(asset.version);
        fs::writeFile(fs::join(target, kPlayerVersionFile), tag.data(), tag.size(), nullptr);
    }
    logger().info(player.name + L" installed: " + r.exe);
    report(InstallStatus::Stage::Done);
    return r;
}

PlayerUpdate updateInstalledPlayer(const PlayerSpec& player, const Vars& vars, const std::atomic<bool>* cancel,
                                   bool install, const InstallProgress& progress) {
    PlayerUpdate u;
    const InstallSpec& spec = player.install;
    if (spec.github.empty()) return u;  // only a player the launcher installed from a release is kept current
    std::wstring target = fs::normalize(vars.expand(spec.dir));
    std::wstring exe = spec.exe.empty() ? std::wstring(L"ruffle.exe") : spec.exe;
    if (target.empty() || !fs::isFile(fs::join(target, exe))) return u;  // not installed by the launcher
    Asset asset;
    std::wstring error;
    if (!findAsset(spec, asset, cancel, error)) {
        u.error = error;
        return u;
    }
    u.checked = true;
    u.version = asset.version;
    // A copy installed before the version was recorded has no file: treat it as older.
    std::string installed;
    fs::readText(fs::join(target, kPlayerVersionFile), installed);
    std::wstring have = trim(widen(installed));
    u.installed = have;
    if (!have.empty() && compareVersions(bareVersion(asset.version), bareVersion(have)) <= 0) return u;
    u.available = true;
    if (!install) return u;
    logger().info(player.name + L" " + (have.empty() ? std::wstring(L"(unknown version)") : have) + L" -> " + asset.version);
    InstallResult r = installPlayer(player, vars, progress, cancel);
    u.updated = r.ok;
    if (!r.ok) u.error = r.error;
    return u;
}

void parseCatalogSettings(std::string_view text, std::map<std::string, std::vector<Setting>>& out) {
    out.clear();
    std::string perr;
    json::Value root = json::parse(text, &perr);
    const json::Value* all = perr.empty() && root.isObject() ? root.find("settings") : nullptr;
    if (!all || !all->isObject()) return;
    for (size_t i = 0; i < all->size(); ++i) {
        const json::Value& list = all->at(i);
        if (!list.isArray()) continue;
        std::vector<ConfigIssue> issues;
        std::vector<Setting> settings = readSettings(list, L"catalog settings." + widen(all->keyAt(i)), issues);
        for (const auto& issue : issues) logger().warn(issue.where + L": " + issue.message);
        if (!settings.empty()) out[all->keyAt(i)] = std::move(settings);
    }
}

bool parseCatalog(std::string_view text, const std::wstring& baseUrl, std::vector<CatalogFile>& out,
                  std::wstring& error) {
    std::string perr;
    json::Value root = json::parse(text, &perr);
    const json::Value* files = root.isObject() ? root.find("files") : nullptr;
    if (!perr.empty() || !files || !files->isArray()) {
        error = L"The game catalog is not valid" + (perr.empty() ? std::wstring() : L" (" + widen(perr) + L")");
        return false;
    }
    std::wstring base = baseUrl.substr(0, baseUrl.rfind(L'/') + 1);
    out.clear();
    for (size_t i = 0; i < files->size(); ++i) {
        const json::Value& f = files->at(i);
        auto str = [&](const char* k) {
            const json::Value* v = f.isObject() ? f.find(k) : nullptr;
            return v && v->isString() ? v->str() : std::string();
        };
        CatalogFile c;
        c.game = str("game");
        c.name = widen(str("name"));
        c.sha256 = toLower(widen(str("sha256")));
        c.description = widen(str("description"));
        std::string path = str("path");
        if (const json::Value* sz = f.isObject() ? f.find("size") : nullptr; sz && sz->isNumber())
            c.size = static_cast<uint64_t>(sz->number());
        // A file name with a folder in it could write outside the games folder.
        if (c.game.empty() || c.name.empty() || path.empty() || c.name.find_first_of(L"\\/:") != std::wstring::npos ||
            c.name.find(L"..") != std::wstring::npos)
            continue;
        c.url = path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0 ? widen(path) : base + widen(path);
        out.push_back(std::move(c));
    }
    return true;
}

std::vector<CatalogFile> outdatedGameFiles(const std::vector<CatalogFile>& catalog, const std::wstring& dir) {
    std::vector<CatalogFile> out;
    for (const auto& f : catalog) {
        std::wstring path = fs::join(dir, f.name);
        if (!f.sha256.empty() && fs::isFile(path) && widen(fs::sha256File(path)) != f.sha256) out.push_back(f);
    }
    return out;
}

std::wstring oldExePath(const std::wstring& exePath) { return exePath + L".old"; }

void removeOldUpdate(const std::wstring& exePath) { DeleteFileW(fs::longPath(oldExePath(exePath)).c_str()); }

UpdateResult updateSelf(const InstallSpec& source, const std::wstring& currentVersion, const std::wstring& exePath,
                        const std::atomic<bool>* cancel) {
    UpdateResult r;
    Asset asset;
    if (!findAsset(source, asset, cancel, r.error)) return r;
    r.version = asset.version;
    if (compareVersions(bareVersion(asset.version), bareVersion(currentVersion)) <= 0) {
        r.ok = true;  // up to date
        return r;
    }
    // Never replace the launcher with a file nobody can vouch for.
    if (asset.sha256.empty()) {
        r.error = L"The " + asset.version + L" release publishes no SHA-256 for " + asset.name + L"; not installed.";
        return r;
    }
    std::wstring fresh = exePath + L".new";
    InstallStatus st;
    st.stage = InstallStatus::Stage::Downloading;
    st.total = asset.size;
    if (!downloadVerified(asset.url, fresh, asset.sha256, asset.size, st, nullptr, cancel, r.error)) return r;
    // A running exe can be renamed but not overwritten: move it aside first.
    std::wstring old = oldExePath(exePath);
    DeleteFileW(fs::longPath(old).c_str());
    if (!MoveFileExW(fs::longPath(exePath).c_str(), fs::longPath(old).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        r.error = lastError(L"Can't replace the launcher (is its folder read-only?)");
        DeleteFileW(fs::longPath(fresh).c_str());
        return r;
    }
    if (!MoveFileExW(fs::longPath(fresh).c_str(), fs::longPath(exePath).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        r.error = lastError(L"Can't put the new launcher in place");
        MoveFileExW(fs::longPath(old).c_str(), fs::longPath(exePath).c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(fs::longPath(fresh).c_str());
        return r;
    }
    r.ok = r.updated = true;
    return r;
}

bool httpGetText(const std::wstring& url, std::string& out, std::wstring& error) {
    out.clear();
    return httpGet(url, [&](const char* d, size_t n) { out.append(d, n); return out.size() < 4 * 1024 * 1024; },
                   nullptr, nullptr, error);
}

bool fetchCatalog(const std::wstring& url, std::vector<CatalogFile>& out, std::string& raw, std::wstring& error,
                  const std::atomic<bool>* cancel) {
    raw.clear();
    if (!httpGet(url, [&](const char* d, size_t n) { raw.append(d, n); return raw.size() < 4 * 1024 * 1024; }, nullptr,
                 cancel, error))
        return false;
    return parseCatalog(raw, url, out, error);
}

InstallResult downloadGameFiles(const std::vector<CatalogFile>& files, const std::wstring& dir,
                                const InstallProgress& progress, const std::atomic<bool>* cancel) {
    InstallResult r;
    InstallStatus st;
    std::vector<const CatalogFile*> todo;
    for (const auto& f : files) {
        std::wstring dest = fs::join(dir, f.name);
        if (fs::isFile(dest) && !f.sha256.empty() && widen(fs::sha256File(dest)) == f.sha256) continue;
        todo.push_back(&f);
        st.total += f.size;
    }
    if (!fs::ensureDir(dir, nullptr, nullptr)) {
        r.error = L"Cannot create " + dir;
        return r;
    }
    st.stage = InstallStatus::Stage::Downloading;
    if (progress) progress(st);
    for (const CatalogFile* f : todo) {
        std::wstring dest = fs::join(dir, f->name), part = dest + L".part";
        logger().info(L"Downloading " + f->url + L" -> " + dest);
        std::wstring error;
        if (!downloadVerified(f->url, part, f->sha256, f->size, st, progress, cancel, error)) {
            r.error = f->name + L": " + error;
            logger().error(L"Download failed: " + r.error);
            st.stage = InstallStatus::Stage::Failed;
            if (progress) progress(st);
            return r;
        }
        // Rename only after the checksum passed, so a half-written file never
        // shows up as a playable game.
        if (!MoveFileExW(fs::longPath(part).c_str(), fs::longPath(dest).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            r.error = lastError((L"Cannot save " + dest).c_str());
            DeleteFileW(fs::longPath(part).c_str());
            return r;
        }
    }
    r.ok = true;
    st.stage = InstallStatus::Stage::Done;
    if (progress) progress(st);
    return r;
}

}  // namespace fl
