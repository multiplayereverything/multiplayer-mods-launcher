# The launcher on Linux

The Linux build is the same app as the Windows one: the same window (`src/`), the same Rust launcher
(`src-tauri/src/launcher/`), and the same C++ engine (`old/`), all built for Linux. Windows code stays as it is behind
`#[cfg(windows)]`; each Windows-only piece has a `#[cfg(unix)]` twin. In the C++ engine, the Linux code is in
`old/src/core/posix/`. [PLATFORMS.md](../PLATFORMS.md) says how updates reach both platforms.

## Running it

```bash
chmod +x MultiplayerEverythingLauncher.appimage
./MultiplayerEverythingLauncher.appimage
```

It needs:

- x86-64 and a glibc of 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 or newer).
- FUSE 2 to mount the AppImage (`libfuse2` on Ubuntu and Debian). Without it, run the AppImage with
  `--appimage-extract-and-run`.
- `curl`, which the launcher downloads with.

Settings and data live in `~/.local/share/MultiplayerEverything/MultiplayerEverythingLauncher` (`$XDG_DATA_HOME`).

## How games run

| Kind of game | On Linux |
|---|---|
| PC game with a native Linux build | Started through Steam |
| PC game without one | Started through Steam, which uses Proton |
| A Windows program started directly (a mod's tool, a player) | Run through Wine; it has to be installed |
| Flash game | Ruffle Together, through Wine: upstream only ships a Windows build |

A file starting with `MZ` is a Windows program, whatever it is called, so it goes through Wine. Everything else is
started directly.

### BepInEx mods

BepInEx loads through a `winhttp.dll` beside the game, and Wine ignores that file unless told otherwise.

- **Started directly through Wine:** the launcher sets `WINEDLLOVERRIDES=winhttp=n,b` when the DLL is there and the
  variable isn't already set.
- **Started through Steam with Proton:** Steam starts the game, so the launcher can't set the variable. Set the
  game's launch options in Steam once:

  ```
  WINEDLLOVERRIDES="winhttp=n,b" %command%
  ```

### Cross-play

The relay, rooms and lobby bridge speak the same protocol on both platforms, so Windows and Linux players can share a
room. The C++ tests and the parity suite cover the protocol on Linux. Playing a real game between a Windows PC and a
Linux PC is checked by hand.

## What's different from Windows

- **Updates:** only the AppImage updates itself. The updater writes the signed AppImage over the one running and opens
  it again. A build run from anywhere else never offers an update, as it would overwrite itself with an AppImage.
- **UPnP:** not implemented in the C++ engine on Linux. Hosting without the relay needs the port forwarded by hand.
- **After a crash:** if the launcher crashes, the programs its background processes started keep running. Windows ties
  them to a job object, which closes them too.
- **Gamepads** need a WebKitGTK built with gamepad support. On one without, the pad does nothing and the keyboard still works.
- **A port already in use** shows as `Unknown error (code 98)`.
- **Opening a link or folder** uses `xdg-open`. The AppImage's own library paths are removed first, so the opened
  program doesn't load them.

## Building it

The release has to be built on the oldest system it should run on, as an AppImage needs a glibc at least as new as
the one it was built with. Ubuntu 22.04 (glibc 2.35) is the oldest with `webkit2gtk-4.1`, which Tauri 2 needs.

On Ubuntu 22.04, or in a chroot of it:

```bash
sudo apt install build-essential g++-12 cmake ninja-build pkg-config curl file git patchelf \
  desktop-file-utils libfuse2 squashfs-tools libwebkit2gtk-4.1-dev libayatana-appindicator3-dev \
  librsvg2-dev libssl-dev libxdo-dev libgtk-3-dev
```

You also need Rust (rustup) and Node.js 24. Then:

```bash
scripts/release-linux.sh
```

This builds the engine and runs its tests, runs `npm test`, builds the AppImage, and puts these in `release/v<version>/`:

- `MultiplayerEverythingLauncher.appimage`, with the engine inside at `usr/bin/MultiplayerEverythingLauncher`.
- `MultiplayerEverythingLauncher.tar.gz`, a `git archive` of `HEAD`.
- `latest.json`, with a `linux-x86_64` entry, only if the AppImage was signed.

Nothing is published. `OUT`, `ENGINE_BUILD` and `CARGO_TARGET_DIR` move the outputs elsewhere.

### Signing

The updater only accepts files signed with the private key that matches the public key in `tauri.conf.json`. The script
reads the key from `$TAURI_SIGNING_PRIVATE_KEY` or `~/.tauri/multiplayer-everything.key`. Without a key the AppImage is
built unsigned and `latest.json` gets no Linux entry, so installed Linux apps stay on their version until someone
downloads the new one.

### A Windows PC with WSL

Build on the ext4 side (`~`), not under `/mnt/c`. A 22.04 chroot works when the distro itself is newer:

```bash
sudo apt install debootstrap
sudo debootstrap --variant=minbase --components=main,universe jammy /srv/jammy http://archive.ubuntu.com/ubuntu
```

Then bind-mount `/proc`, `/sys`, `/dev` and the source into it, add `jammy-updates` to its `sources.list`, and install
the packages above inside it.

## Testing

| What | Command |
|---|---|
| The window's model | `npm test` |
| The Rust launcher | `cargo test --lib` in `src-tauri` |
| The C++ engine, from the repository root | `old/build-linux/launcher_tests` |
| The window, clicked through by the lab | `npm run lab` |
| The same behaviour in C++ and Rust | `node tests/parity/<name>.mjs` (`library`, `rooms`, `launch`, `downloads`) |

Build the engine for the lab first:

```bash
cmake -S old -B old/build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build old/build-linux
```

The lab also needs `tauri-driver` (`cargo install tauri-driver --locked`), `webkitgtk-webdriver` (or
`WEBKIT_WEBDRIVER` pointing at the driver), `zip`, `wl-clipboard` (`xclip` under X11) and a display. WSLg's works, with `DISPLAY=:0`. WSLg doesn't report a minimized
window, so the lab prints that check as `SKIP` there. It also doesn't focus a window nobody clicked, so on Linux the
lab's gamepad checks treat the window as focused.

On Linux the lab's update check replaces a stand-in file (`FL_UPDATE_TARGET`, honoured only by development builds)
rather than the running program.
