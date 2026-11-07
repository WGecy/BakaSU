// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <pthread.h>

namespace yukilinker::internal {
struct ModuleState;
// All hooks run under the operation lock. Abandoned calls keep a bounded,
// queryable uncertain wrapper; the unknown system reference is never retried.
bool modules_has_holders();
void modules_abandon_thread(pthread_t thread);
void modules_after_fork();
void modules_free_state();
// Records that a PRIVATE operation also crossed the opaque system loader.
void modules_note_system_call();
// Internal transition with the operation mutex already held, including the
// CallbackGuard window after the TLS depth was saved but before unlocking.
void modules_note_system_call_locked();
}  // namespace yukilinker::internal
