#include "core/mod/package.h"

#include <algorithm>
#include <filesystem>
#include <cwctype>
#include <vector>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/process.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::mod {

namespace {

std::wstring field(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return v && v->isString() ? widen(v->str()) : std::wstring();
}

void removeTree(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::remove_all(fs::longPath(dir), ec);
}

std::wstring baseFolder(const std::wstring& base) {
    std::wstring b = base;
    while (!b.empty() && (b.back() == L'/' || b.back() == L'\\')) b.pop_back();
    return b + L"/";
}

}  // namespace

bool fetchLatest(const std::wstring& base, const std::wstring& modsInfoId, Latest& out, std::wstring& error) {
    if (base.empty() || modsInfoId.empty()) {
        error = L"No mods-info address or mod id";
        return false;
    }
    std::string text;
    if (!httpGetText(baseFolder(base) + L"mods/" + modsInfoId + L".json", text, error)) return false;
    std::string perr;
    json::Value v = json::parse(text, &perr);
    if (!perr.empty() || !v.isObject()) {
        error = L"mods-info has no readable file for " + modsInfoId;
        return false;
    }
    out = {};
    out.version = field(v, "latest_version");
    out.minVersion = field(v, "min_version");
    out.downloadUrl = field(v, "download_url");
    out.sha256 = toLower(field(v, "download_sha256"));
    out.status = field(v, "status");
    out.notice = field(v, "notice");
    // Only https downloads; mods-info is read by every player.
    if (!out.downloadUrl.empty() && !startsWithI(out.downloadUrl, L"https://") &&
        !startsWithI(out.downloadUrl, L"http://127.0.0.1")) {
        error = L"mods-info lists a download that isn't https://";
        out.downloadUrl.clear();
        return false;
    }
    return true;
}

std::wstring githubAssetDigest(const std::wstring& assetUrl, const std::wstring& apiBase, std::wstring& error) {
    // https://github.com/<owner>/<repo>/releases/download/<tag>/<file>
    const std::wstring marker = L"github.com/";
    size_t at = assetUrl.find(marker);
    if (at == std::wstring::npos || !startsWithI(assetUrl, L"https://")) {
        error = L"No checksum is published for this download";
        return {};
    }
    auto parts = split(assetUrl.substr(at + marker.size()), L'/');
    if (parts.size() < 6 || parts[2] != L"releases" || parts[3] != L"download") {
        error = L"No checksum is published for this download";
        return {};
    }
    std::wstring api = (apiBase.empty() ? std::wstring(L"https://api.github.com") : apiBase) + L"/repos/" + parts[0] +
                       L"/" + parts[1] + L"/releases/tags/" + parts[4];
    std::string body;
    if (!httpGetText(api, body, error)) return {};
    std::string perr;
    json::Value rel = json::parse(body, &perr);
    const json::Value* assets = rel.isObject() ? rel.find("assets") : nullptr;
    if (assets && assets->isArray())
        for (size_t i = 0; i < assets->size(); ++i) {
            const json::Value& a = assets->at(i);
            if (field(a, "name") != parts[5]) continue;
            std::wstring digest = field(a, "digest");
            if (startsWithI(digest, L"sha256:")) return toLower(digest.substr(7));
        }
    error = L"GitHub publishes no SHA-256 for " + parts[5];
    return {};
}

bool findGithubAsset(const std::wstring& repo, const std::wstring& tag, const std::wstring& assetGlob,
                     const std::wstring& apiBase, GithubAsset& out, std::wstring& error) {
    out = {};
    std::wstring api = (apiBase.empty() ? std::wstring(L"https://api.github.com") : apiBase) + L"/repos/" + repo +
                       (tag.empty() ? L"/releases?per_page=10" : L"/releases/tags/" + tag);
    std::string body;
    if (!httpGetText(api, body, error)) return false;
    std::string perr;
    json::Value root = json::parse(body, &perr);
    // The newest published release first (GitHub lists them newest first); drafts are never installed.
    std::vector<const json::Value*> releases;
    if (root.isArray()) {
        for (size_t i = 0; i < root.size(); ++i) releases.push_back(&root.at(i));
    } else if (root.isObject()) {
        releases.push_back(&root);
    }
    for (const json::Value* rel : releases) {
        if (const json::Value* d = rel->find("draft"); d && d->boolean()) continue;
        const json::Value* assets = rel->find("assets");
        if (!assets || !assets->isArray()) continue;
        for (size_t i = 0; i < assets->size(); ++i) {
            const json::Value& a = assets->at(i);
            std::wstring name = field(a, "name");
            if (!globMatch(toLower(assetGlob), toLower(name))) continue;
            out.url = field(a, "browser_download_url");
            std::wstring digest = field(a, "digest");
            if (startsWithI(digest, L"sha256:")) out.sha256 = toLower(digest.substr(7));
            std::wstring v = field(*rel, "tag_name");
            if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
            out.version = v;
            if (!out.url.empty() && !out.version.empty()) return true;
        }
    }
    error = L"No release of " + repo + L" has a file named " + assetGlob;
    return false;
}

namespace {

uint32_t zipU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t zipU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// Windows' tar.exe skips a zip entry whose name isn't plain ASCII ("Archive entry has empty or unreadable filename")
// and then exits with an error. A readme in the author's language shouldn't refuse the whole mod, so that one failure is
// let through: every other entry the zip's directory lists is there at its size, and some entry really had such a name.
// A recipe that names a skipped file still fails, with "payload missing". Only a zip whose SHA-256 was checked gets here.
bool onlyUnreadableNamesSkipped(const std::wstring& archive, const std::wstring& destDir) {
    std::vector<uint8_t> z;
    if (!fs::readFile(archive, z) || z.size() < 22) return false;
    size_t eocd = z.size() - 22;
    while (zipU32(&z[eocd]) != 0x06054b50) {
        if (eocd == 0 || z.size() - eocd > 22 + 65535) return false;
        --eocd;
    }
    uint32_t count = zipU16(&z[eocd + 10]), at = zipU32(&z[eocd + 16]);
    if (count == 0xFFFF || at == 0xFFFFFFFF) return false;  // Zip64: not read here
    bool skippedOne = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<uint64_t>(at) + 46 > z.size() || zipU32(&z[at]) != 0x02014b50) return false;
        uint32_t size = zipU32(&z[at + 24]), nameLen = zipU16(&z[at + 28]), extra = zipU16(&z[at + 30]), comment = zipU16(&z[at + 32]);
        if (static_cast<uint64_t>(at) + 46 + nameLen > z.size()) return false;
        std::string name(reinterpret_cast<const char*>(&z[at + 46]), nameLen);
        at += 46 + nameLen + extra + comment;
        if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
            skippedOne = true;
            continue;
        }
        std::wstring path = fs::normalize(fs::join(destDir, replaceAll(widen(name), L"/", L"\\")));
        if (!name.empty() && name.back() == '/') {
            if (!fs::isDir(path)) return false;
        } else if (fs::fileSize(path).value_or(UINT64_MAX) != size) {
            return false;
        }
    }
    return skippedOne;
}

}  // namespace

bool unpackZip(const std::wstring& archive, const std::wstring& destDir, std::wstring& error) {
    removeTree(destDir);
    if (!fs::ensureDir(destDir, nullptr, nullptr)) {
        error = L"Cannot create " + destDir;
        return false;
    }
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    sys::Started tar;
    std::wstring startError;
    // tar.exe ships with Windows 10 1803+ and unpacks .zip archives.
    if (!sys::startProcess(fs::join(sysDir, L"tar.exe"), {L"-xf", archive, L"-C", destDir}, fs::parent(archive), tar,
                           startError, true)) {
        error = L"Cannot unpack the package: " + startError;
        return false;
    }
    WaitForSingleObject(tar.process, 180000);
    DWORD code = 1;
    GetExitCodeProcess(tar.process, &code);
    CloseHandle(tar.process);
    if (code != 0 && onlyUnreadableNamesSkipped(archive, destDir)) {
        logger().warn(L"Left out of the package: a file whose name Windows' tar can't read");
        code = 0;
    }
    if (code != 0) {
        error = L"Unpacking the package failed";
        removeTree(destDir);
        return false;
    }
    // Nothing may be a link, and everything must sit inside destDir.
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(fs::longPath(destDir), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink(ec) || !fs::isUnder(fs::normalize(it->path().wstring()), destDir)) {
            error = L"The package contains a link or a path outside its folder; it was not used.";
            removeTree(destDir);
            return false;
        }
    }
    return true;
}

namespace {

// A mod that is a single file (a DLL released on its own) is not a zip: its file name comes from the address, and
// only a plain one is accepted.
bool plainFileNameOf(const std::wstring& url, std::wstring& name) {
    size_t cut = url.find_first_of(L"?#");
    std::wstring u = cut == std::wstring::npos ? url : url.substr(0, cut);
    size_t slash = u.find_last_of(L'/');
    name = slash == std::wstring::npos ? u : u.substr(slash + 1);
    if (name.empty() || name.size() > 80 || name == L"." || name == L"..") return false;
    for (wchar_t c : name)
        if (!(iswalnum(c) || c == L'.' || c == L'-' || c == L'_')) return false;
    return true;
}

bool isZipFile(const std::wstring& path) {
    std::vector<uint8_t> head;
    if (!fs::readFile(path, head) || head.size() < 4) return false;
    return head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4;
}

// Downloads one zip, checks it and unpacks it into `dest`. `sha256` empty: GitHub's digest for the address.
// A download that is not a zip is checked the same way and put into `dest` under its own name.
bool fetchAndUnpack(const std::wstring& root, const std::wstring& name, const std::wstring& url, std::wstring sha256,
                    const std::wstring& dest, const std::wstring& githubApi, const InstallProgress& progress,
                    const std::atomic<bool>* cancel, std::wstring& error) {
    if (sha256.empty()) {
        std::wstring digestError;
        sha256 = githubAssetDigest(url, githubApi, digestError);
        if (sha256.empty()) {
            error = (digestError.empty() ? std::wstring(L"No checksum is published for this download") : digestError) +
                    L" The package was not installed.";
            return false;
        }
    }
    CatalogFile file;
    file.name = name;
    file.url = url;
    file.sha256 = sha256;
    InstallResult dl = downloadGameFiles({file}, root, progress, cancel);
    if (!dl.ok) {
        error = dl.error;
        return false;
    }
    std::wstring archive = fs::join(root, name);
    if (!isZipFile(archive)) {
        std::wstring fileName;
        if (!plainFileNameOf(url, fileName)) {
            error = L"The download is not a zip and its name is not a plain file name";
            return false;
        }
        if (!fs::ensureDir(dest, nullptr, nullptr) || !MoveFileExW(fs::longPath(archive).c_str(),
                                                                   fs::longPath(fs::join(dest, fileName)).c_str(), 0)) {
            error = L"Could not put " + fileName + L" in the package folder";
            return false;
        }
        return true;
    }
    if (!unpackZip(archive, dest, error)) return false;
    DeleteFileW(fs::longPath(archive).c_str());
    return true;
}

}  // namespace

bool preparePackage(const std::wstring& id, const std::wstring& version, const std::wstring& url, std::wstring sha256,
                    const std::vector<ExtraDownload>& extras, const std::wstring& cacheDir, const std::wstring& githubApi,
                    const InstallProgress& progress, const std::atomic<bool>* cancel, std::wstring& payloadDir,
                    std::wstring& error) {
    if (url.empty()) {
        error = L"This mod has no download yet";
        return false;
    }
    std::wstring root = fs::join(fs::join(cacheDir, id), version);
    payloadDir = fs::join(root, L"payload");
    std::wstring marker = fs::join(root, L"verified.txt");
    sha256 = toLower(sha256);
    std::wstring digestError;
    if (sha256.empty()) sha256 = githubAssetDigest(url, githubApi, digestError);
    // Every download's checksum is learned before anything is fetched: nothing is installed unchecked.
    std::vector<std::wstring> extraSums;
    for (const auto& e : extras) {
        std::wstring sum = toLower(e.sha256);
        std::wstring err;
        if (sum.empty()) sum = githubAssetDigest(e.url, githubApi, err);
        (void)err;
        extraSums.push_back(sum);  // empty offline: fine for a cached copy, refused below when a download is needed
    }
    // What the cached copy must have been made from: the package and each extra, by checksum.
    std::wstring expected = sha256;
    bool sumsKnown = true;
    for (const auto& s : extraSums) {
        expected += L"+" + s;
        sumsKnown = sumsKnown && !s.empty();
    }

    // Already downloaded and unpacked: reuse it (this also lets Repair work offline).
    std::string saved;
    if (fs::readText(marker, saved) && fs::isDir(payloadDir) && (sha256.empty() || trim(widen(saved)) == expected ||
                                                              (!sumsKnown && trim(widen(saved)).rfind(sha256, 0) == 0))) {
        bool allThere = true;
        for (const auto& e : extras) allThere = allThere && fs::isDir(fs::join(payloadDir, e.dir));
        if (allThere) {
            logger().info(L"Using the cached package " + root);
            return true;
        }
    }
    if (sha256.empty()) {
        error = digestError.empty() ? L"No checksum is published for this download" : digestError;
        error += L" The package was not installed.";
        return false;
    }
    for (size_t i = 0; i < extraSums.size(); ++i)
        if (extraSums[i].empty()) {
            error = L"No checksum is published for " + extras[i].url + L". The package was not installed.";
            return false;
        }

    removeTree(root);
    if (!fetchAndUnpack(root, L"package.zip", url, sha256, payloadDir, githubApi, progress, cancel, error)) {
        removeTree(root);
        return false;
    }
    for (size_t i = 0; i < extras.size(); ++i) {
        std::wstring dir = fs::join(payloadDir, extras[i].dir);
        if (fs::exists(dir)) {
            error = L"The package already has a folder named " + extras[i].dir;
            removeTree(root);
            return false;
        }
        if (!fetchAndUnpack(root, L"extra-" + std::to_wstring(i) + L".zip", extras[i].url, extraSums[i], dir, githubApi,
                            progress, cancel, error)) {
            removeTree(root);
            return false;
        }
    }
    std::string text = narrow(expected);
    fs::writeFile(marker, text.data(), text.size(), nullptr);
    return true;
}

void discardPackage(const std::wstring& cacheDir, const std::wstring& id, const std::wstring& version) {
    removeTree(fs::join(fs::join(cacheDir, id), version));
}

void pruneCache(const std::wstring& cacheDir, const std::wstring& id, const std::wstring& keepVersion) {
    std::wstring dir = fs::join(cacheDir, id);
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(fs::longPath(dir), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec) && !iequals(it->path().filename().wstring(), keepVersion))
            removeTree(it->path().wstring());
}

}  // namespace fl::mod
