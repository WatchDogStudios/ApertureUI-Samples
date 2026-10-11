ns_cmake_init()

# CitySlice: Cities: Skylines-style vertical slice. SDL3 window/input, Vulkan
# through the APPlayer Vulkan backend (reused as-is, plus its scene hook), world
# shaders written in Slang, HUD rendered by Aperture UI.
ns_requires_desktop()

set(PROJECT_NAME cityslice)

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  message(STATUS "cityslice: Vulkan SDK not found, target skipped")
  return()
endif()

find_program(CITYSLICE_SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT CITYSLICE_SLANGC)
  message(STATUS "cityslice: slangc not found (ships with the Vulkan SDK), target skipped")
  return()
endif()

ns_create_target(APPLICATION ${PROJECT_NAME} NO_PCH NO_UNITY NO_WARNINGS_AS_ERRORS)

# The Aperture Vulkan backend lives in APPlayer; compile it into this target
# rather than forking it. Its SPIR-V comes from the shader target APPlayer's
# CMakeLists declares (processed first: subdirectories are added alphabetically).
set(APPLAYER_DIR ${CMAKE_SOURCE_DIR}/Code/Applications/APPlayer)
target_sources(${PROJECT_NAME} PRIVATE
  ${APPLAYER_DIR}/APPlayerVulkanContext.cpp
  # DevTools: the player's view bridge + CDP data provider, so the same
  # inspector/console server is available while the game runs.
  ${APPLAYER_DIR}/APPlayerDevToolsBridge.cpp
  ${APPLAYER_DIR}/APPlayerDevToolsProvider.cpp
  ${APPLAYER_DIR}/APPlayerScriptRuntime.cpp
  ${APPLAYER_DIR}/RecorderService.cpp)
if(NOT TARGET apertureuideveloperengine)
  message(STATUS "cityslice: apertureuideveloperengine not in this build, target skipped")
  return()
endif()
if(TARGET apertureui_vulkan_shaders)
  add_dependencies(${PROJECT_NAME} apertureui_vulkan_shaders)
else()
  message(WARNING "cityslice: apertureui_vulkan_shaders target missing; the UI backend will find no SPIR-V")
endif()

# ── SDL3 (windowing + input only; no SDL_GPU) ────────────────────────────
set(CITYSLICE_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/Code/ThirdParty/.PulledLibs")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(
  sdl3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.2.16
  GIT_SHALLOW    TRUE
  SOURCE_DIR     "${CITYSLICE_3RDPARTY_DIR}/sdl3"
)
FetchContent_GetProperties(sdl3)
if(NOT sdl3_POPULATED)
  FetchContent_Populate(sdl3)
  add_subdirectory(${sdl3_SOURCE_DIR} ${sdl3_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
set_target_properties(SDL3-static PROPERTIES FOLDER "ThirdParty")

# cgltf (header-only, MIT) for the Blender-authored model library. Shared with
# the Vantage sample: FetchContent is idempotent when it is already populated.
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG        v1.15
  GIT_SHALLOW    TRUE
  SOURCE_DIR     "${CITYSLICE_3RDPARTY_DIR}/cgltf")
FetchContent_GetProperties(cgltf)
if(NOT cgltf_POPULATED)
  FetchContent_Populate(cgltf)
endif()

target_include_directories(${PROJECT_NAME}
  PRIVATE
    ${cgltf_SOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${APPLAYER_DIR}
    ${CMAKE_SOURCE_DIR}/Code/ApertureUISource
    ${CMAKE_SOURCE_DIR}/Code
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    apertureuihtmlengine
    apertureuiharrlowengine
    Foundation
    SDL3::SDL3-static
    Vulkan::Vulkan
    apertureuideveloperengine
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE
    APERTURE_HAS_VULKAN=1
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    CITYSLICE_UI_DIR="${CMAKE_CURRENT_LIST_DIR}/UI/dist"
    CITYSLICE_ASSET_DIR="${CMAKE_CURRENT_LIST_DIR}/Assets"
)

# ── Slang → SPIR-V ───────────────────────────────────────────────────────
# Compiled into the build tree, then staged next to the exe (the per-config
# runtime dir is only known as a generator expression).
set(CITYSLICE_SHADER_DIR ${CMAKE_CURRENT_LIST_DIR}/Shaders)
set(CITYSLICE_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/spv)
set(CITYSLICE_SPV
  ${CITYSLICE_SHADER_OUT}/city.vert.spv ${CITYSLICE_SHADER_OUT}/city.frag.spv
  ${CITYSLICE_SHADER_OUT}/post.vert.spv ${CITYSLICE_SHADER_OUT}/post.frag.spv
  ${CITYSLICE_SHADER_OUT}/water.comp.spv)
add_custom_command(
  OUTPUT ${CITYSLICE_SPV}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CITYSLICE_SHADER_OUT}
  COMMAND ${CITYSLICE_SLANGC} ${CITYSLICE_SHADER_DIR}/city.slang -target spirv -profile spirv_1_3
          -entry vsMain -stage vertex -o ${CITYSLICE_SHADER_OUT}/city.vert.spv
  COMMAND ${CITYSLICE_SLANGC} ${CITYSLICE_SHADER_DIR}/city.slang -target spirv -profile spirv_1_3
          -entry psMain -stage fragment -o ${CITYSLICE_SHADER_OUT}/city.frag.spv
  COMMAND ${CITYSLICE_SLANGC} ${CITYSLICE_SHADER_DIR}/post.slang -target spirv -profile spirv_1_3
          -entry vsPost -stage vertex -o ${CITYSLICE_SHADER_OUT}/post.vert.spv
  COMMAND ${CITYSLICE_SLANGC} ${CITYSLICE_SHADER_DIR}/post.slang -target spirv -profile spirv_1_3
          -entry psPost -stage fragment -o ${CITYSLICE_SHADER_OUT}/post.frag.spv
  COMMAND ${CITYSLICE_SLANGC} ${CITYSLICE_SHADER_DIR}/water.slang -target spirv -profile spirv_1_3
          -entry csWater -stage compute -o ${CITYSLICE_SHADER_OUT}/water.comp.spv
  DEPENDS ${CITYSLICE_SHADER_DIR}/city.slang ${CITYSLICE_SHADER_DIR}/post.slang ${CITYSLICE_SHADER_DIR}/water.slang
  COMMENT "cityslice: compiling city/post/water.slang with slangc"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_shaders DEPENDS ${CITYSLICE_SPV})
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_shaders)
set_target_properties(${PROJECT_NAME}_shaders PROPERTIES FOLDER "Applications")

# Stage both shader sets next to the exe: ours under Shaders/City, and the UI
# backend's pre-compiled SPIR-V under Shaders/Vulkan (loaded through the engine
# file system, rooted at the exe directory).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CITYSLICE_SHADER_OUT} $<TARGET_FILE_DIR:${PROJECT_NAME}>/Shaders/City
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/Data/Shaders/Vulkan $<TARGET_FILE_DIR:${PROJECT_NAME}>/Shaders/Vulkan
  COMMENT "cityslice: staging shaders next to the executable")

# ── HUD (React + Vite) ────────────────────────────────────────────────────
# The HUD is a React app under UI/; the engine loads UI/dist/index.html. Build
# it with `npm install && npm run build` in that folder, or via this target.
find_program(CITYSLICE_NPM npm)
if(CITYSLICE_NPM)
  add_custom_target(${PROJECT_NAME}_ui
    COMMAND ${CITYSLICE_NPM} run build
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/UI
    COMMENT "cityslice: building the React HUD (UI/dist)")
  set_target_properties(${PROJECT_NAME}_ui PROPERTIES FOLDER "Applications")
endif()
