/*
 * App.tsx - the CitySlice HUD, laid out like Cities: Skylines.
 *
 *   top-left      city name + milestone progress, population, money, happiness,
 *                 then a row of panel buttons (Chirper, City info, Economy, Map, Overlays)
 *   top-right     date / clock / speed controls
 *   bottom        the category toolbar (undo, bulldoze, zoning, roads, services,
 *                 landscaping); a category opens an asset panel above it
 *   right         the selected-object panel
 *
 * Every panel carries data-ui so the host knows clicks there are for the HUD,
 * not the map. Values that drift every tick are quantised before they reach
 * the DOM, and bars animate with transforms only (see the perf notes in
 * store.ts).
 */
import { useEffect, useRef, useState } from 'react'
import { game, useGame, POLICIES, SERVICES, type ConsoleData, type History, type PolicyId, type ServiceId, type Stats, type ToolName } from './store'

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']
const MONTH_DAYS = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]

// Mirrors kMilestones in City.cpp; index = number of milestones reached.
const MILESTONES: { pop: number; name: string }[] = [
  { pop: 0, name: 'Outpost' },
  { pop: 40, name: 'Hamlet' },
  { pop: 120, name: 'Village' },
  { pop: 350, name: 'Town' },
  { pop: 900, name: 'City' },
  { pop: 2000, name: 'Metropolis' },
]

function fmtMoney(v: number): string {
  const sign = v < 0 ? '-' : ''
  const s = Math.abs(Math.round(v)).toString()
  let out = ''
  let rest = s
  while (rest.length > 3) {
    out = ',' + rest.slice(-3) + out
    rest = rest.slice(0, -3)
  }
  return sign + '$' + rest + out
}

function fmtNum(v: number): string {
  return fmtMoney(v).replace('$', '')
}

function fmtDate(days: number): string {
  let d = Math.floor(days)
  let year = 2026
  let m = 0
  for (;;) {
    const len = MONTH_DAYS[m] + (m === 1 && year % 4 === 0 ? 1 : 0)
    if (d < len) break
    d -= len
    m++
    if (m === 12) {
      m = 0
      year++
    }
  }
  return `${d + 1} ${MONTHS[m]} ${year}`
}

function fmtClock(hour: number): string {
  const h = Math.floor(hour) % 24
  const min = Math.floor((hour - Math.floor(hour)) * 60)
  return `${h.toString().padStart(2, '0')}:${min.toString().padStart(2, '0')}`
}

type Category = 'bulldoze' | 'zone' | 'road' | ServiceId | 'landscape'
const CATEGORIES: { id: Category; label: string; unlock: number; key?: string }[] = [
  { id: 'bulldoze', label: 'Bulldoze', unlock: 0, key: 'B' },
  { id: 'zone', label: 'Zoning', unlock: 0, key: 'Z' },
  { id: 'road', label: 'Roads', unlock: 0, key: 'R' },
  { id: 'power', label: 'Electricity', unlock: 0 },
  { id: 'water', label: 'Water & Sewage', unlock: 0 },
  { id: 'health', label: 'Healthcare', unlock: 1 },
  { id: 'edu', label: 'Education', unlock: 1 },
  { id: 'police', label: 'Police', unlock: 2 },
  { id: 'park', label: 'Parks', unlock: 2 },
  { id: 'transit', label: 'Transport', unlock: 3 },
  { id: 'landscape', label: 'Landscaping', unlock: 0, key: 'G' },
]

const HINTS: Record<ToolName, string> = {
  select: 'Click a building or tile to inspect it. Click the lake to make a splash.',
  road: 'Drag on the map to lay a road. Over water it becomes a bridge ($160 per tile). $40 per tile.',
  avenue: 'Drag to lay a four-lane avenue: faster traffic, lamps on both kerbs. $90 per tile.',
  res: 'Paint residential zones beside roads. Homes rise when demand is high and the land is nice.',
  com: 'Paint commercial zones. Shops need customers nearby and a bus line to reach level 3.',
  ind: 'Paint industrial zones. Factories bring jobs but sour nearby homes.',
  bulldoze: 'Drag over anything to clear it. $10 per tile; roads refund $15, services a quarter.',
  power: 'Place next to a road. Serves every building within 26 road tiles, up to 900 kW.',
  water: 'Place next to a road. Serves 24 road tiles, up to 600 m³ a day.',
  health: 'Place next to a road. Homes within 12 road tiles get healthier and happier.',
  edu: 'Place next to a road. Homes within 12 road tiles can grow to level 3.',
  police: 'Place next to a road. Everyone within 14 road tiles feels safer.',
  park: 'Place next to a road. A big land value boost for the 6 tiles around it.',
  transit: 'Place next to a road. Bus lines reach 20 road tiles; shops need them for level 3.',
  raise: 'Click or hold to raise the ground under the brush. [ ] change the brush size. $6 per metre.',
  lower: 'Click or hold to lower the ground; dig below the waterline to make a lake. $6 per metre.',
  flatten: 'Drag a rectangle to level it to its average height. Handy before a big plant.',
  trees: 'Drag a rectangle to plant a grove on empty land. $12 per tile.',
}

const TOOL_KEYS: Partial<Record<ToolName, string>> = {
  road: 'R', avenue: 'V', res: 'Z', com: 'X', ind: 'C', bulldoze: 'B', raise: 'G', lower: 'H', flatten: 'F', trees: 'T',
}

function categoryOf(tool: ToolName): Category | null {
  if (tool === 'road' || tool === 'avenue') return 'road'
  if (tool === 'bulldoze') return 'bulldoze'
  if (tool === 'res' || tool === 'com' || tool === 'ind') return 'zone'
  if (tool === 'raise' || tool === 'lower' || tool === 'flatten' || tool === 'trees') return 'landscape'
  if (SERVICES.some((s) => s.id === tool)) return tool as ServiceId
  return null
}

function serviceIndex(id: ServiceId): number {
  return SERVICES.findIndex((s) => s.id === id)
}

// ---------------------------------------------------------------------------

function CityBar({ s, panels, toggle }: { s: Stats; panels: Set<string>; toggle: (p: string) => void }) {
  const pop = Math.round(s.population)
  let idx = 0
  for (let i = 0; i < MILESTONES.length; i++) if (pop >= MILESTONES[i].pop) idx = i
  const cur = MILESTONES[idx]
  const next = MILESTONES[Math.min(idx + 1, MILESTONES.length - 1)]
  const span = Math.max(1, next.pop - cur.pop)
  const progress = idx === MILESTONES.length - 1 ? 1 : Math.min(1, (pop - cur.pop) / span)
  const progressQ = Math.round(progress * 40) / 40
  const happyQ = Math.round(s.happiness)
  const moodClass = s.happiness >= 70 ? 'good' : s.happiness >= 45 ? 'ok' : 'bad'
  const problems = s.unpowered + s.unwatered
  return (
    <div id="citybar" data-ui>
      <div className="panel strip">
        <div className="city">
          <div className="city-row">
            <span className="city-name">New Horizon</span>
            <span className="milestone">{cur.name}</span>
          </div>
          <div className="xp" title="Next milestone">
            <div className="xp-fill" style={{ transform: `scaleX(${progressQ})` }} />
          </div>
          <div className="xp-text">
            {idx === MILESTONES.length - 1 ? 'Max level' : `${fmtNum(pop)} / ${fmtNum(next.pop)} to ${next.name}`}
          </div>
        </div>
        <div className="vsep" />
        <div className="kpi" title="Population">
          <span className="ico ico-pop" />
          <div>
            <b>{fmtNum(pop)}</b>
            <small>population</small>
          </div>
        </div>
        <div className="kpi" title="Treasury">
          <span className="ico ico-money" />
          <div>
            <b className={s.money < 0 ? 'bad' : ''}>{fmtMoney(s.money)}</b>
            <small className={s.income >= 0 ? 'up' : 'down'}>{(s.income >= 0 ? '+' : '') + fmtMoney(s.income)} / day</small>
          </div>
        </div>
        <div className="kpi" title="Happiness">
          <span className={'ring ' + moodClass} style={{ ['--pct' as string]: `${happyQ}%` }}>
            <span className="ring-core">{moodClass === 'good' ? '☺' : moodClass === 'ok' ? '•‿•' : '☹'}</span>
          </span>
          <div>
            <b>{happyQ}%</b>
            <small>happiness</small>
          </div>
        </div>
        {problems > 0 && (
          <div className="kpi problems" title="Buildings without power or water">
            <span className="ico ico-warn" />
            <div>
              <b>{problems}</b>
              <small>{s.unpowered ? `${s.unpowered} no power` : ''}{s.unpowered && s.unwatered ? ' · ' : ''}{s.unwatered ? `${s.unwatered} no water` : ''}</small>
            </div>
          </div>
        )}
      </div>
      <div className="quick">
        <button className={'qb chirper' + (panels.has('chirper') ? ' on' : '')} onClick={() => toggle('chirper')} title="Chirper">
          <span className="bird" />
        </button>
        <button className={'qb' + (panels.has('info') ? ' on' : '')} onClick={() => toggle('info')} title="City information">
          <span className="glyph">i</span>
        </button>
        <button className={'qb' + (panels.has('economy') ? ' on' : '')} onClick={() => toggle('economy')} title="Economy">
          <span className="glyph">$</span>
        </button>
        <button className={'qb' + (panels.has('stats') ? ' on' : '')} onClick={() => toggle('stats')} title="Statistics">
          <span className="glyph chart" />
        </button>
        <button className={'qb' + (panels.has('policies') ? ' on' : '')} onClick={() => toggle('policies')} title="Policies">
          <span className="glyph">§</span>
        </button>
        <button className={'qb' + (panels.has('map') ? ' on' : '')} onClick={() => toggle('map')} title="Map">
          <span className="glyph map" />
        </button>
        <button className={'qb' + (panels.has('overlays') ? ' on' : '')} onClick={() => toggle('overlays')} title="Info views">
          <span className="glyph">◔</span>
        </button>
      </div>
    </div>
  )
}

function TimeBar({ s }: { s: Stats }) {
  const hourQ = Math.floor(s.hour * 4) / 4
  const isNight = s.hour < 6 || s.hour >= 20
  const dayPct = Math.round((hourQ / 24) * 100)
  return (
    <div id="timebar" className={s.speed === 0 ? 'paused' : ''} data-ui>
      <div className="panel strip">
        <div className="clock">
          <span className={'sky ' + (isNight ? 'night' : 'day')} style={{ ['--t' as string]: `${dayPct}%` }} />
          <div>
            <b>{fmtClock(hourQ)}</b>
            <small>{fmtDate(s.day)}</small>
          </div>
        </div>
        <div className="vsep" />
        <div className="speed">
          {['II', '1x', '2x', '3x'].map((t, i) => (
            <button key={t} className={s.speed === i ? 'active' : ''} onClick={() => game.setSpeed(i)} title={i === 0 ? 'Pause' : `${i}x`}>
              {t}
            </button>
          ))}
        </div>
      </div>
      <div className="fps">{Math.round(s.fps)} fps · {s.frameMs.toFixed(1)} ms</div>
    </div>
  )
}

function Chirper({ toasts, open }: { toasts: { id: number; text: string; kind: string }[]; open: boolean }) {
  const shown = open ? toasts : toasts.filter((t) => t.kind !== 'info').slice(-2)
  const handle = (kind: string) => (kind === 'milestone' ? '@CityHall' : kind === 'warn' ? '@Treasury' : '@NewHorizonDaily')
  return (
    <div id="chirps" className={open ? 'open' : ''}>
      {shown.map((t) => (
        <div key={t.id} className={'chirp ' + t.kind} data-ui>
          <span className="avatar" />
          <div>
            <small>{handle(t.kind)}</small>
            <span>{t.text}</span>
          </div>
        </div>
      ))}
    </div>
  )
}

function MilestoneBanner({ toasts }: { toasts: { id: number; text: string; kind: string }[] }) {
  const m = toasts.filter((t) => t.kind === 'milestone').slice(-1)[0]
  if (!m) return null
  const name = m.text.replace(/^Milestone:\s*/, '').replace(/\s*\(.*$/, '')
  return (
    <div className="banner" key={m.id}>
      <div className="banner-kicker">Milestone reached</div>
      <div className="banner-title">{name}</div>
      <div className="banner-sub">New services unlocked</div>
    </div>
  )
}

function Toolbar({ s }: { s: Stats }) {
  const cat = categoryOf(s.tool)
  const pick = (id: Category) => {
    if (cat === id) {
      game.setTool('select')
      return
    }
    if (id === 'bulldoze') game.setTool('bulldoze')
    else if (id === 'road') game.setTool('road')
    else if (id === 'zone') game.setTool('res')
    else if (id === 'landscape') game.setTool('raise')
    else game.setTool(id)
  }
  return (
    <div id="bottom" data-ui>
      {cat && <AssetPanel s={s} cat={cat} />}
      <div className="status">
        <span className="status-hint">{HINTS[s.tool]}</span>
        <span className="status-hover">{s.hover || ''}</span>
      </div>
      <nav id="toolbar" className="panel">
        <button className={'cat undo' + (s.canUndo ? '' : ' disabled')} onClick={() => s.canUndo && game.undo()} title="Undo (Ctrl+Z)">
          <span className="cat-icon" />
          <span className="cat-label">Undo</span>
        </button>
        {CATEGORIES.map((c, i) => {
          const locked = s.milestone < c.unlock
          return (
            <button
              key={c.id}
              className={'cat ' + c.id + (cat === c.id ? ' active' : '') + (locked ? ' locked' : '') + (i === 1 ? ' first' : '')}
              onClick={() => (locked ? undefined : pick(c.id))}
            >
              <span className="cat-icon" />
              <span className="cat-label">{c.label}</span>
              {locked ? <span className="lock" title={`Unlocks at ${MILESTONES[c.unlock].name}`} /> : c.key ? <span className="hotkey">{c.key}</span> : null}
            </button>
          )
        })}
      </nav>
    </div>
  )
}

function StatBar({ value, max, cls }: { value: number; max: number; cls?: string }) {
  const f = max > 0 ? Math.min(1, value / max) : 0
  const q = Math.round(f * 40) / 40
  return (
    <span className={'meter ' + (cls ?? '')}>
      <span className="track"><span className={'fill' + (f >= 0.95 ? ' hot' : '')} style={{ transform: `scaleX(${q.toFixed(3)})` }} /></span>
    </span>
  )
}

function ServiceAssets({ s, id }: { s: Stats; id: ServiceId }) {
  const info = SERVICES[serviceIndex(id)]
  const st = s.services.find((x) => x.id === id)
  const pooled = id === 'power' || id === 'water'
  const overlayOn = s.overlay === serviceIndex(id)
  return (
    <div className="assets-body">
      <button className={'asset service ' + id + (s.tool === id ? ' active' : '')} onClick={() => game.setTool(id)}>
        <span className="asset-art" />
        <span className="asset-name">{info.name}</span>
        <span className="asset-sub">{fmtMoney(info.cost)} · {fmtMoney(info.upkeep)}/day</span>
      </button>
      <div className="vsep tall" />
      <div className="service-card">
        <p className="blurb">{info.blurb}</p>
        <div className="rows tight">
          <div className="row"><span>Built</span><b>{st?.buildings ?? 0}</b></div>
          <div className="row"><span>Reach</span><b>{info.radius} road tiles</b></div>
          <div className="row">
            <span>{pooled ? 'Load' : 'Coverage'}</span>
            <span className="meter-row">
              <StatBar value={st?.demand ?? 0} max={Math.max(1, st?.capacity ?? 0)} />
              <b>{fmtNum(st?.demand ?? 0)} / {fmtNum(st?.capacity ?? 0)} {info.unit}</b>
            </span>
          </div>
          <div className="row"><span>{pooled ? 'Buildings served' : 'Tiles served'}</span><b>{st?.covered ?? 0}</b></div>
        </div>
        <button className={'wide small' + (overlayOn ? ' on' : '')} onClick={() => game.setOverlay(overlayOn ? -1 : serviceIndex(id))}>
          {overlayOn ? 'Hide coverage' : 'Show coverage'}
        </button>
      </div>
    </div>
  )
}

function AssetPanel({ s, cat }: { s: Stats; cat: Category }) {
  const zoneTool = s.tool === 'res' || s.tool === 'com' || s.tool === 'ind' ? s.tool : 'res'
  const titles: Record<string, string> = {
    zone: 'Zoning', road: 'Roads', bulldoze: 'Bulldoze', landscape: 'Landscaping',
  }
  const title = titles[cat] ?? CATEGORIES.find((c) => c.id === cat)?.label ?? ''
  return (
    <div id="assets" className="panel">
      <div className="assets-head">
        <span className="assets-title">{title}</span>
        <button className="close" onClick={() => game.setTool('select')}>×</button>
      </div>
      {cat === 'zone' && (
        <div className="assets-body">
          <div className="demand" title="Demand">
            {(['R', 'C', 'I'] as const).map((k) => {
              const v = k === 'R' ? s.demandR : k === 'C' ? s.demandC : s.demandI
              return (
                <div className={'dbar ' + k.toLowerCase()} key={k}>
                  <div className="dfill" style={{ transform: `scaleY(${(Math.round(v * 20) / 20).toFixed(2)})` }} />
                  <span>{k}</span>
                </div>
              )
            })}
          </div>
          <div className="vsep tall" />
          {([
            ['res', 'Residential', 'Homes', 'Z'],
            ['com', 'Commercial', 'Shops & offices', 'X'],
            ['ind', 'Industrial', 'Workshops & plants', 'C'],
          ] as const).map(([id, label, sub, key]) => (
            <button key={id} className={'asset ' + id + (zoneTool === id ? ' active' : '')} onClick={() => game.setTool(id)}>
              <span className="asset-art" />
              <span className="asset-name">{label}</span>
              <span className="asset-sub">{sub} · {key}</span>
            </button>
          ))}
        </div>
      )}
      {cat === 'road' && (
        <div className="assets-body">
          <button className={'asset road' + (s.tool === 'road' ? ' active' : '')} onClick={() => game.setTool('road')}>
            <span className="asset-art" />
            <span className="asset-name">Two-lane road</span>
            <span className="asset-sub">$40 / tile · R</span>
          </button>
          <button className={'asset avenue' + (s.tool === 'avenue' ? ' active' : '')} onClick={() => game.setTool('avenue')}>
            <span className="asset-art" />
            <span className="asset-name">Four-lane avenue</span>
            <span className="asset-sub">$90 / tile · V</span>
          </button>
          <div className="vsep tall" />
          <div className="service-card">
            <p className="blurb">Roads carry power, water and every other service. Drag across water and the road becomes a bridge on piers ($160 per tile). Drawing over a street with the avenue tool upgrades it in place.</p>
          </div>
        </div>
      )}
      {cat === 'bulldoze' && (
        <div className="assets-body">
          <button className="asset bulldoze active">
            <span className="asset-art" />
            <span className="asset-name">Bulldoze</span>
            <span className="asset-sub">$10 / tile · B</span>
          </button>
          <div className="vsep tall" />
          <div className="service-card">
            <p className="blurb">Clears roads, zones, buildings, service buildings and planted trees. Roads refund $15, service buildings a quarter of their price. Ctrl+Z undoes the last action.</p>
          </div>
        </div>
      )}
      {cat === 'landscape' && (
        <div className="assets-body">
          {([
            ['raise', 'Raise ground', '$6 / metre'],
            ['lower', 'Lower ground', '$6 / metre'],
            ['flatten', 'Level area', '$6 / metre'],
            ['trees', 'Plant trees', '$12 / tile'],
          ] as const).map(([id, label, sub]) => (
            <button key={id} className={'asset ' + id + (s.tool === id ? ' active' : '')} onClick={() => game.setTool(id)}>
              <span className="asset-art" />
              <span className="asset-name">{label}</span>
              <span className="asset-sub">{sub} · {TOOL_KEYS[id]}</span>
            </button>
          ))}
          <div className="vsep tall" />
          <div className="service-card">
            <div className="tax">
              <span>Brush size</span>
              <div className="stepper">
                <button onClick={() => game.setBrush(s.brush - 1)} disabled={s.brush <= 1}>−</button>
                <b>{s.brush}</b>
                <button onClick={() => game.setBrush(s.brush + 1)} disabled={s.brush >= 6}>+</button>
              </div>
            </div>
            <p className="blurb">Hold the mouse to keep sculpting. Dig below the waterline to flood a valley; raise a shoreline to reclaim land. Buildings ride the new ground.</p>
          </div>
        </div>
      )}
      {SERVICES.some((x) => x.id === cat) && <ServiceAssets s={s} id={cat as ServiceId} />}
    </div>
  )
}

function Minimap({ s, size, cells, visible }: { s: Stats; size: number; cells: string; visible: boolean }) {
  const canvas = useRef<HTMLCanvasElement>(null)
  const camX = Math.round(s.camX * 96) / 96
  const camY = Math.round(s.camY * 96) / 96
  const camYaw = Math.round(s.camYaw * 8) / 8
  useEffect(() => {
    const c = canvas.current
    if (!c || !size || !visible) return // 9k fillRects per redraw: only while the panel is open
    const ctx = c.getContext('2d')
    if (!ctx) return
    const px = c.width / size
    ctx.fillStyle = '#4f8c3a'
    ctx.fillRect(0, 0, c.width, c.height)
    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        const ch = cells.charCodeAt(y * size + x)
        let color: string | null = null
        switch (ch) {
          case 126: color = '#3d7ec1'; break // ~
          case 35: color = '#2b2b30'; break // #
          case 61: color = '#6d6f78'; break // = avenue / bridge
          case 114: color = '#8ed184'; break // r
          case 82: color = '#39c25a'; break // R
          case 99: color = '#8fb6e6'; break // c
          case 67: color = '#3d8bff'; break // C
          case 105: color = '#e8d48a'; break // i
          case 73: color = '#e0b32c'; break // I
          case 83: color = '#f2f2f2'; break // S
        }
        if (color) {
          ctx.fillStyle = color
          ctx.fillRect(x * px, y * px, px, px)
        }
      }
    }
    ctx.save()
    ctx.translate(camX * c.width, camY * c.height)
    ctx.rotate(-camYaw)
    ctx.strokeStyle = '#ffffff'
    ctx.lineWidth = 2
    ctx.strokeRect(-13, -9, 26, 18)
    ctx.fillStyle = '#ffffff'
    ctx.fillRect(-2, -14, 4, 5)
    ctx.restore()
  }, [size, cells, camX, camY, camYaw, visible])
  const onClick = (e: React.MouseEvent<HTMLDivElement>) => {
    const r = e.currentTarget.getBoundingClientRect()
    if (r.width <= 0 || r.height <= 0) return
    game.lookAt((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height)
  }
  return (
    <div className="minimap" onClick={onClick}>
      <canvas ref={canvas} width={224} height={224} />
    </div>
  )
}

/// A filled line chart on a canvas. The series is one point per in-game day,
/// oldest first. Canvas rather than 120 DOM nodes: the whole plot is one
/// element, so a new day costs a single fillRect pass instead of a relayout.
function Chart({ data, color, width = 246, height = 96, money }: {
  data: number[]; color: string; width?: number; height?: number; money?: boolean
}) {
  const canvas = useRef<HTMLCanvasElement>(null)
  useEffect(() => {
    const c = canvas.current
    if (!c) return
    const ctx = c.getContext('2d')
    if (!ctx) return
    ctx.clearRect(0, 0, width, height)
    ctx.fillStyle = 'rgba(0, 0, 0, 0.30)'
    ctx.fillRect(0, 0, width, height)
    // Horizontal guides at the quarters.
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.07)'
    ctx.lineWidth = 1
    for (let i = 1; i < 4; i++) {
      const y = Math.round((height * i) / 4) + 0.5
      ctx.beginPath()
      ctx.moveTo(0, y)
      ctx.lineTo(width, y)
      ctx.stroke()
    }
    if (data.length < 2) {
      ctx.fillStyle = 'rgba(152, 169, 191, 0.8)'
      ctx.font = '11px "Noto Sans", sans-serif'
      ctx.fillText('Collecting data…', 8, height / 2)
      return
    }
    let lo = Math.min(...data)
    let hi = Math.max(...data)
    if (money) lo = Math.min(lo, 0) // a deficit chart must show the zero line
    if (hi - lo < 1) { hi = lo + 1 }
    const pad = (hi - lo) * 0.08
    lo -= pad
    hi += pad
    const px = (i: number) => (i / (data.length - 1)) * (width - 2) + 1
    const py = (v: number) => height - 2 - ((v - lo) / (hi - lo)) * (height - 4)
    // Zero line when the series crosses it.
    if (lo < 0 && hi > 0) {
      const y = Math.round(py(0)) + 0.5
      ctx.strokeStyle = 'rgba(255, 107, 96, 0.45)'
      ctx.beginPath()
      ctx.moveTo(0, y)
      ctx.lineTo(width, y)
      ctx.stroke()
    }
    ctx.beginPath()
    ctx.moveTo(px(0), py(data[0]))
    for (let i = 1; i < data.length; i++) ctx.lineTo(px(i), py(data[i]))
    ctx.lineTo(px(data.length - 1), height)
    ctx.lineTo(px(0), height)
    ctx.closePath()
    ctx.fillStyle = color.replace('rgb', 'rgba').replace(')', ', 0.22)')
    ctx.fill()
    ctx.beginPath()
    ctx.moveTo(px(0), py(data[0]))
    for (let i = 1; i < data.length; i++) ctx.lineTo(px(i), py(data[i]))
    ctx.strokeStyle = color
    ctx.lineWidth = 2
    ctx.stroke()
    // A dot on the newest sample.
    ctx.beginPath()
    ctx.arc(px(data.length - 1), py(data[data.length - 1]), 2.5, 0, Math.PI * 2)
    ctx.fillStyle = color
    ctx.fill()
  }, [data, color, width, height, money])
  return <canvas className="chart" ref={canvas} width={width} height={height} />
}

const METRICS: { id: keyof History; label: string; color: string; money?: boolean }[] = [
  { id: 'population', label: 'Population', color: 'rgb(79, 214, 109)' },
  { id: 'money', label: 'Treasury', color: 'rgb(244, 197, 66)', money: true },
  { id: 'income', label: 'Net income', color: 'rgb(77, 163, 255)', money: true },
  { id: 'happiness', label: 'Happiness', color: 'rgb(224, 132, 255)' },
  { id: 'jobs', label: 'Jobs', color: 'rgb(242, 193, 78)' },
]

function StatisticsPanel({ h, onClose }: { h: History; onClose: () => void }) {
  const [metric, setMetric] = useState<keyof History>('population')
  const m = METRICS.find((x) => x.id === metric)!
  const data = h[metric]
  const fmt = (v: number) => (m.money ? fmtMoney(v) : metric === 'happiness' ? `${Math.round(v)}%` : fmtNum(Math.round(v)))
  const now = data.length ? data[data.length - 1] : 0
  const prev = data.length > 7 ? data[data.length - 8] : data[0] ?? 0
  const trend = now - prev
  return (
    <Popover id="stats" title="Statistics" onClose={onClose}>
      <div className="chart-tabs">
        {METRICS.map((x) => (
          <button key={x.id} className={metric === x.id ? 'on' : ''} onClick={() => setMetric(x.id)}>{x.label}</button>
        ))}
      </div>
      <Chart data={data} color={m.color} money={m.money} />
      <div className="rows tight">
        <div className="row"><span>Now</span><b>{fmt(now)}</b></div>
        <div className="row">
          <span>Last week</span>
          <b className={trend > 0 ? 'up' : trend < 0 ? 'down' : ''}>{(trend > 0 ? '+' : '') + fmt(trend)}</b>
        </div>
        <div className="row"><span>Peak</span><b>{fmt(data.length ? Math.max(...data) : 0)}</b></div>
        <div className="row"><span>Recorded</span><b>{data.length} day{data.length === 1 ? '' : 's'}</b></div>
      </div>
    </Popover>
  )
}

function PolicyPanel({ s, onClose }: { s: Stats; onClose: () => void }) {
  return (
    <Popover id="policies" title="Policies" onClose={onClose}>
      <div className="policy-list">
        {POLICIES.map((p) => {
          const on = (s.policies & p.bit) !== 0
          return (
            <button
              key={p.id}
              className={'policy' + (on ? ' on' : '')}
              onClick={() => game.togglePolicy(p.id as PolicyId)}
            >
              <span className="switch"><i /></span>
              <span className="policy-text">
                <b>{p.name}</b>
                <small>{p.blurb}</small>
                <em>{p.cost}</em>
              </span>
            </button>
          )
        })}
      </div>
      <div className="rows tight">
        <div className="row total">
          <span>Policies cost</span>
          <b className={s.policyCost > 0 ? 'down' : ''}>{s.policyCost > 0 ? '-' + fmtMoney(s.policyCost) : '$0'} / day</b>
        </div>
      </div>
    </Popover>
  )
}

function Popover({ id, title, onClose, children }: { id: string; title: string; onClose: () => void; children: React.ReactNode }) {
  return (
    <section id={id} className="panel popover" data-ui>
      <div className="pop-head">
        <span>{title}</span>
        <button className="close" onClick={onClose}>×</button>
      </div>
      <div className="pop-body">{children}</div>
    </section>
  )
}

function InfoPanel({ s, onClose }: { s: Stats; onClose: () => void }) {
  return (
    <Popover id="info" title="City information" onClose={onClose}>
      <div className="rows">
        <div className="row"><span>Population</span><b>{fmtNum(Math.round(s.population))}</b></div>
        <div className="row"><span>Jobs</span><b>{fmtNum(Math.round(s.jobs))}</b></div>
        <div className="row"><span>Buildings</span><b>{s.buildings}</b></div>
        <div className="row"><span>Road tiles</span><b>{s.roads}</b></div>
        <div className="row"><span>Vehicles</span><b>{s.cars}</b></div>
        <div className="row"><span>Without power</span><b className={s.unpowered ? 'down' : 'up'}>{s.unpowered}</b></div>
        <div className="row"><span>Without water</span><b className={s.unwatered ? 'down' : 'up'}>{s.unwatered}</b></div>
      </div>
      <div className="rows services">
        {SERVICES.map((info, i) => {
          const st = s.services[i]
          const locked = s.milestone < info.unlock
          return (
            <div className={'row' + (locked ? ' muted' : '')} key={info.id}>
              <span><i className={'dot ' + info.id} />{info.name}</span>
              {locked ? <b>locked</b> : <b>{st?.buildings ?? 0} · {fmtNum(st?.demand ?? 0)}/{fmtNum(st?.capacity ?? 0)}</b>}
            </div>
          )
        })}
      </div>
      <button className="wide danger" onClick={() => game.newCity()}>Start a new city</button>
    </Popover>
  )
}

function OverlayPanel({ s, onClose }: { s: Stats; onClose: () => void }) {
  return (
    <Popover id="overlays" title="Info views" onClose={onClose}>
      <div className="overlay-list">
        <button className={'wide small' + (s.overlay < 0 ? ' on' : '')} onClick={() => game.setOverlay(-1)}>None</button>
        {SERVICES.map((info, i) => (
          <button key={info.id} className={'wide small' + (s.overlay === i ? ' on' : '')} disabled={s.milestone < info.unlock} onClick={() => game.setOverlay(s.overlay === i ? -1 : i)}>
            <i className={'dot ' + info.id} />{info.name.replace(/^(Coal |Medical |Elementary |City )/, '')} coverage
          </button>
        ))}
      </div>
    </Popover>
  )
}

function EconomyPanel({ s, h, onClose }: { s: Stats; h: History; onClose: () => void }) {
  const b = s.budget
  const total = Math.max(1, b.res + b.com + b.ind)
  return (
    <Popover id="economy" title="Economy" onClose={onClose}>
      <div className="tax">
        <span>Tax rate</span>
        <div className="stepper">
          <button onClick={() => game.adjustTax(-1)} disabled={s.tax <= 5}>−</button>
          <b>{s.tax}%</b>
          <button onClick={() => game.adjustTax(1)} disabled={s.tax >= 20}>+</button>
        </div>
      </div>
      <div className="budget-bar">
        <div className="seg res" style={{ width: '33.3%', transform: `scaleX(${(b.res / total * 3).toFixed(3)})` }} />
        <div className="seg com" style={{ width: '33.3%', transform: `scaleX(${(b.com / total * 3).toFixed(3)})` }} />
        <div className="seg ind" style={{ width: '33.3%', transform: `scaleX(${(b.ind / total * 3).toFixed(3)})` }} />
      </div>
      <div className="rows">
        <div className="row"><span><i className="dot res" />Residential tax</span><b>{fmtMoney(b.res)}</b></div>
        <div className="row"><span><i className="dot com" />Commercial tax</span><b>{fmtMoney(b.com)}</b></div>
        <div className="row"><span><i className="dot ind" />Industrial tax</span><b>{fmtMoney(b.ind)}</b></div>
        <div className="row"><span><i className="dot upkeep" />Road upkeep</span><b>-{fmtMoney(b.upkeep)}</b></div>
        <div className="row"><span><i className="dot services" />Service upkeep</span><b>-{fmtMoney(b.services)}</b></div>
        <div className="row"><span><i className="dot policies" />Policies</span><b>{b.policy > 0 ? '-' + fmtMoney(b.policy) : '$0'}</b></div>
        <div className="row total"><span>Net per day</span><b className={s.income >= 0 ? 'up' : 'down'}>{(s.income >= 0 ? '+' : '') + fmtMoney(s.income)}</b></div>
      </div>
      <div className="chart-cap">Treasury, last {Math.min(h.money.length, 120)} days</div>
      <Chart data={h.money} color="rgb(244, 197, 66)" height={70} money />
    </Popover>
  )
}

function HelpPanel({ onClose }: { onClose: () => void }) {
  return (
    <Popover id="help" title="Controls" onClose={onClose}>
      <div className="keys">
        <div><b>W A S D</b> pan</div>
        <div><b>Q / E</b> or right-drag rotate</div>
        <div><b>Wheel</b> zoom · <b>middle-drag</b> pan</div>
        <div><b>Left-drag</b> lays roads or paints zones; a click places a service</div>
        <div><b>R V</b> road / avenue · <b>Z X C</b> zones · <b>B</b> bulldoze</div>
        <div><b>G H F T</b> raise / lower / level / trees · <b>[ ]</b> brush size</div>
        <div><b>Ctrl+Z</b> undo · <b>Space</b> pause · <b>1 2 3</b> speed</div>
        <div><b>Esc</b> back to inspect · <b>Home</b> recentre · <b>`</b> console</div>
      </div>
      <div className="save-row">
        <button className="wide" onClick={() => game.saveCity()}>Quick save</button>
        <button className="wide" onClick={() => game.loadCity()}>Load</button>
      </div>
      <button className="wide" onClick={() => game.unlock()}>Unlock every service (sandbox)</button>
    </Popover>
  )
}

function SelectionPanel({ s }: { s: Stats }) {
  const sel = s.selection
  const [last, setLast] = useState(sel)
  useEffect(() => {
    if (sel) setLast(sel)
  }, [sel])
  const shown = sel ?? last
  if (!shown) return null
  const pct = Math.round(shown.desirability * 100)
  const zoned = shown.kind === 'Residential' || shown.kind === 'Commercial' || shown.kind === 'Industrial'
  const svc = shown.service
  const title = svc ? svc.name
    : zoned
      ? shown.level === 0 ? `Vacant ${shown.kind.toLowerCase()} lot` : shown.kind === 'Residential' ? ['', 'Family home', 'Townhouse', 'Apartment block'][shown.level] : shown.kind === 'Commercial' ? ['', 'Corner shop', 'Office', 'Tower'][shown.level] : ['', 'Warehouse', 'Factory', 'Heavy plant'][shown.level]
      : shown.water && shown.kind !== 'Bridge' ? 'Water' : shown.kind === 'Road' ? 'Two-lane road' : shown.kind === 'Avenue' ? 'Four-lane avenue' : shown.kind === 'Bridge' ? 'Bridge' : shown.planted ? 'Grove' : 'Grassland'
  const iconClass = svc ? 'service ' + svc.id : shown.kind.toLowerCase()
  const covered = SERVICES.filter((_, i) => (shown.coverage & (1 << i)) !== 0)
  return (
    <section id="selection" className={'panel ' + (sel ? 'open' : 'closed')} data-ui>
      <div className="sel-head">
        <span className={'sel-icon ' + iconClass} />
        <div className="sel-title">
          <b>{title}</b>
          <small>{svc ? 'Service' : shown.kind} · tile {shown.x}, {shown.y}</small>
        </div>
        <button className="close" onClick={() => game.clearSelection()}>×</button>
      </div>
      {svc ? (
        <div className="sel-body">
          <div className="sel-row">
            <span>{svc.id === 'power' || svc.id === 'water' ? 'Load' : 'People reached'}</span>
            <span className="meter-row">
              <StatBar value={svc.demand} max={Math.max(1, svc.capacity)} />
              <b>{fmtNum(svc.demand)} / {fmtNum(svc.capacity)}</b>
            </span>
          </div>
          <div className="sel-row"><span>Tiles served</span><b>{svc.covered}</b></div>
          <div className="sel-row"><span>Upkeep</span><b>{fmtMoney(svc.upkeep)} / day</b></div>
          <div className="sel-row">
            <span>Road access</span>
            <b className={shown.access ? 'up' : 'down'}>{shown.access ? 'Connected' : 'No road: serves nobody'}</b>
          </div>
        </div>
      ) : zoned ? (
        <div className="sel-body">
          <div className="sel-row">
            <span>Level</span>
            <span className="stars">
              {[1, 2, 3].map((l) => (
                <i key={l} className={l <= shown.level ? 'on' : ''}>★</i>
              ))}
            </span>
          </div>
          <div className="sel-row">
            <span>Land value</span>
            <span className="meter">
              <span className="track"><span className="fill" style={{ transform: `scaleX(${(pct / 100).toFixed(2)})` }} /></span>
              <b>{pct}%</b>
            </span>
          </div>
          <div className="sel-row">
            <span>{shown.kind === 'Residential' ? 'Residents' : 'Jobs'}</span>
            <b>{shown.kind === 'Residential' ? shown.population : shown.jobs}</b>
          </div>
          <div className="sel-row">
            <span>Road access</span>
            <b className={shown.access ? 'up' : 'down'}>{shown.access ? 'Connected' : 'No road'}</b>
          </div>
          {shown.level > 0 && (
            <div className="sel-row">
              <span>Utilities</span>
              <b>
                <span className={shown.powered ? 'up' : 'down'}>{shown.powered ? 'Power' : 'No power'}</span>
                {' · '}
                <span className={shown.watered ? 'up' : 'down'}>{shown.watered ? 'Water' : 'No water'}</span>
              </b>
            </div>
          )}
          <div className="cov-list">
            {SERVICES.slice(2).map((info) => {
              const on = covered.some((c) => c.id === info.id)
              return <span key={info.id} className={'cov ' + info.id + (on ? ' on' : '')} title={info.name}>{info.name.split(' ').pop()}</span>
            })}
          </div>
        </div>
      ) : (
        <div className="sel-body">
          <p className="sel-note">
            {shown.kind === 'Bridge' ? 'A road on piers. Cars cross, boats pass underneath.'
              : shown.water ? 'Open water. Roads become bridges here; lower or raise the ground with the landscaping tools.'
              : shown.kind === 'Road' || shown.kind === 'Avenue' ? 'Zones next to this road get access and can grow; services travel along it.'
              : shown.planted ? 'A planted grove. Bulldoze to clear it.'
              : 'Unzoned land. Lay a road, then paint a zone beside it.'}
          </p>
          {(shown.kind === 'Road' || shown.kind === 'Avenue' || shown.kind === 'Bridge') && (
            <div className="cov-list">
              {SERVICES.map((info) => {
                const on = covered.some((c) => c.id === info.id)
                return <span key={info.id} className={'cov ' + info.id + (on ? ' on' : '')} title={info.name}>{info.name.split(' ').pop()}</span>
              })}
            </div>
          )}
        </div>
      )}
    </section>
  )
}

// Source-style drop-down developer console. The host owns the text buffer
// and history; this just paints the last lines and the prompt.
function Console({ c }: { c: ConsoleData }) {
  const lines = c.lines.slice(-26)
  return (
    <div id="console" className={c.open ? 'open' : ''} data-ui>
      <div className="con-head">
        <span>Developer console</span>
        <span className="con-hint">` close · ↑↓ history · Tab complete · help</span>
      </div>
      <div className="con-lines">
        {lines.map((l, i) => (
          <div key={c.lines.length - lines.length + i} className={'con-line l' + l[0]}>{l[1]}</div>
        ))}
      </div>
      <div className="con-input">
        <span className="con-prompt">]</span>
        <span className="con-text">{c.input}</span>
        <span className="con-caret" />
      </div>
    </div>
  )
}

export function App() {
  const { stats, toasts, map, connected, console: con, history, panelRequest } = useGame()
  const [intro, setIntro] = useState(true)
  const [panels, setPanels] = useState<Set<string>>(() => new Set())
  // The left-hand popovers share one spot, so opening one closes the others.
  const LEFT = ['info', 'economy', 'map', 'overlays', 'stats', 'policies']
  const toggle = (p: string) =>
    setPanels((prev) => {
      const next = new Set(prev)
      if (next.has(p)) next.delete(p)
      else {
        if (LEFT.includes(p)) for (const q of LEFT) next.delete(q)
        next.add(p)
      }
      return next
    })
  useEffect(() => {
    const t = window.setTimeout(() => setIntro(false), 3200)
    return () => window.clearTimeout(t)
  }, [])
  // The host can open panels for a capture without any input (CITYSLICE_PANELS).
  useEffect(() => {
    if (panelRequest.seq === 0) return
    setPanels(new Set(panelRequest.panels))
    setIntro(false)
  }, [panelRequest])
  // No state classes on the root: a class change there restyles and
  // relayouts the whole document (~90 ms), so paused/night live on the time bar.
  return (
    <div id="hud">
      <CityBar s={stats} panels={panels} toggle={toggle} />
      <Chirper toasts={toasts} open={panels.has('chirper')} />
      <TimeBar s={stats} />
      {panels.has('info') && <InfoPanel s={stats} onClose={() => toggle('info')} />}
      {panels.has('economy') && <EconomyPanel s={stats} h={history} onClose={() => toggle('economy')} />}
      {panels.has('stats') && <StatisticsPanel h={history} onClose={() => toggle('stats')} />}
      {panels.has('policies') && <PolicyPanel s={stats} onClose={() => toggle('policies')} />}
      {panels.has('overlays') && <OverlayPanel s={stats} onClose={() => toggle('overlays')} />}
      {panels.has('help') && <HelpPanel onClose={() => toggle('help')} />}
      {/* Always mounted: the canvas keeps its surface; hidden via CSS. */}
      <section id="map" className={'panel popover ' + (panels.has('map') ? '' : 'hidden')} data-ui>
        <div className="pop-head">
          <span>Map</span>
          <button className="close" onClick={() => toggle('map')}>×</button>
        </div>
        <div className="pop-body">
          <Minimap s={stats} size={map.size} cells={map.cells} visible={panels.has('map')} />
        </div>
      </section>
      <SelectionPanel s={stats} />
      <div id="corner" data-ui>
        <button className={'qb' + (panels.has('help') ? ' on' : '')} onClick={() => toggle('help')} title="Controls">
          <span className="glyph">?</span>
        </button>
      </div>
      <Toolbar s={stats} />
      <MilestoneBanner toasts={toasts} />
      <Console c={con} />
      {stats.speed === 0 && <div className="pause-mark">PAUSED</div>}
      {intro && (
        <div className="intro">
          <div className="intro-title">New Horizon</div>
          <div className="intro-sub">{connected ? 'A CitySlice vertical slice' : 'Connecting…'}</div>
        </div>
      )}
    </div>
  )
}
