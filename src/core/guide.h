// A game's guide page: what you need, how to play it step by step, keys, tips and what the
// website says about it. Built from the game's `guide` in launcher.json, filled in with a
// default for its kind of game where the entry says nothing, so every game has a page.
// No UI and no network: the window paints the blocks and the tests read them.
#pragma once
#include <string>
#include <vector>

#include "config.h"
#include "site.h"

namespace fl {

struct GuideBlock {
    enum class Kind {
        Heading,    // a section title
        Text,       // a paragraph
        Step,       // a numbered step (`number`)
        Bullet,     // a point
        Key,        // `text` is the key, `detail` what it does
        Fact,       // `text` is the label, `detail` the value
        Release,    // a version: `text` the version, `detail` what it is here, `number` 1 when it is SHA-256 checked
    } kind = Kind::Text;
    int number = 0;
    std::wstring text, detail;
};

struct GuidePage {
    std::vector<GuideBlock> blocks;
    std::vector<GuideLink> links;  // up to kGuideMaxLinks, all https
};

// `site` may be null (the website's list isn't there yet).
GuidePage buildGuide(const Config& cfg, const GameSpec& game, const SiteGame* site);

}  // namespace fl
