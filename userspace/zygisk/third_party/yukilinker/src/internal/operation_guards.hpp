// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once
#include <pthread.h>

#include <cstddef>
#include <cstdint>

#include "../../include/yukilinker.h"
#include "config.hpp"
#include "image_state.hpp"
#include "loader_types.hpp"
#include "operation_error.hpp"
#include "tls_model.hpp"

namespace yukilinker::internal {

extern const LoadOptions* g_open_options;
extern ImageState* g_loading_parent;
extern uint64_t g_next_image_generation;
extern uint64_t g_initialization_order;
// Stamps one walk of an image graph; see graph_ready.
extern uint64_t g_graph_visit;
extern int g_relocation_pass;
extern bool g_collecting;
// A collection asked for while one is under way; that one makes another pass. Each pass clears
// it as it starts, so it means something only while g_collecting is set.
extern bool g_collect_again;
// Numbers collection passes; an image a pass makes Closing carries the number (closing_pass).
extern uint64_t g_collection_pass;
// An image the collection under way made Closing in its current pass and has neither freed nor
// kept yet.
static inline bool retiring_image(const ImageState* image) {
  return image->phase == Phase::Closing && g_collecting && image->closing_pass == g_collection_pass;
}
// A Closing image no collection is retiring, and whose finalizers have run: a callback pinned
// it, or its munmap failed. Every lookup ignores it, and an open loads a fresh copy beside it.
static inline bool dead_copy(const ImageState* image) {
  return image->phase == Phase::Closing && !retiring_image(image) && !image->lifecycle.initialized;
}
#if YUKILINKER_FULL
extern pthread_t g_collecting_thread;
extern pthread_mutex_t g_operation_mutex;
extern pthread_key_t g_operation_depth_key;
extern pthread_key_t g_callback_key;
extern pthread_key_t g_snapshot_key;
// Serializes creating the runtime's pthread keys. A failed attempt leaves nothing behind and the
// next caller tries again; once shutdown has deleted the keys (g_keys_retired), none is created.
extern pthread_mutex_t g_key_mutex;
extern bool g_keys_retired;
// Whether api_prepare_fork took the locks: the keys can come into being before api_after_fork.
extern bool g_fork_locked;
extern bool g_operation_depth_valid;
extern size_t g_active_operations;
// The admissions among g_active_operations still waiting in operation_lock for the mutex. They
// hold nothing yet: once one has the lock, it sees whatever the holder changed meanwhile.
extern size_t g_lock_waiters;
// Set while runtime_create is between its admission and its publish, so a hook it calls cannot
// become the owner instead.
extern bool g_runtime_creating;
// The thread inside that span; a fork child keeps the flag only if it is that thread.
extern pthread_t g_runtime_creating_thread;
// The destructors of the callback and snapshot keys. They run only for a thread that ends inside
// module code (pthread_exit from a callback): Bionic does not unwind, so the frames are still in
// memory, and each gives back what its open frames hold.
void abandon_callback_frames(void* innermost);
void abandon_snapshots(void* innermost);
// The operation-depth key's destructor: a thread that ends with admissions and no frame, or
// while an operation_lock of its own waits for the mutex.
void abandon_operation(void* value);
// All three keys or none; valid is published only once they all exist.
static inline void initialize_operation_key() {
  if (pthread_key_create(&g_operation_depth_key, abandon_operation) != 0) return;
  if (pthread_key_create(&g_callback_key, abandon_callback_frames) != 0) {
    pthread_key_delete(g_operation_depth_key);
    return;
  }
  if (pthread_key_create(&g_snapshot_key, abandon_snapshots) != 0) {
    pthread_key_delete(g_callback_key);
    pthread_key_delete(g_operation_depth_key);
    return;
  }
  __atomic_store_n(&g_operation_depth_valid, true, __ATOMIC_RELEASE);
}
// Registers the runtime's atfork handlers with libc; once, by the call that created the keys.
void register_fork_handlers();
static inline bool ensure_operation_keys() {
  if (__atomic_load_n(&g_operation_depth_valid, __ATOMIC_ACQUIRE)) return true;
  pthread_mutex_lock(&g_key_mutex);
  bool created = false;
  if (!g_operation_depth_valid && !g_keys_retired) {
    initialize_operation_key();
    created = g_operation_depth_valid;
  }
  const bool valid = g_operation_depth_valid;
  pthread_mutex_unlock(&g_key_mutex);
  // Outside g_key_mutex: libc takes its atfork lock, under which a fork takes g_key_mutex.
  if (created) register_fork_handlers();
  return valid;
}
// The operation-depth key holds this thread's lock depth in its low bits and, above them, how many
// of its operation_lock calls are still waiting for the mutex (a signal handler's nested call adds
// one), so a thread that ends while it waits is found by the key's destructor and its admissions
// given back.
constexpr unsigned kLockWaitShift = sizeof(uintptr_t) * 8 - 8;
constexpr uintptr_t kDepthMask = (uintptr_t{1} << kLockWaitShift) - 1;
static inline uintptr_t operation_key_value() {
  return g_operation_depth_valid
             ? reinterpret_cast<uintptr_t>(pthread_getspecific(g_operation_depth_key))
             : 0;
}
static inline uintptr_t operation_depth() { return operation_key_value() & kDepthMask; }
// Stores this thread's key value; false when the store failed and the key still holds the old one.
// Each change below is undone only if it was stored, so a failed store never makes a field wrap.
static inline bool store_operation_key(uintptr_t value) {
  return g_operation_depth_valid &&
         pthread_setspecific(g_operation_depth_key, reinterpret_cast<void*>(value)) == 0;
}
static inline void operation_lock() {
  // Admission before waiter: a reader that sees the waiter (sole_holder) sees the admission too.
  // An uncontended lock needs neither the waiter count nor the key's record of the wait.
  if (pthread_mutex_trylock(&g_operation_mutex) == 0) {
    __atomic_add_fetch(&g_active_operations, size_t{1}, __ATOMIC_ACQ_REL);
  } else {
    __atomic_add_fetch(&g_active_operations, size_t{1}, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&g_lock_waiters, size_t{1}, __ATOMIC_ACQ_REL);
    const bool recorded =
        store_operation_key(operation_key_value() + (uintptr_t{1} << kLockWaitShift));
    if (recorded) {
      pthread_mutex_lock(&g_operation_mutex);
    } else {
      // No record of the wait: a handler that ended this thread inside it would leave the waiter
      // and its admission counted for good. Signals wait until the lock is held.
      ScopedSignalBlocker no_signals;
      pthread_mutex_lock(&g_operation_mutex);
    }
    if (recorded && (operation_key_value() >> kLockWaitShift) != 0 &&
        !store_operation_key(operation_key_value() - (uintptr_t{1} << kLockWaitShift))) {
      // The key still records the wait, which the thread's exit gives back as one waiter and one
      // admission: keep both counted until then, the admission as one more of this thread's.
      __atomic_add_fetch(&g_active_operations, size_t{1}, __ATOMIC_ACQ_REL);
    } else {
      __atomic_sub_fetch(&g_lock_waiters, size_t{1}, __ATOMIC_ACQ_REL);
    }
  }
  if ((operation_key_value() & kDepthMask) != kDepthMask)
    (void)store_operation_key(operation_key_value() + 1);
}
static inline void operation_unlock() {
  // A depth whose store failed in operation_lock was never counted: nothing to take back. One
  // whose decrement fails stays in the key, and the thread's exit gives it back: the admission
  // stays counted until then, so the two agree.
  if (operation_depth() == 0 || store_operation_key(operation_key_value() - 1))
    __atomic_sub_fetch(&g_active_operations, size_t{1}, __ATOMIC_ACQ_REL);
  // Before the unlock: only the holder reads the count, so the next one never sees this call.
  pthread_mutex_unlock(&g_operation_mutex);
}
// True when this thread may change the owner: no operation anywhere else, and no callback frame
// of its own open. A callback leaves `g_active_operations` above this thread's depth, and a
// nested call inside a frame adds to both at once, so the frame chain is what decides.
static inline bool callback_frame_open();
static inline bool sole_operation() {
  return __atomic_load_n(&g_active_operations, __ATOMIC_ACQUIRE) == operation_depth() &&
         !callback_frame_open();
}
// sole_operation, except that an admission still waiting for the lock does not count: the
// retirements, which another thread's refused calls would otherwise starve. Only true for the
// lock's holder, so no waiter can leave meanwhile; the waiters are read first, so each one read is
// in the admissions read after it.
static inline bool sole_holder() {
  const size_t waiting = __atomic_load_n(&g_lock_waiters, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&g_active_operations, __ATOMIC_ACQUIRE) - waiting == operation_depth() &&
         !callback_frame_open();
}

// The operation state of a thread that drops the lock in the middle of an operation. Another
// thread may then run its own operation and set its own, so the state is put aside before the
// lock goes and taken back once it is held again.
struct HeldOperation {
  yukilinker_c_error_detail* error = nullptr;
  OperationRefusal* refusal = nullptr;
  OperationRefusal* call_slot = nullptr;
  const LoadOptions* options = nullptr;
  ImageState* parent = nullptr;
  int pass = 1;
  void put_aside() {
    error = g_operation_error;
    refusal = g_operation_refusal;
    call_slot = g_private_call_slot;
    options = g_open_options;
    parent = g_loading_parent;
    pass = g_relocation_pass;
    g_operation_error = nullptr;
    g_operation_refusal = nullptr;
    g_private_call_slot = nullptr;
    g_open_options = nullptr;
    g_loading_parent = nullptr;
    g_relocation_pass = 1;
  }
  void take_back() const {
    g_operation_error = error;
    g_operation_refusal = refusal;
    g_private_call_slot = call_slot;
    g_open_options = options;
    g_loading_parent = parent;
    g_relocation_pass = pass;
  }
};

// An open waiting for another thread's open to finish (wait_for_load), or a close waiting for
// another thread's collection (collect_after_close). The nodes live on the waiting threads' stacks
// and are linked and unlinked under the operation lock.
struct LoadWaiter {
  pthread_t thread;
  // The thread loading the image this one waits for, or collecting it.
  pthread_t owner;
  // Kept alive while the lock is dropped; a fork child gives the context's count back.
  Context* context;
  LoadWaiter* next;
  // The image the wait keeps (active_calls), or null; a thread that ends in the wait gives it
  // back (abandon_callback_frames).
  ImageState* caller = nullptr;
};
extern LoadWaiter* g_load_waiters;
// Paired with g_operation_mutex; broadcast when an open finishes and when a collection frees or
// keeps an image an open may wait for.
extern pthread_cond_t g_load_wait_cond;
struct SnapshotGuard;
// Every thread's active iteration snapshots, linked and unlinked under the operation lock.
extern SnapshotGuard* g_active_snapshots;
// Another thread's collection is retiring it.
static inline bool closing_elsewhere(const ImageState* image) {
  return retiring_image(image) && !pthread_equal(g_collecting_thread, pthread_self());
}
// What an open waits for (wait_for_load) instead of failing at once with YUKILINKER_C_BUSY.
static inline bool awaited_image(const ImageState* image) {
  return image_loading(image) || closing_elsewhere(image);
}
#endif
// This thread's collection is retiring it and has not begun to unmap it: the finalizers of this
// pass run on this thread meanwhile.
static inline bool retiring_here(const ImageState* image) {
#if YUKILINKER_FULL
  return retiring_image(image) && !image->releasing &&
         pthread_equal(g_collecting_thread, pthread_self());
#else
  (void)image;
  return false;
#endif
}
struct OperationGuard {
  OperationGuard() {
#if YUKILINKER_FULL
    operation_lock();
#endif
  }
  ~OperationGuard() {
#if YUKILINKER_FULL
    operation_unlock();
#endif
  }
  OperationGuard(const OperationGuard&) = delete;
  OperationGuard& operator=(const OperationGuard&) = delete;
  OperationGuard(OperationGuard&&) = delete;
  OperationGuard& operator=(OperationGuard&&) = delete;
};

// Keep admissions counted while module code runs without the mutation lock. `keep_lock` leaves
// the lock and this thread's depth alone, which is what the fork child path needs: there the
// mutex has just been reinitialized and the thread must stay the runtime's only occupant.
//
// A frame opened inside another frame of the same thread holds no lock (its depth is 0). The
// operation state is then another thread's, and the outer frame has saved this thread's, so such
// a frame neither saves, clears nor restores it.
struct CallbackGuard {
  ImageState* image;
  // The frame runs image's own code (a constructor, a destructor, a resolver, a callback), as
  // opposed to a host call made on its behalf. A call that image's code makes inside such a frame
  // is covered by it (CallerCall), since the frame ends only once that code has returned.
  bool runs_image_code = false;
  // A CallerCall's marker (CallerCall::frame): no callback, no lock dropped, no active call; it
  // only lets abandon_callback_frames give the call's caller_calls back.
  bool caller_call = false;
#if YUKILINKER_FULL
  uintptr_t depth = 0;
  bool kept_lock = false;
  bool saved_state = false;
  // The frame is on the thread's chain (the callback key holds it): only then does the destructor
  // put the previous one back. A frame whose store failed is invisible to the chain's readers and
  // never leaves the key pointing at a frame that is gone.
  bool linked = false;
  HeldOperation held;
  CallbackGuard* previous;
#endif
  // before_release is an internal, nonblocking state transition. It runs only
  // after the callback frame and lock-depth save both succeeded, with the lock
  // still held; it must not invoke host code or acquire another lock.
  explicit CallbackGuard(ImageState* owner, bool keep_lock = false,
                         void (*before_release)(ImageState*, void*) = nullptr,
                         void* argument = nullptr)
      : image(owner) {
    if (image) ++image->active_calls;
#if YUKILINKER_FULL
    if (keep_lock) {
      kept_lock = true;
      return;
    }
    depth = operation_depth();
    // Once shutdown deleted the keys, their indexes can belong to another library.
    previous = g_operation_depth_valid
                   ? static_cast<CallbackGuard*>(pthread_getspecific(g_callback_key))
                   : nullptr;
    linked = g_operation_depth_valid && pthread_setspecific(g_callback_key, this) == 0;
    if (before_release && !linked) return;
    // A depth of 0 leaves no lock to drop, unless the keys are gone: then every depth reads 0 and
    // an operation keeps the lock throughout.
    if (depth == 0 && g_operation_depth_valid) return;
    // The depth goes to 0; a wait of an interrupted operation_lock stays recorded. The lock is
    // dropped only once the key says so: a key that kept the depth would have this thread's nested
    // operations unlock a mutex it no longer holds.
    if (g_operation_depth_valid && !store_operation_key(operation_key_value() & ~kDepthMask))
      return;
    saved_state = true;
    if (before_release) before_release(image, argument);
    held.put_aside();
    for (uintptr_t i = 0; i < depth; ++i) pthread_mutex_unlock(&g_operation_mutex);
#else
    if (before_release) before_release(image, argument);
#endif
  }
  ~CallbackGuard() {
#if YUKILINKER_FULL
    if (kept_lock) {
      if (image) --image->active_calls;
      return;
    }
    if (saved_state) {
      for (uintptr_t i = 0; i < depth; ++i) pthread_mutex_lock(&g_operation_mutex);
      // A restore that fails leaves the depth reading 0 while the lock is held depth times, as an
      // operation_lock whose store failed does: each unlock still gives its admission and its
      // lock level back, and a later callback keeps the lock instead of dropping it.
      if (g_operation_depth_valid)
        (void)store_operation_key((operation_key_value() & ~kDepthMask) | depth);
      held.take_back();
    }
    if (linked) (void)pthread_setspecific(g_callback_key, previous);
#endif
    if (image) --image->active_calls;
  }
  CallbackGuard(const CallbackGuard&) = delete;
  CallbackGuard& operator=(const CallbackGuard&) = delete;
  CallbackGuard(CallbackGuard&&) = delete;
  CallbackGuard& operator=(CallbackGuard&&) = delete;
};

// A call into the runtime from an image's own code (an interposed dlopen, dlsym, dlclose...), its
// caller found by the return address or a caller stub. While it is in the runtime it counts in
// caller_calls, so a collection that finds the image held then keeps it with a return-address hold:
// the code that made the call runs on after it returns. Inside a frame of this thread that runs the
// same image's code (runs_image_code), the call is part of that frame, which returns only after the
// image's code has; it counts nothing then.
//
// The call is linked on the thread's callback chain as a marker frame (caller_call), so a thread
// that ends inside it (a pthread_exit from a callback the call ran) gives its count back in
// abandon_callback_frames. The chain's readers that look for callbacks skip markers.
struct CallerCall {
  ImageState* image;
#if YUKILINKER_FULL
  CallbackGuard* marker = nullptr;
#endif
  explicit CallerCall(ImageState* caller) : image(caller) {
#if YUKILINKER_FULL
    if (!image || !g_operation_depth_valid) {
      image = nullptr;
      return;
    }
    auto* top = static_cast<CallbackGuard*>(pthread_getspecific(g_callback_key));
    for (const CallbackGuard* frame = top; frame; frame = frame->previous)
      if (!frame->caller_call && frame->image == image && frame->runs_image_code) {
        image = nullptr;
        return;
      }
    // calloc, not a CallbackGuard on the stack: its constructor would drop the lock. A failed
    // allocation, or a marker the key would not take, counts the call without a marker: a thread
    // that then ends inside it leaves the image kept, which errs towards keeping it.
    marker = static_cast<CallbackGuard*>(calloc(1, sizeof(CallbackGuard)));
    if (marker) {
      marker->image = image;
      marker->caller_call = true;
      marker->kept_lock = true;
      marker->previous = top;
      if (pthread_setspecific(g_callback_key, marker) != 0) {
        free(marker);
        marker = nullptr;
      }
    }
    ++image->caller_calls;
#else
    image = nullptr;
#endif
  }
  ~CallerCall() {
#if YUKILINKER_FULL
    if (!image) return;
    --image->caller_calls;
    if (!marker) return;
    // A marker the key still names is left allocated, never freed under it: an exit or a fork
    // child that walks the chain then finds a marker of a call that is over, whose image was
    // counted back here, and a leak is all that is left.
    if (g_operation_depth_valid && pthread_setspecific(g_callback_key, marker->previous) != 0) {
      marker->image = nullptr;
      return;
    }
    free(marker);
#endif
  }
  CallerCall(const CallerCall&) = delete;
  CallerCall& operator=(const CallerCall&) = delete;
  CallerCall(CallerCall&&) = delete;
  CallerCall& operator=(CallerCall&&) = delete;
};

#if YUKILINKER_FULL
// Whether a callback frame of this thread is open: a CallerCall's marker is none.
static inline bool callback_frame_open() {
  if (!g_operation_depth_valid) return false;
  for (auto* frame = static_cast<const CallbackGuard*>(pthread_getspecific(g_callback_key)); frame;
       frame = frame->previous)
    if (!frame->caller_call) return true;
  return false;
}
#endif

// Marks the span in which runtime_create has been admitted but has not yet published an owner.
// A host hook called from inside it re-enters at a higher depth and must be refused.
struct CreateScope {
#if YUKILINKER_FULL
  CreateScope() {
    g_runtime_creating = true;
    g_runtime_creating_thread = pthread_self();
  }
  ~CreateScope() { g_runtime_creating = false; }
#else
  CreateScope() {}
#endif
  CreateScope(const CreateScope&) = delete;
  CreateScope& operator=(const CreateScope&) = delete;
  CreateScope(CreateScope&&) = delete;
  CreateScope& operator=(CreateScope&&) = delete;
};

// Track iterator pins so a child created from a callback can restore them. In the full runtime a
// private dl_iterate_phdr's snapshot (waitable) is also on g_active_snapshots, so a close on
// another thread can wait for the iteration that keeps its image (collect_after_close). Each guard
// is placed at the start of a calloc block that also holds `images`, never on the stack: a thread
// that ends inside the iteration leaves it linked, and abandon_snapshots frees it.
struct SnapshotGuard {
  ImageState** images;
  size_t count;
#if YUKILINKER_FULL
  SnapshotGuard* previous;
  pthread_t thread;
  SnapshotGuard* next_active = nullptr;
  bool waitable;
  // A close ran while this outermost snapshot was active: collect once it ends.
  bool collect_after = false;
  // The snapshot key holds this guard: only then does the destructor put the previous one back.
  bool linked = false;
#endif
  SnapshotGuard(ImageState** snapshot, size_t size, bool wait_for = false)
      : images(snapshot),
        count(size)
#if YUKILINKER_FULL
        ,
        previous(g_operation_depth_valid
                     ? static_cast<SnapshotGuard*>(pthread_getspecific(g_snapshot_key))
                     : nullptr),
        thread(pthread_self()),
        waitable(wait_for)
#endif
  {
    for (size_t i = 0; i < count; ++i) ++images[i]->active_calls;
#if YUKILINKER_FULL
    linked = g_operation_depth_valid && pthread_setspecific(g_snapshot_key, this) == 0;
    if (waitable) {
      next_active = g_active_snapshots;
      g_active_snapshots = this;
    }
#else
    (void)wait_for;
#endif
  }
  ~SnapshotGuard() {
    for (size_t i = 0; i < count; ++i) --images[i]->active_calls;
#if YUKILINKER_FULL
    if (linked) (void)pthread_setspecific(g_snapshot_key, previous);
    if (!waitable) return;
    SnapshotGuard** link = &g_active_snapshots;
    while (*link && *link != this) link = &(*link)->next_active;
    if (*link) *link = next_active;
    // A close on another thread may wait for this iteration to end.
    if (g_load_waiters) (void)pthread_cond_broadcast(&g_load_wait_cond);
#endif
  }
  SnapshotGuard(const SnapshotGuard&) = delete;
  SnapshotGuard& operator=(const SnapshotGuard&) = delete;
  SnapshotGuard(SnapshotGuard&&) = delete;
  SnapshotGuard& operator=(SnapshotGuard&&) = delete;
};

}  // namespace yukilinker::internal
