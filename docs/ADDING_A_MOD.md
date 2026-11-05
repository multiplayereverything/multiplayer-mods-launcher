# Adding a PC game mod to the launcher: checklist

For whoever maintains the launcher. The technical details (package layouts, every option)
are in [MOD_PACKAGES.md](MOD_PACKAGES.md) and [CONFIG.md](../CONFIG.md). This page is the
order to do things in, and what to check before a mod reaches players, because the
launcher puts a mod's code into people's games under our name.

Mods we make ourselves skip steps 1 and 3 (and can use "newest release", step 5).

## 1. Before anything: the author

- [ ] The author agrees to be listed. Ask in writing (Discord or a GitHub issue); keep the link.
- [ ] Their name and a link to the mod's page go on the card (the description says who made it).
- [ ] The mod's license allows redistributing the release file through the launcher.
- [ ] The mod works for the game version players have today; the author says which game
      version it was built for.

## 2. What the author must provide

- [ ] A **public GitHub release** with the mod as a `.zip`. A stable file name that a pattern can match
      (`MyMod-*.zip`) and the same layout in every release.
- [ ] The zip holds **only the mod's files**: no game files, no decompiled code, no keys, no
      loader unless the next point applies.
- [ ] Inside the zip, the files sit where they go in the game folder (for BepInEx mods:
      `BepInEx/plugins/<Mod>/<Mod>.dll`). The launcher installs only the paths the recipe names; anything
      else in the zip is ignored.
- [ ] The loader the mod needs, and its exact version, written down (BepInEx 5 or 6, Mono or IL2CPP, MelonLoader).

## 3. Review what you are about to point players at

- [ ] Read the release notes, the source or the build instructions. Know what the mod does to the game
      (patches, network connections, files it writes).
- [ ] Download the zip by hand, unpack it into a **new, empty folder**, and look at every file. Nothing
      unexpected (extra programs, installers, scripts).
- [ ] Run it once on a game copy, not your main install.
- [ ] Only the files the recipe names are installed, so a surprise in the zip doesn't reach the game folder,
      but the mod's own code does run in the game. The review above is the real protection; checksums only prove
      a file is the one GitHub published.

## 4. The loader

- [ ] **Our mods:** publish the loader as its own pre-release in the mod's repository, with a versioned tag
      (`bepinex-5.4.23.5`), the unmodified official zip, and release notes giving the license (LGPL-2.1) and a link to the
      source. Don't delete it later.
- [ ] **Other people's mods:** use BepInEx's own release, pinned by tag, for example
      `{ "dir": "loader", "github": "BepInEx/BepInEx", "tag": "v5.4.23.5", "asset": "BepInEx_win_x64_5.4.23.5.zip" }`.
      Don't add anything to their repository.
- [ ] BepInEx 6 builds from `builds.bepinex.dev` have no published checksum. Mirror the build somewhere we control
      (a release of our own) so the launcher can check it, or leave the mod out until it can be installed with a checksum.

## 5. The entry

Since 1.0.42 our own mods are team records in mods-info (`records/<id>.json`, docs/COMMUNITY_MODS.md), not
launcher.json entries: the record is the same games[] entry with `id`, `repo`, `platform`, `author` and the approval
added. Write the entry below, check and publish it on the website's dashboard (Team mods), then copy the record into
`tests/fixtures/team` without `platform` and `author`, and add its id to that folder's `index.json`, so the tests know
it. A mod made by someone else goes through the developer SDK instead
(community records, below).

- [ ] Copy the closest existing mod (`spooky-together`, `baldi-together`, `miside-together`) and change the
      id, name, game folder detection (Steam app id, install folder name, game exe, validation files),
      the process names, the repository and the asset pattern.
- [ ] `categories`: `["pc", ...]` plus the kinds that fit (`coop`, `online`).
- [ ] **Our mods:** `package.github` and `asset` = always the newest release.
- [ ] **Other people's mods:** pin the version you reviewed instead: `package.url` with its `sha256` (release
      downloads are `https://github.com/<owner>/<repo>/releases/download/<tag>/<file>.zip`). A newer release from
      them reaches nobody until you have reviewed it and changed the pin.
- [ ] `install.uninstallCleanup` lists the files the loader creates while running, so Uninstall leaves the game folder
      as it was (`BepInEx/cache/**`, logs, and for BepInEx 6 `BepInEx/interop/**`, `BepInEx/unity-libs/**`).
- [ ] `launch.hint` says in one sentence how to open the mod's menu in the game.
- [ ] `online` is set only if the mod uses our central server. Mods that use Steam or direct connections have none.

## 6. Test it before anyone else does

- [ ] `MultiplayerEverythingLauncher.exe --validate` says `OK`.
- [ ] On a **copy** of the game folder (or a fake one with the validation files):
      `--install-mod <id> --game-dir "<folder>"`, then check the files, then
      `--repair-mod`, then `--uninstall-mod`. The folder must be exactly as it started
      ([MOD_PACKAGES.md](MOD_PACKAGES.md), "Try it for real, on a copy").
- [ ] Never test against a game folder that is in use or that you play on. The launcher refuses to install
      while the game runs.
- [ ] Add the mod's release layout to `tests/test_shipped_mods.cpp` (file names only), as the other mods have,
      so a changed layout is caught.
- [ ] `build.ps1 -Test` passes.

## 7. Tell people

- [ ] Add a row to the PC mods table in the README and to the list at the top.
- [ ] Take a fresh screenshot of the library.
- [ ] Credit the author and link their page in the release notes of the launcher release.
- [ ] Tell the author it is live, and who to contact when a release changes its layout.

## 8. Later

- [ ] When the author publishes a new version of an outside mod, repeat steps 3 and 6 for it, then move the pin.
- [ ] If the mod needs a different loader build, that is a new loader release (new tag) and a change to its record
      (republish it on the dashboard; no launcher release needed).
- [ ] If the author stops answering or the mod breaks the game, take the entry out. That also removes its Uninstall button
      for players who already installed it, so say in the release notes how to remove it by hand (delete the mod's folder under
      `BepInEx\plugins`), or keep the entry for one more release with its install disabled.

## Community mods (made by someone else)

A mod whose repository is not under `multiplayereverything` is a community mod. We list it, credit the author and
pin the version; we do not vouch for it beyond what we read.

- [ ] **Ask first.** Full support (the launcher installs it) or only a link to its GitHub page? The owner decides, every time.
- [ ] **List** comes from the record: an installed mod is in PC mods, a Flash record in Flash mods, next to ours, A to Z.
      A link card says which with `platform`. Who made it shows as Made by on the page, and the review line says whether we read its code.
- [ ] **Name** the card after the game ("Forager", "MDK2 HD", "Spore"), never "<game> Co-op" or "Split-screen".
- [ ] **Credit the author and the repository**: `mod.publisher` and `mod.website`, a "Made by X, not by Multiplayer Everything" line
      in the guide tips, a labelled link in `guide.links`, and a link action (the amber button) whose second line names the author
      and the repo. A mod game may carry link actions and nothing else besides the launcher's own buttons.
- [ ] **Pin one release.** Whatever version is in the entry is what every player installs: `package.version`, `package.url`
      (the release asset, never a branch, "latest" or GitHub's generated source zip) and `package.sha256` of the exact file you
      opened. The file may be a zip or a single file such as a `.dll`; either way the hash must match.
- [ ] **Hash at download only.** The launcher checks the SHA-256 when it downloads the file and refuses a mismatch. It does not
      watch the installed files afterwards: what a player does with their own game folder is their choice.
- [ ] **Updating is manual.** When the author publishes a newer release, read what changed (steps 3 and 6 above), and only then
      change `version`, `url` and `sha256` together. A new release never reaches players on its own.
- [ ] Say what is not tested (a Steam copy, the internet, a Windows build) in the guide tips, as the author's own README does.
- [ ] Never edit or rebuild the mod itself here, and the website lists it with `origin: "community"` and `reviewed: false`
      until someone has read its code.

### A community Flash game

The same rules, with a Flash record (`"kind": "flash"`, docs/COMMUNITY_MODS.md "Flash records"): the card lands in
Flash mods, named after the game (`flash.game`).

- [ ] **Pin the game file and its key map** from one release: `flash.file` (the `.swf`) and, when the game has its own
      `.keys` file, `flash.keys` the same way, each with the SHA-256 of the exact file you played. A built-in map is
      named instead (`"keys": "gun-mayhem-2"`).
- [ ] **Play it in Ruffle Together** with 2 players and up to `maxPlayers`, from the launcher's own buttons, before it
      goes out. Set `flash.ruffle` to the oldest Ruffle Together it worked in.
- [ ] No buttons or arguments of your own: the launcher makes them. A game that needs something they can't say is a
      new building block for the launcher first.
