// The pieces every page uses: box-art tiles, shelves that page sideways, pickers, switches and a menu.
import { useEffect, useLayoutEffect, useRef, useState, type ReactNode } from 'react'
import { Cover } from './Cover'
import type { Game, Library, ModInfo } from './api'
import { leadAction, stateLine } from './model'

export function Pips({ n, colors }: { n: number; colors: string[] }) {
  return (
    <span className="pips" aria-hidden="true">
      {Array.from({ length: Math.min(6, Math.max(1, n)) }, (_, i) => <i key={i} style={{ background: colors[i % colors.length] }} />)}
    </span>
  )
}

interface TileProps {
  game: Game
  lib: Library
  mods: Record<string, ModInfo> | null
  playing: boolean
  favorite: boolean
  onOpen: () => void
}

// A glyph from Segoe Fluent Icons, as launcher.json and the C++ window name them: the character or its hex code.
export function Glyph({ code, className, size }: { code: string; className?: string; size?: number }) {
  if (!code) return null
  const ch = /^[0-9A-F]{4}$/i.test(code) ? String.fromCodePoint(parseInt(code, 16)) : code
  return <i className={`glyph ${className ?? ''}`} style={size ? { fontSize: size } : undefined} aria-hidden="true">{ch}</i>
}

// The website's quality grade: a rounded square with the letter, in the grade's color.
export function GradeChip({ grade, size = 22 }: { grade: string; size?: number }) {
  const known = /^[ABCDX]$/.test(grade)
  return (
    <span className={`grade ${known ? 'g-' + grade.toLowerCase() : 'unrated'}`} style={{ width: size, height: size }} aria-label={known ? `Quality ${grade}` : 'Not rated yet'}>
      {known ? grade : '?'}
    </span>
  )
}

// A card as the C++ library draws one: the art across the top with its badges, then the name in the condensed
// display font, the player pips and count, and the state. The selected card has a white outline.
export function Tile({ game, lib, mods, playing, favorite, selected, rooms = 0, onOpen, onPlay }: TileProps & { selected?: boolean; rooms?: number; onPlay?: () => void }) {
  const line = stateLine(game, lib, mods, playing)
  const lead = leadAction(game)
  const t = (k: string, f: string) => lib.text[k] ?? f
  const notOnServer = game.site?.status === 'not-on-server'
  return (
    <div className="tile">
      <button className={`card-c ${selected ? 'picked' : ''}`} onClick={onOpen} aria-label={`${game.name}, ${game.players}, ${line.text}`}
        style={{ ['--gc' as string]: game.color || '#323743' }}>
        <span className="art">
          <Cover game={game} className="fill" />
          <span className="badges">
            {game.onlineId && <span className="badge">{t('badge.online', 'Online')}</span>}
            {game.mod && <span className="badge">{t('badge.pc', 'PC')}</span>}
          </span>
          <span className="badges right">
            {favorite && <span className="badge star">{'★'}</span>}
            {game.site?.quality && <GradeChip grade={game.site.quality} />}
          </span>
        </span>
        <span className="info">
          <span className="name">{game.name}</span>
          <span className="ppl"><Pips n={game.maxPlayers} colors={lib.branding.playerColors} /><span>{game.players}</span></span>
          <span className="st">
            <span className={`tone-${line.tone}`}>{line.text}</span>
            {notOnServer ? <span className="tone-muted right">{t('status.notOnServer', 'Not on the server yet')}</span>
              : rooms > 0 && <span className="tone-online right">{rooms === 1 ? t('rooms.one', '1 room open') : t('rooms.count', '${N} rooms open').replace('${N}', String(rooms))}</span>}
          </span>
        </span>
      </button>
      {/* Start from the grid, as Steam and Playnite allow: red, because it starts the game. */}
      {onPlay && lead?.state === 'ready' && !game.comingSoon && (
        <button className="card-play" aria-label={`${lead.label}: ${game.name}`} title={lead.label} onClick={onPlay}>
          <Glyph code="E768" />
        </button>
      )}
    </div>
  )
}

// The player slots strip of the details panel: P1 to Pn, each with its color along the top.
export function Slots({ n, colors, names }: { n: number; colors: string[]; names?: string[] }) {
  return (
    <div className="pstrip" aria-label={`${n} player slots`}>
      {Array.from({ length: Math.min(6, Math.max(1, n)) }, (_, i) => (
        <span key={i} className={`pslot ${names?.[i] ? 'filled' : ''}`} style={{ ['--p' as string]: colors[i % colors.length] }}>
          {names?.[i] ?? `P${i + 1}`}
        </span>
      ))}
    </div>
  )
}

const SYNCED_KEYS = ['see', 'story', 'enemies', 'world', 'items']

// The website's block in the details panel: the grade and what it means, the facts line, and what stays in sync.
export function SiteBlock({ game, lib }: { game: Game; lib: Library }) {
  const s = game.site
  if (!s || (!s.status && !s.quality)) return null
  const t = (k: string, f = '', vars: Record<string, string> = {}) =>
    Object.entries(vars).reduce((x, [key, v]) => x.replace('${' + key + '}', v), lib.text[k] ?? f)
  const soon = game.comingSoon
  const graded = !!s.quality && !!lib.text['site.quality.' + s.quality]
  const note = soon ? t('site.planned') : graded ? t('site.quality.' + s.quality) : t('site.unrated')
  const facts = [
    t('site.status.' + (soon ? 'coming-soon' : s.status)),
    s.version && !soon ? t(s.status === 'testing' ? 'site.testBuild' : 'site.version', '', { VERSION: s.version }) : '',
    s.updated && !soon ? t('site.updated', '', { DATE: s.updated }) : '',
    s.connect,
  ].filter(Boolean).join(', ')
  const synced = SYNCED_KEYS.some((k) => s.synced[k] && s.synced[k] !== 'unknown')
  return (
    <div className="siteblock">
      <div className="grade-row"><GradeChip grade={graded ? s.quality : ''} size={24} /><span>{note}</span></div>
      {facts && <p className="facts">{facts}</p>}
      {synced && (
        <div className="synced">
          <span className="muted">{t('site.synced', 'In sync')}</span>
          {SYNCED_KEYS.map((k) => {
            const state = s.synced[k] ?? 'unknown'
            return (
              <span key={k} className={`sync ${state}`} title={t('site.synced.' + state)}>
                <i />{t('site.synced.' + k, k)}
              </span>
            )
          })}
        </div>
      )}
    </div>
  )
}

// A button as the C++ details panel paints one: Bahnschrift on the raised surface, a glyph before the label, and
// a second line under it when there is one. Red only when it starts the game.
export function PanelButton({ icon, label, sub, primary, warn, disabled, onClick }: {
  icon?: string; label: string; sub?: string; primary?: boolean; warn?: boolean; disabled?: boolean; onClick?: () => void
}) {
  return (
    <button className={`pbtn ${sub ? 'two' : ''} ${primary ? 'primary' : ''}`} disabled={disabled} onClick={onClick}>
      <span className="top">{icon && <Glyph code={icon} />}<span>{label}</span></span>
      {sub && <span className={`sub ${warn ? 'warn' : ''}`}>{sub}</span>}
    </button>
  )
}

// A titled row of tiles that pages sideways, with "1 of 3" and arrows.
export function Shelf({ title, note, children, count, onSeeAll }: { title: string; note?: string; children: ReactNode; count: number; onSeeAll?: () => void }) {
  const view = useRef<HTMLDivElement>(null)
  const [per, setPer] = useState(6)
  const [page, setPage] = useState(0)
  useLayoutEffect(() => {
    const el = view.current
    if (!el) return
    const fit = () => setPer(Math.max(2, Math.floor((el.clientWidth - 40 + 16) / (232 + 16))))
    fit()
    const ro = new ResizeObserver(fit)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])
  const pages = Math.max(1, Math.ceil(count / per))
  const at = Math.min(page, pages - 1)
  return (
    <section className="shelf" style={{ ['--per' as string]: per }}>
      <div className="shelf-h">
        <div>
          <h2>{title}</h2>
          {note && <p>{note}</p>}
        </div>
        <div className="pager">
          {pages > 1 && <small>{at + 1} of {pages}</small>}
          {onSeeAll && <button className="see" onClick={onSeeAll}>See all</button>}
          {pages > 1 && (
            <>
              <button className="ar" aria-label="Previous" disabled={at === 0} onClick={() => setPage(at - 1)}><Glyph code="E76B" size={18} /></button>
              <button className="ar" aria-label="Next" disabled={at >= pages - 1} onClick={() => setPage(at + 1)}><Glyph code="E76C" size={18} /></button>
            </>
          )}
        </div>
      </div>
      <div className="viewport" ref={view}>
        <div className="track" style={{ transform: `translateX(calc(${-at} * (100% + var(--gap))))` }}>{children}</div>
      </div>
    </section>
  )
}

// Closes on a click outside or Escape.
function useDismiss(open: boolean, close: () => void) {
  const root = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!open) return
    const onDown = (e: MouseEvent) => !root.current?.contains(e.target as Node) && close()
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && (e.stopImmediatePropagation(), close())
    window.addEventListener('mousedown', onDown)
    window.addEventListener('keydown', onKey, true)
    return () => {
      window.removeEventListener('mousedown', onDown)
      window.removeEventListener('keydown', onKey, true)
    }
  }, [open, close])
  return root
}

// A rounded picker: a quiet label, then the choice and a chevron; the list drops under it.
export function Picker<T extends string>({ label, value, options, onChange }: { label: string; value: T; options: { id: T; name: string }[]; onChange: (v: T) => void }) {
  const [open, setOpen] = useState(false)
  const root = useDismiss(open, () => setOpen(false))
  const current = options.find((o) => o.id === value)
  return (
    <div className="pill-sel" ref={root}>
      <button className={`drop ${open ? 'open' : ''}`} aria-haspopup="listbox" aria-expanded={open} onClick={() => setOpen(!open)}>
        <span className="pl">{label}</span><span>{current?.name}</span><Glyph code="E70D" size={14} />
      </button>
      {open && (
        <div className="menu dropmenu" role="listbox">
          {options.map((o) => (
            <button key={o.id} role="option" aria-selected={o.id === value} onClick={() => { onChange(o.id); setOpen(false) }}>
              <span>{o.name}</span>{o.id === value && <Glyph code="E73E" size={15} className="ck" />}
            </button>
          ))}
        </div>
      )}
    </div>
  )
}

export interface MenuItem { label: string; icon?: ReactNode; danger?: boolean; disabled?: boolean; onClick: () => void }

// A button that opens a list of actions (the "..." beside Play).
export function MenuButton({ label, icon, items, className, up }: { label: string; icon: ReactNode; items: (MenuItem | null)[]; className?: string; up?: boolean }) {
  const [open, setOpen] = useState(false)
  const root = useDismiss(open, () => setOpen(false))
  const list = items.filter((i): i is MenuItem => !!i)
  return (
    <div className="menu-wrap" ref={root}>
      <button className={className} aria-label={label} title={label} aria-haspopup="menu" aria-expanded={open} onClick={() => setOpen(!open)}>{icon}</button>
      {open && (
        <div className={`menu actmenu ${up ? 'up' : ''}`} role="menu">
          {list.map((i) => (
            <button key={i.label} role="menuitem" className={i.danger ? 'danger' : ''} disabled={i.disabled} onClick={() => { setOpen(false); i.onClick() }}>
              {i.icon}<span>{i.label}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  )
}

// One choice out of a few, as a segmented bar whose chosen part is solid.
export function Seg<T extends string>({ value, options, onChange, label }: { value: T; options: { id: T; name: string }[]; onChange: (v: T) => void; label: string }) {
  return (
    <div className="seg" role="radiogroup" aria-label={label}>
      {options.map((o) => (
        <button key={o.id} role="radio" aria-checked={o.id === value} onClick={() => onChange(o.id)}>{o.name}</button>
      ))}
    </div>
  )
}

// A labelled on/off switch in a pill.
export function Toggle({ label, on, onChange, icon }: { label: string; on: boolean; onChange: (v: boolean) => void; icon?: ReactNode }) {
  return (
    <button className="toggle" role="switch" aria-checked={on} onClick={() => onChange(!on)}>
      {icon}{label}<span className="switch" />
    </button>
  )
}

export function Switch({ on, onChange, label }: { on: boolean; onChange: (v: boolean) => void; label: string }) {
  return <button className="switch" role="switch" aria-checked={on} aria-label={label} onClick={() => onChange(!on)} />
}
