// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include <pthread.h>

int __cxa_atexit(void (*callback)(void*), void* data, void* owner);
int __cxa_thread_atexit_impl(void (*callback)(void*), void* data, void* owner);
extern void* __dso_handle;

__thread int module_value = 42;
static int* fork_calls;
static void count(void* value) { ++*(int*)value; }
static void child(void) {
  if (fork_calls) ++*fork_calls;
}

int* module_slot(void) { return &module_value; }
int module_thread_exit(int* calls) { return __cxa_thread_atexit_impl(count, calls, &__dso_handle); }
int module_exit(int* calls) { return __cxa_atexit(count, calls, &__dso_handle); }
int module_fork(int* calls) {
  fork_calls = calls;
  return pthread_atfork(0, 0, child);
}
