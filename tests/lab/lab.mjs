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

const env = {
  ...process.env,
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
      return c['All games'] === '10' && c
    })
    await check('the sidebar counts every game', !!counts, JSON.stringify(counts))
    await check('the sidebar counts each category', counts && counts['Flash mods'] === '5' && counts['PC mods'] === '1' && counts['Online multiplayer'] === '3' && counts['Community mods'] === '2' && counts['Coming soon'] === '1', JSON.stringify(counts))
    await wd.click(nav('Flash mods'))
    await check('a category shows its games', (await waitFor(async () => (await wd.count("//button[contains(@class,'card-c')]")) === 5)) === true)
    await wd.click(nav('All games'))
    await wd.type("//input[@placeholder='Search games and mods']", 'relay')
    const found = await waitFor(async () => (await wd.exec("return [...document.querySelectorAll('.card-c .name')].map(e => e.textContent)")).join('|') === 'Relay Test')
    await check('search finds a game by name', !!found)
    await wd.click("//button[@aria-label='Clear the search']")
    await wd.click(button('A to Z'))
    const names = await waitFor(async () => {
      const n = await wd.exec("return [...document.querySelectorAll('.card-c .name')].map(e => e.textContent)")
      return n.length === 10 && n
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

  // A community mod approved into mods-info (community/lab-coop.json): it arrives without a launcher update, installs
  // its pinned release and uninstalls cleanly. The website's second project, with no record, is listed but can't install.
  async communitymods() {
    const labBefore = snapshot(box.labGame)
    await wd.click(nav('All games'))
    const arrived = await waitFor(async () => (await wd.count(card('Lab Game'))) === 1 && (await wd.count(card('Listed Only'))) === 1, 20000)
    await check('approved community mods and listed projects come from mods-info and the website', arrived === true)
    if (!arrived) return
    const saved = existsSync(join(work, 'data', 'community.json')) ? readFileSync(join(work, 'data', 'community.json'), 'utf8') : ''
    await check('the records are kept in the data folder for the mod commands', saved.includes('lab-coop'))
    await openGame('Lab Game')
    const page = await wd.exec("return document.querySelector('.gp')?.innerText ?? ''")
    await check('the card has the website\'s description', page.includes('Four players in one lab.'), page.slice(0, 200))
    await check('the card says whose mod it is', page.includes('by someone'), page.slice(0, 300))
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
    await check('a folder inside the zip is copied with its own subfolders', existsSync(join(box.labGame, 'BepInEx', 'plugins', 'LabCoop', 'lang', 'en.txt')) && !existsSync(join(box.labGame, 'BepInEx', 'plugins', 'LabCoop', 'LabCoop')))
    await check('the community mod shows as installed', (await waitFor(async () => (await pill()).includes('Installed'), 15000)) === true, await pill())
    await waitFor(async () => (await wd.count(panel('Uninstall'))) === 1, 30000)
    await wd.click(panel('Uninstall'))
    await wd.click(inDialog('Uninstall'))
    await waitFor(async () => wd.exec("return [...document.querySelectorAll('.dpanel .pbtn')].some(b => b.textContent.includes('Install'))"), 30000)
    await check('Uninstall leaves the community game folder exactly as it was', snapshot(box.labGame) === labBefore)
    await key('Escape')

    // community/lab-solo.json: one DLL released on its own, the record the website's form writes when there's no manifest.
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

    // Ctrl+F5: a mod approved a minute ago comes now, not when the saved copy's hour is up; one taken down goes again.
    const www = join(work, 'www', 'community')
    const indexBefore = readFileSync(join(www, 'index.json'), 'utf8')
    const late = JSON.parse(readFileSync(join(www, 'lab-solo.json'), 'utf8'))
    late.id = late.mod.id = 'lab-late'
    late.mod.game.name = 'Lab Late'
    writeFileSync(join(www, 'lab-late.json'), JSON.stringify(late))
    writeFileSync(join(www, 'index.json'), JSON.stringify({ mods: [...JSON.parse(indexBefore).mods, 'lab-late'] }))
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
    await ctrlF5()
    await check('Ctrl+F5 drops a mod taken down from mods-info', (await waitFor(async () => (await wd.count(card('Lab Late'))) === 0, 20000)) === true)
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
    await check('Games shows every game', (await waitFor(async () => (await wd.count("//div[contains(@class,'tv-grid')]//button[contains(@class,'tv-tile')]")) === 10)) === true)
    await wd.click("//button[contains(@class,'tv-chip')][starts-with(normalize-space(.),'PC mods')]")
    await check('a filter narrows the games', (await waitFor(async () => (await wd.count("//div[contains(@class,'tv-grid')]//button[contains(@class,'tv-tile')]")) === 1)) === true)
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
