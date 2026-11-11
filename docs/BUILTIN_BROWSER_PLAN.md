# Plan: HTML5 games in the launcher's own window (and TV mode)

Status: plan only, nothing built. First game: Plazma Burst 2 campaign co-op 0.3.0
(`ai-work-flash-games\plazma burst 2 campaign mod`, docs in its HANDOFF.md and pb2-coop\README.md).

## What already exists

Most of the pipe is already in the Rust launcher, just never shipped:

- `players[].kind: "browser"` opens `${URL}` in Edge or Chrome as an app window (`docs/html5-example.jsonc`).
- An action's `server` starts a program hidden first (`BackgroundProcess`, src-tauri/src/launcher/process.rs).
  It runs inside a job object, so the program and its children end with the game even if the app crashes.
  The launcher waits for its `url` to answer, then opens the page.
- `stable_port(game_id)` keeps the same port per game, so browser saves stay put.
- The example entry already has PB2 with Host, Host on this network and Join with a link.

So the new work is the window, reading the server's lines, the Node runtime and TV input.
The rest is config.

## What changes

### 1. A player that is the launcher itself

- New `players[].kind: "window"`, with no `paths` or `exe`. The page opens in a second launcher window.
- Optional `window: { "width": 800, "height": 600, "title": "..." }`, taken from the player or the action.
- `launch::run` gets an opener from lib.rs instead of starting an exe for this kind.
- lib.rs builds a Tauri `WebviewWindow` with the label `game-<session>`, then waits for it to close.
  Closing it stops the server and adds the play time, the same as a player exiting today.
- **Locked down:**
  - The game window gets no Tauri permissions; capabilities stay on `main` only.
  - Navigation is limited to the served origin. Any other link opens in the system browser.
  - No dev tools.
- **Browser setting:** none needed. Step 0 showed WebView2 already lets sound start without a click.
- **The page is loaded from `http://localhost:<port>/`**, never a LAN IP, for two reasons:
  - `crypto.subtle` needs a secure context, and localhost counts as one.
  - The origin stays the same between runs, so saves are kept.
- **Nothing is injected that touches game state.** PB2's anti-tamper reloads the page if it is.
- **The C++ engine** (old/src/core/config) must accept `kind: "window"` and treat it as always found.
  Otherwise `--list` and validation reject the shipped launcher.json.
  It never launches this kind; the CLI says it needs the launcher window.
- **Edge/Chrome stay as a fallback**, so the same game works if WebView2 is broken.
  One `player` for the action, with a list of fallbacks, as Flash games have.

### 2. Reading the server's lines

`server` gets optional patterns that the launcher matches against each line the program prints.
This is a regex for each meaning, so other HTML5 mods can use their own wording:

```jsonc
"server": {
  "exe": ["${RUNTIME:node}", "node.exe"],
  "args": ["server\\server.js", "2053"],
  "url": "http://localhost:2053/",
  "lines": {
    "code":  "room code ([A-Z0-9]{5})",   // host: shown in the Host panel, copied, sent to Discord
    "ready": "Joined room",                 // join: open the window on this instead of polling url
    "error": "Could not join room .*"       // shown as the reason; exit code 1 also counts as failed
  }
}
```

- **`${CODE}` in `server.args`** comes from the window's join box, so Join becomes one action:
  `"args": ["server\\join.js", "${CODE}", "2054"]`.
- **Host:** the code reaches the window through the existing `host-status` event.
  The Host panel and slot strip work as they do for relay games.
- **`"online": { "id": "pb2", "join": "join" }`** would give PB2 the Rooms tab and Quick play.
  Step 0 found the relay doesn't list tunnel-mode rooms, so this waits on a relay change.

### 3. Node.js as a runtime the launcher installs

- New `runtimes[]` entries use the same shape as `players[].install`:
  a `url`, its `sha256` and `exe`, from nodejs.org's official zip, pinned.
- `${RUNTIME:node}` resolves to it. A missing runtime shows the existing
  `status.noRuntime` / `button.getRuntime` texts, and "Installs when you play" in the game page's list.
- **Not shipped inside the launcher.** It is downloaded on first use and checked by SHA-256 before it is used.

### 4. The mod and the game files

- The PB2 co-op mod becomes a release in a `multiplayereverything` repository.
  It is pinned with its SHA-256 and installed into `${GAMES_DIR}\pb2`, like Flash game files from the catalog.
- **Decided 2026-11-11:** the owner allows hosting the game files. They already live in the public
  `multiplayereverything/html5-games` repository (`pb2/plazma_burst_2` plus `pb2/pb2-coop`).
  Each version is published there as a release zip, pinned by SHA-256 and downloaded into `${GAMES_DIR}\pb2`,
  so PB2 is one click. That repository's mod predates 0.3.0, and its README still describes Cloudflare links
  and the old launcher name. Ask the PB2 session to update it.

### 5. TV mode

- The game window opens full screen with no frame when the launch comes from TV mode.
- **Controller to keys and mouse:** a `pad` map on the action, for example:
  - left stick or D-pad to `WASD`
  - right stick to mouse movement
  - RT to left click
  - A to `Space`
  - Start to `Escape`
- **How the input is sent:** Rust reads XInput and uses `SendInput` to the game window only, and only while it has focus.
  Step 0 showed simulated page clicks don't press PB2's buttons.
- **Back to TV:** holding Back plus Start closes the game window and returns to the TV view.
  The TV view shows this hint for that game.

## Step 0 results (2026-11-11)

**Setup:**
- The launcher 1.0.47 release build, with a throwaway WebView2 profile (`WEBVIEW2_USER_DATA_FOLDER`).
- Its window driven over a debug port.
- PB2 co-op 0.3.0 served by `node server\server.js 2060 --no-relay`.

| Check | Result |
|---|---|
| Loads in WebView2 | Yes. It goes straight to the co-op screen and fills the window. |
| `isSecureContext`, `crypto.subtle` on `http://localhost` | Both present |
| Sound with no click | An AudioContext is `running` with no user activation, so WebView2 allows autoplay by default. No browser flag is needed. |
| Saves | Survive a restart. PB2 saves to localStorage `plazma_burst2` and IndexedDB `localforage`. A write made a moment before a **forced** kill was lost, because Chromium flushes storage late. The launcher must close the game window normally, not kill it. |
| Anti-tamper | 0 reloads over 90 s while reading the page and sending simulated input |
| Simulated keys | **Work.** The game's own `onkeydown` reads `keyCode`/`code` and never checks `isTrusted`. The page swaps out `KeyboardEvent`/`MouseEvent`, but `JSKeyboardEvent` is the native one, and `keyCode` set in the init dictionary works. A simulated Space started a level, and a held D moved the view. |
| Simulated mouse | **Moves only.** The cursor follows, but simulated clicks don't press menu buttons. Real (trusted) clicks do. |
| Relay room list | **Not listed.** A host got room code 8QGMC, yet `RELAY\|1\|LIST\|pb2` and `RELAY\|2\|ROOMS\|pb2` both came back empty. Tunnel-mode rooms aren't listed. |

**What changes in the plan:**
- **TV mode input:** sent from Rust with `SendInput` to the focused game window, for keys and mouse alike.
  Keys alone would also work from the page, but one route is simpler.
- **Rooms tab and Quick play:** not possible for PB2 until the relay lists tunnel rooms.
  That is a server change, owned elsewhere. Join by code is unaffected.
- **Closing the game:** the window closes normally, and the server stops after it.

## Order of work

0. **Spike (no commit):**
   - Open PB2's host page in a hand-made Tauri game window.
   - Check:
     - `crypto.subtle`
     - sound starts without a click
     - saves survive a restart
     - no anti-tamper reload
     - whether synthetic key events move the player
     - whether `RELAY|1|LIST` shows a `pb2` tunnel room
1. Config and both engines: `kind: "window"`, `window`, `server.lines`, `${CODE}` in server args, `runtimes[]`.
   Add Rust unit tests for line matching and argument filling, and C++ config tests for the new kind.
2. The game window in lib.rs: open, lock down, close, stop the server, play time, Discord.
3. Host and Join in the UI: the code in the Host panel, Join by code, the join error, Rooms and Quick play.
4. The Node runtime install, with tests against the local fake server, as players have.
5. TV mode: full screen, the pad map, the exit combo.
6. **Lab:**
   - A fake HTML5 server, a few lines of script, that prints a code and serves a page.
   - The lab checks:
     - the window opens on that page
     - the code shows in the Host panel
     - a bad code shows the error
     - closing the window stops the server
   - The lab doesn't need the real PB2.
7. PB2 itself:
   - the mod release
   - the game-files decision
   - the launcher.json or team record
   - a SteamGridDB cover
   - a real two-PC test
8. Docs: CONFIG.md (the new keys), PLATFORMS.md (Linux uses WebKitGTK for this window: note it for the Linux owner),
   and an update to html5-example.jsonc.

## Not in this plan

- Changes to pb2-coop. Its stdout lines and join.js exit codes are what this reads, and the PB2 session was asked to keep them stable.
- Running HTML5 games straight from a website. Only local servers and local pages are covered.
