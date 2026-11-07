// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/caller_stubs.hpp"
#include "../internal/host_imports.hpp"
#include "../internal/registry.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

// Matches the current owner whether or not it still admits calls.
Runtime* owner_from(void* opaque) {
  if (opaque == nullptr || opaque != g_runtime_owner) return nullptr;
  auto* runtime = static_cast<Runtime*>(opaque);
  return runtime->magic == Runtime::kMagic ? runtime : nullptr;
}

// Callers hold an OperationGuard, so the owner cannot retire between this check and the call.
Runtime* runtime_from(void* opaque) {
  auto* runtime = owner_from(opaque);
  return runtime != nullptr && runtime->active ? runtime : nullptr;
}

// A stale owner overwrites the caller's error record instead of leaving old contents behind.
void report_stale(yukilinker_c_error_detail* error, uint32_t stage) {
  if (error == nullptr) return;
  *error = {};
  error->code = YUKILINKER_C_STALE_TOKEN;
  error->stage = stage;
}

// Shared by try_shutdown and runtime_destroy. On success the default host table is back in
// place and the owner admits nothing but runtime_destroy. The owner goes inactive at the
// shutdown's boundary (begin_shutdown), not before: a refusal leaves it as it was.
bool retire_runtime(Runtime* owner, yukilinker_c_error_detail* error) {
  if (!runtime_try_shutdown(error)) return false;
  owner->active = false;
  owner->shut_down = true;
  publish_host_services(nullptr);
  return true;
}

void* owner_runtime_create(const void* host_services) {
  const auto* services = static_cast<const yukilinker_c_host_services*>(host_services);
  if (!validate_host_services(services)) return nullptr;
  if (!ensure_operation_keys()) return nullptr;
  // The table belongs to the caller, so it is copied out before the owner keeps it.
  yukilinker_c_host_services copied = *services;
  copied.size = sizeof(yukilinker_c_host_services);

  // The copy is taken first, the lock second: nothing is latched until the call has been admitted.
  OperationGuard operation;
  // A second owner is refused before the page size hook runs for it, and a create issued from
  // that hook is refused for the same reason: this span has no owner yet.
  // Nor once a shutdown has passed its boundary: the runtime admits nothing new.
  if (g_runtime_owner != nullptr || g_runtime_creating || shutdown_begun()) return nullptr;
  if (!sole_operation()) return nullptr;
  CreateScope creating;
  // The hook runs in a callback frame, so an owner-changing call it makes is refused.
  size_t requested = 0;
  bool has_page_size = false;
  if (!query_host_page_size(copied, &requested, &has_page_size)) return nullptr;
  // The hook ran with the lock released and only creates were refused meanwhile, so another
  // thread's operation may have been admitted; the table must not change under it.
  if (!sole_operation()) return nullptr;
  auto* owner = static_cast<Runtime*>(calloc(1, sizeof(Runtime)));
  if (owner == nullptr) return nullptr;
  // Latched last, so a create that fails leaves no sticky page size behind.
  if (!adopt_host_page_size(requested, has_page_size)) {
    free(owner);
    return nullptr;
  }
  owner->magic = Runtime::kMagic;
  owner->active = true;
  owner->host_services = copied;
  __atomic_store_n(&g_runtime_owner, owner, __ATOMIC_RELEASE);
  publish_host_services(&owner->host_services);
  return owner;
}

uint32_t destroy_owner(void* opaque) {
  OperationGuard operation;
  auto* owner = owner_from(opaque);
  if (owner == nullptr) return 0;
  // A callback deeper in this same thread must not retire the owner under the call it is
  // reporting: the operation that entered it would then finish against a published default table.
  // An owner that already shut down has no table left to tear down, so it stays destroyable.
  if (!owner->shut_down && !sole_holder()) return 0;
  // An inactive owner that has not shut down is past the shutdown's boundary: this call resumes.
  if (!owner->shut_down && !retire_runtime(owner, nullptr)) return 0;
  // Only now: a pointer an image handed out stays callable, reporting its image retired, until
  // the owner is destroyed, try_shutdown included (yukilinker.h, namespace_create). A page that
  // cannot be unmapped stays owned so the host can retry before unmapping the runtime itself.
  if (!release_caller_stub_pages()) {
    host_log_error("yukilinker: a caller stub page could not be unmapped");
    return 0;
  }
  // The storage stays behind as a tombstone; no later handle can match it.
  modules_free_state();
  __atomic_store_n(&g_runtime_owner, static_cast<Runtime*>(nullptr), __ATOMIC_RELEASE);
  return 1;
}

// The runtime's atfork handlers go once the lock is released, before the host can unmap it.
uint32_t owner_runtime_destroy(void* opaque) {
  const uint32_t destroyed = destroy_owner(opaque);
  if (destroyed) retire_fork_handlers();
  return destroyed;
}

uint32_t shut_down_owner(void* opaque, yukilinker_c_error_detail* error) {
  OperationGuard operation;
  // A retired owner outranks the busy check: after shutdown it is stale from any depth. One past
  // the boundary, inactive, is still accepted here: this call resumes its shutdown.
  auto* owner = owner_from(opaque);
  if (owner == nullptr || owner->shut_down) {
    report_stale(error, YUKILINKER_C_RETIREMENT);
    return 0;
  }
  // Refused for the same reason as runtime_destroy: BUSY before the boundary (nothing changed),
  // RETRY past it (the shutdown has begun and the next call resumes).
  if (!sole_holder()) {
    if (error) {
      *error = {};
      error->code = shutdown_begun() ? YUKILINKER_C_RETRY : YUKILINKER_C_BUSY;
      error->stage = YUKILINKER_C_RETIREMENT;
    }
    return 0;
  }
  return retire_runtime(owner, error) ? 1 : 0;
}

uint32_t owner_try_shutdown(void* opaque, yukilinker_c_error_detail* error) {
  const uint32_t retired = shut_down_owner(opaque, error);
  if (retired) retire_fork_handlers();
  return retired;
}

uint32_t owner_host_services_bind(void* opaque, const void* host_services) {
  OperationGuard operation;
  if (runtime_from(opaque) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  if (modules_has_holders()) return YUKILINKER_C_BUSY;
  // A host callback or another thread may still be running with the current table, and a
  // nested bind would invalidate the table the running callback was entered through.
  if (host_services == nullptr) {
    if (!sole_operation()) return YUKILINKER_C_BUSY;
    publish_host_services(nullptr);
    return YUKILINKER_C_OK;
  }
  const auto* services = static_cast<const yukilinker_c_host_services*>(host_services);
  if (!validate_host_services(services)) return YUKILINKER_C_UNSUPPORTED;
  if (!sole_operation()) return YUKILINKER_C_BUSY;
  // The table belongs to the caller, so it is copied out before any host code runs.
  yukilinker_c_host_services copied = *services;
  copied.size = sizeof(yukilinker_c_host_services);
  size_t requested = 0;
  bool has_page_size = false;
  if (!query_host_page_size(copied, &requested, &has_page_size)) return YUKILINKER_C_UNSUPPORTED;
  auto* owner = static_cast<Runtime*>(opaque);
  // The hook ran with the mutation lock released, so the owner may have retired meanwhile, and a
  // nested bind must not invalidate the table the running callback was entered through.
  if (!sole_operation()) return YUKILINKER_C_BUSY;
  if (runtime_from(opaque) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  if (!adopt_host_page_size(requested, has_page_size)) return YUKILINKER_C_UNSUPPORTED;
  owner->host_services = copied;
  publish_host_services(&owner->host_services);
  return YUKILINKER_C_OK;
}

uint32_t owner_context_destroy(void* opaque, void* context) {
  OperationGuard operation;
  if (runtime_from(opaque) == nullptr) return 0;
  return loader_destroy_context(context) != 0;
}

// Registry operations return an error code, so a stale owner must not return zero.
uint32_t owner_snapshot_acquire(void* opaque, void* context, uint64_t* token, size_t* count) {
  OperationGuard operation;
  if (runtime_from(opaque) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_snapshot_acquire(context, token, count);
}

uint32_t owner_snapshot_image(void* opaque, uint64_t token, size_t index,
                              yukilinker_c_image_record* record) {
  OperationGuard operation;
  if (runtime_from(opaque) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_snapshot_image(token, index, record);
}

uint32_t owner_snapshot_headers(void* runtime, uint64_t token, size_t index, void* headers,
                                size_t capacity, size_t* required) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_snapshot_headers(token, index, headers, capacity, required);
}

uint32_t owner_snapshot_release(void* runtime, uint64_t token) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_snapshot_release(token);
}

uint32_t owner_image_acquire(void* runtime, yukilinker_c_image_id id, uint32_t kind,
                             uint64_t* lease) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_image_acquire_kind(id, kind, lease);
}

uint32_t owner_image_query(void* runtime, yukilinker_c_image_id id,
                           yukilinker_c_image_record* record) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_image_query(id, record);
}

uint32_t owner_image_release(void* runtime, uint64_t lease) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_image_release(lease);
}

uint32_t owner_read_events(void* runtime, uint64_t cursor, yukilinker_c_runtime_event* events,
                           size_t capacity, size_t* written, uint64_t* next_cursor) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_read_events(cursor, events, capacity, written, next_cursor);
}

uint32_t owner_namespace_destroy(void* runtime, void* token) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_namespace_destroy(token);
}

uint64_t owner_namespace_id(void* runtime, void* token) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return 0;
  return registry_namespace_id(token);
}

uint32_t owner_snapshot_mapping(void* runtime, uint64_t token, size_t index,
                                yukilinker_c_mapping_record* record) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return registry_snapshot_mapping(token, index, record);
}

uint32_t owner_image_detail(void* runtime, yukilinker_c_image_id id,
                            yukilinker_c_image_detail* detail) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return query_image_detail(id, detail);
}

uint32_t owner_image_dependencies(void* runtime, yukilinker_c_image_id id,
                                  yukilinker_c_dependency_record* records, size_t capacity,
                                  size_t* count) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return query_image_dependencies(id, records, capacity, count);
}

uint32_t owner_image_program_headers(void* runtime, yukilinker_c_image_id id, void* headers,
                                     size_t capacity, size_t* required) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return query_image_program_headers(id, headers, capacity, required);
}

uint32_t owner_namespace_link(void* runtime, void* from, void* to, uint32_t flags,
                              const char* shared_sonames) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return api_namespace_link(from, to, flags, shared_sonames);
}

uint32_t owner_namespace_links(void* runtime, void* from,
                               yukilinker_c_namespace_link_record* records, size_t capacity,
                               size_t* count) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return api_namespace_links(from, records, capacity, count);
}

uint32_t owner_namespace_host_import(void* runtime, void* namespace_token,
                                     const yukilinker_c_host_import* import) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return api_namespace_host_import(namespace_token, import);
}

// Only copies the thread's own record: no host call, no mapping, no lookup, and it ends no
// operation of its own, so it never clears what it reports.
uint32_t owner_host_import_refusal(void* opaque, yukilinker_c_host_import_refusal* refusal) {
  OperationGuard operation;
  if (runtime_from(opaque) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return api_host_import_refusal(refusal);
}

uint32_t owner_wait_timeout(void* opaque, uint64_t nanoseconds) {
  OperationGuard operation;
  if (runtime_from(opaque) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  __atomic_store_n(&g_wait_timeout_ns, nanoseconds, __ATOMIC_RELEASE);
  return YUKILINKER_C_OK;
}

// One slot as it stood between two reads of its sequence, or false when it was free or kept
// changing. Reads only: no lock, no allocation, nothing a thread holding the lock can block.
bool read_wait_slot(const WaitSlot& slot, uint64_t now, yukilinker_c_load_wait* record) {
  for (int attempt = 0; attempt < 16; ++attempt) {
    const uint32_t before = __atomic_load_n(&slot.sequence, __ATOMIC_ACQUIRE);
    if (before & 1) continue;
    const int32_t thread = __atomic_load_n(&slot.thread, __ATOMIC_RELAXED);
    const uint32_t kind = __atomic_load_n(&slot.kind, __ATOMIC_RELAXED);
    const int32_t owner = __atomic_load_n(&slot.owner, __ATOMIC_RELAXED);
    const uint64_t generation = __atomic_load_n(&slot.generation, __ATOMIC_RELAXED);
    const uint64_t namespace_id = __atomic_load_n(&slot.namespace_id, __ATOMIC_RELAXED);
    const uint64_t start = __atomic_load_n(&slot.start_ns, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot.sequence, __ATOMIC_RELAXED) != before) continue;
    if (thread == 0 || kind == 0) return false;
    *record = {};
    record->size = sizeof(*record);
    record->kind = kind;
    record->thread = thread;
    record->owner = owner;
    record->image = {generation, namespace_id};
    record->elapsed_ns = now > start ? now - start : 0;
    return true;
  }
  return false;
}

// No OperationGuard: answering while another thread holds the lock is the point. The owner is
// read without the lock as well; a stale one is refused, a retired one past shutdown still reads.
uint32_t owner_load_waits(void* opaque, yukilinker_c_load_wait* records, size_t capacity,
                          size_t* count) {
  if (opaque == nullptr || opaque != __atomic_load_n(&g_runtime_owner, __ATOMIC_ACQUIRE))
    return YUKILINKER_C_STALE_TOKEN;
  if (!count) return YUKILINKER_C_INVALID_HANDLE;
  if (capacity != 0 && (!records || records[0].size != sizeof(yukilinker_c_load_wait)))
    return YUKILINKER_C_UNSUPPORTED;
  const uint64_t now = monotonic_now_ns();
  size_t found = 0;
  for (const WaitSlot& slot : g_wait_slots) {
    yukilinker_c_load_wait record;
    if (!read_wait_slot(slot, now, &record)) continue;
    if (found < capacity) records[found] = record;
    ++found;
  }
  const size_t overflow = __atomic_load_n(&g_wait_overflow, __ATOMIC_ACQUIRE);
  *count = found + overflow;
  return found <= capacity && overflow == 0 ? YUKILINKER_C_OK : YUKILINKER_C_BUFFER_TOO_SMALL;
}

uint32_t owner_namespace_host_imports(void* runtime, void* namespace_token,
                                      yukilinker_c_host_import_record* records, size_t capacity,
                                      size_t* count) {
  OperationGuard operation;
  if (runtime_from(runtime) == nullptr) return YUKILINKER_C_STALE_TOKEN;
  return api_namespace_host_imports(namespace_token, records, capacity, count);
}

}  // namespace yukilinker::internal
