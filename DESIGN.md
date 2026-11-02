# The prototype's look

The window looks like the released C++ launcher: the same palette, fonts, sidebar, cards, details panel and
status bar. A few page ideas come from RecompLauncher (github.com/elripalda/RecompLauncher, GPL-3.0). We took
its ideas, not its code: nothing here is copied from it, so our license is unaffected.

## From the C++ launcher (as released, commit 14b4552)

- **Palette:** `darkPalette` in `src/core/config.cpp`:
  - Background `#15171C`, panels `#1E2128` and `#272B34`, sidebar `#0E1014`.
  - Text `#E8EAED` and `#9AA1AC`, borders `#323743`.
  - Light blue `#8FB0FF` for focus and online, green `#3CCB7F` for ready, red `#C8313A` only for starting a game.
- **Fonts:** 10pt Segoe UI for text. Bahnschrift SemiBold for headings and buttons, and Bahnschrift SemiBold
  Condensed for names and titles.
- **Sidebar:** the title and subtitle, then each category with its Fluent icon and count, in the C++ order:
  platforms first, then Favorites, Ready to play, Rooms, Coming soon, then the rest. Settings sits at the foot.
- **Cards** (`drawCard`):
  - 16:9 art on top. Online and PC badges at its top left; the website's grade chip (A-D, X) at its top right.
  - Under the art: the name in the condensed font, the player pips and count, and the state ("Ready" in green).
  - Hovering lifts a card 3px. The selected card has a white outline.
- **Details panel:**
  - The P1-P4 slots with their colors along the top, and the state pill.
  - The website's block: the grade and what it means, the version line, and the "In sync" dots.
  - The buttons as `drawButton` paints them: 6px corners, red Host a game, two-line buttons with a second line
    under the label, and the Favorite / Guide / Folder row.
- **Tabs:** the chosen one is underlined.
- **Status bar:** the key hints, Server online, Ruffle Together, and the Flashpoint folder.
- **Page change:** a 28px slide with a fade over 260 ms.

## From RecompLauncher's layout

- Home: a banner that rotates through a few games, and shelves that page sideways (Jump back in, Play together
  online, PC game mods, Coming soon).
- The game page's wide art header, which fades into a blurred copy of itself, with four stat boxes.
- Messages, running games and mod jobs float in an "island" above the status bar.
- The window draws its own title bar.

## Data

The grade, version and sync dots come from `--list --json`. Each game now carries a `site` object, read from the
website's games.json that the C++ launcher caches.

## Ideas from other software (ideas only, in our own style)

- **Steam:**
  - The friends row became "Open right now": the games with rooms on the central server, most players first.
  - Its updates row became "Recently updated", from the website's dates.
  - The header for a game without wide art shows the cover blurred and dark, with the sharp cover in front.
  - Play from the grid.
- **Discord:**
  - Ctrl+K finds a game or a page by name or initials.
  - A notification when a friend joins your room while the launcher is behind the game.
- **Playnite and GOG Galaxy:** a controller and the arrow keys move the focus, and Large or Compact cards.
- **RecompLauncher:** the first-run setup's step-by-step shape, built around this launcher's needs (name, Ruffle
  Together, your PC games, the files folder, a few choices).

## Parity with the C++ panel

These come from the C++ launcher through its command line (`--launch --wait`, `--coop-save`, `--rooms all`,
`--update-players`, and the fields of `--list --json`):
- Quick play, including for lobby games.
- The co-op save choice, each game's own settings, and the player choice per game.
- The download offer when Play meets a missing file.
- Community mod credit.
- Room counts on cards and the server's ping.
- Ruffle Together kept up to date.
- Discord status while a game runs.
- Each action's hint after it starts.
- Compact cards.
- The light theme.
- A modded game that is open says to close it first.
