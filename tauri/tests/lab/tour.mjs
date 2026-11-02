// A look at the window with the real library, for reviewing the design: it only moves between pages (it starts
// nothing and changes no setting) and saves a screenshot of each. node tests/lab/tour.mjs <out-folder> [game ids]
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Driver, q, waitFor } from './webdriver.mjs'

const here = dirname(fileURLToPath(import.meta.url))
const tauriDir = resolve(here, '..', '..')
const out = resolve(process.argv[2] ?? 'tour')
const games = (process.argv[3] ?? '').split(',').filter(Boolean)
const launcher = process.env.FL_LAUNCHER_EXE ?? resolve(tauriDir, '..', '..', 'flash-launcher-multiplayer', 'build', 'Debug', 'MultiplayerEverythingLauncher.exe')
const edgeDir = join(process.env.LOCALAPPDATA ?? '', 'fl-lab')
const edgeDriver = process.env.MSEDGEDRIVER ?? readdirSync(edgeDir).map((d) => join(edgeDir, d, 'msedgedriver.exe')).find(existsSync)
// FL_APP: another build of the window, such as the shipped layout with the launcher beside it (then it must find
// the launcher on its own, so FL_LAUNCHER_EXE is left out).
const app = process.env.FL_APP ?? join(process.env.CARGO_TARGET_DIR ?? join(tauriDir, 'src-tauri', 'target'), 'debug', 'app.exe')
const env = { ...process.env, FL_NO_DISCORD: '1' }
if (process.env.FL_APP) delete env.FL_LAUNCHER_EXE
else env.FL_LAUNCHER_EXE = launcher
delete env.FL_LAUNCHER_CONFIG
const driver = spawn('tauri-driver', ['--port', '4444', '--native-driver', edgeDriver], { env, stdio: 'ignore' })
const wd = new Driver('http://127.0.0.1:4444')
mkdirSync(out, { recursive: true })
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const shot = async (name) => {
  await sleep(900)
  writeFileSync(join(out, `${name}.png`), Buffer.from(await wd.screenshot(), 'base64'))
}
const nav = (label) => `//nav[contains(@class,'navs')]//button[.//span[contains(@class,'lbl') and normalize-space(.)=${q(label)}]]`
try {
  await waitFor(() => fetch('http://127.0.0.1:4444/status').then((r) => r.ok), 10000)
  await wd.start(app)
  await wd.find("//nav[contains(@class,'navs')]", 40000)
  await sleep(4000)  // the rooms and covers
  // TOUR_THEME=light: the launcher.json light palette, for checking it without another config.
  if (process.env.TOUR_THEME) await wd.exec('document.documentElement.dataset.theme = arguments[0]', process.env.TOUR_THEME)
  await shot('home')
  await wd.click(nav('All games'))
  await shot('library')
  await wd.click(nav('Rooms'))
  await sleep(2000)
  await shot('rooms')
  await wd.click("//button[contains(@class,'setbtn')]")
  await shot('settings')
  for (const id of games) {
    await wd.exec('arguments[0]', id)
    await wd.click(nav('All games'))
    const name = await wd.exec('return arguments[0]', id)
    await wd.exec("window.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', ctrlKey: true }))")
    await wd.type("//input[@aria-label='Go to a game or page']", name)
    await sleep(300)
    await wd.type("//input[@aria-label='Go to a game or page']", '', false)
    await sleep(1500)
    await shot(`game-${id.replace(/[^a-z0-9]+/gi, '-')}`)
  }
  console.log(`Saved to ${out}`)
} finally {
  await wd.stop()
  driver.kill()
}
