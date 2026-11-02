// Just enough W3C WebDriver to drive the window through tauri-driver: find, click, type, read, run a script.
const ELEMENT = 'element-6066-11e4-a52e-4f735466cecf'
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

export class Driver {
  constructor(base) {
    this.base = base
    this.id = null
  }

  async call(method, path, body) {
    const res = await fetch(`${this.base}${path}`, {
      method,
      headers: { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    })
    const json = await res.json().catch(() => ({}))
    if (!res.ok) throw new Error(`${method} ${path}: ${json.value?.message ?? res.status}`)
    return json.value
  }

  async start(application) {
    const v = await this.call('POST', '/session', { capabilities: { alwaysMatch: { 'tauri:options': { application } } } })
    this.id = v.sessionId
  }

  stop() {
    return this.id ? this.call('DELETE', `/session/${this.id}`).catch(() => {}) : Promise.resolve()
  }

  s(path) {
    return `/session/${this.id}${path}`
  }

  // Every element matching an XPath, in document order.
  async all(xpath) {
    const v = await this.call('POST', this.s('/elements'), { using: 'xpath', value: xpath })
    return v.map((e) => e[ELEMENT])
  }

  // Laid out and not hidden: has a box on the page and isn't visibility:hidden.
  visible(id) {
    return this.exec(
      'const e = arguments[0]; return e.getClientRects().length > 0 && getComputedStyle(e).visibility !== "hidden"',
      { [ELEMENT]: id },
    )
  }

  // The first visible match, waiting up to `ms` for one to appear.
  async find(xpath, ms = 8000) {
    const end = Date.now() + ms
    let why = ''
    for (;;) {
      for (const id of await this.all(xpath).catch((e) => ((why = e.message), []))) {
        if (await this.visible(id).catch((e) => ((why = e.message), false))) return id
      }
      if (Date.now() > end) throw new Error(`nothing visible matches ${xpath}${why ? ` (${why})` : ''}`)
      await sleep(150)
    }
  }

  async click(xpath, ms) {
    const id = await this.find(xpath, ms)
    await this.call('POST', this.s(`/element/${id}/click`), {})
  }

  async type(xpath, text, clear = true) {
    const id = await this.find(xpath)
    if (clear) await this.call('POST', this.s(`/element/${id}/clear`), {})
    await this.call('POST', this.s(`/element/${id}/value`), { text })
  }

  async text(xpath, ms) {
    const id = await this.find(xpath, ms)
    return this.call('GET', this.s(`/element/${id}/text`))
  }

  async count(xpath) {
    let n = 0
    for (const id of await this.all(xpath)) if (await this.visible(id).catch(() => false)) n++
    return n
  }

  exec(script, ...args) {
    return this.call('POST', this.s('/execute/sync'), { script, args })
  }

  execAsync(script, ...args) {
    return this.call('POST', this.s('/execute/async'), { script, args })
  }

  screenshot() {
    return this.call('GET', this.s('/screenshot'))
  }
}

// Waits until `fn` returns something truthy (or `ms` passes) and returns it.
export async function waitFor(fn, ms = 8000) {
  const end = Date.now() + ms
  for (;;) {
    const v = await Promise.resolve().then(fn).catch(() => undefined)
    if (v) return v
    if (Date.now() > end) return v
    await sleep(150)
  }
}

// XPath for a button whose visible text is exactly `t`, or (prefix) starts with it.
// A Fluent glyph may come first (one character, hidden from screen readers), so the text after it counts too.
export const button = (t, prefix = false) =>
  prefix ? `//button[starts-with(normalize-space(.), ${q(t)}) or starts-with(substring(normalize-space(.), 2), ${q(t)})]`
    : `//button[normalize-space(.)=${q(t)} or substring(normalize-space(.), 2)=${q(t)}]`
// A quoted XPath string that may contain either kind of quote.
export const q = (t) => (t.includes("'") ? `"${t}"` : `'${t}'`)
