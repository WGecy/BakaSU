// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../include/yukilinker.h"
#include "config.hpp"
#include "image_state.hpp"
#include "loader_types.hpp"

namespace yukilinker::internal {

extern uint64_t g_event_sequence;
constexpr size_t kRuntimeEventCapacity = 256;
extern yukilinker_c_runtime_event g_runtime_events[kRuntimeEventCapacity];

// origin names what an event without an image is about (a host import's library), or nothing.
static inline void record_event(const ImageState* image, uint32_t type,
                                uint32_t code = YUKILINKER_C_OK, const char* origin = nullptr) {
  if (g_event_sequence == UINT64_MAX) return;
  yukilinker_c_runtime_event event{};
  event.sequence = ++g_event_sequence;
  event.type = type;
  event.error_code = code;
  if (image) {
    event.id = {image->generation, image->namespace_id};
    event.phase = static_cast<uint32_t>(image->phase);
    (void)snprintf(event.origin, sizeof(event.origin), "%s", image->origin);
  } else if (origin) {
    (void)snprintf(event.origin, sizeof(event.origin), "%s", origin);
  }
  g_runtime_events[(event.sequence - 1) % kRuntimeEventCapacity] = event;
}

}  // namespace yukilinker::internal
