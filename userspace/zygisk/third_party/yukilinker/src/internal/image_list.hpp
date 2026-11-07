// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <pthread.h>
#include <stdlib.h>

#include <cstddef>
#include <cstdint>

#include "../../include/yukilinker.h"
#include "config.hpp"
#include "event_log.hpp"
#include "image_state.hpp"
#include "loader_types.hpp"
#include "token_index.hpp"

namespace yukilinker::internal {

extern ImageState* g_first_image;
extern ImageState* g_last_image;
extern uint64_t g_image_adds;
extern uint64_t g_image_subs;
// Caps snapshot and image-enumeration allocations.
constexpr size_t kMaxSnapshotBytes = size_t{32} * 1024 * 1024;

#if YUKILINKER_FULL
extern pthread_mutex_t g_registry_mutex;
// Every image with a token, by that token, so finding one is not a walk of them all: inserted
// before the image is registered, erased when it is freed, under the operation lock. An image
// that is not registered (one being released) is in it too; state_of checks. The second index
// holds the same images by generation.
extern TokenIndex<ImageState> g_image_index;
extern TokenIndex<ImageState> g_generation_index;
#endif  // #if YUKILINKER_FULL

static inline void registry_lock() {
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_registry_mutex);
#endif  // #if YUKILINKER_FULL
}

static inline void registry_unlock() {
#if YUKILINKER_FULL
  pthread_mutex_unlock(&g_registry_mutex);
#endif  // #if YUKILINKER_FULL
}

static inline void register_image(ImageState* image) {
  registry_lock();
  image->linked = true;
  image->context->links_frozen = true;
  ++image->context->images;
  image->previous = g_last_image;
  image->next = nullptr;
  if (g_last_image != nullptr)
    g_last_image->next = image;
  else
    g_first_image = image;
  g_last_image = image;
  ++g_image_adds;
  record_event(image, YUKILINKER_C_EVENT_PUBLISH);
  registry_unlock();
}

static inline void unregister_image(ImageState* image) {
  registry_lock();
  if (!image->linked) {
    registry_unlock();
    return;
  }
  image->linked = false;
  --image->context->images;
  ++g_image_subs;
  record_event(image, YUKILINKER_C_EVENT_RETIRE);
  if (image->previous != nullptr)
    image->previous->next = image->next;
  else if (g_first_image == image)
    g_first_image = image->next;
  if (image->next != nullptr)
    image->next->previous = image->previous;
  else if (g_last_image == image)
    g_last_image = image->previous;
  image->previous = nullptr;
  image->next = nullptr;
  registry_unlock();
}

// The registered image a handle names: a token (runtime only) is looked up in the index; any other
// value is compared with each image's internal SoHandle.
static inline ImageState* state_of(SoHandle* handle) {
  if (!handle) return nullptr;
#if YUKILINKER_FULL
  // A token's low bits are 10; an SoHandle is pointer-aligned.
  if ((reinterpret_cast<uintptr_t>(handle) & 3) == 2) {
    auto* image = g_image_index.find(reinterpret_cast<uintptr_t>(handle));
    return image && image->linked ? image : nullptr;
  }
#endif
  // Anything else may be a host handle, which is never read: only compared.
  for (auto* image = g_first_image; image; image = image->next)
    if (image->public_handle == handle) return image;
  return nullptr;
}

// The registered image of generation, or null. Generations are never reused, so one an image had
// before it was freed finds nothing.
static inline ImageState* image_by_generation(uint64_t generation) {
#if YUKILINKER_FULL
  auto* image = g_generation_index.find(generation);
  return image && image->linked ? image : nullptr;
#else
  for (auto* image = g_first_image; image; image = image->next)
    if (image->generation == generation) return image;
  return nullptr;
#endif
}

// The image of an SoHandle that load_image just returned: it is registered and its own.
static inline ImageState* image_of(SoHandle* handle) {
  return static_cast<ImageState*>(handle->private_state);
}

static inline void* public_token(ImageState* image) {
#if YUKILINKER_FULL
  return reinterpret_cast<void*>(image->token);
#else
  return image->public_handle;
#endif
}

// The private tag: low bits 10, which neither an aligned soinfo pointer nor an odd Bionic handle
// has, so a stale or forged value of this shape is refused here and never forwarded to the host.
// The tag leaves 30 random bits on LP32, 62 on LP64. LP32's RTLD_NEXT (0xfffffffe) has the tag
// too: callers test the pseudo-handles first, and no token is ever issued equal to one.
static inline bool private_token(void* handle) {
  return (reinterpret_cast<uintptr_t>(handle) & 3) == 2;
}

#if YUKILINKER_FULL
// A random token for a handle, namespace, snapshot or lease, as Bionic's generate_handle: nothing
// public tells it, so no other code can compute one it was never given. low_bits replace the two
// low bits (a mask of 3 keeps them); taken(value) rejects one already issued. Tries a bounded
// number of times, so a broken entropy source cannot keep the lock forever: 0 then, which no
// caller hands out.
template <typename T, typename Taken>
T random_token(T low_bits, T mask, Taken taken) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    T value;
    arc4random_buf(&value, sizeof(value));
    value = (value & ~mask) | low_bits;
    if (value != 0 && !taken(value)) return value;
  }
  return 0;
}
#endif  // #if YUKILINKER_FULL

}  // namespace yukilinker::internal
