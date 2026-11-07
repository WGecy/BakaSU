// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <cstddef>
#include <cstdint>

// Forward declaration; the full type is in include/yukilinker.h.
struct yukilinker_c_image_info;

// src/arch/<abi>/call_barrier.S: calls function(a0, a1, a2). An exception escaping the call ends
// in std::terminate there: runtime code has no cleanups, so unwinding through it would leave its
// lock, guards and phases behind.
extern "C" uintptr_t yuki_call_barrier(const void* function, uintptr_t a0, uintptr_t a1,
                                       uintptr_t a2);

namespace yukilinker::internal {

// Every call from the runtime into module code goes through one of these.
inline void call_module(void (*function)()) {
  (void)yuki_call_barrier(reinterpret_cast<const void*>(function), 0, 0, 0);
}

inline void call_module(void (*function)(void*), void* argument) {
  (void)yuki_call_barrier(reinterpret_cast<const void*>(function),
                          reinterpret_cast<uintptr_t>(argument), 0, 0);
}

inline uintptr_t call_module(uintptr_t function, uintptr_t a0, uintptr_t a1) {
  return yuki_call_barrier(reinterpret_cast<const void*>(function), a0, a1, 0);
}

template <typename Info>
inline int call_module(int (*function)(Info*, size_t, void*), Info* info, size_t size, void* user) {
  return static_cast<int>(yuki_call_barrier(reinterpret_cast<const void*>(function),
                                            reinterpret_cast<uintptr_t>(info), size,
                                            reinterpret_cast<uintptr_t>(user)));
}

// yukilinker_c_dependency_provider: int provider(void* user, const char* requester, const char*
// name).
inline int call_module(int (*function)(void*, const char*, const char*), void* user,
                       const char* requester, const char* name) {
  return static_cast<int>(
      yuki_call_barrier(reinterpret_cast<const void*>(function), reinterpret_cast<uintptr_t>(user),
                        reinterpret_cast<uintptr_t>(requester), reinterpret_cast<uintptr_t>(name)));
}

// yukilinker_c_image_visitor: int visitor(const yukilinker_c_image_info*, void* user).
inline int call_module(int (*function)(const yukilinker_c_image_info*, void*),
                       const yukilinker_c_image_info* info, void* user) {
  return static_cast<int>(yuki_call_barrier(reinterpret_cast<const void*>(function),
                                            reinterpret_cast<uintptr_t>(info),
                                            reinterpret_cast<uintptr_t>(user), 0));
}

}  // namespace yukilinker::internal
