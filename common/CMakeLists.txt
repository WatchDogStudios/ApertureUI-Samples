# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
#
# apui_sample_common: host code the samples share. Licence settings, the D3D11 and Vulkan render contexts,
# gamepad navigation and Media Foundation video decoding.
# apui_sample_sdl2_input: SDL2InputAdapter and SDL2, for APPlayer only. It is a separate target because SDL2 and
# SDL3 define the same SDL_* symbols, so SDL2 must never reach a sample that links SDL3. Without APPlayer, SDL2 is
# not downloaded at all.

find_package(Vulkan QUIET)
add_library(apui_sample_common STATIC
  LicenseArgs.cpp StartSdk.cpp D3D11Context.cpp GamepadNavigator.cpp VideoDecoder.cpp)
set(shaders "")
if(Vulkan_FOUND)
  target_sources(apui_sample_common PRIVATE VulkanContext.cpp)
  target_link_libraries(apui_sample_common PUBLIC Vulkan::Vulkan)
  target_compile_definitions(apui_sample_common PUBLIC APERTURE_HAS_VULKAN=1)
  set(shaders VULKAN_SHADERS) # VulkanContext loads Shaders/Vulkan/*.spv
endif()
target_include_directories(apui_sample_common PUBLIC ${CMAKE_CURRENT_LIST_DIR})
apui_sample_link(apui_sample_common ${shaders})
apui_sample_target(apui_sample_common)
target_link_libraries(apui_sample_common PUBLIC d3d11 dxgi d3dcompiler mf mfplat mfuuid)

if(APUI_SAMPLES_APPLAYER OR APUI_SAMPLES_MINIMAL)
  # SDL2 with the engine's tag and options. In-tree the engine may have added it already; FetchContent then reuses
  # that copy and the alias exists.
  set(SDL_INSTALL OFF CACHE BOOL "Disable SDL install targets" FORCE)
  set(SDL_INSTALL_CMAKEDIR "" CACHE STRING "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
  set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
  set(SDL_TEST OFF CACHE BOOL "Build SDL tests" FORCE)
  include(FetchContent)
  FetchContent_Declare(
    sdl2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG e11183ea6caa3ae4895f4bc54cad2bbb0e365417 # release-2.32.2
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(sdl2)
  if(NOT TARGET 3rdParty::sdl2)
    add_library(3rdParty::sdl2 ALIAS SDL2-static)
  endif()

  add_library(apui_sample_sdl2_input STATIC SDL2InputAdapter.cpp)
  target_include_directories(apui_sample_sdl2_input PUBLIC ${CMAKE_CURRENT_LIST_DIR})
  apui_sample_link(apui_sample_sdl2_input)
  apui_sample_target(apui_sample_sdl2_input)
  target_link_libraries(apui_sample_sdl2_input PUBLIC 3rdParty::sdl2)
endif()

# The parser alone: no SDK, so the test needs no SDK DLLs beside it.
add_executable(apui_license_args_test test/LicenseArgsTest.cpp LicenseArgs.cpp)
apui_sample_target(apui_license_args_test)
add_test(NAME license_args COMMAND apui_license_args_test)
