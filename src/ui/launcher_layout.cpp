// The launcher window: where every control and panel goes.
#include "launcher.h"

#include <algorithm>
#include <cwctype>
#include <regex>

#include "core/embedded.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/motion.h"
#include "core/strings.h"
#include "launcher_internal.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace fl::ui {

using namespace Gdiplus;
using namespace detail;

// ---- layout ---------------------------------------------------------------

bool Launcher::isNarrow() const {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    return cr.right < theme_.s(kNarrowBelow);
}

void Launcher::relayout() {
    endIntro();
    if (tilePlayBtn_) ShowWindow(tilePlayBtn_, SW_HIDE);  // the cards move; it returns with the mouse
    RECT cr;
    GetClientRect(hwnd_, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    const Theme& t = theme_;
    bool narrowMode = isNarrow();
    int barH = t.s(kTopBarH);
    // The details panel belongs to a game's page; the library and Rooms pages are full width.
    int dw = guidePage() ? std::min<int>(t.s(narrowMode ? kDetailNarrowW : kDetailW), cr.right / 2) : 0;
    // A bar across the top (brand, Library and Rooms, Settings); under it the library on the left and the
    // details panel on the right. The categories moved from the sidebar into the toolbar's dropdown.
    topBar_ = {0, 0, cr.right, barH};
    status_ = {0, cr.bottom - t.s(kStatusH), cr.right, cr.bottom};
    sidebar_ = {0, barH, 0, status_.top};
    detail_ = {cr.right - dw, barH, cr.right, status_.top};

    // The game page takes the whole library area: no banner, no toolbar.
    bool page = guidePage();
    ShowWindow(nav_, SW_HIDE);
    ShowWindow(navCombo_, SW_HIDE);
    ShowWindow(sortCombo_, SW_HIDE);
    ShowWindow(catBtn_, page ? SW_HIDE : SW_SHOW);
    ShowWindow(search_, page ? SW_HIDE : SW_SHOW);
    ShowWindow(sortBtn_, narrowMode || page ? SW_HIDE : SW_SHOW);
    ShowWindow(sizeBtn_, narrowMode || page ? SW_HIDE : SW_SHOW);

    HDC dc = GetDC(hwnd_);
    // Top bar: the brand is painted; the tabs follow it, Settings sits at the far end.
    int tabX = t.s(20) + textWidth(dc, t.brand, cfg_.branding.title) + t.s(32);
    int tabTop = t.s(10), tabH = barH - tabTop;
    bool online = std::any_of(navItems_.begin(), navItems_.end(), [](const NavItem& n) { return n.key == "rooms"; });
    for (HWND tab : {libTab_, roomsTopTab_}) {
        if (tab == roomsTopTab_ && !online) {
            ShowWindow(tab, SW_HIDE);
            continue;
        }
        int tw = textWidth(dc, t.button, windowText(tab)) + t.s(32);
        MoveWindow(tab, tabX, tabTop, tw, tabH, TRUE);
        ShowWindow(tab, SW_SHOW);
        tabX += tw + t.s(4);
    }
    // As wide as its icon and label need (a fixed 96 cut "Settings" down to "Set..." in some languages and sizes).
    int setW = std::max<int>(t.s(96), textWidth(dc, t.button, windowText(settingsBtn_)) + t.s(58));
    MoveWindow(settingsBtn_, cr.right - t.s(16) - setW, (barH - t.s(36)) / 2, setW, t.s(36), TRUE);

    // The banner leads the library, the toolbar follows it, the grid takes the rest.
    int left = 0, right = detail_.left, y0 = barH + t.s(16);
    heroGame_ = heroWanted() ? heroCandidate() : -1;
    hero_ = {};
    if (heroGame_ >= 0) {
        hero_ = {left + t.s(kPad), y0, right - t.s(kPad), y0 + t.s(164)};
        int bh = t.s(38), bx = hero_.left + t.s(24), by = hero_.bottom - t.s(18) - bh;
        heroAction_ = heroLead(heroGame_);
        if (heroAction_ >= 0) {
            // Play leads, in red as everywhere else that starts a game; its words are the action's own.
            const std::wstring& label = cfg_.games[heroGame_].actions[heroAction_].label;
            SetWindowTextW(heroPlayBtn_, (label.empty() ? cfg_.t("hero.play") : label).c_str());
            btnInfo_[heroPlayBtn_].icon = kIconPlay;
            int pw = std::max<int>(t.s(120), textWidth(dc, t.button, windowText(heroPlayBtn_)) + t.s(64));
            MoveWindow(heroPlayBtn_, bx, by, pw, bh, TRUE);
            bx += pw + t.s(10);
        }
        MoveWindow(heroBtn_, bx, by, t.s(104), bh, TRUE);
        SetWindowTextW(heroBtn_, cfg_.t("hero.open").c_str());
        y0 = hero_.bottom + t.s(4);
    } else {
        heroAction_ = -1;
    }
    ShowWindow(heroBtn_, heroGame_ >= 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(heroPlayBtn_, heroAction_ >= 0 ? SW_SHOW : SW_HIDE);
    top_ = {left, y0, right, y0 + t.s(52)};

    int x = top_.left + t.s(kPad), boxH = t.s(36), y = top_.top + (height(top_) - boxH) / 2;
    int searchRight = top_.right - t.s(kPad);
    int cw = std::min<int>(t.s(narrowMode ? 170 : 200), textWidth(dc, t.button, windowText(catBtn_)) + t.s(58));
    MoveWindow(catBtn_, x, y, cw, boxH, TRUE);
    x += cw + t.s(8);
    if (!narrowMode) {  // the toolbar to the right of the search box: order, then card size
        int sizeW = std::max<int>(t.s(96), textWidth(dc, t.button, windowText(sizeBtn_)) + t.s(54));
        MoveWindow(sizeBtn_, searchRight - sizeW, y, sizeW, boxH, TRUE);
        searchRight -= sizeW + t.s(8);
        int sortW = std::min<int>(t.s(180), textWidth(dc, t.button, windowText(sortBtn_)) + t.s(58));
        MoveWindow(sortBtn_, searchRight - sortW, y, sortW, boxH, TRUE);
        searchRight -= sortW + t.s(8);
    }
    searchBox_ = {x, y, std::max<LONG>(x + t.s(120), static_cast<LONG>(searchRight)), y + boxH};
    // The edit sits inside a painted rounded box; center its single text line.
    int lineH = textHeight(dc, t.body, L"Ag", 100, DT_SINGLELINE);
    ReleaseDC(hwnd_, dc);
    MoveWindow(search_, searchBox_.left + t.s(30), searchBox_.top + (boxH - lineH) / 2, width(searchBox_) - t.s(40),
               lineH, TRUE);
    gridRc_ = {left, page ? barH : top_.bottom, right, status_.top};
    if (page) {
        top_ = {left, barH, right, barH};
        searchBox_ = {};
    }
    MoveWindow(grid_, gridRc_.left + t.s(kPad) - t.s(kGap) / 2, gridRc_.top, width(gridRc_) - t.s(kPad) * 2 + t.s(kGap),
               height(gridRc_), TRUE);
    layoutGrid();
    layoutRoomsPage();
    layoutGuidePage();
    layoutDetail();
    layoutToast();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Launcher::layoutGrid() {
    RECT r;
    GetClientRect(grid_, &r);
    int avail = r.right - GetSystemMetrics(SM_CXVSCROLL);
    int minTile = theme_.s((user_.compactCards ? kCardCompactW : kCardMinW) + kGap);
    int cols = std::max(1, avail / std::max(minTile, 1));
    int tw = std::max(minTile, avail / cols);
    int cardW = tw - theme_.s(kGap);
    // The picture is the whole card (names over its foot), cropped to 4:3 from the usual wide banner.
    int th = static_cast<int>(cardW * 0.75) + theme_.s(kGap);
    if (tw == tileW_ && th == tileH_) return;
    tileW_ = tw;
    tileH_ = th;
    LVTILEVIEWINFO tvi{sizeof tvi};
    tvi.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
    tvi.dwFlags = LVTVIF_FIXEDSIZE;
    tvi.sizeTile = {tw, th};
    tvi.cLines = 0;
    ListView_SetTileViewInfo(grid_, &tvi);
    for (auto it = artCache_.begin(); it != artCache_.end();) {
        if (!it->first.second) it = artCache_.erase(it);  // card art depends on the tile size
        else ++it;
    }
    ListView_Arrange(grid_, LVA_DEFAULT);
}

void Launcher::layoutDetail() {
    if (intro_) {
        intro_->layoutLater = true;
        return;
    }
    dl_ = {};
    tabRows_.clear();
    std::vector<HWND> all = {playerCombo_, saveCombo_, favBtn_,     aboutBtn_,    locateBtn_,   folderBtn_,  getPlayerBtn_,
                             stopHostBtn_, copyBtn_,     downloadBtn_, tabPlayBtn_, tabRoomsBtn_,
                             moreBtn_,     quickBtn_,    tabCodeEdit_, tabCodeBtn_, hostGameBtn_, joinGameBtn_};
    all.insert(all.end(), settingChecks_.begin(), settingChecks_.end());
    all.insert(all.end(), settingCombos_.begin(), settingCombos_.end());
    all.insert(all.end(), roomBtns_.begin(), roomBtns_.end());
    all.insert(all.end(), actionBtns_.begin(), actionBtns_.end());
    if (selected_ < 0 || width(detail_) <= 0) {
        for (HWND h : all) ShowWindow(h, SW_HIDE);
        return;
    }
    // Controls placed below are collected here and shown together at the end;
    // hiding everything first would throw keyboard focus out of the panel.
    std::set<HWND> shown;
    const Theme& t = theme_;
    const GameSpec& g = cfg_.games[selected_];
    HDC dc = GetDC(hwnd_);
    int x = detail_.left + t.s(kPad), w = width(detail_) - t.s(kPad) * 2, y = detail_.top + t.s(kPad);
    // The header (art, title, meta, status) is placed after the controls so the
    // art can shrink on short windows instead of running into the buttons.
    int titleH = textHeight(dc, t.title, displayName(g), w);
    std::wstring meta = categoryText(g);
    int metaH = meta.empty() ? 0 : textHeight(dc, t.caption, meta, w);
    bool hostingThis = relay_.running() && relayGame_ == selected_;
    int slotsH = g.maxPlayers > 0 ? t.s(hostingThis ? 56 : 40) : 0;
    // Games with online rooms get two tabs: Play (the actions) and Rooms.
    bool tabs = !g.onlineId.empty() && onlineOn_ && !g.comingSoon;
    if (!tabs) detailTab_ = 0;
    bool roomsTab = detailTab_ == 1;
    int tabsH = tabs ? t.s(36) : 0;

    // Everything below the description is laid out bottom-up from the panel's
    // end, so the buttons never move when the description is long.
    int bottom = detail_.bottom - t.s(kPad);
    int rowH = t.s(34), actionH = t.s(54), gap = t.s(8), roomRowH = t.s(40);
    const GameStats* st = user_.find(g.id);
    // Originals show only when Settings asks; "more" actions (direct IP, LAN)
    // wait behind "Other ways to connect".
    std::vector<size_t> mainActs, moreActs, allMain, allMore;
    bool coopUi = !roomsTab && !g.comingSoon && coopFlow(g);
    for (size_t i = 0; i < g.actions.size(); ++i) {
        if (!actionVisible(selected_, g.actions[i])) continue;
        (g.actions[i].more ? allMore : allMain).push_back(i);
        // Host and Join stand in for every way of hosting and joining.
        if (coopUi && (isCoopHostAction(g.actions[i]) || isCoopJoinAction(g.actions[i]))) continue;
        (g.actions[i].more ? moreActs : mainActs).push_back(i);
    }
    // What can be missing (game file, player) is judged on every visible action, folded away or not.
    std::vector<size_t> listed = allMain;
    if (showMore_ || coopUi) listed.insert(listed.end(), allMore.begin(), allMore.end());
    bool quick = tabs && roomsKnown_ && !g.joinAction.empty() && !notOnServer(g);
    const ActionSpec* missingSwf = nullptr;
    const PlayerSpec* missingPlayer = nullptr;
    const ServerSpec* missingRuntime = nullptr;
    // A PC mod whose game can't be found asks for its folder.
    const ModView* mv = g.mod ? modView(selected_) : nullptr;
    bool needFolder = mv && !mv->status.game.found();
    for (size_t i : listed) {
        const ActionSpec& a = g.actions[i];
        if (a.isMod()) continue;
        ResolvedAction r = lib_.resolve(g, a, user_);
        if (r.state == ActionState::NoSwf && !missingSwf) missingSwf = &a;
        if (r.state == ActionState::NoPlayer && !missingPlayer && r.player) missingPlayer = r.player;
        if (r.state == ActionState::NoRuntime && !missingRuntime) missingRuntime = &a.server;
    }
    std::vector<HostLine> hostLinesView = hostView();
    bool hosting = !hostLinesView.empty();
    int hostLines = static_cast<int>(hostLinesView.size());
    int hostLineH = textHeight(dc, t.caption, L"Ag", 100, DT_SINGLELINE) + t.s(4);
    int hostBoxH = t.s(14) + hostLines * hostLineH + t.s(8) + rowH + t.s(14);
    int comboH = t.s(30);
    bool picker = SendMessageW(playerCombo_, CB_GETCOUNT, 0, 0) > 2;  // "Automatic" + 2 players
    // Ruffle Together co-op games: which save the host starts from, with a line saying what that does.
    // (With Host and Join the choice moves into the Host window.)
    bool saveUi = !roomsTab && !g.mod && coopSaveGame(g) && coopSavePlayer() != nullptr && !coopUi;
    bool saveOk = saveUi && coopSaveSupported();
    int saveChoice = saveOk ? coopSaveChoiceFor(g) : 0;
    std::wstring saveNote;
    bool saveWarn = false;
    if (saveUi) {
        if (!saveOk) {
            saveNote = cfg_.t("coopSave.needs", {{L"VERSION", coopSavePlayer()->coopSaveSince}});
        } else if (saveChoice > 0) {
            saveNote = cfg_.t("coopSave.replaces", {{L"GAME", g.name}});
            saveWarn = true;
        } else if (std::wstring f = coopSaveFile(g); !f.empty() && fs::isFile(f)) {
            saveNote = cfg_.t("coopSave.progress");
        }
    }
    int saveNoteH = saveNote.empty() ? 0 : textHeight(dc, t.caption, saveNote, w, DT_WORDBREAK) + t.s(4);
    // The game's own settings (checkboxes and dropdowns) sit under the note.
    int checkH = t.s(28);
    static const std::vector<Setting> noSettings;
    const std::vector<Setting>& setList = roomsTab || g.mod || g.comingSoon || coopUi ? noSettings : settingsFor(g);
    bool setUi = !setList.empty();
    int setsH = 0;
    for (const auto& sd : setList) setsH += (sd.type == "choice" ? comboH + t.s(4) : checkH) + t.s(2);
    if (setUi) setsH += t.s(4);
    int saveH = (saveUi ? comboH + t.s(8) + (saveNoteH ? saveNoteH + t.s(4) : 0) : 0) + setsH;
    std::vector<CatalogFile> downloadable = missingSwf ? missingGameFiles(g) : std::vector<CatalogFile>{};
    bool isUpdate = downloadable.empty();
    if (isUpdate) downloadable = outdatedFor(g);
    std::vector<RoomInfo> roomList;
    if (tabs)
        if (auto it = rooms_.find(g.id); it != rooms_.end()) roomList = it->second;

    auto place = [&](HWND b, bool show) {
        if (!show) return;
        MoveWindow(b, x, bottom - rowH, w, rowH, TRUE);
        shown.insert(b);
        bottom -= rowH + gap;
    };
    int headerBottom = bottom;
    if (!roomsTab) {
        // Short windows: thinner action buttons (no second line) and no player
        // picker, so the game's name and state stay visible above the buttons.
        int fixed = rowH + gap + (missingPlayer || missingRuntime ? rowH + gap : 0) + (missingSwf || needFolder ? rowH + gap : 0) +
                    (downloadable.empty() ? 0 : rowH + gap) + (hosting ? hostBoxH + t.s(12) : 0) +
                    (moreActs.empty() ? 0 : rowH + gap) +
                    (showMore_ ? static_cast<int>(moreActs.size()) * (actionH + gap) : 0);
        int headerMin = titleH + t.s(4 + 24 + 16) + (slotsH ? slotsH + t.s(12) : 0) + (tabs ? tabsH + t.s(12) : 0);
        int actionsH = static_cast<int>(mainActs.size() + (quick ? 1 : 0) + (coopUi ? 2 : 0)) * (actionH + gap) +
                       (picker ? comboH + t.s(16) : 0) + saveH;
        if (bottom - fixed - actionsH < y + headerMin) {
            actionH = t.s(40);
            picker = false;
            saveUi = false;
            setUi = false;
        }

        // Row: favorite + folder (ghost buttons).
        SetWindowTextW(favBtn_, cfg_.t(st && st->favorite ? "button.unfavorite" : "button.favorite").c_str());
        SetWindowTextW(folderBtn_, cfg_.t("button.folder").c_str());
        int half = (w - gap) / 2;
        // Favorite and Open folder share the row (the guide is the page the panel sits on, so no Guide button).
        int bx = x;
        for (HWND b : {favBtn_, folderBtn_}) {
            int bw = b == folderBtn_ ? x + w - bx : half;
            MoveWindow(b, bx, bottom - rowH, bw, rowH, TRUE);
            bx += bw + gap;
        }
        (void)half;
        shown.insert(favBtn_);
        shown.insert(folderBtn_);
        bottom -= rowH + gap;

        // Fix-it buttons for whatever stops the game from starting.
        if (missingPlayer)
            SetWindowTextW(getPlayerBtn_,
                           cfg_.t(missingPlayer->install.enabled() ? "button.installPlayer" : "button.getPlayer",
                                  {{L"PLAYER", missingPlayer->name}})
                               .c_str());
        if (!missingPlayer && missingRuntime)
            SetWindowTextW(getPlayerBtn_, cfg_.t("button.getRuntime", {{L"RUNTIME", missingRuntime->name}}).c_str());
        EnableWindow(getPlayerBtn_, !installing_);
        btnInfo_[getPlayerBtn_].busy = installing_ && !jobIsGame_;
        place(getPlayerBtn_, missingPlayer != nullptr || missingRuntime != nullptr);
        if (missingSwf)
            SetWindowTextW(locateBtn_, (listed.size() > 1 ? cfg_.t("button.locateFor", {{L"ACTION", missingSwf->label}})
                                                          : cfg_.t("button.locate"))
                                           .c_str());
        if (needFolder) {
            mod::Spec ms = modSpec(selected_);
            SetWindowTextW(locateBtn_, ms.t("mod.chooseFolder", mod::withSpec(lib_.vars(), ms)).c_str());
        }
        place(locateBtn_, missingSwf != nullptr || needFolder);
        if (!downloadable.empty()) {
            uint64_t bytes = 0;
            for (const auto& f : downloadable) bytes += f.size;
            SetWindowTextW(downloadBtn_, cfg_.t(isUpdate ? "button.updateGame" : "button.downloadGame",
                                                {{L"SIZE", formatBytes(bytes)}})
                                             .c_str());
            // Missing files block play, so that fix leads; an update is optional.
            btnInfo_[downloadBtn_].style = isUpdate ? BtnStyle::Secondary : BtnStyle::Primary;
        }
        EnableWindow(downloadBtn_, !installing_);
        btnInfo_[downloadBtn_].busy = installing_ && jobIsGame_;
        place(downloadBtn_, !downloadable.empty());

        // Hosting box: relay status, peers, this PC's addresses.
        if (hosting) {
            dl_.host = {x, bottom - hostBoxH, x + w, bottom};
            int by = dl_.host.top + t.s(14);
            for (int i = 0; i < hostLines; ++i, by += hostLineH)
                dl_.hostLines.push_back({x + t.s(14), by, x + w - t.s(14), by + hostLineH});
            int bw = (w - t.s(28) - gap) / 2;
            MoveWindow(copyBtn_, x + t.s(14), dl_.host.bottom - t.s(14) - rowH, bw, rowH, TRUE);
            MoveWindow(stopHostBtn_, x + t.s(14) + bw + gap, dl_.host.bottom - t.s(14) - rowH,
                       w - t.s(28) - bw - gap, rowH, TRUE);
            SetWindowTextW(copyBtn_, cfg_.t("button.copyAddress").c_str());
            SetWindowTextW(stopHostBtn_, cfg_.t("button.stopHost").c_str());
            shown.insert(copyBtn_);
            shown.insert(stopHostBtn_);
            bottom = dl_.host.top - t.s(12);
        }

        // Action buttons, bottom-up: the "more" ones under their toggle, then the
        // rest, Quick play right under the leading action.
        auto placeAction = [&](size_t i) {
            HWND b = actionBtns_[i];
            const ActionSpec& a = g.actions[i];
            bool starting = std::any_of(sessions_.begin(), sessions_.end(), [&](const Session& s) {
                return s.game == selected_ && s.action == a.id && s.server && !s.pid;
            });
            bool modWorking = a.isMod() && modJobGame_ == selected_ && a.modOp == modJobOp_;
            btnInfo_[b].busy = starting || modWorking;
            btnInfo_[b].progress = modWorking;
            if (a.isLink()) btnInfo_[b].style = BtnStyle::Link;
            MoveWindow(b, x, bottom - actionH, w, actionH, TRUE);
            shown.insert(b);
            bottom -= actionH + gap;
        };
        if (showMore_)
            for (size_t k = moreActs.size(); k-- > 0;) placeAction(moreActs[k]);
        if (!moreActs.empty()) {
            SetWindowTextW(moreBtn_, cfg_.t(showMore_ ? "button.less" : "button.more").c_str());
            btnInfo_[moreBtn_].icon = showMore_ ? kIconLess : kIconMore;
            place(moreBtn_, true);
        }
        for (size_t k = mainActs.size(); k-- > 1;) placeAction(mainActs[k]);
        if (quick) {
            const RoomInfo* busiest = nullptr;
            for (const auto& r : roomList)
                if ((r.max <= 0 || r.players < r.max) && (!busiest || r.players > busiest->players)) busiest = &r;
            SetWindowTextW(quickBtn_, cfg_.t("button.quickPlay").c_str());
            btnInfo_[quickBtn_].sub = cfg_.t(busiest ? "button.quickJoin" : "button.quickHost");
            MoveWindow(quickBtn_, x, bottom - actionH, w, actionH, TRUE);
            EnableWindow(quickBtn_, joinReady(g));
            shown.insert(quickBtn_);
            bottom -= actionH + gap;
        }
        if (!mainActs.empty()) placeAction(mainActs[0]);
        // Host and Join lead the list (placed last, so they sit on top).
        if (coopUi) {
            for (HWND b : {joinGameBtn_, hostGameBtn_}) {
                MoveWindow(b, x, bottom - actionH, w, actionH, TRUE);
                shown.insert(b);
                bottom -= actionH + gap;
            }
            // Without the game files (or the player) there is nothing to host or join: Download or Locate leads.
            bool ready = coopReady(g);
            EnableWindow(hostGameBtn_, ready && !installing_);
            EnableWindow(joinGameBtn_, ready && !installing_);
        }

        // The game's settings, right above the action buttons (the last one lowest).
        if (setUi) {
            bottom -= t.s(4);
            for (size_t k = setList.size(); k-- > 0;) {
                const Setting& sd = setList[k];
                bool ok = playerAtLeast(sd.since);
                std::string value = settingValue(g, sd);
                if (sd.type == "checkbox") {
                    SetWindowTextW(settingChecks_[k], (ok ? sd.label : cfg_.t("setting.needs", {{L"LABEL", sd.label}, {L"VERSION", sd.since}})).c_str());
                    SendMessageW(settingChecks_[k], BM_SETCHECK, ok && value == "1" ? BST_CHECKED : BST_UNCHECKED, 0);
                    EnableWindow(settingChecks_[k], ok);
                    MoveWindow(settingChecks_[k], x, bottom - checkH, w, checkH, TRUE);
                    shown.insert(settingChecks_[k]);
                    bottom -= checkH + t.s(2);
                } else {
                    std::wstring label = ok ? sd.label : cfg_.t("setting.needs", {{L"LABEL", sd.label}, {L"VERSION", sd.since}});
                    int labelW = std::min(textWidth(dc, t.strong, label) + t.s(12), w / 2);
                    dl_.settingLabels.push_back({{x, bottom - comboH, x + labelW, bottom}, label});
                    HWND cb = settingCombos_[k];
                    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
                    int sel = 0;
                    for (size_t c = 0; c < sd.choices.size(); ++c) {
                        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sd.choices[c].label.c_str()));
                        if (sd.choices[c].id == value) sel = static_cast<int>(c);
                    }
                    SendMessageW(cb, CB_SETCURSEL, sel, 0);
                    EnableWindow(cb, ok);
                    MoveWindow(cb, x + labelW, bottom - comboH, w - labelW, t.s(240), TRUE);
                    shown.insert(cb);
                    bottom -= comboH + t.s(4) + t.s(2);
                }
            }
        }

        // Co-op save: the note under the choice, the choice right above the settings.
        if (saveUi) {
            bottom -= t.s(4);
            if (saveNoteH) {
                dl_.saveNote = {x, bottom - saveNoteH, x + w, bottom};
                dl_.saveNoteText = saveNote;
                dl_.saveNoteWarn = saveWarn;
                bottom -= saveNoteH + t.s(4);
            }
            int labelW = textWidth(dc, t.strong, cfg_.t("coopSave.label")) + t.s(12);
            dl_.saveLabel = {x, bottom - comboH, x + labelW, bottom};
            MoveWindow(saveCombo_, x + labelW, bottom - comboH, w - labelW, t.s(240), TRUE);
            SendMessageW(saveCombo_, CB_SETCURSEL, saveChoice, 0);
            EnableWindow(saveCombo_, saveOk);
            shown.insert(saveCombo_);
            bottom -= comboH + t.s(8);
        }

        // Player picker, only worth showing when there is a choice.
        if (picker) {
            bottom -= t.s(4);
            int labelW = textWidth(dc, t.strong, cfg_.t("detail.player")) + t.s(12);
            dl_.playerLabel = {x, bottom - comboH, x + labelW, bottom};
            MoveWindow(playerCombo_, x + labelW, bottom - comboH, w - labelW, t.s(240), TRUE);
            shown.insert(playerCombo_);
            bottom -= comboH + t.s(12);
        }
        headerBottom = bottom;
    } else {
        // Rooms tab: join by code at the bottom, the room rows above it.
        int jbw = textWidth(dc, t.button, cfg_.t("roomsPage.joinCode")) + t.s(32);
        int lineH = textHeight(dc, t.button, L"Ag", 100, DT_SINGLELINE);
        dl_.codeBox = {x, bottom - rowH, x + w - jbw - gap, bottom};
        MoveWindow(tabCodeEdit_, dl_.codeBox.left + t.s(12), dl_.codeBox.top + (rowH - lineH) / 2,
                   width(dl_.codeBox) - t.s(20), lineH, TRUE);
        MoveWindow(tabCodeBtn_, x + w - jbw, bottom - rowH, jbw, rowH, TRUE);
        EnableWindow(tabCodeBtn_, selected_ < 0 || joinReady(cfg_.games[selected_]));
        shown.insert(tabCodeEdit_);
        shown.insert(tabCodeBtn_);
        bottom -= rowH + t.s(14);
        headerBottom = bottom - roomRowH * 2;  // the header gives way to at least two rows
    }

    // The art gives way before the description does: keep room for two lines of it.
    int descMin = roomsTab || g.description.empty() ? 0 : textHeight(dc, t.body, L"Ag", w, DT_SINGLELINE) * 2 + t.s(10);
    const SiteGame* siteInfo = roomsTab ? nullptr : siteOf(selected_);
    int siteH = siteBlockHeight(dc, w, siteInfo, g.comingSoon);
    int headerH = titleH + t.s(4) + metaH + t.s(10) + (slotsH ? slotsH + t.s(12) : 0) + t.s(24) + t.s(12) +
                  (tabs ? tabsH + t.s(12) : 0) + descMin + (siteH > 0 ? siteH + t.s(12) : 0);  // the art shrinks first
    // On the game's page its header shows the art, so the panel leaves it out.
    int artH = guidePage() ? 0 : std::min(static_cast<int>(w * 0.5625), t.s(200));
    int room = headerBottom - t.s(8) - y - headerH - t.s(16);
    if (room < artH) artH = room >= t.s(72) ? room : 0;
    if (artH > 0) {
        dl_.art = {x, y, x + w, y + artH};
        y += artH + t.s(16);
    }
    // Each header line is shown only while it still fits above the controls.
    int limit = headerBottom - t.s(8);
    if (y + titleH <= limit) {
        dl_.title = {x, y, x + w, y + titleH};
        y += titleH + t.s(4);
    }
    if (metaH && y + metaH + t.s(10 + 24) <= limit) {
        dl_.meta = {x, y, x + w, y + metaH};
        y += metaH + t.s(10);
    }
    if (slotsH && y + slotsH + t.s(12) <= limit) {
        dl_.slots = {x, y, x + w, y + slotsH};
        y += slotsH + t.s(12);
    }
    if (y + t.s(24) <= limit) {
        dl_.status = {x, y, x + w, y + t.s(24)};
        y += t.s(24) + t.s(12);
    }
    // The website's grade, stage and what stays in sync, while there is still room for the buttons.
    if (siteH > 0 && y + siteH + t.s(12) + descMin <= limit) {
        dl_.site = {x, y, x + w, y + siteH};
        y += siteH + t.s(12);
    }
    if (tabs) {
        std::wstring playLabel = cfg_.t("tab.play");
        std::wstring roomsLabel = roomList.empty() ? cfg_.t("tab.rooms")
                                                   : cfg_.t("tab.roomsCount", {{L"N", std::to_wstring(roomList.size())}});
        int tw1 = textWidth(dc, t.button, playLabel) + t.s(28), tw2 = textWidth(dc, t.button, roomsLabel) + t.s(28);
        dl_.tabs = {x, y, x + w, y + tabsH};
        SetWindowTextW(tabPlayBtn_, playLabel.c_str());
        SetWindowTextW(tabRoomsBtn_, roomsLabel.c_str());
        btnInfo_[tabPlayBtn_].selected = !roomsTab;
        btnInfo_[tabRoomsBtn_].selected = roomsTab;
        MoveWindow(tabPlayBtn_, x, y, tw1, tabsH, TRUE);
        MoveWindow(tabRoomsBtn_, x + tw1, y, tw2, tabsH, TRUE);
        shown.insert(tabPlayBtn_);
        shown.insert(tabRoomsBtn_);
        y += tabsH + t.s(12);
    }

    if (roomsTab) {
        dl_.rooms = {x, y, x + w, bottom};
        size_t fit = static_cast<size_t>(std::max(0, (bottom - y) / roomRowH));
        size_t n = std::min(roomList.size(), fit);
        while (roomBtns_.size() < n) {
            HWND b = makeButton(kIdRoom0 + static_cast<int>(roomBtns_.size()), BtnStyle::Secondary);
            btnInfo_[b].label = cfg_.t("rooms.join");
            roomBtns_.push_back(b);
            all.push_back(b);
        }
        for (size_t i = 0; i < n; ++i) {
            RECT row{x, y + static_cast<int>(i) * roomRowH, x + w, y + static_cast<int>(i + 1) * roomRowH};
            tabRows_.push_back({selected_, roomList[i], row});
            int bw = t.s(64);
            MoveWindow(roomBtns_[i], row.right - bw, row.top + t.s(4), bw, roomRowH - t.s(8), TRUE);
            // The accessible name says which room; the button itself just reads "Join".
            SetWindowTextW(roomBtns_[i], cfg_.t("rooms.joinName", {{L"CODE", roomList[i].code}}).c_str());
            EnableWindow(roomBtns_[i], roomList[i].max <= 0 || roomList[i].players < roomList[i].max);
            shown.insert(roomBtns_[i]);
        }
    } else {
        // Text between the tabs and the controls, dropped when it doesn't fit.
        int textBottom = bottom - t.s(8);
        std::wstring stats = g.comingSoon ? std::wstring() : statsText(st);
        int statsH = textHeight(dc, t.caption, stats, w);
        if (textBottom - statsH >= y) {
            dl_.stats = {x, textBottom - statsH, x + w, textBottom};
            textBottom = dl_.stats.top - t.s(10);
        }
        dl_.desc = {x, y, x + w, std::max(y, textBottom)};
        descText_ = fitText(dc, t.body, g.description, w, height(dl_.desc));
    }
    ReleaseDC(hwnd_, dc);
    for (HWND h : all) ShowWindow(h, shown.count(h) ? SW_SHOW : SW_HIDE);
    InvalidateRect(hwnd_, &detail_, FALSE);
    for (HWND hb : all) InvalidateRect(hb, nullptr, FALSE);
    wake();
}

}  // namespace fl::ui
