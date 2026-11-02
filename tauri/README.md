# Tauri rewrite (prototype)

The launcher rebuilt as a Tauri 2 app: Rust back end, React + TypeScript front end. The C++ launcher in `src/`
stays the one that ships until this one has parity.

Prototype status: the same layout as the C++ launcher's Quiver-style one, and it works: the C++ launcher exe is its
engine until each part is rewritten in Rust (`src-tauri/src/engine.rs`).

- The library, every action's state, the UI texts and your stats come from `--list --json`; the PC mods from
  `--mods --json`; open rooms from `--rooms <id> --json`.
- Play, Host (players and room code or IP), Join (room code or address), the rooms' Join buttons, and mod
  Install / Update / Launch / Repair / Uninstall run `--launch`, `--join`, `--address` and the mod commands.
- Favorites are written to the launcher's own `settings.json`, so both windows agree.

It finds the exe through `FL_LAUNCHER_EXE`, or next to its own exe (the shipped layout):

```bash
set FL_LAUNCHER_EXE=C:\path\to\build\Debug\MultiplayerEverythingLauncher.exe
npx tauri dev
```

In a plain browser (`npm run dev`) the bundled `src/launcher.data.json` stands in, so the UI can be worked on without
the exe. Settings, the lobby window and the hosting box still live in the C++ launcher.

- A top bar (Library, Rooms, Settings); the library is full width: Continue playing, a toolbar of chip menus
  (category, order), search and Compact, then poster cards with Play on hover.
- A card opens its game's page (sliding in): backdrop header, icon, Verified pill, Overview / Details / Releases
  tabs, and the play panel as the right column. Esc or Back returns.
- Key hints in the status bar; with a game controller (the browser's Gamepad API) the D-pad moves between
  controls, A presses, B goes back, Y searches, LB and RB switch Library and Rooms, and the hints show its buttons.
- TV mode (`src/Tv.tsx`) for a controller on the couch: Start, F11, the sidebar's TV button or Settings switch it.
  Full screen; Home shows the game you're on large with its player slots over one row of your games, and LB/RB
  (PgUp/PgDn) move between Home, Games, Rooms and Settings. A plays, Y opens the game's page, X stars it, B goes
  back. A game's page, the lobby and Settings are the window's own, enlarged. It reopens in TV mode if left on.
  The idea comes from RecompLauncher's Big Picture mode; none of its code is used.

```bash
cd tauri
npm install
npm run dev          # the UI alone, in a browser at http://localhost:1420
npx tauri dev        # the real window
npx tauri build --no-bundle   # src-tauri/target/release/app.exe
```

Ideas are borrowed from Quiver Launcher (rounded cards, art first, one restrained accent). Its source is BSL 1.1,
so none of its code is used.

## Testing

```bat
npm test        :: the button logic (src/model.ts), in a second
npm run lab     :: the test lab: builds the window and clicks through every feature
```

The lab (`tests/lab/lab.mjs`) drives the real window through `tauri-driver` against a sandbox in `%TEMP%`. The
sandbox has its own `launcher.json` and data folder, a fake Flash player that records its arguments
(`launcher_tests.exe --fake-player`) and a fake PC game folder. Local servers stand in for the catalog, mods-info
and the room list, and pretend friends speak the relay protocol. Real games and the real launcher's settings are
never touched.

It checks:
- the library, search, sort and the categories;
- rooms, Host and Join (by code and by IP), Play solo, Favorite and Settings;
- minimize and restore, the hosting box (a friend joining, Copy, Stop hosting);
- Download and Locate;
- mod install, repair and uninstall: the game folder must end byte-identical;
- the lobby (host, chat, leave).

A failed check saves a screenshot and the page's errors next to the sandbox.

It needs:
- `cargo install tauri-driver --locked`;
- Microsoft Edge WebDriver matching the installed WebView2, either in `MSEDGEDRIVER` or in
  `%LOCALAPPDATA%\fl-lab\edgedriver-<version>\msedgedriver.exe`;
- the C++ launcher's Debug build, in `FL_LAUNCHER_EXE` (default: `..\old\build\Debug`, made by `..\old\build.ps1 -Config Debug`).

`--no-build` reuses the last build, `--only coop,mods` runs some scenarios, and `--keep` keeps the sandbox.
`FL_LAUNCHER_CONFIG` (another launcher.json for every call to the C++ launcher) is what points the window at the
sandbox, and it works the same for clicking through one by hand.

## Release build

```powershell
.\scripts\release.ps1
```

It builds and tests the C++ launcher (Release), copies it to `src-tauri/engine/`, runs the quick tests, and builds
the installer with `src-tauri/tauri.release.json`. The installer puts `MultiplayerEverything.exe` next to
`MultiplayerEverythingLauncher.exe`, which is where the window looks for the launcher, and installs WebView2 if
the PC lacks it. It is for the current user only, so no admin prompt. Nothing is published.

The installer has one screen: opening it is the choice to install, so it runs in Tauri's passive mode, shows
the app's icon, name, what it is doing and a progress bar in the launcher's colors, then opens the app on its
first-run setup. Run again, it writes the files over the installed ones; settings and games stay. Our copy of
Tauri's installer script is `src-tauri/installer/installer.nsi` (its changes are marked "Multiplayer
Everything:"; carry them over when updating tauri-cli) and the screen is `src-tauri/installer/ui.nsh`.

Not done yet: signing, and updates for the window itself. Those need a signing key and a release feed.
