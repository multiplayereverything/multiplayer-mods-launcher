# Windows and Linux

The launcher ships for Windows and for Linux, and the two stay level: **every update goes to both platforms**.
A change isn't done when it works on Windows; it's done when the Linux build has it too and both are released
under the same version.

## What each update needs

1. Build and test it on Windows: `npm test`, `npm run lab`, and the C++ builds if `old/` changed.
2. Port it to the Linux build. Anything that calls a Windows-only API, path, registry key or tool gets a Linux
   equivalent, or is left out on Linux with a note in the log below. The Linux build is built and tested in WSL
   (distro `Ubuntu`, on the ext4 side, WSLg for the window).
3. The same checks pass on Linux, including the lab.
4. Cross-play still works between Windows and Linux, for PC games and Flash games.
5. Release both under the same version tag. Windows: the setup exe, the launcher exe and `latest.json`. Linux:
   `MultiplayerEverythingLauncher.appimage` and `MultiplayerEverythingLauncher.tar.gz` (the source). Both use the
   icons in `src-tauri/icons/`.

If one platform can't get an update yet, say so in the release notes and add it to the log, so the next session
picks it up.

## Linux specifics

- PC games launch through Steam: the native Linux build when the game has one, otherwise Proton.
- Flash games use Ruffle Together where the Linux build supports it.
- The Linux port is being built on the `linux-port` branch; until it's merged, port each update made on `main`
  to that branch as well.

## Log

Newest first. One line per update: version, what changed, and whether each platform has it.

| Date | Version | Change | Windows | Linux |
|---|---|---|---|---|
| 2026-11-05 | unreleased | mods-info as one zip (`downloads.modsInfoZip`, a new crate, `zip`), and `releases.json` instead of GitHub's API for our mods; refreshed on window focus. Reads WinHTTP on Windows | yes | port with the Linux build |
| 2026-11-05 | unreleased | The launcher reads only mods-info `records/`: no fallback to the deleted `team/` and `community/` | yes | port with the Linux build |
| 2026-11-05 | 1.0.45 | One list of mods: mods-info `records/`, no Community mods category, A to Z, Made by | yes | port with the Linux build |
| 2026-11-05 | 1.0.45 | Controller: the Host and Join dialogs work, on-screen keyboard in the desktop window | yes | port with the Linux build |
| 2026-11-04 | 1.0.44 | Join by IP adds the game's port to an address typed without one | yes | port with the Linux build |
| 2026-11-04 | 1.0.43 | Lab: `communitymods` waits for the previous game to close and for the zip's subfolders (`ec1f773`) | yes | port with the Linux build |
| 2026-11-04 | 1.0.43 | Developers' Flash games come from mods-info, and the launcher makes their buttons | yes | port with the Linux build |
| 2026-11-04 | 1.0.42 | The app updates itself from a signed `latest.json` | yes | port with the Linux build |
| 2026-11-04 | — | Linux port started on `linux-port` (AppImage and source tar.gz, Steam with Proton or native builds) | — | in progress |
