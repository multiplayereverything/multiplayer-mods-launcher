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
    if (!m.installed) return { kind: 'missing', text: m.missing || 'Not installed' }
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

// What the card's last line says and in which color, as the C++ card's stateText.
export function stateLine(game: Game, lib: Library, mods: Record<string, ModInfo> | null, playing: boolean): { text: string; tone: string } {
  const st = statusOf(game, lib, mods, playing)
  const tone = st.kind === 'ready' || st.kind === 'installed' ? 'ok' : st.kind === 'playing' ? 'online' : st.kind === 'soon' || st.kind === 'checking' ? 'muted' : 'warn'
  return { text: st.text, tone }
}
