// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <pthread.h>

#include <cstdint>

#include "config.hpp"

namespace yukilinker::internal {

struct ImageState;

extern "C" int __cxa_atexit(void (*function)(void*), void* argument, void* dso);
extern "C" void __cxa_finalize(void* dso);

#if YUKILINKER_FULL
extern pthread_mutex_t g_exit_mutex;
extern pthread_key_t g_initializing_key;
extern bool g_initializing_key_valid;
// Creates the key that names the image whose initializers run, unless it exists or the keys were
// retired; false when there is none.
bool ensure_initializing_key();

struct ExitCallback {
  void (*function)(void*) = nullptr;
  void* argument = nullptr;
  void* dso = nullptr;
  ExitCallback* next = nullptr;
};
#endif  // #if YUKILINKER_FULL

bool executable_pointer(ImageState* image, uintptr_t address);
// DT_INIT and DT_FINI are not relocated and can be checked as soon as they are parsed.
bool validate_init_fini(ImageState* image);
// Before pass 2, skip_deferred leaves out the slots an IFUNC resolver has yet to write.
bool validate_lifecycle(ImageState* image, bool skip_deferred);
bool run_initializers(ImageState* image);
void run_finalizers(ImageState* image);
void discard_exit_callbacks(ImageState* image);
bool retire_process_exit(ImageState* image);
bool exit_cleanup_uncertain();
void abandon_exit_cleanup(pthread_t thread, bool fork_child);
void module_cxa_finalize(void* dso);
int module_cxa_atexit(void (*function)(void*), void* argument, void* dso);
ImageState* find_exit_owner(void* dso);

}  // namespace yukilinker::internal
