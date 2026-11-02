// A game's lobby: host or join a room, see who is in it, chat, get ready, and the host starts; the C++ launcher runs
// the room (--lobby-session) and opens the game for each player when it starts.
import { useEffect, useRef, useState } from 'react'
import {
  copyText, loadRooms, lobbySend, lobbyStart, onLobby, type Game, type Library, type LobbyRequest, type LobbySnapshot,
} from './api'
import { Glyph, Seg, Switch } from './ui'

export function LobbyPage({ game, lib, quick, onClose }: { game: Game; lib: Library; quick?: boolean; onClose: () => void }) {
  const t = (k: string, fallback: string, vars: Record<string, string> = {}) =>
    Object.entries(vars).reduce((s, [key, v]) => s.replace('${' + key + '}', v), lib.text[k] ?? fallback)
  const [room, setRoom] = useState<LobbySnapshot | null>(null)
  const [open, setOpen] = useState(false)  // a session is running
  const [note, setNote] = useState('')
  const [tab, setTab] = useState<'host' | 'join'>('host')
  const [direct, setDirect] = useState(false)
  const [roomName, setRoomName] = useState(`${lib.user.playerName || 'My'}'s room`)
  const [maxPlayers, setMaxPlayers] = useState(Math.max(2, game.maxPlayers || 4))
  const [isPublic, setPublic] = useState(true)
  const [password, setPassword] = useState('')
  const [port, setPort] = useState(7790)
  const [target, setTarget] = useState('')
  const [chat, setChat] = useState('')
  const chatEnd = useRef<HTMLDivElement>(null)
  const colors = lib.branding.playerColors

  useEffect(() => {
    const off = onLobby((e) => {
      if (e.game !== game.id) return
      const m = e.message
      if (m.t === 'snapshot') {
        setRoom(m)
        if (m.reason) setNote(m.reason)
      } else if (m.t === 'error') setNote(m.message)
      else if (m.t === 'game') setNote(m.state === 'started' ? 'The game is starting...' : m.state === 'exited' ? 'Back in the room.' : m.message || 'The game could not start.')
      else if (m.t === 'closed') {
        if (m.reason) setNote(m.reason)
        setRoom(null)
      } else if (m.t === 'ended') {
        setOpen(false)
        setRoom(null)
      }
    })
    return () => {
      off.then((f) => f())
    }
  }, [game.id])

  // Leaving the page leaves the room.
  useEffect(() => () => void lobbySend({ op: 'leave' }).catch(() => {}), [])

  // Braced: scrollIntoView returns a Promise in current WebView2, and an effect may only return its cleanup.
  useEffect(() => {
    chatEnd.current?.scrollIntoView({ block: 'nearest' })
  }, [room?.chat.length])

  const start = (request: LobbyRequest) => {
    setNote('')
    lobbyStart(game.id, request)
      .then(() => setOpen(true))
      .catch((e) => setNote(String(e)))
  }
  const host = () => start({ op: 'host', direct, roomName: roomName.trim(), password, public: isPublic, maxPlayers, port })

  // Quick play, as the C++ lobby does it: the busiest public room with space, or a new room when there is none.
  const quickDone = useRef(false)
  useEffect(() => {
    if (!quick || quickDone.current) return
    quickDone.current = true
    loadRooms(game.id)
      .then((r) => r.rooms.filter((x) => !x.locked && (x.max <= 0 || x.players < x.max)).sort((a, b) => b.players - a.players)[0])
      .catch(() => undefined)
      .then((busiest) => (busiest ? start({ op: 'join', direct: false, code: busiest.code, password: '' })
        : start({ op: 'host', direct: false, roomName: `${lib.user.playerName || 'My'}'s room`, password: '', public: true, maxPlayers: Math.max(2, game.maxPlayers || 4), port: 7790 })))
  })
  const join = () => {
    const code = target.replace(/[^a-z0-9]/gi, '').toUpperCase()
    if (direct) start({ op: 'join', direct: true, address: target.trim(), password })
    else if (code.length !== 5) setNote(t('lobby.badCode', 'A room code has five letters and digits.'))
    else start({ op: 'join', direct: false, code, password })
  }
  const send = () => {
    const text = chat.trim()
    if (!text) return
    lobbySend({ op: 'chat', text }).catch((e) => setNote(String(e)))
    setChat('')
  }

  const me = room?.players.find((p) => p.you)
  const everyoneReady = !!room && room.players.filter((p) => p.connected && !p.host).every((p) => p.ready) && room.players.length > 1

  return (
    <div className="page narrow">
      <div><button className="btn glass" onClick={() => { if (open) void lobbySend({ op: 'leave' }); onClose() }}><Glyph code="E72B" size={17} />{open ? 'Leave room' : 'Back'}</button></div>
      <div className="head">
        <div>
          <h1>{t('lobby.title', 'Play ${GAME} online', { GAME: game.name })}</h1>
          {t('lobby.subtitle', '') && <p>{t('lobby.subtitle', '')}</p>}
        </div>
      </div>

      {!open && (
        <>
          <Seg label="Host or join" value={tab} onChange={setTab}
            options={[{ id: 'host', name: t('lobby.hostHead', 'Host a room') }, { id: 'join', name: t('lobby.joinHead', 'Join a room') }]} />
          <div className="card set-list">
            <div className="set-row">
              <div className="k"><b>{t('lobby.direct', 'Direct IP (Radmin VPN, ZeroTier, LAN) instead of the server')}</b></div>
              <div className="acts"><Switch on={direct} label="Direct IP" onChange={setDirect} /></div>
            </div>
            {tab === 'host' ? (
              <>
                <div className="set-row"><div className="k"><b>{t('lobby.roomName', 'Room name')}</b></div><div className="acts"><input className="field" value={roomName} maxLength={40} onChange={(e) => setRoomName(e.target.value)} /></div></div>
                <div className="set-row">
                  <div className="k"><b>{t('lobby.players', 'Players')}</b></div>
                  <div className="acts">
                    <select className="sel" value={maxPlayers} onChange={(e) => setMaxPlayers(Number(e.target.value))}>
                      {Array.from({ length: Math.max(1, (game.maxPlayers || 4) - 1) }, (_, i) => i + 2).map((n) => <option key={n} value={n}>{n}</option>)}
                    </select>
                  </div>
                </div>
                {!direct && (
                  <div className="set-row"><div className="k"><b>{t('lobby.public', 'List it in Open rooms')}</b></div><div className="acts"><Switch on={isPublic} label="Public" onChange={setPublic} /></div></div>
                )}
                <div className="set-row"><div className="k"><b>{t('lobby.password', 'Password (optional)')}</b></div><div className="acts"><input className="field" type="password" value={password} onChange={(e) => setPassword(e.target.value)} /></div></div>
                {direct && (
                  <div className="set-row"><div className="k"><b>{t('lobby.port', 'Port')}</b></div><div className="acts"><input className="field short" type="number" min={1} max={65535} value={port} onChange={(e) => setPort(Number(e.target.value))} /></div></div>
                )}
              </>
            ) : (
              <>
                <div className="set-row">
                  <div className="k"><b>{direct ? t('lobby.address', "Host's address") : t('lobby.code', 'Room code')}</b></div>
                  <div className="acts"><input className="field" value={target} onChange={(e) => setTarget(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && join()} placeholder={direct ? '192.168.1.20:7790' : 'ABCDE'} spellCheck={false} /></div>
                </div>
                <div className="set-row"><div className="k"><b>{t('lobby.joinPassword', 'Password, if it has one')}</b></div><div className="acts"><input className="field" type="password" value={password} onChange={(e) => setPassword(e.target.value)} /></div></div>
              </>
            )}
          </div>
          <div className="acts">
            {tab === 'host'
              ? <button className="btn main big" onClick={host}>{t('lobby.host', 'Host room')}</button>
              : <button className="btn main big" onClick={join}>Join room</button>}
          </div>
        </>
      )}

      {open && !room && <div className="loading">Connecting...</div>}

      {room && (
        <div className="lobby-room">
          <div className="card">
            <div className="room-head">
              <div>
                <span className="kick">{room.direct ? 'Direct IP' : 'Room code'}</span>
                <div className="room-code">{room.code || '...'}</div>
                <small className="muted">{room.roomName}{room.reconnecting ? '  (reconnecting...)' : ''}</small>
              </div>
              {room.code && <button className="btn sm" onClick={() => copyText(room.code).then(() => setNote('Copied.'))}><Glyph code="E8C8" size={15} />Copy</button>}
            </div>
            <div className="lobby-slots">
              {Array.from({ length: room.maxPlayers }, (_, slot) => {
                const p = room.players.find((x) => x.slot === slot)
                return (
                  <div key={slot} className={`lobby-slot ${p ? 'taken' : 'open'}`} style={{ ['--p' as string]: colors[slot % colors.length] }}>
                    <span className="slot-tag">P{slot + 1}</span>
                    <span className="slot-name">{p ? `${p.name}${p.you ? ' (you)' : ''}` : 'Open'}</span>
                    {p && <span className={`slot-state ${p.ready || p.host ? 'ready' : ''}`}>{p.host ? 'Host' : !p.connected ? 'Dropped' : p.ready ? 'Ready' : 'Not ready'}{p.ping >= 0 ? `  ${p.ping} ms` : ''}</span>}
                    {p && room.host && !p.you && <button className="btn sm" onClick={() => lobbySend({ op: 'kick', slot, ban: false })}>Kick</button>}
                  </div>
                )
              })}
            </div>
            <div className="acts">
              {room.host ? (
                <button className="btn main big" disabled={!everyoneReady || room.phase !== 'lobby'} onClick={() => lobbySend({ op: 'start' })}>{t('lobby.start', 'Start game')}</button>
              ) : (
                <button className={`btn big ${me?.ready ? '' : 'main'}`} disabled={room.phase !== 'lobby'} onClick={() => lobbySend({ op: 'ready', v: !me?.ready })}>
                  {me?.ready ? 'Not ready' : t('lobby.ready', 'Ready')}
                </button>
              )}
            </div>
          </div>
          <div className="card lobby-chat">
            <div className="chat-lines">
              {room.chat.map((c, i) => (
                <div key={i} className={c.slot < 0 ? 'chat-system' : ''}>{c.slot >= 0 && <b style={{ color: colors[c.slot % colors.length] }}>{c.name}: </b>}{c.text}</div>
              ))}
              <div ref={chatEnd} />
            </div>
            <div className="chat-input">
              <input className="field" value={chat} maxLength={200} placeholder="Say something" onChange={(e) => setChat(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && send()} />
              <button className="btn sm icon" aria-label="Send" onClick={send}><Glyph code="E724" size={15} /></button>
            </div>
          </div>
        </div>
      )}
      {note && <p className="lobby-note">{note}</p>}
    </div>
  )
}
