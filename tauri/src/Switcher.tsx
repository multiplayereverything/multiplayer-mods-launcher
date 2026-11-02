// Ctrl+K: Discord's quick switcher for this launcher. Type a few letters of a game, or of a page (Open rooms,
// Settings), and Enter goes there; the arrow keys pick another match.
import { useEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react'
import { Cover } from './Cover'
import type { Game, Library } from './api'
import { Glyph } from './ui'

export interface SwitchTarget { kind: 'game' | 'page'; id: string; label: string; note: string; game?: Game; icon?: string }

// A match scores by where the letters fall: the start of the name, the start of a word, then anywhere.
function score(text: string, q: string): number {
  const t = text.toLowerCase()
  if (!q) return 1
  if (t.startsWith(q)) return 3
  if (t.includes(' ' + q)) return 2
  if (t.includes(q)) return 1
  // Initials: "sh2" finds Stealth Hunter 2.
  const initials = t.split(/[\s:-]+/).map((w) => w[0]).join('')
  return initials.startsWith(q) ? 1 : 0
}

export function Switcher({ lib, pages, onPick, onClose }: { lib: Library; pages: SwitchTarget[]; onPick: (t: SwitchTarget) => void; onClose: () => void }) {
  const [q, setQ] = useState('')
  const [at, setAt] = useState(0)
  const input = useRef<HTMLInputElement>(null)
  const list = useMemo(() => {
    const needle = q.trim().toLowerCase()
    const games: SwitchTarget[] = lib.games.map((g) => ({
      kind: 'game', id: g.id, label: g.name, game: g,
      note: g.mod ? `PC mod for ${g.mod.gameName || g.name}` : g.comingSoon ? 'Coming soon' : g.players,
    }))
    return [...games, ...pages]
      .map((x) => ({ x, s: Math.max(score(x.label, needle), x.game ? score(x.game.mod?.gameName ?? '', needle) : 0) }))
      .filter((m) => m.s > 0)
      .sort((a, b) => b.s - a.s || (a.x.kind === b.x.kind ? 0 : a.x.kind === 'page' && !needle ? 1 : 0))
      .slice(0, 8)
      .map((m) => m.x)
  }, [q, lib, pages])

  useEffect(() => {
    input.current?.focus()
  }, [])

  const pick = (t: SwitchTarget | undefined) => t && onPick(t)
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'ArrowDown') {
      e.preventDefault()
      setAt((i) => Math.min(list.length - 1, i + 1))
    } else if (e.key === 'ArrowUp') {
      e.preventDefault()
      setAt((i) => Math.max(0, i - 1))
    } else if (e.key === 'Enter') {
      e.preventDefault()
      pick(list[at])
    } else if (e.key === 'Escape') {
      e.preventDefault()
      e.stopPropagation()
      onClose()
    }
  }

  return (
    <div className="scrim top" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="switcher" role="dialog" aria-modal="true" aria-label="Go to a game or page">
        <label className="sw-input">
          <Glyph code="E721" size={18} />
          <input ref={input} value={q} placeholder="Go to a game or page" spellCheck={false} aria-label="Go to a game or page"
            onChange={(e) => (setQ(e.target.value), setAt(0))} onKeyDown={onKey} />
          <kbd>Esc</kbd>
        </label>
        <div className="sw-list" role="listbox">
          {list.map((t, i) => (
            <button key={t.kind + t.id} role="option" aria-selected={i === at} className={i === at ? 'on' : ''}
              onMouseEnter={() => setAt(i)} onClick={() => pick(t)}>
              <span className="th">{t.game ? <Cover game={t.game} className="fill" /> : <Glyph code={t.icon ?? 'E8A9'} />}</span>
              <span className="k"><b>{t.label}</b><small>{t.note}</small></span>
              {i === at && <kbd>Enter</kbd>}
            </button>
          ))}
          {list.length === 0 && <p className="sw-empty">Nothing by that name.</p>}
        </div>
        <div className="sw-foot"><span><kbd>↑</kbd><kbd>↓</kbd> pick</span><span><kbd>Enter</kbd> open</span><span><kbd>Ctrl</kbd>+<kbd>K</kbd> anywhere</span></div>
      </div>
    </div>
  )
}
