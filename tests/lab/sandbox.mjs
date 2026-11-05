// The lab's sandbox: its own launcher.json, data folder, fake games, a fake PC game folder and the files the local
// servers hand out (catalog, mods-info, the mod package). Everything lives under one temp folder; nothing outside it
// is read or written, so a lab run never touches the real launcher's settings or a real game.
import { createHash, randomBytes } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { copyFileSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
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

  // A community mod the owner approved into mods-info (community/<id>.json), its release on a stand-in for github.com,
  // and the fake game it goes into. The website lists it too (/api/projects), with a second project that has no record.
  const labGame = join(work, 'LabGame')
  write(join(labGame, 'LabGame.exe'), 'lab game binary')
  const labPkg = join(work, 'labpkg')
  // The mod is a folder inside the zip, as the website's form allows (source "release\\LabCoop"), and it needs BepInEx.
  write(join(labGame, 'BepInEx', 'core', 'BepInEx.dll'), 'bepinex')
  write(join(labPkg, 'release', 'LabCoop', 'LabCoop.dll'), 'lab coop 1.2.0')
  write(join(labPkg, 'release', 'LabCoop', 'lang', 'en.txt'), 'hello')
  const labZip = join(work, 'www', 'someone', 'lab-coop', 'releases', 'download', 'v1.2.0', 'LabCoop-1.2.0.zip')
  mkdirSync(join(labZip, '..'), { recursive: true })
  execFileSync('powershell', ['-NoProfile', '-Command', `Compress-Archive -Path '${labPkg}\\*' -DestinationPath '${labZip}'`])
  write(join(work, 'www', 'community', 'index.json'), JSON.stringify({ mods: ['lab-coop', 'lab-link', 'lab-solo', 'lab-devflash', 'lab-newflash'] }))
  // A developer's Flash game (1.0.43): the game file and its key map pinned from their release, the buttons made by the
  // launcher. And one that needs a Ruffle Together newer than any there is.
  const devRel = join(work, 'www', 'someone', 'lab-devflash', 'releases', 'download', 'v1.0')
  write(join(devRel, 'LabDevFlash.swf'), 'dev flash game 1.0')
  write(join(devRel, 'lab-devflash.keys'), 'slot 0: UP=38\nslot 1: UP=87\n')
  const pin = (name) => ({ asset: name, url: `http://127.0.0.1:${PORTS.http}/someone/lab-devflash/releases/download/v1.0/${name}`,
    sha256: sha256(join(devRel, name)), size: readFileSync(join(devRel, name)).length })
  const devFlash = (id, game, ruffle, keys) => ({
    id, repo: 'someone/lab-devflash', kind: 'flash', minLauncher: '1.0.43', tag: 'v1.0', version: '1.0', author: 'someone',
    approved_at: '2026-11-04', approved_by: 'lab', description: 'A developer\'s Flash game for two.',
    flash: { game, file: pin('LabDevFlash.swf'), keys, maxPlayers: 2, offline: {}, ruffle },
  })
  write(join(work, 'www', 'community', 'lab-devflash.json'), JSON.stringify(devFlash('lab-devflash', 'Lab Dev Flash', '1.0.0', pin('lab-devflash.keys'))))
  write(join(work, 'www', 'community', 'lab-newflash.json'), JSON.stringify(devFlash('lab-newflash', 'Lab New Flash', '99.0', 'gun-mayhem-2')))
  // A card the team wrote by hand that only links to the mod's page (its own installer), with the card's own text.
  write(join(work, 'www', 'community', 'lab-link.json'), JSON.stringify({
    id: 'lab-link', kind: 'link', repo: 'someone/lab-link', author: 'someone', approved_at: '2026-11-04', approved_by: 'lab',
    link: { game: 'Lab Link', url: 'https://github.com/someone/lab-link/releases' },
    description: 'A mod with an installer of its own.', players: '2-16 players (alpha)', maxPlayers: 4,
    guide: { steps: ['Download the zip from the mod\'s page.'], tips: ['It is an alpha.'] },
  }))
  write(join(work, 'www', 'community', 'lab-coop.json'), JSON.stringify({
    id: 'lab-coop', repo: 'someone/lab-coop', tag: 'v1.2.0', version: '1.2.0', asset: 'LabCoop-1.2.0.zip',
    url: `http://127.0.0.1:${PORTS.http}/someone/lab-coop/releases/download/v1.2.0/LabCoop-1.2.0.zip`, sha256: sha256(labZip),
    author: 'someone', approved_at: '2026-11-03', approved_by: 'lab',
    mod: {
      id: 'lab-coop',
      game: { name: 'Lab Game', executable: 'LabGame.exe', validationFiles: ['${GAME_EXE}'], processNames: ['LabGame-not-running.exe'],
        detection: [{ type: 'path', path: labGame }] },
      install: { targetDir: '${GAME_DIR}\\BepInEx\\plugins\\LabCoop', requires: [{ path: 'BepInEx\\core', message: 'Install BepInEx 5 first' }] },
      components: [{ id: 'mod', name: 'LabCoop', source: 'release\\LabCoop', target: '${INSTALL_DIR}', required: true }],
      launch: { uri: 'steam://rungameid/123456', hint: 'Host from the pause menu.' },
    },
  }))
  // A mod released as one DLL, not a zip, with no manifest on the release: the record the website's form writes.
  const labSolo = join(work, 'LabSolo')
  write(join(labSolo, 'LabSolo.exe'), 'lab solo binary')
  const soloDll = join(work, 'www', 'someone', 'lab-solo', 'releases', 'download', 'v0.3.0', 'LabSolo.dll')
  write(soloDll, 'lab solo 0.3.0')
  write(join(work, 'www', 'community', 'lab-solo.json'), JSON.stringify({
    id: 'lab-solo', repo: 'someone/lab-solo', tag: 'v0.3.0', version: '0.3.0', asset: 'LabSolo.dll',
    url: `http://127.0.0.1:${PORTS.http}/someone/lab-solo/releases/download/v0.3.0/LabSolo.dll`, sha256: sha256(soloDll),
    author: 'someone', approved_at: '2026-11-03', approved_by: 'lab',
    mod: {
      id: 'lab-solo', publisher: 'someone', website: 'https://github.com/someone/lab-solo',
      game: { name: 'Lab Solo', executable: 'LabSolo.exe', steamAppId: '654321', validationFiles: ['${GAME_EXE}'], processNames: ['LabSolo-not-running.exe'],
        detection: [{ type: 'path', path: labSolo }] },
      install: { targetDir: '${GAME_DIR}\\Mods' },
      components: [{ id: 'mod', name: 'Lab Solo', source: 'LabSolo.dll', target: '${INSTALL_DIR}', required: true }],
      launch: { uri: 'steam://rungameid/${STEAM_APP_ID}' },
    },
  }))
  // One of the team's own mods (team/<id>.json): a launcher.json games[] entry from a multiplayereverything repository.
  // It follows the repository's newest release (the stand-in for GitHub's API below) and brings BepInEx from the
  // repository's loader release, left out when the game has it already, and removed with the mod.
  const labTeam = join(work, 'LabTeam')
  // A copy of the fake player under another name notes that it ran, so the lab can check what a record's Launch starts.
  mkdirSync(labTeam, { recursive: true })
  copyFileSync(fakePlayer, join(labTeam, 'LabTeam.exe'))
  const teamPkg = join(work, 'teampkg')
  write(join(teamPkg, 'LabTeam', 'LabTeam.dll'), 'lab team 2.0.0')
  const teamZip = join(work, 'www', 'dl', 'LabTeam-2.0.0.zip')
  mkdirSync(join(teamZip, '..'), { recursive: true })
  execFileSync('powershell', ['-NoProfile', '-Command', `Compress-Archive -Path '${teamPkg}\\*' -DestinationPath '${teamZip}'`])
  const loaderPkg = join(work, 'loaderpkg')
  write(join(loaderPkg, 'BepInEx', 'core', 'BepInEx.dll'), 'bepinex 5')
  write(join(loaderPkg, 'winhttp.dll'), 'doorstop')
  const loaderZip = join(work, 'www', 'dl', 'BepInEx_lab.zip')
  execFileSync('powershell', ['-NoProfile', '-Command', `Compress-Archive -Path '${loaderPkg}\\*' -DestinationPath '${loaderZip}'`])
  const teamApi = join(work, 'www', 'repos', 'multiplayereverything', 'lab-team', 'releases')
  const asset = (file, name) => ({ name, size: readFileSync(file).length, browser_download_url: `http://127.0.0.1:${PORTS.http}/dl/${name}`, digest: `sha256:${sha256(file)}` })
  // GitHub answers both /releases (the list) and /releases/tags/<tag>; a folder and a file can't share a name, so the
  // list is releases.json, which the lab's server gives for /releases (servers.mjs).
  write(join(teamApi, 'tags', 'bepinex-5'), JSON.stringify({ tag_name: 'bepinex-5', draft: false, prerelease: false, assets: [asset(loaderZip, 'BepInEx_lab.zip')] }))
  write(join(work, 'www', 'repos', 'multiplayereverything', 'lab-team', 'releases.json'), JSON.stringify([
    { tag_name: 'v2.0.0', draft: false, prerelease: false, assets: [asset(teamZip, 'LabTeam-2.0.0.zip')] },
    { tag_name: 'bepinex-5', draft: false, prerelease: false, assets: [asset(loaderZip, 'BepInEx_lab.zip')] },
  ]))
  write(join(work, 'www', 'team', 'index.json'), JSON.stringify({ mods: ['lab-team', 'lab-next', 'lab-flash', 'lab-badflash'] }))
  // One of our Flash games as a team record: buttons that start launcher.json's player with a game file.
  write(join(work, 'games', 'flashteam.swf'), 'not a real swf')
  write(join(work, 'www', 'team', 'lab-flash.json'), JSON.stringify({
    id: 'lab-flash', repo: 'multiplayereverything/flash-games', minLauncher: '1.0.42', approved_at: '2026-11-04', approved_by: 'lab',
    name: 'Flash Team', players: '1-2 players', maxPlayers: 2, categories: ['flash'], description: 'One of our Flash games, from mods-info.',
    actions: [{ id: 'play', label: 'Play', swf: ['${GAMES_DIR}\\flashteam.swf'], args: ['--from-record'] }],
  }))
  // And one whose button would run a program of its own: refused whole, so it never shows.
  write(join(work, 'www', 'team', 'lab-badflash.json'), JSON.stringify({
    id: 'lab-badflash', repo: 'multiplayereverything/flash-games', name: 'Bad Flash', categories: ['flash'],
    actions: [{ id: 'play', label: 'Play', swf: ['${GAMES_DIR}\\flashteam.swf'], exe: ['cmd.exe'] }],
  }))
  write(join(work, 'www', 'team', 'lab-team.json'), JSON.stringify({
    id: 'lab-team', repo: 'multiplayereverything/lab-team', minLauncher: '1.0.42', approved_at: '2026-11-04', approved_by: 'lab',
    name: 'Lab Team', players: '2-4 players (co-op)', maxPlayers: 4, categories: ['pc', 'coop'], color: '#D6264F', colorEnd: '#2A0A16',
    description: 'One of our own mods, from mods-info.', guide: { steps: ['Press Install.'], tips: ['Only the host starts a run.'] },
    mod: {
      id: 'lab-team',
      game: { name: 'Lab Team', executable: 'LabTeam.exe', validationFiles: ['${GAME_EXE}'], processNames: ['LabTeam-not-running.exe'],
        detection: [{ type: 'path', path: labTeam }] },
      install: { targetDir: '${GAME_DIR}\\BepInEx\\plugins\\LabTeam', uninstallCleanupWith: 'loader', uninstallCleanup: ['BepInEx/cache/**'] },
      components: [
        { id: 'loader', name: 'BepInEx mod loader', source: 'loader/BepInEx/core', target: '${GAME_DIR}\\BepInEx\\core', skipIfPresent: ['BepInEx/core'], required: true },
        { id: 'loader-winhttp', name: 'BepInEx mod loader', source: 'loader/winhttp.dll', target: '${GAME_DIR}', skipIfPresent: ['BepInEx/core'], required: true },
        { id: 'mod', name: 'Lab Team', source: 'LabTeam', required: true },
      ],
      // A program in the game folder, as gta-coop's record has, not a Steam address the lab couldn't check.
      launch: { executable: '${GAME_DIR}\\LabTeam.exe', arguments: '--from-team-record' },
      package: { github: 'multiplayereverything/lab-team', asset: 'LabTeam-*.zip',
        extras: [{ dir: 'loader', github: 'multiplayereverything/lab-team', tag: 'bepinex-5', asset: 'BepInEx_lab.zip' }] },
    },
  }))
  // A record for a launcher that doesn't exist yet: this one shows a card asking for an update and reads no further.
  write(join(work, 'www', 'team', 'lab-next.json'), JSON.stringify({
    id: 'lab-next', repo: 'multiplayereverything/lab-next', minLauncher: '999.0', name: 'Lab Next', categories: ['pc'],
    mod: { id: 'lab-next', somethingNew: { x: 1 } },
  }))
  // records/: the same mods in one folder, as the dashboard and scripts/records-migrate.py write them. team/ and
  // community/ stay beside it for older launchers.
  const recordIds = []
  for (const folder of ['team', 'community']) {
    for (const id of JSON.parse(readFileSync(join(work, 'www', folder, 'index.json'), 'utf8')).mods) {
      const r = JSON.parse(readFileSync(join(work, 'www', folder, `${id}.json`), 'utf8'))
      const flash = folder === 'team' ? (r.categories ?? []).includes('flash') : r.kind === 'flash'
      const author = folder === 'team' ? 'Multiplayer Everything' : (r.author || r.repo.split('/')[0])
      write(join(work, 'www', 'records', `${id}.json`), JSON.stringify({ ...r, platform: flash ? 'flash' : 'pc', author }))
      recordIds.push(id)
    }
  }
  write(join(work, 'www', 'records', 'index.json'), JSON.stringify({ mods: recordIds }))
  write(join(work, 'www', 'api', 'projects'), JSON.stringify({ games: [
    { id: 'lab-coop', kind: 'pc', name: 'Lab Game', maintainer: 'someone', repo: 'someone/lab-coop', players: '1-4 players', description: 'Four players in one lab.' },
    { id: 'listed-only', kind: 'pc', name: 'Listed Only', maintainer: 'another', repo: 'another/listed-only', description: 'On the website, not in mods-info.' },
  ], attach: {} }))

  const swf = (name) => [`\${GAMES_DIR}\\${name}`]
  const config = {
    branding: { title: 'Multiplayer Everything', subtitle: 'Test lab' },
    paths: { games: '${LAUNCHER_DIR}\\games', players: '${LAUNCHER_DIR}\\players', data: '${LAUNCHER_DIR}\\data', flashpoint: [] },
    downloads: { catalog: `http://127.0.0.1:${PORTS.http}/catalog.json`, modsInfo: `http://127.0.0.1:${PORTS.http}/`, site: `http://127.0.0.1:${PORTS.http}/games.json` },
    rooms: { server: `127.0.0.1:${PORTS.rooms}`, refreshSeconds: 10 },
    // The mod engine asks this stand-in for GitHub's API about mods that follow their newest release (team records).
    updates: { github: 'multiplayereverything/lab-launcher', asset: 'MultiplayerEverythingLauncher.exe', api: `http://127.0.0.1:${PORTS.http}` },
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
  write(join(work, 'www', 'games.json'), JSON.stringify(siteGames))
  // A name and no minimizing, so nothing waits on a prompt and the window stays where WebDriver can see it.
  write(join(work, 'data', 'settings.json'), JSON.stringify({ playerName: 'Tester', online: true, minimize: 0 }))
  return { config: join(work, 'launcher.json'), gameDir, labGame, labSolo, labTeam, args: join(work, 'args.txt'), settings: join(work, 'data', 'settings.json') }
}
