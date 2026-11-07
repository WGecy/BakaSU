// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>

#include <cstddef>
#include <cstdint>

#include "../../include/yukilinker.h"
#include "address_space.hpp"
#include "checked_math.hpp"
#include "config.hpp"
#include "context.hpp"
#include "elf_model.hpp"
#include "elf_reader.hpp"
#include "loader_types.hpp"
#include "tls_model.hpp"

namespace yukilinker::internal {

struct ImageState;

struct SoHandle {
  uint8_t* load_bias = nullptr;
  size_t map_size = 0;
  void* private_state = nullptr;
};

struct Dependency {
  ImageState* image = nullptr;
  const char* name = nullptr;
  void* system_handle = nullptr;
  CleanupCall close_call;
  Dependency* next = nullptr;
  // Added by retain_symbol_provider for a symbol binding, not for a DT_NEEDED entry.
  bool symbol_provider = false;
  // The target is outside this open's group; it takes the open's flags only if the open succeeds.
  bool apply_open_flags = false;
  // Resolved through one of the namespace's links, so it never takes the open's flags.
  bool linked = false;
};

#if YUKILINKER_FULL
struct ExitCallback;
#endif  // #if YUKILINKER_FULL

// Private dependencies load recursively, one level per DT_NEEDED step, and every level holds a
// stack frame, a source descriptor and one more count of the recursive operation lock until the
// levels below it return. A dependency deeper than this is refused; the root is at depth 0.
constexpr uint32_t kMaxLoadDepth = 512;

enum class Phase { Preparing, Relocated, Initializing, Ready, Closing, Retired, Failed };
// The registry and the event log copy a Phase into yukilinker_c_image_phase as is.
static_assert(static_cast<int>(Phase::Preparing) == YUKILINKER_C_IMAGE_MAPPING &&
              static_cast<int>(Phase::Relocated) == YUKILINKER_C_IMAGE_RELOCATED &&
              static_cast<int>(Phase::Initializing) == YUKILINKER_C_IMAGE_INITIALIZING &&
              static_cast<int>(Phase::Ready) == YUKILINKER_C_IMAGE_READY &&
              static_cast<int>(Phase::Closing) == YUKILINKER_C_IMAGE_CLOSING &&
              static_cast<int>(Phase::Retired) == YUKILINKER_C_IMAGE_RETIRED &&
              static_cast<int>(Phase::Failed) == YUKILINKER_C_IMAGE_FAILED);
struct ImageState {
  Context* context = &g_default_context;
  ImageState* load_root = nullptr;
  char origin[4096]{};
  const char* runpath = nullptr;
  const char* soname = nullptr;
  uint64_t device = 0;
  uint64_t inode = 0;
  uint64_t source_offset = 0;
  uint64_t generation = 0;
  uint64_t namespace_id = 0;
  uint64_t initialization_order = 0;
  uint64_t graph_visit = 0;
  // Levels of private dependency loading above this image; see kMaxLoadDepth.
  uint32_t load_depth = 0;
  // The host imports of its context a relocation of this image bound through: bit i the i-th. A
  // private dlsym through a handle looks at these only. Written in pass 1 under the operation lock;
  // the context's imports are frozen by then.
  uint32_t host_import_mask = 0;
  // The page of this image's caller stubs (caller_stubs.hpp), or null: an image of a managed
  // namespace that imports dlopen, android_dlopen_ext, dlsym or dlvsym. The page outlives it,
  // unless stubs_unpublished is set.
  uint8_t* caller_stubs = nullptr;
  // No code of the image's group ran, so no stub address left its relocated words: the
  // collection that retires it unmaps the stub page with it. Set when a thread ended inside the
  // group's open (fail_abandoned_opens); the open's own failure sweep releases the page itself.
  bool stubs_unpublished = false;
  // Calls into the runtime that this image's own code made (dlopen, dlsym, dlclose and the other
  // interposed entries, found by the return address or a caller stub) still in the runtime, outside
  // any runtime->image frame of this image (CallerCall). Under the operation lock.
  size_t caller_calls = 0;
  // Why a Closing image the free loop found held was kept, a dead copy, instead of freed; it is
  // never made RTLD_NODELETE for it. kFrame: only runtime->image->runtime frames (a constructor,
  // a destructor, a resolver, a callback) and the runtime's own host calls held it; they return
  // only once the image's code has, so the first collection that finds it unheld retires it.
  // kReturnAddress: a call its own code made held it, and that code goes on after the call
  // returns, which no count sees end; it stays until its scope's explicit retirement. Never
  // lowered.
  enum class ReentryHold : uint8_t { kNone, kFrame, kReturnAddress };
  ReentryHold reentry_hold = ReentryHold::kNone;
  size_t references = 1;
  // The part of references that opens still under way hold: taken with the reference, given up
  // (committed) when the open returns the handle, or revoked with the reference when it fails or
  // its thread ends inside it. A close consumes committed references only.
  size_t pending_references = 0;
  size_t active_calls = 0;
  size_t snapshot_refs = 0;
  size_t lease_refs = 0;
  size_t execution_leases = 0;
  size_t unwind_leases = 0;
  size_t cfi_leases = 0;
  size_t host_bridge_leases = 0;
#if YUKILINKER_FULL
  pthread_t loading_thread{};
#endif
  size_t tls_users = 0;
  size_t thread_destructors = 0;
  uint32_t flags = RTLD_NOW;
  uint32_t nodelete_reasons = 0;
  uint32_t failure_code = YUKILINKER_C_OK;
  // Kept with failure_code for a group that stays registered as Failed.
  uint32_t failure_stage = 0;
  int32_t failure_errno = 0;
  bool failure_stage_known = false;
  Phase phase = Phase::Preparing;
  bool symbolic = false;
  bool global_group = false;
  bool marked = false;
  // Set on each Closing image as collection's free loop starts; cleared once the loop visits it.
  bool retire_pending = false;
  // The collection pass that made it Closing, until that pass frees or keeps it; 0 otherwise.
  uint64_t closing_pass = 0;
  // Set once relocation pass 2 reaches the image. An image has one pass 2, so it is never cleared.
  bool pass2_started = false;
  bool linked = false;
  bool host = false;
  bool executed = false;
  // Set on every member of a group that ran no code while the failure sweep of its open releases
  // it (dlopen_memfd_offset). The phase stays, so other threads still wait for that open.
  bool releasing = false;
  // A CLEANUP_FAILURE event was recorded for its reservation: a dead copy retried by every
  // collection reports the failure once, not once per retry, so the event ring keeps the rest.
  bool cleanup_reported = false;
  bool mapping_released = false;
  SoHandle handle_storage;
  // Bootstrap uses the address, the runtime exports a generation-checked token.
  // Both refer to this image's own storage until its last cleanup has completed.
  SoHandle* const public_handle = &handle_storage;
  // The handle the full runtime hands out (public_token); 0 until the image is registered.
  uintptr_t token = 0;
  AddressSpace memory;
  DynamicSymbols symbols;
  RelocationSet relocations;
  Lifecycle lifecycle;
  Dependency* dependencies = nullptr;
  TlsTemplate tls;
  ImageState* previous = nullptr;
  ImageState* next = nullptr;
#if YUKILINKER_FULL
  ExitCallback* exit_callbacks = nullptr;
#endif  // #if YUKILINKER_FULL
};

// An open is still loading it: some of its constructors have not run.
// The references a close may consume: those whose open has returned.
static inline size_t committed_references(const ImageState* image) {
  return image->references - image->pending_references;
}

// An open that took a reference (and a pending one) failed: both go.
static inline void revoke_reference(ImageState* image) {
  --image->references;
  --image->pending_references;
}

static inline bool image_loading(const ImageState* image) {
  return image->phase == Phase::Preparing || image->phase == Phase::Relocated ||
         image->phase == Phase::Initializing;
}

static inline bool accessible_image(const ImageState* image) {
  // About to be freed: not even the thread whose open is releasing it gets it any more.
  if (image->releasing) return false;
#if YUKILINKER_FULL
  if (image->phase == Phase::Closing || image->phase == Phase::Retired ||
      image->phase == Phase::Failed)
    return false;
  return image->phase == Phase::Ready || pthread_equal(image->loading_thread, pthread_self());
#else
  return image->phase != Phase::Closing && image->phase != Phase::Retired &&
         image->phase != Phase::Failed;
#endif
}

// The failure sweep of this thread's own open is releasing it: an open that would reach it gets
// YUKILINKER_C_BUSY instead of looking elsewhere. Another thread's open waits for that open
// instead.
static inline bool released_here(const ImageState* image) {
#if YUKILINKER_FULL
  return image->releasing && pthread_equal(image->loading_thread, pthread_self());
#else
  return image->releasing;
#endif
}

static inline bool image_contains(const ImageState* image, const void* pointer) {
  if (!image) return false;
  const auto address = reinterpret_cast<uintptr_t>(pointer);
  const auto bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  return address >= bias &&
         elf::segment_contains(image->memory.program_headers, image->memory.program_header_count,
                               address - bias, 1, 0);
}

template <typename T>
T* runtime_pointer(ImageState* image, ElfW(Addr) virtual_address, size_t count = 1) {
  size_t bytes;
  if (multiply_overflow(sizeof(T), count, &bytes)) return nullptr;
  uintptr_t bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  if (virtual_address > UINTPTR_MAX - bias) return nullptr;
  auto* pointer = reinterpret_cast<T*>(bias + virtual_address);
  if (virtual_address % alignof(T) != 0 ||
      !elf::segment_contains(image->memory.program_headers, image->memory.program_header_count,
                             virtual_address, bytes, PF_R))
    return nullptr;
  return address_space_contains(image->memory, pointer, bytes) ? pointer : nullptr;
}

}  // namespace yukilinker::internal
