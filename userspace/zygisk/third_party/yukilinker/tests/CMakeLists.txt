# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Rouyashiki <https://rouyashiki.org>

cmake_minimum_required(VERSION 3.22)
project(YukiLinkerHosts LANGUAGES C CXX ASM)
enable_testing()
add_subdirectory(.. sdk)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_library(linker_module SHARED module.c)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module.map"
  "MODULE_API { global: module_answer; __cfi_check*; local: *; };\n")
target_link_options(linker_module PRIVATE
  "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/module.map")
set_property(TARGET linker_module APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/module.map")
add_library(linker_lifecycle SHARED lifecycle_module.c)
target_compile_options(linker_lifecycle PRIVATE -fno-emulated-tls)
foreach(module linker_module linker_lifecycle)
  target_link_options(${module} PRIVATE -Wl,-z,max-page-size=16384)
endforeach()
if(YUKILINKER_CFI)
  target_compile_options(linker_module PRIVATE -flto -fvisibility=hidden
    -fsanitize=cfi-icall -fsanitize-cfi-cross-dso)
  target_link_options(linker_module PRIVATE -flto -fsanitize=cfi-icall -fsanitize-cfi-cross-dso)
endif()

add_executable(c_host c_host.c)
target_link_libraries(c_host PRIVATE YukiLinker::runtime dl)
target_compile_options(c_host PRIVATE -Wall -Wextra -pedantic -Werror -UNDEBUG)
add_executable(cpp_host cpp_host.cpp)
target_link_libraries(cpp_host PRIVATE YukiLinker::runtime dl)
target_compile_options(cpp_host PRIVATE -UNDEBUG)
target_link_options(cpp_host PRIVATE -Wl,--wrap=munmap)
foreach(host c_host cpp_host)
  if(YUKILINKER_CFI)
    target_compile_definitions(${host} PRIVATE YUKILINKER_TEST_CFI=1)
    target_compile_options(${host} PRIVATE -flto -fvisibility=hidden
      -fsanitize=cfi-icall -fsanitize-cfi-cross-dso)
  endif()
  add_test(NAME ${host} COMMAND ${host} "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

add_executable(bootstrap_host bootstrap_host.cpp)
target_link_libraries(bootstrap_host PRIVATE YukiLinker::headers dl)
target_compile_options(bootstrap_host PRIVATE -UNDEBUG)
target_link_options(bootstrap_host PRIVATE -Wl,--export-dynamic)
if(TARGET YukiLinker::bootstrap AND NOT YUKILINKER_CFI)
  # Extra version records exercise metadata unmap failure on both pointer widths.
  set(core_versions "CORE { global: zygisk_*; local: *; };\n")
  foreach(index RANGE 1 256)
    string(APPEND core_versions "RECORD_${index} {};\n")
  endforeach()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/core.map" "${core_versions}")
  foreach(form archive sources)
    add_library(core_${form} SHARED core.cpp)
    set_target_properties(core_${form} PROPERTIES OUTPUT_NAME linker_core_${form}
      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_compile_options(core_${form} PRIVATE -UNDEBUG -fno-stack-protector
      -fno-c++-static-destructors)
    target_link_options(core_${form} PRIVATE -nostdlib++
      "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/core.map")
    if(form STREQUAL "archive")
      target_link_libraries(core_${form} PRIVATE YukiLinker::runtime)
    else()
      yukilinker_add_runtime_sources(core_${form})
    endif()
    target_link_libraries(core_${form} PRIVATE dl)
  endforeach()
  add_test(NAME bootstrap_host COMMAND bootstrap_host "${CMAKE_CURRENT_BINARY_DIR}")
endif()
