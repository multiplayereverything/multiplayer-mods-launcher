// The launcher window: category sidebar, search, a grid of game cards and a
// details panel with the game's actions (Play / Host / Join...). Games run in
// their own processes; the window only starts them, watches them exit and
// runs the built-in relay while someone hosts.
#pragma once
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

#include "core/config.h"
#include "core/discord.h"
#include "core/guide.h"
#include "core/install.h"
#include "core/library.h"
#include "core/lobby.h"
#include "core/mod/manager.h"
#include "core/process.h"
#include "core/relay.h"
#include "core/rooms.h"
#include "core/settings.h"
#include "core/site.h"
#include "core/webserver.h"
#include "theme.h"

namespace fl::ui {

struct ModDone;

struct LauncherOptions {
    std::wstring snapshot;   // render the window to this PNG and exit
    std::string select;      // game id to select at start
    int width = 0, height = 0;
    bool demoHost = false;   // snapshot: start the relay for the selected game
    bool guide = false;      // snapshot: open the selected game's guide page
    int guideScroll = 0;     // ...and scroll it this many pixels
    std::string lobby;       // snapshot: "setup" or "room" renders the lobby window instead
};

class Launcher {
public:
    Launcher(const Config& cfg, Library& lib, UserData& user, std::wstring userPath);
    ~Launcher();
    int run(const LauncherOptions& opt);

    // Starts an action without the window (command line --launch). Blocks until
    // the game exits when it hosts, so the relay stays up for the session.
    static int launchHeadless(const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath,
                              const GameSpec& game, const ActionSpec& action, std::wstring& message);

private:
    struct NavItem {
        std::string key;  // all | fav | ready | rooms | cat:<id>
        std::wstring label;
        std::wstring icon;  // one icon-font glyph, drawn before the label
        std::string group;  // a line is drawn where the group changes
        int count = 0;
    };
    struct Session {
        int id = 0;
        DWORD pid = 0;  // the player (or browser); 0 while an HTML5 server starts
        int game = -1;
        std::string action;
        std::wstring player;
        ULONGLONG started = 0;
        int64_t startedUnix = 0;  // for Discord's elapsed time
        std::wstring room;        // the room code it joined, if it came from a room list
        bool host = false;
        // HTML5: what runs next to the browser, and what it told us.
        std::shared_ptr<sys::BackgroundProcess> server;
        std::shared_ptr<WebServer> web;
        std::vector<std::wstring> shareLinks, lanLinks;
        std::wstring url, exe, workDir;
        std::vector<std::wstring> args;  // still holding ${URL}
    };
    // One line of the hosting box: the relay's addresses or an HTML5 server's links.
    struct HostLine {
        std::wstring text;
        int role = 0;  // 0 heading, 1 muted label, 2 address
    };
    enum class BtnStyle { Primary, Secondary, Ghost, Danger, Check, Tab, Link };
    struct BtnInfo {
        BtnStyle style = BtnStyle::Secondary;
        std::wstring sub;    // second, smaller line
        std::wstring label;  // drawn text when shorter than the accessible name ("Join")
        std::wstring icon;   // icon-font glyph before the label
        std::wstring tag;    // small pill after the label ("Original")
        bool ready = true;
        bool selected = false;  // Tab: the open tab
        bool busy = false;      // a light runs around the border while its work runs
        bool progress = false;  // ...and a line along the bottom shows how far a mod job is
    };
    struct DetailLayout {
        RECT art{}, title{}, meta{}, slots{}, status{}, desc{}, controlsHead{}, controls{}, stats{}, playerLabel{}, host{}, saveLabel{}, saveNote{};
        std::vector<std::pair<RECT, std::wstring>> settingLabels;  // the names beside the dropdown settings
        std::wstring saveNoteText;
        bool saveNoteWarn = false;
        std::vector<RECT> hostLines;
        RECT rooms{};    // Rooms tab: the room rows
        RECT tabs{};     // the tab strip, underlined
        RECT codeBox{};  // Rooms tab: the painted box around the room-code field
        RECT site{};     // what the website says: grade, stage, version, what stays in sync
    };
    // One room in a list (the Rooms page or the details panel's Rooms tab).
    struct RoomRow {
        int game = -1;
        RoomInfo room;
        RECT rc{};
    };

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK gridProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR self);
    static LRESULT CALLBACK searchProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR self);
    static LRESULT CALLBACK buttonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR self);
    static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK toastProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT handle(UINT m, WPARAM w, LPARAM l);

    void create(const LauncherOptions& opt);
    void createControls();
    void applyFonts();
    HWND makeButton(int id, BtnStyle style, HWND parent = nullptr);
    void relayout();
    void layoutGrid();
    void layoutDetail();

    void buildNav();
    void rebuildGrid();
    bool matches(int gameIndex) const;
    void select(int gameIndex);
    int selectedAction(int preferred) const;
    void refreshAll();

    void paint(HDC dc, const RECT& dirty);
    void paintSidebar(Gdiplus::Graphics& g, HDC dc);
    void paintDetail(Gdiplus::Graphics& g, HDC dc);
    void paintStatus(Gdiplus::Graphics& g, HDC dc);
    void drawCard(HDC dc, int gameIndex, RECT r, bool selected, bool focused);
    void drawButton(HWND button, HDC dc, RECT r, UINT state);
    void drawNavItem(const DRAWITEMSTRUCT& d);
    void paintSlots(Gdiplus::Graphics& g, const GameSpec& game);
    std::wstring categoryText(const GameSpec& game) const;
    std::wstring statsText(const GameStats* st) const;
    Gdiplus::Bitmap* art(int gameIndex, int w, int h, bool detail);
    void ensureImage(int gameIndex);
    void layoutHero();
    // The banner above the grid: the game played last, with a button that opens it (Quiver's "Continue playing").
    int heroCandidate() const;
    bool heroWanted() const;
    void paintHero(Gdiplus::Graphics& g);

    // `flashvars` are handed to the game when the player supports it (flashvarArg).
    bool runAction(int gameIndex, size_t actionIndex,
                   const std::vector<std::pair<std::wstring, std::wstring>>& flashvars = {});
    bool minimizeOnLaunch() const;
    bool runWeb(int gameIndex, const ActionSpec& a, ResolvedAction r,
                const std::vector<std::pair<std::wstring, std::wstring>>& flashvars);
    bool launchBrowser(Session& s);
    void onServerLine(int sessionId, const std::wstring& line);
    void onServerReady(int sessionId, bool ok);
    void endSession(std::vector<Session>::iterator it);
    Session* webHostSession(int gameIndex);
    std::vector<HostLine> hostView();
    std::wstring clipboardText();

    // Rooms on the central server, polled on a worker thread.
    void startRoomPolling();
    void onRooms();
    void joinRoom(size_t index);
    // Downloaded game files with a newer build in the catalog.
    std::vector<CatalogFile> outdatedFor(const GameSpec& game) const;
    void checkOutdated();

    // The Settings window (settings.cpp). Modal; changes apply at once.
    void openSettings();
    LRESULT settingsHandle(HWND h, UINT m, WPARAM w, LPARAM l);
    void layoutSettings();
    void onProcessExit(DWORD pid, DWORD code);
    void stopRelay(bool announce);
    void locateFile();
    void openFolder();
    void getPlayer();
    // Asks, then downloads and unpacks `player` on a worker thread.
    void offerInstall(const PlayerSpec& player, bool firstRun);
    void onInstalled();
    // Game files the catalog offers for this game that are not on disk yet.
    std::vector<CatalogFile> missingGameFiles(const GameSpec& game) const;
    void offerGameDownload(const GameSpec& game);
    void loadCatalog();
    HWND downloadBtn_ = nullptr, settingsBtn_ = nullptr;
    std::vector<HWND> roomBtns_;
    std::wstring installText() const;
    void copyAddress();
    void copyText(const std::wstring& text);
    std::wstring roomCountText(const std::string& gameId) const;
    void toggleFavorite();
    // The guide page (launcher_guide.cpp): steps, keys and tips for the selected game, in place of the grid.
    struct GuideRow {
        size_t block = 0;
        int y = 0, h = 0;  // from the top of the page's content
    };
    bool guidePage() const;
    void openGuide(int gameIndex);
    void closeGuide();
    void layoutGuidePage();
    void paintGuidePage(Gdiplus::Graphics& g, HDC dc);
    void scrollGuide(int pixels);
    int guideGame_ = -1, guideScroll_ = 0, guideContentH_ = 0;
    GuidePage guide_;
    std::vector<GuideRow> guideRows_;
    RECT guideView_{}, guideHead_{}, guideFoot_{};
    HWND guideBack_ = nullptr;
    std::vector<HWND> guideLinkBtns_;
    std::wstring emptyTitle() const;
    std::wstring emptyBody() const;
    int gridRank(int gameIndex) const;  // PC mods before Flash games; favorites, ready, the rest; coming soon last
    void onPlayerChoice();
    // The host's choice of co-op save (Ruffle Together games). Supported: the installed player knows the option.
    // A game's settings: from the catalog when it lists some for the game, else from launcher.json. The
    // player's choices are kept per game in settings.json.
    bool playerAtLeast(const std::wstring& version) const;
    const std::vector<Setting>& settingsFor(const GameSpec& g) const;
    std::string settingValue(const GameSpec& g, const Setting& s) const;  // "1"/"0" or the choice id
    std::vector<std::wstring> settingArgs(const GameSpec& g, const std::vector<std::wstring>& actionArgs) const;
    void onSettingChanged(int index, bool combo);
    std::array<HWND, kMaxSettings> settingChecks_{}, settingCombos_{};
    std::map<std::string, std::vector<Setting>> catalogSettings_;
    bool coopSaveGame(const GameSpec& g) const { return g.onlineId == "ruffle-coop"; }
    const PlayerSpec* coopSavePlayer() const;
    bool coopSaveSupported() const;
    std::wstring coopSaveFile(const GameSpec& g) const;
    int coopSaveChoiceFor(const GameSpec& g) const;
    void onSaveChoice();
    void notice(const std::wstring& text, const Color& color);
    void save();
    std::wstring stateText(int gameIndex, Color& color) const;
    std::wstring formatDuration(int64_t seconds) const;
    std::wstring formatAgo(int64_t unixTime) const;
    bool isPlaying(int gameIndex) const;
    bool isNarrow() const;

    // Animations (core/motion.h). One frame timer runs only while something
    // moves; with Windows' "Show animations" off, and in snapshots, nothing does.
    void wake();
    void onFrame();
    bool pulsing() const;
    // The opening entrance: the finished first frame, controls included, is
    // drawn once into a bitmap; for a moment the window shows it in pieces
    // (sidebar, cards, details panel) while the real controls wait hidden.
    struct Intro {
        ULONGLONG from = 0;
        std::unique_ptr<Gdiplus::Bitmap> shot;
        std::vector<RECT> cards;  // visible game cards, window coordinates
        RECT gridBar{};           // the grid's scroll bar
        std::vector<HWND> hidden;
        bool layoutLater = false;  // something changed while it ran
    };
    void startIntro();
    void endIntro();
    void paintIntro(Gdiplus::Graphics& g);
    std::unique_ptr<Intro> intro_;
    // Cards lift a little under the mouse: game index -> 0..1.
    std::map<int, float> lift_;
    ULONGLONG liftTick_ = 0;
    bool stepLift();  // true while a card is still moving
    bool anyBusy() const;

    // Online play: the Rooms page, the details panel's Rooms tab, Quick play,
    // the server status, toasts and Discord.
    bool roomsPage() const;
    void layoutRoomsPage();
    void paintRoomsPage(Gdiplus::Graphics& g);
    void paintRoomRow(Gdiplus::Graphics& g, const RoomRow& row, bool showGame, int joinW);
    std::vector<RoomRow> listedRooms(int onlyGame, bool hideFull) const;
    void joinListedRoom(int game, const RoomInfo& room);
    void joinByCode(HWND edit, int game);
    void quickPlay();
    size_t joinActionIndex(const GameSpec& g) const;
    int playersOnline() const;
    void refreshRoomsNav();
    std::wstring serverText(Color& color) const;
    void toast(const std::wstring& text, const Color& color);
    void layoutToast();
    void paintToast(HDC dc);
    void updatePresence();
    bool actionShown(const ActionSpec& a) const;

    // What the website says about each game (launcher_site.cpp).
    const SiteGame* siteOf(int gameIndex) const;
    void startSiteFetch();
    void onSite(int newCovers);
    void paintGradeChip(Gdiplus::Graphics& g, const RECT& box, const std::string& grade);
    void drawGlyphCentered(Gdiplus::Graphics& g, const std::wstring& glyph, const RECT& box, const Color& color);
    int siteBlockHeight(HDC dc, int width, const SiteGame* s, bool soon);
    int siteDotsHeight(HDC dc, int width, const SiteGame& s);
    std::wstring siteFacts(const SiteGame& s, bool soon) const;
    void paintSite(Gdiplus::Graphics& g, HDC dc, const RECT& r, const SiteGame& s, bool soon);
    std::vector<SiteGame> site_, pendingSite_;  // pendingSite_ under installMu_
    std::thread siteThread_;

    // PC game mods (launcher_mods.cpp).
    struct ModView {
        mod::Status status;       // the game, what is installed, whether the game runs
        mod::Source source;       // the newest version, from mods-info
        bool sourceKnown = false; // mods-info has answered
    };
    struct ModSource {
        mod::Source source;
        bool known = false;
    };
    mod::Spec modSpec(int gi) const;
    const ModView* modView(int gi) const;
    bool gameReady(int gi) const;  // playable: a Flash game that can start, or an installed mod
    void refreshMods();
    // A PC mod's game is opened and closed outside the launcher too (Steam, the desktop): look again every
    // two seconds, so "Close <game> first" appears and goes away by itself, and count the time it runs.
    void pollModGames();
    int modPollTick_ = 0, modSaveTick_ = 0;
    void startModsFetch();
    void onModsFetched();
    bool modUpdateAvailable(int gi) const;
    std::wstring modStateText(int gi, Color& color) const;
    bool modActionVisible(int gi, const ActionSpec& a) const;
    bool actionVisible(int gi, const ActionSpec& a) const;
    void applyModAction(int gi, const ActionSpec& a, HWND b);
    bool runModAction(int gi, const ActionSpec& a);
    void onModDone(ModDone* done);
    void chooseGameFolder();
    std::wstring modJobText() const;
    std::map<int, ModView> mods_;  // by game index
    bool forceModsFetch_ = false;  // the next mods lookup skips the short cache (set by F5)
    std::thread modsThread_;
    std::map<std::string, ModSource> pendingSources_;  // handed over in WM_APP_MODS (under installMu_)
    // The running mod job (the install thread is shared with player and game downloads).
    mod::CancelToken modCancel_;
    int modJobGame_ = -1;
    std::string modJobOp_;
    std::wstring modPhase_, modItem_;
    double modFraction_ = 0;

    HWND tabPlayBtn_ = nullptr, tabRoomsBtn_ = nullptr, moreBtn_ = nullptr, quickBtn_ = nullptr;
    HWND tabCodeEdit_ = nullptr, tabCodeBtn_ = nullptr;
    HWND pageGameCombo_ = nullptr, pageHideFull_ = nullptr, pageCodeEdit_ = nullptr, pageCodeBtn_ = nullptr;
    std::vector<HWND> pageRoomBtns_;
    std::vector<RoomRow> pageRows_, tabRows_;
    RECT pageTitle_{}, pageSub_{}, pageList_{}, pageCodeBox_{};
    int pageScroll_ = 0;
    int detailTab_ = 0;  // 0 Play, 1 Rooms
    bool showMore_ = false;
    int pingMs_ = -1, pendingPing_ = -1;  // -1: no answer
    bool serverAsked_ = false;            // at least one poll finished
    HWND toast_ = nullptr;
    std::wstring toastText_;
    Color toastColor_;
    ULONGLONG toastFrom_ = 0;
    std::unique_ptr<DiscordPresence> discord_;

    const Config& cfg_;
    Library& lib_;
    UserData& user_;
    std::wstring userPath_;
    Theme theme_;
    HWND hwnd_ = nullptr, nav_ = nullptr, navCombo_ = nullptr, search_ = nullptr, grid_ = nullptr,
         playerCombo_ = nullptr, saveCombo_ = nullptr;
    std::map<std::string, int> coopSaveChoice_;  // game id -> 0 continue, 1 solo save, 2 new game
    HWND aboutBtn_ = nullptr;
    HWND heroBtn_ = nullptr;
    RECT hero_{};  // empty when the banner is not shown
    int heroGame_ = -1;
    std::unique_ptr<Gdiplus::Bitmap> heroBmp_;
    int heroBmpGame_ = -1, heroBmpW_ = 0, heroBmpH_ = 0;
    HWND favBtn_ = nullptr, locateBtn_ = nullptr, folderBtn_ = nullptr, getPlayerBtn_ = nullptr,
         stopHostBtn_ = nullptr, copyBtn_ = nullptr;
    std::vector<HWND> actionBtns_;
    std::map<HWND, BtnInfo> btnInfo_;
    HWND hoverBtn_ = nullptr;
    int hoverItem_ = -1;

    RECT sidebar_{}, top_{}, searchBox_{}, gridRc_{}, detail_{}, status_{};
    DetailLayout dl_;
    std::wstring descText_, controlsText_;  // shortened to fit their rectangles
    std::vector<NavItem> navItems_;
    size_t navIndex_ = 0;
    std::vector<int> visible_;
    int selected_ = -1;
    bool suppressSelect_ = false;
    int tileW_ = 0, tileH_ = 0;

    std::map<std::pair<int, bool>, std::unique_ptr<Gdiplus::Bitmap>> artCache_;
    std::map<int, std::unique_ptr<Gdiplus::Bitmap>> images_;
    std::map<int, bool> imageTried_;

    std::vector<Session> sessions_;
    int nextSessionId_ = 0;
    std::vector<std::thread> helpers_;  // HTML5 server start-up waits
    std::unique_ptr<sys::ProcessWatcher> watcher_;
    Relay relay_;
    int relayGame_ = -1;
    std::vector<sys::NetAddress> addresses_;

    std::wstring notice_;
    Color noticeColor_;
    ULONGLONG noticeUntil_ = 0;
    bool snapshot_ = false;
    bool motion_ = true, frameTimer_ = false;
    ULONGLONG titleFrom_ = 0;                     // the title decodes after a selection; 0 when done
    std::map<std::wstring, ULONGLONG> codeFrom_;  // room code -> when it first showed up; 0 once decoded
    std::map<int, ULONGLONG> joinFrom_;           // slot -> when its player joined; 0 once settled
    size_t slotPeers_ = 0;

    std::thread installThread_;
    std::atomic<bool> installing_{false}, installCancel_{false};
    const PlayerSpec* installPlayer_ = nullptr;
    mutable std::mutex installMu_;
    InstallStatus installStatus_;
    InstallResult installResult_;
    bool jobIsGame_ = false;  // the running job downloads game files, not a player
    std::wstring jobName_;
    std::vector<CatalogFile> catalog_;
    std::set<std::wstring> outdated_;  // file names; filled by the catalog thread
    std::set<std::wstring> pendingOutdated_;
    bool pendingOutdatedReady_ = false;

    std::map<std::string, std::vector<RoomInfo>> rooms_, pendingRooms_;  // by game id
    bool roomsKnown_ = false, pendingRoomsOk_ = false;
    // Game ids the server answered "Unknown game" for: not on the server yet.
    std::set<std::string> rejected_, pendingRejected_;
    bool notOnServer(const GameSpec& g) const;
    std::thread roomsThread_;
    std::wstring roomServer_;  // the central server the room poll resolved (under installMu_)
    HANDLE roomsWake_ = nullptr;
    std::atomic<bool> roomsStop_{false}, onlineOn_{true};

    // The lobby window (launcher_lobby.cpp) for games with online.lobby: host or
    // join a room, ready up, chat; the host starts and every launcher opens the game.
    static LRESULT CALLBACK lobbyProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK lobbyListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR self);
    LRESULT lobbyHandle(HWND h, UINT m, WPARAM w, LPARAM l);
    void openLobby(int gameIndex);
    void closeLobby();
    void layoutLobby();
    void showLobbyPage(bool room);
    void fillLobbyRooms();
    void lobbyHost();
    void lobbyJoin(const std::wstring& codeOrAddress, const std::wstring& password, bool direct);
    void lobbyQuick(int gameIndex);  // join the busiest open room, or host one
    void onLobby();                  // WM_APP_LOBBY: the session changed
    void updateLobbyRoom();
    void launchFromLobby();
    void startDirectChecks(int port);
    std::wstring lobbyReasonText(const lobby::Snapshot& s) const;
    std::string lobbyVersion(int gameIndex);
    bool usesLobby(int gameIndex) const;
    void copyDiagnostics();
    bool askName(bool firstRun);  // false when the player closed it without a name
    // A friend's "host:port", typed or pasted in a small window. `address` comes in with a guess (the clipboard)
    // and goes out with what was typed; false when the window was closed without one.
    bool askAddress(std::wstring& address, const std::wstring& action);
    std::wstring textPrompt(bool address, const std::wstring& title, const std::wstring& body, const std::wstring& labelText,
                            const std::wstring& okText, const std::wstring& initial, bool anyTarget = false);
    static LRESULT CALLBACK nameProc(HWND h, UINT m, WPARAM w, LPARAM l);
    // Ruffle Together co-op games show Host and Join instead of every way to connect. Host opens a setup
    // window (players, connection, the game's settings); Join takes a room code or an address in one box.
    struct HostChoice {
        size_t action;  // the action that starts this combination
        int players;
        bool relay;     // a room code through the central server; otherwise a direct IP
    };
    static bool isCoopHostAction(const ActionSpec& a);
    static bool isCoopJoinAction(const ActionSpec& a);
    bool coopFlow(const GameSpec& g) const;
    std::vector<HostChoice> hostChoices(const GameSpec& g) const;
    bool hostSetup(int gi, size_t& action);
    bool joinReady(const GameSpec& g) const;  // its join action can start now (game files and player are there)
    bool coopReady(const GameSpec& g) const;  // a host or join action of the game can start right now
    void onHostGame();
    void onJoinGame();
    static LRESULT CALLBACK hostProc(HWND h, UINT m, WPARAM w, LPARAM l);
    HWND hostGameBtn_ = nullptr, joinGameBtn_ = nullptr;
    std::wstring pendingAddress_;  // an address the Join box already took: runAction doesn't ask again
    HWND lobbyWnd_ = nullptr;
    int lobbyGame_ = -1;
    std::unique_ptr<lobby::Session> lobby_;
    lobby::Snapshot lobbySnap_;
    DWORD lobbyPid_ = 0;
    bool lobbyRoomPage_ = false;
    int lobbyUpnpPort_ = 0;
    std::thread lobbyNetThread_;
    std::wstring lobbyNetText_;   // port test / firewall / UPnP results (UI thread)
    std::wstring lobbyMessage_;   // why the last room ended, shown on the setup page
    std::map<std::wstring, std::string> swfHashes_;  // file path -> SHA-256, per run
    HWND lbDirect_ = nullptr, lbRoomName_ = nullptr, lbMax_ = nullptr, lbPublic_ = nullptr, lbHostPw_ = nullptr,
         lbPort_ = nullptr, lbUpnp_ = nullptr, lbCode_ = nullptr, lbJoinPw_ = nullptr, lbList_ = nullptr;
    HWND lbTitle_ = nullptr, lbSub_ = nullptr, lbPlayers_ = nullptr, lbChat_ = nullptr, lbChatEdit_ = nullptr,
         lbInfo_ = nullptr, lbStatus_ = nullptr, lbCodeLabel_ = nullptr;
    std::vector<HWND> lbSetup_, lbRoom_;  // controls of each page
    std::vector<HWND> lbLabels_;
    size_t lbChatShown_ = 0;

    HWND settingsWnd_ = nullptr;
    HWND setOriginals_ = nullptr, setName_ = nullptr, setOffline_ = nullptr;
    HWND setMinimize_ = nullptr, setPlayer_ = nullptr, setOnline_ = nullptr, setDirText_ = nullptr,
         setReset_ = nullptr;
    std::vector<HWND> setLabels_;  // static texts, for fonts and colors
    std::thread catalogThread_;
    // Self-update: checked once at start on a worker thread (core/install.h).
    std::thread updateThread_;
    void startUpdateCheck();
    void onPlayerUpdated(const std::wstring& what);
    void onUpdated(const std::wstring& version);
    HWND setAutoUpdate_ = nullptr;
    std::string catalogRaw_;  // fetched by catalogThread_, handed over in WM_APP_CATALOG
};

}  // namespace fl::ui
