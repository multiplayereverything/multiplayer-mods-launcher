// Checks the reports Worker over HTTP: start it with `npm run dev` (http://localhost:8790), then `npm test`.
// Each run uses its own game ids, so the local database can keep earlier runs.
const base = process.env.REPORTS_URL ?? 'http://localhost:8790'
const url = `${base}/v1/reports`
const run = Date.now().toString(36)
const results = []
const check = async (name, fn) => {
  try {
    await fn()
    results.push([name, true])
  } catch (e) {
    results.push([name, false, e.message])
  }
  const [, ok, why] = results.at(-1)
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${why ? `: ${why}` : ''}`)
}
const assert = (cond, why) => {
  if (!cond) throw new Error(why)
}
const post = (body, ip = '203.0.113.7') =>
  fetch(url, { method: 'POST', headers: { 'content-type': 'application/json', 'cf-connecting-ip': ip }, body: typeof body === 'string' ? body : JSON.stringify(body) })
const get = (game) => fetch(`${url}?game=${encodeURIComponent(game)}`).then((r) => r.json())
const install = (n) => n.toString(16).padStart(32, 'a')
const base_ = (game, n, extra = {}) => ({
  game, install: install(n), verdict: 'runs', version: '1.0.5', mins: 14, os: 'Windows 11 26200',
  gpu: 'NVIDIA GeForce RTX 3060', cpu: 'AMD Ryzen 5 5600X', ram_gb: 16, ...extra,
})

const g = `t-${run}`
await check('an unknown game has no reports', async () => {
  const r = await get(g)
  assert(JSON.stringify(r) === JSON.stringify({ total: 0, counts: { runs: 0, issues: 0, broken: 0 }, versions: {}, recent: [] }), JSON.stringify(r))
})
await check('reports are kept, one per PC, game and version', async () => {
  for (const b of [
    base_(g, 1),
    base_(g, 1, { verdict: 'issues', note: 'Sound\u0007 crackles\n in level 3' }), // replaces the first
    base_(g, 1, { version: '1.0.6' }),
    base_(g, 2, { verdict: 'broken', note: 'x'.repeat(900) }),
  ]) {
    const r = await post(b)
    assert(r.status === 200, `${r.status} ${await r.text()}`)
  }
  const r = await get(g)
  assert(r.total === 3, JSON.stringify(r))
  assert(JSON.stringify(r.counts) === JSON.stringify({ runs: 1, issues: 1, broken: 1 }), JSON.stringify(r.counts))
  assert(r.versions['1.0.5'].issues === 1 && r.versions['1.0.5'].broken === 1 && r.versions['1.0.6'].runs === 1, JSON.stringify(r.versions))
  assert(r.recent.length === 3 && r.recent[0].verdict === 'broken' && r.recent[0].note.length === 500, JSON.stringify(r.recent[0]))
  const issue = r.recent.find((x) => x.verdict === 'issues')
  assert(issue.note === 'Sound crackles in level 3', JSON.stringify(issue))
  assert(issue.gpu === 'NVIDIA GeForce RTX 3060' && issue.ram_gb === 16 && issue.mins === 14, JSON.stringify(issue))
  const all = JSON.stringify(r)
  assert(!all.includes(install(1)) && !all.includes('install') && !all.includes('hidden'), 'the install id or a hidden flag went out')
})
await check('games do not mix', async () => {
  assert((await get(`${g}-other`)).total === 0, 'another game has reports')
})
await check('bad reports are refused', async () => {
  const cases = [
    [base_(g, 3, { verdict: 'great' }), 'Unknown verdict'],
    [base_(g, 3, { install: 'short' }), 'Missing install id'],
    [base_('Bad Game!', 3), 'Bad game id'],
    ['not json', "isn't JSON"],
    ['[1,2]', "isn't JSON"],
    [JSON.stringify(base_(g, 3, { note: 'x'.repeat(5000) })), 'too long'],
  ]
  for (const [body, want] of cases) {
    const r = await post(body)
    const t = await r.text()
    assert(r.status >= 400 && t.includes(want), `${r.status} ${t} (wanted ${want})`)
  }
  assert((await get(g)).total === 3, 'a refused report was counted')
})
await check('twenty reports an hour from one address', async () => {
  const ip = `198.51.100.${Math.floor(Math.random() * 250) + 1}`
  for (let i = 0; i < 20; i++) {
    const r = await post(base_(`${g}-rate`, 100 + i), ip)
    assert(r.status === 200, `report ${i + 1}: ${r.status}`)
  }
  const r = await post(base_(`${g}-rate`, 200), ip)
  assert(r.status === 429, `the 21st got ${r.status}`)
  const other = await post(base_(`${g}-rate`, 201), '198.51.100.251')
  assert(other.status === 200, `another address got ${other.status}`)
})
await check('the website may read the reports (CORS on GET)', async () => {
  const r = await fetch(`${url}?game=${g}`)
  assert(r.headers.get('access-control-allow-origin') === '*', 'no CORS header')
  assert((await fetch(`${base}/other`)).status === 404, 'another path answered')
  assert((await fetch(url, { method: 'DELETE' })).status === 405, 'DELETE answered')
})

const failed = results.filter((r) => !r[1]).length
console.log(`\nREPORTS WORKER ${results.length - failed}/${results.length} passed`)
process.exit(failed ? 1 : 0)
