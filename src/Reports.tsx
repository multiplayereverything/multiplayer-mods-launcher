// "How did it run?": the question after a long session, and the game page's "How it runs" card. The reports live on
// the central server (src-tauri/src/launcher/reports.rs); nothing about the player but the PC's parts goes with them.
import { useEffect, useRef, useState } from 'react'
import type { Game, Library, ModInfo, Report, Reports, Verdict } from './api'
import { loadReports, sendReport } from './api'
import { agoText } from './model'

// Three answers, each saying what counts, so two players mean the same thing by them.
export const VERDICTS: { id: Verdict; label: string; means: string }[] = [
  { id: 'runs', label: 'Plays great', means: 'Started, played and synced with no trouble' },
  { id: 'issues', label: 'Has problems', means: 'Playable, but with lag, desyncs, crashes or extra setup' },
  { id: 'broken', label: "Won't run", means: "Didn't start, or couldn't connect at all" },
]
const label = (v: Verdict) => VERDICTS.find((x) => x.id === v)?.label ?? v

// A long session is the only time the launcher asks.
export const ASK_AFTER_SECS = 600

// What a report is about: a PC mod's installed version, else the version of the player that ran the Flash game.
export function reportVersion(game: Game, lib: Library, mods: Record<string, ModInfo> | null): string {
  if (game.mod) return mods?.[game.id]?.installedVersion ?? ''
  const id = game.player || game.actions.find((a) => a.playerId)?.playerId || ''
  const p = lib.players.find((x) => x.id === id && x.found) ?? lib.players.find((x) => x.found)
  return p?.version ?? ''
}

// Which version of each game this PC has been asked about, so a version is asked about once.
const ASKED = 'fl.ranAsked'
const readAsked = (): Record<string, string> => {
  try {
    return JSON.parse(localStorage.getItem(ASKED) || '{}') as Record<string, string>
  } catch {
    return {}
  }
}
export const wasAsked = (game: string, version: string) => readAsked()[game] === version
export function markAsked(game: string, version: string) {
  try {
    localStorage.setItem(ASKED, JSON.stringify({ ...readAsked(), [game]: version }))
  } catch {
    // Private storage off: the question may come once more, which is harmless.
  }
}

export interface RunAskRequest { game: Game; version: string; mins: number }

// The card that asks. Picking an answer offers a note (not in TV mode, where typing is slow); Send or Just send it.
export function RunAsk({ ask, tv, onSent, onClose }: { ask: RunAskRequest; tv: boolean; onSent: (text: string, ok: boolean) => void; onClose: () => void }) {
  const [verdict, setVerdict] = useState<Verdict | null>(null)
  const [note, setNote] = useState('')
  const [sending, setSending] = useState(false)
  const box = useRef<HTMLDivElement>(null)
  useEffect(() => {
    box.current?.querySelector<HTMLElement>(verdict ? 'textarea' : '.rv button')?.focus()
  }, [verdict])
  // Unanswered, it goes after a minute: it is a question, not a chore.
  useEffect(() => {
    if (verdict) return
    const id = window.setTimeout(onClose, 60000)
    return () => window.clearTimeout(id)
  }, [verdict, onClose])
  const send = (v: Verdict, text: string) => {
    setSending(true)
    sendReport(ask.game.id, v, ask.version, ask.mins, text)
      .then(() => onSent(`Thanks. Your report is on ${ask.game.name}'s page.`, true))
      .catch((e) => onSent(`The report couldn't be sent: ${e}`, false))
  }
  const pick = (v: Verdict) => (tv ? send(v, '') : setVerdict(v))
  return (
    <div className="run-ask" role="dialog" aria-label={`How did ${ask.game.name} run?`} ref={box}
      onKeyDown={(e) => e.key === 'Escape' && (e.stopPropagation(), onClose())}>
      <button className="ask-x" aria-label="Not now" onClick={onClose}>{'×'}</button>
      {!verdict ? (
        <>
          <b>How did {ask.game.name} run?</b>
          <small>It helps other players. Shows on its page with your graphics card and {/Windows NT/.test(navigator.userAgent) || !/Linux/.test(navigator.userAgent) ? 'Windows' : 'Linux'} version, no name.</small>
          <div className="rv">
            {VERDICTS.map((v) => (
              <button key={v.id} className={`btn v-${v.id}`} disabled={sending} onClick={() => pick(v.id)}>
                <i />{v.label}<small>{v.means}</small>
              </button>
            ))}
          </div>
        </>
      ) : (
        <>
          <b>{label(verdict)}. Anything others should know?</b>
          <textarea rows={3} maxLength={500} value={note} onChange={(e) => setNote(e.target.value)}
            placeholder="What worked, what didn't, settings that helped" />
          <div className="rv-row">
            <span className="muted">{note.length} / 500</span>
            <button className="btn" disabled={sending} onClick={() => send(verdict, '')}>Just send it</button>
            <button className="btn main" disabled={sending} onClick={() => send(verdict, note)}>{sending ? 'Sending...' : 'Send'}</button>
          </div>
        </>
      )}
    </div>
  )
}

const pc = (r: Report) => [r.gpu, r.os].filter(Boolean).join(', ')

// The game page's card: the share that plays great, the three counts, and the newest reports.
export function HowItRuns({ game, onReport }: { game: Game; onReport?: () => void }) {
  const [data, setData] = useState<Reports | null>(null)
  const [error, setError] = useState('')
  const [all, setAll] = useState(false)
  useEffect(() => {
    let alive = true
    setData(null)
    setError('')
    loadReports(game.id).then((r) => alive && setData(r)).catch((e) => alive && setError(String(e)))
    return () => {
      alive = false
    }
  }, [game.id])
  if (error) return null  // an older server, or offline: the page just doesn't have the card
  if (!data) return null
  const { total, counts } = data
  const share = total ? Math.round((counts.runs / total) * 100) : 0
  const shown = all ? data.recent : data.recent.slice(0, 3)
  return (
    <div className="card runs">
      <div className="card-h"><h3>How it runs</h3><span>{total === 1 ? '1 report' : `${total} reports`}</span></div>
      {total === 0 ? (
        <p className="muted">No reports yet. After you play for 10 minutes the launcher asks how it went.</p>
      ) : (
        <>
          <p className="runs-share"><b className={share >= 70 ? 'tone-ok' : share >= 40 ? 'tone-warn' : 'tone-bad'}>{share}%</b> say it plays great</p>
          <div className="runs-bar" role="img" aria-label={VERDICTS.map((v) => `${counts[v.id]} ${label(v.id).toLowerCase()}`).join(', ')}>
            {VERDICTS.map((v) => counts[v.id] > 0 && <i key={v.id} className={`v-${v.id}`} style={{ flexGrow: counts[v.id] }} />)}
          </div>
          <div className="runs-counts">
            {VERDICTS.map((v) => <div key={v.id} className={`v-${v.id}`}><i />{v.label}<b>{counts[v.id]}</b></div>)}
          </div>
          <ul className="runs-list">
            {shown.map((r, i) => (
              <li key={i} className={`v-${r.verdict}`}>
                <span><i />{label(r.verdict)}{r.note && <q>{r.note}</q>}<small>{pc(r)}</small></span>
                <small>{[r.version && `v${r.version}`, agoText(r.at)].filter(Boolean).join(', ')}</small>
              </li>
            ))}
          </ul>
        </>
      )}
      <div className="acts">
        {data.recent.length > 3 && <button className="btn" onClick={() => setAll((a) => !a)}>{all ? 'Fewer reports' : `Newest ${data.recent.length} reports`}</button>}
        {onReport && <button className="btn" onClick={onReport}>Say how it runs</button>}
      </div>
    </div>
  )
}
