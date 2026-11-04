<h1 align="center">Multiplayer Everything Launcher</h1>

<p align="center"><b>We make multiplayer mods for games that never had one. This is the app that installs them and gets you into a room with friends.</b></p>

<p align="center">
<a href="https://discord.gg/jnnyz2vqB5"><img src="https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white&style=for-the-badge" alt="Join the Discord" height="36"></a>
<a href="https://multiplayereverything.com"><img src="https://img.shields.io/badge/Website-multiplayereverything.com-1abc9c?style=for-the-badge" alt="multiplayereverything.com" height="36"></a>
<a href="https://github.com/multiplayereverything/multiplayer-mods-launcher/releases/latest"><img src="https://img.shields.io/badge/Download-latest%20release-e5384c?style=for-the-badge" alt="Download the latest release" height="36"></a>
</p>

<p align="center">
<a href="#quick-start">Quick start</a> ·
<a href="#how-it-works">How it works</a> ·
<a href="#what-it-does">What it does</a> ·
<a href="#games">Games</a> ·
<a href="#command-line">Command line</a> ·
<a href="#build">Build</a> ·
<a href="#repository">Repository</a>
</p>

Multiplayer Everything makes multiplayer mods for games that never had multiplayer before. Open the launcher, pick a game, and
press **Play online**. For the **Flash games** it starts them in Ruffle Together (our Ruffle build with online co-op) with
the right file and settings, and you join a room with a code: no IP addresses, no port
forwarding. For the **PC game mods** (Spooky Together, Baldi Together, MiSide Together, CupHeads, Rick and Morty Together, Dave the Diver Multiplayer and more) it finds your copy
of the game, installs the mod into it, keeps it up to date, and can take it out again,
putting back every game file it replaced. Native C++20/Win32, no dependencies, and the
whole app is one `.exe`.

Questions, bugs, or looking for people to play with? Join the [Multiplayer Everything Discord](https://discord.gg/jnnyz2vqB5).

![Library](docs/screenshots/01-library.png)

Games available now: **Gun Mayhem 2** · **Stealth Hunter 2** · **Bad Ice-Cream** ·
**Territory War** · **Bow Man** · **Ultimate Crab Battle** · **Air Battle** · **Raft Wars** ·
PC mods: **Spooky Together** · **Baldi Together** · **MiSide Together** · **CupHeads** · **Rick and Morty Together** · **Dave the Diver Multiplayer** · **GTA San Andreas Co-op**. The sidebar keeps *PC mods* and *Flash mods* apart. Full list under [Games](#games).

## Quick start

1. Download `MultiplayerEverythingLauncher.exe` from [Releases](https://github.com/multiplayereverything/multiplayer-mods-launcher/releases/latest)
   (or the `.zip` with the editable `launcher.json` and covers next to it) and put it in
   its own folder: game files are saved in a `games\` folder beside it.
2. Run it. If Ruffle Together is not found it offers to install it; say yes.
3. Set your name in Settings once (Ctrl+,).
4. **Flash game:** select it and press *Play online*, or *Quick play* to join the busiest
   open room. **PC mod:** select it and press *Install*, then *Launch*.

More detail: [Playing a Flash game](#playing-a-flash-game) and [PC game mods](#pc-game-mods).

## How it works

| | Flash games | PC game mods |
|---|---|---|
| What the launcher does | Finds Ruffle Together and the game's `.swf`, builds the exact command line, starts it, watches it exit | Finds the game, downloads the mod's package, installs it into the game's folder, keeps it updated |
| Online play | Rooms with a code on the central server; direct IP through the built-in relay (Radmin, LAN, Tailscale) | The mod connects through the same central server |
| Safety | Checksums on every download (Ruffle Together, game files, the launcher itself) | One all-or-nothing install with a SHA-256 check; backups of replaced files; Uninstall restores the game |
| Where it's configured | `launcher/launcher.json` | the game's `mod` object in `launcher/launcher.json` |
| Guide | [CONFIG.md](CONFIG.md) | [docs/MOD_PACKAGES.md](docs/MOD_PACKAGES.md) |

A new game or mod never needs a code change: everything lives in `launcher.json`
(also built into the exe, so the exe is all you need to send someone).

## What it does

- **Installs PC game mods safely.** Pick a mod, press *Install*: the launcher finds
  the game (Steam, Epic, registry, or a folder you choose), downloads the mod's
  package, checks it against its SHA-256 and copies it in as one all-or-nothing
  transaction. Cancel, an error or a crash undoes every change; files the mod
  replaces are backed up first; your edited settings files are kept on update.
  *Update*, *Repair* and *Uninstall* work the same way, and a mod installed earlier by
  its stand-alone setup is recognised as installed. Mods are installed per game and
  need the game closed.
- **Finds everything itself.** Ruffle Together (its own `players\ruffle-together\`
  folder) and each game's `.swf` in `games\` or your Flashpoint folder. Anything missing gets a button that fixes it: *Locate game
  file...* or *Install Ruffle Together*.
- **Installs Ruffle Together for you.** Without it, the launcher offers
  to install it: it downloads the newest release from our ruffle-together
  GitHub page, checks it against the published SHA-256 checksum, and unpacks it
  into `players\ruffle-together\`. Nothing is downloaded without your OK.
- **Player slots.** Every game shows how many players it takes, in the players'
  own colors (P1 red, P2 blue, P3 green, P4 yellow). While you host, the slots fill
  with the names of the people who joined.
- **Online first.** Online games lead with *Play online*: rooms with a code on the
  Multiplayer Everything central server, no IP addresses and no port forwarding.
  *Quick play* joins the busiest open room, or starts the game ready to host when
  none is open. Direct IP (Radmin, LAN) is still there under *Other ways to connect*.
- **A lobby for the games.** For mods made for it, *Play online* opens the launcher's
  own lobby: host a room (on the server or by direct IP, with an optional password) or
  join one, see who's in it in their P1-P4 colors, chat, and press *I'm ready*. The host
  presses *Start game* and every launcher opens the game with the room's values; the game
  starts only once everyone's game has loaded. Dropped players get 30 seconds to come
  back to their slot, the host can kick or ban, and direct-IP hosts get their addresses
  (VPNs first), a firewall check, a port test and optional UPnP. The mod itself needs no
  online panel: it talks to the launcher on `127.0.0.1` ([docs/LOBBY_BRIDGE.md](docs/LOBBY_BRIDGE.md)).

  ![Lobby](docs/screenshots/lobby-room.png)

- **Rooms.** The *Rooms* page in the sidebar lists every open room of every game
  (filter by game, hide full rooms, search), with the number of players online next
  to it. Each online game also has a *Rooms* tab. *Join* (or *Join by code*) copies
  the code and starts the game with it; mods that read it join by themselves,
  otherwise paste it in the game's F9 panel. The bottom bar shows whether the server
  answers and how fast. Lists refresh every 30 seconds.
- **Updates itself.** At start the launcher checks the newest release here; when
  there is a newer one it downloads `MultiplayerEverythingLauncher.exe`, checks it against the
  SHA-256 GitHub publishes for it, swaps it in and restarts into it by itself, without asking (while a game
  runs or a room is open, it just starts next time). Settings can turn it off; `--update` does it now.
- **One file.** `launcher.json` and the cover images are built into
  `MultiplayerEverythingLauncher.exe`, so the exe is all you need to send someone. A `launcher.json`
  or `covers\` folder next to the exe still wins, for your own changes.
- **Game updates.** When a newer build of a downloaded game file is published, the
  card says *Update available* and the details panel offers *Update game files*.
- **What the website says.** Each game shows the website's quality grade on its card (A complete, B playable, C rough, D experimental, X none yet; a game still being tested shows none), and its details panel shows the stage, the newest version, how it connects and what stays in sync (players, story, enemies, world, items). The launcher reads [the website's games.json](https://www.multiplayereverything.com/games.json) in the background, at most once an hour, and keeps a copy for when you're offline.
- **Games from the website.** A game the website lists that the launcher doesn't have yet appears as a *Coming soon* card, with the website's name, description, player count, colors and cover picture (downloaded once, kept in the launcher's data folder). A game the launcher has but that leaves out a description, player count or cover gets the website's. The list is read at start, so a new game shows up the next time the launcher starts.
- **A guide for every game.** *Guide* in the details panel opens a page with the game's full description, what you need, how to play it step by step, its keys, tips and what the website says about it, with links. Every game has one: where `launcher.json` has no text, a default for that kind of game fills it in. See `guide` in [CONFIG.md](CONFIG.md).
- **A calmer library.** Games that aren't out yet have their own *Coming soon* view, favorites and ready games come first, and an empty list says what to do.
- **Your name.** The first start asks for your player name; others see it in rooms.
- **Copy diagnostics.** One button bundles the launcher's version, players, network
  addresses, the room's state and the latest log lines for a bug report (your name left out).
- **Settings.** Your name for online rooms, minimize while playing, default player,
  showing offline play (solo and one keyboard, off by default) and the original
  (unmodded) versions, online features on or off, automatic updates, where game
  files are kept, and the log folder (button at the bottom of the sidebar, or Ctrl+,).
- **Discord.** With a Discord application id in `launcher.json`, Discord shows what
  you are playing and the room code (off until an id is set).
- **Calm motion.** A short entrance when the window opens, cards that lift under
  the mouse, open player slots that breathe while you host. All of it follows
  Windows' *Animation effects* setting.
- **Hosting without extra windows.** *Host online* starts a built-in relay (the
  same protocol as the mods' `gm2_relay.py` / `sh2_relay.py`, so no Python is
  needed) and shows who is connected plus the addresses friends should type,
  including VPN adapters such as Radmin VPN, ZeroTier or Tailscale.
- **Smooth.** One native process. Games run in their own player process; the
  launcher only watches them, so it never slows a match down. The game grid is a
  double-buffered list with art pre-scaled once per size.
- **Keeps track.** Favorites, play count, play time, last played, and which
  player you prefer per game.
- **Everything in `launcher.json`.** Games, actions, players, colors and every
  visible string. Adding a game needs no C++ (see [CONFIG.md](CONFIG.md)).

## Games

| Game | Actions | Notes |
|---|---|---|
| Gun Mayhem 2: More Mayhem | Play online, Quick play, Play on one keyboard | Up to 4 players, with the Gun Mayhem 2 Online mod. Direct IP: relay port 7777. |
| Stealth Hunter 2 | Play online, Quick play, Play solo | The Stealth Hunter 2 Online mod adds co-op for up to 4. Runs in Ruffle only. Direct IP: relay port 7778. |
| Bad Ice-Cream | Play online, Quick play, Play on one keyboard | Two players, with the Bad Ice-Cream Online mod (Nitrome's game). |
| Territory War | Play online, Quick play | Red against blue on up to 4 PCs. |
| Bow Man | Play online, Quick play | Two archers on two PCs. Ruffle only. |
| Ultimate Crab Battle | Play online, Quick play | Co-op for up to 4, one ship each. |
| Air Battle | Play online, Quick play | Co-op for up to 4, one balloon boat each. |
| Raft Wars | Play online, Quick play | Co-op for up to 4, taking turns at the bazooka. |
| Intrusion, Intrusion 2 | | Coming soon: listed, no buttons yet. |

The newer mods are online-only (no direct IP); their single-player modes are under
*Play offline* when Settings shows offline play. Until a mod's file is published in
mods-info, the central server doesn't know the game: its card says *Not on the server
yet* and Quick play is hidden.

More games are on the way; the local-only classics (Gun Mayhem, Fireboy and
Watergirl, Boxhead) are set aside in `docs/local-games-example.jsonc` for now.

The original, unmodded versions of the modded games are hidden; Settings can show
them (*Play the original*).

**Getting the games.** When a game file is missing, the game shows *Download game
files*: the launcher fetches them from [multiplayereverything/flash-games](https://github.com/multiplayereverything/flash-games),
checks each file against the SHA-256 in that repository's catalog, and saves it in
`games\`. That covers every game in the list except the coming-soon ones (the
repository's README lists how far each build is tested). You can also put your own `.swf` files in `games\` (names are in
`games\README.txt`), use *Locate game file...*, or install Flashpoint, whose copies
are picked up automatically.

## PC game mods

Not Flash: installed into the game's own folder.

| Mod | Game | Notes |
|---|---|---|
| Spooky Together | Spooky's Jump Scare Mansion: HD Renovation (Steam) | Co-op for 2 to 4 players. Installs the mod from the newest release of [spooky-together](https://github.com/multiplayereverything/spooky-together), together with the BepInEx 5 loader published in the same repository. |
| Baldi Together | Baldi's Basics Plus (Steam) | Co-op for 2 to 4 players, over LAN or Steam. Installs the mod from [baldi-plus-coop](https://github.com/multiplayereverything/baldi-plus-coop), together with the BepInEx 5.4.23.5 loader published in the same repository. |
| MiSide Together | MiSide (Steam) | Co-op for 2 to 4 players, through Steam, LAN or an address. Installs the mod from [miside-coop](https://github.com/multiplayereverything/miside-coop), together with the BepInEx 6 (IL2CPP, be.790) loader it needs, which is published in that repository's releases. |
| CupHeads | Cuphead (Steam) | Online co-op for 2 players over Steam invites or LAN, from [CupHeads](https://github.com/multiplayereverything/CupHeads) (a fork of [Germanized's CupHeads](https://github.com/Germanized/CupHeads)). Installs the mod and the BepInEx 5.4.23.5 loader published in that repository. |
| Rick and Morty Together | Rick and Morty: Virtual Rick-ality (Steam, VR) | Co-op for 2 players, from [rick-and-morty-coop](https://github.com/multiplayereverything/rick-and-morty-coop). Installs the mod, its BepInEx settings (this game needs them) and the BepInEx 5.4.23.5 loader from the same repository. |
| Dave the Diver Multiplayer | DAVE THE DIVER (Steam) | Two-player co-op through Steam or LAN, from [Dave-The-Divers-Coop](https://github.com/multiplayereverything/Dave-The-Divers-Coop) (a fork of [S1ii's Coop-Divers](https://github.com/S1ii/Coop-Divers)). Installs the mod and the BepInEx 6 (IL2CPP, be.790) loader published in that repository. |

| GTA San Andreas Co-op | Grand Theft Auto: San Andreas, PC version 1.0 US (Steam or Rockstar) | Co-op for 2 to 4 players, based on CoopAndreas, from [gta-coop](https://github.com/multiplayereverything/gta-coop). Does what the mod's own `install.bat` does without running it: keeps the game's `eax.dll` as `eax_orig.dll`, puts the co-op `eax.dll` in its place, and Uninstall puts the original back. The host's server is installed under `CoopAndreas\server`. You need your own copy, downgraded to 1.0. |

Each is downloaded from its GitHub release, checked against the SHA-256 GitHub publishes for it, and installed as one all-or-nothing change; Uninstall puts the game folder back exactly as it was. How a mod is described: [docs/MOD_PACKAGES.md](docs/MOD_PACKAGES.md).

Pick the mod, press *Install*, then *Launch*. *Update*, *Repair* and *Uninstall* sit beside it; the game must be closed. Try it safely from the command line with `--game-dir` on a copy of the game folder. Authors: see [docs/MOD_PACKAGES.md](docs/MOD_PACKAGES.md).

## Playing a Flash game


**Hosting:** set your name in Settings once. Select the game, press *Play online*,
then in the game press **F9**, set MODE to ONLINE and click **HOST**. Send friends
the room code it shows; the room also appears in everyone's *Rooms* list.

**Joining:** pick the room on the *Rooms* page or the game's *Rooms* tab and press
*Join* (or type the code under *Join by code*). The code is on your clipboard:
in the game press **F9**, paste it and click **JOIN**, if the game hasn't joined
by itself.

**Direct IP** (*Other ways to connect*): *Host with your IP* starts the built-in
relay and shows the addresses friends type, including Radmin VPN, ZeroTier or
Tailscale. Windows may ask to allow `MultiplayerEverythingLauncher.exe` through the firewall the
first time: allow it, or friends can't connect.

Keyboard: arrow keys move through the grid, **Enter** plays, **Ctrl+F**
searches, **Ctrl+,** opens Settings, **F5** rescans for players and game files,
**Tab** reaches every button.

## Command line

```
MultiplayerEverythingLauncher.exe --list                       every game and whether it can start
MultiplayerEverythingLauncher.exe --launch gunmayhem2:host     start an action without the window
MultiplayerEverythingLauncher.exe --launch gunmayhem2:online --join ABCDE   ...joining a room (--address <ip:port> for direct IP)
MultiplayerEverythingLauncher.exe --relay 7777                 run only the relay (a dedicated relay PC)
MultiplayerEverythingLauncher.exe --install-player ruffle      install Ruffle into players\ruffle
MultiplayerEverythingLauncher.exe --download-game all          download every game file the catalog has
MultiplayerEverythingLauncher.exe --rooms gunmayhem2           list the open rooms on the central server
MultiplayerEverythingLauncher.exe --update                     install the newest launcher release now
MultiplayerEverythingLauncher.exe --mods                       the PC mods: game found, installed version, newest version
MultiplayerEverythingLauncher.exe --install-mod spooky-together  install or update a PC mod (add --game-dir "<folder>" to point at the game)
MultiplayerEverythingLauncher.exe --repair-mod spooky-together   put back missing or damaged files
MultiplayerEverythingLauncher.exe --uninstall-mod spooky-together  remove it and restore the game's files
MultiplayerEverythingLauncher.exe --validate                   check launcher.json
```

Add `--json` to `--list`, `--mods` or `--rooms` for output a program can read (the Tauri prototype drives the
launcher this way). `MultiplayerEverythingLauncher.exe --help` lists everything. Settings and the log live in
`%LOCALAPPDATA%\MultiplayerEverything\FlashLauncher\`.

## Build

The launcher is one Tauri app: the window in `src/`, its Rust side in `src-tauri/`, how to build and test it in
[DEVELOPING.md](DEVELOPING.md). Until the Rust side takes over every job, the window still runs on the C++
launcher, which now lives in `old/`. The plan and what's done so far are in [PORTING.md](PORTING.md).
Every update ships for both Windows and Linux: see [PLATFORMS.md](PLATFORMS.md).

The C++ launcher requires Visual Studio 2022 (or Build Tools) with the C++ workload.

```bat
powershell -ExecutionPolicy Bypass -File old\build.ps1 -Test
powershell -ExecutionPolicy Bypass -File old\build.ps1 -Dist
```

`-Test` runs the C++ tests (config, file lookup, command lines, settings, and
the relay against real sockets) and `tests\e2e.ps1`, which drives the real exe
and its window through the accessibility API with a fake player, and installs a
fake player and fake game files from a local test server (checksums, failed
reinstall, first-run prompt, the window's download button). `-Dist` makes
`dist\MultiplayerEverythingLauncher\` and its `.zip`, and refuses to package any
`.swf` or player binary; it also copies the single-file `dist\MultiplayerEverythingLauncher.exe`.

## Repository

| Path | Contents |
|---|---|
| `src/`, `src-tauri/`, `tests/lab/` | The launcher app: the window (React), its Rust side, the test lab |
| `old/` | The C++ launcher, kept while the Rust port takes its jobs over one by one |
| `old/src/core/mod/` | The PC-mod engine (ported from the installer template): game detection, planning, the all-or-nothing install transaction, the install record, package download |
| `old/src/core/lobby.*` | The lobby: rooms through the server or direct IP, ready-up, start and the loaded barrier, reconnects, and the localhost bridge the game talks to |
| `old/src/core/` | No UI: config, library (players, files, command lines), process watching, relay, downloads, rooms, settings, Discord, built-in files |
| `old/src/ui/` | The old C++ launcher window and its theme |
| `old/src/app/` | Entry point and command line |
| `launcher/` | `launcher.json` and `covers\` (both also built into the C++ exe), `games\`, `players\`: shared by both |
| [docs/ADDING_A_MOD.md](docs/ADDING_A_MOD.md) | Checklist for adding a PC mod: author, review, loader, entry, tests |
| `docs/html5-example.jsonc` | HTML5 support (browser players, Plazma Burst 2 co-op), kept out of the shipped list for now |
| `docs/local-games-example.jsonc` | Gun Mayhem, Fireboy and Watergirl, Boxhead: local-only games set aside for now |
| `old/tests/` | C++ tests and the end-to-end script |
| [CONFIG.md](CONFIG.md) | Every `launcher.json` option |

Ruffle Together is built from Ruffle, which is open source ([ruffle.rs](https://ruffle.rs)). The
games belong to their owners; this project ships neither the player nor any game.

## Made with

This project was made with the help of these tools:

- [antislop-ui](https://github.com/miqdadbadjuber/anti-slop/tree/main/skills/antislop-ui)
- [coop-anything](https://github.com/cranebeaker/coop-anything)
- [REA](https://github.com/morluto/rea)
- [Universal Modder](https://github.com/rehan-remade/universal-modder)
