# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Rouyashiki <https://rouyashiki.org>

# enable_language must run in the calling directory, at file scope. A macro
# supplies that scope even when the SDK was added in a child directory.
macro(yukilinker_add_runtime_sources core_target)
  enable_language(CXX ASM)
  _yukilinker_add_runtime_sources("${core_target}")
endmacro()

# Keep implementation variables scoped while compiling directly in Core.
function(_yukilinker_add_runtime_sources core_target)
  if(NOT TARGET "${core_target}")
    message(FATAL_ERROR "YukiLinker: unknown Core target '${core_target}'")
  endif()
  get_target_property(form "${core_target}" YUKILINKER_RUNTIME_FORM)
  if(form)
    message(FATAL_ERROR "YukiLinker runtime was already added to ${core_target}")
  endif()
  set_property(TARGET "${core_target}" PROPERTY YUKILINKER_RUNTIME_FORM sources)
  foreach(setting SOURCES DEFINITIONS OPTIONS LINK_OPTIONS)
    get_property(runtime_${setting} GLOBAL PROPERTY YUKILINKER_RUNTIME_${setting})
  endforeach()
  target_sources("${core_target}" PRIVATE ${runtime_SOURCES})
  target_link_libraries("${core_target}" PRIVATE YukiLinker::headers dl)
  target_link_options("${core_target}" PRIVATE ${runtime_LINK_OPTIONS})
  target_compile_features("${core_target}" PRIVATE cxx_std_17)
  # Source properties are directory-scoped. Guard them by the consuming target so a
  # bootstrap or archive in that same directory still gets its own build settings.
  foreach(option IN LISTS runtime_OPTIONS)
    set_property(SOURCE ${runtime_SOURCES} TARGET_DIRECTORY "${core_target}" APPEND
      PROPERTY COMPILE_OPTIONS "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,${core_target}>:${option}>")
  endforeach()
  foreach(definition IN LISTS runtime_DEFINITIONS)
    set_property(SOURCE ${runtime_SOURCES} TARGET_DIRECTORY "${core_target}" APPEND
      PROPERTY COMPILE_DEFINITIONS "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,${core_target}>:${definition}>")
  endforeach()
endfunction()
