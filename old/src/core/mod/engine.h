// Planning and executing install / update / repair / modify / uninstall.
//
// Every change goes through a journaled transaction (STATE_DIR\.txn): files
// are written to a temporary name and renamed into place, replaced files are
// stashed rather than deleted. A failure or cancel rolls everything back; a
// crash is rolled back the next time the engine runs against that folder.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/mod/spec.h"
#include "core/mod/state.h"
#include "core/vars.h"

namespace fl::mod {

class Resources;

enum class Operation { Install, Update, Repair, Modify, Uninstall };
std::string operationKey(Operation op);  // "install", "update", ...

struct Selection {
    std::wstring gameDir;
    std::set<std::string> components;  // required components are added automatically
};

struct FilePlan {
    std::wstring sourceRel, target;
    uint64_t size = 0;
    std::string sha256, component;
    bool preserve = false;
};

struct Plan {
    Operation op = Operation::Install;
    Vars vars;
    std::wstring gameDir, installDir, stateDir;
    std::vector<std::string> components;
    std::vector<FilePlan> files;
    std::vector<FileRecord> removals;
    std::optional<InstalledState> previous;
    uint64_t bytesTotal = 0, bytesRequired = 0;
    std::vector<std::wstring> warnings;
};

struct PlanResult {
    bool ok = false;
    std::wstring error, detail;
    bool packageDamaged = false;  // a file the package should have is missing
    Plan plan;
};

// Copy of `base` with GAME_DIR, INSTALL_DIR and STATE_DIR resolved.
Vars resolveDirs(const Spec& cfg, const Vars& base, const std::wstring& gameDir);
// Empty when INSTALL_DIR / STATE_DIR are safe for these vars, else a message.
std::wstring checkLayout(const Spec& cfg, const Vars& vars);
// Empty when an install record found in `stateDir` may be acted on, else the
// user message. Refuses records that were copied or moved there (their paths
// point at another folder) and records listing paths outside the install.
std::wstring checkRecord(const Spec& cfg, const InstalledState& st, const std::wstring& stateDir,
                         const std::wstring& gameDir, const Vars& base);
// Components and shortcuts preselected for a fresh install or an existing one.
Selection defaultSelection(const Spec& cfg, const std::optional<InstalledState>& previous);
// Total payload size of the given components (required ones included).
uint64_t requiredBytes(const Spec& cfg, const Resources& res, const std::set<std::string>& components);

PlanResult buildPlan(const Spec& cfg, const Resources& res, Operation op, const Selection& sel, const Vars& base);
PlanResult buildUninstallPlan(const Spec& cfg, const std::wstring& stateDir, const Vars& base);

struct Progress {
    double fraction = 0;
    std::wstring phase, item;
    int filesDone = 0, filesTotal = 0;
    uint64_t bytesDone = 0, bytesTotal = 0;
    bool rollingBack = false;
};
using ProgressFn = std::function<void(const Progress&)>;

struct CancelToken {
    std::atomic<bool> flag{false};
    void cancel() { flag = true; }
    void reset() { flag = false; }
    bool cancelled() const { return flag.load(); }
};

// One file-level change, reported as it happens (for the progress page's list).
struct Change {
    enum class Kind { Installed, Updated, Replaced, Unchanged, Kept, Removed, Restored, Undone } kind;
    std::wstring path;
};

struct EngineOptions {
    // Test hook called before each payload file; throwing simulates a failure.
    std::function<void(size_t index, const std::wstring& target)> beforeFile;
    // Called from the worker thread for every file the operation changes or checks.
    std::function<void(const Change&)> onChange;
};

struct Outcome {
    enum class Result { Success, Cancelled, Failed } result = Result::Failed;
    std::wstring message, detail;
    bool packageDamaged = false;  // the downloaded package lost or garbled a file (often antivirus): download it afresh
    bool rollbackComplete = true;
    std::vector<std::wstring> warnings;
    int filesWritten = 0, filesSkipped = 0, filesRemoved = 0;
};

class Engine {
public:
    Engine(const Spec& cfg, const Resources* res, EngineOptions options = {});
    Outcome run(const Plan& plan, const ProgressFn& progress, CancelToken& cancel);
    // Rolls back a transaction a crashed run left in `stateDir`. True if one was found.
    static bool recover(const std::wstring& stateDir, std::wstring* report);

private:
    class Meter;
    class Transaction;
    Outcome apply(const Plan& plan, Meter& m, CancelToken& cancel);
    Outcome uninstall(const Plan& plan, Meter& m, CancelToken& cancel);
    std::string copyPayload(const FilePlan& f, const std::wstring& dest, Meter& m, CancelToken& cancel);
    std::string payloadHash(const FilePlan& f);
    void cleanupStateDir(const InstalledState& st, const std::wstring& stateDir);
    std::wstring t(const char* key, std::initializer_list<std::pair<const wchar_t*, std::wstring>> extra = {}) const;
    void changed(Change::Kind kind, const std::wstring& path) const {
        if (opt_.onChange) opt_.onChange({kind, path});
    }
    bool rollback(Transaction& tx, std::vector<std::wstring>& failures) const;

    const Spec& cfg_;
    const Resources* res_;
    EngineOptions opt_;
    const Vars* vars_ = nullptr;
    std::vector<uint8_t> buffer_;
};

}  // namespace fl::mod
