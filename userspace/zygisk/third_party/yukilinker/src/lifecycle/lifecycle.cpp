// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/cleanup_call.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

#if YUKILINKER_FULL
void finalize_at_process_exit(void* generation);
bool g_process_exit_uncertain;

bool exit_cleanup_uncertain() { return g_process_exit_uncertain; }

void abandon_exit_cleanup(pthread_t thread, bool fork_child) {
  for (auto* image = g_first_image; image; image = image->next) {
    auto& lifecycle = image->lifecycle;
    const auto lost_thread = [&](pthread_t owner) {
      return pthread_equal(thread, owner) == !fork_child;
    };
    const auto abandon = [&](CleanupCall& call) {
      if (call.state != CleanupState::Calling || !lost_thread(call.thread)) return;
      call.state = CleanupState::Uncertain;
      image->flags |= RTLD_NODELETE;
      if (fork_child) image->nodelete_reasons |= YUKILINKER_C_NODELETE_FORK;
      g_process_exit_uncertain = true;
    };
    abandon(lifecycle.process_exit);
    for (auto* dso = &lifecycle.dso_cleanup; dso; dso = dso->next) {
      abandon(dso->call);
      if (dso->draining && lost_thread(dso->draining_thread)) dso->draining = false;
    }
    for (auto* dependency = image->dependencies; dependency; dependency = dependency->next)
      abandon(dependency->close_call);
  }
}

bool retire_process_exit(ImageState* image) {
  auto& lifecycle = image->lifecycle;
  if (lifecycle.process_exit.state == CleanupState::None ||
      lifecycle.process_exit.state == CleanupState::Cleared)
    return true;
  lifecycle.process_exit_registered = false;
  return cleanup_call(image, lifecycle.process_exit,
                      [&] { __cxa_finalize(&lifecycle.process_exit); });
}

void remove_process_exit_entries() {
  OperationGuard operation;
  // Normal shutdown already drained these before deleting the operation keys.
  // Also disarm entries on a host dlclose that bypasses the owner contract.
  for (;;) {
    ImageState* pending = nullptr;
    for (auto* image = g_first_image; image; image = image->next)
      if (image->lifecycle.process_exit.state == CleanupState::Registered ||
          image->lifecycle.process_exit.state == CleanupState::Pending) {
        pending = image;
        break;
      }
    if (!pending || !retire_process_exit(pending)) return;
  }
}

bool ensure_initializing_key() {
  if (__atomic_load_n(&g_initializing_key_valid, __ATOMIC_ACQUIRE)) return true;
  pthread_mutex_lock(&g_key_mutex);
  if (!g_initializing_key_valid && !g_keys_retired) {
    const int error = pthread_key_create(&g_initializing_key, nullptr);
    if (error == 0)
      __atomic_store_n(&g_initializing_key_valid, true, __ATOMIC_RELEASE);
    else
      errno = error;
  }
  const bool valid = g_initializing_key_valid;
  pthread_mutex_unlock(&g_key_mutex);
  return valid;
}

ImageState* find_exit_owner(void* dso) {
  if (dso != nullptr) {
    registry_lock();
    for (ImageState* image = g_first_image; image != nullptr; image = image->next) {
      if (image_contains(image, dso)) {
        registry_unlock();
        return image;
      }
    }
    registry_unlock();
  }
  return g_initializing_key_valid
             ? static_cast<ImageState*>(pthread_getspecific(g_initializing_key))
             : nullptr;
}

void drain_exit_callbacks(ImageState* image, void* dso) {
  if (image == nullptr) return;
  for (;;) {
    pthread_mutex_lock(&g_exit_mutex);
    ExitCallback** link = &image->exit_callbacks;
    while (*link != nullptr && dso != nullptr && (*link)->dso != dso) link = &(*link)->next;
    ExitCallback* callback = *link;
    if (callback != nullptr) *link = callback->next;
    pthread_mutex_unlock(&g_exit_mutex);
    if (callback == nullptr) break;
    if (g_operation_error) g_operation_error->execution_started = 1;
    {
      CallbackGuard callback_scope(image);
      callback_scope.runs_image_code = true;
      call_module(callback->function, callback->argument);
    }
    free_object(callback);
  }
}

void discard_exit_callbacks(ImageState* image) {
  // The collector removed the libc registration before unmapping this image.
  image->lifecycle.process_exit_registered = false;
  pthread_mutex_lock(&g_exit_mutex);
  ExitCallback* callback = image->exit_callbacks;
  image->exit_callbacks = nullptr;
  pthread_mutex_unlock(&g_exit_mutex);
  while (callback != nullptr) {
    ExitCallback* next = callback->next;
    free_object(callback);
    callback = next;
  }
}

int module_cxa_atexit(void (*function)(void*), void* argument, void* dso) {
  OperationGuard operation;
  if (function == nullptr) return -1;
  pthread_mutex_lock(&g_exit_mutex);
  ImageState* owner = find_exit_owner(dso);
  if (owner == nullptr) {
    pthread_mutex_unlock(&g_exit_mutex);
    return -1;
  }
  auto* callback = allocate_object<ExitCallback>();
  if (callback == nullptr) {
    pthread_mutex_unlock(&g_exit_mutex);
    return -1;
  }
  callback->function = function;
  callback->argument = argument;
  callback->dso = dso;
  callback->next = owner->exit_callbacks;
  owner->exit_callbacks = callback;
  pthread_mutex_unlock(&g_exit_mutex);
  return 0;
}

namespace {

DsoCleanup* reserve_dso_cleanup(ImageState* image, void* cookie) {
  DsoCleanup* available = nullptr;
  auto** tail = &image->lifecycle.dso_cleanup.next;
  for (auto* record = &image->lifecycle.dso_cleanup; record; record = record->next) {
    if (!record->draining && record->cookie == cookie &&
        record->call.state == CleanupState::Pending)
      return record;
    if (!available &&
        (record->call.state == CleanupState::None || record->call.state == CleanupState::Cleared))
      available = record;
    tail = &record->next;
  }
  if (!available) {
    available = metadata_object<DsoCleanup>(&image->memory.metadata);
    if (!available) {
      // The void ABI cannot return an unrecorded cookie. Keep the image instead
      // of later unmapping code that libc might still reference.
      image->lifecycle.cleanup_record_failed = true;
      cleanup_failure(YUKILINKER_C_NO_MEMORY);
      return nullptr;
    }
    *tail = available;
  }
  available->cookie = cookie;
  available->call.state = CleanupState::Pending;
  return available;
}

bool finish_dso_cleanup(ImageState* image, DsoCleanup* record) {
  if (record->call.state == CleanupState::Cleared || record->call.state == CleanupState::None)
    return true;
  if (record->draining || record->call.state != CleanupState::Pending)
    return cleanup_failure(YUKILINKER_C_BUSY);
  record->draining = true;
  record->draining_thread = pthread_self();
  drain_exit_callbacks(image, record->cookie);
  record->draining = false;
  return cleanup_call(image, record->call, [&] { __cxa_finalize(record->cookie); });
}

bool resume_dso_cleanup(ImageState* image) {
  if (image->lifecycle.cleanup_record_failed) return cleanup_failure(YUKILINKER_C_NO_MEMORY);
  for (auto* record = &image->lifecycle.dso_cleanup; record; record = record->next)
    if (!finish_dso_cleanup(image, record)) return false;
  return true;
}

bool dso_cleanup_pending(const Lifecycle& lifecycle) {
  if (lifecycle.cleanup_record_failed) return true;
  for (auto* record = &lifecycle.dso_cleanup; record; record = record->next)
    if (record->call.state != CleanupState::None && record->call.state != CleanupState::Cleared)
      return true;
  return false;
}

}  // namespace

void module_cxa_finalize(void* dso) {
  OperationGuard operation;
  pthread_mutex_lock(&g_exit_mutex);
  ImageState* owner = find_exit_owner(dso);
  pthread_mutex_unlock(&g_exit_mutex);
  if (!owner) {
    const void* caller = __builtin_return_address(0);
    for (auto* image = g_first_image; image; image = image->next)
      if (image_contains(image, caller)) {
        owner = image;
        break;
      }
  }
  // The image's own code (crtbegin's __on_dlclose) made this call and runs on after it returns.
  CallerCall call(owner);
  // Only libc's __cxa_finalize(dso) runs __unregister_atfork(dso); crtbegin's __on_dlclose calls
  // this before collection unmaps the image. libc takes its own locks and may run entries, so the
  // lock is dropped as for a callback. Never forward null: that is exit()'s finalization.
  if (!dso) {
    drain_exit_callbacks(owner, nullptr);
    return;
  }
  // A cookie outside a private image has no owner we can keep through retry or
  // abandonment. Refuse it rather than forwarding an untracked external cleanup.
  if (!owner) {
    cleanup_failure(YUKILINKER_C_INVALID_HANDLE);
    return;
  }
  auto* record = reserve_dso_cleanup(owner, dso);
  if (record) (void)finish_dso_cleanup(owner, record);
}
#else
int module_cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void module_cxa_finalize(void*) {}
void discard_exit_callbacks(ImageState*) {}
bool retire_process_exit(ImageState*) { return true; }
bool exit_cleanup_uncertain() { return false; }
void abandon_exit_cleanup(pthread_t, bool) {}
#endif  // #if YUKILINKER_FULL

bool executable_pointer(ImageState* image, uintptr_t address) {
#if defined(__arm__)
  address &= ~uintptr_t{1};
#endif
  const auto bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  return address >= bias &&
         elf::segment_contains(image->memory.program_headers, image->memory.program_header_count,
                               address - bias, 1, PF_X);
}

LifecycleFunction normalized_lifecycle_function(ImageState*, LifecycleFunction function) {
  const auto value = reinterpret_cast<uintptr_t>(function);
  return value == UINTPTR_MAX ? nullptr : function;
}

namespace {

bool lifecycle_entry_valid(ImageState* image, LifecycleFunction entry) {
  const auto address = reinterpret_cast<uintptr_t>(entry);
  return address == 0 || address == UINTPTR_MAX || executable_pointer(image, address);
}

bool deferred_slot(const RelocationSet& set, uintptr_t offset) {
  for (size_t i = 0; i < set.deferred_count; ++i)
    if (set.deferred[i].offset == offset) return true;
  return false;
}

bool lifecycle_array_valid(ImageState* image, const LifecycleFunction* array, size_t count,
                           bool skip_deferred) {
  if (count == 0) return true;
  const auto& set = image->relocations;
  const uintptr_t first =
      reinterpret_cast<uintptr_t>(array) - reinterpret_cast<uintptr_t>(image->memory.bias);
  const uintptr_t bytes = count * sizeof(LifecycleFunction);
  // Search slot by slot only when a deferred relocation writes into this array at all.
  bool deferred = false;
  for (size_t i = 0; skip_deferred && !deferred && i < set.deferred_count; ++i)
    deferred = set.deferred[i].offset - first < bytes;
  for (size_t i = 0; i < count; ++i) {
    if (deferred && deferred_slot(set, first + (i * sizeof(LifecycleFunction)))) continue;
    if (!lifecycle_entry_valid(image, array[i])) return false;
  }
  return true;
}

}  // namespace

bool validate_init_fini(ImageState* image) {
  return lifecycle_entry_valid(image, image->lifecycle.init) &&
         lifecycle_entry_valid(image, image->lifecycle.fini);
}

bool validate_lifecycle(ImageState* image, bool skip_deferred) {
  return validate_init_fini(image) &&
         lifecycle_array_valid(image, image->lifecycle.init_array, image->lifecycle.init_count,
                               skip_deferred) &&
         lifecycle_array_valid(image, image->lifecycle.fini_array, image->lifecycle.fini_count,
                               skip_deferred);
}

bool run_initializers(ImageState* image) {
#if YUKILINKER_FULL
  if (!ensure_initializing_key()) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_INITIALIZATION);
  void* previous_initializing = pthread_getspecific(g_initializing_key);
  if (pthread_setspecific(g_initializing_key, image) != 0)
    return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_INITIALIZATION);
#endif  // #if YUKILINKER_FULL
  {
    CallbackGuard callback_scope(image);
    callback_scope.runs_image_code = true;
    LifecycleFunction init = normalized_lifecycle_function(image, image->lifecycle.init);
    if (init != nullptr) call_module(init);
    for (size_t i = 0; i < image->lifecycle.init_count; ++i) {
      LifecycleFunction entry =
          normalized_lifecycle_function(image, image->lifecycle.init_array[i]);
      if (entry != nullptr) call_module(entry);
    }
  }
#if YUKILINKER_FULL
  (void)pthread_setspecific(g_initializing_key, previous_initializing);
#endif  // #if YUKILINKER_FULL
  image->lifecycle.initialized = true;
#if YUKILINKER_FULL
  if (!image->host) {
    // The argument is the image's generation, not its address: see finalize_at_process_exit.
    if (__cxa_atexit(finalize_at_process_exit,
                     reinterpret_cast<void*>(static_cast<uintptr_t>(image->generation)),
                     &image->lifecycle.process_exit) != 0)
      return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_INITIALIZATION);
    image->lifecycle.process_exit.state = CleanupState::Registered;
    image->lifecycle.process_exit_registered = true;
  }
#endif
  return true;
}

void run_finalizers(ImageState* image) {
  if (image == nullptr || !image->lifecycle.initialized || image->lifecycle.fini_running) return;
  Lifecycle& lifecycle = image->lifecycle;
  if (exit_cleanup_uncertain()) {
    lifecycle.blocked_cleanup_pass = g_collection_pass;
    cleanup_failure(YUKILINKER_C_BUSY);
    return;
  }
  lifecycle.fini_running = true;
  lifecycle.fini_thread = pthread_self();
  // A run that a thread ended inside goes on from where it stopped; the event marks the first.
  if (!lifecycle.finalizing) {
    record_event(image, YUKILINKER_C_EVENT_DESTRUCTOR);
    lifecycle.finalizing = true;
    lifecycle.fini_steps_left = lifecycle.fini_count + 1;
  }
#if YUKILINKER_FULL
  // Pending cookies are independent of fini_steps_left: an unentered libc call
  // survives both an ordinary module call and a thread that ended in a finalizer.
  if (!resume_dso_cleanup(image)) {
    lifecycle.fini_running = false;
    lifecycle.blocked_cleanup_pass = g_collection_pass;
    return;
  }
#endif  // #if YUKILINKER_FULL
  // Each step is taken off under the lock, before the frame that drops it for the call.
  while (lifecycle.fini_steps_left != 0) {
    const size_t step = --lifecycle.fini_steps_left;
    LifecycleFunction entry = normalized_lifecycle_function(
        image, step != 0 ? lifecycle.fini_array[step - 1] : lifecycle.fini);
    if (entry == nullptr) continue;
    if (g_operation_error) g_operation_error->execution_started = 1;
    {
      CallbackGuard callback_scope(image);
      callback_scope.runs_image_code = true;
      call_module(entry);
    }
#if YUKILINKER_FULL
    // A void __cxa_finalize refused during this step must not be silently
    // retried in the same close or let subsequent destructor steps overtake it.
    if (dso_cleanup_pending(lifecycle)) {
      lifecycle.fini_running = false;
      lifecycle.blocked_cleanup_pass = g_collection_pass;
      cleanup_failure(exit_cleanup_uncertain() ? YUKILINKER_C_BUSY : YUKILINKER_C_IO_ERROR);
      return;
    }
#endif
  }
#if YUKILINKER_FULL
  drain_exit_callbacks(image, nullptr);
  if (dso_cleanup_pending(lifecycle)) {
    lifecycle.fini_running = false;
    lifecycle.blocked_cleanup_pass = g_collection_pass;
    cleanup_failure(exit_cleanup_uncertain() ? YUKILINKER_C_BUSY : YUKILINKER_C_IO_ERROR);
    return;
  }
#endif  // #if YUKILINKER_FULL
  lifecycle.initialized = false;
  lifecycle.fini_running = false;
}

#if YUKILINKER_FULL
// exit()'s __cxa_finalize(NULL) takes the entry out and drops libc's lock before this call, so a
// collection may free the image before the operation lock is ours. A generation is never reused
// (a handle may be), and only an image still registered is found by it: a late call, or one for a
// closed image whose entry stayed with libc, does nothing.
void finalize_at_process_exit(void* generation) {
  OperationGuard operation;
  ImageState* image = g_first_image;
  while (image && image->generation != reinterpret_cast<uintptr_t>(generation)) image = image->next;
  if (image == nullptr || !image->lifecycle.process_exit_registered) return;
  image->lifecycle.process_exit_registered = false;
  image->lifecycle.process_exit.state = CleanupState::Cleared;
  run_finalizers(image);
}
#endif

}  // namespace yukilinker::internal
