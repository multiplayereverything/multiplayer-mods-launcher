// A mod's downloadable package: what mods-info says about the newest version,
// then download, SHA-256 check and unpack into the launcher's cache. The
// package is a .zip whose top folder is laid out the way the mod's components
// expect (component "source" paths are relative to it).
#pragma once
#include <atomic>
#include <string>
#include <vector>

#include "core/install.h"  // InstallProgress, InstallStatus

namespace fl::mod {

// One mod's file in mods-info (mods/<id>.json), the part that matters here.
struct Latest {
    std::wstring version, minVersion, downloadUrl, sha256, status, notice;
    // A package exists to install.
    bool published() const { return !downloadUrl.empty() && !version.empty(); }
};

// `base` is mods-info's folder address ("https://raw.githubusercontent.com/<org>/mods-info/main/").
bool fetchLatest(const std::wstring& base, const std::wstring& modsInfoId, Latest& out, std::wstring& error);

// SHA-256 GitHub publishes for a release asset address
// (https://github.com/<owner>/<repo>/releases/download/<tag>/<file>); empty with `error` set otherwise.
std::wstring githubAssetDigest(const std::wstring& assetUrl, const std::wstring& apiBase, std::wstring& error);

// Unpacks a .zip with Windows' own tar.exe; rejects links and anything that lands outside `destDir`.
bool unpackZip(const std::wstring& archive, const std::wstring& destDir, std::wstring& error);

// An asset of a GitHub release: the newest release of `repo` (pre-releases count) when `tag` is empty,
// else that tag; the asset whose name matches the glob `assetGlob`. Fills the download address and
// the SHA-256 GitHub publishes for it (empty when it publishes none); `version` is the tag without a leading "v".
struct GithubAsset {
    std::wstring version, url, sha256;
};
bool findGithubAsset(const std::wstring& repo, const std::wstring& tag, const std::wstring& assetGlob,
                     const std::wstring& apiBase, GithubAsset& out, std::wstring& error);

// A second download unpacked into the payload under `dir`.
struct ExtraDownload {
    std::wstring dir, url, sha256;
};

// Makes the package ready in `cacheDir`\<id>\<version>\payload (kept for repairs) and returns that folder.
// `extras` are unpacked beside it, each into payload\<dir>.
// `sha256` may be empty for a GitHub release asset: GitHub's own digest is used. A package whose
// checksum can't be learned is refused, never installed unchecked. `githubApi` empty = api.github.com.
bool preparePackage(const std::wstring& id, const std::wstring& version, const std::wstring& url, std::wstring sha256,
                    const std::vector<ExtraDownload>& extras, const std::wstring& cacheDir, const std::wstring& githubApi,
                    const InstallProgress& progress, const std::atomic<bool>* cancel, std::wstring& payloadDir,
                    std::wstring& error);

// Deletes the cached copy of one version, so the next install downloads and unpacks it afresh.
void discardPackage(const std::wstring& cacheDir, const std::wstring& id, const std::wstring& version);

// Removes cached versions of `id` other than `keepVersion`.
void pruneCache(const std::wstring& cacheDir, const std::wstring& id, const std::wstring& keepVersion);

}  // namespace fl::mod
