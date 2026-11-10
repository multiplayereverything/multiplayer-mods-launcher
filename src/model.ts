// What the window shows about a game, worked out from the launcher's answer the way the C++ window does.
import type { Action, Game, Library, ModInfo } from './api'

export type StatusKind = 'ready' | 'installed' | 'update' | 'missing' | 'soon' | 'checking' | 'playing'
export interface Status { kind: StatusKind; text: string }

// "1.10.0" after "1.9.2": numbers compared as numbers.
export function newer(a: string, b: string): boolean {
  const x = a.split(/[.-]/).map((n) => parseInt(n, 10) || 0)
  const y = b.split(/[.-]/).map((n) => parseInt(n, 10) || 0)
  for (let i = 0; i < Math.max(x.length, y.length); i++) if ((x[i] ?? 0) !== (y[i] ?? 0)) return (x[i] ?? 0) > (y[i] ?? 0)
  return false
}

export function statusOf(g: Game, lib: Library, mods: Record<string, ModInfo> | null, playing: boolean): Status {
  const t = (k: string, fallback: string) => lib.text[k] ?? fallback
  if (g.comingSoon) return { kind: 'soon', text: t('status.comingSoon', 'Coming soon') }
  if (playing) return { kind: 'playing', text: 'Playing' }
  if (g.mod) {
    const m = mods?.[g.id]
    if (!m) return { kind: 'checking', text: 'Checking...' }
    if (!m.gameFound) return { kind: 'missing', text: `${g.mod.gameName || g.name} not found` }
    if (m.problem) return { kind: 'missing', text: m.problem }
    // This line sits on the card and above the slots, one line wide; a longer message is on the Install button.
    if (!m.installed) return { kind: 'missing', text: m.missing ? (m.missing.length > 40 ? "Can't install yet" : m.missing) : 'Not installed' }
    if (m.published && newer(m.newest, m.installedVersion)) return { kind: 'update', text: `Update ${m.newest} available` }
    return { kind: 'installed', text: `Installed ${m.installedVersion}` }
  }
  // A game played from its own page (only links here, as community mods are) is as ready as the C++ card says.
  if (g.actions.length > 0 && g.actions.every((a) => a.link)) return { kind: 'ready', text: t('status.ready', 'Ready') }
  const states = g.actions.filter((a) => a.state).map((a) => a.state)
  if (states.includes('ready') && g.outdated) return { kind: 'update', text: t('status.update', 'Update available') }
  if (states.includes('ready')) return { kind: 'ready', text: t('status.ready', 'Ready') }
  if (states.includes('oldPlayer')) return { kind: 'missing', text: t('status.oldPlayer', 'Needs a newer Ruffle Together').replace('${PLAYER}', g.actions.find((a) => a.state === 'oldPlayer')?.player ?? 'Ruffle Together').replace('${VERSION}', g.actions.find((a) => a.state === 'oldPlayer')?.needs ?? '') }
  if (states.includes('noPlayer')) return { kind: 'missing', text: t('status.noPlayer', 'No Flash player found') }
  return { kind: 'missing', text: t('status.noSwf', 'Game file missing') }
}

export function lastPlayed(lib: Library, id: string): number {
  return lib.user.games[id]?.lastPlayed ?? 0
}

export function agoText(unix: number): string {
  const s = Math.max(0, Date.now() / 1000 - unix)
  if (s < 3600) return `${Math.max(1, Math.round(s / 60))} min ago`
  if (s < 86400) return `${Math.round(s / 3600)} h ago`
  const d = Math.round(s / 86400)
  return d === 1 ? 'yesterday' : `${d} days ago`
}

export function playedText(lib: Library, id: string): string {
  const u = lib.user.games[id]
  if (!u || !u.plays) return 'Not played yet'
  const mins = Math.round(u.seconds / 60)
  return `Played ${u.plays} time${u.plays === 1 ? '' : 's'}${mins ? ` for ${mins} min in total` : ''}, last ${agoText(u.lastPlayed)}.`
}

const plain = (a: Action) => !a.mod && !a.link

// Ruffle Together co-op: the game has both a host and a join action, so Host a game and Join a game stand in for them.
export const isCoop = (g: Game) => g.actions.some((a) => a.coopHost) && g.actions.some((a) => a.coopJoin)

// TV mode: online games pick how to play first (Quick play, host, join with a code); the rest play at once.
export const needsPlayMenu = (g: Game) => !g.mod && !g.comingSoon && (!!g.onlineId || isCoop(g) || g.lobby)

// Settings decide two kinds of action: solo play (show offline) and the unmodded game (show originals).
export interface Shown { showOffline: boolean; showOriginals: boolean }
const visible = (a: Action, s: Shown) => (!a.offline || s.showOffline) && (!a.original || s.showOriginals)

// The buttons besides Host and Join: what isn't hosting, joining or folded under "more".
export function mainActions(g: Game, s: Shown): Action[] {
  const coop = isCoop(g)
  return g.actions.filter((a) => plain(a) && !a.more && visible(a, s) && !(coop && (a.coopHost || a.coopJoin)))
}

// "Other ways to connect": direct IP and LAN, folded away until asked for.
export const moreActions = (g: Game, s: Shown): Action[] => g.actions.filter((a) => plain(a) && a.more && visible(a, s))

export interface HostChoice { action: Action; players: number; relay: boolean }
export const hostChoices = (g: Game): HostChoice[] =>
  g.actions.filter((a) => a.coopHost && a.hostPlayers > 0).map((a) => ({ action: a, players: a.hostPlayers, relay: a.viaRelay }))

export const joinByCode = (g: Game): Action | undefined =>
  g.actions.find((a) => a.id === g.joinAction && a.joinsByCode) ?? g.actions.find((a) => a.joinsByCode)
export const joinByAddress = (g: Game): Action | undefined => g.actions.find((a) => a.asksAddress)

// A friend's address looks like 1.2.3.4:7790 or a host name with a port; a room code is five letters and digits.
export const looksLikeAddress = (s: string) => /[.:]/.test(s.trim())

// What the banner and a card's Play start: the game's first plain action (PC mods have none here).
export function leadAction(g: Game): Action | undefined {
  if (g.mod || g.comingSoon) return undefined
  return g.actions.find((a) => plain(a) && !a.more && !a.original && !a.offline) ?? g.actions.find((a) => plain(a) && !a.more && !a.original)
}

// The action a game's file is judged by, for Locate (the join action of an online game, else the first plain one).
export const fileAction = (g: Game): Action | undefined =>
  g.actions.find((a) => a.id === g.joinAction && plain(a)) ?? g.actions.find((a) => plain(a) && !a.original)

// What the C++ rooms list says after a room's name: whether its game has started and whether it has a password.
export function roomAbout(r: { state: string; locked: boolean; region?: string }, text: Record<string, string>): string {
  return [r.region, r.state === 'ingame' ? text['rooms.playing'] ?? 'playing' : '', r.locked ? text['rooms.locked'] ?? 'password' : '']
    .filter(Boolean).join(', ')
}

// "2-4 players (one keyboard)" without the note in brackets, for tight places.
export const playersShort = (g: Game) => g.players.replace(/ \(.*\)$/, '')

export const platformBadge =(g: Game) => (g.mod ? 'PC' : g.categories.includes('flash') ? 'Flash' : '')

// A missing game file or Flash player that the launcher fetches itself when the player presses Play: not a problem to
// show in amber on every card. A player that can't be installed for them still is.
export function sortsItself(g: Game, lib: Library): boolean {
  if (g.mod || g.comingSoon) return false
  const states = g.actions.filter((a) => a.state).map((a) => a.state)
  if (states.length === 0 || states.includes('ready') || states.includes('noRuntime')) return false
  return !states.includes('noPlayer') || !!lib.players?.some((p) => p.installable)
}

// A mod that simply isn't installed yet is the normal state before pressing Install.
const notInstalledYet = (g: Game, mods: Record<string, ModInfo> | null) => {
  const m = g.mod ? mods?.[g.id] : undefined
  return !!m && m.gameFound && !m.problem && !m.missing && !m.installed
}

// The state in words and its color, as the C++ card's stateText. Amber is kept for what the player has to fix.
export function stateLine(game: Game, lib: Library, mods: Record<string, ModInfo> | null, playing: boolean): { text: string; tone: string } {
  const st = statusOf(game, lib, mods, playing)
  const quiet = st.kind === 'missing' && (sortsItself(game, lib) || notInstalledYet(game, mods))
  const tone = st.kind === 'ready' || st.kind === 'installed' || st.kind === 'update' ? 'ok' : st.kind === 'playing' ? 'online' : st.kind === 'soon' || st.kind === 'checking' || quiet ? 'muted' : 'warn'
  return { text: st.text, tone }
}

// A card's last line: empty when there's nothing the player needs to know yet.
export function cardLine(game: Game, lib: Library, mods: Record<string, ModInfo> | null, playing: boolean): { text: string; tone: string } {
  const line = stateLine(game, lib, mods, playing)
  return !playing && sortsItself(game, lib) ? { text: '', tone: 'muted' } : line
}

// The pill on a game's page; the list under it says what is left.
export function panelLine(game: Game, lib: Library, mods: Record<string, ModInfo> | null, playing: boolean): { text: string; tone: string } {
  const line = stateLine(game, lib, mods, playing)
  return !playing && sortsItself(game, lib) ? { text: lib.text['status.almostReady'] ?? 'Almost ready', tone: 'muted' } : line
}

// A newer build of a Flash game's file is news even before its player is installed (the state line then speaks of
// the player); a mod's newer release only once it is installed.
export const hasUpdate = (game: Game, lib: Library, mods: Record<string, ModInfo> | null) =>
  game.mod ? statusOf(game, lib, mods, false).kind === 'update' : !game.comingSoon && game.outdated

export interface ReadyStep { label: string; note: string; state: 'ok' | 'todo' | 'stop' }

// The game page's "what does this need" list: one line for each thing, with what the launcher will do about it.
export function readySteps(g: Game, lib: Library, mods: Record<string, ModInfo> | null): ReadyStep[] {
  const t = (k: string, fallback: string, vars: Record<string, string> = {}) =>
    Object.entries(vars).reduce((x, [key, v]) => x.replace('${' + key + '}', v), lib.text[k] ?? fallback)
  if (g.comingSoon) return []
  if (g.mod) {
    const m = mods?.[g.id]
    if (!m) return []
    const name = g.mod.gameName || g.name
    const steps: ReadyStep[] = [{
      label: t('ready.copy', 'Your copy of ${GAME}', { GAME: name }),
      note: m.gameFound ? t('ready.found', 'Found on this PC') : t('ready.notFound', 'Not found: choose its folder'),
      state: m.gameFound ? 'ok' : 'stop',
    }]
    if (m.gameFound && !m.installed && (m.missing || m.problem)) steps.push({ label: t('ready.needs', 'What the mod needs'), note: m.missing || m.problem, state: 'stop' })
    if (m.installed) {
      const update = m.published && newer(m.newest, m.installedVersion)
      steps.push({
        label: g.mod.modName || g.name,
        note: update ? t('ready.modUpdate', 'Version ${VERSION} is out', { VERSION: m.newest }) : t('ready.version', 'Version ${VERSION}', { VERSION: m.installedVersion }),
        state: update ? 'todo' : 'ok',
      })
    } else if (m.gameFound) {
      steps.push({ label: g.mod.modName || g.name, note: t('ready.notInstalled', 'Not installed yet'), state: 'todo' })
    }
    return steps
  }
  const a = fileAction(g)
  if (!a?.state) return []
  if (a.state === 'noRuntime') return [{ label: t('ready.runtime', 'Server program'), note: t('ready.runtimeMissing', 'Missing from this PC'), state: 'stop' }]
  const playerName = a.player ?? lib.players?.find((p) => p.installable)?.name ?? t('ready.player', 'Flash player')
  // With the file missing the action still names the player it would use, found or not: ask the list.
  const named = lib.players?.find((p) => p.id === a.playerId)
  const noPlayer = a.state === 'noPlayer' || (a.state === 'noFile' && !(named ? named.found : lib.players?.some((p) => p.found)))
  const installable = !!lib.players?.some((p) => p.installable)
  return [
    {
      label: t('ready.file', 'Game file'),
      note: a.state === 'noFile' ? t('ready.fileMissing', 'Not downloaded yet') : g.outdated ? t('ready.fileNewer', 'A newer build can be downloaded') : t('ready.fileOk', 'On this PC'),
      state: a.state === 'noFile' || g.outdated ? 'todo' : 'ok',
    },
    {
      label: playerName,
      note: a.state === 'oldPlayer' ? t('ready.playerOld', 'Needs ${VERSION} or newer: updates when you play', { VERSION: a.needs ?? '' })
        : noPlayer ? (installable ? t('ready.playerInstalls', 'Installs when you play') : t('ready.playerMissing', 'Not found on this PC'))
        : t('ready.playerOk', 'Installed'),
      state: a.state === 'oldPlayer' || (noPlayer && installable) ? 'todo' : noPlayer ? 'stop' : 'ok',
    },
  ]
}
