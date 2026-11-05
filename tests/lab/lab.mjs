// The test lab: builds the window, starts it through tauri-driver against a sandbox (sandbox.mjs) with local servers,
// and clicks through every feature the way a player would, checking what reached the fake player, the sandbox's
// settings.json and the fake game folder.
//
//   node tests/lab/lab.mjs [--no-build] [--keep] [--only name,name]
//
// Needs: tauri-driver (cargo install tauri-driver --locked), Microsoft Edge WebDriver matching WebView2 (MSEDGEDRIVER,
// or %LOCALAPPDATA%\fl-lab\edgedriver-<version>\msedgedriver.exe), and the C++ launcher's Debug build (FL_LAUNCHER_EXE,
// default old\build\Debug), whose launcher_tests.exe is the fake player.
import { execFileSync, spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { PORTS, makeSandbox, snapshot } from './sandbox.mjs'
import { relayClient, startServers } from './servers.mjs'
import { Driver, button, q, waitFor } from './webdriver.mjs'

const here = dirname(fileURLToPath(import.meta.url))
const tauriDir = resolve(here, '..', '..')
const argv = process.argv.slice(2)
const only = argv.includes('--only') ? argv[argv.indexOf('--only') + 1].split(',') : null
const FAKE_MS = 10000  // how long the fake game stays open: long enough to host, short enough to come back

function need(path, what) {
  if (!path || !existsSync(path)) {
    console.error(`The lab needs ${what}${path ? ` (not found: ${path})` : ''}.`)
    process.exit(2)
  }
  return path
}

const launcher = need(process.env.FL_LAUNCHER_EXE ?? resolve(tauriDir, 'old', 'build', 'Debug', 'MultiplayerEverythingLauncher.exe'),
  'the C++ launcher (set FL_LAUNCHER_EXE)')
const fakePlayer = need(join(dirname(launcher), 'launcher_tests.exe'), 'launcher_tests.exe next to the launcher')
const edgeDir = join(process.env.LOCALAPPDATA ?? '', 'fl-lab')
const edgeDriver = need(process.env.MSEDGEDRIVER ??
  (existsSync(edgeDir) ? readdirSync(edgeDir).filter((d) => d.startsWith('edgedriver-')).map((d) => join(edgeDir, d, 'msedgedriver.exe')).find(existsSync) : undefined),
  'msedgedriver.exe (set MSEDGEDRIVER)')
const targetDir = process.env.CARGO_TARGET_DIR ?? join(tauriDir, 'src-tauri', 'target')

if (!argv.includes('--no-build')) {
  console.log('Building the window (tauri build --debug --no-bundle)...')
  const b = spawnSync('npx tauri build --debug --no-bundle', { cwd: tauriDir, shell: true, stdio: 'inherit' })
  if (b.status !== 0) process.exit(b.status ?? 1)
}
const app = need(join(targetDir, 'debug', 'app.exe'), 'the built window')

const work = join(tmpdir(), `fl-tauri-lab-${process.pid}`)
const box = makeSandbox(work, fakePlayer)
const gameBefore = snapshot(box.gameDir)
const stopServers = startServers(work, PORTS)
const fails = join(work, 'fails')

// The app's own updater (src-tauri/src/app_update.rs) reads a local feed signed with a throwaway key, made here; a
// development build is the only kind that takes FL_UPDATE_ENDPOINT and FL_UPDATE_PUBKEY. The feed is written only by
// the last scenario, so the other scenarios see no update.
const updDir = join(work, 'www', 'upd')
mkdirSync(updDir, { recursive: true })
const updKey = join(work, 'upd.key')
spawnSync(`npx tauri signer generate --ci -w "${updKey}"`, { cwd: tauriDir, shell: true, stdio: 'ignore' })
const env = {
  ...process.env,
  FL_UPDATE_ENDPOINT: `http://127.0.0.1:${PORTS.http}/upd/latest.json`, FL_UPDATE_PUBKEY: readFileSync(`${updKey}.pub`, 'utf8').trim(),
  FL_LAUNCHER_EXE: launcher, FL_LAUNCHER_CONFIG: box.config, FL_FAKE_OUT: box.args, FL_FAKE_MS: String(FAKE_MS),
  FL_NO_NAME_PROMPT: '1', FL_NO_DISCORD: '1', FL_FAKE_ENV: 'FAKE_TV',
}
const driverProc = spawn('tauri-driver', ['--port', '4444', '--native-driver', edgeDriver], { env, stdio: 'ignore' })
const wd = new Driver('http://127.0.0.1:4444')

let passed = 0
let failed = 0
async function check(name, ok, detail = '') {
  if (ok) {
    passed++
    console.log(`  PASS  ${name}`)
    return
  }
  failed++
  console.log(`  FAIL  ${name}${detail ? `: ${detail}` : ''}`)
  const errors = await wd.exec('return window.__labErrors ?? []').catch(() => [])
  if (errors.length) console.log(`        page errors: ${errors.join(' | ')}`)
  const tvLog = await wd.exec("return JSON.parse(localStorage.getItem('fl.tvLog') ?? '[]')").catch(() => [])
  if (tvLog.length) console.log(`        TV mode: ${tvLog.slice(-3).join(' | ')}`)
  try {
    mkdirSync(fails, { recursive: true })
    writeFileSync(join(fails, `${failed}-${name.replace(/[^a-z0-9]+/gi, '-').slice(0, 60)}.png`), Buffer.from(await wd.screenshot(), 'base64'))
  } catch {
    // A screenshot is a help, not a requirement.
  }
}

// ---- reading the sandbox ---------------------------------------------------------------------------------------
const settings = () => JSON.parse(readFileSync(box.settings, 'utf8'))
const clearArgs = () => rmSync(box.args, { force: true })
// What the fake player was started with, once it has been.
const launchedWith = async () => (await waitFor(() => existsSync(box.args) && readFileSync(box.args, 'utf8').trim(), 10000) || '').split(/\r?\n/)
const clipboard = () => execFileSync('powershell', ['-NoProfile', '-Command', 'Get-Clipboard'], { encoding: 'utf8' }).trim()

// ---- moving around the window ------------------------------------------------------------------------------------
const nav = (label) => `//nav[contains(@class,'navs')]//button[.//span[contains(@class,'lbl') and normalize-space(.)=${q(label)}]]`
const card = (name) => `//button[contains(@class,'card-c')][.//span[contains(@class,'name') and normalize-space(.)=${q(name)}]]`
// A panel button by its label (its first line), whatever glyph comes before it.
const panel = (label) => `//aside[contains(@class,'dpanel')]//button[.//span[normalize-space(.)=${q(label)}]]`
const inDialog = (label) => `//div[@role='dialog']${button(label)}`
const pill = () => wd.exec("return document.querySelector('.spill')?.textContent ?? ''")
const toast = () => wd.exec("return [...document.querySelectorAll('.isl.note')].map(e => e.textContent).join(' ')")
const key = (k) => wd.exec(`window.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)} }))`)
const labPick = (path) => wd.exec('window.__flLabPick = arguments[0]', path)

async function openGame(name) {
  await wd.click(nav('All games'))
  await wd.click(card(name))
  await wd.find(`//h1[@aria-label=${q(name)}]`)
}
async function launch(label) {
  clearArgs()
  await wd.click(panel(label))
  return launchedWith()
}
const isMinimized = () =>
  wd.execAsync("const done = arguments[arguments.length - 1]; window.__TAURI_INTERNALS__.invoke('plugin:window|is_minimized', { label: 'main' }).then(done, () => done(null))")

// ---- the scenarios ---------------------------------------------------------------------------------------------
const scenarios = {
  async update() {
    // The sandbox's Updater Test player is one release behind: the window asks at start, then shows the download.
    const card = () => wd.exec("return document.querySelector('.isl.upd')?.textContent ?? ''")
    const asked = await waitFor(async () => { const t = await card(); return t.includes('Update now?') && t }, 15000)
    await check('a newer player release asks to update now', !!asked && asked.includes('Updater Test v1.0.1') && asked.includes('v1.0.0'), String(asked))
    const rtDir = join(box.config, '..', 'players', 'rt')
    await check('asking installs nothing yet', readFileSync(join(rtDir, '.version'), 'utf8').trim() === 'v1.0.0')
    // Every view of the card while it downloads, read from the page itself so none is missed.
    await wd.exec(`window.__labSeen = []
      window.__labWatch = setInterval(() => window.__labSeen.push(document.querySelector('.isl.upd')?.textContent ?? ''), 15)`)
    await wd.click(`//div[contains(@class,'upd')]${button('Update now')}`)
    const installed = await waitFor(() => readFileSync(join(rtDir, '.version'), 'utf8').trim() === 'v1.0.1', 20000)
    await check('Update now installs the new release', !!installed)
    await waitFor(async () => !(await card()), 5000)
    const seen = await wd.exec('clearInterval(window.__labWatch); return window.__labSeen')
    await check('the download shows its progress in percent', seen.some((t) => /Updating Updater Test/.test(t) && /\d+%/.test(t)), [...new Set(seen)].slice(0, 4).join(' | '))
    await check('the update says when it is done', (await waitFor(async () => (await toast()).includes('Updater Test v1.0.1'), 5000)) === true, await toast())
  },

  async library() {
    const counts = await waitFor(async () => {
      const c = await wd.exec("return Object.fromEntries([...document.querySelectorAll('.navs .nav')].map(b => [b.querySelector('.lbl')?.textContent, b.querySelector('.n')?.textContent ?? '']))")
      return c['All games'] === '16' && c
    })
    await check('the sidebar counts every game', !!counts, JSON.stringify(counts))
    await check('the sidebar counts each category', counts && counts['Flash mods'] === '8' && counts['PC mods'] === '5' && counts['Online multiplayer'] === '3' && !('Community mods' in counts) && counts['Coming soon'] === '2', JSON.stringify(counts))
    await wd.click(nav('Flash mods'))
    await check('a category shows its games', (await waitFor(async () => (await wd.count("//button[contains(@class,'card-c')]")) === 8)) === true)
    await wd.click(nav('All games'))
    await wd.type("//input[@placeholder='Search games and mods']", 'relay')
    const found = await waitFor(async () => (await wd.exec("return [...document.querySelectorAll('.card-c .name')].map(e => e.textContent)")).join('|') === 'Relay Test')
    await check('search finds a game by name', !!found)
    await wd.click("//button[@aria-label='Clear the search']")
    await wd.click(button('A to Z'))
    const names = await waitFor(async () => {
      const n = await wd.exec("return [...document.querySelectorAll('.card-c .name')].map(e => e.textContent)")
      return n.length === 16 && n
    })
    await check('A to Z sorts by name', !!names && names.join('|') === [...names].sort((a, b) => a.localeCompare(b)).join('|'), names?.join(', '))
    await wd.click(button('Compact'))
    await check('Compact makes the cards smaller', !!(await waitFor(() => wd.exec("return document.querySelector('.grid')?.classList.contains('compact')"))))
    await check('the card size is saved for both launchers', (await waitFor(() => settings().compactCards === true)) === true)
    await wd.click(button('Large'))
    await waitFor(() => settings().compactCards === false)
    await wd.exec('location.reload()')
    await wd.find("//nav[contains(@class,'navs')]", 15000)
    await wd.click(nav('All games'))
    const kept = await waitFor(() => wd.exec("return document.querySelector('[role=radiogroup] [aria-checked=true]')?.textContent"), 10000)
    await check('the sort order is kept after a restart', kept === 'A to Z', String(kept))
  },

  async rooms() {
    await wd.click(nav('All games'))
    const counted = await waitFor(async () => (await wd.exec("return [...document.querySelectorAll('.card-c')].find(c => c.querySelector('.name')?.textContent === 'Co-op Test')?.querySelector('.st')?.textContent ?? ''")).includes('2 rooms open'), 15000)
    await check('a card shows how many rooms are open', !!counted)
    await wd.click(nav('Home'))
    await check('Home lists the games open right now', !!(await waitFor(async () => (await wd.exec("return [...document.querySelectorAll('.shelf-h h2')].map(h => h.textContent).join('|')")).includes('Open right now'), 8000)))
    await wd.click(nav('Rooms'))
    await check('Open rooms lists the server\'s rooms', (await waitFor(async () => (await wd.count("//div[contains(@class,'row-card')]")) >= 2, 10000)) === true)
    await check('a full room offers no Join', (await wd.count(`//div[contains(@class,'row-card')]${button('Full')}`)) >= 1)
    const full = await wd.exec("return [...document.querySelectorAll('.row-card')].find(r => r.textContent.includes('Full room'))?.textContent ?? ''")
    await check('a room says when its game has started and has a password', full.includes('playing') && full.includes('password'), full)
    clearArgs()
    await wd.click(`//div[contains(@class,'row-card')][.//small[starts-with(normalize-space(.),'Co-op Test')]]${button('Join')}`)
    const a = await launchedWith()
    await check('Join on a room joins it with its code', a.includes('--coop-join-relay') && a.includes('ABCDE') && a.includes(`127.0.0.1:${PORTS.rooms}`), a.join(' '))
  },

  async coop() {
    await openGame('Co-op Test')
    await wd.click(panel('Host a game'))
    clearArgs()
    await wd.click(inDialog('Host'))
    let a = await launchedWith()
    await check('Host a game hosts a room for 2', a.includes('--coop-host-relay') && a.join(' ').includes('--coop-players 2'), a.join(' '))
    await check('starting shows the hint of the action', !!(await waitFor(async () => (await toast()).includes('Send the room code'), 4000)))
    await wd.click(panel('Host a game'))
    await wd.click(inDialog('3 players'))
    clearArgs()
    await wd.click(inDialog('Host'))
    a = await launchedWith()
    await check('Host for 3 players picks the 3-player room', a.join(' ').includes('--coop-players 3'), a.join(' '))
    await wd.click(panel('Join a game'))
    clearArgs()
    await wd.type("//input[@id='join-code']", 'qw3rt')
    a = await launchedWith()
    await check('Join with a code sends it in capitals', a.includes('--coop-join-relay') && a.includes('QW3RT'), a.join(' '))
    await wd.click("//button[contains(normalize-space(.),'Other ways to connect')]")
    a = await launch('Host with your IP (2 players)')
    await check('Host with your IP hosts directly', a.join(' ').includes('--coop-host 7790'), a.join(' '))
    await wd.click(panel('Join by IP'))
    clearArgs()
    await wd.type("//input[@id='join-code']", '192.168.1.20:7790')
    a = await launchedWith()
    await check('Join by IP connects to the address', a.join(' ').includes('--coop-join 192.168.1.20:7790'), a.join(' '))
    await wd.click(panel('Join by IP'))
    clearArgs()
    await wd.type("//input[@id='join-code']", '192.168.1.21')
    a = await launchedWith()
    await check('Join by IP adds the usual port to a bare IP', a.join(' ').includes('--coop-join 192.168.1.21:7790'), a.join(' '))
    await check('Play solo is hidden while offline play is off', (await wd.count(panel('Play solo'))) === 0)
    await wd.click(panel('Favorite'))
    await check('Favorite is saved', (await waitFor(() => settings().games?.coop?.favorite === true)) === true)
    await wd.click(panel('Guide'))
    await check('Guide opens the How to play tab', (await wd.exec("return document.querySelector('.ptabs [aria-selected=true]')?.textContent")) === 'How to play')
    await key('Escape')
    await check('Esc goes back to the list', !!(await waitFor(async () => (await wd.count("//button[contains(@class,'card-c')]")) > 0)))
  },

  async favorites() {
    await wd.click(nav('Favorites'))
    const h = await waitFor(() => wd.exec("const c = document.querySelector('.card-c'); return c && c.getBoundingClientRect().height"))
    await check('the Favorites list shows the favorite', (await wd.count(card('Co-op Test'))) === 1)
    await check('a selected card keeps its full size', h > 150, `height ${h}`)
  },

  async settings() {
    await wd.click("//button[contains(@class,'setbtn')]")
    await wd.click("//button[@role='switch' and contains(@aria-label,'offline')]")
    await check('Show offline play is saved', (await waitFor(() => settings().showOffline === true)) === true)
    await wd.type("//div[contains(@class,'set-row')][.//b[normalize-space(.)='Your name']]//input", 'Tester2')
    await check('Your name is saved', (await waitFor(() => settings().playerName === 'Tester2')) === true)
    await openGame('Co-op Test')
    const a = await launch('Play solo')
    await check('Play solo starts the game on its own', a.length === 1 && a[0].endsWith('coop.swf'), a.join(' '))
    await wd.click(panel('Host a game'))
    clearArgs()
    await wd.click(inDialog('Host'))
    const h = await launchedWith()
    await check('the new name reaches the game', h.join(' ').includes('--coop-name Tester2'), h.join(' '))
  },

  async minimize() {
    // The window comes back once every game it started has closed, so the games of earlier scenarios go first.
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    await wd.click("//button[contains(@class,'setbtn')]")
    await wd.click("//button[@role='switch' and contains(@aria-label,'Minimize')]")
    await check('Minimize while a game runs is saved', (await waitFor(() => settings().minimize === 1 || settings().minimize === true)) === true)
    await openGame('Relay Test')
    await launch('Play')
    await check('the window minimizes while the game runs', (await waitFor(async () => (await isMinimized()) === true, 5000)) === true)
    await check('the window comes back when the game closes', (await waitFor(async () => (await isMinimized()) === false, FAKE_MS + 8000)) === true)
    await wd.click("//button[contains(@class,'setbtn')]")
    await wd.click("//button[@role='switch' and contains(@aria-label,'Minimize')]")
    await waitFor(() => !settings().minimize)
  },

  async hosting() {
    await openGame('Relay Test')
    await launch('Host with the relay')
    const box1 = await waitFor(() => wd.exec("return document.querySelector('.card.pinned')?.textContent"), 10000)
    await check('hosting shows the hosting box with its port', !!box1 && box1.includes(String(PORTS.relay)), box1)
    const gameSide = await relayClient(PORTS.relay, 'H', 'Tester2')
    const friend = await relayClient(PORTS.relay, 'J', 'Friend One')
    await check('a friend who joins is listed', !!(await waitFor(async () => (await wd.exec("return document.querySelector('.card.pinned')?.textContent ?? ''")).includes('Friend One'), 6000)))
    await check('the friend fills the P2 slot', !!(await waitFor(async () => (await wd.exec("return [...document.querySelectorAll('.pslot')].map(e => e.textContent).join('|')")).includes('Friend One'))))
    const addr = await wd.exec("return document.querySelector('.card.pinned .linkbtn')?.textContent")
    await wd.click("//div[contains(@class,'pinned')]//button[contains(@class,'linkbtn')]")
    await check('Copy puts the address on the clipboard', (await waitFor(() => clipboard() === addr, 4000)) === true, `${clipboard()} vs ${addr}`)
    friend.close()
    gameSide.close()
    await wd.click(button('Stop hosting'))
    await check('Stop hosting closes the box', (await waitFor(async () => !(await wd.exec("return !!document.querySelector('.card.pinned')")), 6000)) === true)
    await check('Stop hosting says so', (await waitFor(async () => (await toast()).includes('Hosting stopped'))) === true)
  },

  async download() {
    await openGame('Download Test')
    await check('a missing file says so', (await waitFor(async () => (await pill()).includes('missing'))) === true, await pill())
    // Play on a game whose file is missing offers the download first.
    await wd.click(panel('Play'))
    await check('Play on a missing game offers the download', !!(await wd.find("//div[@role='dialog']//h2[starts-with(normalize-space(.),'Download')]", 4000).catch(() => null)))
    await wd.click(inDialog('Download'))
    await check('Download fetches the file from the catalog', (await waitFor(async () => (await pill()).includes('Ready'), 20000)) === true, await pill())
    const file = join(work, 'games', 'dl.swf')
    await check('the downloaded file is in the games folder', existsSync(file) && readFileSync(file, 'utf8') === 'pretend game file')
    // The cached catalog now names a newer build of that file: the game shows an update, as the C++ card does.
    const cat = JSON.parse(readFileSync(join(work, 'www', 'catalog.json'), 'utf8'))
    cat.files[0].sha256 = '1'.repeat(64)
    writeFileSync(join(work, 'data', 'catalog.json'), JSON.stringify(cat))
    await wd.exec("window.dispatchEvent(new KeyboardEvent('keydown', { key: 'F5' }))")
    await check('a newer build in the catalog shows as an update', (await waitFor(async () => (await pill()).includes('Update available'), 10000)) === true, await pill())
    await check('the update can be downloaded', (await wd.count(panel('Download the update'))) === 1)
  },

  async locate() {
    await openGame('Locate Test')
    const copy = join(work, 'elsewhere', 'my copy.swf')
    await labPick(copy)
    await wd.click(panel('Locate the game file'))
    await check('Locate makes the game ready', (await waitFor(async () => (await pill()).includes('Ready'), 10000)) === true, await pill())
    await labPick(undefined)
    const s = JSON.stringify(settings().games?.locgame ?? {})
    await check('the located file is saved for the game', s.includes('my copy.swf'), s)
    const a = await launch('Play')
    await check('Play uses the located file', a.some((x) => x.endsWith('my copy.swf')), a.join(' '))
  },

  async mods() {
    await openGame('Fake Game')
    await check('a missing PC game says so', (await waitFor(async () => (await pill()).includes('not found'), 15000)) === true, await pill())
    await labPick(box.gameDir)
    await wd.click(panel('Choose the game folder'))
    await labPick(undefined)
    const ready = await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install') && !b.disabled)"), 15000)
    await check('Choose the game folder finds the game', ready === true)
    await wd.click(panel('Install Fake Game'))
    await check('Install finishes', !!(await waitFor(async () => (await wd.count(panel('Repair'))) === 1, 30000)))
    const dat = join(box.gameDir, 'Data', 'original.dat')
    await check('Install puts the mod\'s files in the game folder', readFileSync(dat, 'utf8') === 'MODDED 1.0.0' && existsSync(join(box.gameDir, 'Mods', 'mod-test', 'Mod.dll')))
    await check('the mod shows as installed', (await waitFor(async () => (await pill()).includes('Installed'), 10000)) === true, await pill())
    rmSync(join(box.gameDir, 'Mods', 'mod-test', 'Mod.dll'))
    await wd.click(panel('Repair'))
    await check('Repair puts back a deleted file', (await waitFor(() => existsSync(join(box.gameDir, 'Mods', 'mod-test', 'Mod.dll')), 30000)) === true)
    await waitFor(async () => (await wd.count(panel('Uninstall'))) === 1, 30000)
    await wd.click(panel('Uninstall'))
    await wd.click(inDialog('Uninstall'))
    await check('Uninstall finishes', !!(await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install'))"), 30000)))
    await check('Uninstall leaves the game folder exactly as it was', snapshot(box.gameDir) === gameBefore)
  },

  async lobby() {
    await openGame('Lobby Test')
    await wd.click(panel('Play online'))
    await wd.find("//h1[starts-with(normalize-space(.),'Play')]")
    await wd.click("//button[@role='switch' and @aria-label='Direct IP']")
    await wd.type("//input[@type='number']", String(PORTS.lobby))
    await wd.click(button('Host room'))
    const me = await waitFor(() => wd.exec("return [...document.querySelectorAll('.slot-name')].map(e => e.textContent).join('|')"), 15000)
    await check('hosting a lobby room lists you in it', !!me && me.includes('(you)'), me)
    await wd.type("//input[@placeholder='Say something']", 'hello lab')
    await check('chat reaches the room', !!(await waitFor(async () => (await wd.exec("return document.querySelector('.chat-lines')?.textContent ?? ''")).includes('hello lab'), 8000)))
    await wd.click(button('Leave room'))
    await check('Leave room goes back to the game', !!(await waitFor(() => wd.exec("return document.querySelector('h1')?.getAttribute('aria-label')"))))
  },

  async offer() {
    await openGame('Ruffle Test')
    await wd.click(panel('Play'))
    await check('Play without its player offers to install it', !!(await wd.find("//div[@role='dialog']//h2[normalize-space(.)='Install Ruffle Together?']", 4000).catch(() => null)))
    await wd.click(inDialog('Not now'))
    await check('Not now leaves it for later', (await wd.count("//div[@role='dialog']")) === 0)
  },

  async firstrun() {
    // A new player: no name in settings.json, then the launcher starts.
    const s = settings()
    delete s.playerName
    writeFileSync(box.settings, JSON.stringify(s))
    await wd.exec('location.reload()')
    await check('the setup opens by itself on a first run', !!(await wd.find("//h1[starts-with(normalize-space(.),'Welcome to')]", 15000).catch(() => null)))
    await wd.click(button('Continue'))
    await check('Continue needs a name', (await wd.exec("return [...document.querySelectorAll('.setup-foot button')].find(b => b.textContent === 'Continue')?.disabled")) === true)
    await wd.type("//input[@aria-label='Your name']", 'First Player')
    await wd.click(button('Continue'))
    await check('saving the name keeps the setup open', !!(await wd.find("//h1[normalize-space(.)='The Flash player']", 4000).catch(() => null)))
    for (let i = 0; i < 6 && (await wd.count(button('Start playing'))) === 0; i++) await wd.click(`//div[contains(@class,'slot-r')]//button`)
    await wd.click(button('Start playing'))
    await check('a finished setup opens the launcher', !!(await wd.find("//nav[contains(@class,'navs')]").catch(() => null)))
    await check('the first-run name is saved', settings().playerName === 'First Player')
  },

  async setup() {
    await wd.click("//button[contains(@class,'setbtn')]")
    await wd.click(button('Run setup again'))
    await check('Run setup again opens the setup', !!(await wd.find("//h1[starts-with(normalize-space(.),'Welcome to')]").catch(() => null)))
    await wd.click(button('Continue'))
    await wd.type("//input[@aria-label='Your name']", 'Lab Player')
    await check('the name shows in the P1 slot as it is typed', (await wd.exec("return document.querySelector('.namebox .pslot')?.textContent")) === 'Lab Player')
    await wd.click(button('Continue'))
    await check('the name is saved', (await waitFor(() => settings().playerName === 'Lab Player')) === true)
    await check('the Flash player step offers the install', (await wd.count(button('Install'))) === 1)
    await wd.click(button('Install'))
    await check('Install downloads the Flash player', !!(await waitFor(async () => (await wd.count("//span[@aria-label='Installed']")) === 1, 20000)))
    await check('the player is in the players folder', existsSync(join(work, 'players', 'ruffle', 'ruffle.exe')))
    await wd.click(button('Continue'))
    await check('the PC games step lists the PC game', !!(await waitFor(async () => (await wd.exec("return document.querySelector('.setup-card')?.textContent ?? ''")).includes('Fake Game'), 10000)))
    await wd.click(button('Continue'))
    await check('the folder step shows the games folder', (await wd.exec("return document.querySelector('.setup-card .path')?.textContent ?? ''")).includes('games'))
    await wd.click(button('Continue'))
    const before = settings().showOffline === true
    await wd.click("//button[@role='switch' and contains(@aria-label,'offline')]")
    await check('a choice is saved as it is switched', (await waitFor(() => (settings().showOffline === true) !== before)) === true)
    await wd.click(button('Continue'))
    await check('the last step greets you by name', (await wd.exec("return document.querySelector('.setup-slots')?.textContent ?? ''")).includes('Lab Player'))
    await wd.click(button('Back'))
    await check('Back goes to the previous step', !!(await wd.find("//h1[normalize-space(.)='A few choices']", 4000).catch(() => null)))
    await wd.click(button('Continue'))
    await wd.click(button('Start playing'))
    await check('Start playing opens the launcher', !!(await wd.find("//nav[contains(@class,'navs')]").catch(() => null)))
  },

  async options() {
    await openGame('Co-op Test')
    await check('a co-op game offers its co-op save', (await wd.count("//select[@id='coop-save']")) === 1)
    await wd.exec("const s = document.getElementById('coop-save'); s.value = '2'; s.dispatchEvent(new Event('change', { bubbles: true }))")
    await check('a new co-op game warns that it replaces the progress', (await wd.exec("return document.querySelector('.psel small')?.textContent ?? ''")).includes('replaces'))
    await wd.click("//div[contains(@class,'pcheck')][.//span[normalize-space(.)='Friendly fire']]//button[@role='switch']")
    await check('a game setting is saved', (await waitFor(() => settings().games?.coop?.settings?.ff === '1')) === true)
    await wd.exec("const s = document.getElementById('set-speed'); s.value = 'fast'; s.dispatchEvent(new Event('change', { bubbles: true }))")
    await check('a game choice is saved', (await waitFor(() => settings().games?.coop?.settings?.speed === 'fast')) === true)
    await wd.click(panel('Host a game'))
    clearArgs()
    await wd.click(inDialog('Host'))
    let a = (await launchedWith()).join(' ')
    await check('hosting passes the co-op save choice', a.includes('--coop-save new'), a)
    await check('hosting passes the host settings', a.includes('--friendly-fire') && a.includes('--fast'), a)
    await wd.click(panel('Join a game'))
    clearArgs()
    await wd.type("//input[@id='join-code']", 'zzzzz\uE007')
    a = (await launchedWith()).join(' ')
    await check('joining passes only the settings for everyone', a.includes('--fast') && !a.includes('--friendly-fire') && !a.includes('--coop-save'), a)
    await check('Quick play finds the busiest open room', !!(await waitFor(async () => (await wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].map(b => b.textContent).join('|')")).includes('Join the busiest open room'), 8000)))
    a = (await launch('Quick play')).join(' ')
    await check('Quick play joins that room', a.includes('--coop-join-relay') && a.includes('ABCDE'), a)
  },

  async players() {
    await openGame('Relay Test')
    await check('the player choice shows when two players are found', (await wd.count("//select[@id='game-player']")) === 1)
    await wd.exec("const s = document.getElementById('game-player'); s.value = 'second'; s.dispatchEvent(new Event('change', { bubbles: true }))")
    await check('the chosen player is saved', (await waitFor(() => settings().games?.relaygame?.player === 'second')) === true)
    const a = (await launch('Play')).join(' ')
    await check('the game starts in the chosen player', a.includes('--second-player'), a)
    await check('a running game shows as playing until it closes', !!(await waitFor(async () => (await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing Relay Test'), 4000)))
    await check('its play time is counted when it closes', (await waitFor(() => (settings().games?.relaygame?.seconds ?? 0) >= 5, FAKE_MS + 8000)) === true)
    await wd.exec("const s = document.getElementById('game-player'); s.value = ''; s.dispatchEvent(new Event('change', { bubbles: true }))")
    await waitFor(() => !settings().games?.relaygame?.player)
  },

  async community() {
    await openGame('Relay Test')
    const credit = await waitFor(() => wd.exec("return document.querySelector('.credit')?.textContent ?? ''"))
    await check('a community mod credits its author', !!credit && credit.includes('Some Modder') && credit.includes('We read its code'), credit)
    await check('its grade shows in the panel', (await wd.exec("return document.querySelector('.dpanel .grade')?.textContent")) === 'B')
    await check('the status bar measures the server', /Server online \(\d+ ms\)/.test(await waitFor(() => wd.exec("return document.querySelector('.srv')?.textContent ?? ''").then((x) => /ms/.test(x) && x), 15000) || ''))
  },

  // A community mod approved into mods-info (records/lab-coop.json): it arrives without a launcher update, installs
  // its pinned release and uninstalls cleanly. The website's second project, with no record, is listed but can't install.
  async communitymods() {
    // The players scenario's game may still be open: its card over the grid would take the clicks below.
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    const labBefore = snapshot(box.labGame)
    await wd.click(nav('All games'))
    const arrived = await waitFor(async () => (await wd.count(card('Lab Game'))) === 1 && (await wd.count(card('Listed Only'))) === 1, 20000)
    await check('approved community mods and listed projects come from mods-info and the website', arrived === true)
    if (!arrived) return
    const saved = existsSync(join(work, 'data', 'records.json')) ? readFileSync(join(work, 'data', 'records.json'), 'utf8') : ''
    await check('the records are kept in the data folder for the mod commands', saved.includes('lab-coop'))
    await openGame('Lab Game')
    const page = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('the card has the website\'s description', page.includes('Four players in one lab.'), page.slice(0, 200))
    await check('the card says whose mod it is', page.includes('Made by\nsomeone'), page.slice(0, 300))
    await wd.click("//div[@role='tablist']//button[normalize-space(.)='How to play']")
    const guide = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('its guide credits the author and links the repository', guide.includes('Made by someone, not by Multiplayer Everything') && guide.includes('lab-coop by someone on GitHub'), guide.slice(0, 300))
    await wd.click("//div[@role='tablist']//button[normalize-space(.)='Overview']")
    const ready = await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install') && !b.disabled)"), 20000)
    const modsNow = ready === true ? '' : await wd.exec("return window.__TAURI_INTERNALS__.invoke('mods').then(m => JSON.stringify(m.map(x => [x.id, x.error])))").catch((e) => String(e))
    await check('the community mod finds its game and offers Install', ready === true, `${await pill()} ${modsNow}`)
    if (!ready) return
    await wd.click(panel('Install Lab Game'))
    const dll = join(box.labGame, 'BepInEx', 'plugins', 'LabCoop', 'LabCoop.dll')
    await check('Install puts the pinned release in the game folder', (await waitFor(() => existsSync(dll), 30000)) === true)
    await check('a folder inside the zip is copied with its own subfolders', (await waitFor(() => existsSync(join(box.labGame, 'BepInEx', 'plugins', 'LabCoop', 'lang', 'en.txt')), 15000)) === true && !existsSync(join(box.labGame, 'BepInEx', 'plugins', 'LabCoop', 'LabCoop')))
    await check('the community mod shows as installed', (await waitFor(async () => (await pill()).includes('Installed'), 15000)) === true, await pill())
    await waitFor(async () => (await wd.count(panel('Uninstall'))) === 1, 30000)
    await wd.click(panel('Uninstall'))
    await wd.click(inDialog('Uninstall'))
    await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install'))"), 30000)
    await check('Uninstall leaves the community game folder exactly as it was', snapshot(box.labGame) === labBefore)
    await key('Escape')

    // records/lab-solo.json: one DLL released on its own, the record the website's form writes when there's no manifest.
    const soloBefore = snapshot(box.labSolo)
    await openGame('Lab Solo')
    const soloReady = await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install') && !b.disabled)"), 20000)
    await check('a single-file community mod offers Install', soloReady === true, await pill())
    if (soloReady === true) {
      await wd.click(panel('Install Lab Solo'))
      const soloDll = join(box.labSolo, 'Mods', 'LabSolo.dll')
      await check('a single-file mod is copied as it was released', (await waitFor(() => existsSync(soloDll) && readFileSync(soloDll, 'utf8') === 'lab solo 0.3.0', 30000)) === true)
      await waitFor(async () => (await wd.count(panel('Uninstall'))) === 1, 30000)
      await wd.click(panel('Uninstall'))
      await wd.click(inDialog('Uninstall'))
      await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install'))"), 30000)
      await check('Uninstall leaves the single-file mod\'s game folder exactly as it was', snapshot(box.labSolo) === soloBefore)
    }
    await key('Escape')
    await openGame('Listed Only')
    const listed = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('a listed project says it can\'t be installed yet', listed.includes("can't be installed from the launcher yet") || listed.includes('On the website, not in mods-info.'), listed.slice(0, 200))
    await check('a listed project has no Install button', (await wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].filter(b => /Install/.test(b.textContent)).length")) === 0)
    await key('Escape')

    // records/lab-link.json: a card the team wrote by hand, with its own text and one button to the mod's page.
    await openGame('Lab Link')
    const linkPage = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('a link card shows its own description and players', linkPage.includes('A mod with an installer of its own.') && linkPage.includes('2-16 players'), linkPage.slice(0, 300))
    await check('a link card has the mod\'s page and no Install', (await wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].map(b => b.textContent).join('|')")).includes("Open the mod's page") &&
      (await wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].filter(b => /Install/.test(b.textContent)).length")) === 0,
      await wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].map(b => b.textContent).join('|')"))
    await wd.click("//div[@role='tablist']//button[normalize-space(.)='How to play']")
    const linkGuide = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('a link card\'s guide has its steps, its tips and the credit', linkGuide.includes("Download the zip from the mod's page.") && linkGuide.includes('It is an alpha.') &&
      linkGuide.includes("The launcher doesn't install, update or support it"), linkGuide.slice(0, 400))
    await key('Escape')

    // Ctrl+F5: a mod approved a minute ago comes now, not when the saved copy's hour is up; one taken down goes again.
    const www = join(work, 'www', 'records')
    const indexBefore = readFileSync(join(www, 'index.json'), 'utf8')
    const late = JSON.parse(readFileSync(join(www, 'lab-solo.json'), 'utf8'))
    late.id = late.mod.id = 'lab-late'
    late.mod.game.name = 'Lab Late'
    writeFileSync(join(www, 'lab-late.json'), JSON.stringify(late))
    writeFileSync(join(www, 'index.json'), JSON.stringify({ mods: [...JSON.parse(indexBefore).mods, 'lab-late'] }))
    box.zipModsInfo()
    await wd.click(nav('All games'))
    await key('F5')
    await new Promise((r) => setTimeout(r, 1500))
    await check('F5 alone keeps the saved copy for its hour', (await wd.count(card('Lab Late'))) === 0)
    const ctrlF5 = () => wd.exec("window.dispatchEvent(new KeyboardEvent('keydown', { key: 'F5', ctrlKey: true }))")
    await ctrlF5()
    await check('Ctrl+F5 brings a mod approved since the start', (await waitFor(async () => (await wd.count(card('Lab Late'))) === 1, 20000)) === true)
    await check('Ctrl+F5 says what it found', (await waitFor(async () => (await toast()).includes('New games or changes came'), 10000)) === true, await toast())
    writeFileSync(join(www, 'index.json'), indexBefore)
    rmSync(join(www, 'lab-late.json'))
    box.zipModsInfo()
    await ctrlF5()
    await check('Ctrl+F5 drops a mod taken down from mods-info', (await waitFor(async () => (await wd.count(card('Lab Late'))) === 0, 20000)) === true)
  },

  // records/lab-team.json: one of our own mods from mods-info, as launcher.json would hold it. It follows its newest
  // release, brings BepInEx from the repository's loader release, and takes it away again on Uninstall.
  async teammods() {
    const before = snapshot(box.labTeam)
    await wd.click(nav('PC mods'))
    const arrived = await waitFor(async () => (await wd.count(card('Lab Team'))) === 1, 20000)
    await check('a team record from mods-info is one of the PC mods', arrived === true)
    if (!arrived) return
    await check('next to a community PC mod in the same list', (await wd.count(card('Lab Game'))) === 1)
    const saved = existsSync(join(work, 'data', 'records.json')) ? readFileSync(join(work, 'data', 'records.json'), 'utf8') : ''
    await check('the team records are kept in the data folder for the mod commands', saved.includes('lab-team'))
    await openGame('Lab Team')
    const page = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('its card is the record\'s own text, with no "made by" credit', page.includes('One of our own mods, from mods-info.') && page.includes('2-4 players') && !page.includes('not by Multiplayer Everything'), page.slice(0, 300))
    const ready = await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install') && !b.disabled)"), 30000)
    const modsNow = ready === true ? '' : await wd.exec("return window.__TAURI_INTERNALS__.invoke('mods').then(m => JSON.stringify(m.filter(x => x.id === 'lab-team')))").catch((e) => String(e))
    await check('it finds its game and its newest release, and offers Install', ready === true, `${await pill()} ${modsNow}`)
    if (!ready) return
    const sources = () => readFileSync(join(work, 'data', 'mods', 'sources.json'), 'utf8')
    await check('its newest release comes from mods-info\'s zip, with no question to GitHub', existsSync(join(work, 'data', 'mods', 'releases.json')) && /"from":\s*"file"/.test(sources()), sources().slice(0, 300))
    await wd.click(panel('Install Lab Team'))
    const dll = join(box.labTeam, 'BepInEx', 'plugins', 'LabTeam', 'LabTeam.dll')
    await check('Install puts the newest release in the game folder', (await waitFor(() => existsSync(dll) && readFileSync(dll, 'utf8') === 'lab team 2.0.0', 30000)) === true)
    await check('and BepInEx from the loader release, which the game didn\'t have', existsSync(join(box.labTeam, 'BepInEx', 'core', 'BepInEx.dll')) && existsSync(join(box.labTeam, 'winhttp.dll')))
    await check('the team mod shows as installed at its newest version', (await waitFor(async () => (await pill()).includes('Installed'), 15000)) === true, await pill())
    await wd.click(panel('Launch game'))
    // The engine's own log: the record's program, its arguments and the game folder it runs in.
    const logged = () => readFileSync(join(work, 'data', 'launcher.log'), 'utf8')
    const started = await waitFor(() => /Starting: "[^"]*\\LabTeam\\LabTeam\.exe" --from-team-record {2}\(in [^)]*\\LabTeam\)/.test(logged()), 15000)
    await check('its Launch starts what the record says', started === true, logged().split(/\r?\n/).slice(-4).join(' // '))
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    await waitFor(async () => (await wd.count(panel('Uninstall'))) === 1, 30000)
    await wd.click(panel('Uninstall'))
    await wd.click(inDialog('Uninstall'))
    await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install'))"), 30000)
    await check('Uninstall takes the loader away with the mod: the folder is as it was', snapshot(box.labTeam) === before)
    await key('Escape')

    // records/lab-flash.json: one of our Flash games from mods-info; its Play starts the player with the record's file.
    await openGame('Flash Team')
    const flashPage = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('a Flash team record has its own card', flashPage.includes('One of our Flash games, from mods-info.'), flashPage.slice(0, 200))
    const played = await launch('Play')
    await check('its Play starts the player with the record\'s game file and arguments', played.includes('--from-record') && played.some((x) => x.endsWith('flashteam.swf')), played.join(' '))
    // Let the fake game close, so its "Playing" bubble is gone before the next scenario clicks around.
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    await key('Escape')
    await check('a Flash record whose button runs a program of its own is left out', (await wd.count(card('Bad Flash'))) === 0)

    // records/lab-next.json needs a launcher that doesn't exist yet.
    await openGame('Lab Next')
    const next = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('a record for a newer launcher asks for an update', next.includes('Needs Multiplayer Everything 999.0 or newer'), next.slice(0, 300))
    await check('and has nothing to install', (await wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].filter(b => /Install|Launch/.test(b.textContent)).length")) === 0)
    await key('Escape')
  },

  // records/lab-devflash.json: a developer's Flash game. The launcher makes its buttons, downloads its pinned files into
  // their own folder, and starts the player with the release's key map.
  async devflash() {
    await wd.click(nav('Flash mods'))
    const arrived = await waitFor(async () => (await wd.count(card('Lab Dev Flash'))) === 1, 20000)
    await check('a developer\'s Flash game from mods-info is one of the Flash mods', arrived === true)
    if (!arrived) return
    await check('next to our own', (await wd.count(card('Flash Team'))) === 1)
    await openGame('Lab Dev Flash')
    await wd.click("//div[@role='tablist']//button[normalize-space(.)='How to play']")
    const guide = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('its guide credits the developer and links the repository', guide.includes('Made by someone, not by Multiplayer Everything') && guide.includes('lab-devflash by someone on GitHub'), guide.slice(0, 400))
    await wd.click("//div[@role='tablist']//button[normalize-space(.)='Overview']")
    await check('a missing file says so', (await waitFor(async () => (await pill()).includes('missing'))) === true, await pill())
    await wd.click(panel('Download game files'))
    await check('Download fetches the pinned files', (await waitFor(async () => (await pill()).includes('Ready'), 20000)) === true, await pill())
    const dir = join(work, 'games', 'community', 'lab-devflash')
    await check("into the game's own folder in the games folder", existsSync(join(dir, 'LabDevFlash.swf')) && readFileSync(join(dir, 'LabDevFlash.swf'), 'utf8') === 'dev flash game 1.0' &&
      existsSync(join(dir, 'lab-devflash.keys')))
    await wd.click("//button[contains(normalize-space(.),'Other ways to connect')]")
    const played = await launch('Host with your IP (2 players)')
    const k = played.indexOf('--coop-keys')
    await check("the launcher's own Host with your IP starts the pinned file with its key map", played.some((x) => x.endsWith('LabDevFlash.swf')) && k >= 0 &&
      played[k + 1] === join(dir, 'lab-devflash.keys') && played.join(' ').includes('--coop-host 7790'), played.join(' '))
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    await key('Escape')

    // records/lab-newflash.json needs Ruffle Together 99.0: with an older release picked, Play offers the update.
    await wd.exec("return window.__TAURI_INTERNALS__.invoke('set_game_player', { game: 'lab-newflash', player: 'rt' })")
    await openGame('Lab New Flash')
    await wd.click(panel('Download game files'))
    const needs = await waitFor(async () => (await pill()).includes('Needs Updater Test 99.0 or newer'), 20000)
    await check('a game for a newer player says which release it needs', needs === true, await pill())
    await wd.click("//button[contains(normalize-space(.),'Other ways to connect')]")
    await wd.click(panel('Host with your IP (2 players)'))
    const dialog = await wd.find("//div[@role='dialog']//h2[normalize-space(.)='Update Updater Test?']", 4000).catch(() => null)
    await check('and Play offers the update instead of starting', !!dialog)
    if (dialog) await wd.click(inDialog('Not now'))
    await key('Escape')
  },

  async keys() {
    await wd.click(nav('All games'))
    await wd.find("//button[contains(@class,'card-c')]")
    await wd.exec("document.querySelector('.card-c').focus()")
    const first = await wd.exec("return document.activeElement?.textContent")
    await wd.exec("document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true }))")
    const next = await wd.exec("return document.activeElement?.classList.contains('card-c') ? document.activeElement.textContent : ''")
    await check('arrow keys move between cards', !!next && next !== first, `${first} -> ${next}`)
    await wd.exec("window.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', ctrlKey: true }))")
    await wd.type("//input[@aria-label='Go to a game or page']", 'relay')
    await check('Ctrl+K finds a game by name', (await wd.exec("return document.querySelector('.sw-list [aria-selected=true] b')?.textContent")) === 'Relay Test')
    await wd.type("//input[@aria-label='Go to a game or page']", '', false)
    await check('Enter in Ctrl+K opens the game', !!(await wd.find("//h1[@aria-label='Relay Test']", 4000).catch(() => null)))
    await wd.exec("window.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', ctrlKey: true }))")
    await wd.type("//input[@aria-label='Go to a game or page']", 'settings')
    await check('Ctrl+K opens a page', !!(await wd.find("//h1[normalize-space(.)='Settings']", 4000).catch(() => null)))
    await wd.exec("document.documentElement.dataset.theme = 'light'")
    await check('the light theme switches the palette', (await wd.exec("return getComputedStyle(document.body).backgroundColor")) === 'rgb(255, 255, 255)')
    await check('the selected sidebar row stays dark in the light theme', (await wd.exec("return getComputedStyle(document.querySelector('.nav.on, .setbtn.on')).backgroundColor")) === 'rgb(43, 50, 72)')
    await check('status text in the light theme is dark enough to read', (await wd.exec("const e = document.createElement('span'); e.className = 'tone-ok'; document.body.append(e); const c = getComputedStyle(e).color; e.remove(); return c")) === 'rgb(22, 122, 67)')
    await wd.exec("document.documentElement.dataset.theme = 'dark'")
    await openGame('Co-op Test')
    await wd.click(panel('Host a game'))
    // As a real key press: from the focused element up to the window.
    await wd.exec("document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))")
    // Escape closes the dialog only: the page underneath stays (it doesn't also go Back).
    await check('closing a dialog puts the focus back on its button', !!(await waitFor(async () => (await wd.count("//div[@role='dialog']")) === 0 &&
      (await wd.exec("return document.activeElement?.textContent ?? ''")).includes('Host a game'), 3000)))
  },

  async pad() {
    // A controller, as the Gamepad API reports one: the D-pad moves the focus, A presses, Y searches.
    await wd.click(nav('All games'))
    await wd.find("//button[contains(@class,'card-c')]")
    await wd.exec(`window.__pad = { buttons: Array.from({ length: 17 }, () => ({ pressed: false })), axes: [0, 0, 0, 0] }
      navigator.getGamepads = () => [window.__pad]
      window.dispatchEvent(new Event('gamepadconnected'))
      document.querySelector('.card-c').focus()`)
    await check('a controller shows its own hints', !!(await waitFor(async () => (await wd.exec("return document.querySelector('.statusbar .hints')?.textContent ?? ''")).includes('Select'), 4000)))
    const press = async (i) => {
      await wd.exec('window.__pad.buttons[arguments[0]].pressed = true', i)
      await new Promise((r) => setTimeout(r, 120))
      await wd.exec('window.__pad.buttons[arguments[0]].pressed = false', i)
      await new Promise((r) => setTimeout(r, 120))
    }
    const first = await wd.exec('return document.activeElement?.textContent')
    await press(15)
    await check('the D-pad moves between cards', (await wd.exec('return document.activeElement?.textContent')) !== first)
    await press(0)
    await check('A opens the card in focus', !!(await wd.find("//h1[@aria-label]", 4000).catch(() => null)))
    await press(1)
    await press(3)
    await check('Y goes to the search', (await wd.exec("return document.activeElement?.getAttribute('placeholder')")) === 'Search games and mods')
    await wd.exec("navigator.getGamepads = () => []; window.dispatchEvent(new Event('gamepaddisconnected'))")
  },

  async paddialog() {
    // The Host dialog with only a controller: you can see where you are, the D-pad gets everywhere and stays there,
    // A presses, B closes the dialog and not the page behind it.
    await openGame('Co-op Test')
    // The pad is ignored while another window has the focus; this scenario shouldn't depend on what else is on the PC.
    await wd.exec(`document.hasFocus = () => true
      window.__pad = { buttons: Array.from({ length: 17 }, () => ({ pressed: false })), axes: [0, 0, 0, 0] }
      navigator.getGamepads = () => [window.__pad]
      window.dispatchEvent(new Event('gamepadconnected'))`)
    const press = async (i) => {
      await wd.exec('window.__pad.buttons[arguments[0]].pressed = true', i)
      await new Promise((r) => setTimeout(r, 120))
      await wd.exec('window.__pad.buttons[arguments[0]].pressed = false', i)
      await new Promise((r) => setTimeout(r, 120))
    }
    const focused = () => wd.exec('return document.activeElement?.textContent ?? ""')
    const inDlg = () => wd.exec("return !!document.activeElement?.closest('[role=dialog]')")
    await wd.exec("[...document.querySelectorAll('.dpanel button')].find(b => b.textContent.includes('Host a game')).focus()")
    await press(0)
    await check('A opens the Host dialog', !!(await wd.find("//div[@role='dialog']", 4000).catch(() => null)))
    await check('the dialog starts on Host', (await focused()) === 'Host', await focused())
    await press(12)
    const onIp = await focused()
    await check('the D-pad moves up to Your IP address', onIp.includes('Your IP address'), onIp)
    await check('you can see which control has the focus', await wd.exec("const s = getComputedStyle(document.activeElement); return s.outlineStyle !== 'none' && parseFloat(s.outlineWidth) > 0"))
    await press(0)
    await check('A picks Your IP address', await wd.exec("return document.activeElement?.getAttribute('aria-pressed') === 'true'"))
    // The page behind the dialog redraws on its own; that must not throw the focus back to Host.
    await new Promise((r) => setTimeout(r, 2500))
    await check('the focus stays where you put it', (await focused()).includes('Your IP address'), await focused())
    await press(12)
    await press(12)
    await check('the D-pad reaches the player counts', (await focused()).includes('players'), await focused())
    await press(14)
    await press(14)
    await check('left and right move along the player counts', (await focused()) === '2 players', await focused())
    await press(1)
    await check('B closes the dialog', (await wd.count("//div[@role='dialog']")) === 0)
    await check('B leaves you on the game', (await wd.count(panel('Host a game'))) === 1)
    await check('the focus goes back to Host a game', (await focused()).includes('Host a game'), await focused())
    await press(0)
    await press(12)
    await press(12)
    await press(12)
    await check('the D-pad reaches 3 players', (await focused()) === '3 players', await focused())
    await press(0)
    await press(13)
    await press(13)
    await press(13)
    await press(15)
    await check('the D-pad gets back to Host', (await focused()) === 'Host', await focused())
    clearArgs()
    await press(0)
    const a = (await launchedWith()).join(' ')
    await check('A on Host starts the choice', a.includes('--coop-players 3'), a)
    await wd.exec("navigator.getGamepads = () => []; window.dispatchEvent(new Event('gamepaddisconnected')); delete document.hasFocus")
    // Its game closes before the next scenario: the Playing message would lie over what that one clicks.
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
  },

  // One list: nobody's mods first, every game says who made it, and only someone else's says whether we checked it.
  async onelist() {
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    await wd.click(nav('All games'))
    await waitFor(async () => (await wd.count(card('Lab Dev Flash'))) === 1, 20000)
    await check('there is no Community mods list', (await wd.count(nav('Community mods'))) === 0)
    await check('there is no Our order', !(await wd.exec('return document.body.innerText')).includes('Our order'))
    const names = await wd.exec("return [...document.querySelectorAll('.card-c .name')].map(n => n.textContent)")
    const az = [...names].sort((a, b) => (a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0))
    await check('the list runs A to Z, ours and theirs mixed', names.length > 3 && names.join('|') === az.join('|'), names.join(', '))
    const credit = () => wd.exec("return document.querySelector('.credit')?.textContent ?? ''")
    await openGame('Flash Team')
    const ours = await credit()
    await check('our own mod says Made by us, with no review line', ours.includes('Made by') && ours.includes('Multiplayer Everything') && ours.includes('View all') && !ours.includes('review') && !ours.includes('read its code'), ours)
    await key('Escape')
    await openGame('Lab Game')
    const theirs = await credit()
    await check('a community mod says who made it and whether we checked it', theirs.includes('someone') && theirs.includes('View all') && theirs.includes('Not reviewed by us'), theirs)
    await wd.click("//span[contains(@class,'credit')]//button[normalize-space(.)='View all']")
    await check('View all opens that author\'s mods', !!(await wd.find("//h1[normalize-space(.)='Made by someone']", 4000).catch(() => null)))
    const his = await wd.exec("return [...document.querySelectorAll('.card-c .name')].map(n => n.textContent)")
    await check('and only theirs', his.includes('Lab Game') && his.includes('Lab Dev Flash') && !his.includes('Flash Team') && !his.includes('Lab Team'), his.join(', '))
    await wd.click("//button[normalize-space(.)=\"Show everyone's mods\"]")
    await check('Show everyone\'s mods brings the rest back', (await wd.count(card('Flash Team'))) === 1)
    await wd.exec("localStorage.setItem('fl.sort', 'ours'); location.reload()")
    await new Promise((r) => setTimeout(r, 1500))
    await wd.click(nav('All games'))
    await waitFor(async () => (await wd.count(card('Lab Game'))) === 1, 20000)
    const again = await wd.exec("return [...document.querySelectorAll('.card-c .name')].map(n => n.textContent)")
    await check('a saved Our order opens as A to Z', again.join('|') === [...again].sort((a, b) => (a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0)).join('|'), again.join(', '))
  },

  async padjoin() {
    // Joining with only a controller in the desktop window: A on a text field opens the on-screen keyboard over the
    // dialog, the D-pad stays in it, B cancels only the keyboard, and Done leads on to Join.
    await openGame('Co-op Test')
    await wd.exec(`document.hasFocus = () => true
      window.__pad = { buttons: Array.from({ length: 17 }, () => ({ pressed: false })), axes: [0, 0, 0, 0] }
      navigator.getGamepads = () => [window.__pad]
      window.dispatchEvent(new Event('gamepadconnected'))`)
    const press = async (i) => {
      await wd.exec('window.__pad.buttons[arguments[0]].pressed = true', i)
      await new Promise((r) => setTimeout(r, 120))
      await wd.exec('window.__pad.buttons[arguments[0]].pressed = false', i)
      await new Promise((r) => setTimeout(r, 120))
    }
    const focused = () => wd.exec('return document.activeElement?.textContent ?? ""')
    const field = () => wd.exec("return document.querySelector('#join-code')?.value ?? null")
    const sheet = () => wd.count("//div[@role='dialog' and contains(@class,'tv-sheet')]")
    const topKey = (label) => wd.exec(`[...document.querySelectorAll('.tv-kb button')].find(b => b.textContent.endsWith(${JSON.stringify(label)})).focus()`)
    await wd.exec("[...document.querySelectorAll('.dpanel button')].find(b => b.textContent.includes('Join a game')).focus()")
    await press(0)
    await check('A opens the Join dialog on its field', await wd.exec("return document.activeElement?.id === 'join-code'"))
    await press(0)
    await check('A on the field opens the keyboard', (await sheet()) === 1)
    await check('the keyboard opens over the dialog', await wd.exec("const b = document.querySelector('.tv-sheet').getBoundingClientRect(); return !!document.elementFromPoint(b.left + b.width / 2, b.top + 20)?.closest('.tv-sheet')"))
    await check('the focus starts on the keyboard', await wd.exec("return !!document.activeElement?.closest('.tv-kb')"))
    await press(13)
    await check('the D-pad stays in the keyboard', await wd.exec("return !!document.activeElement?.closest('.tv-kb')"))
    await press(1)
    await check('B cancels only the keyboard', (await sheet()) === 0 && (await wd.count("//div[@role='dialog']")) === 1)
    await check('the focus returns to the field', await wd.exec("return document.activeElement?.id === 'join-code'"))
    await press(0)
    await topKey('1')
    await press(0)
    await press(2)
    await press(0)
    for (const ch of '92.168.1.22') await topKey(ch).then(() => press(0))
    await check('the keyboard types what A presses, and X deletes', (await wd.exec("return document.querySelector('.tv-kb-field')?.textContent ?? ''")) === '192.168.1.22', await wd.exec("return document.querySelector('.tv-kb-field')?.textContent ?? ''"))
    await check('every key fits in the keyboard', await wd.exec("const s = document.querySelector('.tv-sheet'); return s.scrollWidth <= s.clientWidth"))
    await check('the keyboard has a colon for a port', await wd.exec("return [...document.querySelectorAll('.tv-kb button')].some(b => b.textContent === ':')"))
    await topKey('Done')
    await press(0)
    await check('Done puts the text in the field', (await field()) === '192.168.1.22', String(await field()))
    await check('the field keeps the focus', await wd.exec("return document.activeElement?.id === 'join-code'"))
    await press(13)
    await press(15)
    await check('the D-pad reaches Join', (await focused()) === 'Join', await focused())
    clearArgs()
    await press(0)
    const a = (await launchedWith()).join(' ')
    await check('A on Join connects, with the usual port', a.includes('--coop-join 192.168.1.22:7790'), a)
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    // The same keyboard for the page's other fields.
    await wd.click(nav('All games'))
    await wd.exec("document.querySelector('.bigsearch input').focus()")
    await press(0)
    await check('A on the search opens the keyboard too', (await sheet()) === 1)
    await press(1)
    await check('B leaves the search as it was', (await sheet()) === 0 && (await wd.count("//h1[@aria-label]")) === 0)
    await wd.exec("navigator.getGamepads = () => []; window.dispatchEvent(new Event('gamepaddisconnected')); delete document.hasFocus")
  },

  async cardplay() {
    await wd.click(nav('All games'))
    await wd.exec("[...document.querySelectorAll('.card-c')].find(c => c.querySelector('.name')?.textContent === 'Relay Test').focus()")
    clearArgs()
    await wd.click("//div[contains(@class,'tile')][.//span[contains(@class,'name') and normalize-space(.)='Relay Test']]//button[contains(@class,'card-play')]")
    const a = (await launchedWith()).join(' ')
    await check('Play on a card starts the game from the grid', a.includes('relay.swf'), a)
    await check('the settings page shows the versions', (await (async () => {
      await wd.click("//button[contains(@class,'setbtn')]")
      return wd.exec("return document.querySelector('.set-foot')?.textContent ?? ''")
    })()).includes('Fake Player'))
    await wd.click(button('Copy diagnostics'))
    await check('Copy diagnostics puts a report on the clipboard', (await waitFor(() => clipboard().includes('Multiplayer Everything Launcher') && clipboard().includes('Fake Player'), 4000)) === true)
  },

  async lobbyquick() {
    await openGame('Lobby Test')
    await wd.click(panel('Quick play'))
    // The sandbox's central server lists rooms but doesn't speak the lobby, so joining ends at "Can't connect":
    // what counts is that Quick play went straight for the busiest room without the setup form's buttons.
    const went = await waitFor(async () => (await wd.count("//button[normalize-space(.)='Leave room']")) > 0 ||
      (await wd.exec("return document.querySelector('.lobby-note')?.textContent ?? ''")).length > 0, 10000)
    await check('Quick play on a lobby game goes straight for a room', !!went)
    if ((await wd.count("//button[normalize-space(.)='Leave room']")) > 0) await wd.click(button('Leave room'))
  },

  async stats() {
    // A game played and closed, so the play is counted.
    await openGame('Relay Test')
    const plain = await launch('Play')
    await check('outside TV mode a game gets no TV options', plain.length > 0 && !plain.includes('--fullscreen') && !plain.some((a) => a.startsWith('ENV ')), plain.join(' '))
    await waitFor(() => (settings().games?.relaygame?.plays ?? 0) > 0, FAKE_MS + 8000)
    await openGame('Relay Test')
    const played = await wd.exec("return [...document.querySelectorAll('.stats > div')].find(d => d.querySelector('span')?.textContent === 'Played')?.querySelector('b')?.textContent")
    await check('Played never says Not yet after a game was played', played && played !== 'Not yet', played)
  },

  async tv() {
    const fullscreen = () =>
      wd.execAsync("const done = arguments[arguments.length - 1]; window.__TAURI_INTERNALS__.invoke('plugin:window|is_fullscreen', { label: 'main' }).then(done, () => done(null))")
    const hero = () => wd.exec("return document.querySelector('.tv-hero h1')?.getAttribute('aria-label') ?? ''")
    await key('F11')
    await check('F11 opens TV mode', !!(await waitFor(async () => (await wd.count("//div[@id='tv']")) > 0)))
    await check('TV mode fills the screen', (await waitFor(fullscreen)) === true)
    await check('a game has the focus to start from', !!(await waitFor(() => wd.exec("return document.activeElement?.classList.contains('tv-tile')"))))
    const first = await hero()
    // The title decodes as the selection moves: scrambled for a moment, then the name.
    const seen = await wd.execAsync(`const done = arguments[arguments.length - 1], got = []
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight' }))
      const id = setInterval(() => got.push(document.querySelector('.tv-hero h1 span')?.textContent ?? ''), 25)
      setTimeout(() => { clearInterval(id); done(got) }, 900)`)
    const second = await waitFor(async () => { const h = await hero(); return h && h !== first && h })
    await check('moving along the row shows that game at the top', !!second && second === (await wd.exec("return document.activeElement?.querySelector('.name')?.textContent")), `${first} -> ${second}`)
    await check('the title decodes on a new selection', seen.some((s) => s && s !== second) && seen.at(-1) === second, seen.slice(0, 4).join(' / '))
    const hints = await wd.exec("return document.querySelector('.tv-hints')?.textContent ?? ''")
    await check('on a game the hints say what Enter does there', hints.includes('EnterPlay'), hints)
    const coopCard = await wd.exec("return [...document.querySelectorAll('.tv-row .tv-tile')].find(t => t.querySelector('.name')?.textContent === 'Co-op Test')?.querySelector('.st')?.textContent ?? ''")
    await check('a card shows how many rooms are open', /\d+ open/.test(coopCard), coopCard)
    await key('ArrowUp')
    await check('up from the row reaches the game\'s buttons', !!(await waitFor(() => wd.exec("return !!document.activeElement?.closest('.tv-hero')"))))

    // A mouse click picks a game for the top without starting it; A (a click with no mouse) and a double-click play.
    const relayTile = `//button[contains(@class,'tv-tile')][.//span[contains(@class,'name') and normalize-space(.)=${q('Relay Test')}]]`
    clearArgs()
    await wd.click(relayTile)
    await check('a mouse click picks the game without starting it',
      (await waitFor(async () => (await hero()) === 'Relay Test')) === true && !(await waitFor(() => existsSync(box.args), 1500)))
    await wd.exec("document.activeElement.dispatchEvent(new MouseEvent('dblclick', { bubbles: true, detail: 2 }))")
    const dbl = await launchedWith()
    await check('a double-click plays it', dbl.some((a) => a.includes('.swf')), dbl.join(' '))
    await waitFor(async () => (await wd.count("//div[contains(@class,'tv-starting')]")) > 0)
    await key('Escape')
    await waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    clearArgs()
    await wd.click(relayTile)
    await wd.exec('document.activeElement.click()')
    const tvRun = await launchedWith()
    await check('a game in the row starts with A', tvRun.some((a) => a.includes('.swf')))
    await check('a game started from TV mode gets the player\'s TV options', tvRun.includes('--fullscreen') && tvRun.includes('ENV FAKE_TV=1'), tvRun.join(' '))
    await check('starting a game shows its TV screen, not the desktop',
      (await wd.exec("return document.querySelector('.tv-starting h1')?.textContent ?? ''")) === 'Relay Test')
    await key('Escape')
    await check('B goes back to TV mode while the game runs', !!(await waitFor(async () => (await wd.count("//div[contains(@class,'tv-starting')]")) === 0)))

    await key('PageDown')
    await check('RB goes to Games', !!(await waitFor(async () => (await wd.count("//div[contains(@class,'tv-lib')]")) > 0)))
    await check('Games shows every game', (await waitFor(async () => (await wd.count("//div[contains(@class,'tv-grid')]//button[contains(@class,'tv-tile')]")) === 16)) === true)
    await check('TV mode has no Community filter', (await wd.count("//button[contains(@class,'tv-chip')][contains(normalize-space(.),'Community')]")) === 0)
    await wd.click("//button[contains(@class,'tv-chip')][starts-with(normalize-space(.),'PC mods')]")
    await check('a filter narrows the games', (await waitFor(async () => (await wd.count("//div[contains(@class,'tv-grid')]//button[contains(@class,'tv-tile')]")) === 5)) === true)
    await key('Escape')
    await check('B goes back Home', !!(await waitFor(async () => (await wd.count("//div[contains(@class,'tv-home')]")) > 0)))
    await wd.click("//section[contains(@class,'tv-hero')]//button[contains(normalize-space(.),'Game page')]")
    await check('a game\'s page opens in TV mode', !!(await waitFor(async () => (await wd.count("//div[@id='tv']//h1[@aria-label]")) > 0)))
    await key('Escape')

    // The game started above closes while TV mode is on: the window stays in TV mode and full screen. With Minimize
    // while a game runs on (the launcher's default) TV mode doesn't minimize: its starting screen stays behind the game.
    const gameClosed = () => waitFor(async () => !(await wd.exec("return document.querySelector('.island')?.textContent ?? ''")).includes('Playing'), FAKE_MS + 8000)
    const stillTv = async () => (await wd.count("//div[@id='tv']")) > 0 && (await fullscreen()) === true
    await gameClosed()
    await check('TV mode stays on when the game closes', await waitFor(stillTv, 3000) === true,
      (await wd.exec("return JSON.parse(localStorage.getItem('fl.tvLog') ?? '[]')")).slice(-4).join(' | '))
    await wd.click("//button[contains(@class,'tv-tab') and normalize-space(.)='Settings']")
    await wd.click("//button[@role='switch' and contains(@aria-label,'Minimize')]")
    await waitFor(() => settings().minimize === 1 || settings().minimize === true)
    await key('Escape')
    clearArgs()
    await wd.click(relayTile)
    await wd.exec('document.activeElement.click()')
    await new Promise((r) => setTimeout(r, 2500))
    const behind = [await isMinimized(), await fullscreen(), await wd.count("//div[contains(@class,'tv-starting')]")]
    await check('TV mode stays full screen behind the game instead of minimizing', behind[0] === false && behind[1] === true && behind[2] > 0,
      `minimized ${behind[0]}, fullscreen ${behind[1]}, starting screens ${behind[2]}`)
    await gameClosed()
    await check('the starting screen goes when the game closes', !!(await waitFor(async () => (await wd.count("//div[contains(@class,'tv-starting')]")) === 0, 3000)))
    await check('TV mode is still on, full screen, after the game', await waitFor(stillTv, 3000) === true,
      `tv ${await wd.count("//div[@id='tv']")}, fullscreen ${await fullscreen()}, ` +
        (await wd.exec("return JSON.parse(localStorage.getItem('fl.tvLog') ?? '[]')")).slice(-3).join(' | '))
    await wd.click("//button[contains(@class,'tv-tab') and normalize-space(.)='Settings']")
    await wd.click("//button[@role='switch' and contains(@aria-label,'Minimize')]")
    await waitFor(() => !settings().minimize)
    await key('Escape')

    // An online game asks how to play: a sheet the D-pad stays in, then the on-screen keyboard for a room code.
    await wd.click(`//button[contains(@class,'tv-tile')][.//span[contains(@class,'name') and normalize-space(.)=${q('Co-op Test')}]]`)
    await wd.exec('document.activeElement.click()')
    const items = await waitFor(async () => { const t = await wd.exec("return [...document.querySelectorAll('.tv-item b')].map(b => b.textContent)"); return t.length > 0 && t })
    await check('A on an online game asks how to play', !!items && items.includes('Quick play') && items.includes('Join with a code'), String(items))
    await check('the sheet lists the game\'s open rooms', !!items && items.some((i) => i.startsWith('Join ') && i !== 'Join with a code'), String(items))
    for (let i = 0; i < 12; i++) await key('ArrowDown')
    await check('the D-pad stays inside the sheet', await wd.exec("return !!document.activeElement?.closest('.tv-sheet')"))
    await wd.click("//button[contains(@class,'tv-item')][.//b[normalize-space(.)='Join with a code']]")
    await wd.find("//div[contains(@class,'tv-kb')]")
    for (const ch of 'kx7q2') await key(ch)
    const typed = await wd.exec("return [...document.querySelectorAll('.tv-kb-field .cell')].map(c => c.textContent).join('')")
    await check('a keyboard types into the on-screen keyboard', typed === 'KX7Q2', typed)
    await wd.click("//div[contains(@class,'tv-kb')]//button[normalize-space(.)='Q']")
    await check('the on-screen keys stop at five letters', (await wd.exec("return [...document.querySelectorAll('.tv-kb-field .cell')].map(c => c.textContent).join('')")) === 'KX7Q2')
    clearArgs()
    await key('Enter')
    const joined = await launchedWith()
    await check('Enter joins with the typed code', joined.includes('--coop-code') && joined.includes('KX7Q2'), joined.join(' '))

    // A text field with the controller: the page gets a scripted gamepad (the Gamepad API it reads), A on Your name
    // opens the on-screen keyboard, and Done saves the name as typing would.
    await key('Escape')  // the joined game's starting screen
    await wd.click("//button[contains(@class,'tv-tab') and normalize-space(.)='Settings']")
    await wd.exec(`const pad = { id: 'Lab controller', index: 0, connected: true, mapping: 'standard', axes: [0, 0, 0, 0],
        buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) }
      window.__labPad = pad
      navigator.getGamepads = () => [window.__labPad]
      window.dispatchEvent(new Event('gamepadconnected'))`)
    const padPress = async (b) => {
      await wd.exec(`window.__labPad.buttons[${b}] = { pressed: true, value: 1 }`)
      await new Promise((r) => setTimeout(r, 150))
      await wd.exec(`window.__labPad.buttons[${b}] = { pressed: false, value: 0 }`)
      await new Promise((r) => setTimeout(r, 150))
    }
    await wd.exec("document.querySelector('.tv-page input.field')?.focus()")
    await padPress(0)
    const title = await waitFor(() => wd.exec("return document.querySelector('.tv-sheet h2')?.textContent ?? ''"))
    await check('A on a text field opens the on-screen keyboard', title === 'Your name', String(title))
    for (let i = 0; i < 10; i++) await padPress(2)
    for (const ch of 'Ann') await key(ch)
    await wd.click("//div[contains(@class,'tv-kb')]//button[normalize-space(.)='Done']")
    await check('Done saves what was typed', (await waitFor(() => settings().playerName === 'Ann', 5000)) === true, settings().playerName)
    await check('the focus goes back to the field', await wd.exec("return document.activeElement?.matches('.tv-page input.field') ?? false"))
    // Put the name back for the scenarios after this one.
    await padPress(0)
    await wd.find("//div[contains(@class,'tv-kb')]")
    for (let i = 0; i < 5; i++) await padPress(2)
    for (const ch of 'Tester') await key(ch)
    await key('Enter')
    await waitFor(() => settings().playerName === 'Tester', 5000)
    await padPress(1)  // B: Settings back to Home

    await key('F11')
    await check('F11 leaves TV mode', !!(await waitFor(async () => (await wd.count("//nav[contains(@class,'navs')]")) > 0)))
    await check('the window gets its size back', (await waitFor(async () => (await fullscreen()) === false)) === true)
    // A held F11 repeats; only the first press counts.
    await wd.exec("window.dispatchEvent(new KeyboardEvent('keydown', { key: 'F11', repeat: true }))")
    await new Promise((r) => setTimeout(r, 400))
    await check('a repeating F11 doesn\'t switch TV mode', (await wd.count("//div[@id='tv']")) === 0)
    const log = await wd.exec("return JSON.parse(localStorage.getItem('fl.tvLog') ?? '[]')")
    await check('each switch of TV mode is recorded with its cause', log.some((l) => l.includes('on: F11')) && log.some((l) => l.includes('off: F11')), log.slice(-3).join(' | '))
  },
}

// The app's update installs and closes the app, so it runs last, on fresh starts of the window.
scenarios.appupdate = async () => {
  const sign = (file) => {
    rmSync(`${file}.sig`, { force: true })
    spawnSync(`npx tauri signer sign -f "${updKey}" -p "" "${file}"`, { cwd: tauriDir, shell: true, stdio: 'ignore' })
    return readFileSync(`${file}.sig`, 'utf8').trim()
  }
  const setup = join(updDir, 'MultiplayerEverything-setup.exe')
  writeFileSync(setup, readFileSync(fakePlayer))  // a copy of the test exe: it notes "RAN <name> <arguments>"
  const feed = (signature) => writeFileSync(join(updDir, 'latest.json'), JSON.stringify({
    version: '9.9.9', notes: 'Lab release', pub_date: '2026-11-04T00:00:00Z',
    platforms: { 'windows-x86_64': { signature, url: `http://127.0.0.1:${PORTS.http}/upd/MultiplayerEverything-setup.exe` } },
  }))
  const restart = async () => {
    await wd.stop()
    await wd.start(app)
    await wd.find("//nav[contains(@class,'navs')]", 30000)
  }
  const prompt = "//div[contains(@class,'isl') and contains(@class,'upd')][.//b[contains(.,'9.9.9 is out')]]"
  await check('no feed, no update prompt', (await wd.count(prompt)) === 0)

  feed(sign(setup))
  await restart()
  await check('a newer signed release is offered at start', !!(await wd.find(prompt, 15000).catch(() => null)))
  const text = await wd.exec(`return document.evaluate(${JSON.stringify(prompt)}, document, null, 9, null).singleNodeValue?.textContent ?? ''`)
  await check('the prompt names the version the player has', /You have \d+\.\d+\.\d+/.test(text), text)
  await wd.click(`${prompt}${button('Later')}`)
  await check('Later puts it away', !!(await waitFor(async () => (await wd.count(prompt)) === 0, 3000)))

  // A file signed with another key: the app refuses it and stays.
  const other = join(work, 'other.bin')
  writeFileSync(other, 'not the installer')
  feed(sign(other))
  clearArgs()
  await restart()
  await wd.click(`${prompt}${button('Update now')}`, 15000)
  await check('an installer whose signature doesn\'t match is refused', !!(await waitFor(async () => /couldn't be installed/i.test(await toast()), 15000)), await toast())
  await check('and never runs', !existsSync(box.args) || !readFileSync(box.args, 'utf8').includes('RAN '))

  feed(sign(setup))
  await restart()
  await wd.click(`${prompt}${button('Update now')}`, 15000)
  const ran = await waitFor(() => existsSync(box.args) && readFileSync(box.args, 'utf8').split(/\r?\n/).find((l) => l.startsWith('RAN ')), 20000)
  await check('Update now downloads, checks the signature and starts the installer', !!ran, ran || '')
  await check('the installer runs passive, as an update, and opens the app again', !!ran && / \/P\b/.test(ran) && /\/UPDATE\b/.test(ran) && /\/R\b/.test(ran), ran || '')
}

// ---- run ---------------------------------------------------------------------------------------------------------
let code = 0
try {
  await waitFor(() => fetch('http://127.0.0.1:4444/status').then((r) => r.ok), 10000)
  await wd.start(app)
  await wd.find("//nav[contains(@class,'navs')]", 30000)
  // Errors the page throws (a crash leaves it blank), kept for the FAIL lines.
  await wd.exec(`window.__labErrors = []
    addEventListener('error', (e) => window.__labErrors.push(e.message + ' @ ' + String(e.error?.stack ?? '').split(String.fromCharCode(10)).slice(1, 4).join(' < ')))
    addEventListener('unhandledrejection', (e) => window.__labErrors.push(String(e.reason)))`)
  for (const [name, run] of Object.entries(scenarios)) {
    if (only && !only.includes(name)) continue
    console.log(`\n${name}`)
    try {
      await run()
    } catch (e) {
      await check(`${name} ran to the end`, false, e.message)
    }
  }
  console.log(`\n${passed} passed, ${failed} failed${failed ? ` (screenshots in ${fails})` : ''}`)
  code = failed ? 1 : 0
} catch (e) {
  console.error(`The lab couldn't start: ${e.message}`)
  const shown = await wd.exec('return location.href + "\\n" + document.body.innerText.slice(0, 600)').catch((x) => `(no page: ${x.message})`)
  console.error(`The window showed: ${shown}`)
  code = 2
} finally {
  await wd.stop()
  driverProc.kill()
  stopServers()
  if (!argv.includes('--keep') && !failed) {
    // A fake game may still hold a file for a few seconds; a sandbox left in %TEMP% is harmless.
    try {
      rmSync(work, { recursive: true, force: true, maxRetries: 5, retryDelay: 1000 })
    } catch {
      console.log(`Sandbox left for later: ${work}`)
    }
  } else console.log(`Sandbox kept: ${work}`)
}
process.exit(code)
