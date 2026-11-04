# Mods from mods-info: community and team records

How a mod reaches the launcher without a launcher release. mods-info has two folders of records:

- `community/`: mods made by someone else, approved by the owner (the developer SDK). Strict, pinned, credited.
- `team/`: Multiplayer Everything's own mods and Flash games. Each record is the game's launcher.json `games[]` entry
  with a few record keys added, from a `multiplayereverything` repository only; a PC mod follows its newest release.
  See [Team records](#team-records) below.

The launcher is a fixed set of building blocks (finding the game, what it needs first, what goes where, what is
skipped or kept, how it starts, the card); a record only combines them. A mod that needs something no block covers
needs a new block: in the launcher first (both engines, with tests and a lab check), then in the website's checker,
schema and form, then a launcher release. Records that use it say `"minLauncher"`.

For community mods: how a mod made by someone else reaches the launcher without a launcher release. The rules for deciding whether to
list one at all (ask the owner, credit the author, review by hand) are in [ADDING_A_MOD.md](ADDING_A_MOD.md).

## The path

1. The developer adds their mod on the website's developers page (name, description, players, picture) and puts a
   manifest, `multiplayereverything.json`, on their GitHub release next to the mod file. The manifest's format is on
   the website's "Developers, Launcher" page.
2. At approval the website checks the manifest, pins that release (tag, file, SHA-256) and writes the record below
   to [mods-info](https://github.com/multiplayereverything/mods-info) when the owner offers it in the dashboard.
   Taking the project down in the dashboard removes it again.
3. Only the owner's offer publishes it. Within an hour every launcher has it: a card in Community mods, named after
   the game, crediting the author and the repository, with Install, Update, Repair, Uninstall and Launch.

A newer release of the mod reaches nobody until it goes through steps 2 and 3 again.

## In mods-info

```
community/index.json      { "mods": ["lab-coop", ...] }   lowercase letters, digits and '-', at most 200
community/<id>.json       one record per mod
```

```json
{
  "id": "lab-coop",
  "repo": "someone/lab-coop",
  "tag": "v1.2.0",
  "version": "1.2.0",
  "asset": "LabCoop-1.2.0.zip",
  "url": "https://github.com/someone/lab-coop/releases/download/v1.2.0/LabCoop-1.2.0.zip",
  "sha256": "<64 lower-case hex digits of the asset>",
  "manifest_sha256": "<of multiplayereverything.json>",
  "author": "someone",
  "authorUrl": "https://github.com/someone",
  "approved_at": "2026-11-03",
  "approved_by": "JessniX",
  "mod": { "...": "the manifest's mod object, as approved" }
}
```

- `url` must be exactly `https://github.com/<repo>/releases/download/<tag>/<asset>`.
- `mod` is a launcher.json `mod` recipe limited to what can't reach outside the game folder:
  - `game`: `name`, `executable` (a file name), `steamAppId` (digits), `validationFiles`, `processNames`, and
    `detection` with only `steam` and `path` rules; a `steam` rule may have `subPath`, a folder inside the game's
    Steam folder (`Atlas\Binaries\Win64`), never `..`, a drive or a variable;
  - `install`: `targetDir` inside `${GAME_DIR}`, `stateDir` inside `${GAME_DIR}` or `${INSTALL_DIR}` (where the install
    record is kept; the default is `${INSTALL_DIR}\.installer`), `preserve`, `requires` (each `path` relative to the game folder, like `BepInEx\core`, without `${GAME_DIR}`), `uninstallCleanup`;
  - `install` may also have `uninstallCleanupWith`: the `id` of one of the components (the loader). Then
    `uninstallCleanup` is deleted only when that component is uninstalled too;
  - `components`: `id`, `name`, `source` (inside the package), `target` inside `${GAME_DIR}` or `${INSTALL_DIR}`,
    `targetName`, `required`, and `skipIfPresent`: one to ten paths in the game folder (`BepInEx/core`); the component
    is left out when the game has one already (a loader another mod installed);
  - `launch`: `uri` (`steam://rungameid/<app id>` or `${STEAM_APP_ID}`) or `executable`
    (`${GAME_DIR}\<program>.exe`, a program in the game's folder, no `..`, `$`, `:` or `/` after it), one of the two,
    and `hint`;
  - `package`: only `extras`, one to three loader downloads from the mod's own releases, each
    `{ "dir", "tag", "asset", "url", "sha256" }`: `dir` the folder name the loader unpacks into inside the package
    (`loader`, so components use `loader/BepInEx/core`), `url` exactly
    `https://github.com/<repo>/releases/download/<tag>/<asset>`, `sha256` pinned at approval like the mod's own.
- Any other key, anywhere, refuses the whole record, so a record can never do more than it shows: no `package.github`
  or other download, no `allowedRoots`, no `sourceGame`, no registry or Epic detection.
- `minLauncher` (any record): the oldest launcher that understands it, such as `"1.0.42"`. An older launcher shows a
  card named after the game that says to update, with nothing to press, and reads nothing else of the record.
- After those rules the mod gets every check a launcher.json mod gets. An id launcher.json already has is refused.

### Records the team writes by hand

The website's form writes only the record above. A record the team writes by hand (a mod moved out of launcher.json)
may also carry what launcher.json gives a card, and may be a card that only links to the mod's page:

- `description` and `players` (text, at most 600 characters), `maxPlayers`, `color` and `colorEnd` (`#RRGGBB`), and
  `guide` with `needs`, `steps` and `tips` (up to 12 texts each), `keys` (`{ "key", "does" }`) and `links`
  (`{ "label", "url" }`, https only, at most 3). The launcher puts its credit tip first and the repository's link last,
  so a record leaves both out.
- `"kind": "link"`: a mod with its own installer, or none yet. Instead of `tag`, `version`, `asset`, `url`, `sha256`
  and `mod` it has `"link": { "game": "MDK2 HD", "url": "https://github.com/<repo>/...", "label": "..." }`; the url
  must be in the record's own repository and `label` defaults to "Open the mod's page". The card installs nothing.
- `kind` is `pc` when left out, or `flash` (a developer's Flash game, below). A kind a launcher doesn't know is left
  out whole, so a later kind never shows half a card in an older launcher.

Our own 7 PC mods and 17 Flash games became team records on 2026-11-04 (mods-info 107816f) and left launcher.json in
1.0.42; `tests/fixtures/team` is the published copy the tests read, kept equal to mods-info by
`the_team_records_in_a_mods_info_checkout_are_the_published_copy`.

MDK2 HD, The Witness, Little Nightmares and Forager came to mods-info this way (2026-11-04), with the ids and pins
they had in launcher.json, so installs, favorites and play stats carry over. Spore stays in launcher.json: it
writes beside its folder (`..\Data`) and keeps the game's own `steam_api.dll` with `sourceGame`.

## Team records

```
team/index.json      { "mods": ["spooky-together", ...] }
team/<id>.json       one of our own mods
```

```json
{
  "id": "spooky-together",
  "repo": "multiplayereverything/spooky-together",
  "minLauncher": "1.0.42",
  "approved_at": "2026-11-04",
  "approved_by": "Multiplayer Everything Team",
  "name": "Spooky Together", "description": "...", "players": "2-4 players (co-op)", "categories": ["pc", "online", "coop"],
  "site": "spooky", "online": { "id": "spooky" }, "color": "#D6264F", "colorEnd": "#2A0A16", "guide": { "...": "..." },
  "mod": { "...": "the launcher.json mod, with package { github, asset, extras }" }
}
```

- Besides `id`, `repo`, `minLauncher`, `approved_at` and `approved_by`, a team record is a launcher.json `games[]`
  entry: `name`, `description`, `players`, `maxPlayers`, `color`, `colorEnd`, `guide`, `categories`, `site`, `online`,
  `image`, `imageFocus`, `settings`, and either `mod` (a PC mod, with every block launcher.json has: `sourceGame`,
  `allowedRoots`, `extras` by tag, any detection, any launch; the launcher makes its buttons) or `actions` (a Flash
  game's buttons), never both. No `kind`; any other key refuses it.
- A Flash game's `actions` (1 to 20) may only have `id`, `label`, `icon`, `description`, `hint`, `swf`, `args`,
  `joinArgs`, `more`, `offline`, `edition`, `flashvars` and `players`: every button starts one of launcher.json's
  players (Ruffle Together) with a game file, never a program, page, server, working folder or link of its own. Every
  `swf` path starts with `${GAMES_DIR}\`, `${FLASHPOINT}\` or `${folder:Desktop}\` (read, never written), with no
  `..`, `$` or `:` after it. Ruffle Together itself stays defined in launcher.json (`players`), and the game files
  still come from the flash-games catalog. Our Flash games' `repo` is `multiplayereverything/flash-games`.
- `repo` must be `multiplayereverything/<name>`. For a PC mod, `mod.package.github` must be that repo: the mod follows its newest
  release, checked against GitHub's SHA-256 at download (no review per version). A pinned `url` must be one of its
  release files. Every `extras` download must come from a `multiplayereverything` repository.
- `categories` may not hold `community`, and must be launcher.json's categories. The card has no "made by" credit.
- Then every check a launcher.json game gets. An id launcher.json already has is refused (launcher.json wins while
  both have it), and team records are read before community ones, so a developer's record can't take a team id.
- The website writes them through a team-only path (not the developer form), for `multiplayereverything`
  repositories only. The launcher session's test `the_team_records_in_a_mods_info_checkout_are_the_games_launcher_json_has`
  (`FL_TEST_COMMUNITY_DIR=<mods-info checkout>`) proves each record makes exactly the game its launcher.json entry did.

## Flash records (1.0.43)

A Flash game no longer needs hand-written buttons. The record says what the game is, and the launcher makes the
buttons every Flash game has (`src-tauri/src/launcher/community.rs`, `flash_actions`): Play online (a relay room for
2), Host a room for 3 to `maxPlayers`, Host with your IP for 2 to `maxPlayers` (port 7790), Join by IP, the offline
button, and for a team record the original game. Their texts are launcher.json's `flash.*` texts. A record never
holds a command line, so it can't start anything but Ruffle Together with its game file and key map.

### A developer's Flash game

```json
{
  "id": "lab-flash-coop",
  "repo": "someone/lab-flash",
  "kind": "flash",
  "minLauncher": "1.0.43",
  "tag": "v2.0",
  "version": "2.0",
  "manifest_sha256": "<the manifest's SHA-256>",
  "author": "someone",
  "approved_at": "2026-11-04",
  "approved_by": "JessniX",
  "flash": {
    "game": "Lab Flash",
    "file": { "asset": "LabFlash_coop.swf", "url": "https://github.com/someone/lab-flash/releases/download/v2.0/LabFlash_coop.swf",
              "sha256": "<64 hex>", "size": 1234 },
    "keys": { "asset": "lab-flash.keys", "url": "https://github.com/someone/lab-flash/releases/download/v2.0/lab-flash.keys",
              "sha256": "<64 hex>" },
    "maxPlayers": 3,
    "offline": { "local": true },
    "ruffle": "1.0.3.7"
  }
}
```

- `kind` is `flash`. Besides the keys every record may have (`id`, `repo`, `author`, `authorUrl`, `approved_at`,
  `approved_by`, `minLauncher`) and the card's text (`description`, `players`, `maxPlayers`, `color`, `colorEnd`,
  `guide`, as above), it has only `tag`, `version`, `manifest_sha256` and `flash`. No `actions`, `categories` or
  `online`: the card is listed under Community mods and Flash mods, and plays online through Ruffle Together's rooms.
- `flash.game`: the game's name, the card's title (at most 80 characters).
- `flash.file`: the game file, a `.swf` of the pinned release: `asset`, `url` (exactly
  `https://github.com/<repo>/releases/download/<tag>/<asset>`), `sha256`, and `size` in bytes (optional, at most
  512 MB). It downloads into `<games folder>\community\<id>\`, checked against its SHA-256 before it is used.
- `flash.keys`: the key map every player gets. Either the name of one Ruffle Together has built in (`gun-mayhem-2`),
  or the release's `.keys` file, pinned the same way as `file` and downloaded next to it. Ruffle Together reads a
  `.keys` file from 1.0.3.7 (`--coop-keys <path>`); the format is in ruffle-coop's docs/ruffle-together.md.
- `flash.maxPlayers`: how many play online, 2 to 6.
- `flash.offline` (optional): the offline button. `{}` is "Play offline" (one player on their own); `"local": true`
  is "Play on one keyboard" (everyone on one PC). `keys`: another built-in map for it (one player's keys), or
  `false` for none. `label` (at most 40 characters) and `description` (at most 120) replace the button's texts.
- `flash.ruffle` (optional): the oldest Ruffle Together the game runs in. A player with an older release the launcher
  installed sees "Needs Ruffle Together 1.0.3.7 or newer", and Play offers the update instead of starting.
- The credit goes first among the tips and the repository's link last, as for a PC mod. An approved update is a new
  record with the new tag and hashes: a file already downloaded with another SHA-256 shows "Update available".
- Older launchers: 1.0.42 shows a card that asks for the update (`minLauncher`); the C++ mod engine skips it quietly.

### Our own Flash games in the short form

A team record may give `flash` instead of `actions`. Its `file` is a path, as `swf` was (`${GAMES_DIR}\`,
`${FLASHPOINT}\` or `${folder:Desktop}\`); its `keys` a built-in map; and it may have `original`, the unmodded game:
`{ "files": [one to eight paths], "description": "..." }` ("Play the original"). The other keys are a team record's
(`settings`, `online`, `image` and the rest stay as they are).

`scripts/flash-records-short.py <team folder> <out folder>` rewrites the published records this way, and
`tests/fixtures/team-short` is its output for our 17 games. The test
`flash_team_records_in_the_short_form_make_the_same_buttons` proves each makes the same buttons with the same command
lines, card and host options as its published record. Bad Ice-Cream keeps its own buttons (its online ones pass no key
map). Publish them only once most players have 1.0.43: a record with `"minLauncher": "1.0.43"` is an update card in
1.0.42.

## In the launcher

- The app (`src-tauri/src/launcher/community.rs`) asks mods-info at start and then hourly, using the copies it saved
  (`team.json` and `community.json` in the data folder) for an hour; Ctrl+F5 asks at once. A mods-info without the
  folder (404) means none. Any other failure keeps the last good copy.
- The C++ mod commands (`old/src/core/community.cpp`, until PORTING.md part 6) read the same saved copy and check
  each record again, so Install, Repair, Uninstall and Launch know the same mods as the window.
- The card's description, players and picture come from the record, else from the website's `/api/projects` or
  `games.json` entry with the same id. A project the website lists
  that has no approved record shows as a Community card that says it can't be installed yet.
- The download is checked against the pinned SHA-256 before anything is installed, as for every mod.
- `FL_NO_COMMUNITY` leaves community and team mods out (both sides). The lab's `communitymods` scenario installs one
  from a stand-in mods-info and github.com and checks Uninstall leaves the game folder byte-identical; `teammods`
  does the same for a team record that follows its newest release and brings BepInEx, plays a Flash game from a team
  record (and leaves out one whose button runs a program), and shows a record for a newer launcher. A Flash game from
  a record starts, lists its rooms and downloads its files like one in launcher.json (launch.rs, rooms.rs,
  install.rs read the records too).
