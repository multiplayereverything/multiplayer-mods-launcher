# Parity with the C++ window

Each feature of the released C++ window (`src/ui/launcher*.cpp` and `settings.cpp`), where the Tauri window has it,
and the lab check that covers it (`npm run lab`). The C++ launcher does the work underneath through its command line.

| C++ window | Tauri window | Lab check |
|---|---|---|
| Category sidebar with counts (`buildNav`) | Sidebar | the sidebar counts every game / each category |
| Search, sort, compact cards (`syncChips`, `matches`) | Library | search finds a game; A to Z; Compact makes the cards smaller |
| Cards: art, badges, grade, state, room count (`drawCard`) | `Tile` | a card shows how many rooms are open; a selected card keeps its full size |
| Details panel: slots, state, website grade, in sync (`paintDetail`, `paintSite`) | Game page panel | its grade shows in the panel |
| Host and Join, by code and by IP (`onHostGame`, `onJoinGame`, `askAddress`) | Host / Join dialogs | Host a game hosts...; Join with a code...; Join by IP... |
| Other ways to connect, offline and original play (`actionShown`) | Panel | Host with your IP hosts directly; Play solo is hidden while offline play is off |
| Quick play (`quickPlay`, `lobbyQuick`) | Panel | Quick play joins that room; Quick play on a lobby game goes straight for a room |
| Co-op save choice (`onSaveChoice`, `coopSaveSupported`) | Panel | hosting passes the co-op save choice |
| Game settings (`onSettingChanged`) | Panel | a game setting is saved; hosting passes the host settings |
| Player per game (`onPlayerChoice`) | Panel | the game starts in the chosen player |
| Download and locate game files (`offerGameDownload`, `locateFile`, `checkOutdated`) | Panel, dialog | Play on a missing game offers the download; Locate makes the game ready; a newer build shows as an update |
| Install a missing player (`offerInstall`, `getPlayer`) | Dialog, setup | Play without its player offers to install it; Install downloads the Flash player |
| Player updates (`startUpdateCheck`, `onPlayerUpdated`) | At start (`--update-players`) | not in the lab (needs a GitHub release) |
| Relay hosting box (`paintSlots`, `copyAddress`, `stopRelay`) | Hosting card | a friend who joins is listed; Copy puts the address on the clipboard; Stop hosting |
| Rooms page, rooms per game (`roomsPage`, `joinListedRoom`) | Rooms page and tab | Open rooms lists...; a room says when its game has started... |
| PC mods (`runModAction`, `chooseGameFolder`, `pollModGames`) | Panel | Install, Repair, Uninstall leaves the game folder exactly as it was |
| Lobby (`openLobby`, `lobbyHost`, `lobbyJoin`) | Lobby page (`--lobby-session`) | hosting a lobby room lists you in it; chat reaches the room |
| Name prompt (`askName`) | First-run setup | the setup opens by itself on a first run |
| Settings window (`settings.cpp`) | Settings page | Show offline play is saved; Your name is saved; Minimize... |
| Minimize while playing, restore after (`minimizeOnLaunch`, `onProcessExit`) | `--wait` | the window minimizes...; comes back when the game closes |
| Discord status (`updatePresence`) | `--launch --wait` shows it | not in the lab (needs the Discord app) |
| Hints after an action starts (`notice`) | Messages | starting shows the hint of the action |
| Controller (`pollPad`) | `usePad` | the D-pad moves...; A opens...; Y goes to the search |
| Status bar: server, ping, player, Flashpoint (`paintStatus`) | Status bar | the status bar measures the server |
| Copy diagnostics (`copyDiagnostics`) | Settings | Copy diagnostics puts a report on the clipboard |
| Light theme (`lightPalette`) | `data-theme` | the light theme switches the palette |

## Not carried over, on purpose

- **The lobby's direct-connection checks (`startDirectChecks`: UPnP, whether the port can be reached).** No shipped
  game plays through the lobby yet. They belong in `--lobby-session` when one does.
- **Web games and game servers (`runWeb`, `launchBrowser`, `onServerReady`).** No game in launcher.json uses them.
- **Self-update of the window.** The C++ launcher updates itself; the Tauri window needs a signing key and a release
  feed first. The release build (`scripts/release.ps1`) ships the tested C++ launcher beside it.
