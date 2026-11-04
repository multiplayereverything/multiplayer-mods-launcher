#include "community.h"

#include "config.h"
#include <algorithm>

#include "fs.h"
#include "install.h"
#include "json.h"
#include "log.h"
#include "settings.h"
#include "strings.h"

namespace fl {

namespace {

// mods-info is fetched over https; a loopback server stands in for it in the tests.
bool localBase(const std::wstring& base) { return startsWithI(base, L"http://127.0.0.1"); }

}  // namespace

bool loadCommunityCache(const std::wstring& path, CommunityCache& out) {
    std::string data, err;
    if (!fs::readText(path, data)) return false;
    json::Value root = json::parse(data, &err);
    const json::Value* at = root.isObject() ? root.find("at") : nullptr;
    const json::Value* mods = root.isObject() ? root.find("mods") : nullptr;
    if (!at || !at->isNumber() || !mods || !mods->isArray()) return false;
    out.fetchedAt = static_cast<int64_t>(at->number());
    out.raw.clear();
    for (size_t i = 0; i < mods->size(); ++i)
        if (mods->at(i).isString()) out.raw.push_back(mods->at(i).str());
    return true;
}

bool saveCommunityCache(const std::wstring& path, const CommunityCache& cache) {
    json::Value root = json::Value::object();
    root.set("at", cache.fetchedAt);
    json::Value mods = json::Value::array();
    for (const auto& r : cache.raw) mods.push(r);
    root.set("mods", std::move(mods));
    std::string data = json::serialize(root, true);
    fs::ensureDir(fs::parent(path), nullptr, nullptr);
    return fs::writeFile(path, data.data(), data.size(), nullptr);
}

int mergeTeam(Config& cfg, const CommunityCache& cache) {
    bool local = localBase(cfg.modsInfoBase());
    int added = 0;
    for (const auto& raw : cache.raw) {
        GameSpec g;
        std::wstring why;
        if (!readTeamMod(raw, cfg, local, g, why)) {
            if (!why.empty()) logger().warn(L"Team mod left out: " + why);
            continue;
        }
        cfg.games.push_back(std::move(g));
        ++added;
    }
    return added;
}

int mergeCommunity(Config& cfg, const CommunityCache& cache) {
    bool local = localBase(cfg.modsInfoBase());
    int added = 0;
    for (const auto& raw : cache.raw) {
        GameSpec g;
        std::wstring why;
        if (!readCommunityMod(raw, cfg, local, g, why)) {
            if (!why.empty()) logger().warn(L"Community mod left out: " + why);
            continue;
        }
        cfg.games.push_back(std::move(g));
        ++added;
    }
    return added;
}

}  // namespace fl
