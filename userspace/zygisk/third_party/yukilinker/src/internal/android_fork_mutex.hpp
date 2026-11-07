// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#endif

namespace yukilinker::internal {

struct ForkMutexRecovery {
  uintptr_t inherited_depth;
  uintptr_t restored_depth;
};

// Android-specific child recovery for the SDK's recursive operation mutex only.
// Bionic checks its saved owner TID on recursive unlock, and the child's TID is
// different. The parent's waiters do not exist in this address space. Normal
// prepare locks are actually unlocked and never pass through this adapter.
inline ForkMutexRecovery recover_android_fork_mutex(pthread_mutex_t* mutex, uintptr_t depth) {
  ForkMutexRecovery result{depth, 0};
#if __has_feature(thread_sanitizer)
  // Model the release caused by replacing the inherited recursive lock, then
  // retire that object's detector identity. Never use linker_init (it ignores
  // destroy), and never mask an unexpected recursion count.
  const int observed = __tsan_mutex_pre_unlock(mutex, __tsan_mutex_recursive_unlock);
  __tsan_mutex_post_unlock(mutex, 0);
  if (observed < 0 || static_cast<uintptr_t>(observed) != depth) abort();
  __tsan_mutex_destroy(mutex, 0);
#endif
  pthread_mutex_t fresh = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
  memcpy(mutex, &fresh, sizeof(fresh));
#if __has_feature(thread_sanitizer)
  __tsan_mutex_create(mutex, __tsan_mutex_write_reentrant);
#endif
  for (; result.restored_depth < depth; ++result.restored_depth)
    if (pthread_mutex_lock(mutex) != 0) abort();
  return result;
}

}  // namespace yukilinker::internal
