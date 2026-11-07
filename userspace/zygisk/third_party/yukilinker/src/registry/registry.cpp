// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/registry.hpp"

#include <algorithm>
#include <cstddef>
#include <type_traits>

#include "../internal/abi_layout.hpp"
#include "../internal/enumeration.hpp"
#include "../internal/host_imports.hpp"
#include "../internal/token_index.hpp"

namespace yukilinker::internal {

namespace {

struct RuntimeSnapshotEntry {
  ImageState* image;
  yukilinker_c_image_record record;
  yukilinker_c_mapping_record mapping;
};
// Entries come zeroed from calloc and are not constructed again.
static_assert(std::is_trivially_default_constructible_v<RuntimeSnapshotEntry>);

struct RuntimeSnapshot {
  uint64_t token = 0;
  Context* context = nullptr;
  RuntimeSnapshotEntry* entries = nullptr;
  size_t count = 0;
  RuntimeSnapshot* next = nullptr;
};

struct RuntimeImageLease {
  uint64_t token = 0;
  ImageState* image = nullptr;
  uint32_t kind = YUKILINKER_C_LEASE_OBSERVER;
  RuntimeImageLease* next = nullptr;
  RuntimeImageLease* previous = nullptr;
};

RuntimeSnapshot* g_runtime_snapshots = nullptr;
size_t g_runtime_snapshot_bytes = 0;

RuntimeImageLease* g_runtime_leases = nullptr;

// Every live snapshot and lease by its token, so issuing and finding one costs no walk of the
// others. The lists keep their order for the walks that need every entry.
TokenIndex<RuntimeSnapshot> g_snapshot_index;
TokenIndex<RuntimeImageLease> g_lease_index;

// Snapshot and lease tokens are random, as image handles are: no other code can release one it was
// never given by counting. Never 0 and never one still live.
uint64_t fresh_snapshot_token() {
  return random_token<uint64_t>(
      0, 0, [](uint64_t value) { return g_snapshot_index.find(value) != nullptr; });
}

uint64_t fresh_lease_token() {
  return random_token<uint64_t>(
      0, 0, [](uint64_t value) { return g_lease_index.find(value) != nullptr; });
}

uint32_t public_phase(const ImageState* image) { return static_cast<uint32_t>(image->phase); }

// Registry -> ABI record copy. The only serializer for yukilinker_c_image_record.
void fill_image_record(const ImageState* image, yukilinker_c_image_record* record,
                       uint64_t dependency_references) {
  *record = {};
  record->size = sizeof(*record);
  record->phase = public_phase(image);
  record->id = {image->generation, image->namespace_id};
  record->load_bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  record->mapping_start = reinterpret_cast<uintptr_t>(image->memory.reservation);
  record->mapping_size = image->memory.span;
  record->device = image->device;
  record->inode = image->inode;
  record->source_offset = image->source_offset;
  record->flags = image->flags;
  record->program_header_count = image->memory.program_header_count;
  record->tls_module_id = image->tls.module_id;
  record->tls_generation = image->tls.generation;
  record->open_references = image->references;
  record->tls_references = image->tls_users;
  record->destructor_references = image->thread_destructors;
  record->callback_references = image->active_calls;
  record->snapshot_references = image->snapshot_refs;
  record->lease_references = image->lease_refs;
  record->execution_leases = image->execution_leases;
  record->unwind_leases = image->unwind_leases;
  record->cfi_leases = image->cfi_leases;
  record->host_bridge_leases = image->host_bridge_leases;
  record->nodelete_reasons = image->nodelete_reasons;
  record->failure_code = image->failure_code;
  for (auto* dep = image->dependencies; dep; dep = dep->next) {
    if (dep->image)
      ++record->private_dependencies;
    else if (dep->system_handle)
      ++record->system_dependencies;
  }
  record->dependency_references = dependency_references;
  (void)snprintf(record->origin, sizeof(record->origin), "%s", image->origin);
}

// Edge targets are compared, never dereferenced. Collection clears every edge to an image before
// it frees it, so this is a second guard.
uint64_t count_dependency_references(const ImageState* image) {
  uint64_t count = 0;
  for (auto* owner = g_first_image; owner; owner = owner->next)
    for (auto* dep = owner->dependencies; dep; dep = dep->next)
      if (dep->image == image) ++count;
  return count;
}

struct EntryOrder {
  uintptr_t image;
  size_t index;
};

int compare_entry_order(const void* left, const void* right) {
  const uintptr_t a = static_cast<const EntryOrder*>(left)->image;
  const uintptr_t b = static_cast<const EntryOrder*>(right)->image;
  return (a > b) - (a < b);
}

// Counts the references of every entry in one pass over the edges, looking each target up by
// address so it is never dereferenced.
void count_snapshot_dependencies(RuntimeSnapshot* snapshot) {
  auto* order =
      static_cast<EntryOrder*>(calloc(snapshot->count ? snapshot->count : 1, sizeof(EntryOrder)));
  if (!order) {
    for (size_t i = 0; i < snapshot->count; ++i)
      snapshot->entries[i].record.dependency_references =
          count_dependency_references(snapshot->entries[i].image);
    return;
  }
  for (size_t i = 0; i < snapshot->count; ++i)
    order[i] = {reinterpret_cast<uintptr_t>(snapshot->entries[i].image), i};
  qsort(order, snapshot->count, sizeof(*order), compare_entry_order);
  for (auto* owner = g_first_image; owner; owner = owner->next)
    for (auto* dep = owner->dependencies; dep; dep = dep->next) {
      if (!dep->image) continue;
      const EntryOrder key{reinterpret_cast<uintptr_t>(dep->image), 0};
      const auto* match = static_cast<const EntryOrder*>(
          bsearch(&key, order, snapshot->count, sizeof(*order), compare_entry_order));
      if (match) ++snapshot->entries[match->index].record.dependency_references;
    }
  free(order);
}

ImageState* find_image_id(yukilinker_c_image_id id) {
  for (auto* image = g_first_image; image; image = image->next)
    if (image->generation == id.generation && image->namespace_id == id.namespace_id) return image;
  return nullptr;
}

RuntimeSnapshot* find_snapshot(uint64_t token) { return g_snapshot_index.find(token); }

// Namespace tokens are random with the low bits 3. A value of that shape that no live namespace
// holds is stale: destroyed, or never issued, which nothing can tell apart without a record of
// every token ever issued. Called only for a value that no live namespace holds.
bool retired_namespace(const void* opaque) {
  const auto value = reinterpret_cast<uintptr_t>(opaque);
  return (value & 3) == 3 && value / 4 != 0;
}

// Another thread's load stays hidden until it is ready. Failed and closing images stay
// queryable while something pins them.
bool foreign_load(const ImageState* image) {
  return (image->phase == Phase::Preparing || image->phase == Phase::Relocated ||
          image->phase == Phase::Initializing) &&
         !pthread_equal(image->loading_thread, pthread_self());
}

// The typed counter a lease kind maintains; an observer lease has none.
size_t* typed_lease_counter(ImageState* image, uint32_t kind) {
  switch (kind) {
    case YUKILINKER_C_LEASE_EXECUTION:
      return &image->execution_leases;
    case YUKILINKER_C_LEASE_UNWIND:
      return &image->unwind_leases;
    case YUKILINKER_C_LEASE_CFI:
      return &image->cfi_leases;
    case YUKILINKER_C_LEASE_HOST_BRIDGE:
      return &image->host_bridge_leases;
    default:
      return nullptr;
  }
}

uint32_t mapping_flags(const ImageState* image) {
  return image->memory.relro_flags |
         (image->memory.file_backed ? YUKILINKER_C_MAPPING_FILE_BACKED : 0U) |
         (image->memory.caller_reserved ? YUKILINKER_C_MAPPING_CALLER_RESERVED : 0U);
}

// The same visibility as image_query.
ImageState* visible_image(yukilinker_c_image_id id) {
  auto* image = find_image_id(id);
  return image && !foreign_load(image) ? image : nullptr;
}

// The registered image at an edge's target, found by comparing addresses only.
const ImageState* registered_image(const ImageState* target) {
  for (auto* image = g_first_image; image; image = image->next)
    if (image == target) return image;
  return nullptr;
}

// Complete image detail record for the public ABI.
constexpr size_t kImageDetailSize =
    offsetof(yukilinker_c_image_detail, relro_shared_bytes) + sizeof(uint64_t);
static_assert(kImageDetailSize == 128 && kImageDetailSize <= sizeof(yukilinker_c_image_detail));

}  // namespace

NamespaceToken* find_namespace(void* opaque) {
  return g_namespace_index.find(reinterpret_cast<uintptr_t>(opaque));
}

bool registry_has_holders() {
  return g_runtime_snapshots != nullptr || g_runtime_leases != nullptr;
}

uint32_t query_handle_record(void* handle, yukilinker_c_image_record* record) {
  OperationGuard operation;
  if (!record) return YUKILINKER_C_INVALID_HANDLE;
  auto* image = state_of(static_cast<SoHandle*>(handle));
  if (!image || !accessible_image(image) || (private_token(handle) && !image->references))
    return YUKILINKER_C_INVALID_HANDLE;
  fill_image_record(image, record, count_dependency_references(image));
  return YUKILINKER_C_OK;
}

void* registry_namespace_create(void* opaque) {
  OperationGuard operation;
  Context* base = context_from_opaque(opaque);
  // A context under retirement gains no namespace.
  if (!base || context_sealed(base) || g_next_namespace_id == UINT64_MAX) return nullptr;
  // Random, as image handles are: namespace_id, which records and events show, does not tell it.
  // The low bits 3 keep it apart from contexts and image handles.
  const uintptr_t opaque_token = random_token<uintptr_t>(
      3, 3, [](uintptr_t value) { return g_namespace_index.find(value) != nullptr; });
  if (opaque_token == 0) return nullptr;
  auto* context = allocate_object<Context>();
  auto* token = allocate_object<NamespaceToken>();
  // Indexed before anything is linked, so a failed insert changes nothing.
  if (!context || !token || !g_namespace_index.insert(opaque_token, token)) {
    free_object(token);
    free_object(context);
    return nullptr;
  }
  *context = {};
  (void)snprintf(context->search_path, sizeof(context->search_path), "%s", base->search_path);
  context->provider = base->provider;
  context->strict_private = base->strict_private;
  context->user = base->user;
  context->namespace_id = g_next_namespace_id++;
  context->namespace_parent = base;
  ++base->namespace_refs;
  link_context(context);
  token->id = context->namespace_id;
  token->opaque = reinterpret_cast<void*>(opaque_token);
  token->context = context;
  link_namespace(token);
  return token->opaque;
}

uint32_t registry_namespace_destroy(void* opaque) {
  OperationGuard operation;
  auto* token = find_namespace(opaque);
  if (!token) return YUKILINKER_C_STALE_TOKEN;
  auto* context = token->context;
  if (context->snapshot_refs || context->namespace_refs || context->link_refs ||
      context->waiting_opens)
    return YUKILINKER_C_BUSY;
  // Its images go with it, RTLD_GLOBAL ones included. BUSY changed nothing; once the namespace is
  // sealed, IO_ERROR means part of it is retired and the next call resumes.
  if (const uint32_t retired = retire_scope_images(context); retired != YUKILINKER_C_OK)
    return retired;
  // Retirement drops the lock for finalizers; look the token up again.
  token = find_namespace(opaque);
  if (!token) return YUKILINKER_C_STALE_TOKEN;
  context = token->context;
  if (context->snapshot_refs || context->namespace_refs || context->link_refs ||
      context->waiting_opens)
    return YUKILINKER_C_BUSY;
  if (context->images != 0) return YUKILINKER_C_BUSY;
  unlink_context(context);
  unlink_namespace(token);
  g_namespace_index.erase(reinterpret_cast<uintptr_t>(token->opaque));
  --context->namespace_parent->namespace_refs;
  while (auto* link = context->links) {
    context->links = link->next;
    if (link->target) --link->target->link_refs;
    free_object(link);
  }
  // Unlinked first: these host calls drop the lock, and nothing may find the context meanwhile. A
  // library the host failed to close is logged and reported by event and by the shutdown's
  // cleanup_incomplete; the namespace is gone. Imports a thread that ends in here leaves are closed
  // by the next release.
  (void)release_host_imports(context);
  free_object(context);
  free_object(token);
  return YUKILINKER_C_OK;
}

uint64_t registry_namespace_id(void* opaque) {
  OperationGuard operation;
  auto* token = find_namespace(opaque);
  return token ? token->id : 0;
}

namespace {

// A value shaped like a namespace token that no live namespace holds is stale, retired or never
// issued; anything else is not a namespace at all.
uint32_t missing_namespace(const void* opaque) {
  const auto value = reinterpret_cast<uintptr_t>(opaque);
  return (value & 3) == 3 && value / 4 != 0 ? YUKILINKER_C_STALE_TOKEN
                                            : YUKILINKER_C_INVALID_HANDLE;
}

// Every entry 1-255 bytes without '/', none empty, 4095 bytes in all; with ALL, nothing at all.
bool valid_shared_sonames(uint32_t flags, const char* names) {
  if ((flags & YUKILINKER_C_NAMESPACE_LINK_ALL) != 0) return names == nullptr || names[0] == '\0';
  if (names == nullptr || names[0] == '\0') return false;
  const size_t total = strnlen(names, sizeof(NamespaceLink::shared));
  if (total == sizeof(NamespaceLink::shared)) return false;
  size_t entry = 0;
  for (size_t i = 0; i <= total; ++i) {
    if (i == total || names[i] == ':') {
      if (entry == 0 || entry > 255) return false;
      entry = 0;
    } else if (names[i] == '/') {
      return false;
    } else {
      ++entry;
    }
  }
  return true;
}

}  // namespace

uint32_t api_namespace_link(void* from, void* to, uint32_t flags, const char* shared_sonames) {
  OperationGuard operation;
  NamespaceToken* source = find_namespace(from);
  if (!source) return missing_namespace(from);
  Context* target = nullptr;
  if ((flags & YUKILINKER_C_NAMESPACE_LINK_SYSTEM) != 0) {
    if (to) return YUKILINKER_C_UNSUPPORTED;
  } else {
    NamespaceToken* destination = find_namespace(to);
    if (!destination) return missing_namespace(to);
    target = destination->context;
  }
  Context* context = source->context;
  if (context->strict_private && target && !target->strict_private) return YUKILINKER_C_UNSUPPORTED;
  if ((flags & ~(YUKILINKER_C_NAMESPACE_LINK_SYSTEM | YUKILINKER_C_NAMESPACE_LINK_ALL)) != 0 ||
      target == context || !valid_shared_sonames(flags, shared_sonames))
    return YUKILINKER_C_UNSUPPORTED;
  // A namespace under retirement gains no link, from or to it.
  if (context->links_frozen || context_sealed(context) || context_sealed(target))
    return YUKILINKER_C_BUSY;
  size_t count = 0;
  NamespaceLink** tail = &context->links;
  for (; *tail; tail = &(*tail)->next, ++count)
    if ((*tail)->target == target) return YUKILINKER_C_UNSUPPORTED;
  if (count == kMaxNamespaceLinks) return YUKILINKER_C_UNSUPPORTED;
  auto* link = allocate_object<NamespaceLink>();
  if (!link) return YUKILINKER_C_NO_MEMORY;
  link->target = target;
  link->flags = flags;
  if (shared_sonames) (void)snprintf(link->shared, sizeof(link->shared), "%s", shared_sonames);
  *tail = link;
  if (target) ++target->link_refs;
  return YUKILINKER_C_OK;
}

uint32_t api_namespace_links(void* from, yukilinker_c_namespace_link_record* records,
                             size_t capacity, size_t* count) {
  OperationGuard operation;
  if (!count) return YUKILINKER_C_INVALID_HANDLE;
  NamespaceToken* source = find_namespace(from);
  if (!source) return missing_namespace(from);
  size_t links = 0;
  for (auto* link = source->context->links; link; link = link->next) ++links;
  *count = links;
  if (capacity < links || (links && !records)) return YUKILINKER_C_BUFFER_TOO_SMALL;
  size_t index = 0;
  for (auto* link = source->context->links; link; link = link->next) {
    yukilinker_c_namespace_link_record& record = records[index++];
    record = {};
    record.flags = link->flags;
    record.target_namespace_id = link->target ? link->target->namespace_id : 0;
    memcpy(record.shared_sonames, link->shared, sizeof(record.shared_sonames));
  }
  return YUKILINKER_C_OK;
}

uint32_t registry_snapshot_acquire(void* context, uint64_t* token, size_t* count) {
  OperationGuard operation;
  if (!token || !count) return YUKILINKER_C_INVALID_HANDLE;
  Context* selected = context ? context_from_opaque(context) : nullptr;
  if (context && !selected)
    return retired_namespace(context) ? YUKILINKER_C_STALE_TOKEN : YUKILINKER_C_INVALID_HANDLE;
  // A context under retirement gains no pin, nor does a runtime past its shutdown's boundary.
  if (context_sealed(selected) || shutdown_begun()) return YUKILINKER_C_BUSY;
  size_t image_count = 0;
  for (auto* image = g_first_image; image; image = image->next)
    if (accessible_image(image) && (!selected || image->context == selected)) ++image_count;
  if (image_count > kMaxSnapshotBytes / sizeof(RuntimeSnapshotEntry)) return YUKILINKER_C_NO_MEMORY;
  const size_t allocation_count = image_count ? image_count : 1;
  const size_t allocation_bytes = allocation_count * sizeof(RuntimeSnapshotEntry);
  if (allocation_bytes > kMaxSnapshotBytes - g_runtime_snapshot_bytes)
    return YUKILINKER_C_NO_MEMORY;
  auto* snapshot = allocate_object<RuntimeSnapshot>();
  if (!snapshot) return YUKILINKER_C_NO_MEMORY;
  snapshot->entries =
      static_cast<RuntimeSnapshotEntry*>(calloc(allocation_count, sizeof(RuntimeSnapshotEntry)));
  if (!snapshot->entries) {
    free_object(snapshot);
    return YUKILINKER_C_NO_MEMORY;
  }
  snapshot->token = fresh_snapshot_token();
  if (snapshot->token == 0 || !g_snapshot_index.insert(snapshot->token, snapshot)) {
    free(snapshot->entries);
    free_object(snapshot);
    return YUKILINKER_C_NO_MEMORY;
  }
  g_runtime_snapshot_bytes += allocation_bytes;
  snapshot->context = selected;
  if (snapshot->context) ++snapshot->context->snapshot_refs;
  snapshot->count = image_count;
  size_t index = 0;
  for (auto* image = g_first_image; image; image = image->next) {
    if (!accessible_image(image) || (selected && image->context != selected)) continue;
    ++image->snapshot_refs;
    auto& entry = snapshot->entries[index++];
    entry.image = image;
    fill_image_record(image, &entry.record, 0);
    entry.mapping = {sizeof(yukilinker_c_mapping_record), mapping_flags(image),
                     image->memory.relro_written_bytes, image->memory.relro_shared_bytes};
  }
  count_snapshot_dependencies(snapshot);
  snapshot->next = g_runtime_snapshots;
  g_runtime_snapshots = snapshot;
  *token = snapshot->token;
  *count = snapshot->count;
  return YUKILINKER_C_OK;
}

uint32_t registry_snapshot_image(uint64_t token, size_t index, yukilinker_c_image_record* record) {
  OperationGuard operation;
  auto* snapshot = find_snapshot(token);
  if (!snapshot) return YUKILINKER_C_STALE_TOKEN;
  if (!record || index >= snapshot->count) return YUKILINKER_C_INVALID_HANDLE;
  *record = snapshot->entries[index].record;
  return YUKILINKER_C_OK;
}

uint32_t registry_snapshot_headers(uint64_t token, size_t index, void* output, size_t bytes,
                                   size_t* required) {
  OperationGuard operation;
  auto* snapshot = find_snapshot(token);
  if (!snapshot) return YUKILINKER_C_STALE_TOKEN;
  if (index >= snapshot->count || !required) return YUKILINKER_C_INVALID_HANDLE;
  return copy_program_headers(snapshot->entries[index].image, output, bytes, required);
}

uint32_t registry_snapshot_mapping(uint64_t token, size_t index,
                                   yukilinker_c_mapping_record* record) {
  OperationGuard operation;
  auto* snapshot = find_snapshot(token);
  if (!snapshot) return YUKILINKER_C_STALE_TOKEN;
  if (!record || index >= snapshot->count) return YUKILINKER_C_INVALID_HANDLE;
  *record = snapshot->entries[index].mapping;
  return YUKILINKER_C_OK;
}

uint32_t registry_snapshot_release(uint64_t token) {
  OperationGuard operation;
  if (!g_snapshot_index.find(token)) return YUKILINKER_C_STALE_TOKEN;
  RuntimeSnapshot** link = &g_runtime_snapshots;
  while (*link && (*link)->token != token) link = &(*link)->next;
  if (!*link) return YUKILINKER_C_STALE_TOKEN;
  auto* snapshot = *link;
  *link = snapshot->next;
  g_snapshot_index.erase(token);
  for (size_t i = 0; i < snapshot->count; ++i)
    if (snapshot->entries[i].image->snapshot_refs) --snapshot->entries[i].image->snapshot_refs;
  g_runtime_snapshot_bytes -=
      (snapshot->count ? snapshot->count : 1) * sizeof(RuntimeSnapshotEntry);
  if (snapshot->context) --snapshot->context->snapshot_refs;
  free(snapshot->entries);
  free_object(snapshot);
  return collect_images() ? YUKILINKER_C_OK : YUKILINKER_C_IO_ERROR;
}

uint32_t registry_image_acquire_kind(yukilinker_c_image_id id, uint32_t kind, uint64_t* lease) {
  OperationGuard operation;
  if (!lease) return YUKILINKER_C_INVALID_HANDLE;
  if (kind > YUKILINKER_C_LEASE_HOST_BRIDGE) return YUKILINKER_C_UNSUPPORTED;
  auto* image = find_image_id(id);
  if (!image || !accessible_image(image)) return YUKILINKER_C_STALE_TOKEN;
  // An image of a context under retirement gains no pin.
  if (context_sealed(image->context)) return YUKILINKER_C_BUSY;
  const uint64_t token = fresh_lease_token();
  if (token == 0) return YUKILINKER_C_NO_MEMORY;
  auto* entry = allocate_object<RuntimeImageLease>();
  if (!entry) return YUKILINKER_C_NO_MEMORY;
  entry->token = token;
  entry->image = image;
  entry->kind = kind;
  if (!g_lease_index.insert(token, entry)) {
    free_object(entry);
    return YUKILINKER_C_NO_MEMORY;
  }
  // Doubly linked, so a release unlinks without a walk.
  entry->next = g_runtime_leases;
  if (g_runtime_leases) g_runtime_leases->previous = entry;
  g_runtime_leases = entry;
  ++image->lease_refs;
  if (auto* counter = typed_lease_counter(image, kind)) ++*counter;
  *lease = entry->token;
  return YUKILINKER_C_OK;
}

uint32_t registry_image_acquire(yukilinker_c_image_id id, uint64_t* lease) {
  return registry_image_acquire_kind(id, YUKILINKER_C_LEASE_OBSERVER, lease);
}

uint32_t registry_image_query(yukilinker_c_image_id id, yukilinker_c_image_record* record) {
  OperationGuard operation;
  auto* image = find_image_id(id);
  if (!record) return YUKILINKER_C_INVALID_HANDLE;
  if (!image || foreign_load(image)) return YUKILINKER_C_STALE_TOKEN;
  fill_image_record(image, record, count_dependency_references(image));
  return YUKILINKER_C_OK;
}

uint32_t query_image_detail(yukilinker_c_image_id id, yukilinker_c_image_detail* output) {
  OperationGuard operation;
  if (!output) return YUKILINKER_C_INVALID_HANDLE;
  // The largest known version that fits the caller's size; there is one so far.
  if (output->size < kImageDetailSize) return YUKILINKER_C_BUFFER_TOO_SMALL;
  const ImageState* image = visible_image(id);
  if (!image) return YUKILINKER_C_STALE_TOKEN;
  yukilinker_c_image_detail detail{};
  detail.size = kImageDetailSize;
  detail.id = {image->generation, image->namespace_id};
  detail.failure_code = image->failure_code;
  if (image->failure_stage_known) {
    detail.flags |= YUKILINKER_C_DETAIL_FAILURE_STAGE;
    detail.failure_stage = image->failure_stage;
  }
  detail.failure_errno = image->failure_errno;
  detail.mapping_flags = mapping_flags(image);
  if (const Context* parent = image->context->namespace_parent) {
    detail.flags |= YUKILINKER_C_DETAIL_NAMESPACE_PARENT;
    detail.namespace_parent_id = parent->namespace_id;
  }
  detail.tls_initial_image = reinterpret_cast<uintptr_t>(image->tls.initial_bytes);
  detail.tls_file_bytes = image->tls.file_bytes;
  detail.tls_memory_bytes = image->tls.memory_bytes;
  detail.tls_alignment = image->tls.alignment;
  // Never allocates: a thread that has not touched the TLS has no block.
  detail.tls_block = reinterpret_cast<uintptr_t>(
      current_thread_tls_data(image->tls.module_id, image->tls.generation));
  uintptr_t relro_start = UINTPTR_MAX;
  uintptr_t relro_end = 0;
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const ElfW(Phdr) & header = image->memory.program_headers[i];
    if (header.p_type != PT_GNU_RELRO || header.p_memsz == 0) continue;
    ++detail.relro_segment_count;
    uintptr_t start;
    uintptr_t end;
    if (!relro_range(image, header, &start, &end)) continue;
    relro_start = std::min(relro_start, start);
    relro_end = std::max(relro_end, end);
  }
  if (relro_end != 0) {
    detail.relro_start = relro_start;
    detail.relro_size = relro_end - relro_start;
  }
  if (image->memory.relro_protected) detail.flags |= YUKILINKER_C_DETAIL_RELRO_PROTECTED;
  detail.relro_written_bytes = image->memory.relro_written_bytes;
  detail.relro_shared_bytes = image->memory.relro_shared_bytes;
  memcpy(output, &detail, kImageDetailSize);
  return YUKILINKER_C_OK;
}

uint32_t query_image_dependencies(yukilinker_c_image_id id, yukilinker_c_dependency_record* records,
                                  size_t capacity, size_t* count) {
  OperationGuard operation;
  if (!count) return YUKILINKER_C_INVALID_HANDLE;
  const ImageState* image = visible_image(id);
  if (!image) return YUKILINKER_C_STALE_TOKEN;
  size_t edges = 0;
  for (auto* dep = image->dependencies; dep; dep = dep->next) ++edges;
  *count = edges;
  if (capacity < edges || (edges && !records)) return YUKILINKER_C_BUFFER_TOO_SMALL;
  size_t index = 0;
  for (auto* dep = image->dependencies; dep; dep = dep->next) {
    yukilinker_c_dependency_record& record = records[index++];
    record = {};
    record.flags = (dep->symbol_provider ? YUKILINKER_C_DEPENDENCY_PROVIDER : 0U) |
                   (dep->linked ? YUKILINKER_C_DEPENDENCY_LINKED : 0U);
    // Collection clears an edge before it frees the target; an edge whose target is not
    // registered is reported as unresolved all the same.
    if (const ImageState* target = dep->image ? registered_image(dep->image) : nullptr) {
      record.kind = YUKILINKER_C_DEPENDENCY_PRIVATE;
      record.target = {target->generation, target->namespace_id};
    } else {
      record.kind =
          dep->system_handle ? YUKILINKER_C_DEPENDENCY_SYSTEM : YUKILINKER_C_DEPENDENCY_UNRESOLVED;
    }
    if (dep->name && static_cast<size_t>(snprintf(record.name, sizeof(record.name), "%s",
                                                  dep->name)) >= sizeof(record.name))
      record.flags |= YUKILINKER_C_DEPENDENCY_NAME_TRUNCATED;
  }
  return YUKILINKER_C_OK;
}

uint32_t query_image_program_headers(yukilinker_c_image_id id, void* output, size_t bytes,
                                     size_t* required) {
  OperationGuard operation;
  if (!required) return YUKILINKER_C_INVALID_HANDLE;
  const ImageState* image = visible_image(id);
  if (!image) return YUKILINKER_C_STALE_TOKEN;
  return copy_program_headers(image, output, bytes, required);
}

uint32_t registry_image_release(uint64_t lease) {
  OperationGuard operation;
  auto* entry = g_lease_index.find(lease);
  if (!entry) return YUKILINKER_C_STALE_TOKEN;
  g_lease_index.erase(lease);
  if (entry->previous)
    entry->previous->next = entry->next;
  else
    g_runtime_leases = entry->next;
  if (entry->next) entry->next->previous = entry->previous;
  auto* image = entry->image;
  --image->lease_refs;
  if (auto* counter = typed_lease_counter(image, entry->kind)) --*counter;
  free_object(entry);
  return collect_images() ? YUKILINKER_C_OK : YUKILINKER_C_IO_ERROR;
}

uint32_t registry_read_events(uint64_t since, yukilinker_c_runtime_event* output, size_t capacity,
                              size_t* written, uint64_t* next) {
  OperationGuard operation;
  if (!written || !next) return YUKILINKER_C_INVALID_HANDLE;
  *written = 0;
  const uint64_t first =
      g_event_sequence > kRuntimeEventCapacity ? g_event_sequence - kRuntimeEventCapacity + 1 : 1;
  *next = g_event_sequence;
  if (since < first - 1) {
    *next = first - 1;
    return YUKILINKER_C_RETRY;
  }
  if (since >= g_event_sequence) return YUKILINKER_C_OK;
  const uint64_t available = g_event_sequence - since;
  if (capacity < available) {
    *written = static_cast<size_t>(available);
    return YUKILINKER_C_BUFFER_TOO_SMALL;
  }
  if (!output) return YUKILINKER_C_INVALID_HANDLE;
  // Counted, so a saturated sequence cannot wrap the cursor.
  for (uint64_t i = 0; i < available; ++i)
    output[(*written)++] = g_runtime_events[(since + i) % kRuntimeEventCapacity];
  return YUKILINKER_C_OK;
}

}  // namespace yukilinker::internal
