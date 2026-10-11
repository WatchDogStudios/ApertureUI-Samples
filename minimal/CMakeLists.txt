# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT

add_library(apui_minimal_host STATIC MinimalHost.cpp)
target_include_directories(apui_minimal_host PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(apui_minimal_host PRIVATE apui_sample_common apui_sample_sdl2_input)
apui_sample_link(apui_minimal_host)
apui_sample_target(apui_minimal_host)

function(add_minimal_sample target directory)
  add_executable(${target} ${directory}/main.cpp)
  target_link_libraries(${target} PRIVATE apui_minimal_host)
  apui_sample_link(${target})
  apui_sample_target(${target})
  apui_sample_stage(${target} DATA ${CMAKE_CURRENT_LIST_DIR}/${directory})
endfunction()

add_minimal_sample(minimal_hello_view hello-view)
add_minimal_sample(minimal_data_binding data-binding)
add_minimal_sample(minimal_input input)