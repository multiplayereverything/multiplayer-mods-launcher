// TV mode: the launcher on a TV, for a controller on the couch. The whole screen; on Home the game you're on fills
// the top with its player slots and Play, your games run in one row along the bottom, and LB and RB move between
// Home, Games, Rooms and Settings. A game's page, the lobby and Settings are the desktop ones, drawn larger.
import { useEffect, useRef, useState, type ReactNode, type RefObject } from 'react'
import { Cover } from './Cover'
import type { Game, Library, ModInfo, Rooms } from './api'
import { agoText, lastPlayed, leadAction, roomAbout, stateLine } from './model'
import { Glyph, Pips, Slots } from './ui'
import { useDecode } from './useDecode'

export type TvSection = 'home' | 'library' | 'rooms' | 'settings'
export const TV_SECTIONS: { id: TvSection; name: string }[] = [
  { id: 'home', name: 'Home' },
  { id: 'library', name: 'Games' },
  { id: 'rooms', name: 'Rooms' },
  { id: 'settings', name: 'Settings' },
]

export interface TvCommon {
  lib: Library
  mods: Record<string, ModInfo> | null
  favs: Set<string>
  playingIds: Set<string>
  allRooms: Record<string, Rooms> | null
  pad: boolean
  onOpen: (g: Game) => void
  onPlay: (g: Game) => void
}

// A controller button as a round chip, shown only while a controller is connected.
export function PadKey({ k, pad }: { k: string; pad: boolean }) {
  return pad ? <kbd className="pk">{k}</kbd> : null
}

// A game in a row or the grid. A (or Enter) plays it when it is ready, otherwise opens its page (to download or
// install). A mouse is pointing, not committing: a click on Home's row picks the game for the top (a double-click
// plays it), a click in the grid opens its page.
function TvTile({ game, c, onFocus }: { game: Game; c: TvCommon; onFocus?: () => void }) {
  const line = stateLine(game, c.lib, c.mods, c.playingIds.has(game.id))
  const ready = leadAction(game)?.state === 'ready'
  const commit = () => (ready ? c.onPlay(game) : c.onOpen(game))
  const rooms = c.allRooms?.[game.id]?.rooms.length ?? 0
  const t = (k: string, f: string) => c.lib.text[k] ?? f
  return (
    <button className="tv-tile" data-game={game.id} onFocus={onFocus}
      // detail is 0 for Enter, Space and the controller's A (a click() with no mouse).
      onClick={(e) => (e.detail === 0 ? commit() : onFocus ? undefined : c.onOpen(game))}
      onDoubleClick={() => onFocus && commit()}
      aria-label={`${game.name}, ${line.text}${rooms ? `, ${rooms} open` : ''}`}>
      <span className="art">
        <Cover game={game} className="fill" />
        <span className="badges">
          {game.onlineId && <span className="badge">{t('badge.online', 'Online')}</span>}
          {game.mod && <span className="badge">{t('badge.pc', 'PC')}</span>}
        </span>
        {c.favs.has(game.id) && <span className="badge star">{'★'}</span>}
      </span>
      <span className="name">{game.name}</span>
      <span className="st">
        <span className={`tone-${line.tone}`}>{line.text}</span>
        {rooms > 0 && <span className="tone-online">{rooms === 1 ? '1 open' : `${rooms} open`}</span>}
      </span>
    </button>
  )
}

// The first game in a list gets the focus when a section opens, so the controller has somewhere to start.
function useFirstFocus(box: RefObject<HTMLElement | null>, key: number = 0) {
  useEffect(() => {
    const b = box.current
    if (!b || b.contains(document.activeElement)) return
    ;(b.querySelector<HTMLElement>('.tv-tile') ?? b.querySelector<HTMLElement>('button:not(:disabled)'))?.focus({ preventScroll: true })
  }, [box, key])
}

// The selected game's name decodes as the selection moves, as the C++ window's title does; readers get the name.
function HeroTitle({ name }: { name: string }) {
  const shown = useDecode(name)
  return <h1 aria-label={name}><span aria-hidden="true">{shown}</span></h1>
}

export function TvHome({ c, games }: { c: TvCommon; games: Game[] }) {
  const [id, setId] = useState(games[0]?.id ?? '')
  const row = useRef<HTMLDivElement>(null)
  useFirstFocus(row)
  const g = games.find((x) => x.id === id) ?? games[0]
  if (!g) {
    return (
      <div className="tv-empty">
        <Glyph code="E7FC" size={48} />
        <h2>No games yet</h2>
      </div>
    )
  }
  const lead = leadAction(g)
  const when = lastPlayed(c.lib, g.id)
  const line = stateLine(g, c.lib, c.mods, c.playingIds.has(g.id))
  const rooms = c.allRooms?.[g.id]?.rooms.length ?? 0
  return (
    <div className="tv-home">
      <section className="tv-hero" data-game={g.id} key={g.id}>
        <div className="tv-hero-text">
          <span className="tv-kick">{when ? `Played ${agoText(when)}` : g.mod ? 'PC game mod' : g.onlineId ? 'Play online' : 'Play together'}</span>
          <HeroTitle name={g.name} />
          <span className="tv-based">
            <Pips n={g.maxPlayers} colors={c.lib.branding.playerColors} />
            <span>{g.mod ? <>Mod for <b>{g.mod.gameName || g.name}</b></> : <b>{g.players}</b>}</span>
          </span>
          <Slots n={g.maxPlayers} colors={c.lib.branding.playerColors} />
          <span className="tv-state">
            <span className={`tone-${line.tone}`}>{line.text}</span>
            {rooms > 0 && <span className="tone-online">{rooms === 1 ? '1 room open' : `${rooms} rooms open`}</span>}
          </span>
          <p>{g.description}</p>
          <div className="acts">
            {lead?.state === 'ready' && <button className="btn main big" onClick={() => c.onPlay(g)}><PadKey k="A" pad={c.pad} />{lead.label}</button>}
            <button className="btn big" onClick={() => c.onOpen(g)}><PadKey k="Y" pad={c.pad} />Game page</button>
          </div>
        </div>
        <div className="tv-hero-art"><Cover game={g} className="fill" tall /></div>
      </section>
      <h2 className="tv-row-h">Your games</h2>
      <div className="tv-row" ref={row}>
        {games.map((x) => <TvTile key={x.id} game={x} c={c} onFocus={() => setId(x.id)} />)}
      </div>
    </div>
  )
}

export function TvLibrary({ c, cats, inCat }: { c: TvCommon; cats: { id: string; name: string }[]; inCat: (g: Game, id: string) => boolean }) {
  const [cat, setCat] = useState('all')
  const box = useRef<HTMLDivElement>(null)
  useFirstFocus(box)
  const all = [{ id: 'all', name: 'All games' }, ...cats]
  const list = c.lib.games.filter((g) => inCat(g, cat))
  return (
    <div className="tv-lib" ref={box}>
      <div className="tv-chips" role="group" aria-label="Show">
        {all.map((k) => (
          <button key={k.id} className={`tv-chip ${k.id === cat ? 'on' : ''}`} aria-pressed={k.id === cat} onClick={() => setCat(k.id)}>
            {k.name}<span className="n">{c.lib.games.filter((g) => inCat(g, k.id)).length}</span>
          </button>
        ))}
      </div>
      {list.length ? (
        <div className="tv-grid">{list.map((g) => <TvTile key={g.id} game={g} c={c} />)}</div>
      ) : (
        <div className="tv-empty"><h2>Nothing here yet</h2></div>
      )}
    </div>
  )
}

export function TvRooms({ c, onJoin }: { c: TvCommon; onJoin: (g: Game, code: string) => void }) {
  const box = useRef<HTMLDivElement>(null)
  const seen = new Set<string>()
  const rooms = c.allRooms
    ? c.lib.games.flatMap((g) => (c.allRooms![g.id]?.rooms ?? []).filter((r) => !seen.has(g.onlineId + r.code) && seen.add(g.onlineId + r.code)).map((room) => ({ game: g, room })))
    : null
  useFirstFocus(box, rooms?.length ?? -1)
  return (
    <div className="tv-rooms" ref={box}>
      {!rooms && <div className="tv-empty"><h2>Asking the central server...</h2></div>}
      {rooms?.length === 0 && (
        <div className="tv-empty">
          <Glyph code="E716" size={48} />
          <h2>No open rooms right now</h2>
          <p>Host one from a game's page and it shows up here for everyone.</p>
        </div>
      )}
      {rooms?.map(({ game, room }) => {
        const full = room.players >= room.max
        return (
          <div className="tv-room" key={game.id + room.code} data-game={game.id}>
            <span className="th"><Cover game={game} className="fill" /></span>
            <span className="k">
              <b>{room.name || game.name}</b>
              <small>{[game.name, roomAbout(room, c.lib.text)].filter(Boolean).join(', ')}</small>
            </span>
            <span className="code">{room.code}</span>
            <span className="num">{room.players} of {room.max}</span>
            <button className="btn" disabled={full} onClick={() => onJoin(game, room.code)}>{full ? 'Full' : 'Join'}</button>
          </div>
        )
      })}
    </div>
  )
}

// The top bar: the launcher's name, the sections with LB and RB at their ends, the time, and the way out.
export function TvTop({ lib, section, pad, onSection, onLeave }: { lib: Library; section: TvSection | ''; pad: boolean; onSection: (s: TvSection) => void; onLeave: () => void }) {
  const [now, setNow] = useState(() => new Date())
  useEffect(() => {
    const id = window.setInterval(() => setNow(new Date()), 15000)
    return () => window.clearInterval(id)
  }, [])
  return (
    <header className="tv-top">
      <span className="tv-brand"><img src="/icon.png" alt="" />{lib.branding.title}</span>
      <nav className="tv-tabs" aria-label="Sections">
        <PadKey k="LB" pad={pad} />
        {TV_SECTIONS.map((s) => (
          <button key={s.id} className={`tv-tab ${s.id === section ? 'on' : ''}`} aria-current={s.id === section ? 'page' : undefined} onClick={() => onSection(s.id)}>
            {s.name}
          </button>
        ))}
        <PadKey k="RB" pad={pad} />
      </nav>
      <span className="tv-clock">{now.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}</span>
      <button className="btn tv-leave" onClick={onLeave}><Glyph code="E73F" size={16} />Leave TV mode</button>
    </header>
  )
}

// Between pressing Play and the game's window: the launcher stays full screen with the game's art instead of
// dropping to the desktop. Once the game has had time to open it says so, and B (or the button) goes back to TV
// mode while the game keeps running; it goes by itself when the game closes.
export function TvStarting({ game, lib, label, pad, onBack }: { game: Game; lib: Library; label: string; pad: boolean; onBack: () => void }) {
  const [open, setOpen] = useState(false)
  useEffect(() => {
    const id = window.setTimeout(() => setOpen(true), 6000)
    return () => window.clearTimeout(id)
  }, [])
  return (
    <div className="tv-starting" role="status" aria-live="polite">
      <div className="art"><Cover game={game} className="fill" tall /></div>
      <h1>{game.name}</h1>
      <Slots n={game.maxPlayers} colors={lib.branding.playerColors} />
      {open ? (
        <>
          <p>{`${game.name} is running. Press Alt+F4 in it${pad ? ', or hold Start for its menu,' : ''} to come back here.`}</p>
          <button className="btn" onClick={onBack}><PadKey k="B" pad={pad} />Back to TV mode</button>
        </>
      ) : (
        <>
          <p>{label}...</p>
          <span className="tv-busy" aria-hidden="true"><i /></span>
        </>
      )}
    </div>
  )
}

type HintPlace = 'game' | 'sheet' | 'keys' | 'field' | 'other'
const HINTS: Record<HintPlace, { pad: string[][]; keys: string[][] }> = {
  game: {
    pad: [['A', 'Play'], ['Y', 'Game page'], ['X', 'Favorite'], ['B', 'Back'], ['LB RB', 'Sections'], ['Start', 'Leave TV mode']],
    keys: [['Enter', 'Play'], ['Double-click', 'Play'], ['Arrows', 'Move'], ['Esc', 'Back'], ['PgUp PgDn', 'Sections'], ['F11', 'Leave TV mode']],
  },
  sheet: { pad: [['A', 'Choose'], ['B', 'Back']], keys: [['Enter', 'Choose'], ['Arrows', 'Move'], ['Esc', 'Back']] },
  field: { pad: [['A', 'Type'], ['B', 'Back'], ['LB RB', 'Sections']], keys: [['Letters', 'Type'], ['Arrows', 'Move'], ['Esc', 'Back']] },
  keys: { pad: [['A', 'Type'], ['X', 'Delete'], ['B', 'Back']], keys: [['Letters', 'Type'], ['Backspace', 'Delete'], ['Enter', 'Join'], ['Esc', 'Back']] },
  other: {
    pad: [['A', 'Select'], ['B', 'Back'], ['LB RB', 'Sections'], ['Start', 'Leave TV mode']],
    keys: [['Arrows', 'Move'], ['Enter', 'Select'], ['Esc', 'Back'], ['PgUp PgDn', 'Sections'], ['F11', 'Leave TV mode']],
  },
}
const placeOf = (el: Element | null): HintPlace =>
  el?.closest('.tv-kb') ? 'keys' : el instanceof HTMLInputElement ? 'field' : el?.closest('[aria-modal="true"]') ? 'sheet' : el?.closest('[data-game]') ? 'game' : 'other'

// The buttons that do something where the focus is: on a game, in a sheet, on the on-screen keyboard, elsewhere.
export function TvHints({ pad, children }: { pad: boolean; children?: ReactNode }) {
  const [place, setPlace] = useState<HintPlace>(() => placeOf(document.activeElement))
  useEffect(() => {
    const onFocus = () => setPlace(placeOf(document.activeElement))
    document.addEventListener('focusin', onFocus)
    return () => document.removeEventListener('focusin', onFocus)
  }, [])
  const keys = pad ? HINTS[place].pad : HINTS[place].keys
  return (
    <footer className="tv-hints">
      {keys.map(([k, l]) => <span key={k}>{k.split(' ').map((x) => <kbd key={x} className="pk">{x}</kbd>)}{l}</span>)}
      <span className="grow" />
      {children}
    </footer>
  )
}
