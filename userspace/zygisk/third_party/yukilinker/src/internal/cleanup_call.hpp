// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include "lifecycle_model.hpp"
#include "operation_guards.hpp"
#if defined(YUKILINKER_RUNTIME)
#include "module_state.hpp"
#endif

namespace yukilinker::internal {

inline bool cleanup_failure(uint32_t code) {
  if (g_operation_error) g_operation_error->cleanup_incomplete = 1;
  return fail(code, YUKILINKER_C_RETIREMENT);
}

// Callers hold the operation lock. Only this checked scope admits external
// cleanup; the pinned image and persistent record survive reentry and abandonment.
template <typename Invoke>
bool cleanup_call(ImageState* image, CleanupCall& call, Invoke invoke,
                  yukilinker_c_error_detail* output = nullptr) {
  const auto refused = [&](uint32_t code) {
    if (!output) return cleanup_failure(code);
    output->code = code;
    output->stage = YUKILINKER_C_RETIREMENT;
    output->cleanup_incomplete = 1;
    return false;
  };
  if (call.state == CleanupState::Cleared) return true;
  if (exit_cleanup_uncertain() || call.state == CleanupState::Calling ||
      call.state == CleanupState::Uncertain)
    return refused(YUKILINKER_C_BUSY);
  call.state = CleanupState::Pending;
  bool returned = false;
  {
    CallbackGuard scope(
        image, false,
        [](ImageState*, void* argument) {
          auto& pending = *static_cast<CleanupCall*>(argument);
          pending.thread = pthread_self();
          pending.state = CleanupState::Calling;
#if defined(YUKILINKER_RUNTIME)
          modules_note_system_call_locked();
#endif
        },
        &call);
#if YUKILINKER_FULL
    if (scope.linked && scope.saved_state)
#endif
    {
      invoke();
      returned = true;
    }
  }
  if (!returned) return refused(YUKILINKER_C_IO_ERROR);
  call.state = CleanupState::Cleared;
  return true;
}

}  // namespace yukilinker::internal
