// The first-run setup: what this launcher needs before someone can play with friends. The C++ launcher asks for a
// name and installs Ruffle Together quietly on its first start; here each of those is a step people can see, with
// the folders and the few choices that matter. Everything saves to the shared settings.json as it is chosen.
import { useEffect, useState, type ReactNode } from 'react'
import { installPlayer, live, open, pickFolder, setGameDir, setSetting, windowControl, type Library, type ModInfo, type Settings } from './api'
import { Glyph, Switch } from './ui'

type StepId = 'welcome' | 'name' | 'player' | 'pcgames' | 'folder' | 'choices' | 'done'

export function SetupPage({
  lib, mods, onChanged, onRefresh, onRefreshMods, onDone,
}: {
  lib: Library
  mods: Record<string, ModInfo> | null
  onChanged: (s: Partial<Settings>) => void
  onRefresh: () => Promise<void>
  onRefreshMods: () => Promise<unknown>
  onDone: () => void
}) {
  const u = lib.user
  const t = (k: string, fallback: string) => (lib.text[k] ?? fallback).replace(/…/g, '...')
  const colors = lib.branding.playerColors
  const pcGames = lib.games.filter((g) => g.mod && !g.comingSoon)
  const steps: StepId[] = ['welcome', 'name', 'player', ...(pcGames.length ? ['pcgames' as StepId] : []), 'folder', 'choices', 'done']
  const [at, setAt] = useState(0)
  const [dir, setDir] = useState<'fwd' | 'back'>('fwd')
  const [name, setName] = useState(u.playerName)
  const [error, setError] = useState('')
  const [installing, setInstalling] = useState(false)
  const step = steps[at]

  const save = (key: keyof Settings, value: string | boolean) =>
    setSetting(key, value).then(() => onChanged({ [key]: value })).catch((e) => setError(String(e)))

  const player = lib.players.find((p) => p.id === 'ruffle') ?? lib.players.find((p) => p.installable) ?? lib.players[0]
  const nameOk = name.trim().length > 0

  const go = (d: 1 | -1) => {
    setError('')
    if (step === 'name' && d === 1) {
      if (!nameOk) return
      if (name.trim() !== u.playerName) void save('playerName', name.trim())
    }
    if (step === 'done' && d === 1) return onDone()
    setDir(d === 1 ? 'fwd' : 'back')
    setAt((i) => Math.max(0, Math.min(steps.length - 1, i + d)))
  }

  // Enter moves on, as the Continue button does; Esc steps back.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Enter' && !(e.target instanceof HTMLButtonElement)) go(1)
      else if (e.key === 'Escape' && at > 0) go(-1)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  })

  const install = () => {
    if (!player) return
    setInstalling(true)
    setError('')
    installPlayer(player.id)
      .then(onRefresh)
      .catch((e) => setError(String(e)))
      .finally(() => setInstalling(false))
  }

  const chooseGame = async (id: string, gameName: string) => {
    const d = await pickFolder(`Where is ${gameName} installed?`)
    if (!d) return
    setGameDir(id, d).then(onRefreshMods).catch((e) => setError(String(e)))
  }

  const gamesDir = u.gamesDir || lib.defaultGamesDir || lib.gamesDir
  const row = (title: string, note: string, key: 'online' | 'autoUpdate' | 'minimize' | 'showOffline') => (
    <div className="set-row">
      <div className="k"><b>{title}</b><small>{note}</small></div>
      <div className="acts"><Switch on={u[key]} label={title} onChange={(v) => void save(key, v)} /></div>
    </div>
  )

  let body: ReactNode
  if (step === 'welcome') {
    body = (
      <>
        <Slots colors={colors} names={['P1', 'P2', 'P3', 'P4']} />
        <h1>Welcome to {lib.branding.title}</h1>
        <p className="lead">{lib.branding.subtitle ? `${lib.branding.subtitle}, played together.` : 'Games to play together.'} A minute of setup and you're in.</p>
        <div className="feats">
          <Feat icon={<Glyph code="E774" size={20} />} title="Online with friends" text="Host a room and send the five-letter code. No port forwarding, no VPN." />
          <Feat icon={<Glyph code="E7F4" size={20} />} title="Flash games" text="Classic Flash games, made multiplayer, running in Ruffle Together." />
          <Feat icon={<Glyph code="E7FC" size={20} />} title="PC game mods" text="Installed into your own copy of a game, and taken back out just as cleanly." />
        </div>
      </>
    )
  } else if (step === 'name') {
    body = (
      <>
        <h1>What should friends call you?</h1>
        <p className="lead">Shown to other players in rooms and in the game. You can change it in Settings.</p>
        <div className="namebox">
          <span className="pslot big" style={{ ['--p' as string]: colors[0] }}>{name.trim() || 'P1'}</span>
          <input className="field" autoFocus value={name} maxLength={24} placeholder="Your name" spellCheck={false}
            aria-label="Your name" onChange={(e) => setName(e.target.value)} />
        </div>
      </>
    )
  } else if (step === 'player') {
    body = (
      <>
        <h1>The Flash player</h1>
        <p className="lead">{player?.name ?? 'Ruffle Together'} runs every Flash game here, and it is what connects you to your friends.</p>
        <div className="card setup-card">
          <div className="set-row">
            <div className="k">
              <b>{player?.name ?? 'Ruffle Together'}</b>
              <small>{player?.found ? 'Installed and ready.' : installing ? 'Downloading and checking it...' : 'Not on this PC yet. It is a small download, checked before it is used.'}</small>
            </div>
            <div className="acts">
              {player?.found ? (
                <span className="tick ok" aria-label="Installed"><Glyph code="E73E" size={16} /></span>
              ) : (
                <button className="btn" disabled={installing || !player?.installable} onClick={install}>
                  {installing ? <><Glyph code="E895" size={16} className="spin" />Installing...</> : 'Install'}
                </button>
              )}
            </div>
          </div>
        </div>
        {!player?.found && <p className="hint">You can skip this: the launcher installs it the first time you play.</p>}
      </>
    )
  } else if (step === 'pcgames') {
    body = (
      <>
        <h1>Your PC games</h1>
        <p className="lead">Mods go into a copy of the game you own. These are the ones found on this PC.</p>
        <div className="card setup-card set-list">
          {pcGames.map((g) => {
            const m = mods?.[g.id]
            const gameName = g.mod?.gameName || g.name
            return (
              <div className="set-row" key={g.id}>
                <div className="k">
                  <b>{gameName}</b>
                  <small>{!m ? 'Looking for it...' : m.gameFound ? m.gameDir : 'Not found on this PC'}</small>
                </div>
                <div className="acts">
                  {m?.gameFound ? (
                    <span className="tick ok" aria-label="Found"><Glyph code="E73E" size={16} /></span>
                  ) : (
                    <button className="btn sm" disabled={!m} onClick={() => chooseGame(g.id, gameName)}>Choose folder</button>
                  )}
                </div>
              </div>
            )
          })}
        </div>
        <p className="hint">Don't have one? Skip it. Each game's page can find it later.</p>
      </>
    )
  } else if (step === 'folder') {
    body = (
      <>
        <h1>Where should game files go?</h1>
        <p className="lead">Flash games download here the first time you play them.</p>
        <div className="card setup-card">
          <div className="set-row">
            <Glyph code="E838" size={20} className="muted" />
            <div className="k"><b className="path">{gamesDir}</b></div>
            <div className="acts">
              <button className="btn sm" onClick={async () => {
                const d = await pickFolder(t('settings.pickFolder', 'Choose the folder for game files'), gamesDir)
                if (d) void save('gamesDir', d)
              }}>{t('settings.change', 'Change...')}</button>
              <button className="btn sm icon" aria-label="Open the folder" title="Open the folder" disabled={!gamesDir} onClick={() => open(gamesDir)}><Glyph code="E838" size={16} /></button>
            </div>
          </div>
        </div>
      </>
    )
  } else if (step === 'choices') {
    body = (
      <>
        <h1>A few choices</h1>
        <p className="lead">All of these are in Settings later.</p>
        <div className="card setup-card set-list">
          {row(t('settings.onlineToggle', 'Show open rooms and check for game updates'), 'Asks the central server for open rooms, and GitHub for new versions.', 'online')}
          {row(t('settings.autoUpdate', 'Update the launcher automatically'), 'New versions install themselves when you close the launcher.', 'autoUpdate')}
          {row(t('settings.minimize', 'Minimize the launcher while a game runs'), 'It comes back when the game closes.', 'minimize')}
          {row(t('settings.offline', 'Show offline play (solo and one keyboard)'), 'Adds buttons to play alone or two on one keyboard.', 'showOffline')}
        </div>
      </>
    )
  } else {
    body = (
      <>
        <Slots colors={colors} names={[u.playerName || name || 'P1', 'P2', 'P3', 'P4']} />
        <h1>You're all set</h1>
        <p className="lead">Pick a game, host a room and send the code to a friend.</p>
      </>
    )
  }

  return (
    <div className="setup">
      <div className="setup-top" data-tauri-drag-region>
        <span className="word"><b>{lib.branding.title}</b></span>
        {live && (
          <div className="win">
            <button aria-label="Minimize" onClick={() => windowControl('minimize')}><Glyph code="E921" size={16} /></button>
            <button aria-label="Maximize" onClick={() => windowControl('maximize')}><Glyph code="E922" size={13} /></button>
            <button className="close" aria-label="Close" onClick={() => windowControl('close')}><Glyph code="E8BB" size={17} /></button>
          </div>
        )}
      </div>
      <div className={`setup-body ${dir}`} key={step}>
        {body}
        {error && <p className="error-text">{error}</p>}
      </div>
      <div className="setup-foot">
        <div className="slot-l">{at > 0 && <button className="btn" onClick={() => go(-1)}>Back</button>}</div>
        <div className="dots" aria-label={`Step ${at + 1} of ${steps.length}`}>
          {steps.map((s, i) => <i key={s} className={i === at ? 'on' : i < at ? 'past' : ''} />)}
        </div>
        <div className="slot-r">
          {step === 'done' ? (
            <button className="btn main big" onClick={onDone}>Start playing</button>
          ) : (
            <button className="btn next" disabled={step === 'name' && !nameOk} onClick={() => go(1)}>
              {step === 'player' && !player?.found && !installing ? 'Skip for now' : 'Continue'}
            </button>
          )}
        </div>
      </div>
    </div>
  )
}

function Feat({ icon, title, text }: { icon: ReactNode; title: string; text: string }) {
  return (
    <div className="feat-card">
      <span className="ic">{icon}</span>
      <b>{title}</b>
      <span>{text}</span>
    </div>
  )
}

// The player slots, the launcher's one bold element, in place of a logo.
function Slots({ colors, names }: { colors: string[]; names: string[] }) {
  return (
    <div className="setup-slots" aria-hidden="true">
      {names.map((n, i) => <span key={i} className="pslot big" style={{ ['--p' as string]: colors[i % colors.length], animationDelay: `${i * 70}ms` }}>{n}</span>)}
    </div>
  )
}
