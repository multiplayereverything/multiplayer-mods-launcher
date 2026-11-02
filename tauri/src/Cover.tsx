import { useState } from 'react'
import { GLYPH_H, GLYPH_W, glyph, initials } from './pixelFont'
import type { Game } from './api'

// Cover art: the game's picture when it has one, otherwise the initials in the pixel font on its colors.
// `tall` is for poster frames: a picture much wider than the frame shows whole over a blurred copy of itself,
// instead of losing two thirds of it to the crop.
export function Cover({ game, className, tall }: { game: Game; className?: string; tall?: boolean }) {
  const [wide, setWide] = useState(false)
  if (game.image) {
    const pos = { objectPosition: `50% ${(game.imageFocus ?? 0.5) * 100}%` }
    return (
      <div className={`cover ${wide ? 'wide' : ''} ${className ?? ''}`}>
        {wide && <img className="cover-blur" src={game.image} alt="" draggable={false} />}
        <img src={game.image} alt="" style={pos} draggable={false}
          onLoad={(e) => tall && setWide(e.currentTarget.naturalWidth > e.currentTarget.naturalHeight * 1.15)} />
      </div>
    )
  }
  const text = initials(game.name)
  const gap = 1
  const width = text.length * GLYPH_W + (text.length - 1) * gap
  const cells: [number, number][] = []
  text.split('').forEach((ch, i) => {
    const bits = glyph(ch)
    if (!bits) return
    for (let r = 0; r < GLYPH_H; r++)
      for (let c = 0; c < GLYPH_W; c++)
        if (bits[r * GLYPH_W + c] === '1') cells.push([i * (GLYPH_W + gap) + c, r])
  })
  return (
    <div
      className={`cover cover-pixel ${className ?? ''}`}
      style={{ background: `linear-gradient(160deg, ${game.color ?? '#3a3f4b'}, ${game.colorEnd ?? '#1a1d24'})` }}
    >
      <svg viewBox={`-2 -2 ${width + 4} ${GLYPH_H + 4}`} shapeRendering="crispEdges" aria-hidden="true">
        {cells.map(([x, y]) => (
          <rect key={`${x}-${y}`} x={x} y={y} width="1" height="1" fill="#fff" fillOpacity="0.92" />
        ))}
      </svg>
    </div>
  )
}
