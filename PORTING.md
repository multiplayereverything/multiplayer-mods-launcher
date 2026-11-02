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
| 2 | Open rooms and the central server's address and ping | `rooms` | `--rooms all --json`, `--rooms <id>`, ping | |
| 3 | Starting a game: the command line, host options, TV options, the relay, the HTML5 web server, Discord, play time | `library`, `process`, `relay`, `webserver`, `discord`, `game_options`, `launcher_headless` | `--launch ... --wait [--tv] [--coop-save]` | |
| 4 | Players and game files: GitHub releases, downloads with SHA-256, unzip, updates with progress | `install` | `--install-player`, `--update-players [--check]`, `--download-game` | |
| 5 | The lobby and the game bridge | `lobby`, `lobby_cli` | `--lobby-session` | |
| 6 | The PC-mod engine: detection, the install record, the journaled install / update / repair / uninstall, packages | `mod/*` | `--mods --json`, `--install-mod`, `--repair-mod`, `--uninstall-mod` | |
| 7 | Community mods approved into mods-info (`community/<id>.json`); the C++ draft of the rules is on branch `wip/community-mods-cpp` | (new) | | |
| 8 | The app updating itself: Tauri's updater with a signature from an offline key | `install` (`updateSelf`) | `--update` | |
| 9 | Delete `old/`, ship the app alone | | | |

The PC-mod engine is late on purpose: it writes into players' game folders, so it moves only once its tests
(`old/tests/test_mod.cpp` and the "PC game mods" part of `old/tests/e2e.ps1`) are ported and pass.

## Found while porting (kept as the C++ does it, for later)

- Part 1: the window never downloads the website's games.json; the library only reads the copy the C++ window
  saved (data\site.json). The Rust side should fetch it, at most once an hour, as the C++ window did.
- Part 1: with no "minimize" in settings.json, `user.minimize` reads true even when launcher.json's
  `behavior.minimizeOnLaunch` is false.
- Part 1: the launcher version comes from old/CMakeLists.txt (src-tauri/build.rs); give it a home before old/ goes.
- Part 1: games added from the website are not validated (mergeSite runs after validation).
