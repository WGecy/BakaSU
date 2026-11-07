# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Rouyashiki <https://rouyashiki.org>

option(YUKILINKER_BUILD_BOOTSTRAP "Build the Core bootstrap loader" ON)
option(YUKILINKER_CFI "Instrument the Core runtime with cross-DSO CFI" OFF)
set(YUKILINKER_SANITIZER "" CACHE STRING "Optional NDK sanitizer")
set(YUKILINKER_SUPPORTED_ABIS arm64-v8a armeabi-v7a)
set_property(CACHE YUKILINKER_SANITIZER PROPERTY STRINGS "" address hwaddress undefined thread)
set(_yukilinker_sanitizers address hwaddress undefined thread)
if(NOT YUKILINKER_SANITIZER STREQUAL "" AND
   NOT YUKILINKER_SANITIZER IN_LIST _yukilinker_sanitizers)
  message(FATAL_ERROR "Unsupported YUKILINKER_SANITIZER: ${YUKILINKER_SANITIZER}")
endif()
