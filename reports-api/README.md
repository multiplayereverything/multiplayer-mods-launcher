# Reports API

The launcher's "How did it run?" reports: a Cloudflare Worker with one D1 database. The launcher sends a report
after a session of 10 minutes or more and reads them back for each game's page (`src/Reports.tsx`,
`src-tauri/src/launcher/reports.rs`). The website can read them too (GET answers any origin).

| | |
|---|---|
| `POST /v1/reports` | `{game, install, verdict, version, mins, note, os, gpu, cpu, ram_gb}` -> `{ok: true}` or `{error}` |
| `GET /v1/reports?game=<id>` | `{total, counts: {runs, issues, broken}, versions, recent}` (the newest 20) |

- Verdicts are `runs`, `issues` and `broken`. One report per PC, game and version: a second one replaces the first.
- Nothing names a player. The install id is stored only as a hash, and an IP only as a hash salted with the hour,
  for the limit of 20 reports an hour, then deleted.
- The Worker makes its tables itself the first time it runs, so a new database needs no step.

## Run and test

```bash
npm install
npm run dev      # http://localhost:8790, with a local database
npm test         # in a second terminal, against the running dev server
```

To point the launcher at the local copy, give it a launcher.json with `"reports": { "url": "http://localhost:8790" }`
through `FL_LAUNCHER_CONFIG`. The Rust side has a test against it too:
`FL_TEST_REPORTS_URL=http://localhost:8790 cargo test --lib the_local_worker -- --nocapture`.

## Going live (only with the owner's OK)

1. `npm run deploy` (`cf deploy`) creates the Worker and its D1 database on the Multiplayer Everything account.
2. Put the Worker's address in `launcher/launcher.json`: `"reports": { "url": "https://..." }`. Until then the
   launcher asks nothing and shows no card.

## Hiding a report

Notes show as they were typed. To hide one, set `hidden = 1` on its row (for example with `cf d1 raw` against the
database): it stops counting and showing. A new answer from the same PC for the same version shows again.
