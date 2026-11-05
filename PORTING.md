# Moving the launcher's work into the Tauri app

The launcher becomes one Tauri app. Today the window still asks the C++ launcher in `old/` for everything
(`src-tauri/src/engine.rs` runs it with the commands below). Each part here moves that job into Rust inside the
app; when the last one is done, `old/` is deleted and the app ships alone.

## How each part is done

1. Port the C++ module to Rust under `src-tauri/src/` and its tests from `old/tests` with it: same inputs, same
   answers.
2. Switch the window's command from "run the C++ launcher" to the Rust code, keeping the JSON the window gets the
   same, so the React side doesn't change.
3. `npm run lab` passes, and the C++ e2e checks for that job pass against the Rust side too: they are the record
   of what the launcher must do.
4. Only then the next part. The C++ launcher keeps doing whatever isn't ported yet, so the app works at every step.

Rules carried over from the C++ launcher (old `AGENTS.md`): no game names in code, everything visible comes from
`launcher.json`'s texts, game files and player binaries are never shipped, downloads are checked by SHA-256 under
a temporary name, and a failed or cancelled mod install leaves the game folder byte-identical.

## The parts, in order

| # | Part | C++ today | Window commands it replaces | Status |
|---|---|---|---|---|
| 1 | launcher.json, variables, settings.json, the library (players, game files, command lines), the website's games.json merge | `config`, `vars`, `settings`, `library`, `site`, `embedded`, `cli_json` | `--list --json`, the settings writes, favorites, located files | Done: `src-tauri/src/launcher`, same JSON on every input of `tests/parity/library.mjs` |
| 2 | Open rooms and the central server's address and ping | `rooms` | `--rooms all --json`, `--rooms <id>`, ping | Done: `src-tauri/src/launcher/rooms.rs` (and `http.rs` for servers.json), same JSON, exit codes, errors and server requests on every case of `tests/parity/rooms.mjs`; ping was already Rust |
| 3 | Starting a game: the command line, host options, TV options, the relay, the HTML5 web server, Discord, play time | `library`, `process`, `relay`, `webserver`, `discord`, `game_options`, `launcher_headless` | `--launch ... --wait [--tv] [--coop-save]` | Done for every non-mod action: `src-tauri/src/launcher/launch.rs` (with `process.rs`, `relay.rs`, `webserver.rs`, `discord.rs`, `log.rs`); same arguments, environment, working folder, exit codes, messages, settings.json, relay reports and relay protocol on every case of `tests/parity/launch.mjs`. PC mods' Launch still runs the C++ launcher (part 6) |
| 4 | Players and game files: GitHub releases, downloads with SHA-256, unzip, updates with progress | `install` | `--install-player`, `--update-players [--check]`, `--download-game` | Done: `src-tauri/src/launcher/install.rs` (with `http.rs`'s streamed downloads); same exit codes, stdout and progress lines, messages, requests, and players, games and data folders (names, sizes, SHA-256, launcher.log) on every case of `tests/parity/downloads.mjs`. Unzipping is still Windows' tar.exe, as in C++ |
| 5 | The lobby and the game bridge | `lobby`, `lobby_cli` | `--lobby-session` | |
| 6 | The PC-mod engine: detection, the install record, the journaled install / update / repair / uninstall, packages | `mod/*` | `--mods --json`, `--install-mod`, `--repair-mod`, `--uninstall-mod` | |
| 7 | Community mods approved into mods-info (`records/<id>.json`, docs/COMMUNITY_MODS.md), and the website's listed projects | (new) | the mod commands read the app's saved copy | Done: `community.rs` fetches and checks them; `old/src/core/community.cpp` reads the same copy for the C++ mod engine until part 6; lab `communitymods` installs one |
| 8 | The app updating itself: Tauri's updater with a signature from an offline key | `install` (`updateSelf`) | `--update` | Done: `src-tauri/src/app_update.rs` (tauri-plugin-updater, `latest.json` from the newest release, key in `~\.tauri`), the prompt in App.tsx, `scripts/release.ps1` signs and writes the feed; lab `appupdate`. The C++ launcher hands its players over to the app (`updates.app`, `appExe`; e2e "handing over") |
| 9 | Delete `old/`, ship the app alone | | | |

The PC-mod engine is late on purpose: it writes into players' game folders, so it moves only once its tests
(`old/tests/test_mod.cpp` and the "PC game mods" part of `old/tests/e2e.ps1`) are ported and pass.

## Found while porting (kept as the C++ does it, for later)

- Part 1: the window never downloaded the website's games.json; the library only read the copy the C++ window
  saved (data\site.json). Fixed: `refresh_site` (library_json.rs, site.rs) fetches it and its covers at start and
  every hour, using the saved copy for an hour, as the C++ window did.
- Part 1: with no "minimize" in settings.json, `user.minimize` reads true even when launcher.json's
  `behavior.minimizeOnLaunch` is false.
- Part 1: the launcher version comes from old/CMakeLists.txt (src-tauri/build.rs); give it a home before old/ goes.
- Part 1: games added from the website are not validated (mergeSite runs after validation).
- Part 2: `--rooms all` and `--rooms <id>` disagree on a bare "E|" answer: the single game's error is empty, while
  `all` fills it with why servers.json couldn't be read (main.cpp's `e.empty() ? err : e`). Kept in both.
- Part 2: why servers.json failed is only shown when there's no `rooms.server` to fall back on; with one, a broken
  servers.json is silent.
- Part 2: an "online" server in servers.json whose address is empty wins over `rooms.server`, so no rooms are
  listed; a `status` that isn't text counts as online.
- Part 2: WinHTTP's errors read "Unknown error (code 12029)": FormatMessage is asked without winhttp.dll's
  message table.
- Part 2: the room errors ("Cannot reach ...", "Server address needs a port: ...", "No answer from ...") are English
  in the code, not texts from launcher.json.
- Part 2: a server that answered RELAY|2 with "unsupported" is asked with RELAY|1 for as long as the process runs;
  in the app that is until it is closed, as in the C++ window.
- Part 2: the room's `version` is parsed but not in the JSON; `--rooms <id>` lists a coming-soon game, `all`
  skips it.
- Part 3: `coopSaveGame` recognises Ruffle Together's co-op games by the online id "ruffle-coop" written in the code
  (catalog.rs `coop_save_game`): a game name in code, kept so both launchers offer the save choice to the same games.
- Part 3: `--join` keeps letters and digits outside ASCII (iswalnum) but upper-cases only ASCII ones (towupper in the
  C locale): " éa1ß" becomes "éA1ß". Kept.
- Part 3: when a C++ launch failed, the window's message was the launcher's whole stderr: the log lines it echoed
  (time-stamped, `--launch` turns the echo on) and then the reason. The Rust launch sends the reason alone; nothing
  else differs.
- Part 3: every Flash launch fetches servers.json (for `${SERVER}`), even a solo play whose arguments don't use it,
  so a slow mods-info delays starting a game.
- Part 3: the relay doesn't read what a client sends together with its goodbye (data and FIN in one read): a friend
  who sends "J|name" and hangs up at once never joins or leaves. A second "H|" is told "E|..." but stays connected
  with no role and its name changed, and its next message is read as a role request again.
- Part 3: a relay port already taken ("in use" or "access denied") is silent: the game starts without the relay and
  the hosting box gets no reports. Another error fails the launch.
- Part 3: Stop hosting (killing the C++ launcher, now the stop signal) leaves the game running and its play time is
  never added; the play count was added at start.
- Part 3: the web server takes the first character of the path as the '/' whatever it is, so a request line without
  a target looks for "ndex.html".
- Part 3: one behaviour changes with the move: the C++ `--launch --wait` outlived the window, so closing the window
  while a game ran kept the relay up and still counted the time. In the app, closing it ends the relay and that
  session's time isn't added. Discord's presence now comes from the app's process (one per running game) instead of
  one launcher process each.
- Part 3: each `--launch` restarted launcher.log (opened "wb", others denied writing). The Rust launch restarts it too
  and appends line by line, so the C++ launcher (mods, lobby) can still open it.
- Part 3: players start with CREATE_NO_WINDOW, as they effectively did under the hidden C++ launcher; without it the
  lab's console stand-in player opened a window that took the focus and minimized the window in TV mode.
- Part 3: closing the app while a game runs now ends that session's relay and its play time (the C++ child used to
  outlive the window). Only games that host through the launcher's own relay are affected.
- Part 3: the lab's TV on-screen keyboard check failed once in a full run and passed in the next two; watch it.
- Part 4: SHA-256 is checked only when it is known. A player from a fixed URL without `sha256`, a release asset with no
  digest (or one that isn't "sha256:"), and a catalog entry without `sha256` are installed unchecked; such a catalog
  file is downloaded again every time. Kept, so both launchers accept the same configs and catalogs.
- Part 4: WinHTTP ends a read without an error when the server hangs up before its Content-Length, so a dropped
  download is caught only by the expected size or the checksum ("The download is incomplete (7 bytes of 14 bytes; ...)"
  or "doesn't match its published checksum"). With neither, a cut-off catalog file would be saved as the game.
- Part 4: "The download is incomplete" is also what a file larger than the catalog's `size` reads ("14 bytes of 3 bytes").
- Part 4: unzipping is Windows' tar.exe, as in C++. It refuses entries that climb out with ".." and links (the whole
  install fails: "Unpacking the download failed", the old copy stays), and strips a leading "/" from absolute entries.
  A tar still running after two minutes counts as failed while it keeps running.
- Part 4: "Cannot replace <folder>" read Windows' error after the work folder was removed, so its code could be the
  removal's; the Rust message gives the move's own code.
- Part 4: updating a player asks GitHub twice (the check, then the install looks the release up again): two of the 60
  unsigned requests an hour.
- Part 4: a `gamesDir` in settings.json isn't expanded or made absolute, so "${LAUNCHER_DIR}\x" is a folder of that
  name under the current folder; both launchers download there. With a `gamesDir`, the player lookups are logged twice
  (the library refreshes again).
- Part 4: each of these commands restarts launcher.log, as each C++ process did; the window's player check at start
  and a game download while a game runs restart it too (they did when the C++ launcher ran them).
