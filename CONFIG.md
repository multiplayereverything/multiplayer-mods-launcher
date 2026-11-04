# launcher.json reference

`launcher.json` sits next to `MultiplayerEverythingLauncher.exe` (or pass `--config <file>`).
Without one, the exe uses the copy built into it at compile time; the same goes for
cover images, so the exe works on its own.
It is JSON with comments (`//`, `/* */`) and trailing commas allowed. Unknown
keys are reported as warnings by `MultiplayerEverythingLauncher.exe --validate`, so typos show
up. Keys starting with `_` are ignored (use them for notes).

## Variables

Any path or argument can use these:

| Variable | Value |
|---|---|
| `${LAUNCHER_DIR}` | Folder of `launcher.json` |
| `${GAMES_DIR}` | `paths.games` |
| `${PLAYERS_DIR}` | `paths.players` |
| `${DATA_DIR}` | `paths.data` (settings and log) |
| `${FLASHPOINT}` | First `paths.flashpoint` folder that contains `FPSoftware`. A candidate path using it is skipped when Flashpoint isn't installed. |
| `${GAME_ID}`, `${GAME_NAME}` | The game being started |
| `${SWF}`, `${SWF_DIR}` | The chosen game file and its folder (player arguments only) |
| `${PORT}` | The action's relay port |
| `${TITLE}`, `${SUBTITLE}` | `branding.title` / `branding.subtitle` |
| `${PLAYER_NAME}` | The name set in Settings (`flashvars` only) |
| `${URL}`, `${FILE}`, `${FILE_DIR}` | HTML5 actions: the page to open, and the action's file and its folder |
| `${env:NAME}` | Environment variable, e.g. `${env:ProgramFiles}` |
| `${folder:NAME}` | Desktop, Documents, LocalAppData, RoamingAppData, ProgramFiles, Profile, Temp... |

## Top level

| Key | Default | |
|---|---|---|
| `schemaVersion` | `1` | |
| `branding` | | See below |
| `paths` | | See below |
| `behavior` | | See below |
| `downloads` | | See below |
| `rooms` | | See below |
| `discord` | | `{ "clientId": "<application id>" }`: Discord shows "Playing <game>" and the room code. Empty or missing: off. |
| `updates` | | `{ "github": "owner/repo", "asset": "MultiplayerEverythingLauncher.exe" }`: the launcher installs newer releases of itself (checked against GitHub's SHA-256; a release without one is refused). Missing: no updates. With `"app"` (the app's installer in the same releases, a file-name pattern) and `"appExe"` (where it installs) the C++ window hands over: started on its own it installs the app once, then only opens it. Give both or neither. |
| `players` | required | Flash players, tried in this order |
| `categories` | `[]` | Sidebar entries: `{ "id", "name", "icon", "group" }`; `icon` is a Segoe Fluent Icons code, e.g. `"E774"` (globe). Entries with the same `group` sit together and a line separates groups (the shipped config has *PC mods* and *Flash mods* in one, the kinds of game in another). |
| `games` | required | The library |
| `text` | | Overrides for any UI string (run `--validate` to catch unknown keys) |

### branding

| Key | Default |
|---|---|
| `title` | `"Multiplayer Everything"` |
| `website` | the launcher's GitHub page: opened by *Project page* in Settings |
| `subtitle` | `"Multiplayer Everything Launcher"` |
| `windowTitle` | `"${TITLE} ${SUBTITLE}"` |
| `theme` | `"dark"` (or `"light"`) |
| `colors` | Overrides per role: `background`, `surface`, `surfaceHover`, `sidebar`, `sidebarText`, `sidebarTextMuted`, `primary`, `primaryHover`, `onPrimary`, `text`, `textMuted`, `border`, `focus`, `success`, `warning`, `error`, `online` (`"#RRGGBB"`). Keep text at 4.5:1 contrast or better. |
| `font`, `fontSize` | `"Segoe UI"`, `10` (points): body text |
| `displayFont` | `"Bahnschrift"`: titles, card names, buttons (falls back to `font`) |
| `playerColors` | `["#E5484D", "#3E7BFA", "#2EBD6B", "#F5B83D"]`: P1-P4, used only to mark player slots |
| `window` | `{ "width": 1180, "height": 720, "minWidth": 720, "minHeight": 520 }` (96-DPI units) |

### paths

| Key | Default |
|---|---|
| `games` | `"${LAUNCHER_DIR}\\games"` |
| `players` | `"${LAUNCHER_DIR}\\players"` |
| `data` | `"${env:LOCALAPPDATA}\\MultiplayerEverything\\MultiplayerEverythingLauncher"` |
| `flashpoint` | `["C:\\Flashpoint", "D:\\Flashpoint", "${folder:Profile}\\Flashpoint"]` |

The shipped `launcher.json` points `games` and `players` at `${folder:Documents}\\MultiplayerEverythingLauncher\\games` and `\\players`; the launcher creates both folders when it starts.

### behavior

| Key | Default | |
|---|---|---|
| `minimizeOnLaunch` | `false` | Flash slows down minimized windows, but the launcher's own window doesn't matter; turn on if you like a clean desktop. |
| `stopRelayWithGame` | `true` | Stop the relay when the hosting game closes. |
| `quickExitSeconds` | `4` | A player that exits with an error faster than this is reported. |

### rooms

| Key | Default | |
|---|---|---|
| `servers` | none | mods-info `servers.json`: the first server with status `online` is asked for rooms (the mods use the same file) |
| `server` | none | `host:port` used when `servers.json` can't be read |
| `refreshSeconds` | `30` | How often the room list refreshes |

A game takes part with `"online": { "id": "<mod id on the server>", "join": "<action id>" }`.
When the server answers that it doesn't know the id, the game shows *Not on the server
yet* instead of a room count.
*Join* runs that action and passes the room code as `join=<code>` through the
player's `flashvarArg`.

### downloads

| Key | Default | |
|---|---|---|
| `site` | `https://www.multiplayereverything.com/games.json` | The website's list of games: each one's stage, quality grade, version, how it connects and what stays in sync, shown on the cards and in the details panel. Empty: the launcher never asks the website. Must be `https://`. |
| `modsInfo` | folder of `rooms.servers` | Folder of mods-info: `mods/<id>.json` gives each PC mod's newest version and download. Must be `https://`. |
| `catalog` | none | Address of a game catalog (`catalog.json` in the flash-games repository). Games with missing files then offer *Download game files*. Must be `https://`. |

The catalog lists files as `{ "name", "game", "path", "size", "sha256" }`: `game` is
the game `id` here, `name` the file name saved in `paths.games` (it must match one
of the game's `${GAMES_DIR}` candidates), and `path` is relative to the catalog's
folder. Every download is checked against its SHA-256 and saved under a temporary
name until the check passes. The launcher keeps the last catalog it read in
`paths.data` and refreshes it in the background when it starts.

## players[]

| Key | |
|---|---|
| `id` | Letters, digits, `-`, `_`, `.` |
| `name` | Shown in the UI |
| `website` | Opened by the *Get <player>* button when the player can't be installed |
| `install` | Optional: how to install the player when it's missing (see below) |
| `paths` | Candidates, first existing wins. Absolute paths may use `*` in the file name (the highest version wins); a bare name like `ruffle.exe` is searched on PATH. |
| `args` | Arguments before the game's own. `${SWF}` is added at the end unless an argument mentions it. |
| `flashvarArg` | How the player passes a value into the game, e.g. `"-P${KEY}=${VALUE}"` for Ruffle. Players without it can't receive room codes. |
| `kind` | `"flash"` (default) or `"browser"` (Edge, Chrome: HTML5 games, `${URL}` in `args`) |
| `tv` | Optional: what a game started from TV mode (`--launch --tv`, the Tauri window's full-screen controller view) adds: `args` at the end of the command line and `env`, variables set for the player only, e.g. `{ "args": ["--fullscreen"], "env": { "RUFFLE_TOGETHER_TV": "1" } }`. Put anything an older player would refuse in `env`: unknown variables are ignored, unknown options stop it. |

### players[].install

The launcher offers to install a missing player (on first run, and with the
*Install <player>* button or `--install-player <id>`). It unpacks the archive
with Windows' own `tar.exe`, so `.zip` files work.

| Key | |
|---|---|
| `github` | `"owner/repo"`: use the latest stable release of this repository |
| `asset` | File name pattern of the release file, e.g. `"*windows-x86_64.zip"` |
| `url`, `sha256` | Instead of `github`: a fixed `https://` download and its optional SHA-256 |
| `dir` | Folder to unpack into (replaced on reinstall; the old copy is kept until the new one is in place) |
| `exe` | The player inside the archive, e.g. `"ruffle.exe"` (found at the top or one folder down) |
| `api` | GitHub API address, default `https://api.github.com` (tests point it at a local server; also accepted in `updates`) |

GitHub releases publish a SHA-256 for each file; the download is refused when it
doesn't match.

## games[]

| Key | |
|---|---|
| `id` | Unique; also the key in settings |
| `name`, `description` | Shown in the details panel (`controls` is accepted but no longer shown) |
| `mod` | Makes this a **PC game mod** instead of a Flash game: see "PC game mods" below. Such a game has no `actions` (the launcher makes Install, Update, Launch, Repair and Uninstall itself). |
| `players` | Player-count text, e.g. `"1-4 players"` |
| `maxPlayers` | Player slots to show; default: the highest number in `players` |
| `guide` | The text of the game's **Guide** page: `{ "needs": [...], "steps": [...], "tips": [...], "keys": [ { "key": "F5", "does": "..." } ], "links": [ { "label": "...", "url": "https://..." } ] }`. Every part is optional: what you leave out is filled in from a default for that kind of game (PC mod, Flash game with the lobby, Flash game with the F9 panel, one-keyboard play, coming soon), so a game needs no `guide` at all. In any text, `${NAME}` is the mod's name and `${GAME}` the game it goes into. `links` are `https://` only, at most four, shown as buttons at the bottom of the page. A GitHub link for the mod is added from the website's data when there is one. |
| `site` | The game's id on the website when it isn't the same as `id` (`"baldi"` for `baldi-together`). Without it the launcher matches by `id`, then by the game's id on the server (`online.id`). |
| `online` | `{ "id", "join" }`: the game's id on the central server and the action that joins a room. Gives the game a *Rooms* tab and *Quick play*. Add `"lobby": { "version": "1.3", "port": 7790 }` for a mod that uses the launcher's lobby and localhost bridge: *Play online* then opens the lobby window (host or join, ready-up, start), and the game gets `room`, `role`, `slot`, `bridgePort`, `sessionToken` and `seed` FlashVars. See [docs/LOBBY_BRIDGE.md](docs/LOBBY_BRIDGE.md). |
| `categories` | Category ids |
| `color`, `colorEnd` | Gradient of the generated cover art |
| `image` | Optional cover image (PNG/JPG), relative to `launcher.json` or absolute. Images in `launcher/covers` are built into the exe. |
| `imageFocus` | `0`-`1`, default `0.5`: which part of the cover stays in view when it is cropped (0 top, 1 bottom) |
| `actions` | Buttons; the first shown is the primary one (Enter / double-click) |
| `comingSoon` | `true`: listed with *Coming soon* and no buttons; `actions` may be left out |
| `settings` | The game's settings (a checkbox or a dropdown each, up to four): see "Game settings" below. The catalog's list for the game wins; this one is the fallback. |

### actions[]

| Key | |
|---|---|
| `id`, `label` | `label` is the button text |
| `description` | Second line on the button |
| `hint` | Shown in the status bar after starting (e.g. "press F9, click HOST") |
| `swf` | Candidate files (string or list); `http(s)://` URLs are passed through as-is |
| `args` | Extra player arguments for this action. `${PLAYER_NAME}`, `${SERVER}` (the central server), `${CODE}` (a room code) and `${ADDRESS}` (a `host:port` copied to the clipboard; typed without a port, it gets the game's `online.lobby.port`, 7790 by default) are filled in when the game starts |
| `joinArgs` | Used instead of `args` when a room code comes with the launch (the Rooms list, Join by code) |
| `link` | An `https://` address the button opens in the browser; no player or file is needed. For a mod made by someone else that has its own setup (put the game in the `community` category) |
| `players` | Allowed player ids, in order; default all. `["ruffle"]` keeps a game on Ruffle. |
| `overrides` | Per player id: `{ "swf": [...], "args": [...] }` replacing the action's own |
| `relay` | `{ "port": 7777 }` makes this a host action: the built-in relay starts first |
| `flashvars` | Values passed into the game through the player's `flashvarArg`, e.g. `{ "online": "1", "name": "${PLAYER_NAME}" }`. Empty values are left out; *Join* adds `join=<code>`, *Quick play* with no room open adds `host=1`. |
| `more` | `true`: listed under *Other ways to connect* (direct IP, LAN) |
| `offline` | `true`: solo or one-keyboard play, hidden unless Settings shows offline play (off by default). Give the game an online action too, or it shows no buttons. |
| `icon` | A Segoe Fluent Icons code drawn before the label, e.g. `"E774"` (globe) |
| `edition` | `"original"`: the unmodded game, shown only when Settings asks for originals |

HTML5 actions open a page in a browser player (`players[].kind` = `"browser"`)
instead of a `.swf`: `page` (a local `.html`, served by the launcher on 127.0.0.1,
or a URL), `file` (files the game needs, like `swf`), `server` (a program started
hidden first, such as a co-op server: `name`, `website`, `exe`, `args`, `cwd`,
`url` it answers on, and `share`/`lan` patterns for the links to show friends), or
`joinLink: true` (open the http(s) link on the clipboard). See
`docs/html5-example.jsonc`.

Example, a new local multiplayer game:

```jsonc
{
  "id": "mygame",
  "name": "My Game",
  "players": "2 players",
  "categories": ["local"],
  "color": "#3CCB7F", "colorEnd": "#1B3A6B",
  "description": "Two players race to the exit.",
  "actions": [
    { "id": "play", "label": "Play", "swf": "${GAMES_DIR}\\mygame.swf" }
  ]
}
```


## Game settings

A game can offer settings the player changes in the launcher: a checkbox or a dropdown under the game's
buttons. The launcher adds the setting's `args` to the command line. The list for a game comes from the
**flash-games catalog**, so a game can add or change a setting without a launcher release: `catalog.json`
has a top-level `"settings"` object, game id to list. A game the catalog doesn't list uses `settings` from
its `launcher.json` entry. The player's choices are kept per game in `settings.json`.

```json
"settings": {
  "dadnme": [
    { "id": "friendlyfire", "type": "checkbox", "scope": "host", "label": "Friendly fire",
      "since": "1.0.3.7", "args": ["--coop-option", "friendlyfire=1"] },
    { "id": "difficulty", "type": "choice", "scope": "all", "label": "Difficulty", "default": "normal",
      "choices": [ { "id": "easy",   "label": "Easy",   "args": ["--coop-option", "difficulty=0"] },
                   { "id": "normal", "label": "Normal", "args": [] } ] }
  ]
}
```

| Key | Meaning |
|---|---|
| `id`, `label` | Required. The id is what is saved; the label is shown. |
| `type` | `"checkbox"` (default; `args` are added when ticked) or `"choice"` (the picked choice's `args`; at least two `choices`, each with `id`, `label`, `args`) |
| `scope` | Which actions get the args: `"host"` (default; the ones that start a room), `"join"`, or `"all"` |
| `since` | The first Ruffle Together release that knows the option. Before it the setting is disabled, because an unknown option stops the player from starting. |
| `default` | A checkbox: `"on"` starts ticked. A choice: the default choice's id (else the first). |

Entries that don't parse are logged and left out; at most four are used.

## Relay protocol

Host actions start a relay compatible with the mods' Python relays: NUL-terminated
UTF-8 messages; `H|name` makes the host, `J|name` a guest; host messages go to
every guest unchanged, guest messages reach the host as `G|<id>|<msg>`; the relay
sends `W|<id>` welcome, `J|<id>|<name>` / `L|<id>` to the host and `X` to guests
when the host leaves; `D|text` lines go to the log. It answers Flash's
`<policy-file-request/>` on the game port and on 843 when free.

## PC game mods

A game with a `mod` object installs files into another game's folder. The shape follows
the installer template (`installer.json`), so a mod installed by its stand-alone setup is
adopted: use the same `mod.id` and `install` layout and the launcher sees it as installed.

```jsonc
{
  "id": "spooky-together", "name": "Spooky Together",
  "online": { "id": "spooky" },                      // optional: its rooms on the server
  "mod": {
    "id": "spooky-together",                         // default: the game's id; keep a setup's id to adopt its installs
    "game": {
      "name": "Spooky's Jump Scare Mansion", "executable": "SpookyUnity.exe", "steamAppId": "577690",
      "validationFiles": ["${GAME_EXE}", "SpookyUnity_Data"],   // what only the real game folder has
      "processNames": ["SpookyUnity.exe"],                       // the game must be closed to install
      "detection": [ { "type": "steam", "appId": "577690", "installDir": "..." },
                     { "type": "path", "path": "${folder:ProgramFilesX86}\\Steam\\..." } ]
    },
    "install": {
      "targetDir": "${GAME_DIR}\\BepInEx\\plugins\\SpookyTogether",
      "preserve": ["config/*.ini"],                  // user-edited files kept on update and uninstall
      "uninstallCleanup": ["BepInEx/cache/**"]       // files the game creates; removed unless they were there before
    },
    "components": [
      { "id": "loader", "name": "BepInEx", "source": "loader", "target": "${GAME_DIR}", "required": true },
      { "id": "mod", "name": "Spooky Together", "source": "mod", "required": true }
    ],
    "launch": { "uri": "steam://rungameid/${STEAM_APP_ID}", "hint": "Press F5 in the game." },
    "package": { "modsInfo": "spooky" }              // or "url" (+ "sha256") to pin a download
  }
}
```

| Key | |
|---|---|
| `game.detection` | Rules tried in order: `steam` (`appId`, `installDir`), `epic` (`appName`), `registry` (`key`, `value`, `subPath`), `path`. A folder the player chooses wins when it holds the `validationFiles`. |
| `install.targetDir` | Where the mod's own files go (default `${GAME_DIR}\Mods\${MOD_ID}`). Only the game folder and `allowedRoots` can be written. |
| `install.stateDir` | The install record and backups (default `${INSTALL_DIR}\.installer`). |
| `components[]` | `source` is a folder inside the package, `target` where it goes (default `${INSTALL_DIR}`). `required` ones are always installed. |
| `launch` | `uri` is opened (a `steam://` address), else `executable` with `arguments`. |
| `package.github`, `asset` | The mod's GitHub repository (`"owner/repo"`) and a file name pattern (`"MiSideTogether-*.zip"`): the launcher installs the newest release (pre-releases count) and takes the SHA-256 GitHub publishes for the file. |
| `package.modsInfo` | The mod's id in mods-info. `latest_version`, `download_url` and `download_sha256` come from `mods/<id>.json`. |
| `package.url`, `sha256`, `version` | A package pinned in `launcher.json` instead. `version` is what the window shows and compares: change it with the url, so a new pin counts as an update. The download may be a `.zip` or a single file (a `.dll`), placed in the package folder under its own name; it is checked against `sha256` when it is downloaded, and only then. For a community mod see [docs/ADDING_A_MOD.md](docs/ADDING_A_MOD.md#community-mods-made-by-someone-else). |
| `components[].targetName` | Installs a single-file `source` under another file name (`coop_eax.dll` as `eax.dll`). |
| `components[].sourceGame` | Instead of `source`: a copy of a file the game folder already has, the first of this list that exists, installed under `targetName` (or its own name). For a loader that wraps one of the game's own files and needs the original beside it: `["eax_orig.dll", "eax.dll"]` with `"targetName": "eax_orig.dll"` keeps an update from copying the wrapper over the copy. List it before the component that replaces the original. |
| `package.extras[]` | A second download unpacked beside the package under `dir` (components then use `"<dir>/..."` as their `source`), usually the mod loader from its own release: `{ "dir": "loader", "github": "BepInEx/BepInEx", "tag": "v5.4.23.5", "asset": "BepInEx_win_x64_5.4.23.5.zip" }`, or a `url`. Its checksum is GitHub's, or `sha256` you pin. |
| `actions[]` on a mod game | Only `link` actions: the mod's own page, shown after the launcher's Install and Launch buttons in the amber link style. |
| `install.requires[]` | `{ "path", "message" }`: a file or folder the game folder must already have (a loader the launcher can't download with a checksum). `path` is relative to the game folder: `BepInEx\core`, never `${GAME_DIR}\...`. While it's missing, Install is off and the message is shown. |

A package is a `.zip` (or one file) laid out as the `components[].source` folders expect. It is downloaded
only over `https://`, and **only installed when its SHA-256 is known**: from `download_sha256`,
from the `sha256` pin, or from GitHub for a release asset. See [docs/MOD_PACKAGES.md](docs/MOD_PACKAGES.md).
