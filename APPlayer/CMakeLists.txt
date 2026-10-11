# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
#
# apertureplayer: the reference player. Dear ImGui (docking branch, pinned) and nativefiledialog-extended are
# fetched at configure time; SDL2 comes from common/ (apui_sample_sdl2_input).

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 64944b4520b30772de8dbf0b37d0311746477a32 # docking branch, 1.93.0 WIP
  GIT_SHALLOW FALSE
)
FetchContent_Declare(
  nfd
  GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
  GIT_TAG v1.2.1
  GIT_SHALLOW TRUE
)
set(NFD_PORTAL OFF CACHE BOOL "" FORCE)
# Dear ImGui has no CMakeLists.txt, so this only downloads it; the library is built below.
FetchContent_MakeAvailable(imgui nfd)

if(NOT TARGET imgui_aplayer)
  add_library(imgui_aplayer STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
  target_include_directories(imgui_aplayer PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_aplayer PUBLIC 3rdParty::sdl2 d3d11 dxgi)
endif()

add_executable(apertureplayer WIN32
  App.cpp Options.cpp Shell.cpp Toolbar.cpp FileBrowser.cpp LogPanel.cpp Inspector.cpp LiveEditor.cpp State.cpp)
# Advapi32: RegGetValueW (dark mode). The Vulkan shaders are a dependency of apui_sample_common, which
# holds the Vulkan backend, so they are built before this target in-tree too.
target_link_libraries(apertureplayer PRIVATE apui_sample_common apui_sample_sdl2_input imgui_aplayer nfd Advapi32)
apui_sample_link(apertureplayer)
apui_sample_target(apertureplayer)
apui_sample_stage(apertureplayer DATA ${CMAKE_CURRENT_LIST_DIR}/Data)

# The command-line parser alone: no SDK, so the test needs no SDK DLLs beside it.
add_executable(apertureplayer_options_test test/OptionsTest.cpp Options.cpp)
apui_sample_target(apertureplayer_options_test)
add_test(NAME apertureplayer_options COMMAND apertureplayer_options_test)
