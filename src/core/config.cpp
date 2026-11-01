#include "config.h"

#include "core/mod/resources.h"

#include <algorithm>
#include <regex>
#include <set>

#include "fs.h"
#include "json.h"
#include "strings.h"

namespace fl {

bool parseColor(const std::wstring& text, Color& out) {
    std::wstring s = trim(text);
    if (s.empty() || s[0] != L'#') return false;
    s = s.substr(1);
    for (wchar_t c : s)
        if (!iswxdigit(c)) return false;
    auto hex = [&](size_t pos, size_t len) {
        return static_cast<uint8_t>(wcstoul(s.substr(pos, len).c_str(), nullptr, 16) * (len == 1 ? 17 : 1));
    };
    if (s.size() == 3) {
        out = {hex(0, 1), hex(1, 1), hex(2, 1), 255};
    } else if (s.size() == 6 || s.size() == 8) {
        out = {hex(0, 2), hex(2, 2), hex(4, 2), s.size() == 8 ? hex(6, 2) : uint8_t(255)};
    } else {
        return false;
    }
    return true;
}

static Color rgb(uint32_t v) { return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255}; }

ThemeColors darkPalette() {
    ThemeColors c;
    // Deep navy rather than neutral gray: the night-sky look of the launchers players compare us with.
    c.background = rgb(0x0B1119);
    c.surface = rgb(0x141B26);
    c.surfaceHover = rgb(0x1F2835);
    c.sidebar = rgb(0x070C13);
    c.sidebarText = rgb(0xF2F4F7);
    c.sidebarTextMuted = rgb(0x8E9BAE);
    c.primary = rgb(0xC8313A);  // the arcade start button; white on it is 5.3:1
    c.primaryHover = rgb(0xB42A33);
    c.onPrimary = rgb(0xFFFFFF);
    c.text = rgb(0xE6EBF2);
    c.textMuted = rgb(0x93A0B3);
    c.border = rgb(0x253041);
    c.focus = rgb(0x38BDF8);
    c.success = rgb(0x3CCB7F);
    c.warning = rgb(0xF2B544);
    c.error = rgb(0xFF6464);
    c.online = rgb(0x38BDF8);
    return c;
}

ThemeColors lightPalette() {
    ThemeColors c;
    c.background = rgb(0xFFFFFF);
    c.surface = rgb(0xF4F6F9);
    c.surfaceHover = rgb(0xE9EDF3);
    c.sidebar = rgb(0x1B2030);
    c.sidebarText = rgb(0xF2F4F7);
    c.sidebarTextMuted = rgb(0xA3ABBA);
    c.primary = rgb(0xB42A33);
    c.primaryHover = rgb(0x9C232B);
    c.onPrimary = rgb(0xFFFFFF);
    c.text = rgb(0x1C2128);
    c.textMuted = rgb(0x5B6472);
    c.border = rgb(0xD5DAE1);
    c.focus = rgb(0x2F6FEB);
    c.success = rgb(0x1F9D57);
    c.warning = rgb(0xB7791F);
    c.error = rgb(0xD93636);
    c.online = rgb(0x0E9C90);
    return c;
}

std::vector<std::pair<const char*, Color*>> colorFields(ThemeColors& c) {
    return {{"background", &c.background},
            {"surface", &c.surface},
            {"surfaceHover", &c.surfaceHover},
            {"sidebar", &c.sidebar},
            {"sidebarText", &c.sidebarText},
            {"sidebarTextMuted", &c.sidebarTextMuted},
            {"primary", &c.primary},
            {"primaryHover", &c.primaryHover},
            {"onPrimary", &c.onPrimary},
            {"text", &c.text},
            {"textMuted", &c.textMuted},
            {"border", &c.border},
            {"focus", &c.focus},
            {"success", &c.success},
            {"warning", &c.warning},
            {"error", &c.error},
            {"online", &c.online}};
}

const std::vector<std::pair<std::string, std::wstring>>& defaultTexts() {
    static const std::vector<std::pair<std::string, std::wstring>> texts = {
        {"nav.all", L"All games"},
        {"nav.favorites", L"Favorites"},
        {"nav.ready", L"Ready to play"},
        {"nav.rooms", L"Rooms"},
        {"nav.online", L"${N} online"},
        {"roomsPage.title", L"Open rooms"},
        {"roomsPage.subtitle", L"Rooms on the Multiplayer Everything server. Join one, or host from a game's page."},
        {"roomsPage.allGames", L"All games"},
        {"roomsPage.hideFull", L"Hide full rooms"},
        {"roomsPage.code", L"Room code"},
        {"roomsPage.join", L"Join"},
        {"roomsPage.joinCode", L"Join by code"},
        {"roomsPage.empty", L"No open rooms right now."},
        {"roomsPage.emptyBody", L"Host one from a game's page; it shows up here for everyone."},
        {"roomsPage.offline", L"Can't reach the server."},
        {"roomsPage.offlineBody", L"Check your internet connection. Direct connect still works from a game's page."},
        {"roomsPage.disabled", L"Online features are off."},
        {"roomsPage.disabledBody", L"Turn them on in Settings to see open rooms."},
        {"roomsPage.badCode", L"Room codes are 5 letters or digits, like VK7Q2."},
        {"roomsPage.pickGame", L"Pick the game for room ${CODE} in the list first."},
        {"roomsPage.host", L"Hosted by ${NAME}"},
        {"tab.play", L"Play"},
        {"tab.rooms", L"Rooms"},
        {"tab.roomsCount", L"Rooms (${N})"},
        {"button.quickPlay", L"Quick play"},
        {"button.quickJoin", L"Join the busiest open room"},
        {"button.quickHost", L"No rooms open: host one"},
        {"button.more", L"Other ways to connect"},
        {"button.less", L"Fewer options"},
        {"badge.original", L"Original"},
        {"server.checking", L"Checking the server\u2026"},
        {"server.ok", L"Server online (${MS} ms)"},
        {"server.okUntimed", L"Server online"},
        {"server.down", L"Server unreachable: use direct connect"},
        {"server.off", L"Online features off"},
        {"settings.name", L"Your name"},
        {"settings.nameNote", L"Shown to other players in online rooms."},
        {"settings.originals", L"Show the original versions of modded games"},
        {"settings.offline", L"Show offline play (solo and one keyboard)"},
        {"toast.joined", L"Joining room ${CODE}"},
        {"discord.playing", L"Playing ${GAME}"},
        // PC game mods: what the install engine and detection say (see core/mod).
        {"path.empty", L"Choose the folder where ${GAME_NAME} is installed."},
        {"path.invalidChars", L"The path contains characters that aren't allowed in folder names."},
        {"path.notAbsolute", L"Enter a full folder path, for example C:\\Games\\${GAME_NAME}."},
        {"path.isFile", L"That path points to a file. Choose a folder instead."},
        {"path.notFound", L"That folder doesn't exist."},
        {"path.unsafe", L"Choose the game's own folder, not a drive root or a Windows system folder."},
        {"path.ok", L"${GAME_NAME} found."},
        {"path.unvalidated", L"${FILE} wasn't found here. Make sure this is the ${GAME_NAME} folder."},
        {"path.missingFile", L"This doesn't look like the ${GAME_NAME} folder: ${FILE} is missing."},
        {"path.suggest", L"${GAME_NAME} was found in ${SUGGESTION}."},
        {"path.targetOutside", L"The install folder ${INSTALL_DIR} is outside the game folder. Check install.targetDir."},
        {"path.noSpace", L"Not enough free space: ${REQUIRED} needed, ${AVAILABLE} available."},
        {"error.foreign", L"Another mod (${OTHER}) owns this install folder."},
        {"error.recordElsewhere", L"The install record in ${FILE} belongs to another game folder (${OTHER}). It looks copied or moved, so nothing was changed."},
        {"error.payloadMissing", L"The mod package lost a file: ${FILE}. Antivirus software often removes files from mods it doesn't know: check its history (Windows Security, Protection history), allow the file, then try again. The package is downloaded afresh."},
        {"error.payloadCorrupt", L"The package file ${FILE} is damaged. Antivirus software may have changed it: allow it and try again. The package is downloaded afresh."},
        {"error.writeFailed", L"Couldn't write ${FILE}."},
        {"error.fileInUse", L"${FILE} is in use by another program. Close ${GAME_NAME} and try again."},
        {"error.accessDenied", L"The launcher isn't allowed to change ${FILE}. Try running it as administrator."},
        {"error.diskFull", L"The disk is full. Free up some space and try again."},
        {"error.cancelledBody", L"Nothing was changed."},
        {"error.unexpected", L"Something unexpected went wrong."},
        {"uninstall.notInstalled", L"${MOD_NAME} isn't installed here, or its install record is missing."},
        // PC game mods in the window.
        {"mod.update", L"Update to ${VERSION}"},
        {"mod.update.sub", L"You have ${INSTALLED}"},
        {"mod.install", L"Install ${GAME_NAME}"},
        {"mod.install.sub", L"Version ${VERSION}, into your ${GAME_NAME} folder"},
        {"mod.install.subUnknown", L"Into your ${GAME_NAME} folder"},
        {"mod.launch", L"Launch game"},
        {"mod.repair.subOffline", L"Needs the download, which isn't published yet"},
        {"badge.pc", L"PC"},
        {"mod.launch.sub", L"${MOD_NAME} ${INSTALLED} is installed"},
        {"mod.repair", L"Repair"},
        {"mod.repair.sub", L"Put back missing or damaged files"},
        {"mod.uninstall", L"Uninstall"},
        {"mod.uninstall.sub", L"Restore the files the mod replaced"},
        {"mod.status.notInstalled", L"Not installed"},
        {"mod.status.installed", L"Installed ${INSTALLED}"},
        {"mod.status.installedStale", L"Installed ${INSTALLED}, can't check for updates right now"},
        {"mod.status.update", L"Update ${VERSION} available"},
        {"mod.status.noGame", L"${GAME_NAME} not found"},
        {"mod.status.notPublished", L"Not published yet"},
        {"mod.status.running", L"Close ${GAME_NAME} first"},
        {"mod.status.problem", L"Install record problem"},
        {"mod.chooseFolder", L"Choose the ${GAME_NAME} folder…"},
        {"mod.pickFolder", L"Choose the folder that contains ${GAME_NAME}"},
        {"mod.busy", L"Another install or download is running; wait for it to finish."},
        {"mod.launched", L"Started ${GAME_NAME}."},
        {"mod.done.install", L"${MOD_NAME} ${VERSION} is installed."},
        {"mod.done.update", L"${MOD_NAME} is updated to ${VERSION}."},
        {"mod.done.repair", L"${MOD_NAME} is repaired."},
        {"mod.done.uninstall", L"${MOD_NAME} is removed and the game's files are put back."},
        {"mod.failed", L"${MOD_NAME}: ${ERROR}"},
        {"mod.rollbackIncomplete", L"Not everything could be undone; details are in the log."},
        {"mod.uninstall.title", L"Remove ${MOD_NAME}?"},
        {"mod.uninstall.body", L"The files it added are deleted and the game files it replaced are put back. ${GAME_NAME} itself isn't touched."},
        {"mod.uninstall.yes", L"Uninstall"},
        {"mod.uninstall.no", L"Cancel"},
        {"mod.joinNeedsMod", L"Room ${CODE} is copied. Install ${MOD_NAME} first, then start ${GAME_NAME} and paste it in."},
        {"mod.joinCopied", L"Room ${CODE} is copied. In the game, open the multiplayer panel and paste it."},
        {"mod.notPublished", L"${MOD_NAME} isn't published yet, so there is nothing to install."},
        {"mod.phase.downloading", L"Downloading ${MOD_NAME}"},
        {"phase.preparing", L"Preparing\u2026"},
        {"phase.checking", L"Checking files"},
        {"phase.copying", L"Copying files"},
        {"phase.removing", L"Removing files"},
        {"phase.shortcuts", L"Removing shortcuts"},
        {"phase.finishing", L"Finishing up"},
        {"phase.rollback", L"Undoing changes\u2026"},
        {"update.player", L"${PLAYER} is installed."},
        {"update.nextTime", L"Multiplayer Everything Launcher ${VERSION} is installed and starts next time."},
        {"settings.autoUpdate", L"Update the launcher automatically"},
        {"discord.room", L"Room ${CODE}"},
        {"search.cue", L"Search games  (Ctrl+F)"},
        {"status.ready", L"Ready"},
        {"status.noPlayer", L"No Flash player found"},
        {"status.noSwf", L"Game file missing"},
        {"status.noRuntime", L"${RUNTIME} not found"},
        {"button.getRuntime", L"Get ${RUNTIME}"},
        {"web.starting", L"Starting ${GAME}\u2026"},
        {"web.serverRunning", L"Co-op server running"},
        {"web.serverFailed", L"The ${GAME} server stopped: ${ERROR}"},
        {"web.serverTimeout", L"The ${GAME} server didn't start within 30 seconds. Details are in the log."},
        {"web.joinNeedsLink", L"Copy the link your friend sent you, then press ${ACTION} again."},
        {"web.joinOpening", L"Opening ${LINK}"},
        {"web.links", L"Friends join at:"},
        {"web.waitingLink", L"Waiting for the internet link\u2026"},
        {"status.playing", L"Playing"},
        {"badge.online", L"Online"},
        {"slot.label", L"P${N}"},
        {"slot.open", L"Open"},
        {"slot.host", L"${NAME} (host)"},
        {"detail.playerAuto", L"Automatic (${PLAYER})"},
        {"detail.playerAutoNone", L"Automatic"},
        {"detail.player", L"Player"},
        {"setting.needs", L"${LABEL} (needs Ruffle Together ${VERSION} or newer)"},
        {"coopSave.label", L"Co-op save"},
        {"coopSave.continue", L"Continue co-op"},
        {"coopSave.solo", L"Start from my solo save"},
        {"coopSave.new", L"New game"},
        {"coopSave.replaces", L"This replaces your co-op progress for ${GAME} the first time the game saves."},
        {"coopSave.progress", L"Co-op progress saved."},
        {"notice.needGame", L"The game is not installed yet. Download or locate its files first."},
        {"coopSave.needs", L"Needs Ruffle Together ${VERSION} or newer."},
        {"detail.players", L"Players"},
        {"detail.controls", L"Controls"},
        {"detail.never", L"Not played yet"},
        {"detail.stats", L"Played ${COUNT} times for ${TIME} in total, last ${WHEN}."},
        {"detail.statsOnce", L"Played once for ${TIME}, ${WHEN}."},
        {"detail.empty", L"Pick a game on the left."},
        {"button.favorite", L"\u2606  Favorite"},
        {"button.unfavorite", L"\u2605  Favorite"},
        {"button.locate", L"Locate game file\u2026"},
        {"button.locateFor", L"Locate file for \u201C${ACTION}\u201D\u2026"},
        {"button.folder", L"Folder"},
        {"button.getPlayer", L"Get ${PLAYER}"},
        {"button.installPlayer", L"Install ${PLAYER}"},
        {"button.downloadGame", L"Download game files (${SIZE})"},
        {"button.updateGame", L"Update game files (${SIZE})"},
        {"status.update", L"Update available"},
        {"status.comingSoon", L"Coming soon"},
        {"status.notOnServer", L"Not on the server yet"},
        {"rooms.notOnServer", L"This game isn't on the Multiplayer Everything server yet, so there are no rooms to show. It is added when its online mod is published."},
        {"rooms.count", L"${N} rooms open"},
        {"rooms.one", L"1 room open"},
        {"rooms.title", L"Open rooms"},
        {"rooms.none", L"No open rooms right now. Host one and friends will see it here."},
        {"rooms.row", L"${CODE}  ${NAME}"},
        {"rooms.players", L"${N}/${MAX}"},
        {"rooms.join", L"Join"},
        {"rooms.joinName", L"Join room ${CODE}"},
        {"rooms.copied", L"Room ${CODE} copied. In the game press F9, choose ONLINE, paste the code and press JOIN."},
        {"rooms.full", L"Room ${CODE} is full."},
        {"rooms.playing", L"playing"},
        {"rooms.locked", L"password"},
        {"lobby.title", L"Play ${GAME} online"},
        {"lobby.subtitle", L"Host a room or join one. When everyone is ready the host starts, and the launcher opens the game for each player."},
        {"lobby.direct", L"Direct IP (Radmin VPN, ZeroTier, LAN) instead of the server"},
        {"lobby.hostHead", L"Host a room"},
        {"lobby.roomName", L"Room name"},
        {"lobby.players", L"Players"},
        {"lobby.public", L"List it in Open rooms"},
        {"lobby.password", L"Password (optional)"},
        {"lobby.port", L"Port"},
        {"lobby.upnp", L"Open the port on my router (UPnP)"},
        {"lobby.host", L"Host room"},
        {"lobby.joinHead", L"Join a room"},
        {"lobby.code", L"Room code"},
        {"lobby.address", L"Host's address"},
        {"lobby.joinPassword", L"Password, if it has one"},
        {"lobby.join", L"Join"},
        {"lobby.listHead", L"Open rooms"},
        {"lobby.listEmpty", L"No open rooms for this game right now. Host one!"},
        {"lobby.refresh", L"Refresh"},
        {"lobby.colCode", L"Code"},
        {"lobby.colRoom", L"Room"},
        {"lobby.colPlayers", L"Players"},
        {"lobby.colPing", L"Ping"},
        {"lobby.colRegion", L"Region"},
        {"lobby.colState", L"Status"},
        {"lobby.stateLobby", L"In lobby"},
        {"lobby.statePlaying", L"Playing"},
        {"lobby.locked", L"Password"},
        {"lobby.room", L"Room ${CODE}"},
        {"lobby.directRoom", L"Direct IP"},
        {"lobby.copyCode", L"Copy code"},
        {"lobby.copied", L"Room code ${CODE} copied."},
        {"lobby.colSlot", L"Slot"},
        {"lobby.colName", L"Player"},
        {"lobby.colReady", L"Ready"},
        {"lobby.colStatus", L"Status"},
        {"lobby.you", L"${NAME} (you)"},
        {"lobby.hostTag", L"Host"},
        {"lobby.isReady", L"Ready"},
        {"lobby.notReady", L"Not ready"},
        {"lobby.loaded", L"Game loaded"},
        {"lobby.loading", L"Loading"},
        {"lobby.away", L"Reconnecting"},
        {"lobby.ready", L"I'm ready"},
        {"lobby.unready", L"Not ready"},
        {"lobby.start", L"Start game"},
        {"lobby.kick", L"Kick"},
        {"lobby.ban", L"Ban"},
        {"lobby.leave", L"Leave room"},
        {"lobby.diag", L"Copy diagnostics"},
        {"lobby.send", L"Send"},
        {"lobby.chatHead", L"Chat"},
        {"lobby.phaseConnecting", L"Connecting..."},
        {"lobby.phaseLobby", L"Waiting for the host to start. Press I'm ready when you are."},
        {"lobby.phaseLobbyHost", L"Start the game when everyone is ready."},
        {"lobby.waitFriends", L"Waiting for friends to join. Share the room code, or start alone to try the game."},
        {"lobby.waitingFor", L"Waiting for: ${NAMES}."},
        {"lobby.phaseStarting", L"Starting: waiting for every player's game to load."},
        {"lobby.phaseInGame", L"Playing. Close the game to come back to the lobby."},
        {"lobby.reconnecting", L"Connection lost. Reconnecting..."},
        {"lobby.r.hostLeft", L"The host closed the room."},
        {"lobby.r.kicked", L"The host removed you from the room."},
        {"lobby.r.banned", L"The host banned you from this room."},
        {"lobby.r.full", L"That room is full."},
        {"lobby.r.ingame", L"That room is already playing. Try again when the round ends."},
        {"lobby.r.version", L"You have a different build of the game than the host (${HOST}). Update the game files, then join again."},
        {"lobby.r.password", L"Wrong password."},
        {"lobby.r.tries", L"Too many wrong passwords. Wait a minute and try again."},
        {"lobby.r.noRoom", L"No room with that code. Check the code, or the host closed it."},
        {"lobby.r.unreachable", L"Can't connect. ${DETAIL}"},
        {"lobby.r.lost", L"Lost the connection and couldn't get back in."},
        {"lobby.r.server", L"The server said: ${DETAIL}"},
        {"lobby.versionUpdate", L"A newer build of the game files is available. Update them now?"},
        {"lobby.addrHead", L"Friends join with one of these addresses:"},
        {"lobby.addrVpn", L"${ADDR}:${PORT}  (${VPN})"},
        {"lobby.addrLan", L"${ADDR}:${PORT}  (home network)"},
        {"lobby.addrOther", L"${ADDR}:${PORT}"},
        {"lobby.portTesting", L"Checking whether friends can reach port ${PORT}..."},
        {"lobby.portOpen", L"Port ${PORT} is reachable from the internet."},
        {"lobby.portClosed", L"Port ${PORT} isn't reachable from the internet. Forward it on your router, turn on UPnP, or play over a VPN such as Radmin."},
        {"lobby.portUnknown", L"Couldn't test port ${PORT} from the internet. Friends on your VPN or network can still join."},
        {"lobby.firewallBlocked", L"Windows Firewall may block friends. Allow the launcher when Windows asks, or add a rule for port ${PORT}."},
        {"lobby.upnpOk", L"Port ${PORT} is open on your router (UPnP)."},
        {"lobby.upnpFailed", L"UPnP didn't work: ${ERROR}"},
        {"lobby.needName", L"Choose your player name first."},
        {"lobby.badCode", L"A room code has 5 letters."},
        {"lobby.badAddress", L"Type the host's address, like 26.1.2.3:7790."},
        {"lobby.diagCopied", L"Diagnostics copied. Paste them into your bug report."},
        {"lobby.noGameFile", L"The game file isn't here yet. Download it first."},
        {"lobby.everyoneReady", L"Not everyone is ready yet."},
        {"lobby.pickPlayer", L"Select a player in the list first."},
        {"name.title", L"Welcome to Multiplayer Everything"},
        {"name.body", L"Choose your player name. Other players see it in rooms and in the game. You can change it later in Settings."},
        {"name.label", L"Player name"},
        {"name.ok", L"Continue"},
        {"button.hostGame", L"Host a game"},
        {"button.joinGame", L"Join a game"},
        {"host.title", L"Host ${GAME}"},
        {"host.players", L"Players"},
        {"host.playersN", L"${N} players"},
        {"host.connection", L"Connection"},
        {"host.viaRoom", L"Room code (no IP needed)"},
        {"host.viaIp", L"Direct IP (Radmin, Tailscale or LAN)"},
        {"host.start", L"Start"},
        {"host.cancel", L"Cancel"},
        {"join.body", L"Type or paste your friend's room code, or their address and port (for example 192.168.1.20:7790). You can also pick a room in the Rooms tab."},
        {"join.label", L"Room code or address"},
        {"join.ok", L"Join"},
        {"address.body", L"Type or paste your friend's address and port, for example 192.168.1.20:7790. Radmin, Tailscale and LAN addresses work."},
        {"address.label", L"Host address"},
        {"address.ok", L"Join"},
        {"nav.soon", L"Coming soon"},
        {"button.guide", L"Guide"},
        {"tab.library", L"Library"},
        {"page.madeBy", L"Made by"},
        {"page.players", L"Players"},
        {"page.source", L"Source"},
        {"page.version", L"Version"},
        {"page.pinned", L"${VERSION}, pinned and checked by SHA-256"},
        {"page.status", L"Status"},
        {"tab.rooms", L"Rooms"},
        {"sort.ours", L"Our order"},
        {"sort.recent", L"Recently played"},
        {"sort.name", L"Name"},
        {"sort.players", L"Most players"},
        {"button.compactCards", L"Compact"},
        {"button.normalCards", L"Larger"},
        {"hero.continue", L"Continue playing"},
        {"hero.played", L"Played ${WHEN}"},
        {"hero.open", L"Open"},
        {"guide.back", L"Back to games"},
        {"guide.title", L"Guide: ${NAME}"},
        {"guide.h.about", L"About"},
        {"guide.h.needs", L"What you need"},
        {"guide.h.steps", L"How to play"},
        {"guide.h.keys", L"Keys"},
        {"guide.h.tips", L"Good to know"},
        {"guide.h.site", L"From the website"},
        {"guide.github", L"Open its GitHub page"},
        {"guide.soon", L"This one isn't available yet. When it is, it appears in the main list and you can install or play it from here."},
        {"guide.need.game", L"Your own copy of ${GAME}, on this PC."},
        {"guide.need.version", L"Everyone you play with needs the same version of the mod. The launcher keeps yours up to date."},
        {"guide.need.player", L"Ruffle Together. The launcher installs it for you the first time."},
        {"guide.need.name", L"A player name: the launcher asks for it on the first start, and Settings changes it."},
        {"guide.mod.1", L"Press Install. The launcher finds your copy of ${GAME} and puts the mod in it."},
        {"guide.mod.launch", L"Press Launch. The game starts the way the mod needs it to."},
        {"guide.mod.friends", L"Friends do the same steps on their PC, then join your game from the mod's menu."},
        {"guide.mod.tip", L"Uninstall puts your game back exactly as it was."},
        {"guide.lobby.1", L"Press Play online. The lobby opens."},
        {"guide.lobby.2", L"Host a room, or pick an open room from the list (or type its code) and press Join."},
        {"guide.lobby.3", L"Everyone presses I'm ready. The host presses Start game, and the game opens for everyone."},
        {"guide.lobby.tip", L"Rooms use the Multiplayer Everything server, so nobody needs an IP address or port forwarding. Direct IP is there for Radmin, ZeroTier and LAN."},
        {"guide.online.1", L"Press Play online. Quick play joins the busiest open room, or starts a room if none is open."},
        {"guide.online.2", L"In the game press F9, set MODE to ONLINE, and press HOST (you get a room code) or type a friend's code and press JOIN."},
        {"guide.online.3", L"Friends can also find your room in the Rooms list and press Join."},
        {"guide.online.tip", L"Rooms use the Multiplayer Everything server, so nobody needs an IP address or port forwarding."},
        {"guide.play.1", L"Press the Play button."},
        {"guide.site.stage", L"Stage"},
        {"guide.site.quality", L"Quality"},
        {"guide.site.version", L"Newest version"},
        {"guide.site.updated", L"Last updated"},
        {"guide.site.connect", L"Connects through"},
        {"guide.site.madeBy", L"Made by"},
        {"guide.site.origin", L"Kind of mod"},
        {"guide.site.communityUnverified", L"Community mod: made by someone else and not reviewed by us. Report problems to its author."},
        {"guide.site.communityReviewed", L"Community mod: made by someone else; we read its code. Report problems to its author."},
        {"guide.site.players", L"Players"},
        {"guide.site.sync", L"What stays in sync"},
        {"site.planned", L"We are working on this one. It gets a grade once friends have played it."},
        {"button.about", L"About"},
        {"about.title", L"${NAME}"},
        {"about.github", L"Open its GitHub page"},
        {"about.close", L"Close"},
        {"about.stage", L"Stage"},
        {"about.quality", L"Quality"},
        {"about.version", L"Newest version"},
        {"about.updated", L"Last updated"},
        {"about.connect", L"Connects through"},
        {"about.madeBy", L"Made by"},
        {"about.players", L"Players"},
        {"about.sync", L"What stays in sync"},
        {"empty.fav.title", L"No favorites yet"},
        {"empty.fav.body", L"Open a game and press Favorite to keep it here."},
        {"empty.ready.title", L"Nothing is ready to play yet"},
        {"empty.ready.body", L"Install a mod, or download a game's files from its page."},
        {"empty.soon.title", L"Nothing is coming soon"},
        {"empty.soon.body", L"Games we are working on show up here."},
        {"empty.search.title", L"No game matches"},
        {"empty.search.body", L"Try other words, or clear the search."},
        {"site.status.available", L"Available"},
        {"site.status.testing", L"In testing"},
        {"site.status.not-on-server", L"Not on the server yet"},
        {"site.status.coming-soon", L"Coming soon"},
        {"site.quality", L"Quality ${GRADE}"},
        {"site.quality.A", L"Complete: plays start to finish with friends."},
        {"site.quality.B", L"Playable: works online with friends today."},
        {"site.quality.C", L"Rough: plays online, with known gaps or bugs."},
        {"site.quality.D", L"Experimental: an early build, expect missing parts."},
        {"site.quality.X", L"None yet: not playable online yet."},
        {"site.unrated", L"Not rated yet. It gets a grade once friends have played it."},
        {"site.version", L"Version ${VERSION}"},
        {"site.testBuild", L"Test build ${VERSION}"},
        {"site.updated", L"updated ${DATE}"},
        {"site.synced", L"In sync"},
        {"site.synced.see", L"Players"},
        {"site.synced.story", L"Story"},
        {"site.synced.enemies", L"Enemies"},
        {"site.synced.world", L"World"},
        {"site.synced.items", L"Items"},
        {"site.synced.synced", L"synced"},
        {"site.synced.partly", L"partly synced"},
        {"site.synced.unknown", L"not known yet"},
        {"button.settings", L"Settings"},
        {"settings.title", L"Settings"},
        {"settings.playing", L"Playing"},
        {"settings.minimize", L"Minimize the launcher while a game runs"},
        {"settings.player", L"Default player"},
        {"settings.online", L"Online"},
        {"settings.onlineToggle", L"Show open rooms and check for game updates"},
        {"settings.onlineNote", L"Uses the internet: the room list from the central server, the game list from GitHub."},
        {"settings.folders", L"Folders"},
        {"settings.gamesDir", L"Game files"},
        {"settings.change", L"Change\u2026"},
        {"settings.open", L"Open"},
        {"settings.reset", L"Use default"},
        {"settings.log", L"Open log folder"},
        {"settings.about", L"Multiplayer Everything Launcher ${VERSION}"},
        {"settings.github", L"Project page"},
        {"settings.close", L"Close"},
        {"settings.pickFolder", L"Choose the folder for game files"},
        {"download.title", L"Download ${GAME}?"},
        {"download.body", L"The launcher downloads ${COUNT} file(s), ${SIZE} in total, from ${SOURCE}, checks each one against its published checksum, and saves them in ${DIR}."},
        {"download.yes", L"Download"},
        {"download.downloading", L"Downloading ${GAME}: ${DONE} of ${TOTAL}"},
        {"download.done", L"${GAME} is downloaded and ready."},
        {"download.failed", L"Couldn't download ${GAME}: ${ERROR}"},
        {"install.title", L"Install ${PLAYER}?"},
        {"install.firstRun", L"No Flash player was found on this PC, so games can't start yet."},
        {"install.body", L"The launcher downloads the latest ${PLAYER} release from ${SOURCE}, checks it, and puts it in ${DIR}. ${PLAYER} is free and open source."},
        {"install.yes", L"Install ${PLAYER}"},
        {"install.no", L"Not now"},
        {"install.finding", L"Looking up the latest ${PLAYER} release\u2026"},
        {"install.downloading", L"Downloading ${PLAYER}: ${DONE} of ${TOTAL}"},
        {"install.unpacking", L"Unpacking ${PLAYER}\u2026"},
        {"install.done", L"${PLAYER} is installed. Games can start now."},
        {"install.failed", L"Couldn't install ${PLAYER}: ${ERROR}"},
        {"button.stopHost", L"Stop hosting"},
        {"button.copyAddress", L"Copy address"},
        {"host.running", L"Hosting on port ${PORT}"},
        {"host.waiting", L"Waiting for friends\u2026"},
        {"host.connected", L"${COUNT} connected"},
        {"host.you", L"(host)"},
        {"host.addresses", L"Friends join with:"},
        {"host.noAddress", L"No network found. Friends on this PC use 127.0.0.1:${PORT}"},
        {"host.portInUse", L"Port ${PORT} is already in use; using the relay that is already running."},
        {"host.failed", L"Could not start the relay on port ${PORT}: ${ERROR}"},
        {"notice.launched", L"Started ${GAME} in ${PLAYER}."},
        {"notice.quitFast", L"${PLAYER} closed right away (exit code ${CODE}). Details are in the log."},
        {"notice.launchFailed", L"Could not start ${PLAYER}: ${ERROR}"},
        {"notice.notReady", L"${GAME} can't start: ${REASON}"},
        {"notice.needCode", L"${ACTION} needs a room code: type it in Join by code or pick a room in the Rooms list."},
        {"notice.needAddress", L"${ACTION}: copy your friend's address and port (for example 192.168.1.20:7790) first, then press it again."},
        {"notice.needServer", L"${ACTION} needs the central server's address, which isn't set."},
        {"notice.copied", L"Copied ${ADDRESS} to the clipboard."},
        {"notice.located", L"${GAME} will use ${FILE}."},
        {"notice.relayStopped", L"Stopped hosting."},
        {"bar.found", L"${PLAYER} \u2713"},
        {"bar.missing", L"${PLAYER} \u2717"},
        {"bar.flashpoint", L"Flashpoint: ${PATH}"},
        {"bar.playing", L"Playing ${GAME} for ${TIME}"},
        {"empty.title", L"No games here"},
        {"empty.body", L"Try another category or clear the search."},
        {"time.never", L"never"},
        {"time.justNow", L"just now"},
        {"time.minutesAgo", L"${N} min ago"},
        {"time.hoursAgo", L"${N} h ago"},
        {"time.daysAgo", L"${N} days ago"},
        {"dialog.locateTitle", L"Find the game file for ${GAME}"},
        {"dialog.swfFilter", L"Flash games (*.swf)"},
    };
    return texts;
}

std::wstring Config::t(const std::string& key,
                       std::initializer_list<std::pair<std::wstring_view, std::wstring_view>> vars) const {
    auto it = text.find(key);
    std::wstring s = it == text.end() ? widen(key) : it->second;
    for (const auto& [name, value] : vars) s = replaceAll(s, L"${" + std::wstring(name) + L"}", value);
    return s;
}

std::wstring Config::modsInfoBase() const {
    if (!downloads.modsInfo.empty()) return downloads.modsInfo;
    size_t slash = rooms.servers.rfind(L'/');
    return slash == std::wstring::npos ? std::wstring() : rooms.servers.substr(0, slash + 1);
}

const GameSpec* Config::findGame(std::string_view id) const {
    for (const auto& g : games)
        if (g.id == id) return &g;
    return nullptr;
}

const PlayerSpec* Config::findPlayer(std::string_view id) const {
    for (const auto& p : players)
        if (p.id == id) return &p;
    return nullptr;
}

const Category* Config::findCategory(std::string_view id) const {
    for (const auto& c : categories)
        if (c.id == id) return &c;
    return nullptr;
}

bool LoadResult::ok() const {
    for (const auto& i : issues)
        if (i.severity == ConfigIssue::Severity::Error) return false;
    return true;
}

namespace {

using json::Type;
using json::Value;

const wchar_t* typeName(Type t) {
    switch (t) {
        case Type::Null: return L"null";
        case Type::Bool: return L"true/false";
        case Type::Number: return L"a number";
        case Type::String: return L"a string";
        case Type::Array: return L"a list";
        case Type::Object: return L"an object";
    }
    return L"?";
}

// Reads one JSON object into struct fields and reports type errors and
// unknown keys (usually typos) with their path.
class Section {
public:
    Section(const Value* v, std::wstring path, std::vector<ConfigIssue>& issues)
        : v_(v && v->isObject() ? v : nullptr), path_(std::move(path)), issues_(issues) {
        if (v && !v->isObject() && !v->isNull()) error(L"", L"must be an object");
    }
    ~Section() { reportUnknown(); }

    const Value* get(const char* key, Type type) {
        if (!v_) return nullptr;
        seen_.insert(key);
        const Value* f = v_->find(key);
        if (!f || f->isNull()) return nullptr;
        if (f->type() != type) {
            error(widen(key), std::wstring(L"must be ") + typeName(type) + L", found " + typeName(f->type()));
            return nullptr;
        }
        return f;
    }
    void str(const char* key, std::wstring& out) {
        if (auto f = get(key, Type::String)) out = widen(f->str());
    }
    void str(const char* key, std::string& out) {
        if (auto f = get(key, Type::String)) out = f->str();
    }
    void boolean(const char* key, bool& out) {
        if (auto f = get(key, Type::Bool)) out = f->boolean();
    }
    void integer(const char* key, int& out, int lo, int hi) {
        if (auto f = get(key, Type::Number)) {
            double d = f->number();
            if (d < lo || d > hi) error(widen(key), L"must be between " + std::to_wstring(lo) + L" and " + std::to_wstring(hi));
            else out = static_cast<int>(d);
        }
    }
    void real(const char* key, float& out, float lo, float hi) {
        if (auto f = get(key, Type::Number)) {
            double d = f->number();
            if (d < lo || d > hi) error(widen(key), L"is out of range");
            else out = static_cast<float>(d);
        }
    }
    void color(const char* key, Color& out) {
        std::wstring text;
        str(key, text);
        if (!text.empty() && !parseColor(text, out)) error(widen(key), L"must be a color like \"#4F8BFF\"");
    }
    // Accepts a single string as a one-item list (handy for "swf": "x.swf").
    void strList(const char* key, std::vector<std::wstring>& out, bool* present = nullptr) {
        if (!v_) return;
        seen_.insert(key);
        const Value* f = v_->find(key);
        if (!f || f->isNull()) return;
        if (present) *present = true;
        out.clear();
        if (f->isString()) {
            out.push_back(widen(f->str()));
            return;
        }
        if (!f->isArray()) {
            error(widen(key), L"must be a string or a list of strings");
            return;
        }
        for (size_t i = 0; i < f->size(); ++i) {
            if (f->at(i).isString()) out.push_back(widen(f->at(i).str()));
            else error(widen(key), L"must be a list of strings");
        }
    }
    void idList(const char* key, std::vector<std::string>& out) {
        std::vector<std::wstring> w;
        strList(key, w);
        for (const auto& s : w) out.push_back(narrow(s));
    }
    std::wstring child(const std::string& key) const { return path_.empty() ? widen(key) : path_ + L"." + widen(key); }
    void error(const std::wstring& key, const std::wstring& msg) {
        issues_.push_back({ConfigIssue::Severity::Error, key.empty() ? path_ : child(narrow(key)), msg});
    }

private:
    void reportUnknown() {
        if (!v_) return;
        for (size_t i = 0; i < v_->size(); ++i) {
            const std::string& k = v_->keyAt(i);
            if (k.empty() || k[0] == '_' || k[0] == '$') continue;  // "_comment" keys are allowed
            if (!seen_.count(k))
                issues_.push_back({ConfigIssue::Severity::Warning, child(k), L"unknown setting (typo?)"});
        }
    }
    const Value* v_;
    std::wstring path_;
    std::vector<ConfigIssue>& issues_;
    std::set<std::string> seen_;
};

// "icon": a Segoe Fluent Icons code point written as hex ("E774" is the globe).
void readIcon(Section& s, std::wstring& out) {
    std::wstring icon;
    s.str("icon", icon);
    if (icon.empty()) return;
    wchar_t* end = nullptr;
    unsigned long cp = wcstoul(icon.c_str(), &end, 16);
    if (*end || cp < 0xE000 || cp > 0xF8FF)
        s.error(L"icon", L"must be a Segoe Fluent Icons code like \"E774\"");
    else
        out = std::wstring(1, static_cast<wchar_t>(cp));
}

std::wstring indexPath(const wchar_t* list, size_t i) { return std::wstring(list) + L"[" + std::to_wstring(i) + L"]"; }

}  // namespace

std::vector<Setting> readSettings(const Value& list, const std::wstring& where, std::vector<ConfigIssue>& issues) {
    std::vector<Setting> out;
    for (size_t k = 0; k < list.size(); ++k) {
        Section s(&list.at(k), where + L"[" + std::to_wstring(k) + L"]", issues);
        Setting st;
        s.str("id", st.id);
        s.str("type", st.type);
        s.str("scope", st.scope);
        s.str("label", st.label);
        s.str("since", st.since);
        s.str("default", st.def);
        s.strList("args", st.args);
        if (auto choices = s.get("choices", Type::Array))
            for (size_t c = 0; c < choices->size(); ++c) {
                Section cs(&choices->at(c), s.child("choices") + L"[" + std::to_wstring(c) + L"]", issues);
                SettingChoice ch;
                cs.str("id", ch.id);
                cs.str("label", ch.label);
                cs.strList("args", ch.args);
                if (ch.id.empty() || ch.label.empty()) cs.error(L"id", L"give \"id\" and \"label\"");
                else st.choices.push_back(std::move(ch));
            }
        if (st.type != "checkbox" && st.type != "choice") s.error(L"type", L"must be \"checkbox\" or \"choice\"");
        else if (st.scope != "host" && st.scope != "join" && st.scope != "all") s.error(L"scope", L"must be \"host\", \"join\" or \"all\"");
        else if (st.id.empty() || st.label.empty()) s.error(L"id", L"give \"id\" and \"label\"");
        else if (st.type == "checkbox" && st.args.empty()) s.error(L"args", L"a checkbox needs \"args\"");
        else if (st.type == "choice" && st.choices.size() < 2) s.error(L"choices", L"a choice needs at least two \"choices\"");
        else if (out.size() >= kMaxSettings) s.error(L"id", L"at most four settings");
        else out.push_back(std::move(st));
    }
    return out;
}

namespace {

ActionSpec readAction(const Value& v, const std::wstring& where, std::vector<ConfigIssue>& issues) {
    ActionSpec a;
    Section s(&v, where, issues);
    s.str("id", a.id);
    s.str("label", a.label);
    s.str("description", a.description);
    s.str("hint", a.hint);
    s.strList("swf", a.swf);
    s.strList("file", a.swf);  // the same list, named for HTML5 games
    s.strList("page", a.page);
    s.boolean("joinLink", a.joinLink);
    if (auto sv = s.get("server", Type::Object)) {
        Section ss(sv, s.child("server"), issues);
        ss.str("name", a.server.name);
        ss.str("website", a.server.website);
        ss.strList("exe", a.server.exe);
        ss.strList("args", a.server.args);
        ss.str("cwd", a.server.cwd);
        ss.str("url", a.server.url);
        ss.str("share", a.server.share);
        ss.str("lan", a.server.lan);
    }
    s.strList("args", a.args);
    s.strList("joinArgs", a.joinArgs);
    s.str("link", a.link);
    s.idList("players", a.players);
    if (auto o = s.get("overrides", Type::Object)) {
        for (size_t i = 0; i < o->size(); ++i) {
            PlayerOverride po;
            Section os(&o->at(i), s.child("overrides") + L"." + widen(o->keyAt(i)), issues);
            os.strList("swf", po.swf, &po.hasSwf);
            os.strList("args", po.args, &po.hasArgs);
            a.overrides[o->keyAt(i)] = std::move(po);
        }
    }
    if (auto fv = s.get("flashvars", Type::Object)) {
        for (size_t i = 0; i < fv->size(); ++i) {
            if (!fv->at(i).isString()) {
                s.error(L"flashvars", L"values must be strings");
                continue;
            }
            a.flashvars.emplace_back(widen(fv->keyAt(i)), widen(fv->at(i).str()));
        }
    }
    s.boolean("more", a.more);
    s.boolean("offline", a.offline);
    readIcon(s, a.icon);
    std::wstring edition;
    s.str("edition", edition);
    if (!edition.empty() && edition != L"original" && edition != L"modded")
        s.error(L"edition", L"must be \"original\" or \"modded\"");
    a.original = edition == L"original";
    if (auto r = s.get("relay", Type::Object)) {
        Section rs(r, s.child("relay"), issues);
        a.relay.enabled = true;
        rs.boolean("enabled", a.relay.enabled);
        rs.integer("port", a.relay.port, 1, 65535);
    }
    if (a.label.empty()) a.label = widen(a.id);
    return a;
}

// A PC game mod: how to find the game, which files go where, how to start it.
mod::Spec readMod(const Value& v, const GameSpec& game, const std::wstring& where, std::vector<ConfigIssue>& issues) {
    mod::Spec m;
    Section s(&v, where, issues);
    m.mod.id = widen(game.id);
    m.mod.name = game.name;
    m.mod.description = game.description;
    s.str("id", m.mod.id);  // keep a stand-alone installer's id to adopt its installs
    s.str("publisher", m.mod.publisher);
    s.str("website", m.mod.website);
    s.str("supportUrl", m.mod.supportUrl);
    if (auto g = s.get("game", Type::Object)) {
        Section gs(g, s.child("game"), issues);
        gs.str("name", m.game.name);
        gs.str("executable", m.game.executable);
        gs.str("steamAppId", m.game.steamAppId);
        gs.strList("validationFiles", m.game.validationFiles);
        gs.strList("processNames", m.game.processNames);
        gs.boolean("allowUnvalidated", m.game.allowUnvalidated);
        if (auto list = gs.get("detection", Type::Array)) {
            for (size_t i = 0; i < list->size(); ++i) {
                mod::DetectionRule r;
                Section rs(&list->at(i), gs.child("detection") + L"[" + std::to_wstring(i) + L"]", issues);
                rs.str("type", r.type);
                rs.str("appId", r.appId);
                rs.str("installDir", r.installDir);
                rs.str("appName", r.appName);
                rs.str("key", r.key);
                rs.str("value", r.value);
                rs.str("subPath", r.subPath);
                rs.str("path", r.path);
                if (r.type != L"steam" && r.type != L"epic" && r.type != L"registry" && r.type != L"path")
                    rs.error(L"type", L"must be steam, epic, registry or path");
                m.game.detection.push_back(std::move(r));
            }
        }
    }
    if (m.game.name.empty()) m.game.name = game.name;
    if (auto in = s.get("install", Type::Object)) {
        Section is(in, s.child("install"), issues);
        is.str("targetDir", m.install.targetDir);
        is.str("stateDir", m.install.stateDir);
        is.str("uninstallKey", m.install.uninstallKey);
        is.strList("allowedRoots", m.install.allowedRoots);
        is.strList("preserve", m.install.preserve);
        is.strList("uninstallCleanup", m.install.uninstallCleanup);
        is.str("uninstallCleanupWith", m.install.uninstallCleanupWith);
        if (auto rq = is.get("requires", Type::Array))
            for (size_t i = 0; i < rq->size(); ++i) {
                mod::Requirement r;
                Section rs(&rq->at(i), is.child("requires") + L"[" + std::to_wstring(i) + L"]", issues);
                rs.str("path", r.path);
                rs.str("message", r.message);
                m.install.requires_.push_back(std::move(r));
            }
        is.boolean("backupOverwritten", m.install.backupOverwritten);
        is.integer("lockedFileRetries", m.install.lockedFileRetries, 0, 30);
        is.integer("extraSpaceMB", m.install.extraSpaceMB, 0, 1 << 20);
    }
    if (auto list = s.get("components", Type::Array)) {
        for (size_t i = 0; i < list->size(); ++i) {
            mod::Component c;
            Section cs(&list->at(i), s.child("components") + L"[" + std::to_wstring(i) + L"]", issues);
            cs.str("id", c.id);
            cs.str("name", c.name);
            cs.str("description", c.description);
            cs.str("source", c.source);
            cs.str("target", c.target);
            cs.str("targetName", c.targetName);
            cs.strList("sourceGame", c.sourceGame);
            cs.strList("skipIfPresent", c.skipIfPresent);
            cs.boolean("required", c.required);
            cs.boolean("selected", c.selected);
            if (c.name.empty()) c.name = widen(c.id);
            m.components.push_back(std::move(c));
        }
    }
    if (auto l = s.get("launch", Type::Object)) {
        Section ls(l, s.child("launch"), issues);
        ls.str("executable", m.launch.executable);
        ls.str("arguments", m.launch.arguments);
        ls.str("workingDir", m.launch.workingDir);
        ls.str("uri", m.launch.uri);
        ls.str("hint", m.launch.hint);
    }
    if (auto p = s.get("package", Type::Object)) {
        Section ps(p, s.child("package"), issues);
        ps.str("modsInfo", m.package.modsInfo);
        ps.str("url", m.package.url);
        ps.str("sha256", m.package.sha256);
        ps.str("version", m.package.version);
        ps.str("github", m.package.github);
        ps.str("asset", m.package.asset);
        if (auto ex = ps.get("extras", Type::Array))
            for (size_t i = 0; i < ex->size(); ++i) {
                mod::ExtraPackage e;
                Section es(&ex->at(i), ps.child("extras") + L"[" + std::to_wstring(i) + L"]", issues);
                es.str("dir", e.dir);
                es.str("github", e.github);
                es.str("tag", e.tag);
                es.str("asset", e.asset);
                es.str("url", e.url);
                es.str("sha256", e.sha256);
                m.package.extras.push_back(std::move(e));
            }
    }
    return m;
}

// The buttons of a mod game; the window shows the ones that fit its state.
void addModActions(GameSpec& g) {
    // The top button is the first one shown: Update when there is one, else Launch.
    for (const char* op : {"update", "install", "launch", "repair", "uninstall"}) {
        ActionSpec a;
        a.id = op;
        a.modOp = op;
        a.label = widen(op);
        g.actions.push_back(std::move(a));
    }
}

}  // namespace

LoadResult loadConfig(std::string_view jsonText, const std::wstring& configDir) {
    LoadResult r;
    Config& c = r.config;
    auto& issues = r.issues;
    c.configDir = configDir;
    for (const auto& [k, v] : defaultTexts()) c.text[k] = v;

    std::string err;
    Value root = json::parse(jsonText, &err);
    if (!err.empty()) {
        issues.push_back({ConfigIssue::Severity::Error, L"launcher.json", widen(err)});
        return r;
    }
    if (!root.isObject()) {
        issues.push_back({ConfigIssue::Severity::Error, L"launcher.json", L"the top level must be an object"});
        return r;
    }

    Section top(&root, L"", issues);
    top.integer("schemaVersion", c.schemaVersion, 1, 1);
    {
        Section s(top.get("branding", Type::Object), L"branding", issues);
        Branding& b = c.branding;
        s.str("title", b.title);
        s.str("subtitle", b.subtitle);
        s.str("website", b.website);
        s.str("windowTitle", b.windowTitle);
        s.str("theme", b.theme);
        if (!iequals(b.theme, L"dark") && !iequals(b.theme, L"light")) s.error(L"theme", L"must be \"dark\" or \"light\"");
        b.colors = iequals(b.theme, L"light") ? lightPalette() : darkPalette();
        if (auto colors = s.get("colors", Type::Object)) {
            Section cs(colors, L"branding.colors", issues);
            for (auto& [name, field] : colorFields(b.colors)) cs.color(name, *field);
        }
        s.str("font", b.font);
        s.str("displayFont", b.displayFont);
        if (auto pc = s.get("playerColors", Type::Array)) {
            b.playerColors.clear();
            for (size_t i = 0; i < pc->size(); ++i) {
                Color col;
                if (!pc->at(i).isString() || !parseColor(widen(pc->at(i).str()), col))
                    s.error(L"playerColors", L"must be a list of colors like \"#E5484D\"");
                else
                    b.playerColors.push_back(col);
            }
            if (b.playerColors.empty()) s.error(L"playerColors", L"needs at least one color");
        }
        s.real("fontSize", b.fontSize, 7, 20);
        Section ws(s.get("window", Type::Object), L"branding.window", issues);
        ws.integer("width", b.window.width, 400, 8000);
        ws.integer("height", b.window.height, 300, 8000);
        ws.integer("minWidth", b.window.minWidth, 400, 8000);
        ws.integer("minHeight", b.window.minHeight, 300, 8000);
    }
    {
        Section s(top.get("paths", Type::Object), L"paths", issues);
        s.str("games", c.paths.games);
        s.str("players", c.paths.players);
        s.str("data", c.paths.data);
        s.strList("flashpoint", c.paths.flashpoint);
    }
    {
        Section s(top.get("behavior", Type::Object), L"behavior", issues);
        s.boolean("minimizeOnLaunch", c.behavior.minimizeOnLaunch);
        s.boolean("stopRelayWithGame", c.behavior.stopRelayWithGame);
        s.integer("quickExitSeconds", c.behavior.quickExitSeconds, 0, 60);
    }
    {
        Section rs(top.get("rooms", Type::Object), L"rooms", issues);
        rs.str("servers", c.rooms.servers);
        rs.str("server", c.rooms.server);
        rs.integer("refreshSeconds", c.rooms.refreshSeconds, 10, 3600);
        if (!c.rooms.servers.empty() && !startsWithI(c.rooms.servers, L"https://") &&
            !startsWithI(c.rooms.servers, L"http://127.0.0.1"))
            rs.error(L"servers", L"must be an https:// address");
    }
    {
        Section s(top.get("updates", Type::Object), L"updates", issues);
        s.str("github", c.updates.source.github);
        s.str("asset", c.updates.source.asset);
        s.str("api", c.updates.source.api);
        if (!c.updates.source.github.empty() && c.updates.source.github.find('/') == std::string::npos)
            s.error(L"github", L"must look like \"owner/repo\"");
        if (!c.updates.source.github.empty() && c.updates.source.asset.empty())
            s.error(L"asset", L"give the release file name, e.g. \"MultiplayerEverythingLauncher.exe\"");
    }
    {
        Section s(top.get("discord", Type::Object), L"discord", issues);
        s.str("clientId", c.discord.clientId);
        if (c.discord.clientId.find_first_not_of(L"0123456789") != std::wstring::npos)
            s.error(L"clientId", L"must be the application id, digits only");
    }
    {
        Section s(top.get("downloads", Type::Object), L"downloads", issues);
        s.str("catalog", c.downloads.catalog);
        s.str("modsInfo", c.downloads.modsInfo);
        s.str("site", c.downloads.site);
        if (!c.downloads.site.empty() && !startsWithI(c.downloads.site, L"https://") &&
            !startsWithI(c.downloads.site, L"http://127.0.0.1"))
            s.error(L"site", L"must be an https:// address");
        const std::wstring& u = c.downloads.catalog;
        if (!u.empty() && !startsWithI(u, L"https://") && !startsWithI(u, L"http://127.0.0.1"))
            s.error(L"catalog", L"must be an https:// address");
        const std::wstring& mi = c.downloads.modsInfo;
        if (!mi.empty() && !startsWithI(mi, L"https://") && !startsWithI(mi, L"http://127.0.0.1"))
            s.error(L"modsInfo", L"must be an https:// address");
    }
    if (auto list = top.get("players", Type::Array)) {
        for (size_t i = 0; i < list->size(); ++i) {
            PlayerSpec p;
            Section s(&list->at(i), indexPath(L"players", i), issues);
            s.str("id", p.id);
            s.str("kind", p.kind);
            if (p.kind != "flash" && p.kind != "browser") s.error(L"kind", L"must be \"flash\" or \"browser\"");
            s.str("name", p.name);
            s.str("website", p.website);
            s.strList("paths", p.paths);
            s.strList("args", p.args);
            s.str("flashvarArg", p.flashvarArg);
            s.str("coopSaveArg", p.coopSaveArg);
            s.str("coopSaveSince", p.coopSaveSince);
            if (auto inst = s.get("install", Type::Object)) {
                Section is(inst, s.child("install"), issues);
                is.str("github", p.install.github);
                is.str("asset", p.install.asset);
                is.str("url", p.install.url);
                is.str("sha256", p.install.sha256);
                is.str("dir", p.install.dir);
                is.str("exe", p.install.exe);
                is.str("api", p.install.api);
            }
            if (p.name.empty()) p.name = widen(p.id);
            c.players.push_back(std::move(p));
        }
    }
    if (auto list = top.get("categories", Type::Array)) {
        for (size_t i = 0; i < list->size(); ++i) {
            Category cat;
            Section s(&list->at(i), indexPath(L"categories", i), issues);
            s.str("id", cat.id);
            s.str("name", cat.name);
            s.str("group", cat.group);
            readIcon(s, cat.icon);
            if (cat.name.empty()) cat.name = widen(cat.id);
            c.categories.push_back(std::move(cat));
        }
    }
    if (auto list = top.get("games", Type::Array)) {
        for (size_t i = 0; i < list->size(); ++i) {
            GameSpec g;
            std::wstring where = indexPath(L"games", i);
            Section s(&list->at(i), where, issues);
            s.str("id", g.id);
            s.str("name", g.name);
            s.str("description", g.description);
            s.str("players", g.playerCount);
            s.integer("maxPlayers", g.maxPlayers, 0, 16);
            if (auto on = s.get("online", Type::Object)) {
                Section os(on, s.child("online"), issues);
                os.str("id", g.onlineId);
                os.str("join", g.joinAction);
                if (auto lv = os.get("lobby", Type::Object)) {
                    Section ls(lv, os.child("lobby"), issues);
                    g.lobby.enabled = true;
                    ls.boolean("enabled", g.lobby.enabled);
                    ls.str("version", g.lobby.version);
                    ls.integer("port", g.lobby.port, 1, 65535);
                }
            }
            if (!g.maxPlayers)
                for (size_t k = 0; k < g.playerCount.size(); ++k)  // "1-4 players" -> 4
                    if (iswdigit(g.playerCount[k]))
                        g.maxPlayers = std::max(g.maxPlayers, _wtoi(g.playerCount.c_str() + k));
            s.str("controls", g.controls);
            s.str("image", g.image);
            s.str("site", g.siteId);
            if (auto gv = s.get("guide", Type::Object)) {
                Section gs(gv, s.child("guide"), issues);
                gs.strList("needs", g.guide.needs);
                gs.strList("steps", g.guide.steps);
                gs.strList("tips", g.guide.tips);
                if (auto keys = gs.get("keys", Type::Array))
                    for (size_t k = 0; k < keys->size(); ++k) {
                        Section ks(&keys->at(k), gs.child("keys") + L"[" + std::to_wstring(k) + L"]", issues);
                        std::wstring key, does;
                        ks.str("key", key);
                        ks.str("does", does);
                        if (key.empty() || does.empty()) ks.error(L"key", L"give both \"key\" and \"does\"");
                        else g.guide.keys.emplace_back(key, does);
                    }
                if (auto links = gs.get("links", Type::Array))
                    for (size_t k = 0; k < links->size(); ++k) {
                        Section ls(&links->at(k), gs.child("links") + L"[" + std::to_wstring(k) + L"]", issues);
                        GuideLink link;
                        ls.str("label", link.label);
                        ls.str("url", link.url);
                        if (link.label.empty() || !startsWithI(link.url, L"https://"))
                            ls.error(L"url", L"give a \"label\" and an https:// \"url\"");
                        else if (g.guide.links.size() >= kGuideMaxLinks)
                            ls.error(L"url", L"at most four links");
                        else g.guide.links.push_back(std::move(link));
                    }
            }
            s.real("imageFocus", g.imageFocus, 0, 1);
            s.boolean("comingSoon", g.comingSoon);
            if (auto st = s.get("settings", Type::Array)) g.settings = readSettings(*st, s.child("settings"), issues);
            s.idList("categories", g.categories);
            s.color("color", g.color);
            s.color("colorEnd", g.colorEnd);
            if (auto actions = s.get("actions", Type::Array)) {
                for (size_t j = 0; j < actions->size(); ++j)
                    g.actions.push_back(readAction(actions->at(j), where + L"." + indexPath(L"actions", j), issues));
            }
            if (auto mv = s.get("mod", Type::Object)) {
                // The launcher makes the install / launch buttons; the only buttons a mod game may add are links
                // (the mod's own page), which come after them.
                std::vector<ActionSpec> links;
                for (size_t j = 0; j < g.actions.size(); ++j) {
                    if (g.actions[j].isLink()) links.push_back(std::move(g.actions[j]));
                    else
                        issues.push_back({ConfigIssue::Severity::Error, where + L"." + indexPath(L"actions", j),
                                          L"a mod game gets its buttons from the launcher; only a \"link\" action can be added"});
                }
                g.actions.clear();
                g.mod = std::make_shared<const mod::Spec>(readMod(*mv, g, s.child("mod"), issues));
                addModActions(g);
                for (auto& l : links) g.actions.push_back(std::move(l));
            }
            if (g.name.empty()) g.name = widen(g.id);
            c.games.push_back(std::move(g));
        }
    }
    if (auto texts = top.get("text", Type::Object)) {
        for (size_t i = 0; i < texts->size(); ++i) {
            const std::string& k = texts->keyAt(i);
            if (!k.empty() && k[0] == '_') continue;
            if (!texts->at(i).isString()) {
                issues.push_back({ConfigIssue::Severity::Error, L"text." + widen(k), L"must be a string"});
                continue;
            }
            if (!c.text.count(k))
                issues.push_back({ConfigIssue::Severity::Warning, L"text." + widen(k), L"unknown text key (typo?)"});
            c.text[k] = widen(texts->at(i).str());
        }
    }
    if (r.ok()) validateConfig(c, issues);
    return r;
}

LoadResult loadConfigFile(const std::wstring& path) {
    std::string text;
    if (!fs::readText(path, text)) {
        LoadResult r;
        r.issues.push_back({ConfigIssue::Severity::Error, path, L"cannot read the file"});
        return r;
    }
    return loadConfig(text, fs::parent(path));
}

namespace {

void checkVars(const std::wstring& text, const std::wstring& where, std::vector<ConfigIssue>& issues) {
    const auto& known = knownVariableNames();
    size_t i = 0;
    while ((i = text.find(L"${", i)) != std::wstring::npos) {
        size_t end = text.find(L'}', i);
        if (end == std::wstring::npos) break;
        std::wstring name = text.substr(i + 2, end - i - 2);
        bool ok = startsWithI(name, L"env:") || startsWithI(name, L"folder:");
        for (const auto& k : known) ok = ok || iequals(k, name);
        if (!ok) issues.push_back({ConfigIssue::Severity::Warning, where, L"unknown variable ${" + name + L"}"});
        i = end + 1;
    }
}

bool validId(const std::string& id) {
    if (id.empty()) return false;
    for (char ch : id)
        if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')) return false;
    return true;
}

}  // namespace

void validateConfig(const Config& cfg, std::vector<ConfigIssue>& issues) {
    auto err = [&](const std::wstring& where, const std::wstring& msg) {
        issues.push_back({ConfigIssue::Severity::Error, where, msg});
    };
    if (cfg.players.empty()) err(L"players", L"at least one player (Ruffle, Flash Player...) is required");
    if (cfg.games.empty()) err(L"games", L"the launcher needs at least one game");

    std::set<std::string> ids;
    for (size_t i = 0; i < cfg.players.size(); ++i) {
        const auto& p = cfg.players[i];
        std::wstring where = indexPath(L"players", i);
        if (!validId(p.id)) err(where + L".id", L"must be letters, digits, '-', '_' or '.'");
        else if (!ids.insert(p.id).second) err(where + L".id", L"duplicate player id \"" + widen(p.id) + L"\"");
        if (p.paths.empty()) err(where + L".paths", L"list at least one place to look for the player");
        for (const auto& s : p.paths) checkVars(s, where + L".paths", issues);
        for (const auto& s : p.args) checkVars(s, where + L".args", issues);
        const InstallSpec& in = p.install;
        if (in.enabled() || !in.dir.empty()) {
            if (in.dir.empty()) err(where + L".install.dir", L"say which folder the player is unpacked into");
            if (!in.github.empty() && in.asset.empty())
                err(where + L".install.asset", L"give the release file name, e.g. \"*windows-x86_64.zip\"");
            if (!in.github.empty() && in.github.find('/') == std::string::npos)
                err(where + L".install.github", L"must look like \"owner/repo\"");
            if (!in.url.empty() && !startsWithI(in.url, L"https://") && !startsWithI(in.url, L"http://127.0.0.1"))
                err(where + L".install.url", L"must be an https:// address");
            if (!in.enabled()) err(where + L".install", L"needs \"github\" or \"url\"");
            checkVars(in.dir, where + L".install.dir", issues);
        }
    }
    ids.clear();
    for (size_t i = 0; i < cfg.categories.size(); ++i) {
        std::wstring where = indexPath(L"categories", i);
        if (!validId(cfg.categories[i].id)) err(where + L".id", L"must be letters, digits, '-', '_' or '.'");
        else if (!ids.insert(cfg.categories[i].id).second) err(where + L".id", L"duplicate category id");
    }
    ids.clear();
    for (size_t i = 0; i < cfg.games.size(); ++i) {
        const auto& g = cfg.games[i];
        std::wstring where = indexPath(L"games", i);
        if (!validId(g.id)) err(where + L".id", L"must be letters, digits, '-', '_' or '.'");
        else if (!ids.insert(g.id).second) err(where + L".id", L"duplicate game id \"" + widen(g.id) + L"\"");
        for (const auto& c : g.categories)
            if (!cfg.findCategory(c)) err(where + L".categories", L"unknown category \"" + widen(c) + L"\"");
        if (g.lobby.enabled && g.onlineId.empty()) err(where + L".online.lobby", L"needs the game's online id");
        if (!g.joinAction.empty() &&
            std::none_of(g.actions.begin(), g.actions.end(), [&](const ActionSpec& a) { return a.id == g.joinAction; }))
            err(where + L".online.join", L"no action with id \"" + widen(g.joinAction) + L"\"");
        checkVars(g.image, where + L".image", issues);
        if (g.mod) {
            const mod::Spec& m = *g.mod;
            std::wstring mw = where + L".mod";
            if (m.game.executable.empty() && m.game.validationFiles.empty())
                err(mw + L".game", L"give the game's \"executable\" or \"validationFiles\" so its folder can be recognised");
            if (m.game.detection.empty()) err(mw + L".game.detection", L"say where to look for the game (steam, epic, registry or path)");
            for (const auto& r : m.game.detection)
                if (r.type == L"steam" && r.appId.empty() && m.game.steamAppId.empty() && r.installDir.empty())
                    err(mw + L".game.detection", L"a steam rule needs an appId (or game.steamAppId) or an installDir");
            if (m.components.empty()) err(mw + L".components", L"add at least one component");
            std::set<std::string> cids;
            for (const auto& c : m.components) {
                if (!validId(c.id)) err(mw + L".components", L"component ids must be letters, digits, '-', '_' or '.'");
                else if (!cids.insert(c.id).second) err(mw + L".components", L"duplicate component id \"" + widen(c.id) + L"\"");
                if (c.source.empty() == c.sourceGame.empty())
                    err(mw + L".components." + widen(c.id),
                        L"give either \"source\" (a path inside the package) or \"sourceGame\" (files in the game folder)");
                if (!c.targetName.empty() && (c.targetName.find_first_of(L"/\\") != std::wstring::npos || c.targetName == L"." ||
                                              c.targetName == L".."))
                    err(mw + L".components." + widen(c.id) + L".targetName", L"a file name, not a path");
                for (const auto& sg : c.sourceGame)
                    if (mod::normRel(sg).empty() || sg.find(L"..") != std::wstring::npos)
                        err(mw + L".components." + widen(c.id) + L".sourceGame", L"paths inside the game folder");
                checkVars(c.source, mw + L".components", issues);
                checkVars(c.target, mw + L".components", issues);
            }
            if (m.package.modsInfo.empty() && m.package.url.empty() && m.package.github.empty())
                err(mw + L".package", L"give \"github\" (owner/repo), \"modsInfo\" (the mod's id in mods-info) or a \"url\"");
            if (!m.package.url.empty() && !startsWithI(m.package.url, L"https://"))
                err(mw + L".package.url", L"must be an https:// address");
            if (!m.package.github.empty() && m.package.github.find(L'/') == std::wstring::npos)
                err(mw + L".package.github", L"must look like \"owner/repo\"");
            std::set<std::wstring> extraDirs;
            for (size_t k = 0; k < m.package.extras.size(); ++k) {
                const auto& e = m.package.extras[k];
                std::wstring ew = mw + L".package.extras[" + std::to_wstring(k) + L"]";
                if (e.dir.empty() || mod::normRel(e.dir).empty() || e.dir.find_first_of(L"/\\") != std::wstring::npos)
                    err(ew + L".dir", L"a single folder name, e.g. \"loader\"");
                else if (!extraDirs.insert(toLower(e.dir)).second) err(ew + L".dir", L"used twice");
                if (e.url.empty() && (e.github.empty() || e.asset.empty()))
                    err(ew, L"give a \"url\", or \"github\" and \"asset\"");
                if (!e.url.empty() && !startsWithI(e.url, L"https://")) err(ew + L".url", L"must be an https:// address");
            }
            for (size_t k = 0; k < m.install.requires_.size(); ++k)
                if (m.install.requires_[k].path.empty() || mod::normRel(m.install.requires_[k].path).empty())
                    err(mw + L".install.requires[" + std::to_wstring(k) + L"].path", L"a path inside the game folder");
            for (size_t k = 0; k < m.install.uninstallCleanup.size(); ++k) {
                const std::wstring& pat = m.install.uninstallCleanup[k];
                std::wstring prefix = pat.substr(0, pat.find_first_of(L"*?"));
                if (mod::normRel(pat).empty() || (!prefix.empty() && mod::normRel(prefix).empty()) || pat.rfind(L"*", 0) == 0)
                    err(mw + L".install.uninstallCleanup[" + std::to_wstring(k) + L"]",
                        L"must be a path inside the game folder that starts with a folder name, e.g. \"Loader/cache/**\"");
            }
            if (iequals(m.install.stateDir, m.install.targetDir) || iequals(m.install.stateDir, L"${GAME_DIR}"))
                err(mw + L".install.stateDir", L"must be a sub-folder, not the install or game folder itself");
            checkVars(m.install.targetDir, mw + L".install", issues);
            checkVars(m.install.stateDir, mw + L".install", issues);
            for (const auto& d : m.game.detection) {
                checkVars(d.path, mw + L".game.detection", issues);
                checkVars(d.installDir, mw + L".game.detection", issues);
            }
        } else if (g.actions.empty() && !g.comingSoon) {
            err(where + L".actions", L"add at least one action (e.g. \"Play\")");
        }
        std::set<std::string> actionIds;
        for (size_t j = 0; j < g.actions.size(); ++j) {
            const auto& a = g.actions[j];
            std::wstring aw = where + L"." + indexPath(L"actions", j);
            if (!validId(a.id)) err(aw + L".id", L"must be letters, digits, '-', '_' or '.'");
            else if (!actionIds.insert(a.id).second) err(aw + L".id", L"duplicate action id \"" + widen(a.id) + L"\"");
            if (a.isMod()) continue;  // made by the launcher: no file to check
            if (a.isLink()) {
                if (!startsWithI(a.link, L"https://")) err(aw + L".link", L"must be an https:// address");
                continue;
            }
            if (a.isWeb()) {
                if (a.server.enabled() && a.server.url.empty())
                    err(aw + L".server.url", L"give the address the game opens at, e.g. \"http://localhost:2053/\"");
                for (const auto& pid : a.players)
                    if (auto p = cfg.findPlayer(pid); p && p->kind != "browser")
                        err(aw + L".players", L"\"" + widen(pid) + L"\" is not a browser; HTML5 games need one");
                if (!std::any_of(cfg.players.begin(), cfg.players.end(), [](const PlayerSpec& p) { return p.kind == "browser"; }))
                    err(aw, L"HTML5 games need a player with \"kind\": \"browser\"");
                for (const auto& re : {a.server.share, a.server.lan}) {
                    try {
                        if (!re.empty()) std::wregex test(re);
                    } catch (const std::regex_error&) {
                        err(aw + L".server", L"not a valid pattern: " + re);
                    }
                }
            } else if (a.swf.empty()) {
                bool all = !a.overrides.empty();
                for (const auto& p : cfg.players)
                    if (a.players.empty() || std::find(a.players.begin(), a.players.end(), p.id) != a.players.end()) {
                        auto o = a.overrides.find(p.id);
                        all = all && o != a.overrides.end() && o->second.hasSwf;
                    }
                if (!all) err(aw + L".swf", L"list where the game's .swf file is");
            }
            for (const auto& s : a.swf) checkVars(s, aw + L".swf", issues);
            for (const auto& s : a.args) checkVars(s, aw + L".args", issues);
            for (const auto& [k, v] : a.flashvars) checkVars(v, aw + L".flashvars", issues);
            for (const auto& p : a.players)
                if (!cfg.findPlayer(p)) err(aw + L".players", L"unknown player \"" + widen(p) + L"\"");
            for (const auto& [pid, o] : a.overrides) {
                if (!cfg.findPlayer(pid)) err(aw + L".overrides", L"unknown player \"" + widen(pid) + L"\"");
                for (const auto& s : o.swf) checkVars(s, aw + L".overrides", issues);
                for (const auto& s : o.args) checkVars(s, aw + L".overrides", issues);
            }
        }
    }
    checkVars(cfg.paths.games, L"paths.games", issues);
    checkVars(cfg.paths.players, L"paths.players", issues);
    checkVars(cfg.paths.data, L"paths.data", issues);
}

std::wstring formatIssues(const std::vector<ConfigIssue>& issues) {
    std::wstring out;
    for (const auto& i : issues) {
        out += i.severity == ConfigIssue::Severity::Error ? L"error   " : L"warning ";
        out += i.where.empty() ? L"(top level)" : i.where;
        out += L": " + i.message + L"\n";
    }
    return out;
}

}  // namespace fl
