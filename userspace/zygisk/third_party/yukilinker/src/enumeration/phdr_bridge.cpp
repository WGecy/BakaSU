// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include <link.h>
#include <stddef.h>
#include <string.h>

#include "../internal/enumeration.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

namespace {

struct SystemIterateState {
  PrivateDlIterateCallback callback;
  void* user;
  ImageState** images;
  // Per snapshot entry: the system reported the same image.
  bool* reported;
  size_t count;
  // The private loads and unloads when the iteration began, added to the host's counters.
  uint64_t private_adds = 0;
  uint64_t private_subs = 0;
  // The host's counters, from its first entry that carries them.
  uint64_t host_adds = 0;
  uint64_t host_subs = 0;
  bool host_counted = false;
};

constexpr size_t kCountersEnd = offsetof(struct dl_phdr_info, dlpi_subs) + sizeof(uint64_t);

// A host that reports private images too (its own system_iterate_phdr) points dlpi_phdr at its
// own copy of the headers or into the mapped image, never at the runtime's metadata copy
// (create_address_space): an entry is the image when the bias, the count and the headers match.
bool reports_image(const ImageState* image, const struct dl_phdr_info* info, size_t size) {
  constexpr size_t kNeeded = offsetof(struct dl_phdr_info, dlpi_phnum) + sizeof(info->dlpi_phnum);
  const size_t count = image->memory.program_header_count;
  return size >= kNeeded && info->dlpi_phdr && count == info->dlpi_phnum &&
         reinterpret_cast<uintptr_t>(image->memory.bias) == info->dlpi_addr &&
         memcmp(image->memory.program_headers, info->dlpi_phdr, count * sizeof(ElfW(Phdr))) == 0;
}

int forward_system_image(struct dl_phdr_info* info, size_t size, void* opaque) {
  auto* state = static_cast<SystemIterateState*>(opaque);
  // The snapshot pins every entry, and these fields do not change once an image is loaded.
  for (size_t i = 0; i < state->count; ++i)
    if (!state->reported[i] && reports_image(state->images[i], info, size))
      state->reported[i] = true;
  if (size < kCountersEnd) return call_module(state->callback, info, size, state->user);
  // Every entry of one iteration carries the same counters, the host's plus the private ones, so a
  // private load or unload shows in the system entries as well.
  if (!state->host_counted) {
    state->host_adds = info->dlpi_adds;
    state->host_subs = info->dlpi_subs;
    state->host_counted = true;
  }
  struct dl_phdr_info entry{};
  memcpy(&entry, info, size < sizeof(entry) ? size : sizeof(entry));
  entry.dlpi_adds = state->host_adds + state->private_adds;
  entry.dlpi_subs = state->host_subs + state->private_subs;
  return call_module(state->callback, &entry, size < sizeof(entry) ? size : sizeof(entry),
                     state->user);
}

// A Closing image is still mapped while this thread's collection runs its finalizers, which may
// throw and catch inside it: the unwinder must find it. Another thread's collection may unmap it.
// releasing marks the one this thread's collection unmaps next. The lock stays held from that
// munmap to unregister_image, so only a signal handler on this thread can iterate then; another
// thread's snapshot never holds it, as its pin would have kept the image (held_image).
bool bridge_visible(const ImageState* image, const ImageState* caller) {
  if (caller && image->context != caller->context) return false;
  if (accessible_image(image)) return true;
  return image->phase == Phase::Closing && !image->releasing && g_collecting &&
         pthread_equal(g_collecting_thread, pthread_self());
}

}  // namespace

// Bionic's dl_iterate_phdr never fails, and an unwinder takes any nonzero result for a match, so an
// internal failure reports nothing (0). The iteration collects only when one of its callbacks
// closed an image it kept (collect_after_close); a close on another thread waits for it.
int module_dl_iterate_phdr(PrivateDlIterateCallback callback, void* user) {
  OperationGuard operation;
  auto* caller = caller_image(__builtin_return_address(0));
  return module_dl_iterate_phdr_for(caller, callback, user);
}

int module_dl_iterate_phdr_for(ImageState* caller, PrivateDlIterateCallback callback, void* user) {
  if (!callback) return 0;
  CallerCall call(caller);
  size_t count = 0;
  for (auto* image = g_first_image; image; image = image->next)
    if (bridge_visible(image, caller)) ++count;
  if (count > kMaxSnapshotBytes / sizeof(ImageState*)) {
    host_log_error("yukilinker: dl_iterate_phdr snapshot of %zu images is too large", count);
    return 0;
  }
  const size_t slots = count ? count : 1;
  // The snapshot, its array and the flags share one block: see SnapshotGuard.
  void* storage = calloc(1, sizeof(SnapshotGuard) + slots * (sizeof(ImageState*) + sizeof(bool)));
  if (!storage) {
    host_log_error("yukilinker: dl_iterate_phdr is out of memory");
    return 0;
  }
  auto** images =
      reinterpret_cast<ImageState**>(static_cast<char*>(storage) + sizeof(SnapshotGuard));
  auto* reported = reinterpret_cast<bool*>(images + slots);
  size_t filled = 0;
  for (auto* image = g_first_image; image && filled < count; image = image->next)
    if (bridge_visible(image, caller)) images[filled++] = image;
  SystemIterateState state{callback, user, images, reported, filled};
  state.private_adds = g_image_adds;
  state.private_subs = g_image_subs;
  int result = 0;
  auto* snapshot = new (storage) SnapshotGuard(images, filled, true);
  {
    CallbackGuard system_scope(nullptr);
    result = host_iterate_phdr(forward_system_image, &state);
  }
  for (size_t i = 0; i < filled && !result; ++i) {
    auto* image = images[i];
    if (reported[i]) continue;
    struct dl_phdr_info info{};
    info.dlpi_addr = reinterpret_cast<uintptr_t>(image->memory.bias);
    info.dlpi_name = image->origin;
    info.dlpi_phdr = image->memory.program_headers;
    info.dlpi_phnum = image->memory.program_header_count;
    info.dlpi_adds = state.host_adds + state.private_adds;
    info.dlpi_subs = state.host_subs + state.private_subs;
    // The runtime numbers its own TLS modules from 1, as the system linker numbers its own: a
    // private id is no index the host's __tls_get_addr knows, and would equal a system image's. 0
    // says there is none, and as in Bionic it goes with a null block;
    // yukilinker_c_image_record.tls_module_id and yukilinker_c_image_detail have both.
    info.dlpi_tls_modid = 0;
    info.dlpi_tls_data = nullptr;
    // The caller's callback runs, not image's code: image is only what it is told about.
    CallbackGuard callback_scope(image);
    result = call_module(callback, &info, sizeof(info), user);
  }
  const bool collect = snapshot->collect_after;
  snapshot->~SnapshotGuard();
  free(storage);
  // A callback closed an image this iteration kept. An unwinder's callback closes nothing, so
  // its frame lookups never collect.
  if (collect) (void)collect_images();
  return result;
}

#if defined(__arm__)
uintptr_t module_dl_unwind_find_exidx(uintptr_t pc, int* pcount) {
  OperationGuard operation;
  if (auto* image = caller_image(reinterpret_cast<void*>(pc))) {
    // Bionic's do_dl_unwind_find_exidx: the containing image's PT_ARM_EXIDX, or none.
    const auto* headers = image->memory.program_headers;
    for (size_t i = 0; i < image->memory.program_header_count; ++i) {
      if (headers[i].p_type != PT_ARM_EXIDX) continue;
      *pcount = static_cast<int>(headers[i].p_memsz / 8);
      return reinterpret_cast<uintptr_t>(image->memory.bias) + headers[i].p_vaddr;
    }
    *pcount = 0;
    return 0;
  }
  // Any other pc is the system's. The lookup and the call drop the lock: Bionic takes g_dl_mutex.
  void* system = nullptr;
  if (!host_dlsym(RTLD_DEFAULT, "dl_unwind_find_exidx", nullptr, &system) || !system) {
    *pcount = 0;
    return 0;
  }
  CallbackGuard system_scope(nullptr);
  return reinterpret_cast<uintptr_t (*)(uintptr_t, int*)>(system)(pc, pcount);
}
#endif

}  // namespace yukilinker::internal
