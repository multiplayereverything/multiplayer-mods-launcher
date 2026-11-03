// Parity check for part 2 of PORTING.md: the app's Rust rooms (src-tauri/src/launcher/rooms.rs) must answer exactly
// what `MultiplayerEverythingLauncher.exe --rooms <id> --json` and `--rooms all --json` answer for the same
// launcher.json and the same server answers: the JSON (compared value by value), the exit code, the error text, and
// the requests each side sent to the room server, in order.
//
//   node tests/parity/rooms.mjs [--keep] [--only name,name]
//
// Every server is a stand-in on loopback (a room server that speaks both protocols, an older one that only knows
// RELAY|1, a web server for mods-info's servers.json); the real central server is never asked. The data folder and
// LOCALAPPDATA are in a temp folder. Needs the C++ launcher's Release build (old\build.ps1, or FL_LAUNCHER_EXE).
import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { createServer } from 'node:http'
import net from 'node:net'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')
const args = process.argv.slice(2)
const keep = args.includes('--keep')
const only = args.includes('--only') ? args[args.indexOf('--only') + 1].split(',') : null

const cpp = process.env.FL_LAUNCHER_EXE ?? join(root, 'old', 'build', 'Release', 'MultiplayerEverythingLauncher.exe')
if (!existsSync(cpp)) throw new Error(`No C++ launcher at ${cpp}: run old\\build.ps1 or set FL_LAUNCHER_EXE`)

console.log('Building the Rust side (cargo build --example rooms_json)...')
const b = spawnSync('cargo', ['build', '--example', 'rooms_json', '--quiet'], { cwd: join(root, 'src-tauri'), stdio: 'inherit', shell: true })
if (b.status !== 0) process.exit(1)
const rust = join(root, 'src-tauri', 'target', 'debug', 'examples', 'rooms_json.exe')

const work = join(tmpdir(), 'fl-parity-rooms')
rmSync(work, { recursive: true, force: true })
mkdirSync(work, { recursive: true })
function write(file, data) {
  mkdirSync(dirname(file), { recursive: true })
  writeFileSync(file, data)
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms))

// --- The stand-in servers ------------------------------------------------------------------------------------------

// What the room server was asked, in order, during one run.
let asked = []

// Each game id on the server answers its own way. `v2` answers RELAY|2|ROOMS, `v1` RELAY|1|LIST; a function gets
// the socket and does what it likes (slow, silent, half an answer).
const NUL = '\0'
const answers = {
  // An open room, a full one, a locked one, one whose game started, regions and pings, a name with commas and one
  // in another script, a version, an empty ping, a room in a lobby without a region.
  busy: {
    v2: 'ROOMS|ABCDE,1,4,0,lobby,EU,40,1.0#ab,Test room;FULL1,4,4,0,lobby,US,80,1,Full room;LOCK1,2,4,1,lobby,NL,12,1,Friday, late, locked;'
      + 'GAME1,3,4,0,ingame,JP,250,1,\u6e38\u620f \u2603;NOPNG,1,2,0,,,,v,No ping;NEG01,-1,0,0,LOBBY,eu,-5,,Odd numbers',
    v1: 'LIST|ABCDE,1,4,Test room;FULL1,4,4,Full room',
  },
  empty: { v2: 'ROOMS|', v1: 'LIST|' },
  // Rows the parser skips: too few fields, no code, an empty row; numbers it reads as atoi does; a trailing ';'.
  edge: { v2: 'ROOMS|SHORT,1,2,0;,1,2,0,lobby,EU,1,1,No code;;OK001, 3x,+4,1,ingame,EU, 7ms,1,Spaces,;99999999999,99999999999,-99999999999,2,x,y,z,w,Big;' },
  unknown: { v2: "E|Unknown game 'unknown' on this server" },
  bare: { v2: 'E|' },
  garbage: { v2: 'HELLO THERE' },
  // Bytes that aren't UTF-8 in a name and an error.
  badutf8: { v2: Buffer.concat([Buffer.from('ROOMS|BAD01,1,2,0,lobby,EU,1,1,'), Buffer.from([0xff, 0xfe, 0x6f, 0x6b, 0xe2, 0x82, 0x20, 0xed, 0xa0, 0x80, 0xc0, 0xaf, 0xf0, 0x9f, 0x98])]) },
  badutf8err: { v2: Buffer.concat([Buffer.from('E|Unknown '), Buffer.from([0xc3, 0x28, 0xe9])]) },
  // An answer that ends with the connection instead of a NUL, and one with text after its NUL.
  nonul: { v2: 'ROOMS|NONUL,1,2,0,lobby,EU,1,1,No NUL', end: true },
  afternul: { v2: 'ROOMS|AFTER,1,2,0,lobby,EU,1,1,Before' + NUL + 'ROOMS|NOPE,1,2,0,,,,,After' },
  nulfirst: { v2: NUL + 'ROOMS|X,1,2,0,,,,,y' },
  // Says nothing and hangs up; answers after the 4 s wait; sends half and then goes quiet past the wait.
  silent: { fn: (s) => s.end() },
  slow: { fn: (s) => setTimeout(() => s.end('ROOMS|LATE1,1,2,0,lobby,EU,1,1,Too late' + NUL), 5000) },
  half: { fn: (s) => { s.write('ROOMS|HALF1,1,2,0,lobby,EU,1,1,Half'); setTimeout(() => s.end(';REST1,1,2,0,,,,,Rest' + NUL), 5000) } },
  // Pieces a second apart, each inside the per-read wait.
  drip: { fn: (s) => { s.write('ROOMS|DRIP1,1,'); setTimeout(() => s.write('2,0,lobby,EU,1,1,Drip'), 1000); setTimeout(() => s.end(NUL), 2000) } },
}

function roomServer(older) {
  const sockets = new Set()
  const srv = net.createServer((s) => {
    sockets.add(s)
    s.on('close', () => sockets.delete(s))
    s.on('error', () => {})
    let buf = Buffer.alloc(0)
    s.on('data', (d) => {
      buf = Buffer.concat([buf, d])
      const nul = buf.indexOf(0)
      if (nul < 0) return
      const req = buf.subarray(0, nul).toString('utf8')
      asked.push((older ? 'old ' : '') + req)
      const m = /^RELAY\|(\d)\|(ROOMS|LIST)\|(.*)$/.exec(req)
      if (!m) return s.end('E|Bad request' + NUL)
      const [, ver, , game] = m
      if (older && ver === '2') return s.end('E|Bad handshake: this server speaks RELAY|1' + NUL)
      const a = answers[game.replace(/^old-/, '')]
      if (!a) return s.end(`E|Unknown game '${game}'` + NUL)
      if (a.fn) return a.fn(s)
      const text = ver === '2' || !a.v1 ? a.v2 : a.v1
      s.end(Buffer.concat([Buffer.from(text), a.end ? Buffer.alloc(0) : Buffer.from(NUL)]))
    })
  })
  return { srv, sockets }
}

const listen = (srv) => new Promise((r) => srv.listen(0, '127.0.0.1', () => r(srv.address().port)))
const rooms = roomServer(false)
const older = roomServer(true)
const roomsPort = await listen(rooms.srv)
const olderPort = await listen(older.srv)
// A port nobody listens on: taken, then let go.
const downPort = await (async () => {
  const s = net.createServer()
  const p = await listen(s)
  await new Promise((r) => s.close(r))
  return p
})()
const ROOMS = `127.0.0.1:${roomsPort}`
const OLDER = `127.0.0.1:${olderPort}`
const DOWN = `127.0.0.1:${downPort}`

// mods-info's servers.json, in every shape the launcher meets.
const files = {
  '/servers.json': { servers: [{ name: 'Old', address: DOWN, status: 'offline' }, { name: 'Main', address: ROOMS, status: 'online' }, { address: OLDER, status: 'online' }] },
  '/nostatus.json': { servers: [{ address: 5 }, { address: ROOMS, status: 7 }] },
  '/older.json': { servers: [{ address: OLDER }] },
  '/none-online.json': { servers: [{ address: ROOMS, status: 'offline' }, { status: 'online' }, 'text'] },
  '/no-list.json': { servers: { address: ROOMS } },
  '/array.json': [{ address: ROOMS }],
  '/empty-address.json': { servers: [{ address: '', status: 'online' }] },
  '/no-port.json': { servers: [{ address: '127.0.0.1', status: 'online' }] },
  '/down.json': { servers: [{ address: DOWN, status: 'online' }] },
  '/bom.json': '\ufeff' + JSON.stringify({ servers: [{ address: ROOMS }] }),
  '/comments.json': '// mods-info\n{ "servers": [ /* main */ { "address": "' + ROOMS + '" } ] }',
  '/invalid.json': 'this is not JSON {',
  '/empty.json': '',
}
const web = createServer((req, res) => {
  const url = req.url.split('?')[0]
  if (url === '/redirect.json') return res.writeHead(302, { Location: '/servers.json' }).end()
  if (url === '/ratelimit.json') return res.writeHead(429, { 'Retry-After': '600' }).end()
  if (url === '/forbidden.json') return res.writeHead(403).end()
  if (url === '/error.json') return res.writeHead(500).end('oops')
  if (url === '/big.json') {
    res.writeHead(200)
    res.end(Buffer.alloc(5 * 1024 * 1024, 0x20))
    return
  }
  const f = files[url]
  if (f === undefined) return res.writeHead(404).end()
  res.writeHead(200, { 'Content-Type': 'application/json' }).end(typeof f === 'string' ? f : JSON.stringify(f))
})
const webPort = await listen(web)
const WEB = `http://127.0.0.1:${webPort}`
const DOWNWEB = `http://127.0.0.1:${downPort}`

// --- The inputs ----------------------------------------------------------------------------------------------------

const game = (id, onlineId, extra = {}) => ({
  id, name: `Game ${id}`, ...(onlineId ? { online: { id: onlineId, join: 'play' } } : {}), actions: [{ id: 'play', swf: '${GAMES_DIR}\\g.swf' }], ...extra,
})
// Every behaviour of the room server as a game, two games sharing an online id, a game without one, one coming soon.
const everyGame = [
  ...Object.keys(answers).map((k) => game(`g-${k}`, k)),
  game('twin-a', 'busy'), game('twin-b', 'busy'),
  game('notonline', ''),
  game('soon', 'busy', { comingSoon: true }),
  game('missing', 'notonserver'),
]
const fastGames = everyGame.filter((g) => !['g-slow', 'g-half'].includes(g.id))
const pick = (...ids) => ids.map((id) => everyGame.find((g) => g.id === id))
// A listed game, one the server doesn't run, and one whose server says only "E|" (whose error the C++ `all` fills
// with why servers.json couldn't be read).
const three = pick('g-busy', 'g-unknown', 'g-bare')

function config(dir, rooms, games = fastGames) {
  const c = {
    paths: { games: '${LAUNCHER_DIR}\\games', players: '${LAUNCHER_DIR}\\players', data: '${LAUNCHER_DIR}\\data' },
    rooms,
    downloads: { catalog: '', site: '' },
    players: [{ id: 'p', paths: ['${PLAYERS_DIR}\\p.exe'] }],
    games,
  }
  write(join(dir, 'launcher.json'), JSON.stringify(c, null, 2))
  return join(dir, 'launcher.json')
}

// The shipped launcher.json with its folders and servers moved onto the stand-ins.
function shipped(dir, rooms) {
  let text = readFileSync(join(root, 'launcher', 'launcher.json'), 'utf8')
  const before = text
  text = text.replace('"games": "${folder:Documents}\\\\MultiplayerEverythingLauncher\\\\games"', '"games": "${LAUNCHER_DIR}\\\\games", "data": "${LAUNCHER_DIR}\\\\data"')
  text = text.replace('"players": "${folder:Documents}\\\\MultiplayerEverythingLauncher\\\\players"', '"players": "${LAUNCHER_DIR}\\\\players"')
  text = text.replace(/"rooms": \{[^}]*\}/, `"rooms": ${JSON.stringify(rooms)}`)
  if (text === before || text.includes('165.232.82.11') || text.includes('mods-info/main/servers.json')) throw new Error('The shipped paths or rooms block changed; update shipped()')
  write(join(dir, 'launcher.json'), text)
  return join(dir, 'launcher.json')
}

const fastIds = fastGames.map((g) => g.id)
const scenarios = [
  { name: 'fallback', about: 'no servers.json: rooms.server; every answer the server gives, each game alone and all at once',
    prepare: (d) => config(d, { server: ROOMS }), commands: [...fastIds, 'nosuchgame', 'all'] },
  { name: 'servers-json', about: "servers.json's first online server (an offline one listed first)",
    prepare: (d) => config(d, { servers: `${WEB}/servers.json`, server: DOWN }), commands: ['g-busy', 'g-empty', 'g-unknown', 'all'] },
  { name: 'slow', about: 'a server that answers after the 4 s wait, and one that sends half and goes quiet',
    prepare: (d) => config(d, { server: ROOMS }, everyGame.filter((g) => ['g-slow', 'g-half', 'g-busy'].includes(g.id))), commands: ['g-slow', 'g-half', 'all'] },
  { name: 'older-server', about: 'a server that only speaks RELAY|1: asked again with LIST, and from then on (all)',
    prepare: (d) => config(d, { servers: `${WEB}/older.json` }, [game('o-busy', 'old-busy'), game('o-empty', 'old-empty'), game('o-edge', 'old-edge'), game('o-x', 'old-unknown')]),
    commands: ['o-busy', 'o-empty', 'all'] },
  { name: 'server-down', about: 'nothing listens at the server',
    prepare: (d) => config(d, { server: DOWN }, three), commands: ['g-busy', 'all'] },
  { name: 'no-port', about: 'rooms.server without a port',
    prepare: (d) => config(d, { server: '127.0.0.1' }, three), commands: ['g-busy', 'all'] },
  { name: 'bad-port', about: 'rooms.server with a port that is not a number',
    prepare: (d) => config(d, { server: '127.0.0.1:rooms' }, three), commands: ['g-busy', 'all'] },
  { name: 'service-port', about: 'rooms.server with a service name for its port',
    prepare: (d) => config(d, { server: '127.0.0.1:echo' }, three), commands: ['g-busy', 'all'] },
  { name: 'localhost', about: 'rooms.server by name',
    prepare: (d) => config(d, { server: `localhost:${roomsPort}` }, three), commands: ['g-busy', 'all'] },
  { name: 'no-server', about: 'neither servers.json nor rooms.server',
    prepare: (d) => config(d, {}, three), commands: ['g-busy', 'g-bare', 'all'] },
  ...[
    ['nostatus', 'addresses that are not text, a status that is not text'],
    ['none-online', 'no usable server, with a fallback'],
    ['no-list', '"servers" is not a list'],
    ['array', 'the file is a list'],
    ['bom', 'a byte-order mark'],
    ['comments', 'comments in the file'],
    ['invalid', 'not JSON'],
    ['empty', 'an empty file'],
    ['missing', 'HTTP 404'],
    ['redirect', 'a redirect to the real file'],
  ].map(([f, about]) => ({ name: `servers-${f}`, about: `servers.json: ${about} (fallback ${ROOMS})`,
    prepare: (d) => config(d, { servers: `${WEB}/${f}.json`, server: ROOMS }, three), commands: ['g-busy', 'g-bare', 'all'] })),
  ...[
    ['invalid', 'not JSON'],
    ['none-online', 'no usable server'],
    ['missing', 'HTTP 404'],
    ['forbidden', 'HTTP 403 without rate-limit headers'],
    ['ratelimit', 'HTTP 429 with Retry-After'],
    ['error', 'HTTP 500'],
    ['big', 'more than 4 MB'],
    ['empty-address', 'an online server whose address is empty'],
    ['no-port', 'an online server without a port'],
    ['down', 'an online server that is down'],
  ].map(([f, about]) => ({ name: `nofallback-${f}`, about: `servers.json: ${about}, no rooms.server`,
    prepare: (d) => config(d, { servers: `${WEB}/${f}.json` }, three),
    commands: ['g-busy', 'g-bare', 'g-unknown', 'all'] })),
  { name: 'servers-unreachable', about: 'the web server for servers.json is down, no rooms.server',
    prepare: (d) => config(d, { servers: `${DOWNWEB}/servers.json` }, three), commands: ['g-busy', 'g-bare', 'all'] },
  { name: 'servers-unreachable-fallback', about: 'the web server for servers.json is down, rooms.server set',
    prepare: (d) => config(d, { servers: `${DOWNWEB}/servers.json`, server: ROOMS }, three), commands: ['g-busy', 'g-bare', 'all'] },
  { name: 'bad-config', about: 'launcher.json with problems (servers not https)',
    prepare: (d) => config(d, { servers: 'http://example.org/servers.json', server: ROOMS }), commands: ['g-busy', 'all'] },
  { name: 'shipped', about: "the shipped launcher.json's games, its servers on the stand-ins",
    prepare: (d) => shipped(d, { servers: `${WEB}/none-online.json`, server: ROOMS }), commands: ['all'] },
]

// --- Running both sides --------------------------------------------------------------------------------------------

function run(exe, cfg, target, isCpp) {
  const env = { ...process.env, FL_NO_DISCORD: '1', FL_NO_SITE: '1', LOCALAPPDATA: join(work, 'localappdata'), FL_LAUNCHER_CONFIG: cfg }
  // The window passed FL_LAUNCHER_CONFIG to the C++ launcher as --config (src-tauri/src/engine.rs).
  const argv = isCpp ? ['--config', cfg, '--rooms', target, '--json'] : [target]
  asked = []
  return new Promise((res) => {
    const p = spawn(exe, argv, { env })
    let stdout = '', stderr = ''
    p.stdout.on('data', (d) => (stdout += d))
    p.stderr.on('data', (d) => (stderr += d))
    p.on('close', async (code) => {
      await wait(50) // a request still in flight is logged
      res({ code, stdout: stdout.trim(), stderr: stderr.replace(/\r\n/g, '\n').trim(), asked: [...asked] })
    })
  })
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

// What the answer says, in a few words, to show the case did what it is about.
function gist(j) {
  const one = (g) => (g.error ? `error "${g.error.slice(0, 70)}"` : `${g.rooms.length} rooms`) + (g.onServer ? '' : ', not on server')
  if (j.games) {
    const errs = [...new Set(Object.values(j.games).map((g) => g.error).filter(Boolean))]
    return `${Object.keys(j.games).length} games via "${j.server}"` + (errs.length ? `, errors: ${errs.map((e) => `"${e.slice(0, 50)}"`).join(', ')}` : '')
  }
  return `"${j.server}": ${one(j)}`
}

let failures = 0, cases = 0
for (const s of scenarios) {
  if (only && !only.includes(s.name)) continue
  const dir = join(work, s.name)
  const fresh = () => {
    rmSync(dir, { recursive: true, force: true })
    rmSync(join(work, 'localappdata'), { recursive: true, force: true })
    return s.prepare(dir)
  }
  for (const target of s.commands) {
    cases++
    const a = await run(cpp, fresh(), target, true)
    const r = await run(rust, fresh(), target, false)
    const out = []
    if (a.code !== r.code) out.push(`exit code: C++ ${a.code} / Rust ${r.code}`)
    if (a.stderr !== r.stderr) out.push(`error text:\n--- C++\n${a.stderr}\n--- Rust\n${r.stderr}`)
    if (JSON.stringify(a.asked) !== JSON.stringify(r.asked)) out.push(`requests:\n--- C++\n${a.asked.join('\n')}\n--- Rust\n${r.asked.join('\n')}`)
    let what = a.stderr ? 'refused: ' + a.stderr.split('\n')[0].slice(0, 80) : ''
    if (a.stdout || r.stdout) {
      try {
        const ja = JSON.parse(a.stdout), jr = JSON.parse(r.stdout)
        diff(ja, jr, '$', out)
        what = gist(ja)
      } catch (e) {
        out.push(`output isn't JSON: ${e.message}\nC++: ${a.stdout.slice(0, 300)}\nRust: ${r.stdout.slice(0, 300)}`)
      }
    }
    console.log(`${out.length ? 'FAIL' : 'ok  '} ${s.name} ${target}: ${out.length} differences, exit ${a.code}, ${a.asked.length} requests (${what}) - ${s.about}`)
    for (const line of out.slice(0, 100)) console.log('     ' + line)
    if (out.length) failures++
  }
}
rooms.srv.close(); older.srv.close(); web.close()
for (const s of [...rooms.sockets, ...older.sockets]) s.destroy()
if (!keep) rmSync(work, { recursive: true, force: true })
else console.log(`Kept ${work}`)
console.log(failures ? `${failures} of ${cases} case(s) differ` : `All ${cases} cases match`)
process.exit(failures ? 1 : 0)
