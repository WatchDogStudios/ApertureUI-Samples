# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.28) # FetchContent_Declare(EXCLUDE_FROM_ALL), cmake -E copy_directory_if_different
# Standalone only. CMAKE_PROJECT_NAME is a cache entry, so testing it would skip project() on every re-configure.
# The C++ runtime is chosen in cmake/ApertureUISamples.cmake from the SDK package's build type.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ApertureUISamples LANGUAGES C CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  enable_testing()
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(ApertureUISamples)

option(APUI_SAMPLES_APPLAYER "Build APPlayer" ON)
option(APUI_SAMPLES_CITYSLICE "Build CitySlice (needs the Vulkan SDK)" ON)
option(APUI_SAMPLES_MINIMAL "Build the minimal samples" ON)

add_subdirectory(common)
if(APUI_SAMPLES_APPLAYER)
  add_subdirectory(APPlayer)
endif()
if(APUI_SAMPLES_CITYSLICE)
  add_subdirectory(CitySlice)
endif()
if(APUI_SAMPLES_MINIMAL)
  add_subdirectory(minimal)
endif()
