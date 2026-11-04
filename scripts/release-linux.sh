#!/usr/bin/env bash
# Builds the Linux release next to the Windows one (scripts/release.ps1), for the same tag. Nothing is published:
# the files to attach to the GitHub release land in release/v<version>/:
#   MultiplayerEverythingLauncher.appimage  the app with the C++ launcher (engine) inside it
#   MultiplayerEverythingLauncher.tar.gz    the source the release was built from (git archive of HEAD)
#   latest.json                             gets a "linux-x86_64" entry when the AppImage is signed
#
# Build it on the oldest system it should run on: an AppImage needs a glibc at least as new as the one it was
# built with. Ubuntu 22.04 (glibc 2.35) is the oldest with webkit2gtk-4.1, which Tauri 2 needs; docs/LINUX.md says
# how to set one up (a chroot works).
#
# The updater's private key is read from $TAURI_SIGNING_PRIVATE_KEY (the key or its path), else from
# ~/.tauri/multiplayer-everything.key. Without it the AppImage is built unsigned and latest.json is left alone, so
# installed Linux apps don't update to it by themselves.
#
#   scripts/release-linux.sh                 build, test, package
#   OUT=/some/dir scripts/release-linux.sh   put the files there instead
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
version=$(node -p "require('./src-tauri/tauri.conf.json').version")
cmake_version=$(sed -n 's/^ *project([^ ]* VERSION \([0-9.]*\).*/\1/p' old/CMakeLists.txt | head -1)
[ "$version" = "$cmake_version" ] || { echo "tauri.conf.json says $version but old/CMakeLists.txt says $cmake_version"; exit 1; }
out=${OUT:-$root/release/v$version}
engine_build=${ENGINE_BUILD:-$root/old/build-release-linux}
export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-$root/src-tauri/target}

key=${TAURI_SIGNING_PRIVATE_KEY:-}
if [ -z "$key" ] && [ -f "$HOME/.tauri/multiplayer-everything.key" ]; then key="$HOME/.tauri/multiplayer-everything.key"; fi
extra=()
if [ -n "$key" ]; then
  export TAURI_SIGNING_PRIVATE_KEY="$key" TAURI_SIGNING_PRIVATE_KEY_PASSWORD="${TAURI_SIGNING_PRIVATE_KEY_PASSWORD:-}"
  extra=(--config '{"bundle":{"createUpdaterArtifacts":true}}')
else
  echo "No updater key: the AppImage will be unsigned and latest.json gets no Linux entry."
fi

# The engine ships only when its own tests pass (run from the repository root, as on Windows).
cmake -S old -B "$engine_build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$engine_build"
"$engine_build/launcher_tests"
mkdir -p src-tauri/engine
cp "$engine_build/MultiplayerEverythingLauncher" src-tauri/engine/
strip src-tauri/engine/MultiplayerEverythingLauncher

npm ci
npm test
npx tauri build --config src-tauri/tauri.linux.json "${extra[@]}"

bundle="$CARGO_TARGET_DIR/release/bundle/appimage"
appimage=$(ls "$bundle"/*_"${version}"_amd64.AppImage | head -1)
mkdir -p "$out"
cp "$appimage" "$out/MultiplayerEverythingLauncher.appimage"
chmod +x "$out/MultiplayerEverythingLauncher.appimage"
if git rev-parse --git-dir >/dev/null 2>&1; then
  git archive --format=tar.gz --prefix="MultiplayerEverythingLauncher-$version/" -o "$out/MultiplayerEverythingLauncher.tar.gz" HEAD
else
  echo "Not a git checkout: make MultiplayerEverythingLauncher.tar.gz with git archive where the repository is."
fi

if [ -f "$appimage.sig" ]; then
  # The updater downloads the file under its release name; the signature is of its bytes, so the copy keeps it.
  SIG=$(cat "$appimage.sig") URL="https://github.com/multiplayereverything/multiplayer-mods-launcher/releases/download/v$version/MultiplayerEverythingLauncher.appimage" \
  FEED="$out/latest.json" VERSION="$version" node -e '
    const fs = require("fs");
    const f = process.env.FEED;
    const feed = fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, "utf8")) : { version: process.env.VERSION, notes: "", pub_date: new Date().toISOString().replace(/\.\d+Z$/, "Z"), platforms: {} };
    if (feed.version !== process.env.VERSION) throw new Error(f + " is for " + feed.version);
    feed.platforms["linux-x86_64"] = { signature: process.env.SIG.trim(), url: process.env.URL };
    fs.writeFileSync(f, JSON.stringify(feed, null, 2));
  '
fi
echo "Release files for v$version in $out (not published):"
ls -l "$out"
