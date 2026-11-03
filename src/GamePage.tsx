import { useEffect, useRef, useState, type ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { Cover } from './Cover'
import type { Action, Game, Library, ModInfo, RelayStatus, Room } from './api'
import { copyText, loadRooms, open, setGamePlayer, setGameSetting } from './api'
import {
  agoText, fileAction, hostChoices, isCoop, joinByAddress, joinByCode, lastPlayed, looksLikeAddress, mainActions, moreActions, newer,
  roomAbout, stateLine,
} from './model'
import { HowItRuns } from './Reports'
import { Glyph, PanelButton, SiteBlock, Slots, Switch } from './ui'
import { useDecode } from './useDecode'

function Dialog({ title, children, onClose }: { title: string; children: ReactNode; onClose: () => void }) {
  const box = useRef<HTMLDivElement>(null)
  // Focus goes into the dialog, and back to the button that opened it when it closes.
  useEffect(() => {
    const opener = document.activeElement instanceof HTMLElement ? document.activeElement : null
    // After the key that closed it is done: an Enter that submits must not also press the button focus returns to.
    return () => {
      window.setTimeout(() => opener?.isConnected && opener.focus(), 0)
    }
  }, [])
  useEffect(() => {
    box.current?.querySelector<HTMLElement>('input, select, .btn.main')?.focus()
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && (e.stopImmediatePropagation(), onClose())
    window.addEventListener('keydown', esc, true)
    return () => window.removeEventListener('keydown', esc, true)
  }, [onClose])
  // On the document itself: inside the page, the page's entrance animation would keep it under the messages.
  return createPortal(
    <div className="scrim" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="dialog" role="dialog" aria-modal="true" aria-label={title} ref={box}>
        <h2>{title}</h2>
        {children}
      </div>
    </div>,
    document.body,
  )
}

function HostDialog({ game, onHost, onClose }: { game: Game; onHost: (a: Action) => void; onClose: () => void }) {
  const choices = hostChoices(game)
  const counts = [...new Set(choices.map((c) => c.players))].sort((a, b) => a - b)
  const [players, setPlayers] = useState(counts[0] ?? 2)
  const viaRelay = choices.some((c) => c.relay)
  const viaIp = choices.some((c) => !c.relay)
  const [relay, setRelay] = useState(viaRelay)
  const pick = choices.find((c) => c.players === players && c.relay === relay) ?? choices.find((c) => c.players === players)
  return (
    <Dialog title={`Host ${game.name}`} onClose={onClose}>
      <div className="opt-h">Players</div>
      <div className="seg">
        {counts.map((n) => <button key={n} role="radio" aria-checked={n === players} onClick={() => setPlayers(n)}>{n} players</button>)}
      </div>
      {viaRelay && viaIp && (
        <>
          <div className="opt-h">Friends join with</div>
          <div className="choice">
            <button aria-pressed={relay} onClick={() => setRelay(true)}><Glyph code="E774" size={18} /><span><b>A room code</b><small>Through the central server. No port forwarding.</small></span></button>
            <button aria-pressed={!relay} onClick={() => setRelay(false)}><Glyph code="E968" size={18} /><span><b>Your IP address</b><small>Straight to this PC: LAN, Radmin VPN or ZeroTier.</small></span></button>
          </div>
        </>
      )}
      <p className="dialog-note">{pick?.action.description}</p>
      <div className="dialog-row">
        <button className="btn" onClick={onClose}>Cancel</button>
        <button className="btn main" disabled={!pick} onClick={() => pick && onHost(pick.action)}>Host</button>
      </div>
    </Dialog>
  )
}

function JoinDialog({ game, onJoin, onClose }: { game: Game; onJoin: (a: Action, code?: string, address?: string) => void; onClose: () => void }) {
  const [text, setText] = useState('')
  const byCode = joinByCode(game)
  const byAddress = joinByAddress(game)
  const address = looksLikeAddress(text)
  const code = text.replace(/[^a-z0-9]/gi, '').toUpperCase()
  const action = address ? byAddress : byCode
  const ok = !!action && (address ? text.trim().length > 3 : code.length === 5)
  const go = () => ok && action && (address ? onJoin(action, undefined, text.trim()) : onJoin(action, code))
  return (
    <Dialog title={`Join ${game.name}`} onClose={onClose}>
      <label className="opt-h" htmlFor="join-code">{byAddress ? 'Room code or the host\'s address' : 'Room code'}</label>
      <input id="join-code" className="field code" value={text} onChange={(e) => setText(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && (e.preventDefault(), go())} placeholder="ABCDE" spellCheck={false} />
      <p className="dialog-note">{address ? (byAddress ? 'Connects straight to the host.' : 'This game joins with a room code.') : 'The five letters the host sees.'}</p>
      <div className="dialog-row">
        <button className="btn" onClick={onClose}>Cancel</button>
        <button className="btn main" disabled={!ok} onClick={go}>Join</button>
      </div>
    </Dialog>
  )
}

function RoomsTab({ game, text, onJoinRoom }: { game: Game; text: Record<string, string>; onJoinRoom: (code: string) => void }) {
  const [rooms, setRooms] = useState<Room[] | null>(null)
  const [error, setError] = useState('')
  useEffect(() => {
    let alive = true
    const get = () =>
      loadRooms(game.id)
        .then((r) => alive && (setRooms(r.rooms), setError(r.onServer ? r.error : 'Not on the central server yet.')))
        .catch((e) => alive && setError(String(e)))
    get()
    const id = window.setInterval(get, 15000)
    return () => {
      alive = false
      window.clearInterval(id)
    }
  }, [game.id])
  return (
    <div className="card">
      <div className="card-h"><h3>Open rooms</h3><span>Refreshes every 15 seconds</span></div>
      {!rooms && !error && <p className="muted">Asking the central server...</p>}
      {error && <p className="muted">{error}</p>}
      {rooms?.length === 0 && !error && <p className="muted">No open rooms. Host one and friends see it here.</p>}
      <div className="dl">
        {rooms?.map((r) => (
          <div key={r.code} className="room-line">
            <span><b>{r.name || r.code}</b><small>{[r.code, `${r.players} of ${r.max} players`, roomAbout(r, text)].filter(Boolean).join(', ')}</small></span>
            <button className="btn sm" disabled={r.players >= r.max} onClick={() => onJoinRoom(r.code)}>{r.players >= r.max ? 'Full' : 'Join'}</button>
          </div>
        ))}
      </div>
    </div>
  )
}

export interface PageActions {
  run: (game: Game, action: Action, code?: string, address?: string, coopSave?: string) => void
  refresh: () => void
  modOp: (game: Game, op: 'install' | 'repair' | 'uninstall') => void
  openLobby: (game: Game, quick?: boolean) => void
  stopHosting: (session: number) => void
  download: (game: Game) => void
  locate: (game: Game, action: Action) => void
  chooseFolder: (game: Game) => void
  report: (game: Game) => void  // "How did it run?" asked from the page
}

// While this PC hosts with the built-in relay: where friends connect, who has, and Stop hosting.
function HostingCard({ status, session, onStop, colors }: { status: RelayStatus; session: number; onStop: (s: number) => void; colors: string[] }) {
  const [copied, setCopied] = useState('')
  const guests = status.peers.filter((p) => !p.host)
  return (
    <div className="card pinned">
      <div className="card-h"><h3>Hosting</h3><span>Port {status.port}</span></div>
      <div className="dl">
        {status.addresses.slice(0, 3).map((a) => {
          const addr = `${a.address}:${status.port}`
          return (
            <div key={addr}>
              <dt>{a.adapter}</dt>
              <dd><button className="linkbtn" onClick={() => copyText(addr).then(() => setCopied(addr))}>{copied === addr ? 'Copied' : addr}</button></dd>
            </div>
          )
        })}
      </div>
      <p className={guests.length ? 'joined' : 'waiting'}>
        {guests.length
          ? guests.map((p, i) => <span key={i}><i style={{ background: colors[(i + 1) % colors.length] }} />{p.name || p.address}</span>)
          : 'Waiting for friends to join...'}
      </p>
      <button className="btn danger" onClick={() => onStop(session)}>Stop hosting</button>
    </div>
  )
}

// Tabs as the C++ details panel draws them: the chosen one in full text with a line under it.
function Tabs<T extends string>({ tabs, value, onChange }: { tabs: { id: T; name: string }[]; value: T; onChange: (t: T) => void }) {
  return (
    <div className="ptabs" role="tablist">
      {tabs.map((t) => (
        <button key={t.id} role="tab" aria-selected={t.id === value} className={t.id === value ? 'on' : ''} onClick={() => onChange(t.id)}>{t.name}</button>
      ))}
    </div>
  )
}

type Tab = 'overview' | 'guide' | 'versions' | 'rooms'

export function GamePage({
  game, lib, mods, playing, modLine, downloading, hosting, favorite, onFavorite, onBack, actions, askDownload,
}: {
  game: Game
  lib: Library
  mods: Record<string, ModInfo> | null
  playing: boolean
  modLine?: string
  downloading: boolean
  hosting?: { session: number; relay: RelayStatus }
  favorite: boolean
  onFavorite: () => void
  onBack: () => void
  actions: PageActions
  askDownload?: boolean  // opened by Play on a game whose file is missing: offer the download at once
}) {
  const [tab, setTab] = useState<Tab>('overview')
  const [showMore, setShowMore] = useState(false)
  const [dialog, setDialog] = useState<'host' | 'join' | 'uninstall' | 'download' | null>(askDownload ? 'download' : null)
  // The host's co-op save (0 continue, 1 from the solo save, 2 new game) and the game's own settings, as changed here.
  const [saveChoice, setSaveChoice] = useState(0)
  const [setValues, setSetValues] = useState<Record<string, string>>({})
  const [rooms, setRooms] = useState<Room[] | null>(null)
  const t = (k: string, fallback: string, vars: Record<string, string> = {}) =>
    Object.entries(vars).reduce((x, [key, v]) => x.replace('${' + key + '}', v), lib.text[k] ?? fallback)
  const backBtn = useRef<HTMLButtonElement>(null)
  const title = useDecode(game.name)
  const mod = mods?.[game.id]
  const line = stateLine(game, lib, mods, playing)
  const cats = lib.categories.filter((c) => game.categories.includes(c.id)).map((c) => c.name)
  const colors = lib.branding.playerColors
  const coop = isCoop(game) && !game.lobby
  const main = mainActions(game, lib.user)
  const more = moreActions(game, lib.user)
  const busy = modLine !== undefined
  const fileAct = fileAction(game)
  const fileMissing = !game.mod && !game.comingSoon && fileAct?.state === 'noFile'
  const playerMissing = !game.mod && game.actions.some((a) => a.state === 'noPlayer') && !game.actions.some((a) => a.state === 'ready')
  const modName = game.mod?.gameName || game.name
  const u = lib.user.games[game.id]
  const saveArg = (a: Action) => (a.coopHost && saveChoice > 0 && game.coopSave?.supported ? (saveChoice === 1 ? 'solo' : 'new') : undefined)
  // A missing game file is offered for download first, as the C++ window does when Play is pressed.
  const run = (a: Action, code?: string, address?: string) => {
    if (a.state === 'noFile') {
      setDialog('download')
      return
    }
    actions.run(game, a, code, address, saveArg(a))
  }
  const online = !!game.onlineId && !game.comingSoon && !game.mod
  // Quick play: the busiest open room with space, or host one when nobody is playing.
  useEffect(() => {
    if (!online || !lib.user.online) return
    let alive = true
    loadRooms(game.id).then((r) => alive && setRooms(r.rooms)).catch(() => alive && setRooms([]))
    return () => {
      alive = false
    }
  }, [game.id, online, lib.user.online])
  const busiest = (rooms ?? []).filter((r) => r.max <= 0 || r.players < r.max).sort((a, b) => b.players - a.players)[0]
  const quickPlay = () => {
    if (game.lobby) return actions.openLobby(game, true)
    const byCode = joinByCode(game)
    if (busiest && byCode) return run(byCode, busiest.code)
    const host = game.actions.find((a) => a.id === game.joinAction) ?? byCode
    if (host) run(host)
  }
  const foundPlayers = lib.players.filter((p) => p.found)
  const autoPlayer = foundPlayers[0]?.name
  const settingValue = (id: string, value: string) => setValues[id] ?? value
  const changeSetting = (id: string, value: string) => {
    setSetValues((v) => ({ ...v, [id]: value }))
    setGameSetting(game.id, id, value).catch(() => {})
  }
  const community = game.site?.community
  const creditUrl = game.site?.authorUrl || (game.site?.repo ? `https://github.com/${game.site.repo}` : '')
  const hasGuide = game.guide.steps.length > 0 || game.guide.needs.length > 0 || game.guide.tips.length > 0

  useEffect(() => {
    backBtn.current?.focus()
  }, [])

  // The C++ panel's buttons for a PC mod: find the game, install, update, launch, repair, uninstall.
  let modButtons: ReactNode = null
  if (game.mod && !game.comingSoon) {
    if (!mod) modButtons = <PanelButton label="Checking..." sub="Asking mods-info for the newest version" disabled />
    else if (busy) modButtons = <PanelButton primary label="Working..." sub={modLine || 'Starting'} disabled />
    else if (mod.running) modButtons = <PanelButton label={t('mod.status.running', 'Close ${GAME_NAME} first', { GAME_NAME: modName })} sub="The mod can't change while the game is open" warn disabled />
    else if (!mod.gameFound)
      modButtons = (
        <>
          <PanelButton icon="E896" label={`Install ${modName}`} sub={`${modName} wasn't found on this PC`} warn disabled />
          <PanelButton primary icon="E8B7" label="Choose the game folder" sub={`Where ${modName} is installed`} onClick={() => actions.chooseFolder(game)} />
        </>
      )
    else if (!mod.installed && mod.missing)
      // The loader this mod needs (install.requires) isn't in the game folder: the launcher can't install it.
      modButtons = <PanelButton icon="E896" label={`Install ${modName}`} sub={mod.missing} warn disabled />
    else if (!mod.installed)
      modButtons = (
        <PanelButton primary icon="E896" label={`Install ${modName}`} disabled={!mod.published}
          sub={mod.published ? `Version ${mod.newest}, into your ${modName} folder` : mod.error || 'Not published yet'}
          onClick={() => actions.modOp(game, 'install')} />
      )
    else {
      const update = mod.published && newer(mod.newest, mod.installedVersion)
      const launchAct = game.actions.find((a) => a.mod === 'launch')
      modButtons = (
        <>
          {update && <PanelButton primary icon="E777" label={`Update to ${mod.newest}`} sub={`You have ${mod.installedVersion}`} onClick={() => actions.modOp(game, 'install')} />}
          {launchAct && <PanelButton primary={!update} icon="E768" label="Launch game" sub={`${game.mod.modName || game.name} ${mod.installedVersion} is installed`} onClick={() => run(launchAct)} />}
          <PanelButton icon="E90F" label="Repair" sub="Put back missing or damaged files" onClick={() => actions.modOp(game, 'repair')} />
          <PanelButton icon="E74D" label="Uninstall" sub="Restore the files the mod replaced" onClick={() => setDialog('uninstall')} />
        </>
      )
    }
  }

  const tabs: { id: Tab; name: string }[] = [
    { id: 'overview', name: 'Overview' },
    ...(hasGuide ? [{ id: 'guide' as Tab, name: 'How to play' }] : []),
    ...(game.mod ? [{ id: 'versions' as Tab, name: 'Releases' }] : []),
    ...(game.onlineId ? [{ id: 'rooms' as Tab, name: 'Rooms' }] : []),
  ]
  const g = game.guide
  const stats: [string, string][] = [
    ['Played', !u?.plays ? 'Not yet' : u.seconds >= 3600 ? `${Math.floor(u.seconds / 3600)} h ${Math.round((u.seconds % 3600) / 60)} min` : u.seconds >= 60 ? `${Math.round(u.seconds / 60)} min` : '< 1 min'],
    ['Last played', lastPlayed(lib, game.id) ? agoText(lastPlayed(lib, game.id)) : 'Never'],
    ['Players', game.players.replace(/ \(.*\)$/, '')],
    game.mod ? ['Version', mod?.installedVersion || game.mod.pinnedVersion || mod?.newest || 'Unknown'] : ['Times played', String(u?.plays ?? 0)],
  ]
  const guests = hosting ? hosting.relay.peers.filter((p) => !p.host).map((p) => p.name || p.address) : []
  const folder = mod?.gameDir || lib.gamesDir

  return (
    <div className="gp">
      <div className="gp-amb"><Cover game={game} className="fill" /></div>
      <div className="gp-hero">
        <Cover game={game} className="fill" />
        <button ref={backBtn} className="btn glass gp-back" onClick={onBack}><Glyph code="E72B" size={17} />Back</button>
      </div>
      <div className="gp-top">
        <div className="gp-cover"><Cover game={game} className="fill" /></div>
        <div className="gp-title">
          <h1 aria-label={game.name}>{title}</h1>
          {cats.length > 0 && <span className="cats">{cats.join(', ')}</span>}
          {community && game.site && (
            <span className="credit">
              <span>Community mod by</span>
              {creditUrl
                ? <button className="linkbtn" onClick={() => open(creditUrl)}>{game.site.author || game.site.repo}</button>
                : <b>{game.site.author || 'its author'}</b>}
              <span className="muted">{game.site.reviewed ? 'We read its code.' : 'Not reviewed by us.'}</span>
            </span>
          )}
          {game.mod && <span className="cats">Mod for {modName}, by {game.mod.publisher || 'Multiplayer Everything'}</span>}
        </div>
      </div>
      {busy && <div className="gp-prog"><span className="bar"><i /></span>{modLine}</div>}

      <div className="g-body">
        <div className="stack">
          <Tabs tabs={tabs} value={tab} onChange={setTab} />
          <div className="stack" key={tab}>
            {tab === 'overview' && (
              <>
                <div className="stats">{stats.map(([k, v]) => <div key={k}><span>{k}</span><b>{v}</b></div>)}</div>
                <p className="blurb">{game.description}</p>
                {!game.comingSoon && lib.user.online && <HowItRuns game={game} onReport={u?.plays ? () => actions.report(game) : undefined} />}
                {g.steps.length > 0 && (
                  <div className="card">
                    <h3>How to play</h3>
                    <ol className="steps">{g.steps.slice(0, 3).map((s) => <li key={s}>{s}</li>)}</ol>
                    {g.steps.length > 3 && <button className="linkbtn" onClick={() => setTab('guide')}>All the steps</button>}
                  </div>
                )}
              </>
            )}
            {tab === 'guide' && (
              <>
                {g.needs.length > 0 && <div className="card"><h3>What you need</h3><ul className="steps">{g.needs.map((s) => <li key={s}>{s}</li>)}</ul></div>}
                {g.steps.length > 0 && <div className="card"><h3>How to play</h3><ol className="steps">{g.steps.map((s) => <li key={s}>{s}</li>)}</ol></div>}
                {g.tips.length > 0 && <div className="card"><h3>Tips</h3><ul className="steps">{g.tips.map((s) => <li key={s}>{s}</li>)}</ul></div>}
                {g.links.length > 0 && <div className="acts">{g.links.map((l) => <button key={l.url} className="btn" onClick={() => open(l.url)}><Glyph code="E8A7" size={16} />{l.label}</button>)}</div>}
              </>
            )}
            {tab === 'versions' && (
              <div className="card">
                <h3>Releases</h3>
                {!(game.mod?.pinnedVersion || mod?.newest) ? (
                  <p className="muted">{mod?.error || 'The versions show once mods-info answers.'}</p>
                ) : (
                  <div className="dl">
                    <div><dt>Newest</dt><dd>{game.mod?.pinnedVersion || mod?.newest} <span className="verified">Verified</span></dd></div>
                    <div><dt>On this PC</dt><dd>{mod?.installed ? mod.installedVersion : 'Not installed'}</dd></div>
                    {mod?.notice && <div><dt>Note</dt><dd>{mod.notice}</dd></div>}
                    {game.mod?.website && <div><dt>Source</dt><dd><button className="linkbtn" onClick={() => open(game.mod!.website)}>{game.mod.website.replace(/^https?:\/\//, '')}</button></dd></div>}
                  </div>
                )}
              </div>
            )}
            {tab === 'rooms' && <RoomsTab game={game} text={lib.text} onJoinRoom={(code) => { const a = joinByCode(game); if (a) run(a, code) }} />}
          </div>
        </div>

        <aside className="dpanel" aria-label="Play">
          <Slots n={game.maxPlayers || 2} colors={colors} names={hosting ? ['You', ...guests] : undefined} />
          <span className={`spill tone-${line.tone}`}><i />{line.text}</span>
          <SiteBlock game={game} lib={lib} />
          {playerMissing && <p className="pnote">Ruffle Together isn't installed yet. The launcher installs it on its first start.</p>}
          {hosting && <HostingCard status={hosting.relay} session={hosting.session} onStop={actions.stopHosting} colors={colors} />}
          <div className="pacts">
            {game.comingSoon && <p className="pnote">Not playable yet: this mod is still being built.</p>}
            {fileMissing && fileAct && (
              <>
                <PanelButton primary icon="E896" label={downloading ? 'Downloading...' : 'Download game files'} disabled={downloading}
                  sub="From the game catalog, checked by SHA-256" onClick={() => actions.download(game)} />
                <PanelButton icon="E8E5" label="Locate the game file" sub="A copy you already have" onClick={() => actions.locate(game, fileAct)} />
              </>
            )}
            {game.outdated && !fileMissing && (
              <PanelButton icon="E896" label={downloading ? 'Downloading...' : 'Download the update'} disabled={downloading}
                sub="A newer build of the game file, checked by SHA-256" onClick={() => actions.download(game)} />
            )}
            {!game.comingSoon && game.lobby && (
              <PanelButton primary={!fileMissing} icon="E774" label="Play online" sub="Host a room or join one" onClick={() => actions.openLobby(game)} />
            )}
            {!game.comingSoon && coop && game.coopSave && (
              <div className="psel">
                <label htmlFor="coop-save">{t('coopSave.label', 'Co-op save')}</label>
                <select id="coop-save" className="sel" disabled={!game.coopSave.supported} value={saveChoice} onChange={(e) => setSaveChoice(Number(e.target.value))}>
                  <option value={0}>{t('coopSave.continue', 'Continue co-op')}</option>
                  <option value={1}>{t('coopSave.solo', 'Start from my solo save')}</option>
                  <option value={2}>{t('coopSave.new', 'New game')}</option>
                </select>
                <small className={saveChoice > 0 ? 'tone-warn' : ''}>
                  {!game.coopSave.supported ? t('coopSave.needs', 'Needs Ruffle Together ${VERSION} or newer.', { VERSION: game.coopSave.since })
                    : saveChoice > 0 ? t('coopSave.replaces', 'This replaces your co-op progress for ${GAME} the first time the game saves.', { GAME: game.name })
                    : game.coopSave.progress ? t('coopSave.progress', 'Co-op progress saved.') : ''}
                </small>
              </div>
            )}
            {!game.comingSoon && coop && (
              <>
                <PanelButton primary={!fileMissing} icon="E774" label="Host a game" onClick={() => setDialog('host')} />
                <PanelButton icon="E774" label="Join a game" onClick={() => setDialog('join')} />
              </>
            )}
            {online && (coop || game.lobby || !!joinByCode(game)) && (
              <PanelButton icon="E945" label={t('button.quickPlay', 'Quick play')}
                sub={rooms === null ? 'Looking for open rooms...' : busiest ? t('button.quickJoin', 'Join the busiest open room') : t('button.quickHost', 'No rooms open: host one')}
                onClick={quickPlay} />
            )}
            {!game.comingSoon && !game.mod && main.filter((a) => !(game.lobby && a.id === game.joinAction)).map((a, i) => (
              <PanelButton key={a.id} primary={!coop && !game.lobby && i === 0 && a.state === 'ready'} icon={a.icon} label={a.label}
                sub={a.state !== 'ready' ? (a.state === 'noFile' ? 'Game file missing' : a.state === 'noPlayer' ? 'No Flash player found' : 'Server program missing') : a.description || (a.player ?? '')}
                warn={a.state !== 'ready'} onClick={() => run(a)} />
            ))}
            {!game.comingSoon && more.length > 0 && (
              <>
                <button className="pmore" aria-expanded={showMore} onClick={() => setShowMore(!showMore)}>
                  <Glyph code={showMore ? 'E70E' : 'E70D'} />{showMore ? 'Fewer ways to connect' : 'Other ways to connect'}
                </button>
                {showMore && more.map((a) => (
                  <PanelButton key={a.id} icon={a.icon} label={a.label} sub={a.description} onClick={() => (a.asksAddress ? setDialog('join') : run(a))} />
                ))}
              </>
            )}
            {modButtons}
            {game.actions.filter((a) => a.link).map((a) => (
              <button className="pbtn two link" key={a.id} onClick={() => open(a.link)}>
                <span className="top"><span>{a.label}</span></span>
                {a.description && <span className="sub">{a.description}</span>}
              </button>
            ))}
            {!game.comingSoon && !game.mod && game.settings.length > 0 && (
              <div className="psets">
                {game.settings.map((st) => st.type === 'checkbox' ? (
                  <div key={st.id} className={`pcheck ${st.enabled ? '' : 'off'}`}
                    title={st.enabled ? undefined : t('coopSave.needs', 'Needs Ruffle Together ${VERSION} or newer.', { VERSION: st.since })}>
                    <Switch on={settingValue(st.id, st.value) === '1'} label={st.label} onChange={(v) => st.enabled && changeSetting(st.id, v ? '1' : '0')} />
                    <span>{st.label}</span>
                  </div>
                ) : (
                  <div key={st.id} className="psel">
                    <label htmlFor={`set-${st.id}`}>{st.label}</label>
                    <select id={`set-${st.id}`} className="sel" disabled={!st.enabled} value={settingValue(st.id, st.value)} onChange={(e) => changeSetting(st.id, e.target.value)}>
                      {st.choices.map((c) => <option key={c.id} value={c.id}>{c.label}</option>)}
                    </select>
                  </div>
                ))}
              </div>
            )}
            {!game.comingSoon && !game.mod && foundPlayers.length > 1 && (
              <div className="psel">
                <label htmlFor="game-player">{t('detail.player', 'Player')}</label>
                <select id="game-player" className="sel" value={game.player} onChange={(e) => setGamePlayer(game.id, e.target.value).then(actions.refresh)}>
                  <option value="">{autoPlayer ? t('detail.playerAuto', 'Automatic (${PLAYER})', { PLAYER: autoPlayer }) : t('detail.playerAutoNone', 'Automatic')}</option>
                  {foundPlayers.map((pl) => <option key={pl.id} value={pl.id}>{pl.name}</option>)}
                </select>
              </div>
            )}
            <div className="prow">
              <button className={`pbtn ${favorite ? 'faved' : ''}`} aria-pressed={favorite} onClick={onFavorite}>
                <span className="top"><Glyph code={favorite ? 'E735' : 'E734'} /><span>Favorite</span></span>
              </button>
              {hasGuide && <button className="pbtn" onClick={() => setTab('guide')}><span className="top"><span>Guide</span></span></button>}
              <button className="pbtn" disabled={!folder} onClick={() => open(folder)}><span className="top"><span>Folder</span></span></button>
            </div>
          </div>
        </aside>
      </div>

      {dialog === 'host' && <HostDialog game={game} onClose={() => setDialog(null)} onHost={(a) => (setDialog(null), run(a))} />}
      {dialog === 'join' && <JoinDialog game={game} onClose={() => setDialog(null)} onJoin={(a, code, address) => (setDialog(null), run(a, code, address))} />}
      {dialog === 'download' && fileAct && (
        <Dialog title={t('download.title', 'Download ${GAME}?', { GAME: game.name })} onClose={() => setDialog(null)}>
          <p className="dialog-note">The game file isn't on this PC yet. The launcher downloads it from the game catalog and checks it against its published checksum, or you can point it at a copy you already have.</p>
          <div className="dialog-row">
            <button className="btn" onClick={() => setDialog(null)}>Cancel</button>
            <button className="btn" onClick={() => (setDialog(null), actions.locate(game, fileAct))}>Locate a copy</button>
            <button className="btn next" onClick={() => (setDialog(null), actions.download(game))}>{t('download.yes', 'Download')}</button>
          </div>
        </Dialog>
      )}
      {dialog === 'uninstall' && (
        <Dialog title={`Uninstall ${game.mod?.modName || game.name}?`} onClose={() => setDialog(null)}>
          <p className="dialog-note">The mod's files are removed and the game's own files it replaced are put back.</p>
          <div className="dialog-row">
            <button className="btn" onClick={() => setDialog(null)}>Cancel</button>
            <button className="btn main" onClick={() => (setDialog(null), actions.modOp(game, 'uninstall'))}>Uninstall</button>
          </div>
        </Dialog>
      )}
    </div>
  )
}
