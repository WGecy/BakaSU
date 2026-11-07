// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once
#include <stddef.h>
#include <stdint.h>

#include "yukilinker.h"
enum class BootCase {
  Success,
  RejectReady,
  RejectDependencies,
  RejectFinish,
  HoldConstructor,
  Reenter,
  CleanupFailure,
  ReusePacket,
  ForkTango,
  RejectNativeBinding
};
struct BootControl {
  BootCase mode = BootCase::Success;
  unsigned constructors = 0, entries = 0, adopted = 0, takes = 0, finishes = 0, finalized = 0;
  unsigned entered = 0, release = 0, exit_callbacks = 0;
  uintptr_t base = 0, loader_base = 0;
  size_t span = 0, dependencies = 0;
  int (*exercise)(const char*) = nullptr;
  int (*retire)() = nullptr;
  const void* api = nullptr;
  yukilinker_c_runtime* owner = nullptr;
  yukilinker_c_handle (*open)(yukilinker_c_runtime*, const char*, const yukilinker_c_open_options*,
                              yukilinker_c_error*) = nullptr;
  uint32_t (*query)(yukilinker_c_runtime*, yukilinker_c_handle,
                    yukilinker_c_handle_info*) = nullptr;
  yukilinker_c_close_result (*close)(yukilinker_c_runtime*, yukilinker_c_handle) = nullptr;
  void (*bootstrap)(int, int) = nullptr;
  int retry_fd = -1;
  int packet_argument = 0;
  unsigned packet_transfers = 0;
  void (*arm_cleanup_failure)() = nullptr;
  int reused_fd = -1;
  uint32_t tango_stub = 0, tango_size = 0;
  unsigned tango_finalized = 0;
  unsigned native_binds = 0, native_opens = 0;
};
