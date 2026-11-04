# Mods from mods-info: community and team records

How a mod reaches the launcher without a launcher release. mods-info has two folders of records:

- `community/`: mods made by someone else, approved by the owner (the developer SDK). Strict, pinned, credited.
- `team/`: Multiplayer Everything's own mods. Each record is the mod's launcher.json `games[]` entry with a few
  record keys added, from a `multiplayereverything` repository only, following its newest release. See
  [Team records](#team-records) below.

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
- `kind` is `pc` when left out. A kind a launcher doesn't know is left out whole, so a later kind (a Flash mod by
  someone else: its pinned SWF and SHA-256, key map, player counts and the Ruffle Together version it needs) never
  shows half a card in an older launcher.

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
  `image`, `imageFocus` and `mod`, with every block launcher.json has (`sourceGame`, `allowedRoots`, `extras` by tag,
  any detection, any launch). No `actions` (the launcher makes the buttons), no `kind`; any other key refuses it.
- `repo` must be `multiplayereverything/<name>`. `mod.package.github` must be that repo: the mod follows its newest
  release, checked against GitHub's SHA-256 at download (no review per version). A pinned `url` must be one of its
  release files. Every `extras` download must come from a `multiplayereverything` repository.
- `categories` may not hold `community`, and must be launcher.json's categories. The card has no "made by" credit.
- Then every check a launcher.json game gets. An id launcher.json already has is refused (launcher.json wins while
  both have it), and team records are read before community ones, so a developer's record can't take a team id.
- The website writes them through a team-only path (not the developer form), for `multiplayereverything`
  repositories only. The launcher session's test `the_team_records_in_a_mods_info_checkout_are_the_games_launcher_json_has`
  (`FL_TEST_COMMUNITY_DIR=<mods-info checkout>`) proves each record makes exactly the game its launcher.json entry did.

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
  does the same for a team record that follows its newest release and brings BepInEx, and shows a record for a newer
  launcher.
