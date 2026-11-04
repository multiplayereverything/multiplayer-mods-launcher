// Settings, as the C++ Settings window has them: each change is saved at once to the launcher's own settings.json,
// so both windows agree.
import { useState, type ReactNode } from 'react'
import { TV_LOG, copyText, open, pickFolder, setSetting, type Library, type Settings } from './api'
import { Glyph, Switch } from './ui'

function Row({ title, note, children }: { title: string; note?: string; children: ReactNode }) {
  return (
    <div className="set-row">
      <div className="k"><b>{title}</b>{note && <small>{note}</small>}</div>
      <div className="acts">{children}</div>
    </div>
  )
}

// What the C++ lobby's Copy diagnostics gives a bug report: versions, folders, the server, the players.
function diagnostics(lib: Library, server: string): string {
  const win = /Windows NT ([\d.]+)/.exec(navigator.userAgent)?.[1]
  const linux = !win && /Linux/.test(navigator.userAgent)
  return [
    `Multiplayer Everything Launcher ${lib.launcherVersion} (Tauri window)`,
    linux ? 'Linux' : `Windows NT ${win ?? 'unknown'}`,
    `Config: ${lib.configDir}`,
    `Data: ${lib.dataDir}`,
    `Online features: ${lib.user.online ? 'on' : 'off'}, name set: ${lib.user.playerName ? 'yes' : 'no'}`,
    `Server: ${lib.roomsServer || 'none'}, ${server}`,
    'Players:',
    ...lib.players.map((p) => `  ${p.name}: ${p.found ? `${p.exe ?? 'found'}${p.version ? ` (${p.version})` : ''}` : 'not found'}`),
    'TV mode, latest last:',
    ...tvLog().map((l) => `  ${l}`),
  ].join('\n')
}

function tvLog(): string[] {
  try {
    return JSON.parse(localStorage.getItem(TV_LOG) ?? '[]')
  } catch {
    return []
  }
}

export function SettingsPage({ lib, onChanged, onRunSetup, server = '', tv = false, onTv }: {
  lib: Library; onChanged: (s: Partial<Settings>) => void; onRunSetup: () => void; server?: string; tv?: boolean; onTv?: () => void
}) {
  const t = (k: string, fallback: string) => (lib.text[k] ?? fallback).replace('${VERSION}', lib.launcherVersion).replace(/…|…/g, '...')
  const u = lib.user
  const [name, setName] = useState(u.playerName)
  const [error, setError] = useState('')
  const [copied, setCopied] = useState(false)

  const save = (key: keyof Settings, value: string | boolean) => {
    setError('')
    setSetting(key, value)
      .then(() => onChanged({ [key]: value }))
      .catch((e) => setError(String(e)))
  }
  const sw = (key: 'minimize' | 'online' | 'showOffline' | 'showOriginals' | 'autoUpdate', label: string, note?: string) => (
    <Row title={label} note={note}><Switch on={u[key]} label={label} onChange={(v) => save(key, v)} /></Row>
  )
  const commitName = () => {
    const v = name.trim()
    if (v !== u.playerName) save('playerName', v)
  }
  const gamesDir = u.gamesDir || lib.defaultGamesDir || lib.gamesDir
  const logDir = lib.logFile.replace(/[\\/][^\\/]*$/, '')

  return (
    <div className="page narrow">
      <div className="head">
        <div>
          <h1>{t('settings.title', 'Settings')}</h1>
          <p>Changes save straight away, and the classic launcher sees them too.</p>
        </div>
      </div>

      <section className="set-group">
        <h2>{t('settings.playing', 'Playing')}</h2>
        <div className="card set-list">
          {sw('minimize', t('settings.minimize', 'Minimize the launcher while a game runs'))}
          <Row title={t('settings.player', 'Default player')}>
            <select className="sel" value={u.defaultPlayer} onChange={(e) => save('defaultPlayer', e.target.value)}>
              <option value="">Automatic</option>
              {lib.players.map((p) => <option key={p.id} value={p.id}>{p.name}{p.found ? '' : '  (not found)'}</option>)}
            </select>
          </Row>
          {sw('showOffline', t('settings.offline', 'Show offline play (solo and one keyboard)'))}
          {sw('showOriginals', t('settings.originals', 'Show the original versions of modded games'))}
          {onTv && (
            <Row title="TV mode" note="Full screen, made for a controller on the couch. Start on a controller or F11 switches it too.">
              <button className="btn sm" onClick={onTv}>{tv ? 'Leave TV mode' : 'Open TV mode'}</button>
            </Row>
          )}
        </div>
      </section>

      <section className="set-group">
        <h2>{t('settings.online', 'Online')}</h2>
        <div className="card set-list">
          <Row title={t('settings.name', 'Your name')} note={t('settings.nameNote', '') || undefined}>
            <input className="field" value={name} maxLength={24} onChange={(e) => setName(e.target.value)} onBlur={commitName} onKeyDown={(e) => e.key === 'Enter' && commitName()} />
          </Row>
          {sw('online', t('settings.onlineToggle', 'Show open rooms and check for game updates'), t('settings.onlineNote', '') || undefined)}
          {sw('autoUpdate', t('settings.autoUpdate', 'Update the launcher automatically'))}
        </div>
      </section>

      <section className="set-group">
        <h2>{t('settings.folders', 'Folders')}</h2>
        <div className="card set-list">
          <Row title={t('settings.gamesDir', 'Game files')} note={gamesDir}>
            <button className="btn sm" onClick={async () => {
              const dir = await pickFolder(t('settings.pickFolder', 'Choose the folder for game files'), gamesDir)
              if (dir) save('gamesDir', dir)
            }}>{t('settings.change', 'Change...')}</button>
            <button className="btn sm icon" aria-label={t('settings.open', 'Open')} title={t('settings.open', 'Open')} disabled={!gamesDir} onClick={() => open(gamesDir).catch((e) => setError(String(e)))}><Glyph code="E838" size={16} /></button>
            <button className="btn sm" disabled={!u.gamesDir} onClick={() => save('gamesDir', '')}>{t('settings.reset', 'Use default')}</button>
          </Row>
          <Row title={t('settings.log', 'Open log folder')} note={logDir}>
            <button className="btn sm icon" aria-label={t('settings.log', 'Open log folder')} disabled={!logDir} onClick={() => open(logDir).catch((e) => setError(String(e)))}><Glyph code="E838" size={16} /></button>
          </Row>
        </div>
      </section>

      <div className="set-foot">
        <span>
          {t('settings.about', 'Multiplayer Everything Launcher')}
          {lib.players.filter((p) => p.found).map((p) => <span key={p.id} className="ver">{p.name} {p.version || 'installed'}</span>)}
        </span>
        <button className="btn sm" onClick={onRunSetup}>Run setup again</button>
        <button className="btn sm" onClick={() => copyText(diagnostics(lib, server)).then(() => setCopied(true)).catch((e) => setError(String(e)))}>
          {copied ? 'Copied' : 'Copy diagnostics'}
        </button>
        {lib.website && <button className="linkbtn" onClick={() => open(lib.website)}><Glyph code="E8A7" size={14} />{t('settings.github', 'Project page')}</button>}
      </div>
      {error && <p className="error-text">{error}</p>}
    </div>
  )
}
