// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <pthread.h>

namespace yukilinker::internal {

enum class CleanupState { None, Registered, Pending, Calling, Cleared, Uncertain };

// Lives with the resource being released, never in a callback's stack frame.
// Calling is published before releasing the operation lock. A thread/fork that
// loses that call cannot infer whether the external reference was consumed.
struct CleanupCall {
  CleanupState state = CleanupState::None;
  pthread_t thread{};
};

struct DsoCleanup {
  void* cookie = nullptr;
  CleanupCall call;
  DsoCleanup* next = nullptr;
  bool draining = false;
  pthread_t draining_thread{};
};

}  // namespace yukilinker::internal
