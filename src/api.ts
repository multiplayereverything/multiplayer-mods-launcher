// The launcher's data and actions. In the Tauri window they come from the C++ launcher exe (src-tauri/src/engine.rs);
// in a plain browser (npm run dev) the bundled sample stands in, so the UI can still be worked on.
import { convertFileSrc, invoke, isTauri } from '@tauri-apps/api/core'
import { listen, type UnlistenFn } from '@tauri-apps/api/event'
import sample from './launcher.data.json'

export interface Action {
  id: string
  label: string
  description: string
  hint: string  // what to do next, shown when the action starts
  icon: string
  more: boolean
  offline: boolean
  original: boolean
  link: string
  mod: string | null
  relayPort: number
  coopHost: boolean
  coopJoin: boolean
  joinsByCode: boolean
  hostPlayers: number
  viaRelay: boolean
  asksAddress: boolean
  state?: 'ready' | 'noPlayer' | 'noFile' | 'noRuntime'
  player?: string | null
  playerId?: string | null
  file?: string
}

export interface Game {
  id: string
  name: string
  description: string
  players: string
  maxPlayers: number
  color: string
  colorEnd: string
  image: string
  imageFocus: number
  onlineId: string
  joinAction: string
  comingSoon: boolean
  lobby: boolean
  categories: string[]
  mod?: { modName: string; publisher: string; website: string; gameName: string; pinnedVersion: string }
  guide: { needs: string[]; steps: string[]; tips: string[]; links: { label: string; url: string }[] }
  actions: Action[]
  // What the website's games.json says (grade A-D or X, version, what stays in sync), when it knows the game.
  site?: SiteInfo
  // The game's own options (checkboxes and choices that become command-line options) and, for Ruffle Together
  // co-op, the save the host starts from.
  settings: GameSetting[]
  coopSave?: { supported: boolean; since: string; progress: boolean }
  player: string  // the player picked for this game; empty = automatic
  outdated: boolean  // the game catalog has a newer build of its file
}

export interface GameSetting {
  id: string
  type: 'checkbox' | 'choice'
  scope: string
  label: string
  since: string
  enabled: boolean
  value: string
  choices: { id: string; label: string }[]
}

export interface SiteInfo {
  status: string
  quality: string
  version: string
  updated: string
  connect: string
  community: boolean
  reviewed: boolean
  author: string
  authorUrl: string
  repo: string
  maintainer: string
  synced: Record<string, 'synced' | 'partly' | 'unknown'>
}

export interface Category { id: string; name: string; icon: string; group: string }
export interface UserGame { favorite: boolean; plays: number; seconds: number; lastPlayed: number }

export interface Settings {
  playerName: string
  minimize: boolean
  online: boolean
  defaultPlayer: string
  gamesDir: string
  showOffline: boolean
  showOriginals: boolean
  autoUpdate: boolean
  compactCards: boolean  // smaller cards in the grid, as in the C++ window
}

export interface Library {
  branding: { title: string; subtitle: string; playerColors: string[] }
  dataDir: string
  configDir: string
  flashpoint: string
  gamesDir: string
  defaultGamesDir: string
  launcherVersion: string
  website: string
  logFile: string
  players: { id: string; name: string; found: boolean; installable: boolean; version: string; website: string; exe?: string }[]
  theme: string
  roomsServer: string
  discord: boolean
  categories: Category[]
  games: Game[]
  user: Settings & { games: Record<string, UserGame> }
  text: Record<string, string>
}

export interface ModInfo {
  id: string
  gameFound: boolean
  gameDir: string
  installed: boolean
  installedVersion: string
  running: boolean
  problem: string
  missing: string
  known: boolean
  published: boolean
  newest: string
  notice: string
  error: string
}

export interface Room { code: string; name: string; players: number; max: number; locked: boolean; state: string; region: string }
export interface Rooms { server: string; onServer: boolean; error: string; rooms: Room[] }

export const live = isTauri()

export async function loadLibrary(): Promise<Library> {
  if (!live) return sampleLibrary()
  const lib = await invoke<Library>('library')
  for (const g of lib.games) g.image = g.image ? convertFileSrc(g.image) : ''
  return lib
}

export async function loadMods(): Promise<Record<string, ModInfo>> {
  const list = live ? await invoke<ModInfo[]>('mods') : sampleMods()
  return Object.fromEntries(list.map((m) => [m.id, m]))
}

export const loadRooms = (game: string): Promise<Rooms> =>
  live ? invoke<Rooms>('rooms', { game }) : Promise.resolve({ server: 'sample', onServer: true, error: '', rooms: [] })

// Games started while TV mode is on start as from TV mode (--tv: the player's full-screen and controller options).
let tvMode = false
export const setTvMode = (on: boolean) => {
  tvMode = on
}

// Each time TV mode turned on or off and why (the last 20), for Copy diagnostics.
export const TV_LOG = 'fl.tvLog'
export function noteTv(on: boolean, cause: string) {
  try {
    const log: string[] = JSON.parse(localStorage.getItem(TV_LOG) ?? '[]')
    log.push(`${new Date().toISOString()} ${on ? 'on' : 'off'}: ${cause}`)
    localStorage.setItem(TV_LOG, JSON.stringify(log.slice(-20)))
  } catch {
    // Only a help for a bug report.
  }
}

export const launch = (game: string, action: string, code?: string, address?: string, coopSave?: string): Promise<number> =>
  live ? invoke<number>('launch', { game, action, code, address, coopSave, tv: tvMode }) : Promise.resolve(0)
export const setGameSetting = (game: string, id: string, value: string): Promise<void> =>
  live ? invoke('set_game_setting', { game, id, value }) : Promise.resolve()
export const setGamePlayer = (game: string, player: string): Promise<void> =>
  live ? invoke('set_game_player', { game, player }) : Promise.resolve()
export interface PlayerUpdate { id: string; checked: boolean; available: boolean; updated: boolean; version: string; installed: string; error: string }
// Whether a newer release exists (nothing is installed), then the update itself with `player-progress` events.
export const checkPlayers = (): Promise<{ players: PlayerUpdate[] }> =>
  live ? invoke('check_players') : Promise.resolve({ players: [] })
export const updatePlayers = (): Promise<{ players: PlayerUpdate[] }> =>
  live ? invoke('update_players') : Promise.resolve({ players: [] })
export interface PlayerProgress { id: string; done: number; total: number }
export interface AllRooms { server: string; games: Record<string, Rooms> }
export const loadAllRooms = (): Promise<AllRooms> => (live ? invoke('all_rooms') : Promise.resolve({ server: '', games: {} }))
export const ping = (server: string): Promise<number> => (live ? invoke('ping', { server }) : Promise.reject(new Error('sample')))

export const modJob = (op: 'install' | 'repair' | 'uninstall', game: string): Promise<void> =>
  live ? invoke('mod_job', { op, game }) : Promise.resolve()

export const setFavorite = (game: string, on: boolean): Promise<void> => (live ? invoke('set_favorite', { game, on }) : Promise.resolve())

export const open = (target: string): Promise<void> => (live ? invoke('open', { target }) : (window.open(target, '_blank'), Promise.resolve()))

export const stopSession = (session: number): Promise<void> => (live ? invoke('stop_session', { session }) : Promise.resolve())
export const downloadGame = (game: string): Promise<void> => (live ? invoke('download_game', { game }) : Promise.resolve())
export const setSetting = (key: keyof Settings, value: string | boolean): Promise<void> =>
  live ? invoke('set_setting', { key, value }) : Promise.resolve()
export const installPlayer = (player: string): Promise<void> => (live ? invoke('install_player', { player }) : Promise.resolve())
export const setGameDir = (game: string, dir: string): Promise<void> => (live ? invoke('set_game_dir', { game, dir }) : Promise.resolve())
export const setGameFile = (game: string, action: string, file: string): Promise<void> =>
  live ? invoke('set_game_file', { game, action, file }) : Promise.resolve()

// The test lab answers the next picker itself (tests/lab), since WebDriver can't reach Windows' own dialogs.
const labPick = (): string | null | undefined => (window as unknown as { __flLabPick?: string | null }).__flLabPick

// Folder and file pickers (the dialog plugin); null when the player cancels.
export async function pickFolder(title: string, start?: string): Promise<string | null> {
  if (labPick() !== undefined) return labPick() ?? null
  if (!live) return null
  const { open: openDialog } = await import('@tauri-apps/plugin-dialog')
  const r = await openDialog({ directory: true, title, defaultPath: start || undefined })
  return typeof r === 'string' ? r : null
}
export async function pickFile(title: string, extensions: string[]): Promise<string | null> {
  if (labPick() !== undefined) return labPick() ?? null
  if (!live) return null
  const { open: openDialog } = await import('@tauri-apps/plugin-dialog')
  const r = await openDialog({ title, filters: [{ name: 'Game file', extensions }] })
  return typeof r === 'string' ? r : null
}

// The system clipboard through the clipboard plugin: the WebView's own clipboard API is refused outside a click.
export async function copyText(text: string): Promise<void> {
  if (!live) return navigator.clipboard.writeText(text)
  const { writeText } = await import('@tauri-apps/plugin-clipboard-manager')
  await writeText(text)
}

export async function minimizeWindow(): Promise<void> {
  if (!live) return
  const { getCurrentWindow } = await import('@tauri-apps/api/window')
  await getCurrentWindow().minimize()
}

// Back from the taskbar once the last game started from here has closed, as the C++ window does.
export async function restoreWindow(): Promise<void> {
  if (!live) return
  const { getCurrentWindow } = await import('@tauri-apps/api/window')
  const w = getCurrentWindow()
  if (await w.isMinimized()) {
    await w.unminimize()
    await w.setFocus()
  }
}

// A Windows notification, only while the launcher isn't the window in front (a friend joined, say).
export async function notify(title: string, body: string): Promise<void> {
  if (!live || document.hasFocus()) return
  const n = await import('@tauri-apps/plugin-notification')
  if (!(await n.isPermissionGranted()) && (await n.requestPermission()) !== 'granted') return
  n.sendNotification({ title, body })
}

// The window draws its own title bar, so its three buttons call these.
export async function windowControl(what: 'minimize' | 'maximize' | 'close'): Promise<void> {
  if (!live) return
  const { getCurrentWindow } = await import('@tauri-apps/api/window')
  const w = getCurrentWindow()
  if (what === 'minimize') await w.minimize()
  else if (what === 'maximize') await w.toggleMaximize()
  else await w.close()
}

// TV mode fills the screen; leaving it gives the window back its size.
export async function setFullscreen(on: boolean): Promise<void> {
  if (!live) return
  const { getCurrentWindow } = await import('@tauri-apps/api/window')
  await getCurrentWindow().setFullscreen(on)
}

// The lobby: the C++ launcher runs the room (--lobby-session); the window sends commands and draws what it prints.
export type LobbyRequest =
  | { op: 'host'; direct: boolean; roomName: string; password: string; public: boolean; maxPlayers: number; port: number }
  | { op: 'join'; direct: boolean; code?: string; address?: string; password: string }
export type LobbyCommand = { op: 'ready'; v: boolean } | { op: 'start' } | { op: 'chat'; text: string } | { op: 'kick'; slot: number; ban: boolean } | { op: 'leave' }
export interface LobbyPlayer { slot: number; name: string; host: boolean; ready: boolean; connected: boolean; loaded: boolean; you: boolean; ping: number }
export interface LobbySnapshot {
  t: 'snapshot'
  phase: 'connecting' | 'lobby' | 'starting' | 'ingame' | 'closed'
  direct: boolean
  host: boolean
  reconnecting: boolean
  locked: boolean
  isPublic: boolean
  code: string
  roomName: string
  maxPlayers: number
  mySlot: number
  gameConnected: boolean
  reason: string
  players: LobbyPlayer[]
  chat: { slot: number; name: string; text: string }[]
}
export type LobbyMessage =
  | LobbySnapshot
  | { t: 'error'; message: string }
  | { t: 'game'; state: 'started' | 'failed' | 'exited'; message?: string }
  | { t: 'closed'; reason: string }
  | { t: 'ended' }

export const lobbyStart = (game: string, request: LobbyRequest): Promise<void> =>
  live ? invoke('lobby_start', { game, request, tv: tvMode }) : Promise.reject(new Error('The lobby needs the Tauri window'))
export const lobbySend = (command: LobbyCommand): Promise<void> => (live ? invoke('lobby_send', { command }) : Promise.resolve())

export interface LaunchEnded { session: number; game: string; code: number; message: string }
export interface ModProgress { game: string; line: string; done: boolean; ok: boolean }
export interface RelayStatus { port: number; addresses: { address: string; adapter: string }[]; peers: { name: string; address: string; host: boolean }[] }
export interface HostStatus { session: number; game: string; relay: RelayStatus }

const on = <T>(event: string, f: (e: T) => void): Promise<UnlistenFn> =>
  live ? listen<T>(event, (e) => f(e.payload)) : Promise.resolve(() => {})
export const onLaunchEnded = (f: (e: LaunchEnded) => void) => on('launch-ended', f)
export const onModProgress = (f: (e: ModProgress) => void) => on('mod-progress', f)
export const onPlayerProgress = (f: (e: PlayerProgress) => void) => on('player-progress', f)
export const onDownloadEnded = (f: (e: ModProgress) => void) => on('download-ended', f)
export const onHostStatus = (f: (e: HostStatus) => void) => on('host-status', f)
export const onLobby = (f: (e: { game: string; message: LobbyMessage }) => void) => on('lobby', f)

// ---- the sample, shaped like the real answer ----

type SampleGame = {
  id: string; name: string; players?: string; maxPlayers?: number; online?: { id: string }; categories?: string[]
  color?: string; colorEnd?: string; image?: string; imageFocus?: number; description?: string
  mod?: { publisher?: string; website?: string; gameName?: string; version?: string }
  actions: { id: string; label: string; description?: string; link?: string }[]
}
const SAMPLE_SOON = new Set(['baldi-together', 'miside-together', 'cupheads', 'rick-and-morty-together', 'dave-the-diver-multiplayer', 'gta-coop'])

function sampleLibrary(): Library {
  const s = sample as unknown as { branding: { title: string; subtitle: string }; categories: Category[]; games: SampleGame[] }
  const now = Date.now() / 1000
  const action = (a: Partial<Action> & { id: string; label: string }): Action => ({
    description: '', hint: '', icon: '', more: false, offline: false, original: false, link: '', mod: null, relayPort: 0,
    coopHost: false, coopJoin: false, joinsByCode: false, hostPlayers: 0, viaRelay: false, asksAddress: false, state: 'ready', ...a,
  })
  return {
    branding: { ...s.branding, playerColors: ['#E5484D', '#3E7BFA', '#2EBD6B', '#F5B83D', '#E5484D', '#3E7BFA'] },
    dataDir: '', configDir: '', flashpoint: '', gamesDir: '', defaultGamesDir: '', launcherVersion: 'sample', website: '', logFile: '',
    players: [{ id: 'ruffle', name: 'Ruffle Together', found: true, installable: true, version: '1.0.5', website: '' }],
    theme: 'dark', roomsServer: '', discord: false,
    categories: s.categories,
    games: s.games.map((g) => ({
      id: g.id, name: g.name, description: g.description ?? '', players: g.players ?? '', maxPlayers: g.maxPlayers ?? 2,
      color: g.color ?? '#3a3f4b', colorEnd: g.colorEnd ?? '#1a1d24', image: g.image ?? '', imageFocus: g.imageFocus ?? 0.5,
      onlineId: g.online?.id ?? '', joinAction: g.online ? 'online' : '', comingSoon: SAMPLE_SOON.has(g.id), lobby: false, categories: g.categories ?? [],
      mod: g.mod ? { modName: g.name, publisher: g.mod.publisher ?? '', website: g.mod.website ?? '', gameName: g.mod.gameName ?? g.name, pinnedVersion: '' } : undefined,
      guide: { needs: [], steps: [], tips: [], links: g.actions.filter((a) => a.link).map((a) => ({ label: a.label, url: a.link! })) },
      settings: [], player: '', outdated: false,
      actions: g.mod
        ? ['install', 'update', 'launch', 'repair', 'uninstall'].map((op) => action({ id: op, label: op, mod: op }))
        : g.online
          ? [
              action({ id: 'online', label: 'Play online', coopHost: true, coopJoin: true, joinsByCode: true, hostPlayers: 2, viaRelay: true }),
              action({ id: 'room4', label: 'Host a room for 4 players', more: true, coopHost: true, hostPlayers: 4, viaRelay: true }),
              action({ id: 'join', label: 'Join by IP', more: true, coopJoin: true, asksAddress: true }),
              action({ id: 'offline', label: 'Play offline', description: 'The game on your own', offline: true, icon: '\uE77B' }),
            ]
          : [action({ id: 'play', label: 'Play' })],
    })),
    user: {
      playerName: 'You', minimize: false, online: true, defaultPlayer: '', gamesDir: '', showOffline: true, showOriginals: false, autoUpdate: true, compactCards: false,
      games: { gunmayhem2: { favorite: false, plays: 3, seconds: 900, lastPlayed: now - 11 * 3600 } },
    },
    text: { 'status.ready': 'Ready', 'status.noSwf': 'Game file missing', 'status.noPlayer': 'No Flash player found', 'status.comingSoon': 'Coming soon' },
  }
}

function sampleMods(): ModInfo[] {
  const base = { gameFound: true, gameDir: 'C:\\Games', running: false, problem: '', missing: '', known: true, published: true, notice: '', error: '' }
  return [
    { ...base, id: 'spore-coop', installed: true, installedVersion: '0.7.3.1', newest: '0.7.4' },
    { ...base, id: 'little-nightmares-coop', installed: true, installedVersion: '0.7.1', newest: '0.7.1' },
    { ...base, id: 'spooky-together', installed: false, installedVersion: '', newest: '1.2.0' },
  ]
}
