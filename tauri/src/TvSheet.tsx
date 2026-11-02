// TV mode's choices for an online game, and the on-screen keyboard for a room code. A sheet takes the controller
// while it is open: the D-pad stays inside it (moveFocus keeps to an aria-modal box), B steps back, and on the
// keyboard X deletes, as a console's keyboard does. A real keyboard types into it too, and the mouse clicks.
import { useEffect, useRef, useState, type MutableRefObject } from 'react'
import type { Action, Game, Library, Rooms } from './api'
import { hostChoices, isCoop, joinByAddress, joinByCode, looksLikeAddress, mainActions, roomAbout } from './model'
import { Glyph } from './ui'
import { PadKey } from './Tv'

// What a sheet does with the controller's B and X before the page gets them; true means handled.
export interface SheetPad { back?: () => boolean; x?: () => boolean }

interface Item { key: string; label: string; note?: string; icon: string; primary?: boolean; disabled?: boolean; run: () => void }

function Items({ items }: { items: Item[] }) {
  const box = useRef<HTMLDivElement>(null)
  useEffect(() => {
    box.current?.querySelector<HTMLElement>('button:not(:disabled)')?.focus({ preventScroll: true })
  }, [items.length])
  return (
    <div className="tv-items" ref={box}>
      {items.map((it) => (
        <button key={it.key} className={`tv-item ${it.primary ? 'primary' : ''}`} disabled={it.disabled} onClick={it.run}>
          <Glyph code={it.icon} size={22} />
          <span><b>{it.label}</b>{it.note && <small>{it.note}</small>}</span>
        </button>
      ))}
    </div>
  )
}

const ROWS = ['1234567890', 'QWERTYUIOP', 'ASDFGHJKL', 'ZXCVBNM']

// A code stops at five; it grows past that only as an address, once it has a '.' or ':' (192.168.1.20:7790).
function grow(t: string, ch: string, address: boolean): string {
  const isAddress = address && /[.:]/.test(t + ch)
  return t.length < (isAddress ? 40 : 5) ? t + ch : t
}

// A room code is five letters and digits; with `address` the host's IP and port can be typed instead.
function CodeKeyboard({ address, pad, padRef, onJoin }: { address: boolean; pad: boolean; padRef: MutableRefObject<SheetPad>; onJoin: (text: string) => void }) {
  const [text, setText] = useState('')
  const isAddress = address && looksLikeAddress(text)
  const ok = isAddress ? text.trim().length > 3 : text.length === 5
  const type = (ch: string) => setText((t) => grow(t, ch, address))
  const del = () => setText((t) => t.slice(0, -1))
  const box = useRef<HTMLDivElement>(null)
  const latest = useRef({ text, ok, onJoin })
  useEffect(() => {
    latest.current = { text, ok, onJoin }
  })
  useEffect(() => {
    box.current?.querySelector<HTMLElement>('button')?.focus({ preventScroll: true })
    const pr = padRef.current
    const add = (ch: string) => setText((t) => grow(t, ch, address))
    const back = () => setText((t) => t.slice(0, -1))
    pr.x = () => (back(), true)
    // A real keyboard types straight in; Enter joins once the code is whole.
    const onKey = (e: KeyboardEvent) => {
      if (e.ctrlKey || e.altKey || e.metaKey) return
      const k = e.key.length === 1 ? e.key.toUpperCase() : e.key
      if (/^[A-Z0-9]$/.test(k) || (address && (k === '.' || k === ':'))) add(k)
      else if (k === 'Backspace') back()
      else if (k === 'Enter') {
        if (latest.current.ok) latest.current.onJoin(latest.current.text)
      } else return
      e.preventDefault()
      e.stopPropagation()
    }
    window.addEventListener('keydown', onKey, true)
    return () => {
      window.removeEventListener('keydown', onKey, true)
      pr.x = undefined
    }
  }, [address, padRef])

  return (
    <div className="tv-kb" ref={box}>
      <div className="tv-kb-field" aria-live="polite" aria-label={text ? `Typed ${text.split('').join(' ')}` : 'Nothing typed yet'}>
        {isAddress || text.length > 5
          ? <span className="addr">{text}<i className="caret" /></span>
          : Array.from({ length: 5 }, (_, i) => <span key={i} className={`cell ${i === text.length ? 'at' : ''}`}>{text[i] ?? ''}</span>)}
      </div>
      <p className="tv-kb-note">{address ? 'The five letters the host sees, or their address and port.' : 'The five letters the host sees.'}</p>
      {ROWS.map((row, r) => (
        <div className="tv-kb-row" key={r}>
          {row.split('').map((ch) => <button key={ch} className="key" onClick={() => type(ch)}>{ch}</button>)}
          {r === 3 && address && ['.', ':'].map((ch) => <button key={ch} className="key" onClick={() => type(ch)}>{ch}</button>)}
        </div>
      ))}
      <div className="tv-kb-row">
        <button className="key wide" onClick={del}><PadKey k="X" pad={pad} />Delete</button>
        <button className="key wide" onClick={() => setText('')}>Clear</button>
        <button className="key wide done" disabled={!ok} onClick={() => onJoin(text)}>Join</button>
      </div>
    </div>
  )
}

export function TvPlaySheet({ game, lib, rooms, pad, padRef, onRun, onLobby, onPage, onClose }: {
  game: Game
  lib: Library
  rooms: Rooms | undefined
  pad: boolean
  padRef: MutableRefObject<SheetPad>
  onRun: (a: Action, code?: string, address?: string) => void
  onLobby: (quick: boolean) => void
  onPage: () => void
  onClose: () => void
}) {
  const [step, setStep] = useState<'menu' | 'host' | 'code'>('menu')
  const byCode = joinByCode(game)
  const byAddress = joinByAddress(game)
  const coop = isCoop(game) && !game.lobby
  const choices = hostChoices(game)
  const open = (rooms?.rooms ?? []).filter((r) => r.max <= 0 || r.players < r.max).sort((a, b) => b.players - a.players)
  // Hosting where friends join with a code: the online action itself, as the game page's Quick play hosts.
  const hostAction = game.actions.find((a) => a.id === game.joinAction) ?? byCode
  const others = mainActions(game, lib.user).filter((a) => a.id !== game.joinAction && !a.coopHost && !a.coopJoin && !a.joinsByCode)

  useEffect(() => {
    const pr = padRef.current
    pr.back = () => {
      if (step === 'menu') onClose()
      else setStep('menu')
      return true
    }
    return () => {
      pr.back = undefined
    }
  }, [step, onClose, padRef])

  let items: Item[] = []
  if (step === 'menu') {
    if (game.lobby) {
      items.push({ key: 'lobby', label: 'Play online', note: 'Host a room or join one in the lobby', icon: 'E774', primary: true, run: () => onLobby(false) })
      items.push({ key: 'quick', label: 'Quick play', note: 'Straight into the busiest open room', icon: 'E945', run: () => onLobby(true) })
    } else {
      if (byCode || coop) {
        items.push({
          key: 'quick', label: 'Quick play', icon: 'E945', primary: true,
          note: open[0] ? `Joins ${open[0].name || open[0].code}, ${open[0].players} of ${open[0].max} players` : 'Nobody is hosting: you host a room',
          run: () => (open[0] && byCode ? onRun(byCode, open[0].code) : hostAction && onRun(coop ? (choices[0]?.action ?? hostAction) : hostAction)),
        })
      }
      if (coop && choices.length > 1) items.push({ key: 'host', label: 'Host a room', note: 'Pick the players and how friends join', icon: 'E774', run: () => setStep('host') })
      else if (coop ? choices[0] : hostAction) {
        const a = coop ? choices[0].action : hostAction!
        items.push({ key: 'host', label: 'Host a room', note: 'Friends join with the room code you get', icon: 'E774', run: () => onRun(a) })
      }
      if (byCode || byAddress) items.push({ key: 'code', label: 'Join with a code', note: byAddress ? 'A room code, or the host\'s address' : 'The five letters the host sees', icon: 'E8D2', run: () => setStep('code') })
      for (const r of open.slice(0, 4)) {
        if (!byCode) break
        items.push({ key: 'room' + r.code, label: `Join ${r.name || r.code}`, icon: 'E716',
          note: [r.code, `${r.players} of ${r.max} players`, roomAbout(r, lib.text)].filter(Boolean).join(', '), run: () => onRun(byCode, r.code) })
      }
    }
    for (const a of others) items.push({ key: 'a' + a.id, label: a.label, note: a.description, icon: a.icon || 'E768', disabled: a.state !== 'ready', run: () => onRun(a) })
    items.push({ key: 'page', label: 'Game page', note: 'Everything else: settings, guide, other ways to connect', icon: 'E946', run: onPage })
  } else if (step === 'host') {
    items = choices.map((c) => ({
      key: c.action.id, label: `${c.players} players`, icon: c.relay ? 'E774' : 'E968',
      note: c.relay ? 'Friends join with a room code. No port forwarding.' : 'Friends join with your IP address: LAN, Radmin VPN or ZeroTier.',
      run: () => onRun(c.action),
    }))
  }

  return (
    <div className="scrim tv-scrim" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="tv-sheet" role="dialog" aria-modal="true" aria-label={`${game.name}: ${step === 'code' ? 'join with a code' : 'how to play'}`}>
        <div className="tv-sheet-h">
          <span className="kick">{step === 'menu' ? 'How do you want to play?' : step === 'host' ? 'Host a room' : 'Join with a code'}</span>
          <h2>{game.name}</h2>
        </div>
        {step === 'code'
          ? <CodeKeyboard address={!!byAddress} pad={pad} padRef={padRef}
              onJoin={(t) => (looksLikeAddress(t) && byAddress ? onRun(byAddress, undefined, t.trim()) : byCode && onRun(byCode, t))} />
          : <Items key={step} items={items} />}
        <div className="tv-sheet-f">
          <span><PadKey k="A" pad={pad} />{pad ? 'Choose' : 'Click or Enter to choose'}</span>
          <span><PadKey k="B" pad={pad} />{step === 'menu' ? 'Close' : 'Back'}{pad ? '' : ' (Esc)'}</span>
        </div>
      </div>
    </div>
  )
}
