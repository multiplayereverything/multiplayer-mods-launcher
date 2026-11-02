// The lab's sandbox: its own launcher.json, data folder, fake games, a fake PC game folder and the files the local
// servers hand out (catalog, mods-info, the mod package). Everything lives under one temp folder; nothing outside it
// is read or written, so a lab run never touches the real launcher's settings or a real game.
import { createHash, randomBytes } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { join, relative } from 'node:path'

export const PORTS = { http: 48811, rooms: 48812, relay: 48813, lobby: 48814 }

const sha256 = (file) => createHash('sha256').update(readFileSync(file)).digest('hex')

function write(file, text) {
  mkdirSync(join(file, '..'), { recursive: true })
  writeFileSync(file, text)
}

// Every file under `dir` with its SHA-256, so a mod uninstall can be checked to leave the folder byte-identical.
export function snapshot(dir) {
  const out = []
  const walk = (d) => {
    for (const name of readdirSync(d)) {
      const p = join(d, name)
      if (statSync(p).isDirectory()) walk(p)
      else out.push(`${relative(dir, p)} ${sha256(p)}`)
    }
  }
  walk(dir)
  return out.sort().join('\n')
}

export function makeSandbox(work, fakePlayer) {
  rmSync(work, { recursive: true, force: true })
  for (const f of ['coop.swf', 'relay.swf', 'lob.swf']) write(join(work, 'games', f), 'not a real swf')
  write(join(work, 'elsewhere', 'my copy.swf'), 'a copy the player already has')

  // The catalog's file for the game that is missing.
  const dl = join(work, 'www', 'games', 'dlgame', 'dl.swf')
  write(dl, 'pretend game file')
  write(join(work, 'www', 'catalog.json'), JSON.stringify({
    schemaVersion: 1,
    files: [{ name: 'dl.swf', game: 'dlgame', path: 'games/dlgame/dl.swf', size: statSync(dl).size, sha256: sha256(dl) }],
  }))

  // The fake PC game and the mod package mods-info points at.
  const gameDir = join(work, 'FakeGame')
  write(join(gameDir, 'Game.exe'), 'game binary')
  write(join(gameDir, 'Data', 'original.dat'), 'ORIGINAL')
  const pkg = join(work, 'pkg')
  write(join(pkg, 'core', 'Mod.dll'), 'mod dll 1.0.0')
  write(join(pkg, 'core', 'config', 'settings.ini'), 'default=1.0.0')
  write(join(pkg, 'override', 'Data', 'original.dat'), 'MODDED 1.0.0')
  const zip = join(work, 'www', 'dl', 'modtest.zip')
  mkdirSync(join(zip, '..'), { recursive: true })
  execFileSync('powershell', ['-NoProfile', '-Command', `Compress-Archive -Path '${pkg}\\*' -DestinationPath '${zip}'`])
  write(join(work, 'www', 'mods', 'modtest.json'), JSON.stringify({
    schema: 1, id: 'modtest', name: 'Mod Test', game: 'Fake Game', engine: 'none', status: 'testing',
    latest_version: '1.0.0', min_version: '1.0.0', protocol: 'x', download_url: `http://127.0.0.1:${PORTS.http}/dl/modtest.zip`,
    download_sha256: sha256(zip), changelog_url: '', relay: { mode: 'tunnel', max_players: 4, server: 'main' }, notice: '', updated: '2026-11-01',
  }))

  // A stand-in for Ruffle Together that the first-run setup installs from the local server. It never runs: the
  // fake player comes first in the list, so games keep starting that one.
  const ruffleSrc = join(work, 'ruffle-src')
  write(join(ruffleSrc, 'ruffle.exe'), 'not a real player')
  const ruffleZip = join(work, 'www', 'ruffle.zip')
  execFileSync('powershell', ['-NoProfile', '-Command', `Compress-Archive -Path '${ruffleSrc}\\*' -DestinationPath '${ruffleZip}'`])

  // A player the launcher installed from a GitHub release, one release behind: the local server answers as GitHub's
  // API does with v1.0.1, so the window offers the update at start and the lab installs it with its progress.
  write(join(work, 'players', 'rt', 'rt.exe'), 'updater test 1.0.0')
  write(join(work, 'players', 'rt', '.version'), 'v1.0.0')
  const rtSrc = join(work, 'rt-src')
  write(join(rtSrc, 'rt.exe'), randomBytes(3_000_000))  // random, so the zip stays big enough to show progress
  const rtZip = join(work, 'www', 'rt-1.0.1.zip')
  execFileSync('powershell', ['-NoProfile', '-Command', `Compress-Archive -Path '${rtSrc}\\*' -DestinationPath '${rtZip}'`])
  write(join(work, 'www', 'repos', 'o', 'rt', 'releases', 'latest'), JSON.stringify({
    tag_name: 'v1.0.1', draft: false,
    assets: [{ name: 'rt-1.0.1.zip', browser_download_url: `http://127.0.0.1:${PORTS.http}/rt-1.0.1.zip`, digest: `sha256:${sha256(rtZip)}` }],
  }))

  const swf = (name) => [`\${GAMES_DIR}\\${name}`]
  const config = {
    branding: { title: 'Multiplayer Everything', subtitle: 'Test lab' },
    paths: { games: '${LAUNCHER_DIR}\\games', players: '${LAUNCHER_DIR}\\players', data: '${LAUNCHER_DIR}\\data', flashpoint: [] },
    downloads: { catalog: `http://127.0.0.1:${PORTS.http}/catalog.json`, modsInfo: `http://127.0.0.1:${PORTS.http}/` },
    rooms: { server: `127.0.0.1:${PORTS.rooms}`, refreshSeconds: 10 },
    players: [
      { id: 'fake', name: 'Fake Player', paths: [fakePlayer], args: ['--fake-player'], flashvarArg: '-P${KEY}=${VALUE}', coopSaveArg: '--coop-save',
        tv: { args: ['--fullscreen'], env: { FAKE_TV: '1' } } },
      { id: 'second', name: 'Second Player', paths: [fakePlayer], args: ['--fake-player', '--second-player'] },
      { id: 'ruffle', name: 'Ruffle Together', paths: ['${PLAYERS_DIR}\\ruffle\\ruffle.exe'],
        install: { url: `http://127.0.0.1:${PORTS.http}/ruffle.zip`, sha256: sha256(ruffleZip), dir: '${PLAYERS_DIR}\\ruffle', exe: 'ruffle.exe' } },
      // Last, so no game picks it as its automatic player.
      { id: 'rt', name: 'Updater Test', paths: ['${PLAYERS_DIR}\\rt\\rt.exe'],
        install: { github: 'o/rt', asset: 'rt-*.zip', api: `http://127.0.0.1:${PORTS.http}`, dir: '${PLAYERS_DIR}\\rt', exe: 'rt.exe' } },
    ],
    categories: [
      { id: 'flash', name: 'Flash mods', group: 'platform' }, { id: 'pc', name: 'PC mods', group: 'platform' },
      { id: 'online', name: 'Online multiplayer' }, { id: 'coop', name: 'Co-op' },
    ],
    games: [
      {
        id: 'coop', name: 'Co-op Test', players: '1-4 players (online co-op)', maxPlayers: 4, online: { id: 'ruffle-coop', join: 'online' },
        settings: [
          { id: 'ff', type: 'checkbox', scope: 'host', label: 'Friendly fire', args: ['--friendly-fire'] },
          { id: 'speed', type: 'choice', scope: 'all', label: 'Game speed', default: 'normal',
            choices: [{ id: 'normal', label: 'Normal', args: [] }, { id: 'fast', label: 'Fast', args: ['--fast'] }] },
        ],
        categories: ['flash', 'online', 'coop'], description: 'Stands in for a Ruffle Together co-op game.',
        guide: { steps: ['Press Host a game.', 'Send the code to a friend.', 'Start the mission.'], tips: ['A tip.'] },
        actions: [
          { id: 'online', label: 'Play online', icon: 'E774', description: 'Host a room for 2 players, or join one with a code', swf: swf('coop.swf'),
            hint: 'Send the room code the game shows to your friend.',
            args: ['--coop-host-relay', '${SERVER}', '--coop-name', '${PLAYER_NAME}', '--coop-players', '2'],
            joinArgs: ['--coop-join-relay', '${SERVER}', '--coop-code', '${CODE}', '--coop-name', '${PLAYER_NAME}'] },
          { id: 'room3', label: 'Host a room for 3 players', icon: 'E774', more: true, description: 'Starts when 2 friends have joined', swf: swf('coop.swf'),
            args: ['--coop-host-relay', '${SERVER}', '--coop-players', '3'] },
          { id: 'host', label: 'Host with your IP (2 players)', icon: 'E774', more: true, description: 'Direct connect on port 7790', swf: swf('coop.swf'),
            args: ['--coop-host', '7790', '--coop-players', '2'] },
          { id: 'joinip', label: 'Join by IP', icon: 'E774', more: true, description: "Type the host's address", swf: swf('coop.swf'), args: ['--coop-join', '${ADDRESS}'] },
          { id: 'solo', label: 'Play solo', icon: 'E77B', offline: true, description: 'The campaign on your own', swf: swf('coop.swf') },
        ],
      },
      {
        id: 'relaygame', name: 'Relay Test', players: '1-2 players', maxPlayers: 2, categories: ['flash', 'online'],
        description: "Hosts with the launcher's built-in relay.",
        actions: [
          { id: 'play', label: 'Play', swf: swf('relay.swf') },
          { id: 'hostrelay', label: 'Host with the relay', description: 'Friends connect to this PC', swf: swf('relay.swf'),
            args: ['--tcp-connections', 'allow'], relay: { port: PORTS.relay } },
        ],
      },
      { id: 'dlgame', name: 'Download Test', players: '1-2 players', categories: ['flash'], description: 'Its file comes from the catalog.',
        actions: [{ id: 'play', label: 'Play', swf: swf('dl.swf') }] },
      { id: 'ruffleonly', name: 'Ruffle Test', players: '1-2 players', description: 'Only runs in Ruffle Together, which is not installed yet.',
        actions: [{ id: 'play', label: 'Play', swf: swf('coop.swf'), players: ['ruffle'] }] },
      { id: 'locgame', name: 'Locate Test', players: '1-2 players', categories: ['flash'], description: 'Its file is somewhere else on this PC.',
        actions: [{ id: 'play', label: 'Play', swf: swf('nowhere.swf') }] },
      {
        id: 'lob', name: 'Lobby Test', players: '1-4 players', maxPlayers: 4, categories: ['flash', 'online'], description: 'Plays through the lobby.',
        online: { id: 'lob', join: 'online', lobby: { version: '2.0', port: PORTS.lobby } },
        actions: [{ id: 'online', label: 'Play online', swf: swf('lob.swf'), flashvars: { online: '1' } }],
      },
      {
        id: 'modtest', name: 'Mod Test', players: '1-4 players', maxPlayers: 4, categories: ['pc', 'coop'], description: 'A PC mod in a fake game folder.',
        mod: {
          id: 'mod-test',
          game: { name: 'Fake Game', executable: 'Game.exe', processNames: ['FakeGame-not-running.exe'], detection: [{ type: 'path', path: 'C:\\no\\such\\folder' }] },
          install: { preserve: ['config/*.ini'], lockedFileRetries: 1 },
          components: [
            { id: 'core', name: 'Core', source: 'core', required: true },
            { id: 'override', name: 'Override', source: 'override', target: '${GAME_DIR}', required: true },
          ],
          package: { modsInfo: 'modtest' },
        },
      },
    ],
  }
  write(join(work, 'launcher.json'), JSON.stringify(config, null, 2))
  const siteGames = {
    games: [{ id: 'relaygame', name: 'Relay Test', kind: 'flash', status: 'available', quality: 'B', version: '3', updated: '2026-10-30',
      origin: 'community', reviewed: true, author: 'Some Modder', authorUrl: 'https://github.com/somemodder/relay-test',
      synced: { see: 'synced', story: 'partly' } }],
  }
  write(join(work, 'data', 'site.json'), JSON.stringify({ at: Math.floor(Date.now() / 1000), games: JSON.stringify(siteGames) }))
  // A name and no minimizing, so nothing waits on a prompt and the window stays where WebDriver can see it.
  write(join(work, 'data', 'settings.json'), JSON.stringify({ playerName: 'Tester', online: true, minimize: 0 }))
  return { config: join(work, 'launcher.json'), gameDir, args: join(work, 'args.txt'), settings: join(work, 'data', 'settings.json') }
}
