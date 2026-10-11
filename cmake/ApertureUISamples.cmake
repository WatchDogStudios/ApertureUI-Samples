# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
#
# Dual-mode linking. Inside the Aperture UI engine tree the engine's own targets exist and are linked
# directly; anywhere else the SDK download is found with find_package(ApertureUI).

if(TARGET apertureuihtmlengine)
  set(APUI_SAMPLES_IN_TREE ON)
else()
  set(APUI_SAMPLES_IN_TREE OFF)
  option(APUI_SAMPLES_STAGE_PDBS "Also copy the SDK's .pdb files next to each sample (a Dev package holds gigabytes of them)" OFF)
  find_package(ApertureUI CONFIG)
  if(NOT ApertureUI_FOUND)
    message(FATAL_ERROR
      "Aperture UI SDK not found. Download the SDK from https://apertureui.wdstudios.tech/portal (a free key is enough), "
      "extract it, and pass -DApertureUI_DIR=<extracted folder>/cmake")
  endif()

  # The SDK folder comes from ApertureUI_DIR on every configure. The package's own APERTUREUI_*_DIR variables are
  # not used: a package generated before they became plain variables caches the first SDK's paths, so switching
  # SDKs would link one package and stage another.
  get_filename_component(APUI_SDK_ROOT "${ApertureUI_DIR}/.." ABSOLUTE)
  foreach(sub bin bin/resources shaders data)
    if(NOT IS_DIRECTORY "${APUI_SDK_ROOT}/${sub}")
      message(FATAL_ERROR
        "The Aperture UI SDK at ${APUI_SDK_ROOT} has no '${sub}' folder. Extract the SDK download again, "
        "and pass -DApertureUI_DIR=<extracted folder>/cmake")
    endif()
  endforeach()

  # The C++ runtime has to match the package: the DLLs pass standard library types across, and Dev and
  # Shipping packages use the release runtime (/MD) while a Debug package uses the debug runtime (/MDd).
  if(NOT TARGET ApertureUI::HTMLEngine)
    message(FATAL_ERROR "${ApertureUI_DIR} does not define ApertureUI::HTMLEngine. Use the config from the SDK download.")
  endif()
  get_target_property(_apui_defs ApertureUI::HTMLEngine INTERFACE_COMPILE_DEFINITIONS)
  if(_apui_defs MATCHES "BUILDSYSTEM_BUILDTYPE_([A-Za-z]+)")
    set(APUI_SDK_BUILD_TYPE "${CMAKE_MATCH_1}")
  else()
    message(FATAL_ERROR "${ApertureUI_DIR} exports no BUILDSYSTEM_BUILDTYPE_* definition, so the C++ runtime for the "
      "samples cannot be chosen. Use the config from the SDK download.")
  endif()
  if(APUI_SDK_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDebugDLL)
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
  endif()
  message(STATUS "Aperture UI SDK: ${APUI_SDK_ROOT} (${APUI_SDK_BUILD_TYPE} package)")
endif()

# Link the engine into <target>. The SDK's imported targets carry the include directories and compile
# definitions the public headers need, so nothing else is required to compile against them.
# VULKAN_SHADERS: the sample loads the engine's Vulkan shaders. In-tree they are compiled by the engine target
# apertureui_vulkan_shaders, which is defined after the samples and only when slangc is installed, so the
# dependency is added at the end of the top-level directory if that target exists then, and a warning says so
# if it does not; the SDK ships them precompiled.
function(apui_sample_link target)
  cmake_parse_arguments(ARG "VULKAN_SHADERS" "" "" ${ARGN})
  if(APUI_SAMPLES_IN_TREE)
    # The core targets are named unconditionally: Foundation is added after the Applications folder, so it does
    # not exist yet when a sample is configured. Only the optional engines are tested for.
    target_link_libraries(${target} PRIVATE apertureuihtmlengine apertureuiharrlowengine Foundation)
    foreach(t apertureuideveloperengine apertureuinetwork)
      if(TARGET ${t})
        target_link_libraries(${target} PRIVATE ${t})
      endif()
    endforeach()
    if(ARG_VULKAN_SHADERS)
      set_property(GLOBAL APPEND PROPERTY APUI_SAMPLES_VULKAN_SHADER_USERS ${target})
      get_property(scheduled GLOBAL PROPERTY APUI_SAMPLES_VULKAN_SHADERS_SCHEDULED)
      if(NOT scheduled)
        set_property(GLOBAL PROPERTY APUI_SAMPLES_VULKAN_SHADERS_SCHEDULED ON)
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _apui_samples_add_vulkan_shader_deps)
      endif()
    endif()
  else()
    target_link_libraries(${target} PRIVATE ApertureUI::HTMLEngine ApertureUI::HarrlowEngine ApertureUI::Foundation
      ApertureUI::DeveloperEngine ApertureUI::Network)
  endif()
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endfunction()

# Runs once, at the end of the top-level directory (see apui_sample_link). Like the engine's own player, a
# missing shader target is a warning, not an error: the build works and the Vulkan backend reports the missing
# .spv files when it starts.
function(_apui_samples_add_vulkan_shader_deps)
  get_property(users GLOBAL PROPERTY APUI_SAMPLES_VULKAN_SHADER_USERS)
  if(TARGET apertureui_vulkan_shaders)
    foreach(t IN LISTS users)
      add_dependencies(${t} apertureui_vulkan_shaders)
    endforeach()
  else()
    list(JOIN users ", " names)
    message(WARNING "The engine defines no apertureui_vulkan_shaders target (it needs slangc from the Vulkan SDK), "
      "so the Vulkan shaders are not built for ${names}.")
  endif()
endfunction()

# Copy what the exe needs at run time next to it. In-tree the engine already writes every DLL and the
# shaders into the shared output folder, so only sample data is copied there. Standalone, the SDK's DLLs,
# V8 resources, shaders and data are copied; only changed files are written, so an incremental build is cheap.
# A Dev package's bin folder also holds gigabytes of PDBs; they are copied only with -DAPUI_SAMPLES_STAGE_PDBS=ON.
function(apui_sample_stage target)
  cmake_parse_arguments(ARG "" "" "DATA" ${ARGN})
  set(dest "$<TARGET_FILE_DIR:${target}>")
  if(NOT APUI_SAMPLES_IN_TREE)
    file(GLOB runtime_files "${APUI_SDK_ROOT}/bin/*.dll")
    if(NOT runtime_files)
      message(FATAL_ERROR "The Aperture UI SDK at ${APUI_SDK_ROOT} has no DLLs in bin/. Extract the SDK download again.")
    endif()
    if(APUI_SAMPLES_STAGE_PDBS)
      file(GLOB pdb_files "${APUI_SDK_ROOT}/bin/*.pdb")
      list(APPEND runtime_files ${pdb_files})
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${runtime_files} "${dest}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${APUI_SDK_ROOT}/bin/resources" "${dest}/resources"
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${APUI_SDK_ROOT}/shaders" "${dest}/Shaders"
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${APUI_SDK_ROOT}/data" "${dest}/Data"
      VERBATIM)
  endif()
  foreach(d IN LISTS ARG_DATA)
    get_filename_component(d "${d}" ABSOLUTE)  # relative paths are relative to the calling CMakeLists.txt
    string(REGEX REPLACE "[/\\\\]+$" "" d "${d}")  # a trailing slash would make the folder's name empty
    if(NOT IS_DIRECTORY "${d}")
      message(FATAL_ERROR "apui_sample_stage(${target}): DATA folder '${d}' does not exist")
    endif()
    get_filename_component(name "${d}" NAME)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${d}" "${dest}/${name}" VERBATIM)
  endforeach()
endfunction()

# Every sample target is C++17. In-tree targets also keep the engine's output folder, so they sit beside the
# engine DLLs.
function(apui_sample_target target)
  set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  if(APUI_SAMPLES_IN_TREE AND COMMAND ns_set_default_target_output_dirs)
    ns_set_default_target_output_dirs(${target})
  endif()
endfunction()
