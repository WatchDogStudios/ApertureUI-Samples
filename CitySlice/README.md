# CitySlice

A Cities: Skylines-style vertical slice: lay roads, paint residential /
commercial / industrial zones, watch buildings grow with RCI demand, run a toy
economy, and see traffic drive the network. The HUD is an Aperture UI HTML
document; the world is Vulkan with shaders written in Slang.

| Piece | Where |
|---|---|
| Window + input (no SDL_GPU) | SDL3 3.2.16, fetched at configure time |
| Vulkan device / swapchain / UI | `APPlayer/APPlayerVulkanContext` reused as-is, plus its `SetPreSceneCallback` (own passes before the swapchain pass) and `SetSceneCallback` (post pass, inside it) hooks |
| World renderer | `CityRenderer.cpp` (passes) + `CityModels.cpp` (procedural meshes): water simulation (compute), 2048² sun shadow map, HDR scene pass (RGBA16F colour + normal/roughness + depth, 4x MSAA) with PBR, tile-gridded spotlights and subsurface scattering, then a post pass with screen-space reflections and ACES tonemapping |
| Shaders | `Shaders/city.slang` (scene), `post.slang` (SSR + tonemap), `water.slang` (wave equation), compiled by `slangc` at build time |
| Simulation | `City.*` (engine-free; `cityslice --selftest` runs its checks): zones, services with road-network coverage and capacity pools, bridges, avenues, terrain sculpting, undo, traffic, boats |
| HUD | `UI/` (React + Vite, `npm run build` -> `UI/dist`), talks to C++ via the binder's `game.*` functions |

## Playing

Lay roads (drag; across water they become bridges on piers), paint zones
beside them, and place a **power plant** and a **water tower** next to a road:
every building within their road reach draws from a shared pool, and buildings
without power or water stop growing and blink a marker. Clinics, schools,
police, parks and bus depots unlock with milestones and raise land value
along the roads they reach (`Show coverage` in each service panel, or the
Info views button). Landscaping raises, lowers and levels the ground (dig a
lake, reclaim a shore) and plants groves. Ctrl+Z undoes the last action. The
lake reacts to the cursor, to construction and to the boats.

## Build

The target is part of the `PartialCore` filter and needs the Vulkan SDK
(`slangc` ships with it).

```bat
cmake --build E:\aperture\Workspace\vs2026x64 --config Dev --target cityslice -- -m:4 /p:UserRootDir=E:\tmp\noprops
```

Run `Output\Bin\WinVs2022Dev64\cityslice.exe` from that directory: the UI
backend loads `Shaders/Vulkan/*.spv` and `Data/Base/Fonts` relative to the exe.

## Controls

| Input | Action |
|---|---|
| WASD / arrows | pan |
| Q / E or right-drag | rotate |
| wheel | zoom |
| middle-drag | pan |
| left-drag | draw an L-shaped road / paint a zone rectangle / bulldoze / level / plant; click places a service; hold sculpts |
| R V Z X C B, Esc | road, avenue, residential, commercial, industrial, bulldoze, deselect |
| G H F T, [ ] | raise, lower, level, trees; brush size |
| Ctrl+Z | undo |
| Space, 1 2 3 | pause, speed |

## How the frame works

1. SDL events go to the `UIView` first; a hit element with a `data-ui`
   ancestor means the HUD owns the click, otherwise the map tool does.
2. `City::Tick`, `UIView::tick`, and every 100 ms a stats object is pushed to
   `window.__cityStats(...)` through `executeJavaScriptRaw`.
3. `BeginFrame` on the Vulkan backend begins the swapchain render pass and calls
   the scene callback; `CityRenderer::Record` draws the world depth-tested.
4. The backend then records the HUD command list depth-off on top and presents.
