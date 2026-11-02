#include "core/mod/state.h"

#include "core/fs.h"
#include "core/json.h"
#include "core/strings.h"

namespace fl::mod {

using json::Value;

static Value strList(const std::vector<std::wstring>& v) {
    Value a = Value::array();
    for (const auto& s : v) a.push(narrow(s));
    return a;
}

static Value strListA(const std::vector<std::string>& v) {
    Value a = Value::array();
    for (const auto& s : v) a.push(s);
    return a;
}

std::string serializeState(const InstalledState& s) {
    Value root = Value::object();
    root.set("schema", s.schema);
    root.set("modId", narrow(s.modId));
    root.set("modName", narrow(s.modName));
    root.set("version", narrow(s.version));
    root.set("gameDir", narrow(s.gameDir));
    root.set("installDir", narrow(s.installDir));
    root.set("stateDir", narrow(s.stateDir));
    root.set("installedAt", narrow(s.installedAt));
    root.set("uninstallKey", narrow(s.uninstallKey));
    root.set("registered", s.registered);
    root.set("components", strListA(s.components));
    root.set("shortcutIds", strListA(s.shortcutIds));
    root.set("shortcuts", strList(s.shortcuts));
    root.set("createdDirs", strList(s.createdDirs));
    root.set("keepFiles", strList(s.keepFiles));
    Value files = Value::array();
    for (const auto& f : s.files) {
        Value o = Value::object();
        o.set("path", narrow(f.path));
        o.set("size", f.size);
        o.set("sha256", f.sha256);
        o.set("component", f.component);
        if (!f.backup.empty()) o.set("backup", narrow(f.backup));
        if (f.preserved) o.set("preserved", true);
        files.push(std::move(o));
    }
    root.set("files", std::move(files));
    return json::serialize(root);
}

std::optional<InstalledState> parseState(const std::string& text, std::wstring* error) {
    std::string err;
    Value root = json::parse(text, &err);
    if (!err.empty() || !root.isObject()) {
        if (error) *error = L"The install record is damaged: " + widen(err);
        return std::nullopt;
    }
    auto str = [&](const Value& o, const char* k) {
        const Value* v = o.find(k);
        return v ? widen(v->str()) : std::wstring();
    };
    auto list = [&](const char* k) {
        std::vector<std::wstring> out;
        if (const Value* v = root.find(k))
            for (size_t i = 0; i < v->size(); ++i) out.push_back(widen(v->at(i).str()));
        return out;
    };
    auto listA = [&](const char* k) {
        std::vector<std::string> out;
        if (const Value* v = root.find(k))
            for (size_t i = 0; i < v->size(); ++i) out.push_back(v->at(i).str());
        return out;
    };
    InstalledState s;
    if (const Value* v = root.find("schema")) s.schema = static_cast<int>(v->number(1));
    s.modId = str(root, "modId");
    s.modName = str(root, "modName");
    s.version = str(root, "version");
    s.gameDir = str(root, "gameDir");
    s.installDir = str(root, "installDir");
    s.stateDir = str(root, "stateDir");
    s.installedAt = str(root, "installedAt");
    s.uninstallKey = str(root, "uninstallKey");
    if (const Value* v = root.find("registered")) s.registered = v->boolean();
    s.components = listA("components");
    s.shortcutIds = listA("shortcutIds");
    s.shortcuts = list("shortcuts");
    s.createdDirs = list("createdDirs");
    s.keepFiles = list("keepFiles");
    if (const Value* files = root.find("files")) {
        for (size_t i = 0; i < files->size(); ++i) {
            const Value& f = files->at(i);
            FileRecord r;
            r.path = str(f, "path");
            if (const Value* v = f.find("size")) r.size = static_cast<uint64_t>(v->number());
            if (const Value* v = f.find("sha256")) r.sha256 = v->str();
            if (const Value* v = f.find("component")) r.component = v->str();
            r.backup = str(f, "backup");
            if (const Value* v = f.find("preserved")) r.preserved = v->boolean();
            if (!r.path.empty()) s.files.push_back(std::move(r));
        }
    }
    if (s.modId.empty()) {
        if (error) *error = L"The install record has no mod id.";
        return std::nullopt;
    }
    return s;
}

std::optional<InstalledState> loadState(const std::wstring& stateDir, std::wstring* error) {
    std::string text;
    if (!fs::readText(fs::join(stateDir, kManifestName), text)) {
        if (error) *error = L"No install record in " + stateDir;
        return std::nullopt;
    }
    return parseState(text, error);
}

}  // namespace fl::mod
