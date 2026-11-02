// Parity check for part 1 of PORTING.md: the app's Rust library (src-tauri/src/launcher) must answer exactly what
// `MultiplayerEverythingLauncher.exe --list --json` answers for the same launcher.json, settings.json, data folder and
// files on disk. Both run on each input below, inside a temp folder; the parsed JSON is compared value by value (key
// order aside) and every differing path is printed. Exit code 1 when anything differs.
//
//   node tests/parity/library.mjs [--keep] [--only name,name]
//
// Needs the C++ launcher's Release build (old\build.ps1, or FL_LAUNCHER_EXE) and, for the lab's sandbox, its Debug
// tests exe (old\build.ps1 -Config Debug) as the fake player. Nothing outside the temp folder is written: the data
// folder is the temp one (LOCALAPPDATA points there too), and the one input that uses the shipped paths only reads
// the real Documents\MultiplayerEverythingLauncher folders, which must exist already.
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { makeSandbox } from '../lab/sandbox.mjs'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')
const args = process.argv.slice(2)
const keep = args.includes('--keep')
const only = args.includes('--only') ? args[args.indexOf('--only') + 1].split(',') : null

const cpp = process.env.FL_LAUNCHER_EXE ?? join(root, 'old', 'build', 'Release', 'MultiplayerEverythingLauncher.exe')
const fakePlayer = join(root, 'old', 'build', 'Debug', 'launcher_tests.exe')
if (!existsSync(cpp)) throw new Error(`No C++ launcher at ${cpp}: run old\\build.ps1 or set FL_LAUNCHER_EXE`)

console.log('Building the Rust side (cargo build --example library_json)...')
const b = spawnSync('cargo', ['build', '--example', 'library_json', '--quiet'], { cwd: join(root, 'src-tauri'), stdio: 'inherit', shell: true })
if (b.status !== 0) process.exit(1)
const rust = join(root, 'src-tauri', 'target', 'debug', 'examples', 'library_json.exe')

const work = join(tmpdir(), 'fl-parity')
rmSync(work, { recursive: true, force: true })
mkdirSync(work, { recursive: true })

const sha256 = (file) => createHash('sha256').update(readFileSync(file)).digest('hex')
function write(file, data) {
  mkdirSync(dirname(file), { recursive: true })
  writeFileSync(file, data)
}

function run(exe, config, extraEnv, isCpp) {
  const env = { ...process.env, FL_NO_DISCORD: '1', LOCALAPPDATA: join(work, 'localappdata'), FL_LAUNCHER_CONFIG: config, ...extraEnv }
  delete env.FL_NO_SITE
  if (extraEnv.FL_NO_SITE) env.FL_NO_SITE = extraEnv.FL_NO_SITE
  // The window passed FL_LAUNCHER_CONFIG to the C++ launcher as --config (src-tauri/src/engine.rs).
  const argv = isCpp ? ['--config', config, '--list', '--json'] : []
  const r = spawnSync(exe, argv, { env, encoding: 'utf8', maxBuffer: 64 << 20 })
  return { code: r.status, stdout: r.stdout.trim(), stderr: r.stderr.replace(/\r\n/g, '\n').trim() }
}

// Every path where the two differ.
function diff(a, b, path, out) {
  const ta = Array.isArray(a) ? 'array' : a === null ? 'null' : typeof a
  const tb = Array.isArray(b) ? 'array' : b === null ? 'null' : typeof b
  if (ta !== tb) return out.push(`${path}: C++ ${JSON.stringify(a)} / Rust ${JSON.stringify(b)}`)
  if (ta === 'array') {
    if (a.length !== b.length) out.push(`${path}: length C++ ${a.length} / Rust ${b.length}`)
    for (let i = 0; i < Math.min(a.length, b.length); i++) diff(a[i], b[i], `${path}[${i}]`, out)
  } else if (ta === 'object') {
    for (const k of new Set([...Object.keys(a), ...Object.keys(b)])) {
      if (!(k in a)) out.push(`${path}.${k}: only in Rust (${JSON.stringify(b[k])})`)
      else if (!(k in b)) out.push(`${path}.${k}: only in C++ (${JSON.stringify(a[k])})`)
      else diff(a[k], b[k], `${path}.${k}`, out)
    }
  } else if (a !== b) {
    out.push(`${path}: C++ ${JSON.stringify(a)} / Rust ${JSON.stringify(b)}`)
  }
}

// The shipped launcher.json with its folders moved into `dir` (and no covers next to it, so the built-in ones are
// written out under the data folder).
function shippedCopy(dir) {
  let text = readFileSync(join(root, 'launcher', 'launcher.json'), 'utf8')
  const before = text
  text = text.replace('"games": "${folder:Documents}\\\\MultiplayerEverythingLauncher\\\\games"', '"games": "${LAUNCHER_DIR}\\\\games", "data": "${LAUNCHER_DIR}\\\\data"')
  text = text.replace('"players": "${folder:Documents}\\\\MultiplayerEverythingLauncher\\\\players"', '"players": "${LAUNCHER_DIR}\\\\players"')
  if (text === before) throw new Error('The shipped paths block changed; update shippedCopy')
  write(join(dir, 'launcher.json'), text)
  return join(dir, 'launcher.json')
}

// A user who has played: favorites, stats, a located file, a games folder of their own, chosen players, game settings,
// an installed Ruffle Together with its release tag, a cached catalog (one file outdated) and the website's list.
function shippedUser(dir, cppList) {
  const config = shippedCopy(dir)
  const games = join(dir, 'my games')
  const ruffle = join(dir, 'players', 'ruffle-together')
  write(join(ruffle, 'ruffle_desktop.exe'), 'not a real player')
  write(join(ruffle, '.version'), 'v1.0.3.7\r\n')
  const present = ['gunmayhem2_online.swf', 'DadNMe_online.swf', 'Zombotron_online.swf', 'ChaosFaction2_online.swf']
  for (const f of present) write(join(games, f), `pretend ${f}`)
  write(join(dir, 'elsewhere', 'my copy.swf'), 'a located copy')
  // A game whose first action has no file yet gets one located for it.
  const missing = cppList.games.find((g) => g.actions[0]?.state === 'noFile' && !g.mod && !['gunmayhem2', 'zombotron', 'dadnme'].includes(g.id))
  const settings = {
    playerName: 'Tëster ☃', online: false, minimize: 0, showOffline: true, showOriginals: true, autoUpdate: false,
    compactCards: true, defaultPlayer: 'ruffle', gamesDir: games, sortMode: 2,
    games: {
      gunmayhem2: { favorite: true, plays: 7, seconds: 4321, lastPlayed: 1761000000, player: 'ruffle' },
      zombotron: { plays: 1, seconds: 60, lastPlayed: 1762000000, settings: { camera: 'follow', zoom: 'bogus', bump: '1', friendlyfire: 'x' } },
      dadnme: { favorite: true, player: 'nobody', settings: { friendlyfire: '1' } },
      [missing.id]: { swf: { [missing.actions[0].id]: join(dir, 'elsewhere', 'my copy.swf'), nothere: join(dir, 'nope.swf') } },
      'not-a-game': { favorite: true, plays: 2 },
      broken: 5,
    },
  }
  write(join(dir, 'data', 'settings.json'), JSON.stringify(settings, null, 2))
  const catalog = {
    schemaVersion: 1,
    files: [
      { name: 'gunmayhem2_online.swf', game: 'gunmayhem2', path: 'games/gunmayhem2/gunmayhem2_online.swf', size: 1, sha256: 'ab'.repeat(32) },
      { name: 'DadNMe_online.swf', game: 'dadnme', path: 'games/dadnme/DadNMe_online.swf', size: 1, sha256: sha256(join(games, 'DadNMe_online.swf')).toUpperCase() },
      { name: '../evil.swf', game: 'zombotron', path: 'x', sha256: '00' },
    ],
    settings: {
      gunmayhem2: [{ id: 'mode', type: 'choice', scope: 'all', label: 'Mode', since: '1.0.9', default: 'b',
        choices: [{ id: 'a', label: 'A', args: ['--a'] }, { id: 'b', label: 'B', args: [] }] }],
      dadnme: [],
      zombotron: 'not a list',
    },
  }
  write(join(dir, 'data', 'catalog.json'), JSON.stringify(catalog))
  const site = {
    games: [
      { id: 'gunmayhem2', kind: 'flash', name: 'Gun Mayhem 2', status: 'available', quality: 'A', version: '2', updated: '2026-10-30',
        connect: 'Relay', maintainer: 'Us', synced: { see: 'synced', story: 'partly', odd: 5 }, repo: 'a/b', origin: 'official' },
      { id: 'by-mod-id', modId: 'ruffle-coop', name: 'Shared entry', status: 'testing', quality: '', community: true, origin: 'community',
        author: 'Someone', authorUrl: 'https://github.com/someone/thing', reviewed: true },
      { id: 'newpc', kind: 'pc', name: 'New PC Game', players: '2-4 players', slots: 4, color: '#112233', colorEnd: 'bad',
        cover: 'covers/newpc.png', coverFocus: 0.3, description: 'Soon.' },
      { id: 'newflash', name: 'New Flash Game', cover: 'covers/newflash.webp', coverFocus: 7 },
      { id: 'noname' },
      { id: '../bad', name: 'Bad id' },
    ],
  }
  write(join(dir, 'data', 'site.json'), JSON.stringify({ at: 1, games: JSON.stringify(site) }))
  write(join(dir, 'data', 'covers', 'site-newpc.png'), 'png')
  return config
}

const scenarios = [
  {
    name: 'shipped',
    about: 'the shipped launcher/launcher.json as it is, fresh data folder (reads the real Documents folders)',
    prepare: (dir) => {
      const docs = spawnSync('powershell', ['-NoProfile', '-Command', "[Environment]::GetFolderPath('MyDocuments')"], { encoding: 'utf8' }).stdout.trim()
      for (const d of ['games', 'players'])
        if (!existsSync(join(docs, 'MultiplayerEverythingLauncher', d))) throw new Error(`${docs}\\MultiplayerEverythingLauncher\\${d} doesn't exist; this input would create it`)
      mkdirSync(dir, { recursive: true })
      return join(root, 'launcher', 'launcher.json')
    },
  },
  { name: 'shipped-copy', about: 'the shipped launcher.json in a temp folder, no covers beside it (built-in covers)', prepare: shippedCopy },
  {
    name: 'shipped-user',
    about: 'the same with a settings.json, a games folder of its own, Ruffle Together installed, a cached catalog and site.json',
    needsList: true,
    prepare: shippedUser,
  },
  {
    name: 'shipped-user-nosite',
    about: 'the same with FL_NO_SITE (the website copy is ignored)',
    needsList: true,
    env: { FL_NO_SITE: '1' },
    prepare: shippedUser,
  },
  {
    name: 'damaged-settings',
    about: 'a settings.json that is not JSON (moved aside to settings.json.bad)',
    prepare: (dir) => {
      const config = shippedCopy(dir)
      write(join(dir, 'data', 'settings.json'), '{ broken')
      return config
    },
  },
  {
    name: 'bad-config',
    about: 'a launcher.json with errors: both refuse it with the same message',
    prepare: (dir) => {
      write(join(dir, 'launcher.json'), '{ "players": [], "games": [{ "id": "a b", "nmae": 1, "actions": [] }], "branding": { "theme": "pink" } }')
      return join(dir, 'launcher.json')
    },
  },
  {
    name: 'missing-config',
    about: 'a launcher.json that does not exist',
    prepare: (dir) => {
      mkdirSync(dir, { recursive: true })
      return join(dir, 'nope.json')
    },
  },
  {
    name: 'features',
    about: 'a hand-written launcher.json with what the others lack: HTML5 actions, PATH lookup, wildcards, overrides, a pinned mod',
    prepare: (dir) => {
      for (const f of ['players\\flash_9_sa.exe', 'players\\flash_32_sa.exe', 'players\\edge.exe', 'games\\a.swf', 'games\\b.swf',
        'web\\index.html', 'mod\\server\\server.js', 'node\\node.exe', 'covers\\mine.png', 'players\\rt\\rt.exe'])
        write(join(dir, f), 'x')
      write(join(dir, 'players', 'rt', '.version'), 'V1.0.10 \n')
      const config = {
        branding: { title: 'T', subtitle: 'S', playerColors: ['#F00', '#00ff00', ' #0000FFCC '], theme: 'light', colors: { text: '#123' } },
        paths: { games: '${LAUNCHER_DIR}\\games', players: '${LAUNCHER_DIR}/players/', data: '${env:FL_PARITY_DATA}', flashpoint: ['${LAUNCHER_DIR}\\fp', 'C:\\nope'] },
        rooms: { server: 'example.org:1', servers: 'https://example.org/servers.json' },
        discord: { clientId: '123' },
        downloads: { catalog: '', site: 'http://127.0.0.1:1/games.json' },
        players: [
          { id: 'rt', name: 'RT', paths: ['${PLAYERS_DIR}\\rt\\rt.exe'], coopSaveArg: '--coop-save', coopSaveSince: '1.0.9',
            install: { github: 'o/rt', asset: 'rt-*.zip', dir: '${PLAYERS_DIR}\\rt' } },
          { id: 'flash', paths: ['${FLASHPOINT}\\x.exe', '${PLAYERS_DIR}\\flash_*_sa.exe'], args: ['-v', '${SWF_DIR}'] },
          { id: 'cmd', paths: ['cmd.exe'], website: 'https://example.org' },
          { id: 'edge', kind: 'browser', paths: ['${PLAYERS_DIR}\\edge.exe'], args: ['--app=${URL}'] },
          { id: 'ghost', paths: ['${PLAYERS_DIR}\\ghost.exe'], install: { url: 'https://example.org/g.zip', dir: '${PLAYERS_DIR}\\g' } },
        ],
        categories: [{ id: 'a', icon: '0xE774', group: 'g' }, { id: 'b', name: 'B', icon: ' e775' }],
        games: [
          { id: 'coop', name: 'Coop', players: '2 to 10 players, 3 teams', online: { id: 'ruffle-coop', join: 'online' }, categories: ['a', 'b'],
            image: 'covers\\mine.png', imageFocus: 0.3, color: '#abc', colorEnd: '#A1B2C3FF', _note: 'skipped', $schema: 'x',
            settings: [
              { id: 's1', label: 'S1', args: ['--s1'], default: 'on', since: '1.0.2' },
              { id: 's2', type: 'choice', scope: 'join', label: 'S2', since: '2.0', choices: [{ id: 'x', label: 'X' }, { id: 'y', label: 'Y', args: ['--y'] }], default: 'y' },
            ],
            guide: { needs: 'one', steps: ['a', 'b'], links: [{ label: 'L', url: 'https://example.org' }], keys: [{ key: 'A', does: 'B' }] },
            actions: [
              { id: 'online', label: 'Online', icon: 'E774', swf: ['${GAMES_DIR}\\missing.swf', '${GAMES_DIR}\\a.swf'], players: ['flash', 'rt'],
                args: ['--coop-host-relay', '${SERVER}', '--coop-players', ' 3x'], joinArgs: ['--coop-join-relay', '${CODE}'], flashvars: { a: '1' } },
              { id: 'ip', swf: 'http://localflash/a.swf', args: ['--coop-join', '${ADDRESS}'], more: true, offline: true, edition: 'original' },
              { id: 'ov', overrides: { flash: { swf: ['${GAMES_DIR}\\b.swf'], args: ['--ov'] }, rt: { swf: '${GAMES_DIR}\\none.swf' } },
                relay: { port: 7000 }, players: ['rt', 'flash'] },
              { id: 'cmdplay', swf: '${GAMES_DIR}\\b.swf', players: ['cmd'] },
              { id: 'link', link: 'https://example.org/page' },
            ] },
          { id: 'web', name: 'Web', maxPlayers: 3, actions: [
            { id: 'host', file: '${LAUNCHER_DIR}\\mod\\server\\server.js', server: { name: 'Node', exe: ['${FILE_DIR}\\..\\..\\node\\node.exe'], args: ['s.js'], cwd: '${FILE_DIR}\\..', url: 'http://localhost:2053/', share: 'https://[a-z]+\\.x' } },
            { id: 'nonode', file: '${LAUNCHER_DIR}\\mod\\server\\server.js', server: { exe: ['${LAUNCHER_DIR}\\no\\node.exe'], url: 'http://localhost:1/' } },
            { id: 'join', joinLink: true },
            { id: 'page', page: '${LAUNCHER_DIR}\\web\\index.html' },
            { id: 'remote', page: 'https://example.org/game' },
            { id: 'nopage', page: '${LAUNCHER_DIR}\\web\\none.html' } ] },
          { id: 'pinned', name: 'Pinned', image: 'C:\\nope\\x.png', online: { id: 'pm', lobby: { port: 9000 } },
            mod: { publisher: 'P', website: 'https://example.org', game: { name: 'The Game', executable: 'g.exe', detection: [{ type: 'path', path: 'C:\\nope' }] },
              components: [{ id: 'c', source: 'c' }], package: { url: 'https://example.org/p.zip', sha256: 'ab', version: '1.2.3' } },
            actions: [{ id: 'page', link: 'https://example.org/mod' }] },
          { id: 'soon', comingSoon: true, image: '${LAUNCHER_DIR}\\covers\\mine.png', lobby: 1 },
          { id: 'lob', online: { id: 'lob', join: 'p', lobby: { version: '3' } }, actions: [{ id: 'p', swf: '${GAMES_DIR}\\a.swf' }] },
        ],
        text: { 'nav.all': 'Everything \u00e9', 'custom.key': 'mine', _c: 'x' },
      }
      write(join(dir, 'launcher.json'), '// a note\n' + JSON.stringify(config, null, 2).replace('"branding"', '/* inline */ "branding"'))
      write(join(dir, 'data', 'settings.json'), JSON.stringify({ games: { coop: { settings: { s1: '0', s2: 'x' }, player: 'flash' } }, minimize: 3, online: 'yes' }))
      return join(dir, 'launcher.json')
    },
    env: { FL_PARITY_DATA: join(work, 'features', 'data') },
  },
  {
    name: 'sandbox',
    about: "the lab's sandbox (tests/lab/sandbox.mjs): its launcher.json, settings.json and site.json",
    slow: true,
    prepare: (dir) => {
      if (!existsSync(fakePlayer)) throw new Error(`No fake player at ${fakePlayer}: run old\\build.ps1 -Config Debug`)
      return makeSandbox(dir, fakePlayer).config
    },
  },
]

let failures = 0
let shippedCopyList = null
for (const s of scenarios) {
  if (only && !only.includes(s.name)) continue
  const dir = join(work, s.name)
  const fresh = (list) => {
    rmSync(dir, { recursive: true, force: true })
    rmSync(join(work, 'localappdata'), { recursive: true, force: true })
    return s.prepare(dir, list)
  }
  if (s.needsList && !shippedCopyList) {
    const d = join(work, 'list-for-inputs')
    shippedCopyList = JSON.parse(run(cpp, shippedCopy(d), {}, true).stdout)
  }
  // Each side runs on inputs made afresh, so neither sees what the other wrote (covers, moved settings, folders).
  // The sandbox is slow to build (it zips packages) and both sides only read it, so it is made once.
  let config = fresh(shippedCopyList)
  const a = run(cpp, config, s.env ?? {}, true)
  if (!s.slow) config = fresh(shippedCopyList)
  const r = run(rust, config, s.env ?? {}, false)
  const out = []
  if (a.code !== r.code) out.push(`exit code: C++ ${a.code} / Rust ${r.code}`)
  if (a.stdout || r.stdout) {
    let ja, jr
    try {
      ja = JSON.parse(a.stdout)
      jr = JSON.parse(r.stdout)
    } catch (e) {
      out.push(`output isn't JSON: ${e.message}\nC++: ${a.stdout.slice(0, 300)}\nRust: ${r.stdout.slice(0, 300)}`)
    }
    if (ja && jr) diff(ja, jr, '$', out)
  } else if (a.stderr !== r.stderr) {
    out.push(`error text:\n--- C++\n${a.stderr}\n--- Rust\n${r.stderr}`)
  }
  const games = (() => {
    try {
      return JSON.parse(r.stdout).games.length + ' games'
    } catch {
      return 'refused: ' + r.stderr.split('\n')[0]
    }
  })()
  console.log(`${out.length ? 'FAIL' : 'ok  '} ${s.name}: ${out.length} differences (${games}) - ${s.about}`)
  for (const line of out.slice(0, 200)) console.log('     ' + line)
  if (out.length) failures++
}
if (!keep) rmSync(work, { recursive: true, force: true })
else console.log(`Kept ${work}`)
console.log(failures ? `${failures} input(s) differ` : 'All inputs match')
process.exit(failures ? 1 : 0)
