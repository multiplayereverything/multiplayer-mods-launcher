// What the window works out from the launcher's answer: which buttons a game shows, what hosting and joining use,
// and the state line. Run with: node --test tests/
import { test } from 'node:test'
import assert from 'node:assert/strict'
import type { Action, Game, Library, ModInfo } from '../src/api.ts'
import {
  fileAction, hostChoices, isCoop, joinByAddress, joinByCode, leadAction, looksLikeAddress, mainActions, moreActions, newer, playersShort, statusOf,
} from '../src/model.ts'

const action = (a: Partial<Action> & { id: string }): Action => ({
  label: a.id, description: '', hint: '', icon: '', more: false, offline: false, original: false, link: '', mod: null, relayPort: 0,
  coopHost: false, coopJoin: false, joinsByCode: false, hostPlayers: 0, viaRelay: false, asksAddress: false, state: 'ready', ...a,
})
const game = (g: Partial<Game> & { actions: Action[] }): Game => ({
  id: 'g', name: 'Game', description: '', players: '1-4 players (online co-op)', maxPlayers: 4, color: '', colorEnd: '', image: '',
  imageFocus: 0.5, onlineId: '', joinAction: '', comingSoon: false, lobby: false, categories: [],
  guide: { needs: [], steps: [], tips: [], links: [] }, settings: [], player: '', outdated: false, ...g,
})
const lib = { text: {}, user: { games: {} } } as unknown as Library
const shown = { showOffline: false, showOriginals: false }

const coop = game({
  joinAction: 'online',
  actions: [
    action({ id: 'online', coopHost: true, coopJoin: true, joinsByCode: true, hostPlayers: 2, viaRelay: true }),
    action({ id: 'room3', more: true, coopHost: true, hostPlayers: 3, viaRelay: true }),
    action({ id: 'host', more: true, coopHost: true, hostPlayers: 2 }),
    action({ id: 'joinip', more: true, asksAddress: true }),
    action({ id: 'solo', offline: true }),
    action({ id: 'orig', original: true }),
  ],
})

test('a co-op game is recognised and its host choices listed', () => {
  assert.equal(isCoop(coop), true)
  assert.deepEqual(hostChoices(coop).map((c) => [c.players, c.relay]), [[2, true], [3, true], [2, false]])
  assert.equal(joinByCode(coop)?.id, 'online')
  assert.equal(joinByAddress(coop)?.id, 'joinip')
})

test('offline and original play follow the settings', () => {
  assert.deepEqual(mainActions(coop, shown).map((a) => a.id), [])
  assert.deepEqual(mainActions(coop, { showOffline: true, showOriginals: true }).map((a) => a.id), ['solo', 'orig'])
  assert.deepEqual(moreActions(coop, shown).map((a) => a.id), ['room3', 'host', 'joinip'])
})

test('the lead action skips offline and original play', () => {
  assert.equal(leadAction(coop)?.id, 'online')
  assert.equal(leadAction(game({ actions: [action({ id: 'solo', offline: true }), action({ id: 'play' })] }))?.id, 'play')
  assert.equal(leadAction(game({ comingSoon: true, actions: [action({ id: 'play' })] })), undefined)
})

test('the file a game is judged by is its join action', () => {
  assert.equal(fileAction(coop)?.id, 'online')
})

test('addresses and room codes are told apart', () => {
  assert.equal(looksLikeAddress('192.168.1.20:7790'), true)
  assert.equal(looksLikeAddress('host.example:7790'), true)
  assert.equal(looksLikeAddress('ABCDE'), false)
})

test('versions compare as numbers', () => {
  assert.equal(newer('1.10.0', '1.9.2'), true)
  assert.equal(newer('1.0', '1.0.0'), false)
  assert.equal(newer('2.0.0-beta', '1.9'), true)
})

test('the state line for Flash games', () => {
  assert.equal(statusOf(game({ actions: [action({ id: 'play' })] }), lib, null, false).kind, 'ready')
  assert.equal(statusOf(game({ actions: [action({ id: 'play', state: 'noFile' })] }), lib, null, false).text, 'Game file missing')
  assert.equal(statusOf(game({ actions: [action({ id: 'play', state: 'noPlayer' })] }), lib, null, false).kind, 'missing')
  assert.equal(statusOf(game({ actions: [action({ id: 'play' })] }), lib, null, true).kind, 'playing')
})

test('a game played from its own page is ready', () => {
  assert.equal(statusOf(game({ actions: [action({ id: 'page', link: 'https://example.com', state: undefined })] }), lib, null, false).kind, 'ready')
})

test('a game file with a newer build in the catalog shows an update', () => {
  assert.equal(statusOf(game({ outdated: true, actions: [action({ id: 'play' })] }), lib, null, false).kind, 'update')
})

test('the state line for PC mods', () => {
  const g = game({ mod: { modName: 'M', publisher: '', website: '', gameName: 'Fake Game', pinnedVersion: '' }, actions: [] })
  const m = (x: Partial<ModInfo>): Record<string, ModInfo> => ({
    g: { id: 'g', gameFound: true, gameDir: '', installed: true, installedVersion: '1.0.0', running: false, problem: '', missing: '', known: true, published: true, newest: '1.0.0', notice: '', error: '', ...x },
  })
  assert.equal(statusOf(g, lib, null, false).kind, 'checking')
  assert.equal(statusOf(g, lib, m({ gameFound: false }), false).text, 'Fake Game not found')
  assert.equal(statusOf(g, lib, m({ installed: false }), false).text, 'Not installed')
  assert.equal(statusOf(g, lib, m({ newest: '1.1.0' }), false).kind, 'update')
  assert.equal(statusOf(g, lib, m({}), false).text, 'Installed 1.0.0')
})

test('the player count loses its note where space is short', () => {
  assert.equal(playersShort(coop), '1-4 players')
})
