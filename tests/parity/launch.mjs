// Parity check for part 3 of PORTING.md: the app's Rust launch (src-tauri/src/launcher/launch.rs, through the
// `launch` example, which is the code path of the window's `launch` command) must do exactly what
// `MultiplayerEverythingLauncher.exe --launch <game>:<action> --json --wait ...` does for the same launcher.json and
// settings: the player's arguments, environment and working folder, the exit code and message, settings.json
// afterwards, and the relay (its report lines and what test clients are told), the HTML5 web server and game server.
//
//   node tests/parity/launch.mjs [--keep] [--only name,name]
//
// The player is the C++ test helper's fake player (launcher_tests.exe --fake-player, copied into the temp folder so
// a release tag can sit beside it): it appends its arguments to %FL_FAKE_OUT%, which is a relative name here, so the
// file lands in its working folder. Everything is on loopback and in a temp folder; Discord is off (FL_NO_DISCORD).
import { spawn, spawnSync } from 'node:child_process'
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { createServer } from 'node:http'
import net from 'node:net'
import { tmpdir } from 'node:os'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')
const args = process.argv.slice(2)
const keep = args.includes('--keep')
const only = args.includes('--only') ? args[args.indexOf('--only') + 1].split(',') : null

const win = process.platform === 'win32'
const exe = win ? '.exe' : ''
const cpp = process.env.FL_LAUNCHER_EXE ?? (win ? join(root, 'old', 'build', 'Release', 'MultiplayerEverythingLauncher.exe') : join(root, 'old', 'build-linux', 'MultiplayerEverythingLauncher'))
if (!existsSync(cpp)) throw new Error(`No C++ launcher at ${cpp}: run old\\build.ps1 or set FL_LAUNCHER_EXE`)
const fakeSrc = join(dirname(cpp), `launcher_tests${exe}`)
if (!existsSync(fakeSrc)) throw new Error(`No launcher_tests${exe} next to ${cpp}`)

console.log('Building the Rust side (cargo build --example launch)...')
const b = spawnSync('cargo', ['build', '--example', 'launch', '--quiet'], { cwd: join(root, 'src-tauri'), stdio: 'inherit', shell: true })
if (b.status !== 0) process.exit(1)
const rust = join(root, 'src-tauri', 'target', 'debug', 'examples', `launch${exe}`)

const work = join(tmpdir(), 'fl-parity-launch')
rmSync(work, { recursive: true, force: true })
mkdirSync(work, { recursive: true })
const wait = (ms) => new Promise((r) => setTimeout(r, ms))
function write(file, data) {
  mkdirSync(dirname(file), { recursive: true })
  writeFileSync(file, data)
}

const PORTS = { relay: 48931, rooms: 48932, server: 48933, busy: 48934, web: 48935 }
const NUL = '\0'

// mods-info's servers.json, for ${SERVER} from a server list.
const web = createServer((req, res) => {
  if (req.url.startsWith('/servers.json')) return res.writeHead(200).end(JSON.stringify({ servers: [{ address: '127.0.0.1:48999', status: 'offline' }, { address: 'listed.example:5000', status: 'online' }] }))
  res.writeHead(404).end()
}).listen(PORTS.web, '127.0.0.1')

// --- The case folder -----------------------------------------------------------------------------------------------

const FAKE_MS = 2300
// The fake player; `version` is the release tag beside it (the co-op save and settings' "since" read it).
function prepare(dir, c) {
  rmSync(dir, { recursive: true, force: true })
  const fake = join(dir, 'players', 'fake', 'fake.exe')
  mkdirSync(dirname(fake), { recursive: true })
  copyFileSync(fakeSrc, fake)
  if (c.version) write(join(dir, 'players', 'fake', '.version'), c.version)
  for (const f of ['coop.swf', 'relay.swf', 'plain.swf', 'my dir\\spaced game.swf']) write(join(dir, 'games', f), 'not a real swf')
  write(join(dir, 'games', 'webpage', 'index.html'), '<h1>web demo</h1>')
  write(join(dir, 'games', 'webgame', 'server.js'), '// stand-in')
  const swf = (name) => [`\${GAMES_DIR}\\${name}`]
  const fakePath = '${PLAYERS_DIR}\\fake\\fake.exe'
  const config = {
    paths: { games: '${LAUNCHER_DIR}\\games', players: '${LAUNCHER_DIR}\\players', data: '${LAUNCHER_DIR}\\data', flashpoint: [] },
    rooms: c.rooms ?? { server: `127.0.0.1:${PORTS.rooms}` },
    downloads: { catalog: '', site: '' },
    players: [
      { id: 'fake', name: 'Fake Player', paths: [fakePath], args: ['--fake-player'], flashvarArg: '-P${KEY}=${VALUE}', coopSaveArg: '--coop-save', coopSaveSince: '1.0.4',
        tv: { args: ['--fullscreen', '--tv=${GAME_ID}'], env: { FAKE_TV: '1', FAKE_SECOND: 'two words' } } },
      { id: 'second', name: 'Second Player', paths: [fakePath], args: ['--fake-player', '--second'] },
      { id: 'gone', name: 'Missing Player', paths: ['${PLAYERS_DIR}\\gone\\gone.exe'] },
      { id: 'browser', name: 'Fake Browser', kind: 'browser', paths: [fakePath], args: ['--fake-player', '--app=${URL}'] },
    ],
    categories: [{ id: 'online', name: 'Online' }],
    games: [
      {
        id: 'coop', name: 'Co-op Test', online: { id: 'ruffle-coop', join: 'online' },
        settings: [
          { id: 'ff', type: 'checkbox', scope: 'host', label: 'Friendly fire', args: ['--friendly-fire'] },
          { id: 'speed', type: 'choice', scope: 'all', label: 'Speed', default: 'normal',
            choices: [{ id: 'normal', label: 'Normal', args: [] }, { id: 'fast', label: 'Fast', args: ['--fast'] }] },
          { id: 'newer', type: 'checkbox', scope: 'all', label: 'Needs 2.0', default: 'on', since: '2.0', args: ['--newer'] },
          { id: 'joinonly', type: 'checkbox', scope: 'join', label: 'Join only', default: 'on', args: ['--join-only'] },
        ],
        actions: [
          { id: 'online', label: 'Play online', swf: swf('coop.swf'), hint: 'Send the room code the game shows to your friend.',
            args: ['--coop-host-relay', '${SERVER}', '--coop-name', '${PLAYER_NAME}', '--coop-players', '2'],
            joinArgs: ['--coop-join-relay', '${SERVER}', '--coop-code', '${CODE}', '--coop-name', '${PLAYER_NAME}'] },
          { id: 'host', label: 'Host with your IP', swf: swf('coop.swf'), args: ['--coop-host', '7790'] },
          { id: 'joinip', label: 'Join by IP', swf: swf('coop.swf'), args: ['--coop-join', '${ADDRESS}'] },
          { id: 'needcode', label: 'Needs a code', swf: swf('coop.swf'), args: ['--code', '${CODE}'] },
          { id: 'solo', label: 'Play solo', swf: swf('coop.swf') },
        ],
      },
      {
        id: 'relaygame', name: 'Relay Test',
        actions: [
          { id: 'play', label: 'Play', swf: swf('relay.swf') },
          { id: 'hostrelay', label: 'Host with the relay', swf: swf('relay.swf'), args: ['--tcp-connections', 'allow', '--port=${PORT}'], relay: { port: PORTS.relay } },
          { id: 'second', label: 'In the second player', swf: swf('relay.swf'), players: ['second'] },
        ],
      },
      {
        id: 'flash', name: 'FlashVars Test',
        actions: [
          { id: 'play', label: 'Play', swf: swf('my dir\\spaced game.swf'), args: ['--title', 'A "quoted" title', '', 'C:\\trailing\\'],
            flashvars: { online: '1', name: '${PLAYER_NAME}', empty: '' } },
          { id: 'joinargs', label: 'Join args', swf: swf('plain.swf'), flashvars: { online: '1' }, joinArgs: ['--joined'] },
        ],
      },
      { id: 'nofile', name: 'No File', actions: [{ id: 'play', label: 'Play', swf: swf('nowhere.swf') }] },
      { id: 'noplayer', name: 'No Player', actions: [{ id: 'play', label: 'Play', swf: swf('plain.swf'), players: ['gone'] }] },
      { id: 'link', name: 'Link Test', actions: [{ id: 'site', label: 'Open the site', link: 'https://example.org/game' }] },
      { id: 'soon', name: 'Coming Test', comingSoon: true, actions: [{ id: 'play', label: 'Play', swf: swf('plain.swf') }] },
      {
        id: 'web', name: 'Web Demo',
        actions: [
          { id: 'page', label: 'Play page', page: '${GAMES_DIR}\\webpage\\index.html' },
          { id: 'host', label: 'Host co-op', file: '${GAMES_DIR}\\webgame\\server.js',
            server: { name: 'Fake server', exe: [fakePath], args: ['--fake-server', String(PORTS.server)], url: `http://127.0.0.1:${PORTS.server}/` } },
          { id: 'quits', label: 'Server quits', file: '${GAMES_DIR}\\webgame\\server.js',
            // launcher_tests.exe with only --fake-server runs no tests and exits at once.
            server: { name: 'Fake server', exe: [fakePath], args: ['--fake-server'], url: `http://127.0.0.1:${PORTS.busy}/` } },
          { id: 'noserver', label: 'No server program', file: '${GAMES_DIR}\\webgame\\server.js',
            server: { name: 'Node.js', exe: ['${GAMES_DIR}\\node\\node.exe'], url: 'http://127.0.0.1:1/' } },
          { id: 'join', label: 'Join with a link', joinLink: true },
        ],
      },
    ],
    ...(c.config ?? {}),
  }
  write(join(dir, 'launcher.json'), c.rawConfig ?? JSON.stringify(config, null, 2))
  write(join(dir, 'data', 'settings.json'), JSON.stringify(c.settings ?? { playerName: 'Tester', minimize: 0, extraKey: 'dropped', games: { coop: { settings: { ff: '1' } } } }))
  if (c.catalog) write(join(dir, 'data', 'catalog.json'), JSON.stringify(c.catalog))
  return join(dir, 'launcher.json')
}

// Where the fake player wrote its arguments: its working folder, relative to the case folder.
function findOut(dir) {
  const hits = []
  const walk = (d) => {
    for (const n of readdirSync(d)) {
      const p = join(d, n)
      if (statSync(p).isDirectory()) walk(p)
      else if (n === 'fake-out.txt') hits.push(p)
    }
  }
  walk(dir)
  return hits.map((p) => ({ folder: relative(dir, dirname(p)) || '.', lines: readFileSync(p, 'utf8').split('\n').filter(Boolean) }))
}

// --- Test clients --------------------------------------------------------------------------------------------------

const portOpen = (port, host = '127.0.0.1') => new Promise((r) => {
  const s = net.connect(port, host, () => { s.destroy(); r(true) })
  s.on('error', () => r(false))
})
async function waitPort(port, ms = 8000) {
  const until = Date.now() + ms
  while (Date.now() < until) {
    if (await portOpen(port)) return true
    await wait(100)
  }
  return false
}

// A relay client that keeps every NUL-terminated message it gets, and whether the relay hung up.
function client(port) {
  return new Promise((resolve) => {
    const got = []
    let buf = ''
    const s = net.connect(port, '127.0.0.1', () => resolve(c))
    const c = {
      got, closed: false,
      send: (m) => s.write(m + NUL),
      raw: (t) => s.write(t),
      close: () => s.destroy(),
      // The next message, or "<closed>" / "<timeout>".
      next: async (ms = 3000) => {
        const until = Date.now() + ms
        while (!got.length && !c.closed && Date.now() < until) await wait(20)
        return got.length ? got.shift() : c.closed ? '<closed>' : '<timeout>'
      },
    }
    s.on('data', (d) => {
      buf += d.toString('utf8')
      let i
      while ((i = buf.indexOf(NUL)) >= 0) {
        got.push(buf.slice(0, i))
        buf = buf.slice(i + 1)
      }
    })
    s.on('close', () => (c.closed = true))
    s.on('error', () => (c.closed = true))
  })
}

// What the relay says to a game and its friends, step by step, while the game runs.
async function relayTalk(t) {
  if (!(await waitPort(PORTS.relay))) return t.push('relay never opened')
  const pol = await client(PORTS.relay)
  pol.raw('<policy-fi')
  await wait(150)
  pol.raw('le-request/>' + NUL)
  t.push('policy on the game port: ' + (await pol.next()))
  t.push('then: ' + (await pol.next()))
  const p843 = await portOpen(843)
  if (p843) {
    const p = await client(843)
    p.raw('anything')
    t.push('843: ' + (await p.next()) + ' / ' + (await p.next()))
  } else t.push('843: not listening')
  const early = await client(PORTS.relay)
  early.send('J|Early')
  const earlyW = await early.next()
  t.push('early guest: ' + earlyW.replace(/\d+$/, '<id>'))
  const host = await client(PORTS.relay)
  host.send('H|Game Host')
  t.push('host: ' + (await host.next()) + ', ' + (await host.next()).replace(/^J\|\d+/, 'J|<id>'))
  await wait(1300)
  const friend = await client(PORTS.relay)
  friend.send('\r\nJ|Friend \u00e9')
  t.push('friend: ' + (await friend.next()).replace(/\d+$/, '<id>'))
  t.push('host told: ' + (await host.next()).replace(/^J\|\d+/, 'J|<id>'))
  host.send('F|12|keys')
  t.push('guests get: ' + (await friend.next()) + ' / ' + (await early.next()))
  friend.send('I|3|left')
  t.push('host gets: ' + (await host.next()).replace(/^G\|\d+/, 'G|<id>'))
  friend.send('D|debug')
  friend.send('I|4')
  t.push('after a debug line: ' + (await host.next()).replace(/^G\|\d+/, 'G|<id>'))
  const second = await client(PORTS.relay)
  second.send('H|Mallory')
  t.push('second host: ' + (await second.next()))
  await wait(1300)
  friend.close()
  t.push('friend left: ' + (await host.next()).replace(/^L\|\d+/, 'L|<id>'))
  await wait(1300)
  host.close()
  t.push('host left: ' + (await early.next()))
  await wait(1300)
  early.close(); second.close(); pol.close()
}

// --- The cases -----------------------------------------------------------------------------------------------------

const plainSettings = { playerName: 'Tester', minimize: 0 }
const cases = [
  { name: 'play', about: 'a plain play: the arguments, the working folder, plays and time in settings.json', target: 'relaygame:play' },
  { name: 'play-first', about: 'no action given: the first one', target: 'relaygame' },
  { name: 'second-player', about: "an action that names its player", target: 'relaygame:second' },
  { name: 'quoting', about: 'spaces, quotes, an empty argument and a trailing backslash; FlashVars before the file', target: 'flash:play' },
  { name: 'flashvars-join', about: 'a room code as a FlashVar (-Pjoin=)', target: 'flash:play', extra: ['--join', 'ab cd e'] },
  { name: 'flashvars-joinargs', about: "an action with joinArgs gets no join FlashVar", target: 'flash:joinargs', extra: ['--join', 'abcde'] },
  { name: 'host-relay', about: 'hosting with the relay: report lines, policy file, routing, join/leave, host left', target: 'relaygame:hostrelay',
    ms: 9500, during: relayTalk },
  { name: 'relay-port-in-use', about: 'the relay port is taken: no relay, the game still starts', target: 'relaygame:hostrelay', busy: PORTS.relay },
  { name: 'join-code', about: '--join with lower case and spaces: joinArgs, ${CODE}, ${SERVER}, ${PLAYER_NAME}', target: 'coop:online', extra: ['--join', ' ab c-de ' ] },
  // Windows only: Linux's C++ engine runs in the C locale, where only ASCII counts as a letter. Real room codes are ASCII.
  ...(win ? [{ name: 'join-code-unicode', about: '--join with letters outside ASCII', target: 'coop:online', extra: ['--join', '\u00e9a1\u00df\u03c9\u00b2\u0663\u216b\u4e2d_.'] }] : []),
  { name: 'host-server-list', about: "${SERVER} from servers.json's first online server", target: 'coop:online',
    rooms: { servers: `http://127.0.0.1:${PORTS.web}/servers.json`, server: '127.0.0.1:1' } },
  { name: 'host-server-list-down', about: 'servers.json unreachable: rooms.server', target: 'coop:online',
    rooms: { servers: 'http://127.0.0.1:1/servers.json', server: 'fallback.example:7' } },
  { name: 'no-server', about: '${SERVER} with no server at all', target: 'coop:online', rooms: {} },
  { name: 'address', about: '--address fills ${ADDRESS}', target: 'coop:joinip', extra: ['--address', '10.0.0.5:7790'] },
  { name: 'address-missing', about: 'no --address for ${ADDRESS}', target: 'coop:joinip' },
  { name: 'code-missing', about: 'no --join for ${CODE}', target: 'coop:needcode' },
  { name: 'no-name', about: 'no player name: ${PLAYER_NAME} empty', target: 'coop:online', settings: { minimize: 0 } },
  { name: 'coop-save-solo', about: 'host from the solo save, player new enough', target: 'coop:online', extra: ['--coop-save', 'solo'], version: 'v1.0.5' },
  { name: 'coop-save-new', about: 'host a new game, player exactly coopSaveSince', target: 'coop:online', extra: ['--coop-save', 'new'], version: '1.0.4\r\n' },
  { name: 'coop-save-old-player', about: 'a player older than coopSaveSince: no option', target: 'coop:online', extra: ['--coop-save', 'new'], version: '1.0.3' },
  { name: 'coop-save-no-version', about: "a player the launcher didn't install: no option", target: 'coop:online', extra: ['--coop-save', 'solo'] },
  { name: 'coop-save-joining', about: 'joining never takes the save option', target: 'coop:online', extra: ['--coop-save', 'new', '--join', 'XYZ12'], version: '9' },
  { name: 'coop-save-continue', about: '--coop-save continue', target: 'coop:online', extra: ['--coop-save', 'continue'], version: '9' },
  { name: 'coop-save-bad', about: '--coop-save with an unknown value', target: 'coop:online', extra: ['--coop-save', 'sideways'] },
  { name: 'settings-host', about: 'game settings: host scope on a host action, all scope, since gate passed', target: 'coop:host', version: '2.1',
    settings: { playerName: 'Tester', games: { coop: { settings: { ff: '1', speed: 'fast' } } } } },
  { name: 'settings-solo', about: 'game settings on an action that neither hosts nor joins', target: 'coop:solo', version: '2.1',
    settings: { playerName: 'Tester', games: { coop: { settings: { ff: '1', speed: 'gone' } } } } },
  { name: 'settings-catalog', about: "the cached catalog's settings replace launcher.json's", target: 'coop:host', version: '1.0',
    catalog: { files: [], settings: { coop: [{ id: 'cat', label: 'From the catalog', default: 'on', args: ['--from-catalog'] },
      { id: 'diff', type: 'choice', label: 'Difficulty', scope: 'all', default: 'hard', choices: [{ id: 'easy', label: 'Easy', args: ['--d', '0'] }, { id: 'hard', label: 'Hard', args: ['--d', '2'] }] }] } } },
  { name: 'tv', about: "--tv: the player's TV arguments and environment (one replacing a variable already set)", target: 'relaygame:play', extra: ['--tv'] },
  { name: 'tv-web', about: '--tv on an HTML5 action: nothing added', target: 'web:page', extra: ['--tv'] },
  { name: 'no-file', about: 'the game file is missing', target: 'nofile:play' },
  { name: 'no-player', about: 'the player is missing', target: 'noplayer:play' },
  { name: 'link', about: 'a link action is refused', target: 'link:site' },
  { name: 'coming-soon', about: 'a coming-soon game', target: 'soon:play' },
  { name: 'no-game', about: 'no such game', target: 'nosuch:play' },
  { name: 'no-action', about: 'no such action', target: 'coop:nosuch' },
  { name: 'bad-config', about: 'launcher.json with problems', target: 'coop:solo', rawConfig: '{ "players": [{ "id": "x" }], "games": [{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf", "players": ["nobody"] }] }] }' },
  { name: 'edited-mid-game', about: 'settings.json edited while the game runs: kept, the time added on top', target: 'relaygame:play', ms: 3300,
    during: async (t, dir) => {
      await wait(1200)
      const f = join(dir, 'data', 'settings.json')
      const s = JSON.parse(readFileSync(f, 'utf8'))
      s.playerName = 'Changed'
      s.games.coop = { favorite: true, settings: { ff: '0' } }
      s.games.relaygame.player = 'second'
      writeFileSync(f, JSON.stringify(s))
      t.push('edited')
    } },
  { name: 'web-page', about: 'an HTML5 page: served on 127.0.0.1 while the browser runs, gone after', target: 'web:page', ms: 3000,
    during: async (t, dir) => {
      let app = null
      const until = Date.now() + 8000
      while (!app && Date.now() < until) {
        const out = findOut(dir)[0]
        app = out?.lines.find((l) => l.startsWith('--app='))
        if (!app) await wait(100)
      }
      if (!app) return t.push('no --app argument')
      const url = app.slice(6)
      const r = await fetch(url).then(async (x) => `${x.status} ${x.headers.get('content-type')} ${await x.text()}`).catch((e) => String(e))
      t.push('page: ' + r)
      const range = await fetch(url, { headers: { Range: 'bytes=4-6' } }).then(async (x) => `${x.status} ${x.headers.get('content-range')} ${await x.text()}`).catch((e) => String(e))
      t.push('range: ' + range)
      const outside = await fetch(url.replace('index.html', '..%5c..%5clauncher.json')).then((x) => x.status).catch((e) => String(e))
      t.push('outside the folder: ' + outside)
      t.port = Number(new URL(url).port)
    },
    after: async (t) => t.push('page server after: ' + (t.port ? ((await portOpen(t.port)) ? 'open' : 'closed') : '?')) },
  { name: 'web-server', about: "an HTML5 game's server: started first, waited for, the browser opens its URL, stopped after", target: 'web:host',
    after: async (t) => {
      await wait(300)
      t.push('server after: ' + ((await portOpen(PORTS.server)) ? 'open' : 'closed'))
    } },
  { name: 'web-server-quits', about: "a game's server that quits at once", target: 'web:quits' },
  { name: 'web-no-runtime', about: "a game's server program that isn't there", target: 'web:noserver' },
  { name: 'web-join-link', about: 'Join with a link needs the window', target: 'web:join' },
  { name: 'stop-hosting', about: 'Stop hosting while the relay runs (the C++ launcher killed): no time added, the relay closes', target: 'relaygame:hostrelay',
    ms: 6000, stopAfter: 2500,
    during: async (t) => {
      await waitPort(PORTS.relay)
      const h = await client(PORTS.relay)
      h.send('H|Host')
      t.push('host: ' + (await h.next()))
      await wait(2500)
      t.push('relay after stop: ' + ((await portOpen(PORTS.relay)) ? 'open' : 'closed') + ', host connection ' + (h.closed ? 'closed' : 'open'))
      h.close()
    } },
]

// --- Running both sides --------------------------------------------------------------------------------------------

const stamp = /^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} (DEBUG|INFO |WARN |ERROR) /
function run(isCpp, c, dir) {
  const cfg = prepare(dir, c)
  const env = {
    ...process.env, FL_NO_DISCORD: '1', FL_NO_SITE: '1', LOCALAPPDATA: join(work, 'localappdata'), FL_LAUNCHER_CONFIG: cfg,
    FL_FAKE_OUT: 'fake-out.txt', FL_FAKE_MS: String(c.ms ?? FAKE_MS), FL_FAKE_ENV: 'FAKE_TV;FAKE_SECOND;FL_PARITY_KEEP', fake_tv: '0', FL_PARITY_KEEP: 'kept',
  }
  // The window passed FL_LAUNCHER_CONFIG to the C++ launcher as --config, with --json and --wait (src-tauri/src/lib.rs).
  const argv = isCpp ? ['--config', cfg, '--launch', c.target, '--json', '--wait', ...(c.extra ?? [])]
    : [c.target, '--wait', ...(c.extra ?? []), ...(c.stopAfter ? ['--stop-after', String(c.stopAfter)] : [])]
  return new Promise(async (res) => {
    let busy = null
    if (c.busy) busy = await new Promise((r) => { const s = net.createServer().listen(c.busy, '0.0.0.0', () => r(s)) })
    const transcript = []
    const p = spawn(isCpp ? cpp : rust, argv, { env, cwd: work })
    let stdout = '', stderr = ''
    p.stdout.on('data', (d) => (stdout += d))
    p.stderr.on('data', (d) => (stderr += d))
    if (isCpp && c.stopAfter) setTimeout(() => p.kill(), c.stopAfter)
    const during = c.during ? c.during(transcript, dir) : Promise.resolve()
    p.on('close', async (code) => {
      await during
      if (c.after) await c.after(transcript)
      if (busy) busy.close()
      // The fake player may outlive a stopped launcher: give it time to finish.
      await wait(c.stopAfter ? (c.ms ?? FAKE_MS) : 100)
      let settings = null
      try { settings = readFileSync(join(dir, 'data', 'settings.json'), 'utf8') } catch { }
      res({
        code: c.stopAfter ? 'stopped' : code,
        stdout: stdout.replace(/\r\n/g, '\n').trim().split('\n').filter(Boolean),
        message: stderr.replace(/\r\n/g, '\n').split('\n').filter((l) => !stamp.test(l)).join('\n').trim(),
        out: findOut(dir),
        settings,
        transcript,
      })
    })
  })
}

// seconds and lastPlayed are clocks: checked for range, then blanked.
function normSettings(text, c) {
  if (text === null) return { text: null, problems: [] }
  const problems = []
  const j = JSON.parse(text)
  const now = Math.floor(Date.now() / 1000)
  for (const [id, g] of Object.entries(j.games ?? {})) {
    if (g.lastPlayed && Math.abs(now - g.lastPlayed) > 120) problems.push(`${id}.lastPlayed ${g.lastPlayed} isn't now`)
    const ms = c.stopAfter ? 0 : c.ms ?? FAKE_MS
    if (g.seconds && !(g.seconds >= Math.floor(ms / 1000) && g.seconds <= Math.floor(ms / 1000) + 1)) problems.push(`${id}.seconds ${g.seconds} for a ${ms} ms game`)
  }
  return { text: text.replace(/"lastPlayed": [1-9]\d*/g, '"lastPlayed": <now>').replace(/"seconds": \d+/g, '"seconds": <n>'), problems, plays: Object.fromEntries(Object.entries(j.games ?? {}).map(([k, g]) => [k, [g.plays, g.seconds]])) }
}

// The relay's report lines, with the message line apart.
function split(lines) {
  const reports = [], other = []
  for (const l of lines) {
    try {
      const j = JSON.parse(l)
      if (j.relay) { reports.push(l); continue }
    } catch { }
    other.push(l)
  }
  return { reports, other }
}

let failures = 0, ran = 0
for (const c of cases) {
  if (only && !only.includes(c.name)) continue
  ran++
  const dir = join(work, 'case')
  const a = await run(true, c, dir)
  await wait(300)
  const r = await run(false, c, dir)
  await wait(300)
  const diffs = []
  const cmp = (what, x, y) => {
    const sx = JSON.stringify(x, null, 1), sy = JSON.stringify(y, null, 1)
    if (sx !== sy) diffs.push(`${what}:\n--- C++\n${sx}\n--- Rust\n${sy}`)
  }
  cmp('exit code', a.code, r.code)
  cmp('message', a.message, r.message)
  const sa = split(a.stdout), sr = split(r.stdout)
  cmp('stdout', sa.other, sr.other)
  cmp('relay reports', sa.reports, sr.reports)
  cmp('player (working folder, arguments, environment)', a.out, r.out)
  const na = normSettings(a.settings, c), nr = normSettings(r.settings, c)
  cmp('settings.json', na.text, nr.text)
  for (const p of [...na.problems.map((x) => 'C++ ' + x), ...nr.problems.map((x) => 'Rust ' + x)]) diffs.push(p)
  cmp('relay and servers', a.transcript, r.transcript)
  const gist = a.code !== 0 && a.code !== 'stopped' ? `exit ${a.code}: ${a.message.split('\n')[0].slice(0, 90)}`
    : `exit ${a.code}, ${a.out.length ? a.out[0].lines.length + ' player lines in ' + a.out[0].folder : 'no player'}, ${sa.reports.length} reports, plays/seconds ${JSON.stringify(na.plays ?? {})}`
  console.log(`${diffs.length ? 'FAIL' : 'ok  '} ${c.name}: ${diffs.length} differences (${gist}) - ${c.about}`)
  for (const d of diffs) console.log('     ' + d.replace(/\n/g, '\n     '))
  if (process.env.PARITY_SHOW) console.log(JSON.stringify(a, null, 1))
  if (diffs.length) failures++
}
web.close()
if (!keep) rmSync(work, { recursive: true, force: true })
else console.log(`Kept ${work}`)
console.log(failures ? `${failures} of ${ran} case(s) differ` : `All ${ran} cases match`)
process.exit(failures ? 1 : 0)
