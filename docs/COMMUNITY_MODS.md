# Community mods from mods-info

How a mod made by someone else reaches the launcher without a launcher release. The rules for deciding whether to
list one at all (ask the owner, credit the author, review by hand) are in [ADDING_A_MOD.md](ADDING_A_MOD.md).

## The path

1. The developer adds their mod on the website's developers page (name, description, players, picture) and puts a
   manifest, `multiplayereverything.json`, on their GitHub release next to the mod file. The manifest's format is on
   the website's "Developers, Launcher" page.
2. At approval the website checks the manifest, pins that release (tag, file, SHA-256) and opens a pull request on
   [mods-info](https://github.com/multiplayereverything/mods-info) with the record below.
3. Only the owner's merge publishes it. Within an hour every launcher has it: a card in Community mods, named after
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
    `detection` with only `steam` and `path` rules;
  - `install`: `targetDir` inside `${GAME_DIR}`, `preserve`, `requires` (each `path` relative to the game folder, like `BepInEx\core`, without `${GAME_DIR}`), `uninstallCleanup`;
  - `components`: `id`, `name`, `source` (inside the package), `target` inside `${GAME_DIR}` or `${INSTALL_DIR}`,
    `targetName`, `required`;
  - `launch`: only `uri` (`steam://rungameid/<app id>` or `${STEAM_APP_ID}`) and `hint`.
- Any other key, anywhere, refuses the whole record, so a record can never do more than it shows: no `package`, no
  `extras`, no `allowedRoots` or `stateDir`, no executable to run, no registry or Epic detection.
- After those rules the mod gets every check a launcher.json mod gets. An id launcher.json already has is refused.

## In the launcher

- The app (`src-tauri/src/launcher/community.rs`) asks mods-info at start and then hourly, using the copy it saved
  (`community.json` in the data folder) for an hour. A mods-info without `community/` (404) means none. Any other
  failure keeps the last good copy.
- The C++ mod commands (`old/src/core/community.cpp`, until PORTING.md part 6) read the same saved copy and check
  each record again, so Install, Repair, Uninstall and Launch know the same mods as the window.
- The card's description, players and picture come from the website's `/api/projects`. A project the website lists
  that has no approved record shows as a Community card that says it can't be installed yet.
- The download is checked against the pinned SHA-256 before anything is installed, as for every mod.
- `FL_NO_COMMUNITY` leaves community mods out (both sides). The lab's `communitymods` scenario installs one from a
  stand-in mods-info and github.com and checks Uninstall leaves the game folder byte-identical.
