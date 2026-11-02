// Screenshots of the first-run setup, step by step, as a brand-new player sees it (no name yet), for reviewing the
// design: node tests/lab/shots.mjs <out-folder>. Uses the lab's sandbox, servers and the last lab build.
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { PORTS, makeSandbox } from './sandbox.mjs'
import { startServers } from './servers.mjs'
import { Driver, button, waitFor } from './webdriver.mjs'

const here = dirname(fileURLToPath(import.meta.url))
const tauriDir = resolve(here, '..', '..')
const out = resolve(process.argv[2] ?? 'setup-shots')
const launcher = process.env.FL_LAUNCHER_EXE ?? resolve(tauriDir, '..', '..', 'flash-launcher-multiplayer', 'build', 'Debug', 'MultiplayerEverythingLauncher.exe')
const edgeDir = join(process.env.LOCALAPPDATA ?? '', 'fl-lab')
const edgeDriver = process.env.MSEDGEDRIVER ?? readdirSync(edgeDir).map((d) => join(edgeDir, d, 'msedgedriver.exe')).find(existsSync)
const app = join(process.env.CARGO_TARGET_DIR ?? join(tauriDir, 'src-tauri', 'target'), 'debug', 'app.exe')

const work = join(tmpdir(), `fl-tauri-shots-${process.pid}`)
const box = makeSandbox(work, join(dirname(launcher), 'launcher_tests.exe'))
writeFileSync(box.settings, JSON.stringify({ online: true }))  // no name: the setup opens by itself
const stop = startServers(work, PORTS)
const env = { ...process.env, FL_LAUNCHER_EXE: launcher, FL_LAUNCHER_CONFIG: box.config, FL_NO_NAME_PROMPT: '1', FL_NO_DISCORD: '1' }
const driver = spawn('tauri-driver', ['--port', '4444', '--native-driver', edgeDriver], { env, stdio: 'ignore' })
const wd = new Driver('http://127.0.0.1:4444')
mkdirSync(out, { recursive: true })
const shot = async (name) => {
  await new Promise((r) => setTimeout(r, 600))
  writeFileSync(join(out, `${name}.png`), Buffer.from(await wd.screenshot(), 'base64'))
}
try {
  await waitFor(() => fetch('http://127.0.0.1:4444/status').then((r) => r.ok), 10000)
  await wd.start(app)
  await wd.find("//h1[starts-with(normalize-space(.),'Welcome to')]", 30000)
  await shot('1-welcome')
  await wd.click(button('Continue'))
  await wd.type("//input[@aria-label='Your name']", 'Ford')
  await shot('2-name')
  await wd.click(button('Continue'))
  await shot('3-player')
  await wd.click(button('Install'))
  await waitFor(async () => (await wd.count("//span[@aria-label='Installed']")) === 1, 20000)
  await shot('3b-player-installed')
  await wd.click(button('Continue'))
  await waitFor(async () => (await wd.exec("return document.querySelector('.setup-card')?.textContent ?? ''")).includes('Not found'), 10000)
  await shot('4-pc-games')
  await wd.click(button('Continue'))
  await shot('5-folder')
  await wd.click(button('Continue'))
  await shot('6-choices')
  await wd.click(button('Continue'))
  await shot('7-done')
  console.log(`Saved to ${out}`)
} finally {
  await wd.stop()
  driver.kill()
  stop()
  try {
    rmSync(work, { recursive: true, force: true, maxRetries: 3, retryDelay: 500 })
  } catch {
    // left in %TEMP%
  }
}
