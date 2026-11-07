# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Rouyashiki <https://rouyashiki.org>

# YUKILINKER_SUPPORTED_ABIS (YukiLinkerOptions.cmake) is the one allowlist; each entry needs its
# branch below.
if(NOT CMAKE_ANDROID_ARCH_ABI IN_LIST YUKILINKER_SUPPORTED_ABIS)
  string(REPLACE ";" " and " _yukilinker_abis "${YUKILINKER_SUPPORTED_ABIS}")
  message(FATAL_ERROR "YukiLinker supports ${_yukilinker_abis} only, not "
    "'${CMAKE_ANDROID_ARCH_ABI}'")
endif()
if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
  set(YUKILINKER_ABI_SUFFIX 64)
  set(YUKILINKER_ARCH_SOURCES src/arch/arm64/tlsdesc.S src/arch/arm64/caller_stub.S)
  set(YUKILINKER_BARRIER_SOURCES src/arch/arm64/call_barrier.S)
  # Every object of the runtime and the archives, assembly included, carries BTI and PAC. The
  # linker ANDs the GNU property note over all its inputs, so this only makes the note possible:
  # a consuming Core keeps it only if all of its own
  # objects carry it too.
  set(YUKILINKER_BRANCH_PROTECTION -mbranch-protection=standard)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
  set(YUKILINKER_ABI_SUFFIX 32)
  set(YUKILINKER_ARCH_SOURCES src/arch/arm32/caller_stub.S)
  set(YUKILINKER_BARRIER_SOURCES src/arch/arm32/call_barrier.S)
  set(YUKILINKER_BRANCH_PROTECTION)
else()
  message(FATAL_ERROR "YUKILINKER_SUPPORTED_ABIS lists ${CMAKE_ANDROID_ARCH_ABI} without sources")
endif()

add_library(yukilinker_headers INTERFACE)
add_library(YukiLinker::headers ALIAS yukilinker_headers)
target_include_directories(yukilinker_headers INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

# Shared by the runtime and the reduced bootstrap loader.
set(YUKILINKER_LOADER_SOURCES
  src/api/loader_api.cpp
  src/dependency/dependencies.cpp
  src/elf/metadata.cpp
  src/elf/properties.cpp
  src/enumeration/image_views.cpp
  src/host/default_host_services.cpp
  src/host/host_services.cpp
  src/lifecycle/lifecycle.cpp
  src/lifecycle/hwasan_globals.cpp
  src/memory/mapping.cpp
  src/relocation/relocation_engine.cpp
  src/runtime/runtime.cpp
  src/runtime/runtime_state.cpp
  src/tls/tls_manager.cpp
  ${YUKILINKER_BARRIER_SOURCES})

# Full runtime state is compiled into each consuming Core, never into the bootstrap.
set(YUKILINKER_RUNTIME_SOURCES
  ${YUKILINKER_LOADER_SOURCES}
  src/api/c_api.cpp
  src/api/caller_stubs.cpp
  src/api/owner_api.cpp
  src/api/module_api.cpp
  src/dependency/host_imports.cpp
  src/enumeration/phdr_bridge.cpp
  src/enumeration/program_headers.cpp
  src/registry/registry.cpp
  ${YUKILINKER_ARCH_SOURCES})

# The two integration routes use exactly the same source list and build settings.
list(TRANSFORM YUKILINKER_RUNTIME_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(YUKILINKER_RUNTIME_DEFINITIONS YUKILINKER_FULL=1 YUKILINKER_RUNTIME=1)
set(YUKILINKER_RUNTIME_OPTIONS
  -fPIC -Os -g -fno-exceptions -fno-rtti -fno-stack-protector -fno-c++-static-destructors
  -ffunction-sections -fdata-sections -fvisibility=hidden ${YUKILINKER_BRANCH_PROTECTION})
set(YUKILINKER_RUNTIME_LINK_OPTIONS -Wl,--gc-sections -Wl,--as-needed
  -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
if(YUKILINKER_SANITIZER)
  list(APPEND YUKILINKER_RUNTIME_OPTIONS -fsanitize=${YUKILINKER_SANITIZER} -fno-omit-frame-pointer)
  list(APPEND YUKILINKER_RUNTIME_LINK_OPTIONS -fsanitize=${YUKILINKER_SANITIZER})
endif()
if(YUKILINKER_CFI)
  list(APPEND YUKILINKER_RUNTIME_DEFINITIONS YUKILINKER_CFI=1)
  list(APPEND YUKILINKER_RUNTIME_OPTIONS -flto -fsanitize=cfi-icall -fsanitize-cfi-cross-dso -fsanitize-trap=cfi-icall)
  list(APPEND YUKILINKER_RUNTIME_LINK_OPTIONS -flto -fsanitize=cfi-icall -fsanitize-cfi-cross-dso -fsanitize-trap=cfi-icall)
endif()

add_library(yukilinker_runtime STATIC ${YUKILINKER_RUNTIME_SOURCES})
add_library(YukiLinker::runtime ALIAS yukilinker_runtime)
set_target_properties(yukilinker_runtime PROPERTIES
  OUTPUT_NAME "ylrt${YUKILINKER_ABI_SUFFIX}" POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  INTERFACE_YUKILINKER_RUNTIME_FORM archive)
set_property(TARGET yukilinker_runtime APPEND PROPERTY COMPATIBLE_INTERFACE_STRING YUKILINKER_RUNTIME_FORM)
target_link_libraries(yukilinker_runtime PUBLIC YukiLinker::headers PRIVATE dl)
target_compile_definitions(yukilinker_runtime PRIVATE ${YUKILINKER_RUNTIME_DEFINITIONS})
target_compile_options(yukilinker_runtime PRIVATE ${YUKILINKER_RUNTIME_OPTIONS})
target_link_options(yukilinker_runtime INTERFACE ${YUKILINKER_RUNTIME_LINK_OPTIONS})

# Functions called from a consumer directory must not depend on that directory's variables.
foreach(setting SOURCES DEFINITIONS OPTIONS LINK_OPTIONS)
  set_property(GLOBAL PROPERTY YUKILINKER_RUNTIME_${setting} "${YUKILINKER_RUNTIME_${setting}}")
endforeach()
include(${CMAKE_CURRENT_LIST_DIR}/YukiLinkerRuntimeSources.cmake)

if(YUKILINKER_BUILD_BOOTSTRAP)
  add_library(yukilinker_bootstrap SHARED
    src/bootstrap/bootstrap_loader.cpp src/bootstrap/legacy_packet.cpp
    ${YUKILINKER_LOADER_SOURCES})
  add_library(YukiLinker::bootstrap ALIAS yukilinker_bootstrap)
  set_target_properties(yukilinker_bootstrap PROPERTIES
    OUTPUT_NAME "yloader${YUKILINKER_ABI_SUFFIX}"
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  target_link_libraries(yukilinker_bootstrap PUBLIC YukiLinker::headers PRIVATE dl)
  target_compile_definitions(yukilinker_bootstrap PRIVATE YUKILINKER_FULL=0 YUKILINKER_BOOTSTRAP=1)
  target_compile_options(yukilinker_bootstrap PRIVATE
    -fPIC -Os -g -fno-exceptions -fno-rtti -fno-stack-protector -fno-c++-static-destructors
    -ffunction-sections -fdata-sections -fvisibility=hidden ${YUKILINKER_BRANCH_PROTECTION})
  target_link_options(yukilinker_bootstrap PRIVATE -nostdlib++ -Wl,--gc-sections -Wl,--as-needed
    -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/exports.map")
  set_property(TARGET yukilinker_bootstrap APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/exports.map)
  if(YUKILINKER_SANITIZER)
    target_compile_options(yukilinker_bootstrap PRIVATE -fsanitize=${YUKILINKER_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(yukilinker_bootstrap PRIVATE -fsanitize=${YUKILINKER_SANITIZER})
  endif()
endif()
