#include "core/mod/engine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <stdexcept>

#include "core/fs.h"
#include "core/log.h"
#include "core/mod/resources.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::mod {

std::string operationKey(Operation op) {
    switch (op) {
        case Operation::Install: return "install";
        case Operation::Update: return "update";
        case Operation::Repair: return "repair";
        case Operation::Modify: return "modify";
        case Operation::Uninstall: return "uninstall";
    }
    return "install";
}

// ---- planning --------------------------------------------------------------

Vars resolveDirs(const Spec& cfg, const Vars& base, const std::wstring& gameDir) {
    Vars v = base;
    v.set(L"GAME_DIR", fs::normalize(gameDir));
    v.set(L"INSTALL_DIR", fs::normalize(v.expand(cfg.install.targetDir)));
    v.set(L"STATE_DIR", fs::normalize(v.expand(cfg.install.stateDir)));
    return v;
}

static std::vector<std::wstring> allowedRoots(const Spec& cfg, const Vars& v) {
    std::vector<std::wstring> roots{v.get(L"GAME_DIR"), v.get(L"INSTALL_DIR")};
    for (const auto& r : cfg.install.allowedRoots) {
        std::wstring n = fs::normalize(v.expand(r));
        if (!n.empty()) roots.push_back(n);
    }
    return roots;
}

static bool underAny(const std::wstring& path, const std::vector<std::wstring>& roots) {
    return std::any_of(roots.begin(), roots.end(), [&](const std::wstring& r) { return fs::isUnder(path, r); });
}

std::wstring checkLayout(const Spec& cfg, const Vars& v) {
    std::wstring game = v.get(L"GAME_DIR"), inst = v.get(L"INSTALL_DIR"), state = v.get(L"STATE_DIR");
    std::vector<std::wstring> roots{game};
    for (const auto& r : cfg.install.allowedRoots) roots.push_back(fs::normalize(v.expand(r)));
    if (!fs::isAbsolute(inst) || !underAny(inst, roots) || fs::isDangerousRoot(inst))
        return cfg.t("path.targetOutside", v);
    if (!fs::isAbsolute(state) || iequals(state, inst) || iequals(state, game) || !underAny(state, roots) ||
        fs::isDangerousRoot(state))
        return L"install.stateDir must be a dedicated sub-folder inside the install folder (" + state + L").";
    return {};
}

Selection defaultSelection(const Spec& cfg, const std::optional<InstalledState>& previous) {
    Selection s;
    if (previous) {
        for (const auto& id : previous->components)
            if (cfg.component(id)) s.components.insert(id);
    } else {
        for (const auto& c : cfg.components)
            if (c.selected) s.components.insert(c.id);
    }
    for (const auto& c : cfg.components)
        if (c.required) s.components.insert(c.id);
    return s;
}

uint64_t requiredBytes(const Spec& cfg, const Resources& res, const std::set<std::string>& components) {
    uint64_t total = 0;
    for (const auto& c : cfg.components) {
        if ((!c.required && !components.count(c.id)) || c.source.empty()) continue;
        for (const auto& e : res.list(c.source)) total += e.size;
    }
    return total;
}

static bool matchesPreserve(const Spec& cfg, const std::wstring& installDir, const std::wstring& gameDir,
                            const std::wstring& path) {
    if (cfg.install.preserve.empty()) return false;
    std::wstring base = fs::isUnder(path, installDir) ? installDir : gameDir;
    if (!fs::isUnder(path, base) || iequals(path, base)) return false;
    std::wstring rel = path.substr(base.size() + (base.back() == L'\\' ? 0 : 1));
    for (const auto& g : cfg.install.preserve)
        if (globMatch(g, rel)) return true;
    return false;
}

std::wstring checkRecord(const Spec& cfg, const InstalledState& st, const std::wstring& stateDir,
                         const std::wstring& gameDir, const Vars& base) {
    std::wstring here = fs::normalize(stateDir);
    Vars v = base;
    v.set(L"GAME_DIR", st.gameDir);
    v.set(L"INSTALL_DIR", st.installDir);
    auto refuse = [&](const std::wstring& why) {
        logger().error(L"Refusing install record in " + here + L": " + why);
        return cfg.t("error.recordElsewhere", v, {{L"OTHER", st.gameDir}, {L"FILE", here}});
    };
    // A record copied along with a game folder still lists the original's absolute
    // paths; acting on it would change the original folder.
    if (!iequals(fs::normalize(st.stateDir), here)) return refuse(L"written for " + st.stateDir);
    if (!gameDir.empty() && !iequals(fs::normalize(st.gameDir), fs::normalize(gameDir)))
        return refuse(L"game folder is " + st.gameDir);
    std::vector<std::wstring> roots{fs::normalize(st.gameDir), fs::normalize(st.installDir), here};
    for (const auto& r : cfg.install.allowedRoots) roots.push_back(fs::normalize(v.expand(r)));
    if (!fs::isUnder(here, st.installDir) && !fs::isUnder(here, st.gameDir)) return refuse(L"state folder outside the install");
    auto inside = [&](const std::wstring& p) {
        return p.empty() || std::any_of(roots.begin(), roots.end(), [&](const std::wstring& r) { return fs::isUnder(p, r); });
    };
    for (const auto& f : st.files)
        if (!inside(f.path) || !inside(f.backup)) return refuse(L"lists " + f.path);
    for (const auto& d : st.createdDirs)
        if (!inside(d)) return refuse(L"lists " + d);
    for (const auto& k : st.keepFiles)
        if (!inside(k)) return refuse(L"lists " + k);
    return {};
}

PlanResult buildPlan(const Spec& cfg, const Resources& res, Operation op, const Selection& sel, const Vars& base) {
    PlanResult r;
    Plan& p = r.plan;
    p.op = op;
    p.vars = resolveDirs(cfg, base, sel.gameDir);
    p.gameDir = p.vars.get(L"GAME_DIR");
    p.installDir = p.vars.get(L"INSTALL_DIR");
    p.stateDir = p.vars.get(L"STATE_DIR");
    if (std::wstring e = checkLayout(cfg, p.vars); !e.empty()) {
        r.error = e;
        return r;
    }

    std::wstring stateErr;
    p.previous = loadState(p.stateDir, &stateErr);
    if (p.previous) {
        p.vars.set(L"INSTALLED_VERSION", p.previous->version);
        if (!iequals(p.previous->modId, cfg.mod.id)) {
            r.error = cfg.t("error.foreign", p.vars, {{L"OTHER", p.previous->modName}});
            return r;
        }
        if (std::wstring e = checkRecord(cfg, *p.previous, p.stateDir, p.gameDir, base); !e.empty()) {
            r.error = e;
            return r;
        }
    } else if (fs::isFile(fs::join(p.stateDir, kManifestName))) {
        r.error = stateErr;  // exists but unreadable: never guess what we own
        return r;
    }

    std::set<std::string> chosen = sel.components;
    for (const auto& id : sel.components) {
        if (!cfg.component(id)) {
            r.error = L"Unknown component: " + widen(id);
            return r;
        }
    }
    for (const auto& c : cfg.components)
        if (c.required) chosen.insert(c.id);

    std::map<std::wstring, const FileRecord*> owned;
    if (p.previous)
        for (const auto& rec : p.previous->files) owned[toLower(rec.path)] = &rec;

    std::vector<std::wstring> roots = allowedRoots(cfg, p.vars);
    std::map<std::wstring, size_t> byTarget;
    for (const auto& comp : cfg.components) {
        if (!chosen.count(comp.id)) continue;
        // A loader the player already has stays theirs: not copied over, not recorded, so not removed later.
        bool ours = p.previous && std::find(p.previous->components.begin(), p.previous->components.end(), comp.id) !=
                                      p.previous->components.end();
        if (!ours && std::any_of(comp.skipIfPresent.begin(), comp.skipIfPresent.end(), [&](const std::wstring& rel) {
                return fs::exists(fs::normalize(fs::join(p.gameDir, rel)));
            }))
            continue;
        p.components.push_back(comp.id);
        if (comp.source.empty()) continue;
        std::wstring src = normRel(comp.source);
        if (src.empty() || !res.exists(src)) {
            r.error = cfg.t("error.payloadMissing", p.vars, {{L"FILE", comp.source}});
            r.detail = L"Component '" + widen(comp.id) + L"' source not found in " + res.describe();
            r.packageDamaged = true;
            return r;
        }
        bool single = res.stat(src).has_value();
        std::wstring targetBase = fs::normalize(p.vars.expand(comp.target));
        if (!fs::isAbsolute(targetBase) || !underAny(targetBase, roots)) {
            r.error = L"Component '" + widen(comp.id) + L"' would install outside the allowed folders: " + targetBase;
            return r;
        }
        for (const auto& e : res.list(src)) {
            std::wstring rel = normRel(e.rel);
            std::wstring target = fs::normalize(
                fs::join(targetBase, single && !comp.targetName.empty() ? comp.targetName : replaceAll(rel, L"/", L"\\")));
            if (rel.empty() || !fs::isUnder(target, targetBase) || iequals(target, targetBase)) {
                r.error = L"Invalid payload path: " + e.rel;
                return r;
            }
            if (fs::isUnder(target, p.stateDir)) {
                r.error = L"Payload file " + e.rel + L" would overwrite the installer's own state folder.";
                return r;
            }
            FilePlan fp;
            fp.sourceRel = single ? src : src + L"/" + rel;
            fp.target = target;
            fp.size = e.size;
            fp.sha256 = e.sha256;
            fp.component = comp.id;
            fp.preserve = matchesPreserve(cfg, p.installDir, p.gameDir, target);
            auto [it, inserted] = byTarget.emplace(toLower(target), p.files.size());
            if (inserted) {
                p.files.push_back(fp);
            } else {
                p.warnings.push_back(L"Component '" + widen(comp.id) + L"' replaces " + target + L" from '" +
                                     widen(p.files[it->second].component) + L"'");
                p.files[it->second] = fp;
            }
        }
    }

    for (const auto& f : p.files) {
        p.bytesTotal += f.size;
        auto it = owned.find(toLower(f.target));
        if (it == owned.end() || it->second->size != f.size) p.bytesRequired += f.size;
    }

    if (p.previous) {
        for (const auto& rec : p.previous->files) {
            if (byTarget.count(toLower(rec.path))) continue;
            if (rec.preserved || matchesPreserve(cfg, p.installDir, p.gameDir, rec.path)) continue;
            p.removals.push_back(rec);
        }
    }

    uint64_t need = p.bytesRequired + static_cast<uint64_t>(cfg.install.extraSpaceMB) * 1024 * 1024;
    if (auto free = fs::freeSpace(p.installDir); free && *free < need) {
        r.error = cfg.t("path.noSpace", p.vars, {{L"REQUIRED", formatBytes(need)}, {L"AVAILABLE", formatBytes(*free)}});
        return r;
    }
    r.ok = true;
    return r;
}

PlanResult buildUninstallPlan(const Spec& cfg, const std::wstring& stateDir, const Vars& base) {
    PlanResult r;
    Plan& p = r.plan;
    p.op = Operation::Uninstall;
    p.vars = base;
    std::wstring err;
    auto st = loadState(fs::normalize(stateDir), &err);
    if (!st) {
        r.error = cfg.t("uninstall.notInstalled", base);
        r.detail = err;
        return r;
    }
    if (!iequals(st->modId, cfg.mod.id)) {
        r.error = cfg.t("error.foreign", base, {{L"OTHER", st->modName}});
        return r;
    }
    if (std::wstring e = checkRecord(cfg, *st, stateDir, L"", base); !e.empty()) {
        r.error = e;
        return r;
    }
    p.vars.set(L"GAME_DIR", st->gameDir);
    p.vars.set(L"INSTALL_DIR", st->installDir);
    p.vars.set(L"STATE_DIR", fs::normalize(stateDir));
    p.vars.set(L"INSTALLED_VERSION", st->version);
    p.gameDir = st->gameDir;
    p.installDir = st->installDir;
    p.stateDir = fs::normalize(stateDir);
    p.removals = st->files;
    p.components = st->components;
    p.previous = std::move(st);
    r.ok = true;
    return r;
}

// ---- execution -------------------------------------------------------------

namespace {

struct Cancelled {};
struct Failure {
    std::wstring message, detail;
    bool damagedPackage = false;
};

constexpr uint64_t kFileUnits = 64 * 1024;  // progress weight of one file operation

bool transient(DWORD e) {
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED ||
           e == ERROR_USER_MAPPED_FILE;
}

std::wstring nowIso() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

std::wstring backupPathFor(const std::wstring& stateDir, const std::wstring& target) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the lower-case path
    for (wchar_t c : toLower(target)) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    wchar_t hex[20];
    swprintf(hex, 20, L"%016llx", static_cast<unsigned long long>(h));
    return fs::join(fs::join(stateDir, L"backup"), std::wstring(hex) + L"_" + fs::fileName(target));
}

struct CleanupMatches {
    std::vector<std::wstring> files;
    std::vector<std::wstring> roots;  // literal folder prefix of each pattern
};

// Files under GAME_DIR matching install.uninstallCleanup. Only the literal
// folder prefix of each pattern is scanned, never the whole game folder.
CleanupMatches findCleanup(const Spec& cfg, const std::wstring& gameDir, const std::wstring& stateDir) {
    CleanupMatches out;
    for (const auto& pattern : cfg.install.uninstallCleanup) {
        std::wstring g = replaceAll(pattern, L"\\", L"/");
        size_t wild = g.find_first_of(L"*?");
        std::wstring prefix = wild == std::wstring::npos ? g : g.substr(0, g.rfind(L'/', wild) + 1);
        std::wstring root = fs::normalize(fs::join(gameDir, replaceAll(normRel(prefix), L"/", L"\\")));
        if (!fs::isUnder(root, gameDir) || iequals(root, gameDir)) continue;
        auto consider = [&](const std::wstring& file) {
            std::wstring rel = replaceAll(file.substr(gameDir.size() + 1), L"\\", L"/");
            if (!globMatch(g, rel) || fs::isUnder(file, stateDir)) return;
            for (const auto& f : out.files)
                if (iequals(f, file)) return;
            out.files.push_back(file);
        };
        if (wild == std::wstring::npos) {
            // A single file: its folder may be left empty once it is gone.
            std::wstring dir = fs::parent(root);
            if (fs::isUnder(dir, gameDir) && !iequals(dir, gameDir) && fs::isDir(dir)) out.roots.push_back(dir);
            if (fs::isFile(root)) consider(root);
            continue;
        }
        if (!fs::isDir(root)) continue;
        out.roots.push_back(root);
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(fs::longPath(root), ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) consider(fs::normalize(it->path().wstring()));
    }
    return out;
}

// Folders below and including each root; recorded at install time (with a
// trailing '\') so uninstall never removes folders that were already there.
std::vector<std::wstring> foldersUnder(const std::vector<std::wstring>& roots) {
    std::vector<std::wstring> dirs;
    for (const auto& root : roots) {
        dirs.push_back(root);
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(fs::longPath(root), ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) dirs.push_back(fs::normalize(it->path().wstring()));
    }
    return dirs;
}

// Removes empty folders below and including each root, deepest first,
// except folders listed in `keep` (as "path\").
void removeEmptyTrees(const std::vector<std::wstring>& roots, const std::vector<std::wstring>& keep) {
    std::vector<std::wstring> dirs = foldersUnder(roots);
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (const auto& d : dirs) {
        bool kept = std::any_of(keep.begin(), keep.end(), [&](const std::wstring& k) { return iequals(k, d + L"\\"); });
        if (!kept) RemoveDirectoryW(fs::longPath(d).c_str());
    }
}

// The Apps & Features entry a stand-alone installer registered for this mod.
void removeUninstallEntry(const std::wstring& keyName) {
    if (keyName.empty()) return;
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + keyName).c_str());
}

bool deleteFileForce(const std::wstring& p) {
    std::wstring lp = fs::longPath(p);
    if (DeleteFileW(lp.c_str())) return true;
    if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return true;
    SetFileAttributesW(lp.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(lp.c_str()) != 0;
}

}  // namespace

class Engine::Meter {
public:
    explicit Meter(const ProgressFn& fn) : fn_(fn) {}
    void setTotals(uint64_t units, int files, uint64_t bytes) {
        units_ = units ? units : 1;
        p_.filesTotal = files;
        p_.bytesTotal = bytes;
    }
    void phase(const std::wstring& name) {
        p_.phase = name;
        p_.item.clear();
        emit(true);
    }
    void item(const std::wstring& name) {
        p_.item = name;
        emit(false);
    }
    void advance(uint64_t units, uint64_t bytes = 0) {
        done_ += units;
        p_.bytesDone += bytes;
        emit(false);
    }
    void fileDone() {
        ++p_.filesDone;
        emit(false);
    }
    void rollingBack() { p_.rollingBack = true; }
    void finish() {
        done_ = units_;
        emit(true);
    }
    void emit(bool force) {
        if (!fn_) return;
        ULONGLONG now = GetTickCount64();
        if (!force && now - last_ < 25) return;
        last_ = now;
        p_.fraction = std::min(1.0, static_cast<double>(done_) / static_cast<double>(units_));
        fn_(p_);
    }

private:
    const ProgressFn& fn_;
    Progress p_;
    uint64_t units_ = 1, done_ = 0;
    ULONGLONG last_ = 0;
};

// Journal lines: "D\t<dir>" created folder, "C\t<file>" created file,
// "M\t<from>\t<to>" moved file. Undone in reverse order.
class Engine::Transaction {
public:
    explicit Transaction(int retries) : retries_(retries) {}
    ~Transaction() { closeJournal(); }

    void begin(const std::wstring& stateDir) {
        txnDir_ = fs::join(stateDir, L".txn");
        std::vector<std::wstring> created;
        DWORD e = 0;
        bool ok = fs::ensureDir(txnDir_, &created, &e);
        for (const auto& d : created) ops_.push_back({'D', d, L""});
        if (!ok) throw e;
        if (_wfopen_s(&journal_, fs::longPath(journalPath()).c_str(), L"wb") != 0 || !journal_) {
            journal_ = nullptr;
            throw static_cast<DWORD>(ERROR_ACCESS_DENIED);
        }
        for (const auto& op : ops_) write(op);
    }

    void load(const std::wstring& stateDir) {
        txnDir_ = fs::join(stateDir, L".txn");
        std::string text;
        if (!fs::readText(journalPath(), text)) return;
        for (const auto& line : split(widen(text), L'\n')) {
            auto parts = split(trim(line), L'\t');
            if (parts.size() < 2 || parts[0].size() != 1) continue;
            ops_.push_back({static_cast<char>(parts[0][0]), parts[1], parts.size() > 2 ? parts[2] : L""});
        }
    }

    void created(const std::wstring& file) { add('C', file); }

    void ensureDir(const std::wstring& dir) {
        if (fs::isDir(dir)) return;
        std::vector<std::wstring> created;
        DWORD e = 0;
        bool ok = fs::ensureDir(dir, &created, &e);
        for (const auto& d : created) add('D', d);
        if (!ok) throw e;
    }

    void move(const std::wstring& from, const std::wstring& to) {
        add('M', from, to);
        DWORD e = moveWithRetry(from, to);
        if (e) throw e;
    }

    std::wstring stash(const std::wstring& path) {
        std::wstring to = fs::join(txnDir_, std::to_wstring(++counter_) + L".stash");
        move(path, to);
        return to;
    }

    std::vector<std::wstring> createdDirs() const {
        std::vector<std::wstring> out;
        for (const auto& op : ops_)
            if (op.kind == 'D' && !iequals(op.a, txnDir_)) out.push_back(op.a);
        return out;
    }

    // `undone` receives each game file put back the way it was (new file
    // withdrawn or original returned), once per path, state folder excluded.
    bool rollback(std::vector<std::wstring>& failures, const std::function<void(const std::wstring&)>& undone = {}) {
        std::vector<std::wstring> dirs, reported;
        std::wstring stateDir = fs::parent(txnDir_);
        auto report = [&](const std::wstring& p) {
            if (!undone || fs::isUnder(p, stateDir)) return;
            for (const auto& r : reported)
                if (iequals(r, p)) return;
            reported.push_back(p);
            undone(p);
        };
        for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {
            switch (it->kind) {
                case 'C':
                    if (fs::exists(it->a) && !deleteFileForce(it->a)) failures.push_back(L"delete " + it->a);
                    break;
                case 'M':
                    if (fs::exists(it->b) && !fs::exists(it->a)) {
                        fs::ensureDir(fs::parent(it->a), nullptr, nullptr);
                        if (DWORD e = moveWithRetry(it->b, it->a))
                            failures.push_back(L"restore " + it->a + L" from " + it->b + L": " + win32ErrorMessage(e));
                        else if (it->a.size() > 7 && iequals(it->a.substr(it->a.size() - 7), L".fl-new"))
                            report(it->b);  // a new file taken back out
                        else
                            report(it->a);  // an original put back
                    }
                    break;
                case 'D': dirs.push_back(it->a); break;
            }
        }
        closeJournal();
        if (failures.empty()) {
            deleteFileForce(journalPath());
            RemoveDirectoryW(fs::longPath(txnDir_).c_str());
            for (const auto& d : dirs) RemoveDirectoryW(fs::longPath(d).c_str());  // only succeeds when empty
        }
        return failures.empty();
    }

    void commit() {
        closeJournal();
        for (const auto& op : ops_)
            if (op.kind == 'M' && fs::isUnder(op.b, txnDir_) && !deleteFileForce(op.b))
                logger().warn(L"Could not delete stashed file " + op.b);
        deleteFileForce(journalPath());
        RemoveDirectoryW(fs::longPath(txnDir_).c_str());
    }

    bool hasJournal() const { return fs::isFile(journalPath()); }

private:
    struct Op {
        char kind;
        std::wstring a, b;
    };

    std::wstring journalPath() const { return fs::join(txnDir_, L"journal.log"); }

    void add(char kind, const std::wstring& a, const std::wstring& b = L"") {
        ops_.push_back({kind, a, b});
        write(ops_.back());
    }

    void write(const Op& op) {
        if (!journal_) return;
        std::string line(1, op.kind);
        line += "\t" + narrow(op.a);
        if (!op.b.empty()) line += "\t" + narrow(op.b);
        line += "\n";
        fwrite(line.data(), 1, line.size(), journal_);
        fflush(journal_);
    }

    DWORD moveWithRetry(const std::wstring& from, const std::wstring& to) const {
        for (int attempt = 0;; ++attempt) {
            if (MoveFileExW(fs::longPath(from).c_str(), fs::longPath(to).c_str(),
                            MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
                return 0;
            DWORD e = GetLastError();
            if (!transient(e) || attempt >= retries_) return e;
            logger().debug(L"Retrying move of " + from + L" (" + win32ErrorMessage(e) + L")");
            Sleep(300);
        }
    }

    void closeJournal() {
        if (journal_) fclose(journal_);
        journal_ = nullptr;
    }

    int retries_;
    std::wstring txnDir_;
    FILE* journal_ = nullptr;
    std::vector<Op> ops_;
    int counter_ = 0;
};

Engine::Engine(const Spec& cfg, const Resources* res, EngineOptions options)
    : cfg_(cfg), res_(res), opt_(std::move(options)), buffer_(1 << 20) {}

std::wstring Engine::t(const char* key, std::initializer_list<std::pair<const wchar_t*, std::wstring>> extra) const {
    static const Vars empty;
    return cfg_.t(key, vars_ ? *vars_ : empty, extra);
}

static Failure fileFailure(const Spec& cfg, const Vars& vars, DWORD code, const std::wstring& path) {
    const char* key = "error.writeFailed";
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION || code == ERROR_USER_MAPPED_FILE)
        key = "error.fileInUse";
    else if (code == ERROR_ACCESS_DENIED || code == ERROR_WRITE_PROTECT)
        key = "error.accessDenied";
    else if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL)
        key = "error.diskFull";
    return {cfg.t(key, vars, {{L"FILE", path}}), path + L": " + win32ErrorMessage(code)};
}

bool Engine::rollback(Transaction& tx, std::vector<std::wstring>& failures) const {
    return tx.rollback(failures, [this](const std::wstring& p) { changed(Change::Kind::Undone, p); });
}

bool Engine::recover(const std::wstring& stateDir, std::wstring* report) {
    Transaction tx(3);
    tx.load(stateDir);
    if (!tx.hasJournal()) return false;
    logger().warn(L"Found an interrupted operation in " + stateDir + L"; rolling it back.");
    std::vector<std::wstring> failures;
    bool clean = tx.rollback(failures);
    for (const auto& f : failures) logger().error(L"Recovery could not " + f);
    if (report) *report = clean ? L"An interrupted installation was rolled back." : L"Recovery was incomplete.";
    return true;
}

Outcome Engine::run(const Plan& plan, const ProgressFn& progress, CancelToken& cancel) {
    vars_ = &plan.vars;
    Meter m(progress);
    logger().info(L"=== " + widen(operationKey(plan.op)) + L" " + cfg_.mod.name + L" " + cfg_.mod.version + L" ===");
    logger().info(L"Game folder: " + plan.gameDir + L" | install folder: " + plan.installDir + L" | state: " +
                  plan.stateDir);
    for (const auto& w : plan.warnings) logger().warn(w);

    std::wstring report;
    if (recover(plan.stateDir, &report)) logger().warn(report);

    Outcome o = plan.op == Operation::Uninstall ? uninstall(plan, m, cancel) : apply(plan, m, cancel);
    switch (o.result) {
        case Outcome::Result::Success:
            logger().info(L"Completed: " + std::to_wstring(o.filesWritten) + L" written, " +
                          std::to_wstring(o.filesSkipped) + L" unchanged, " + std::to_wstring(o.filesRemoved) +
                          L" removed.");
            break;
        case Outcome::Result::Cancelled: logger().warn(L"Cancelled by the user."); break;
        case Outcome::Result::Failed: logger().error(L"Failed: " + o.message + L" | " + o.detail); break;
    }
    vars_ = nullptr;
    return o;
}

std::string Engine::payloadHash(const FilePlan& f) {
    if (!f.sha256.empty()) return f.sha256;
    auto r = res_->open(f.sourceRel);
    if (!r) return {};
    fs::Sha256 sha;
    for (;;) {
        size_t n = r->read(buffer_.data(), buffer_.size());
        if (n == 0) break;
        if (n == SIZE_MAX) return {};
        sha.update(buffer_.data(), n);
    }
    return sha.hex();
}

std::string Engine::copyPayload(const FilePlan& f, const std::wstring& dest, Meter& m, CancelToken& cancel) {
    auto reader = res_->open(f.sourceRel);
    if (!reader)
        throw Failure{t("error.payloadMissing", {{L"FILE", f.sourceRel}}),
                      L"Cannot open " + f.sourceRel + L" in " + res_->describe(), true};
    HANDLE out = CreateFileW(fs::longPath(dest).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (out == INVALID_HANDLE_VALUE) throw fileFailure(cfg_, *vars_, GetLastError(), f.target);

    fs::Sha256 sha;
    uint64_t total = 0;
    for (;;) {
        if (cancel.cancelled()) {
            CloseHandle(out);
            throw Cancelled{};
        }
        size_t n = reader->read(buffer_.data(), buffer_.size());
        if (n == 0) break;
        if (n == SIZE_MAX) {
            CloseHandle(out);
            throw Failure{t("error.payloadCorrupt", {{L"FILE", f.sourceRel}}), L"Read error in " + f.sourceRel, true};
        }
        DWORD written = 0;
        if (!WriteFile(out, buffer_.data(), static_cast<DWORD>(n), &written, nullptr) || written != n) {
            DWORD e = GetLastError();
            CloseHandle(out);
            throw fileFailure(cfg_, *vars_, e, f.target);
        }
        sha.update(buffer_.data(), n);
        total += n;
        m.advance(n, n);
    }
    CloseHandle(out);
    std::string hash = sha.hex();
    if (total != f.size || (!f.sha256.empty() && hash != f.sha256))
        throw Failure{t("error.payloadCorrupt", {{L"FILE", f.sourceRel}}),
                      L"Checksum or size mismatch for " + f.sourceRel + L" (expected " + std::to_wstring(f.size) +
                          L" bytes, got " + std::to_wstring(total) + L")",
                      true};
    return hash;
}

Outcome Engine::apply(const Plan& plan, Meter& m, CancelToken& cancel) {
    Outcome o;
    Transaction tx(cfg_.install.lockedFileRetries);

    InstalledState next;
    next.modId = cfg_.mod.id;
    next.modName = cfg_.mod.name;
    next.version = cfg_.mod.version;
    next.gameDir = plan.gameDir;
    next.installDir = plan.installDir;
    next.stateDir = plan.stateDir;
    next.installedAt = nowIso();
    // A mod first set up by its stand-alone installer keeps that installer's
    // shortcuts and Apps & Features entry in the record.
    if (plan.previous) {
        next.uninstallKey = plan.previous->uninstallKey;
        next.registered = plan.previous->registered;
        next.shortcuts = plan.previous->shortcuts;
        next.shortcutIds = plan.previous->shortcutIds;
    }
    next.components = plan.components;
    // Remember runtime files that predate the first install so uninstall keeps them.
    if (plan.previous) {
        next.keepFiles = plan.previous->keepFiles;
    } else {
        CleanupMatches existing = findCleanup(cfg_, plan.gameDir, plan.stateDir);
        next.keepFiles = existing.files;
        for (const auto& d : foldersUnder(existing.roots)) next.keepFiles.push_back(d + L"\\");
    }
    std::vector<std::wstring> dirs = plan.previous ? plan.previous->createdDirs : std::vector<std::wstring>{};

    std::map<std::wstring, const FileRecord*> owned;
    if (plan.previous)
        for (const auto& r : plan.previous->files) owned[toLower(r.path)] = &r;

    uint64_t units = 3 * kFileUnits;
    for (const auto& f : plan.files) units += f.size + kFileUnits;
    units += plan.removals.size() * kFileUnits;
    m.setTotals(units, static_cast<int>(plan.files.size()), plan.bytesTotal);

    std::wstring current = plan.stateDir;
    try {
        m.phase(t("phase.preparing"));
        tx.begin(plan.stateDir);
        m.advance(kFileUnits);

        m.phase(t(plan.op == Operation::Repair ? "phase.checking" : "phase.copying"));
        for (size_t i = 0; i < plan.files.size(); ++i) {
            const FilePlan& f = plan.files[i];
            current = f.target;
            if (cancel.cancelled()) throw Cancelled{};
            if (opt_.beforeFile) opt_.beforeFile(i, f.target);
            m.item(f.target);

            FileRecord rec{f.target, f.size, f.sha256, f.component, L"", false};
            auto prevIt = owned.find(toLower(f.target));
            const FileRecord* prev = prevIt == owned.end() ? nullptr : prevIt->second;
            if (prev) rec.backup = prev->backup;
            if (fs::isDir(f.target)) throw fileFailure(cfg_, plan.vars, ERROR_DIRECTORY, f.target);
            bool exists = fs::isFile(f.target);

            if (exists && f.preserve) {
                // Keep the hash of what we originally installed: uninstall
                // removes the file only if the player never changed it.
                rec.preserved = true;
                rec.sha256 = prev ? prev->sha256 : std::string();
                rec.size = prev ? prev->size : 0;
                logger().info(L"Kept user file " + f.target);
                changed(Change::Kind::Kept, f.target);
                m.advance(f.size + kFileUnits, f.size);
                m.fileDone();
                ++o.filesSkipped;
                next.files.push_back(rec);
                continue;
            }
            if (exists && prev && fs::fileSize(f.target).value_or(UINT64_MAX) == f.size) {
                std::string want = payloadHash(f);
                if (!want.empty() && fs::sha256File(f.target) == want) {
                    rec.sha256 = want;
                    logger().debug(L"Unchanged " + f.target);
                    changed(Change::Kind::Unchanged, f.target);
                    m.advance(f.size + kFileUnits, f.size);
                    m.fileDone();
                    ++o.filesSkipped;
                    next.files.push_back(rec);
                    continue;
                }
            }

            tx.ensureDir(fs::parent(f.target));
            std::wstring tmp = f.target + L".fl-new";
            tx.created(tmp);
            rec.sha256 = copyPayload(f, tmp, m, cancel);
            if (exists) {
                if (prev || !cfg_.install.backupOverwritten) {
                    tx.stash(f.target);
                } else {
                    std::wstring b = backupPathFor(plan.stateDir, f.target);
                    tx.ensureDir(fs::parent(b));
                    if (fs::exists(b)) tx.stash(b);
                    tx.move(f.target, b);
                    rec.backup = b;
                    logger().info(L"Backed up original " + f.target);
                }
            }
            tx.move(tmp, f.target);
            logger().debug(L"Wrote " + f.target);
            changed(!exists ? Change::Kind::Installed
                    : rec.backup.empty() || prev ? Change::Kind::Updated
                                                 : Change::Kind::Replaced,
                    f.target);
            m.advance(kFileUnits);
            m.fileDone();
            ++o.filesWritten;
            next.files.push_back(rec);
        }

        if (!plan.removals.empty()) m.phase(t("phase.removing"));
        for (const auto& r : plan.removals) {
            current = r.path;
            if (cancel.cancelled()) throw Cancelled{};
            m.item(r.path);
            if (fs::isFile(r.path)) tx.stash(r.path);
            bool restored = !r.backup.empty() && fs::isFile(r.backup);
            if (restored) tx.move(r.backup, r.path);
            logger().info(L"Removed " + r.path);
            changed(restored ? Change::Kind::Restored : Change::Kind::Removed, r.path);
            ++o.filesRemoved;
            m.advance(kFileUnits);
        }

        m.phase(t("phase.finishing"));
        for (const auto& d : tx.createdDirs())
            if (std::none_of(dirs.begin(), dirs.end(), [&](const std::wstring& x) { return iequals(x, d); }))
                dirs.push_back(d);
        next.createdDirs = dirs;

        std::wstring manifest = fs::join(plan.stateDir, kManifestName);
        std::wstring tmp = manifest + L".fl-new";
        std::string text = serializeState(next);
        tx.created(tmp);
        DWORD e = 0;
        if (!fs::writeFile(tmp, text.data(), text.size(), &e)) throw fileFailure(cfg_, plan.vars, e, manifest);
        if (fs::exists(manifest)) tx.stash(manifest);
        tx.move(tmp, manifest);
        if (cancel.cancelled()) throw Cancelled{};
        tx.commit();
    } catch (const Cancelled&) {
        m.rollingBack();
        m.phase(t("phase.rollback"));
        std::vector<std::wstring> failures;
        o.result = Outcome::Result::Cancelled;
        o.rollbackComplete = rollback(tx, failures);
        for (const auto& f : failures) logger().error(L"Rollback could not " + f);
        o.message = t("error.cancelledBody");
        return o;
    } catch (...) {
        Failure f;
        try {
            throw;
        } catch (const Failure& x) {
            f = x;
        } catch (DWORD code) {
            f = fileFailure(cfg_, plan.vars, code, current);
        } catch (const std::exception& x) {
            f = {t("error.unexpected"), widen(x.what()) + L" (" + current + L")"};
        }
        if (f.message.empty()) f.message = t("error.writeFailed", {{L"FILE", plan.stateDir}});
        logger().error(L"Error: " + f.message + L" | " + f.detail);
        m.rollingBack();
        m.phase(t("phase.rollback"));
        std::vector<std::wstring> failures;
        o.result = Outcome::Result::Failed;
        o.rollbackComplete = rollback(tx, failures);
        for (const auto& x : failures) logger().error(L"Rollback could not " + x);
        o.message = f.message;
        o.detail = f.detail;
        o.packageDamaged = f.damagedPackage;
        return o;
    }

    m.finish();
    Logger::get().copyTo(fs::join(plan.stateDir, L"install.log"));
    o.result = Outcome::Result::Success;
    return o;
}

Outcome Engine::uninstall(const Plan& plan, Meter& m, CancelToken& cancel) {
    Outcome o;
    const InstalledState& st = *plan.previous;
    Transaction tx(cfg_.install.lockedFileRetries);
    m.setTotals((st.files.size() + st.shortcuts.size() + 3) * kFileUnits, static_cast<int>(st.files.size()), 0);
    CleanupMatches cleanup;
    std::wstring current = plan.stateDir;
    try {
        m.phase(t("phase.preparing"));
        tx.begin(plan.stateDir);
        m.advance(kFileUnits);
        m.phase(t("phase.removing"));
        for (size_t i = 0; i < st.files.size(); ++i) {
            const FileRecord& r = st.files[i];
            current = r.path;
            if (cancel.cancelled()) throw Cancelled{};
            if (opt_.beforeFile) opt_.beforeFile(i, r.path);
            m.item(r.path);
            bool keep = (r.preserved || matchesPreserve(cfg_, st.installDir, st.gameDir, r.path)) &&
                        fs::isFile(r.path) && fs::sha256File(r.path) != r.sha256;
            if (keep) {
                logger().info(L"Kept user file " + r.path);
                changed(Change::Kind::Kept, r.path);
            } else {
                if (fs::isFile(r.path)) tx.stash(r.path);
                bool restored = !r.backup.empty() && fs::isFile(r.backup);
                if (restored) {
                    tx.move(r.backup, r.path);
                    logger().info(L"Restored original " + r.path);
                }
                changed(restored ? Change::Kind::Restored : Change::Kind::Removed, r.path);
                ++o.filesRemoved;
            }
            m.advance(kFileUnits);
            m.fileDone();
        }
        const std::string& with = cfg_.install.uninstallCleanupWith;
        if (with.empty() || std::find(st.components.begin(), st.components.end(), with) != st.components.end())
            cleanup = findCleanup(cfg_, st.gameDir, plan.stateDir);
        for (const auto& f : cleanup.files) {
            current = f;
            if (cancel.cancelled()) throw Cancelled{};
            if (std::any_of(st.keepFiles.begin(), st.keepFiles.end(), [&](const std::wstring& k) { return iequals(k, f); }))
                continue;
            m.item(f);
            tx.stash(f);
            logger().info(L"Removed runtime file " + f);
            changed(Change::Kind::Removed, f);
            ++o.filesRemoved;
        }
        if (!st.shortcuts.empty()) m.phase(t("phase.shortcuts"));
        for (const auto& s : st.shortcuts) {
            if (fs::isFile(s)) {
                tx.stash(s);
                changed(Change::Kind::Removed, s);
            }
            m.advance(kFileUnits);
        }
        if (cancel.cancelled()) throw Cancelled{};
        tx.commit();
    } catch (const Cancelled&) {
        m.rollingBack();
        m.phase(t("phase.rollback"));
        std::vector<std::wstring> failures;
        o.result = Outcome::Result::Cancelled;
        o.rollbackComplete = rollback(tx, failures);
        for (const auto& f : failures) logger().error(L"Rollback could not " + f);
        o.message = t("error.cancelledBody");
        return o;
    } catch (...) {
        Failure f;
        try {
            throw;
        } catch (const Failure& x) {
            f = x;
        } catch (DWORD code) {
            f = fileFailure(cfg_, plan.vars, code, current);
        } catch (const std::exception& x) {
            f = {t("error.unexpected"), widen(x.what())};
        }
        if (f.message.empty()) f.message = t("error.writeFailed", {{L"FILE", plan.stateDir}});
        m.rollingBack();
        m.phase(t("phase.rollback"));
        std::vector<std::wstring> failures;
        o.result = Outcome::Result::Failed;
        o.rollbackComplete = rollback(tx, failures);
        for (const auto& x : failures) logger().error(L"Rollback could not " + x);
        o.message = f.message;
        o.detail = f.detail;
        o.packageDamaged = f.damagedPackage;
        return o;
    }

    m.phase(t("phase.finishing"));
    removeEmptyTrees(cleanup.roots, st.keepFiles);
    cleanupStateDir(st, plan.stateDir);
    if (st.registered) removeUninstallEntry(st.uninstallKey);
    m.finish();
    o.result = Outcome::Result::Success;
    return o;
}

void Engine::cleanupStateDir(const InstalledState& st, const std::wstring& stateDir) {
    for (const wchar_t* name : {kManifestName, L"install.log"}) deleteFileForce(fs::join(stateDir, name));
    std::wstring backup = fs::join(stateDir, L"backup");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(fs::longPath(fs::join(backup, L"*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) deleteFileForce(fs::join(backup, fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(fs::longPath(backup).c_str());

    // Folders we created, deepest first; RemoveDirectory only removes empty ones.
    std::vector<std::wstring> dirs = st.createdDirs;
    if (std::none_of(dirs.begin(), dirs.end(), [&](const std::wstring& d) { return iequals(d, stateDir); }))
        dirs.push_back(stateDir);
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });

    // Left behind by a stand-alone installer: its uninstaller program.
    deleteFileForce(fs::join(stateDir, kUninstallerName));
    for (const auto& d : dirs) RemoveDirectoryW(fs::longPath(d).c_str());
}

}  // namespace fl::mod
