# Third-party notices

The samples in this repository are MIT licensed (see `LICENSE`). They download, link or bundle the third-party
components below, each under its own licence. The Aperture UI SDK has its own notices file in the SDK download.

| Component | Version | Licence | Used by |
|---|---|---|---|
| [SDL2](https://github.com/libsdl-org/SDL) | SDL 2.32.2, commit `e11183ea6caa3ae4895f4bc54cad2bbb0e365417` | zlib | APPlayer only, through `common/`'s `apui_sample_sdl2_input` (fetched at configure time, and only when APPlayer is built) |
| [SDL3](https://github.com/libsdl-org/SDL) | `release-3.2.16` | zlib | CitySlice and the minimal samples (fetched at configure time) |
| [Dear ImGui](https://github.com/ocornut/imgui) | commit `64944b4520b30772de8dbf0b37d0311746477a32` of the `docking` branch (1.93.0 WIP) | MIT | APPlayer (fetched at configure time) |
| stb `stb_rect_pack` 1.01, `stb_textedit` 1.14, `stb_truetype` 1.26 (bundled in Dear ImGui as `imstb_*.h`) | as bundled with the Dear ImGui commit above | MIT or public domain, at your choice | APPlayer, through Dear ImGui |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | `v1.2.1` | zlib | APPlayer (fetched at configure time) |
| [cgltf](https://github.com/jkuhlmann/cgltf) | `v1.15` | MIT | CitySlice (fetched at configure time) |
| [React](https://github.com/facebook/react) `react` | 19.2.8 | MIT | CitySlice HUD, bundled in `CitySlice/UI/dist` |
| React `react-dom` | 19.2.8 | MIT | CitySlice HUD, bundled in `CitySlice/UI/dist` |
| React `scheduler` | 0.27.0 | MIT | CitySlice HUD, bundled in `CitySlice/UI/dist` |

## SDL2 and SDL3

```
Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## nativefiledialog-extended

nativefiledialog-extended is distributed by its authors and contributors under the zlib licence. Its licence file
carries no separate copyright line.

```
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Dear ImGui

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## stb libraries bundled in Dear ImGui

Dear ImGui carries slightly modified copies of three single-file libraries by Sean Barrett: `imstb_rectpack.h`
(stb_rect_pack 1.01), `imstb_textedit.h` (stb_textedit 1.14) and `imstb_truetype.h` (stb_truetype 1.26). Each is
available under two licences, MIT or public domain, whichever you prefer. The MIT alternative:

```
Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## cgltf

```
Copyright (c) 2018-2021 Johannes Kuhlmann

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## React, React DOM and Scheduler

The built HUD in `CitySlice/UI/dist` contains `react` 19.2.8, `react-dom` 19.2.8 and `scheduler` 0.27.0.

The HUD is built with Vite 7.3.6 (MIT, Copyright (c) 2019-present, VoidZero Inc. and Vite contributors). No Vite code
is in the bundle: the build sets `modulePreload: false`, so Vite's `modulepreload` polyfill is not emitted, and
the `modulepreload` string in `assets/app.js` belongs to React DOM's resource-hint support.

```
MIT License

Copyright (c) Meta Platforms, Inc. and affiliates.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
