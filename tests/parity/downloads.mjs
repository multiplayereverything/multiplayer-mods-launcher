// Parity check for part 4 of PORTING.md: the app's Rust players and game files (src-tauri/src/launcher/install.rs,
// through the `downloads` example, which is the code path of the window's install_player, check_players,
// update_players and download_game commands) must do exactly what `MultiplayerEverythingLauncher.exe
// --install-player <id>`, `--update-players [--check] [--json]` and `--download-game <id>` do for the same
// launcher.json, settings and server answers: the exit code, stdout (progress lines included), the error message,
// the requests sent, and the players, games and data folders afterwards (every name, size and SHA-256; launcher.log
// line by line without its time stamps).
//
//   node tests/parity/downloads.mjs [--keep] [--only name,name]
//
// One loopback server stands in for GitHub's API (releases/latest with tag_name and assets whose digest is
// "sha256:..."), the download host and the game catalog. Zips are written here, byte by byte, so some can carry a path
// that climbs out of the folder or a link. Everything is in a temp folder; Discord is off (FL_NO_DISCORD).
import { spawn, spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { createServer } from 'node:http'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')
const args = process.argv.slice(2)
const keep = args.includes('--keep')
const only = args.includes('--only') ? args[args.indexOf('--only') + 1].split(',') : null

const win = process.platform === 'win32'
const exe = win ? '.exe' : ''
const cpp = process.env.FL_LAUNCHER_EXE ?? (win ? join(root, 'old', 'build', 'Release', 'MultiplayerEverythingLauncher.exe') : join(root, 'old', 'build-linux', 'MultiplayerEverythingLauncher'))
if (!existsSync(cpp)) throw new Error(`No C++ launcher at ${cpp}: run old\\build.ps1 or set FL_LAUNCHER_EXE`)

console.log('Building the Rust side (cargo build --example downloads)...')
const b = spawnSync('cargo', ['build', '--example', 'downloads', '--quiet'], { cwd: join(root, 'src-tauri'), stdio: 'inherit', shell: true })
if (b.status !== 0) process.exit(1)
const rust = join(root, 'src-tauri', 'target', 'debug', 'examples', `downloads${exe}`)

const work = join(tmpdir(), 'fl-parity-downloads')
rmSync(work, { recursive: true, force: true })
mkdirSync(work, { recursive: true })
const wait = (ms) => new Promise((r) => setTimeout(r, ms))
const sha = (buf) => createHash('sha256').update(buf).digest('hex')
function write(file, data) {
  mkdirSync(dirname(file), { recursive: true })
  writeFileSync(file, data)
}

// ---- Zips, stored (no compression) ---------------------------------------------------------------------------------

const crcTable = Array.from({ length: 256 }, (_, n) => {
  let c = n
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
  return c >>> 0
})
const crc32 = (buf) => {
  let c = 0xffffffff
  for (const x of buf) c = crcTable[(c ^ x) & 0xff] ^ (c >>> 8)
  return (c ^ 0xffffffff) >>> 0
}
// entries: { name, data } files, { name, link } symbolic links (Unix mode in the external attributes).
function zip(entries) {
  const locals = [], centrals = []
  let offset = 0
  for (const e of entries) {
    const name = Buffer.from(e.name, 'utf8')
    const data = Buffer.from(e.link ?? e.data ?? '')
    const crc = crc32(data)
    const lh = Buffer.alloc(30)
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(0x0800, 6); lh.writeUInt16LE(0, 8)
    lh.writeUInt32LE(0, 10); lh.writeUInt32LE(crc, 14); lh.writeUInt32LE(data.length, 18); lh.writeUInt32LE(data.length, 22)
    lh.writeUInt16LE(name.length, 26); lh.writeUInt16LE(0, 28)
    locals.push(lh, name, data)
    const ch = Buffer.alloc(46)
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(e.link ? 0x0314 : 0x0014, 4); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(0x0800, 8)
    ch.writeUInt16LE(0, 10); ch.writeUInt32LE(0, 12); ch.writeUInt32LE(crc, 16); ch.writeUInt32LE(data.length, 20); ch.writeUInt32LE(data.length, 24)
    ch.writeUInt16LE(name.length, 28); ch.writeUInt32LE(e.link ? (0o120777 << 16) >>> 0 : 0, 38); ch.writeUInt32LE(offset, 42)
    centrals.push(ch, name)
    offset += 30 + name.length + data.length
  }
  const cd = Buffer.concat(centrals)
  const end = Buffer.alloc(22)
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10)
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16)
  return Buffer.concat([...locals, cd, end])
}

// Fixed bytes so both sides download the same thing.
const bytes = (n, seed) => {
  const b = Buffer.alloc(n)
  let x = seed
  for (let i = 0; i < n; i++) b[i] = (x = (x * 1103515245 + 12345) >>> 0) >>> 24
  return b
}
const ZIPS = {
  fake: zip([{ name: 'fakeplay.exe', data: 'fake player v1' }, { name: 'readme.txt', data: 'hello' }]),
  rt101: zip([{ name: 'rt.exe', data: 'rt 1.0.1' }]),
  rt102: zip([{ name: 'rt.exe', data: bytes(600_000, 7) }, { name: 'lib\\data.bin', data: 'data' }]),
  wrapped: zip([{ name: 'ruffle-top/', data: '' }, { name: 'ruffle-top/fakeplay.exe', data: 'wrapped' }, { name: 'ruffle-top/x/y.txt', data: 'deep' }]),
  noexe: zip([{ name: 'other.exe', data: 'not it' }]),
  dotdot: zip([{ name: 'fakeplay.exe', data: 'escape test' }, { name: '../escaped.txt', data: 'outside' }, { name: 'sub/../../escaped2.txt', data: 'outside 2' }]),
  absolute: zip([{ name: 'fakeplay.exe', data: 'absolute test' }, { name: '/abs.txt', data: 'abs' }]),
  link: zip([{ name: 'fakeplay.exe', data: 'link test' }, { name: 'link-out', link: '../../outside.txt' }]),
  notzip: bytes(5000, 3),
}
const GAME = { a: Buffer.from('game file a v2'), b: Buffer.from('game file b'), old: Buffer.from('game file a v1') }

// ---- The stand-in server --------------------------------------------------------------------------------------------

const PORT = 48951
const base = `http://127.0.0.1:${PORT}`
let state = {}
let requests = []
const server = createServer((req, res) => {
  requests.push(req.url)
  const s = state
  const route = s.routes?.[req.url]
  if (!route) return res.writeHead(404).end()
  const now = Math.floor(Date.now() / 1000)
  if (route.status) return res.writeHead(route.status, typeof route.headers === 'function' ? route.headers(now) : route.headers ?? {}).end(route.body ?? '')
  const body = Buffer.from(typeof route.body === 'function' ? route.body() : route.body)
  if (route.drop) {
    res.writeHead(200, { 'Content-Length': body.length })
    res.write(body.subarray(0, body.length >> 1))
    return setTimeout(() => res.socket.destroy(), 100)
  }
  if (route.noLength) {
    // Written by hand: node would send it chunked.
    return req.socket.end(Buffer.concat([Buffer.from('HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n'), body.subarray(0, route.noLength)]))
  }
  if (route.slow) {
    res.writeHead(200, { 'Content-Length': body.length })
    let i = 0
    const step = Math.ceil(body.length / route.slow)
    const next = () => {
      if (i >= body.length) return res.end()
      res.write(body.subarray(i, i + step))
      i += step
      setTimeout(next, 60)
    }
    return next()
  }
  res.writeHead(200, { 'Content-Length': body.length }).end(body)
}).listen(PORT, '127.0.0.1')

const release = (tag, assets) => JSON.stringify({ tag_name: tag, draft: false, assets })
const asset = (name, zipBuf, { digest = true, size = true, url } = {}) => ({
  name, browser_download_url: url ?? `${base}/dl/${name}`, ...(size ? { size: zipBuf.length } : {}),
  ...(digest === true ? { digest: `sha256:${sha(zipBuf).toUpperCase()}` } : digest ? { digest } : {}),
})
const latest = '/repos/o/rt/releases/latest'
const ghRoutes = (tag, name, zipBuf, opts = {}, file = {}) => ({
  [latest]: { body: release(tag, [asset('notes.txt', Buffer.from('n'), { digest: false }), asset(name, zipBuf, opts)]) },
  [`/dl/${name}`]: { body: zipBuf, ...file },
})
const catalog = (files) => JSON.stringify({ schemaVersion: 1, files })
const centry = (game, name, buf, extra = {}) => ({ game, name, path: `games/${game}/${name}`, size: buf.length, sha256: sha(buf), description: '', ...extra })

// ---- The case folder ------------------------------------------------------------------------------------------------

function prepare(dir, c) {
  rmSync(dir, { recursive: true, force: true })
  const config = {
    paths: { games: '${LAUNCHER_DIR}\\games', players: '${LAUNCHER_DIR}\\players', data: '${LAUNCHER_DIR}\\data', flashpoint: [] },
    downloads: { catalog: c.noCatalog ? '' : `${base}/catalog.json`, site: '' },
    players: [
      { id: 'fake', name: 'Fake Player', paths: ['${PLAYERS_DIR}\\fake\\fakeplay.exe'],
        install: { url: `${base}/dl/fake.zip`, ...(c.pin === undefined ? { sha256: sha(ZIPS.fake) } : c.pin === null ? {} : { sha256: c.pin }), dir: '${PLAYERS_DIR}\\fake', exe: 'fakeplay.exe' } },
      { id: 'rt', name: 'Release Player', paths: ['${PLAYERS_DIR}\\rt\\rt.exe'],
        install: { github: 'o/rt', asset: c.pattern ?? 'RT-*.zip', api: base, dir: '${PLAYERS_DIR}\\rt', exe: 'rt.exe' } },
      { id: 'plain', name: 'Plain Player', paths: ['plain.exe'] },
      ...(c.players ?? []),
    ],
    games: [
      { id: 'ga', name: 'Game A', actions: [{ id: 'play', label: 'Play', swf: '${GAMES_DIR}\\a.swf' }] },
      { id: 'gb', name: 'Game B', actions: [{ id: 'play', label: 'Play', swf: '${GAMES_DIR}\\b.swf' }] },
    ],
  }
  write(join(dir, 'launcher.json'), JSON.stringify(config, null, 2))
  const settings = typeof c.settings === 'function' ? c.settings(dir) : c.settings
  write(join(dir, 'data', 'settings.json'), JSON.stringify(settings ?? { playerName: 'Tester', minimize: 0 }))
  for (const [rel, data] of Object.entries(c.files ?? {})) write(join(dir, rel), data)
  return join(dir, 'launcher.json')
}

const stamp = /^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} (DEBUG|INFO |WARN |ERROR) /
function tree(dir, at = '') {
  const out = []
  let names = []
  try { names = readdirSync(join(dir, at)).sort() } catch { return out }
  for (const n of names) {
    const rel = at ? `${at}\\${n}` : n
    const full = join(dir, rel)
    const st = statSync(full, { throwIfNoEntry: false })
    if (!st) { out.push(`${rel} (unreadable link)`); continue }
    if (st.isDirectory()) { out.push(`${rel}\\`); out.push(...tree(dir, rel)); continue }
    if (rel === 'data\\launcher.log') {
      out.push(...readFileSync(full, 'utf8').split(/\r?\n/).filter(Boolean).map((l) => 'log: ' + (stamp.test(l) ? l.replace(stamp, (m, lvl) => lvl.trim() + ' ') : l)))
      continue
    }
    if (rel === 'launcher.json') continue
    const data = readFileSync(full)
    out.push(`${rel} ${data.length} ${sha(data).slice(0, 16)}`)
  }
  return out
}

function run(isCpp, c, dir) {
  const cfg = prepare(dir, c)
  state = { routes: typeof c.routes === 'function' ? c.routes() : c.routes ?? {} }
  requests = []
  const env = { ...process.env, FL_NO_DISCORD: '1', FL_NO_SITE: '1', LOCALAPPDATA: join(work, 'localappdata'), FL_LAUNCHER_CONFIG: cfg }
  const [what, ...rest] = c.cmd
  const cppArgs = { 'install-player': ['--install-player', ...rest], 'update-players': ['--update-players', ...rest], 'download-game': ['--download-game', ...rest] }[what]
  const argv = isCpp ? ['--config', cfg, ...cppArgs] : c.cmd
  return new Promise((res) => {
    const p = spawn(isCpp ? cpp : rust, argv, { env, cwd: work })
    let stdout = '', stderr = ''
    p.stdout.on('data', (d) => (stdout += d))
    p.stderr.on('data', (d) => (stderr += d))
    p.on('close', async (code) => {
      await wait(150)
      res({
        code,
        stdout: stdout.replace(/\r\n/g, '\n').trim().split('\n').filter(Boolean),
        message: stderr.replace(/\r\n/g, '\n').trim(),
        requests: [...requests],
        tree: tree(dir),
        outside: ['escaped.txt', 'escaped2.txt', 'abs.txt', 'outside.txt'].filter((f) => existsSync(join(dir, f)) || existsSync(join(dir, 'players', f)) || existsSync(join(work, f))),
      })
    })
  })
}

// Progress lines: whole percents, rising, ending at 100. How many arrive depends on how WinHTTP splits the body, so
// a slow download is compared by its shape.
function progressShape(lines) {
  const pr = lines.map((l) => { try { return JSON.parse(l).progress } catch { return null } }).filter(Boolean)
  if (!pr.length) return 'none'
  const pct = pr.map((p) => Math.floor((p.done * 100) / p.total))
  const rising = pct.every((x, i) => i === 0 || x > pct[i - 1])
  return `${pr.length > 2 ? 'several' : pr.length} lines, ids ${[...new Set(pr.map((p) => p.id))]}, total ${pr[0].total}, first ${pct[0]}%, last ${pct.at(-1)}%, ${rising ? 'rising' : 'NOT rising'}`
}

const rtOld = { 'players\\rt\\rt.exe': 'rt 1.0.0 old', 'players\\rt\\.version': 'v1.0.1', 'players\\rt\\keep.cfg': 'old settings' }
const fakeOld = { 'players\\fake\\fakeplay.exe': 'old fake', 'players\\fake\\old-only.txt': 'from before' }
const rl403 = { status: 403, headers: (now) => ({ 'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': String(now + 1500) }), body: '{"message":"API rate limit exceeded"}' }
const fakeRoute = (extra = {}) => ({ '/dl/fake.zip': { body: ZIPS.fake, ...extra } })
const fakeWith = (zipBuf) => ({ '/dl/fake.zip': { body: zipBuf } })
const catRoutes = (files, extra = {}) => ({
  '/catalog.json': { body: catalog(files) },
  '/games/ga/a.swf': { body: GAME.a }, '/games/gb/b.swf': { body: GAME.b }, ...extra,
})

const cases = [
  // --install-player
  { name: 'install-url', about: 'first install from a URL with a pinned sha256', cmd: ['install-player', 'fake'], routes: fakeRoute() },
  { name: 'install-url-over', about: 'reinstall over a copy: the folder is replaced as a whole', cmd: ['install-player', 'fake'], routes: fakeRoute(), files: fakeOld },
  { name: 'install-url-wrong-sha', about: 'the pinned sha256 does not match: refused, the copy intact', cmd: ['install-player', 'fake'], routes: fakeRoute(), files: fakeOld, pin: '0'.repeat(64) },
  { name: 'install-url-upper-sha', about: 'a pinned sha256 in capitals', cmd: ['install-player', 'fake'], routes: fakeRoute(), pin: sha(ZIPS.fake).toUpperCase() },
  { name: 'install-url-no-sha', about: 'no pinned hash (the C++ installs it unchecked)', cmd: ['install-player', 'fake'], routes: fakeRoute(), pin: null },
  { name: 'install-github', about: 'install from a GitHub release: digest checked, .version written', cmd: ['install-player', 'rt'], routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102) },
  { name: 'install-github-no-digest', about: 'a release asset without a digest (the C++ installs it unchecked)', cmd: ['install-player', 'rt'], routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, { digest: false }) },
  { name: 'install-github-wrong-digest', about: 'a digest that does not match: refused, the copy intact', cmd: ['install-player', 'rt'], files: rtOld,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, { digest: 'sha256:' + 'ab'.repeat(32) }) },
  { name: 'install-github-other-digest', about: 'a digest that is not sha256: unchecked', cmd: ['install-player', 'rt'], routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, { digest: 'md5:00' }) },
  { name: 'install-github-no-match', about: 'no asset matches the pattern', cmd: ['install-player', 'rt'], routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102), pattern: '*.7z' },
  { name: 'install-github-404', about: 'releases/latest answers 404', cmd: ['install-player', 'rt'], routes: {} },
  { name: 'install-github-403-limit', about: 'rate limit used up (403, X-RateLimit-Reset)', cmd: ['install-player', 'rt'], routes: { [latest]: rl403 } },
  { name: 'install-github-429-retry', about: 'too many requests (429, Retry-After)', cmd: ['install-player', 'rt'], routes: { [latest]: { status: 429, headers: { 'Retry-After': '120' } } } },
  { name: 'install-github-403-plain', about: 'a 403 that is not the rate limit', cmd: ['install-player', 'rt'], routes: { [latest]: { status: 403, headers: { 'X-RateLimit-Remaining': '12' } } } },
  { name: 'install-github-bad-json', about: 'releases/latest is not JSON', cmd: ['install-player', 'rt'], routes: { [latest]: { body: '<html>nope' } } },
  { name: 'install-github-redirect', about: 'the download redirects (as GitHub sends it to its CDN)', cmd: ['install-player', 'rt'],
    routes: { ...ghRoutes('v2', 'rt-2.zip', ZIPS.rt101), '/dl/rt-2.zip': { status: 302, headers: { Location: `${base}/cdn/rt-2.zip` } }, '/cdn/rt-2.zip': { body: ZIPS.rt101 } } },
  { name: 'install-wrapped', about: 'an archive with a top folder: the player is found one folder down', cmd: ['install-player', 'fake'], routes: fakeWith(ZIPS.wrapped), pin: sha(ZIPS.wrapped) },
  { name: 'install-no-exe', about: 'the archive lacks the player', cmd: ['install-player', 'fake'], routes: fakeWith(ZIPS.noexe), pin: sha(ZIPS.noexe), files: fakeOld },
  { name: 'install-not-zip', about: 'the download is not an archive', cmd: ['install-player', 'fake'], routes: fakeWith(ZIPS.notzip), pin: sha(ZIPS.notzip), files: fakeOld },
  { name: 'install-zip-dotdot', about: 'entries that climb out with ..', cmd: ['install-player', 'fake'], routes: fakeWith(ZIPS.dotdot), pin: sha(ZIPS.dotdot) },
  { name: 'install-zip-absolute', about: 'an entry with an absolute path', cmd: ['install-player', 'fake'], routes: fakeWith(ZIPS.absolute), pin: sha(ZIPS.absolute) },
  { name: 'install-zip-link', about: 'a symbolic link pointing outside', cmd: ['install-player', 'fake'], routes: fakeWith(ZIPS.link), pin: sha(ZIPS.link) },
  { name: 'install-drop', about: 'the server drops the connection halfway: the copy intact, no temp files', cmd: ['install-player', 'fake'], routes: fakeRoute({ drop: true }), files: fakeOld },
  { name: 'install-short', about: 'a body shorter than the release says, without Content-Length', cmd: ['install-player', 'rt'], files: rtOld,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, {}, { noLength: 1000 }) },
  { name: 'install-file-404', about: 'the download itself is 404', cmd: ['install-player', 'fake'], routes: {}, files: fakeOld },
  { name: 'install-unknown', about: 'an unknown player id', cmd: ['install-player', 'nosuch'], routes: {} },
  { name: 'install-no-entry', about: 'a player without an install entry', cmd: ['install-player', 'plain'], routes: {} },
  { name: 'install-unsafe-dir', about: 'an install folder that is a drive root', cmd: ['install-player', 'root'], routes: {},
    players: [{ id: 'root', name: 'Root Player', paths: ['x.exe'], install: { url: `${base}/dl/fake.zip`, dir: 'C:\\', exe: 'x.exe' } }] },
  { name: 'install-default-exe', about: 'no exe named: ruffle.exe is looked for', cmd: ['install-player', 'dflt'], routes: fakeRoute(),
    players: [{ id: 'dflt', name: 'Default Exe', paths: ['x.exe'], install: { url: `${base}/dl/fake.zip`, dir: '${PLAYERS_DIR}\\dflt' } }] },
  { name: 'install-games-dir', about: 'a games folder chosen in Settings (the player lookups are logged twice)', cmd: ['install-player', 'fake'], routes: fakeRoute(),
    settings: (dir) => ({ playerName: 'Tester', gamesDir: join(dir, 'mygames') }) },
  // --update-players
  { name: 'check-newer', about: '--check --json: a newer release is reported, nothing written', cmd: ['update-players', '--check', '--json'], files: rtOld,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102) },
  { name: 'update-newer', about: '--json: the newer release installed, progress lines, .version', cmd: ['update-players', '--json'], files: rtOld,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102) },
  { name: 'update-newer-no-size', about: 'the asset has no size: progress from Content-Length', cmd: ['update-players', '--json'], files: rtOld,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, { size: false }) },
  { name: 'update-slow', about: 'a slow download: progress lines rising to 100%', cmd: ['update-players', '--json'], files: rtOld, shape: true,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, {}, { slow: 25 }) },
  { name: 'update-up-to-date', about: 'the installed release is the newest', cmd: ['update-players', '--json'], files: rtOld, routes: ghRoutes('v1.0.1', 'rt-1.0.1.zip', ZIPS.rt101) },
  { name: 'update-older', about: 'the newest release is older than the installed one', cmd: ['update-players', '--json'], files: rtOld, routes: ghRoutes('1.0.0', 'rt-1.0.0.zip', ZIPS.rt101) },
  { name: 'update-unrecorded', about: 'no .version: counted as older, even the same release installs', cmd: ['update-players', '--json'],
    files: { 'players\\rt\\rt.exe': 'rt unknown' }, routes: ghRoutes('v1.0.1', 'rt-1.0.1.zip', ZIPS.rt101) },
  { name: 'update-version-spaces', about: '.version with a line break and spaces', cmd: ['update-players', '--check', '--json'],
    files: { 'players\\rt\\rt.exe': 'x', 'players\\rt\\.version': '  V1.0.2\r\n' }, routes: ghRoutes('v1.0.10', 'rt-1.0.10.zip', ZIPS.rt101) },
  { name: 'update-not-installed', about: 'the release player is not installed: not even asked', cmd: ['update-players', '--json'], routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102) },
  { name: 'update-wrong-digest', about: 'the newer release fails its checksum: exit 1, the copy intact', cmd: ['update-players', '--json'], files: rtOld,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, { digest: 'sha256:' + '0'.repeat(64) }) },
  { name: 'update-rate-limit', about: 'the lookup is rate limited', cmd: ['update-players', '--check', '--json'], files: rtOld, routes: { [latest]: rl403 } },
  { name: 'update-drop', about: 'the update download drops halfway', cmd: ['update-players', '--json'], files: rtOld,
    routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102, {}, { drop: true }) },
  { name: 'update-text-newer', about: 'without --json: the human lines (updated)', cmd: ['update-players'], files: rtOld, routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102) },
  { name: 'update-text-check', about: 'without --json, --check (available)', cmd: ['update-players', '--check'], files: { 'players\\rt\\rt.exe': 'x' }, routes: ghRoutes('v1.0.2', 'rt-1.0.2.zip', ZIPS.rt102) },
  { name: 'update-text-same', about: 'without --json (up to date)', cmd: ['update-players'], files: rtOld, routes: ghRoutes('v1.0.1', 'rt-1.0.1.zip', ZIPS.rt101) },
  { name: 'update-text-error', about: 'without --json (an error)', cmd: ['update-players'], files: rtOld, routes: {} },
  // --download-game
  { name: 'download-new', about: "a game's file from the catalog", cmd: ['download-game', 'ga'], routes: catRoutes([centry('ga', 'a.swf', GAME.a), centry('gb', 'b.swf', GAME.b)]) },
  { name: 'download-present', about: 'already there with the right hash: not downloaded again', cmd: ['download-game', 'ga'],
    files: { 'games\\a.swf': GAME.a }, routes: catRoutes([centry('ga', 'a.swf', GAME.a)]) },
  { name: 'download-outdated', about: 'there with another hash: replaced', cmd: ['download-game', 'ga'], files: { 'games\\a.swf': GAME.old }, routes: catRoutes([centry('ga', 'a.swf', GAME.a)]) },
  { name: 'download-wrong-sha', about: 'the file fails its checksum: the old copy intact, no .part', cmd: ['download-game', 'ga'], files: { 'games\\a.swf': GAME.old },
    routes: catRoutes([centry('ga', 'a.swf', GAME.a, { sha256: 'F'.repeat(64) })]) },
  { name: 'download-no-sha', about: 'a catalog entry without sha256 (downloaded unchecked, every time)', cmd: ['download-game', 'ga'], files: { 'games\\a.swf': GAME.a },
    routes: catRoutes([centry('ga', 'a.swf', GAME.a, { sha256: undefined })]) },
  { name: 'download-wrong-size', about: 'the catalog size is wrong', cmd: ['download-game', 'ga'], routes: catRoutes([centry('ga', 'a.swf', GAME.a, { size: 3 })]) },
  { name: 'download-all', about: 'download all: every game, one already there', cmd: ['download-game', 'all'], files: { 'games\\b.swf': GAME.b },
    routes: catRoutes([centry('ga', 'a.swf', GAME.a), centry('gb', 'b.swf', GAME.b), centry('gc', 'c.swf', GAME.b, { path: `${base}/mirror/c.swf` })], { '/mirror/c.swf': { body: GAME.b } }) },
  { name: 'download-all-one-fails', about: 'download all: the second file fails, the first stays', cmd: ['download-game', 'all'],
    routes: catRoutes([centry('ga', 'a.swf', GAME.a), centry('gc', 'c.swf', GAME.b, { path: 'http://127.0.0.1:1/c.swf' }), centry('gb', 'b.swf', GAME.b)]) },
  { name: 'download-absolute-url', about: 'a catalog path that is a full address', cmd: ['download-game', 'ga'],
    routes: catRoutes([centry('ga', 'a.swf', GAME.a, { path: `${base}/mirror/a.swf` })], { '/mirror/a.swf': { body: GAME.a } }) },
  { name: 'download-drop', about: 'the file drops halfway', cmd: ['download-game', 'ga'], files: { 'games\\a.swf': GAME.old },
    routes: catRoutes([centry('ga', 'a.swf', GAME.a)], { '/games/ga/a.swf': { body: GAME.a, drop: true } }) },
  { name: 'download-404', about: 'the file is 404', cmd: ['download-game', 'ga'], routes: catRoutes([centry('ga', 'a.swf', GAME.a, { path: 'games/ga/gone.swf' })]) },
  { name: 'download-unsafe-names', about: 'catalog names that would leave the games folder are dropped', cmd: ['download-game', 'gx'],
    routes: catRoutes([centry('gx', '..\\evil.swf', GAME.a), centry('gx', 'sub/evil.swf', GAME.a), centry('gx', 'C:evil.swf', GAME.a)]) },
  { name: 'download-unknown', about: 'an unknown game id', cmd: ['download-game', 'nosuch'], routes: catRoutes([centry('ga', 'a.swf', GAME.a)]) },
  { name: 'download-no-catalog', about: 'launcher.json has no catalog', cmd: ['download-game', 'ga'], routes: {}, noCatalog: true },
  { name: 'download-catalog-404', about: 'the catalog is 404', cmd: ['download-game', 'ga'], routes: {} },
  { name: 'download-catalog-bad', about: 'the catalog is not valid JSON', cmd: ['download-game', 'ga'], routes: { '/catalog.json': { body: '{ "files": [ }' } } },
  { name: 'download-games-dir', about: 'into the games folder chosen in Settings', cmd: ['download-game', 'ga'], routes: catRoutes([centry('ga', 'a.swf', GAME.a)]),
    settings: (dir) => ({ playerName: 'Tester', gamesDir: join(dir, 'mygames') }) },
]

let failures = 0, ran = 0
for (const c of cases) {
  if (only && !only.includes(c.name)) continue
  ran++
  const dir = join(work, 'case')
  const a = await run(true, c, dir)
  const r = await run(false, c, dir)
  const diffs = []
  const cmp = (what, x, y) => {
    const sx = JSON.stringify(x, null, 1), sy = JSON.stringify(y, null, 1)
    if (sx !== sy) diffs.push(`${what}:\n--- C++\n${sx}\n--- Rust\n${sy}`)
  }
  cmp('exit code', a.code, r.code)
  cmp('message', a.message, r.message)
  if (c.shape) {
    const rest = (l) => l.filter((x) => !x.startsWith('{"progress"'))
    cmp('stdout (without progress)', rest(a.stdout), rest(r.stdout))
    cmp('progress', progressShape(a.stdout), progressShape(r.stdout))
    cmp('files', a.tree, r.tree)
  } else {
    // Linux: the engine reads through curl and the app through its own client, so the percents between start and end differ.
    const steps = (l) => (win ? l : l.filter((x) => !/^[0-9]+%$/.test(x) || x === '100%'))
    cmp('stdout', steps(a.stdout), steps(r.stdout))
    cmp('files', a.tree, r.tree)
  }
  cmp('requests', a.requests, r.requests)
  cmp('written outside', a.outside, r.outside)
  const stray = a.tree.filter((l) => !l.startsWith('log: ') && /\.(installing|old|part)\b/.test(l))
  const gist = `exit ${a.code}${a.message ? ': ' + a.message.split('\n')[0].slice(0, 100) : ''}${stray.length ? ', LEFT ' + stray.join(' ') : ''}${a.outside.length ? ', OUTSIDE ' + a.outside : ''}`
  console.log(`${diffs.length ? 'FAIL' : 'ok  '} ${c.name}: ${diffs.length} differences (${gist}) - ${c.about}`)
  for (const d of diffs) console.log('     ' + d.replace(/\n/g, '\n     '))
  if (process.env.PARITY_SHOW) console.log(JSON.stringify(a, null, 1))
  if (diffs.length) failures++
}
server.close()
if (!keep) rmSync(work, { recursive: true, force: true })
else console.log(`Kept ${work}`)
console.log(failures ? `${failures} of ${ran} case(s) differ` : `All ${ran} cases match`)
process.exit(failures ? 1 : 0)
