import { useEffect, useRef, useState } from 'react'

// A game controller through the browser's Gamepad API (WebView2 has it): the D-pad or left stick move the focus to
// the nearest control in that direction, A presses it, B goes back, Y searches, LB and RB switch the top tabs.
// The same moves as the C++ launcher's XInput mapping. X and Start/Menu are TV mode's: X stars the game you're on,
// Start switches TV mode on and off.

type Dir = 'up' | 'down' | 'left' | 'right'

const FOCUSABLE = 'button:not([disabled]), a[href], input, [tabindex]:not([tabindex="-1"])'

function visible(el: HTMLElement): boolean {
  const r = el.getBoundingClientRect()
  return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== 'hidden'
}

// The closest control whose middle lies in that direction, favouring ones in line with the current one.
export function moveFocus(dir: Dir) {
  const all = [...document.querySelectorAll<HTMLElement>(FOCUSABLE)].filter(visible)
  const cur = document.activeElement instanceof HTMLElement && all.includes(document.activeElement) ? document.activeElement : null
  if (!cur) {
    all[0]?.focus()
    return
  }
  const a = cur.getBoundingClientRect()
  const ax = a.left + a.width / 2
  const ay = a.top + a.height / 2
  let best: HTMLElement | null = null
  let bestScore = Infinity
  for (const el of all) {
    if (el === cur) continue
    const b = el.getBoundingClientRect()
    const dx = b.left + b.width / 2 - ax
    const dy = b.top + b.height / 2 - ay
    const main = dir === 'left' ? -dx : dir === 'right' ? dx : dir === 'up' ? -dy : dy
    const side = dir === 'left' || dir === 'right' ? Math.abs(dy) : Math.abs(dx)
    if (main <= 4) continue
    const score = main + side * 2.5
    if (score < bestScore) {
      bestScore = score
      best = el
    }
  }
  if (best) {
    best.focus()
    best.scrollIntoView({ block: 'nearest', inline: 'nearest' })
  }
}

export interface PadHandlers {
  back: () => void
  search: () => void
  prevTab: () => void
  nextTab: () => void
  x?: () => void
  menu?: () => void
}

// Returns whether a controller is connected, for the status bar's hints.
export function usePad(handlers: PadHandlers): boolean {
  const [connected, setConnected] = useState(false)
  const h = useRef(handlers)
  useEffect(() => {
    h.current = handlers
  })

  useEffect(() => {
    let raf = 0
    let held: boolean[] = []
    let repeatDir: Dir | null = null
    let repeatAt = 0
    const onChange = () => {
      const any = [...navigator.getGamepads()].some(Boolean)
      setConnected(any)
      if (any && !raf) raf = requestAnimationFrame(tick)
    }
    const tick = () => {
      raf = 0
      const pad = [...navigator.getGamepads()].find(Boolean)
      if (!pad) return
      const now = performance.now()
      const stick = (i: number) => pad.axes[i] ?? 0
      const down = (i: number) => !!pad.buttons[i]?.pressed
      // Standard mapping: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 9 Start/Menu, 12-15 the D-pad.
      const state: boolean[] = []
      state[0] = down(0)
      state[1] = down(1)
      state[2] = down(2)
      state[3] = down(3)
      state[9] = down(9)
      state[4] = down(4)
      state[5] = down(5)
      state[12] = down(12) || stick(1) < -0.5
      state[13] = down(13) || stick(1) > 0.5
      state[14] = down(14) || stick(0) < -0.5
      state[15] = down(15) || stick(0) > 0.5
      const pressed = (i: number) => state[i] && !held[i]
      // Only while this window has the focus: a pad held in another app is not ours.
      if (document.hasFocus()) {
        const dirs: [number, Dir][] = [[12, 'up'], [13, 'down'], [14, 'left'], [15, 'right']]
        for (const [i, d] of dirs) {
          if (pressed(i)) {
            moveFocus(d)
            repeatDir = d
            repeatAt = now + 400
          }
        }
        if (repeatDir && !dirs.some(([i, d]) => d === repeatDir && state[i])) repeatDir = null
        if (repeatDir && now >= repeatAt) {
          moveFocus(repeatDir)
          repeatAt = now + 120
        }
        if (pressed(0) && document.activeElement instanceof HTMLElement) document.activeElement.click()
        if (pressed(1)) h.current.back()
        if (pressed(2)) h.current.x?.()
        if (pressed(3)) h.current.search()
        if (pressed(9)) h.current.menu?.()
        if (pressed(4)) h.current.prevTab()
        if (pressed(5)) h.current.nextTab()
      }
      held = state
      raf = requestAnimationFrame(tick)
    }
    window.addEventListener('gamepadconnected', onChange)
    window.addEventListener('gamepaddisconnected', onChange)
    onChange()
    return () => {
      window.removeEventListener('gamepadconnected', onChange)
      window.removeEventListener('gamepaddisconnected', onChange)
      cancelAnimationFrame(raf)
    }
  }, [])
  return connected
}
