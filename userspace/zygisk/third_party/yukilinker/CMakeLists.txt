# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Rouyashiki <https://rouyashiki.org>

cmake_minimum_required(VERSION 3.22)
project(YukiLinker VERSION 1.0.0 LANGUAGES C CXX ASM)

include(cmake/YukiLinkerOptions.cmake)

if(NOT ANDROID)
  message(FATAL_ERROR "YukiLinker requires an Android NDK toolchain")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Debug info and __FILE__ name the tree under fixed virtual roots, so an archive does not depend
# on where the source, the build directory or the NDK lie. The roots are absolute: with a relative
# source root the debugger joined it to comp_dir (build/./src/...). The build directory comes last:
# it lies inside the source tree, and the last matching map wins. gdb's substitute-path takes the
# first matching rule, so map /yukilinker/build before /yukilinker.
add_compile_options(
  "$<$<COMPILE_LANGUAGE:C,CXX,ASM>:-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=/yukilinker>"
  "$<$<COMPILE_LANGUAGE:C,CXX,ASM>:-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=/yukilinker/build>")
if(CMAKE_ANDROID_NDK)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,ASM>:-ffile-prefix-map=${CMAKE_ANDROID_NDK}=/ndk>")
endif()

include(cmake/YukiLinkerTargets.cmake)

include(GNUInstallDirs)
# Both ABIs install into one prefix: libraries and exported targets go to per-ABI directories,
# so the second install does not replace the first one's targets file. The key is the one
# cmake/YukiLinkerTargets.cmake selects sources with; CMake's own Android platform does not set
# ANDROID_ABI, and an empty key would let the two ABIs overwrite each other.
if(NOT CMAKE_ANDROID_ARCH_ABI)
  message(FATAL_ERROR "YukiLinker install needs CMAKE_ANDROID_ARCH_ABI")
endif()
set(YUKILINKER_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR}/${CMAKE_ANDROID_ARCH_ABI})
set(YUKILINKER_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/YukiLinker)
install(FILES include/yukilinker.h include/yukilinker.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS yukilinker_headers yukilinker_runtime
  EXPORT YukiLinkerTargets
  ARCHIVE DESTINATION ${YUKILINKER_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${YUKILINKER_INSTALL_LIBDIR})
if(TARGET yukilinker_bootstrap)
  install(TARGETS yukilinker_bootstrap EXPORT YukiLinkerTargets
    ARCHIVE DESTINATION ${YUKILINKER_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${YUKILINKER_INSTALL_LIBDIR})
endif()
install(EXPORT YukiLinkerTargets NAMESPACE YukiLinker::
  DESTINATION ${YUKILINKER_INSTALL_CMAKEDIR}/${CMAKE_ANDROID_ARCH_ABI})

# install(EXPORT) alone writes only the targets file; without this config
# find_package(YukiLinker) has nothing to find. The config is the same for every ABI and picks
# the consumer's ABI. The version file lets a consumer ask for a compatible major version; it is
# ARCH_INDEPENDENT because both ABIs share it (the default would reject the other pointer size).
include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/YukiLinkerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/YukiLinkerConfig.cmake
  INSTALL_DESTINATION ${YUKILINKER_INSTALL_CMAKEDIR})
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/YukiLinkerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)
# CMake's template still writes the producer's pointer size into the check that ARCH_INDEPENDENT
# skips (an early return() precedes it), so the two ABIs' files differ in dead code. Both install
# to one place; the check is dropped here so either ABI writes the same file.
file(READ ${CMAKE_CURRENT_BINARY_DIR}/YukiLinkerConfigVersion.cmake YUKILINKER_VERSION_FILE)
string(FIND "${YUKILINKER_VERSION_FILE}" "# if the installed or the using project don't have"
       YUKILINKER_ARCH_CHECK)
if(YUKILINKER_ARCH_CHECK GREATER -1)
  string(SUBSTRING "${YUKILINKER_VERSION_FILE}" 0 ${YUKILINKER_ARCH_CHECK}
         YUKILINKER_VERSION_FILE)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/YukiLinkerConfigVersion.cmake
       "${YUKILINKER_VERSION_FILE}")
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/YukiLinkerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/YukiLinkerConfigVersion.cmake
  DESTINATION ${YUKILINKER_INSTALL_CMAKEDIR})
