# Aperture UI Samples

Source code for the samples that ship with [Aperture UI](https://apertureui.wdstudios.tech), the HTML/CSS/JS UI
middleware for games. The samples are MIT licensed, so you can copy them into your own project.

## What each sample shows

| Sample | What it shows |
|---|---|
| `APPlayer` | The reference player. It loads HTML/CSS/JS and renders it with Direct3D 11 behind a Dear ImGui shell, or with Vulkan, and serves the Chrome DevTools frontend so you can inspect the running page. |
| `CitySlice` | A small city-builder game on SDL3 and Vulkan with a React HUD. It shows how a game feeds data to an Aperture UI page and renders the page on top of its own scene. |
| `minimal/hello-view` | The smallest complete program: create a view, load a page, handle a click and draw it every frame. |
| `minimal/data-binding` | Drive an HTML page from C++ data with `data-bind-*` attributes. |
| `minimal/input` | Move focus across a page with the keyboard and a gamepad, and activate buttons. |
| `minimal/pak-cache` | Keep compiled page scripts in a game's own cache with a custom script cache provider. |

## The SDK is not open source

Only the sample code in this repository is MIT licensed. The Aperture UI SDK that the samples build against is
proprietary. You need the SDK download, which comes with a free key from the portal at
https://apertureui.wdstudios.tech/portal. Your use of the SDK is governed by the Free Agreement or the Commercial
License you accepted there.

## Prerequisites

- Visual Studio 2026 with the C++ desktop workload
- CMake 4.2 or newer
- The Aperture UI SDK download, extracted somewhere on disk
- The [Vulkan SDK](https://vulkan.lunarg.com/), for CitySlice and the Vulkan mode of APPlayer
- Node 20 or newer, only if you want to rebuild the CitySlice HUD (the built HUD is checked in)

## Build

```
cmake -S . -B build -G "Visual Studio 18 2026" -DApertureUI_DIR=<sdk>/cmake
cmake --build build --config Release
```

`<sdk>` is the folder you extracted the SDK to. The build copies the SDK's DLLs, V8 resources, shaders and data next to
each executable, taken from the SDK that `ApertureUI_DIR` names, so pointing the same build folder at another SDK
stages that one. Add `-DAPUI_SAMPLES_STAGE_PDBS=ON` to copy the SDK's `.pdb` files too (a Dev package has gigabytes of
them). Use `-DAPUI_SAMPLES_APPLAYER=OFF`, `-DAPUI_SAMPLES_CITYSLICE=OFF` or `-DAPUI_SAMPLES_MINIMAL=OFF` to skip a
group of samples, for example CitySlice when the Vulkan SDK is not installed.

The DLLs pass C++ standard library types across their boundary, so the samples use the C++ runtime of the package:
the release runtime (`/MD`) for a Dev or Shipping package, even in a Debug build of the sample, and the debug runtime
(`/MDd`) for a Debug package.

## Run

Run each sample from its build folder, where the build put the SDK's DLLs. Do not put the SDK's `bin` folder on
PATH instead: Windows loads `System32\icuuc.dll` before anything on PATH, and the sample fails to start.

The SDK checks for a licence at start-up. Give the sample your licence settings on the command line or in the
environment:

| Command line | Environment variable | Use |
|---|---|---|
| `--license-key <key>` | `APUI_LICENSE_KEY` | Development builds: your key from the portal |
| `--project-id <uuid>` | `APUI_PROJECT_ID` | Development builds: the project the key is for |
| `--certificate <path>` | `APUI_CERTIFICATE` | Shipping builds: the `aperture.license` file |

Command-line options win over environment variables. See https://apertureui.wdstudios.tech/docs/licensing for how
keys, projects and certificates work.

A flag takes its value as `--flag=value` or as the next argument. A next argument that starts with `-` is taken for
the next option, never for a value (give such a value with `=`), and the sample reports the flag as having no value.
Flag names ignore letter case. Pass secrets as `--flag=value`: if the flag name is mistyped, the sample then logs an
unknown option by name only, whereas in `--flag value` the key would be read as the page to open and logged with it.

### APPlayer

`apertureplayer <file or http(s) URL>` opens a page; the options are listed at the top of `APPlayer/App.cpp`. F11
toggles fullscreen, and F12 turns DevTools on and off (http://127.0.0.1:9222/). The player writes
`apertureplayer.log`, `aperture_player_state.json` and `aperture_player_imgui.ini` beside its executable.

Earlier players drew layout outlines with F1 and accepted `--no-layout-debug`. The public API has no switch for them,
so this player has neither (it logs `--no-layout-debug` as an unknown option). The nearest substitute is the
"Box Model" overlay on the Inspector's Debug tab, in Debug and Dev builds of the SDK.

## Attribution

A title released under the Free Agreement has to credit Aperture UI. Section 5(4) of the agreement
(https://apertureui.wdstudios.tech/docs/legal/free-agreement) says:

> (4) **Attribution.** Licensee must show the following attribution, verbatim except that the bracketed words are replaced by the Title's name, together with the Aperture UI logo, on the Title's startup screen or in the Title's "license.txt" file:
>
> > [This Product] uses Aperture UI, a HTML/CSS/JS UI Middleware for games. Aperture UI is made and copyrighted by WD Studios LLC. All rights reserved.
>
> Licensee must also ship the file THIRD_PARTY_NOTICES.md from the SDK package, unchanged, with every build of a Title that it distributes.

The text above is quoted for convenience; the agreement you accepted is the authority. The `THIRD_PARTY_NOTICES.md` it
requires is the one inside the SDK package. The one in this repository covers only what the samples themselves use.

## Contributing

Pull requests are welcome. Every pull request runs the checks in `.github/workflows/checks.yml`, and they must pass:

```
python -W error -m unittest discover -s tools -p "test_*.py"
python tools/check_headers.py .
python tools/forbidden_scan.py .
gitleaks git --config .gitleaks.toml --redact --exit-code 2 .
```

- `check_headers.py` requires every source file to start with the MIT copyright and SPDX header, written as a
  comment of the file's own language.
- `forbidden_scan.py` rejects strings, file names and folder names that must not appear in a public repository, and
  committed build output (`.dll`, `.lib`, `.pdb`, `.exe` and the like). Its patterns are in `tools/forbidden.txt`. A
  reviewed exception goes in `tools/forbidden_allow.txt`: either a line `path:line:pattern` copied from the scan's
  output, or `@identifier NAME` for a name the SDK defines, which is then allowed wherever it appears as a whole
  identifier.
- `gitleaks` scans the whole history for secrets and for Aperture UI licence keys.
- The CMake tests need `cmake`; on Windows they run with `APUI_TEST_CMAKE=1` because the Visual Studio generator
  makes each configure slow.

By contributing you agree that your contribution is licensed under the MIT License in this repository.

Third-party code used by the samples is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
