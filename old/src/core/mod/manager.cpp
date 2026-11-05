#include "core/mod/manager.h"

#include <cstdio>
#include <ctime>

#include "core/fs.h"
#include "core/log.h"
#include "core/mod/package.h"
#include "core/mod/resources.h"
#include "core/json.h"
#include "core/process.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::mod {

Vars withSpec(const Vars& base, const Spec& spec) {
    Vars v = base;
    Vars own = spec.baseVars();
    for (const wchar_t* name : {L"MOD_ID", L"MOD_NAME", L"MOD_VERSION", L"MOD_PUBLISHER", L"MOD_WEBSITE",
                                L"MOD_SUPPORT_URL", L"MOD_DESCRIPTION", L"GAME_NAME", L"GAME_EXE", L"STEAM_APP_ID"})
        v.set(name, own.get(name));
    return v;
}

GameLocation findGame(const Spec& spec, const Vars& base, const std::wstring& chosenDir) {
    Vars v = withSpec(base, spec);
    if (!chosenDir.empty()) {
        std::wstring dir = fs::normalize(chosenDir);
        if (looksLikeGameDir(spec, v, dir)) return {dir, L"Chosen folder"};
    }
    for (const auto& c : detectGame(spec, v))
        if (c.valid) return {c.path, c.source};
    return {};
}

std::wstring missingRequirement(const Spec& spec, const Vars& base, const std::wstring& gameDir) {
    if (gameDir.empty()) return {};
    Vars v = withSpec(base, spec);
    for (const auto& r : spec.install.requires_)
        if (!fs::exists(fs::join(gameDir, replaceAll(normRel(v.expand(r.path)), L"/", L"\\"))))
            return r.message.empty() ? L"The game folder needs " + r.path + L" first" : v.expand(r.message);
    return {};
}

Status inspect(const Spec& spec, const Vars& base, const std::wstring& chosenDir) {
    Status s;
    s.game = findGame(spec, base, chosenDir);
    if (!s.game.found()) return s;
    Vars v = resolveDirs(spec, withSpec(base, spec), s.game.dir);
    std::wstring stateDir = v.get(L"STATE_DIR");
    std::wstring error;
    if (auto st = loadState(stateDir, &error)) {
        if (!iequals(st->modId, spec.mod.id)) {
            s.problem = spec.t("error.foreign", v, {{L"OTHER", st->modName}});
        } else if (std::wstring e = checkRecord(spec, *st, stateDir, s.game.dir, withSpec(base, spec)); !e.empty()) {
            s.problem = e;
        } else {
            s.installed = true;
            s.installedVersion = st->version;
        }
    } else if (fs::isFile(fs::join(stateDir, kManifestName))) {
        s.problem = error;  // there, but unreadable: never guess what the mod owns
    }
    s.running = sys::isProcessRunning(spec.game.processNames);
    s.missing = missingRequirement(spec, base, s.game.dir);
    return s;
}

Outcome install(const Spec& spec, const Vars& base, Operation op, const std::wstring& gameDir,
                const std::wstring& payloadDir, const ProgressFn& progress, CancelToken& cancel,
                EngineOptions options) {
    Outcome o;
    Vars v = withSpec(base, spec);
    // Components that copy a file the game already has: put the copy into the payload folder (ours, in the cache)
    // so the engine installs it like any other file.
    Spec local = spec;
    for (auto& comp : local.components) {
        if (comp.sourceGame.empty()) continue;
        std::wstring found;
        for (const auto& rel : comp.sourceGame) {
            std::wstring path = fs::join(gameDir, replaceAll(normRel(v.expand(rel)), L"/", L"\\"));
            if (fs::isFile(path)) {
                found = path;
                break;
            }
        }
        if (found.empty()) {
            o.message = L"The game folder doesn't have " + comp.sourceGame.back() + L". Is it the version this mod needs?";
            return o;
        }
        std::wstring name = comp.targetName.empty() ? fs::fileName(found) : comp.targetName;
        std::wstring rel = L"_game/" + widen(comp.id) + L"/" + name;
        std::wstring dest = fs::join(payloadDir, replaceAll(rel, L"/", L"\\"));
        fs::ensureDir(fs::parent(dest), nullptr, nullptr);
        if (!CopyFileW(fs::longPath(found).c_str(), fs::longPath(dest).c_str(), FALSE)) {
            o.message = L"Cannot read " + found;
            return o;
        }
        comp.source = rel;
        comp.targetName = name;
        comp.sourceGame.clear();
    }
    const Spec& spec_ = local;
    auto res = openDirectory(payloadDir);
    Selection sel = defaultSelection(spec_, std::nullopt);
    // Updating or repairing keeps what was chosen at the first install.
    Vars dirs = resolveDirs(spec_, v, gameDir);
    if (auto prev = loadState(dirs.get(L"STATE_DIR"))) sel = defaultSelection(spec_, prev);
    sel.gameDir = gameDir;
    PlanResult plan = buildPlan(spec_, *res, op, sel, v);
    if (!plan.ok) {
        o.message = plan.error;
        o.detail = plan.detail;
        o.packageDamaged = plan.packageDamaged;
        return o;
    }
    Engine engine(spec_, res.get(), std::move(options));
    return engine.run(plan.plan, progress, cancel);
}

Outcome uninstall(const Spec& spec, const Vars& base, const std::wstring& gameDir, const ProgressFn& progress,
                  CancelToken& cancel, EngineOptions options) {
    Outcome o;
    Vars v = withSpec(base, spec);
    Vars dirs = resolveDirs(spec, v, gameDir);
    PlanResult plan = buildUninstallPlan(spec, dirs.get(L"STATE_DIR"), v);
    if (!plan.ok) {
        o.message = plan.error;
        o.detail = plan.detail;
        return o;
    }
    Engine engine(spec, nullptr, std::move(options));
    return engine.run(plan.plan, progress, cancel);
}

namespace {

// `known`: what was resolved last time. A loader pinned by tag never changes, so it is taken from there instead of
// asking GitHub again: checking for a mod's new release stays one question, not one per download.
bool resolveSourceUncached(const Spec& spec, const std::wstring& modsInfoBase, Source& out, std::wstring& error,
                           const std::wstring& githubApi, const Source* known = nullptr) {
    out = {};
    if (!spec.package.github.empty()) {
        GithubAsset a;
        if (!findGithubAsset(spec.package.github, L"", spec.package.asset.empty() ? L"*.zip" : spec.package.asset, githubApi, a,
                             error))
            return false;
        out.version = a.version;
        out.url = a.url;
        out.sha256 = a.sha256;
    } else if (!spec.package.modsInfo.empty()) {
        Latest l;
        if (!modsInfoBase.empty() && !fetchLatest(modsInfoBase, spec.package.modsInfo, l, error)) return false;
        out.version = l.version;
        out.url = l.downloadUrl;
        out.sha256 = l.sha256;
        out.minVersion = l.minVersion;
        out.notice = l.notice;
    }
    // A package pinned in launcher.json wins over mods-info.
    if (!spec.package.url.empty()) {
        out.url = spec.package.url;
        out.sha256 = toLower(spec.package.sha256);
        out.version = !spec.package.version.empty() ? spec.package.version : out.version.empty() ? L"1" : out.version;
    }
    for (const auto& e : spec.package.extras) {
        ExtraDownload d;
        d.dir = e.dir;
        d.sha256 = toLower(e.sha256);
        const ExtraDownload* before = nullptr;
        if (known && !e.tag.empty())
            for (const auto& k : known->extras)
                if (iequals(k.dir, e.dir) && !k.url.empty() && !k.sha256.empty()) before = &k;
        if (!e.url.empty()) {
            d.url = e.url;
        } else if (before) {
            d.url = before->url;
            if (d.sha256.empty()) d.sha256 = before->sha256;
        } else {
            GithubAsset a;
            if (!findGithubAsset(e.github, e.tag, e.asset, githubApi, a, error)) return false;
            d.url = a.url;
            if (d.sha256.empty()) d.sha256 = a.sha256;
        }
        out.extras.push_back(std::move(d));
    }
    return true;
}

// What the answer depends on: a changed recipe never reuses an old answer.
std::string sourceSignature(const Spec& spec) {
    std::string sig = narrow(spec.package.github + L"|" + spec.package.asset + L"|" + spec.package.url + L"|" + spec.package.version);
    for (const auto& e : spec.package.extras)
        sig += "|" + narrow(e.dir + L"," + e.github + L"," + e.tag + L"," + e.asset + L"," + e.url + L"," + e.sha256);
    return sig;
}

constexpr int64_t kSourceCacheSeconds = 600;  // a new release shows within ten minutes of its publishing

constexpr int64_t kReleasesMaxAge = 3 * 24 * 3600;  // the job that writes releases.json runs at least daily
constexpr int64_t kReleasesClockSkew = 3600;
const wchar_t* const kOurOwner = L"multiplayereverything/";

bool plainSegment(const std::wstring& s) {
    if (s.empty() || s.size() > 120 || s == L"." || s == L"..") return false;
    for (wchar_t c : s)
        if (!(c < 128 && (iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'+'))) return false;
    return true;
}

bool isOurs(const std::wstring& repo) { return startsWithI(repo, kOurOwner); }

// Exactly <repo>'s release file <tag>/<asset> on github.com, the repository's spelling aside. A stand-in server on this
// PC counts only when mods-info itself is one, as for the records (the tests).
bool isReleaseUrl(const std::wstring& url, const std::wstring& repo, const std::wstring& tag, const std::wstring& asset, bool local) {
    if (!plainSegment(tag) || !plainSegment(asset)) return false;
    std::wstring rest;
    const std::wstring github = L"https://github.com/";
    const std::wstring loop = L"http://127.0.0.1:";
    if (startsWithI(url, github)) {
        rest = url.substr(github.size() - 1);
    } else if (local && startsWithI(url, loop)) {
        size_t slash = url.find(L'/', loop.size());
        if (slash == std::wstring::npos) return false;
        rest = url.substr(slash);
    } else {
        return false;
    }
    std::wstring prefix = L"/" + repo + L"/releases/download/";
    return startsWithI(rest, prefix) && rest.substr(prefix.size()) == tag + L"/" + asset;
}

bool isHashOrEmpty(const std::wstring& s) {
    if (s.empty()) return true;
    if (s.size() != 64) return false;
    for (wchar_t c : s)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

int64_t parseUtc(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec;
    int64_t at = _mkgmtime64(&t);
    return at > 0 ? at : 0;
}

// What the saved releases.json (mods-info's, mods_zip.rs) says about this mod, or false to ask GitHub. Nothing in the file
// is believed that the mod's own record doesn't allow: the file can only name a release of the repository the record
// names, so whoever could change it can't point a player at any other file. Only our own repositories are in it; a
// community mod has no package.github and never comes here.
bool sourceFromReleases(const Spec& spec, const std::wstring& cacheDir, bool local, int64_t now, Source& out) {
    const auto& pkg = spec.package;
    if (pkg.github.empty() || !pkg.url.empty() || !isOurs(pkg.github)) return false;
    std::string text, perr;
    if (!fs::readText(fs::join(cacheDir, L"releases.json"), text)) return false;
    json::Value root = json::parse(text, &perr);
    auto say = [&](const wchar_t* why) {
        logger().warn(L"releases.json: " + spec.mod.id + L": " + why + L"; asking GitHub");
        return false;
    };
    if (!perr.empty() || !root.isObject()) return say(L"not a JSON object");
    const json::Value* format = root.find("format");
    if (!format || !format->isNumber() || format->number() != 1) return say(L"a format this launcher doesn't know");
    const json::Value* checked = root.find("checkedAt");
    int64_t at = checked && checked->isString() ? parseUtc(checked->str()) : 0;
    if (at <= 0 || now - at > kReleasesMaxAge || at - now > kReleasesClockSkew) return say(L"out of date");
    const json::Value* mods = root.find("mods");
    const json::Value* e = mods && mods->isObject() ? mods->find(narrow(spec.mod.id)) : nullptr;
    if (!e) return false;  // a new mod, or one the job left out: nothing wrong
    if (!e->isObject()) return say(L"not an object");
    auto field = [](const json::Value& o, const char* k) {
        const json::Value* v = o.find(k);
        return v && v->isString() ? widen(v->str()) : std::wstring();
    };
    std::wstring tag = field(*e, "tag"), version = field(*e, "version"), asset = field(*e, "asset"), url = field(*e, "url"),
                 sha = field(*e, "sha256");
    if (!iequals(field(*e, "repo"), pkg.github)) return say(L"another repository");
    if (!isReleaseUrl(url, pkg.github, tag, asset, local)) return say(L"not a release file of the mod's repository");
    std::wstring bare = !tag.empty() && (tag[0] == L'v' || tag[0] == L'V') ? tag.substr(1) : tag;
    if (version.empty() || version != bare) return say(L"the version isn't the tag's");
    if (!globMatch(toLower(pkg.asset.empty() ? L"*.zip" : pkg.asset), toLower(asset))) return say(L"not the mod's file");
    if (!isHashOrEmpty(sha)) return say(L"not a SHA-256");

    Source s;
    s.version = version;
    s.url = url;
    s.sha256 = sha;
    const json::Value* extras = e->find("extras");
    size_t count = extras && extras->isArray() ? extras->size() : 0;
    if (count != pkg.extras.size()) return say(L"not the record's loader downloads");
    for (const auto& want : pkg.extras) {
        const json::Value* x = nullptr;
        for (size_t i = 0; i < count; ++i)
            if (iequals(field(extras->at(i), "dir"), want.dir)) x = &extras->at(i);
        if (!x) return say(L"not the record's loader downloads");
        ExtraDownload d;
        d.dir = want.dir;
        d.sha256 = toLower(want.sha256);
        if (!want.url.empty()) {  // the record pins it: its address and hash, whatever the file says
            d.url = want.url;
        } else {
            std::wstring repo = want.github.empty() ? pkg.github : want.github;
            std::wstring xTag = field(*x, "tag"), xAsset = field(*x, "asset"), xSha = field(*x, "sha256");
            if (!isOurs(repo) || !iequals(field(*x, "repo"), repo) || xTag != want.tag ||
                !globMatch(toLower(want.asset), toLower(xAsset)) || !isReleaseUrl(field(*x, "url"), repo, xTag, xAsset, local) ||
                !isHashOrEmpty(xSha))
                return say(L"a loader download the record doesn't name");
            d.url = field(*x, "url");
            if (d.sha256.empty()) d.sha256 = xSha;
        }
        s.extras.push_back(std::move(d));
    }
    out = std::move(s);
    return true;
}

bool sameSource(const Source& a, const Source& b) {
    if (a.version != b.version || a.url != b.url || a.sha256 != b.sha256 || a.extras.size() != b.extras.size()) return false;
    for (size_t i = 0; i < a.extras.size(); ++i)
        if (a.extras[i].dir != b.extras[i].dir || a.extras[i].url != b.extras[i].url || a.extras[i].sha256 != b.extras[i].sha256)
            return false;
    return true;
}

json::Value sourceToJson(const Source& s, const std::string& sig, int64_t at, bool fromFile) {
    json::Value o = json::Value::object();
    o.set("sig", sig);
    o.set("at", at);
    o.set("from", std::string(fromFile ? "file" : "github"));
    o.set("version", narrow(s.version));
    o.set("url", narrow(s.url));
    o.set("sha256", narrow(s.sha256));
    json::Value extras = json::Value::array();
    for (const auto& e : s.extras) {
        json::Value x = json::Value::object();
        x.set("dir", narrow(e.dir));
        x.set("url", narrow(e.url));
        x.set("sha256", narrow(e.sha256));
        extras.push(x);
    }
    o.set("extras", extras);
    return o;
}

bool sourceFromJson(const json::Value& o, const std::string& sig, Source& out, int64_t& at) {
    const json::Value* s = o.find("sig");
    const json::Value* a = o.find("at");
    if (!s || !s->isString() || s->str() != sig || !a || !a->isNumber()) return false;
    auto str = [&](const char* k) {
        const json::Value* v = o.find(k);
        return v && v->isString() ? widen(v->str()) : std::wstring();
    };
    at = static_cast<int64_t>(a->number());
    out = {};
    out.version = str("version");
    out.url = str("url");
    out.sha256 = str("sha256");
    if (const json::Value* ex = o.find("extras"); ex && ex->isArray())
        for (size_t i = 0; i < ex->size(); ++i) {
            ExtraDownload d;
            if (const json::Value* v = ex->at(i).find("dir"); v && v->isString()) d.dir = widen(v->str());
            if (const json::Value* v = ex->at(i).find("url"); v && v->isString()) d.url = widen(v->str());
            if (const json::Value* v = ex->at(i).find("sha256"); v && v->isString()) d.sha256 = widen(v->str());
            out.extras.push_back(std::move(d));
        }
    return out.published();
}

}  // namespace

bool resolveSource(const Spec& spec, const std::wstring& modsInfoBase, Source& out, std::wstring& error,
                   const std::wstring& githubApi, const std::wstring& cacheDir, bool forceFresh) {
    if (spec.package.github.empty() || cacheDir.empty())
        return resolveSourceUncached(spec, modsInfoBase, out, error, githubApi);
    std::wstring file = fs::join(cacheDir, L"sources.json");
    std::string text, perr;
    json::Value all = fs::readText(file, text) ? json::parse(text, &perr) : json::Value();
    if (!all.isObject()) all = json::Value::object();
    std::string key = narrow(spec.mod.id), sig = sourceSignature(spec);
    Source cached;
    int64_t at = 0;
    bool have = false, cachedFromFile = false;
    if (const json::Value* e = all.find(key)) {
        have = sourceFromJson(*e, sig, cached, at);
        const json::Value* from = e->find("from");
        cachedFromFile = from && from->isString() && from->str() == "file";
    }
    int64_t now = unixNow();
    bool fresh = have && now - at >= 0 && now - at < kSourceCacheSeconds;
    auto save = [&](const Source& s, bool fromFile) {
        all.set(key, sourceToJson(s, sig, now, fromFile));
        std::string data = json::serialize(all, true);
        fs::ensureDir(cacheDir, nullptr, nullptr);
        fs::writeFile(file, data.data(), data.size(), nullptr);
    };
    // The saved file costs no GitHub request, so it isn't held to the ten minutes. Two things come before it: F5 asks GitHub
    // itself, and an answer GitHub gave minutes ago is newer than the file, which would otherwise take a release back.
    if (!forceFresh && !(fresh && !cachedFromFile)) {
        Source fromFile;
        if (sourceFromReleases(spec, cacheDir, startsWithI(modsInfoBase, L"http://127.0.0.1"), now, fromFile)) {
            // Saved only when it changes: this runs every time the window gets the focus. Kept as the last answer for when
            // the file can't be used any more.
            if (!have || !cachedFromFile || !sameSource(cached, fromFile)) save(fromFile, true);
            out = std::move(fromFile);
            return true;
        }
    }
    if (fresh && !forceFresh) {
        out = cached;
        return true;
    }
    if (resolveSourceUncached(spec, modsInfoBase, out, error, githubApi, have ? &cached : nullptr)) {
        save(out, false);
        return true;
    }
    if (have) {  // GitHub says no (rate limit, offline): the last answer is still a good one
        logger().warn(L"GitHub didn't answer (" + error + L"); using the last known release of " + spec.mod.id);
        out = cached;
        out.stale = true;
        error.clear();
        return true;
    }
    return false;
}

Outcome run(const Spec& spec, const Vars& base, const RunRequest& req, const Report& report, CancelToken& cancel,
            EngineOptions options) {
    Outcome o;
    ProgressFn engineProgress = [&](const Progress& p) {
        if (report) report(p.phase, p.fraction, p.item);
    };
    if (req.op == Operation::Uninstall) return uninstall(spec, base, req.gameDir, engineProgress, cancel, std::move(options));

    if (std::wstring need = missingRequirement(spec, base, req.gameDir); !need.empty()) {
        o.message = need;
        return o;
    }
    if (!req.source.published()) {
        o.message = spec.t("mod.notPublished", withSpec(base, spec));
        return o;
    }
    Spec versioned = spec;
    versioned.mod.version = req.source.version;
    if (report) report(spec.t("mod.phase.downloading", withSpec(base, spec)), 0, L"");
    std::atomic<bool> stop{false};
    std::wstring payload, error;
    InstallProgress dl = [&](const InstallStatus& s) {
        if (cancel.cancelled()) stop = true;
        if (report && s.total) report(versioned.t("mod.phase.downloading", withSpec(base, versioned)),
                                      static_cast<double>(s.done) / static_cast<double>(s.total), L"");
    };
    if (!preparePackage(widen(narrow(spec.mod.id)), req.source.version, req.source.url, req.source.sha256,
                        req.source.extras, req.cacheDir, req.githubApi, dl, &stop, payload, error)) {
        o.result = cancel.cancelled() ? Outcome::Result::Cancelled : Outcome::Result::Failed;
        o.message = cancel.cancelled() ? spec.t("error.cancelledBody", withSpec(base, spec)) : error;
        return o;
    }
    o = install(versioned, base, req.op, req.gameDir, payload, engineProgress, cancel, std::move(options));
    if (o.result == Outcome::Result::Success) pruneCache(req.cacheDir, spec.mod.id, req.source.version);
    // The package lost a file (an antivirus program removing one is the usual cause): the next try downloads it
    // again instead of trusting the damaged copy.
    if (o.packageDamaged) discardPackage(req.cacheDir, widen(narrow(spec.mod.id)), req.source.version);
    return o;
}

bool launchGame(const Spec& spec, const Vars& base, const std::wstring& gameDir, std::wstring& error) {
    Vars v = resolveDirs(spec, withSpec(base, spec), gameDir);
    if (!spec.launch.uri.empty()) {
        std::wstring uri = v.expand(spec.launch.uri);
        if (sys::openPath(uri)) return true;
        error = L"Couldn't open " + uri;
        return false;
    }
    std::wstring exe = fs::normalize(v.expand(spec.launch.executable));
    std::vector<std::wstring> args;
    std::wstring a = v.expand(spec.launch.arguments);
    if (!a.empty()) args.push_back(a);
    sys::Started p;
    if (!sys::startProcess(exe, args, fs::normalize(v.expand(spec.launch.workingDir)), p, error)) return false;
    CloseHandle(p.process);
    return true;
}

}  // namespace fl::mod
