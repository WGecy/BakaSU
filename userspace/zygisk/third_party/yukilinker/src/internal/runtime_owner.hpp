// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <pthread.h>

#include <cstdint>

#include "../../include/yukilinker.h"
#include "config.hpp"
#include "loader_types.hpp"
#include "module_state.hpp"

namespace yukilinker::internal {

struct Runtime {
  static constexpr uint64_t kMagic = UINT64_C(0x59554b4952544d31);
  uint64_t magic = kMagic;
  yukilinker_c_host_services host_services{};
  ModuleState* modules = nullptr;
  // False from the step past which a shutdown cannot be refused (ShutdownPhase::kDestroying):
  // only try_shutdown and runtime_destroy accept the owner then, and they resume it.
  bool active = true;
  // Set once try_shutdown retires the runtime; only runtime_destroy accepts the owner then.
  bool shut_down = false;
};

extern Runtime* g_runtime_owner;

#if YUKILINKER_FULL
// The runtime's own retirement (try_shutdown and runtime_destroy). Its checks never
// drop the lock, so a refused shutdown stays kActive and changes nothing. kDestroying from the
// first step that changes anything, and for good: the default context is sealed, the owner admits
// only the calls that resume, and a shutdown that stopped short (a failed unmap, another call in
// flight, a thread that ended in a destructor, a fork) is resumed by the next one, each step at
// most once. kShutDown once the keys are gone.
enum class ShutdownPhase : uint8_t { kActive, kDestroying, kShutDown };
extern ShutdownPhase g_shutdown_phase;
// Set while a call resumes the kDestroying steps, which drop the lock (destructors, the host's
// dlclose); another call meanwhile, or one from those callbacks, is told to retry. A thread that
// ends inside them, or a fork child without that thread, clears it: every step left is resumable.
extern bool g_shutdown_running;
extern pthread_t g_shutdown_thread;
// Set once the system linker failed to close a library the runtime held for this owner: a host
// import (a namespace_destroy's release, the shutdown's own, the reference an import refused after
// RTLD_NOLOAD gives back) or a system dependency of a retired image. Every try_shutdown that
// reports the shutdown complete says so (cleanup_incomplete), whichever call completed it.
extern bool g_host_import_leaked;
// System dlcloses of such libraries under way: one whose thread ended inside it (or that a fork
// child has no thread to finish) never says whether the library was closed, so it counts as one
// that failed.
extern size_t g_host_closes_in_flight;
static inline bool host_close_leaked() {
  return g_host_import_leaked || g_host_closes_in_flight != 0;
}

static inline bool shutdown_begun() { return g_shutdown_phase != ShutdownPhase::kActive; }

// The resumption a thread that is gone (ended, or not in this fork child) left running.
static inline void abandon_shutdown_run(pthread_t thread) {
  if (g_shutdown_running && pthread_equal(g_shutdown_thread, thread)) g_shutdown_running = false;
}
#endif

}  // namespace yukilinker::internal
