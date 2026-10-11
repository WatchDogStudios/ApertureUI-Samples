/*
 * store.ts - the bridge between the C++ game loop and React.
 *
 * The host pushes `window.__cityStats({...})` five times a second and
 * `window.__cityMap(n, chars)` once a second; React subscribes through
 * useSyncExternalStore. Calls back into the game go through the engine's
 * namespaced binder functions on `window.game` (all arguments are strings).
 */
import { useSyncExternalStore } from 'react'

export type ToolName =
  | 'select' | 'road' | 'avenue' | 'res' | 'com' | 'ind' | 'bulldoze'
  | 'power' | 'water' | 'health' | 'edu' | 'police' | 'park' | 'transit'
  | 'raise' | 'lower' | 'flatten' | 'trees'

export type ServiceId = 'power' | 'water' | 'health' | 'edu' | 'police' | 'park' | 'transit'

export interface ServiceInfo {
  id: ServiceId
  name: string
  cost: number
  upkeep: number
  radius: number
  capacity: number
  unit: string
  unlock: number // milestone index (0 = from the start)
  blurb: string
}

// Mirrors City::kServices in City.cpp (order = ServiceKind = coverage bit).
export const SERVICES: ServiceInfo[] = [
  { id: 'power', name: 'Coal power plant', cost: 3000, upkeep: 120, radius: 26, capacity: 900, unit: 'kW', unlock: 0,
    blurb: 'Powers every building within 26 road tiles. Homes stop growing without electricity.' },
  { id: 'water', name: 'Water tower', cost: 1500, upkeep: 60, radius: 24, capacity: 600, unit: 'm³', unlock: 0,
    blurb: 'Fresh water for 24 road tiles around it. Dry taps make for unhappy citizens.' },
  { id: 'health', name: 'Medical clinic', cost: 2200, upkeep: 90, radius: 12, capacity: 400, unit: 'patients', unlock: 1,
    blurb: 'Raises land value and happiness for homes it can reach.' },
  { id: 'edu', name: 'Elementary school', cost: 2600, upkeep: 110, radius: 12, capacity: 300, unit: 'pupils', unlock: 1,
    blurb: 'Homes need a school nearby to grow past level 2.' },
  { id: 'police', name: 'Police station', cost: 1800, upkeep: 80, radius: 14, capacity: 500, unit: 'people', unlock: 2,
    blurb: 'Keeps shops and homes safe; everyone in reach is a little happier.' },
  { id: 'park', name: 'City park', cost: 600, upkeep: 20, radius: 6, capacity: 250, unit: 'visitors', unlock: 2,
    blurb: 'A small green with a pond and a fountain. Big land value boost next door.' },
  { id: 'transit', name: 'Bus depot', cost: 2400, upkeep: 100, radius: 20, capacity: 800, unit: 'passengers', unlock: 3,
    blurb: 'Bus lines along every road within 20 tiles. Shops need transit to reach level 3.' },
]

export interface PolicyInfo {
  id: PolicyId
  bit: number
  name: string
  blurb: string
  cost: string
}

export type PolicyId = 'recycling' | 'transit' | 'education' | 'holiday' | 'lighting'

// Mirrors City::kPolicies in City.cpp; index = bit position in Stats.policies.
export const POLICIES: PolicyInfo[] = [
  { id: 'recycling', bit: 1, name: 'Recycling programme',
    blurb: 'Kerbside collection. Industry upsets its neighbours less and everyone is a little happier.',
    cost: '$40/day + $0.60 per building' },
  { id: 'transit', bit: 2, name: 'Free public transport',
    blurb: 'Fares abolished. Bus coverage counts double and shops draw more custom.',
    cost: '$60/day + $0.50 per building' },
  { id: 'education', bit: 4, name: 'Education grant',
    blurb: 'Extra funding per pupil. Homes near a school grow 35% faster.',
    cost: '$50/day + $0.40 per building' },
  { id: 'holiday', bit: 8, name: 'Small business holiday',
    blurb: 'Commercial tax halved while it stands. Shops open 30% faster in return.',
    cost: '$0.90 per building' },
  { id: 'lighting', bit: 16, name: 'Night lighting',
    blurb: 'Lit streets everywhere. Safer nights, paid for by the metre of road.',
    cost: '$20/day + $0.12 per road tile' },
]

export interface Selection {
  x: number
  y: number
  kind: string
  level: number
  desirability: number
  population: number
  jobs: number
  access: boolean
  water: boolean
  coverage: number
  powered: boolean
  watered: boolean
  service: { id: ServiceId; name: string; capacity: number; demand: number; covered: number; upkeep: number } | null
  planted: boolean
  bridge: boolean
}

export interface ServiceStats {
  id: ServiceId
  buildings: number
  capacity: number
  demand: number
  covered: number
}

export interface Stats {
  money: number
  income: number
  population: number
  jobs: number
  roads: number
  buildings: number
  day: number
  hour: number
  speed: number
  demandR: number
  demandC: number
  demandI: number
  tax: number
  happiness: number
  budget: { res: number; com: number; ind: number; upkeep: number; services: number; policy: number }
  tool: ToolName
  hover: string
  cars: number
  camX: number
  camY: number
  camYaw: number
  fps: number
  frameMs: number
  milestone: number
  unpowered: number
  unwatered: number
  canUndo: boolean
  policies: number
  policyCost: number
  brush: number
  overlay: number
  services: ServiceStats[]
  selection: Selection | null
  events: string[]
}

export interface Toast {
  id: number
  text: string
  kind: 'info' | 'milestone' | 'warn'
  born: number
}

/// One point per in-game day, oldest first. Pushed on its own slow channel
/// because it only changes when the date does.
export interface History {
  population: number[]
  money: number[]
  happiness: number[]
  income: number[]
  jobs: number[]
}

export interface ConsoleData {
  open: boolean
  input: string
  lines: [number, string][]
}

export interface State {
  stats: Stats
  toasts: Toast[]
  map: { size: number; cells: string }
  connected: boolean
  console: ConsoleData
  history: History
  /// Panels the host asked to open (CITYSLICE_PANELS). Bumped, not merged, so
  /// the app can tell a repeat request from no request.
  panelRequest: { seq: number; panels: string[] }
}

const initialStats: Stats = {
  money: 25000, income: 0, population: 0, jobs: 0, roads: 0, buildings: 0, day: 0, hour: 8,
  speed: 1, demandR: 1, demandC: 0.5, demandI: 0.5, tax: 10, happiness: 70,
  budget: { res: 0, com: 0, ind: 0, upkeep: 0, services: 0, policy: 0 }, tool: 'select', hover: '', cars: 0,
  camX: 0.5, camY: 0.5, camYaw: 0, fps: 0, frameMs: 0, milestone: 0, unpowered: 0, unwatered: 0,
  canUndo: false, policies: 0, policyCost: 0, brush: 2, overlay: -1, services: [], selection: null, events: [],
}

const emptyHistory: History = { population: [], money: [], happiness: [], income: [], jobs: [] }

let state: State = {
  stats: initialStats, toasts: [], map: { size: 0, cells: '' }, connected: false,
  console: { open: false, input: '', lines: [] }, history: emptyHistory,
  panelRequest: { seq: 0, panels: [] },
}
const listeners = new Set<() => void>()
let nextToastId = 1

function emit() {
  for (const l of listeners) l()
}

function pushToast(text: string) {
  const kind: Toast['kind'] = text.startsWith('Milestone') ? 'milestone'
    : /^(Not enough|Cannot|That tile|.* needs a road)/.test(text) || text.includes('unlocks at') ? 'warn' : 'info'
  const toast: Toast = { id: nextToastId++, text, kind, born: Date.now() }
  state = { ...state, toasts: [...state.toasts.slice(-3), toast] }
  window.setTimeout(() => {
    state = { ...state, toasts: state.toasts.filter((t) => t.id !== toast.id) }
    emit()
  }, kind === 'milestone' ? 7000 : 4500)
}

// Every committed DOM change costs the engine a restyle+relayout pass, so the
// counters that churn during growth are committed at most twice a second;
// interaction-driven fields stay immediate.
const NOISY: (keyof Stats)[] = ['population', 'jobs', 'buildings', 'cars', 'demandR', 'demandC',
  'demandI', 'happiness', 'money', 'income', 'day', 'hour', 'budget', 'fps', 'frameMs', 'services',
  'unpowered', 'unwatered']
let lastNoisyCommit = 0

function applyStats(incoming: Stats) {
  const now = Date.now()
  let s = incoming
  if (state.connected && now - lastNoisyCommit < 500) {
    const merged = { ...incoming } as Stats
    for (const k of NOISY) (merged as unknown as Record<string, unknown>)[k] = state.stats[k]
    s = merged
  } else {
    lastNoisyCommit = now
  }
  state = { ...state, stats: s, connected: true }
  if (incoming.events && incoming.events.length) {
    for (const e of incoming.events) pushToast(e)
  }
  emit()
}

function applyMap(n: number, cells: string) {
  if (state.map.size === n && state.map.cells === cells) return // unchanged: no re-render, no canvas redraw
  state = { ...state, map: { size: n, cells } }
  emit()
}

function applyHistory(h: History) {
  state = { ...state, history: h }
  emit()
}

function applyPanels(p: { panels: string[] }) {
  state = { ...state, panelRequest: { seq: state.panelRequest.seq + 1, panels: p.panels ?? [] } }
  emit()
}

function applyConsole(c: ConsoleData) {
  state = { ...state, console: c }
  emit()
}

export function installNativeBridge() {
  const w = window as unknown as {
    __cityStats?: (s: Stats) => void
    __cityMap?: (n: number, cells: string) => void
    __cityMapJson?: (m: { size: number; cells: string }) => void
    __cityConsole?: (c: ConsoleData) => void
    __cityHistory?: (h: History) => void
    __cityPanels?: (p: { panels: string[] }) => void
  }
  w.__cityStats = applyStats
  w.__cityMap = applyMap
  w.__cityMapJson = (m) => applyMap(m.size, m.cells)
  w.__cityConsole = applyConsole
  w.__cityHistory = applyHistory
  w.__cityPanels = applyPanels

  // The host also mirrors every push into attributes on #bridge. The DOM is
  // shared even if the module script and the host's executeJavaScript run in
  // different realms, so polling it makes the bridge realm-proof.
  let lastStats = ''
  let lastMap = ''
  window.setInterval(() => {
    const el = document.getElementById('bridge')
    if (!el) return
    const st = el.getAttribute('data-stats')
    if (st && st !== lastStats) {
      lastStats = st
      try { applyStats(JSON.parse(st) as Stats) } catch (e) { /* partial write; next tick */ }
    }
    const mp = el.getAttribute('data-map')
    if (mp && mp !== lastMap) {
      lastMap = mp
      const sep = mp.indexOf(':')
      if (sep > 0) applyMap(parseInt(mp.slice(0, sep), 10), mp.slice(sep + 1))
    }
  }, 100)
}

export function useGame(): State {
  return useSyncExternalStore(
    (cb) => {
      listeners.add(cb)
      return () => listeners.delete(cb)
    },
    () => state,
  )
}

// ---- calls into C++ (engine binder: every argument crosses as a string) ----
type GameApi = {
  setTool(name: string): void
  setSpeed(speed: string): void
  newCity(): void
  adjustTax(delta: string): void
  lookAt(x: string, y: string): void
  clearSelection(): void
  echo(text: string): void
  undo(): void
  setBrush(radius: string): void
  setOverlay(kind: string): void
  unlock(): void
  togglePolicy(id: string): void
  saveCity(): void
  loadCity(): void
}

function api(): GameApi | undefined {
  return (window as unknown as { game?: GameApi }).game
}

export const game = {
  setTool: (t: ToolName) => api()?.setTool(t),
  setSpeed: (s: number) => api()?.setSpeed(String(s)),
  newCity: () => api()?.newCity(),
  adjustTax: (d: number) => api()?.adjustTax(String(d)),
  lookAt: (x: number, y: number) => api()?.lookAt(x.toFixed(3), y.toFixed(3)),
  clearSelection: () => api()?.clearSelection(),
  echo: (text: string) => api()?.echo(text),
  undo: () => api()?.undo(),
  setBrush: (r: number) => api()?.setBrush(String(r)),
  setOverlay: (kind: number) => api()?.setOverlay(String(kind)),
  unlock: () => api()?.unlock(),
  togglePolicy: (id: PolicyId) => api()?.togglePolicy(id),
  saveCity: () => api()?.saveCity(),
  loadCity: () => api()?.loadCity(),
}
