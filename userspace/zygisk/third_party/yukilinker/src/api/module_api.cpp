// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../../include/yukilinker.h"
#include "../internal/caller_stubs.hpp"
#include "../internal/cleanup_call.hpp"
#include "../internal/owner_api.hpp"
#include "../internal/registry.hpp"

namespace yukilinker::internal {

namespace {
struct Slot {
  uint64_t generation = 0;
  void* raw = nullptr;
  yukilinker_c_image_id image{};
  uint32_t backend = 0;
  uint32_t state = YUKILINKER_C_HANDLE_OPENING;
  uint32_t reference = YUKILINKER_C_REFERENCE_NONE;
  uint32_t reasons = 0;
  uint32_t pending = 0;
};
struct Pending;
}  // namespace

struct ModuleState {
  Slot* slots = nullptr;
  uint32_t limit = YUKILINKER_C_DEFAULT_HANDLE_LIMIT;
  Pending* pending = nullptr;
};

namespace {
uint64_t g_next_generation = 1;
bool g_tracking = false;

Runtime* owner(void* opaque) {
  return opaque && opaque == g_runtime_owner && g_runtime_owner->active ? g_runtime_owner : nullptr;
}

ModuleState* state(Runtime* runtime) {
  if (!runtime->modules) {
    void* memory = calloc(1, sizeof(ModuleState));
    if (memory) {
      runtime->modules = new (memory) ModuleState;
      __atomic_store_n(&g_tracking, true, __ATOMIC_RELEASE);
    }
  }
  return runtime->modules;
}

bool system_uncertain(const ModuleState* data) {
  if (exit_cleanup_uncertain()) return true;
  if (!data || !data->slots) return false;
  for (size_t i = 0; i < data->limit; ++i) {
    const auto& slot = data->slots[i];
    if (slot.generation && slot.state == YUKILINKER_C_HANDLE_UNCERTAIN) return true;
  }
  return false;
}

void error(yukilinker_c_error* output, uint32_t code, uint32_t stage) {
  if (!output) return;
  *output = {};
  output->size = sizeof(*output);
  output->detail.code = code;
  output->detail.stage = stage;
}

void private_error(yukilinker_c_error* output, const yukilinker_c_error_detail& source) {
  if (!output) return;
  error(output, source.code, source.stage);
  output->detail = source;
  output->execution = source.execution_started ? YUKILINKER_C_EXECUTION_STARTED
                                               : YUKILINKER_C_EXECUTION_NOT_STARTED;
}

// Called while the callback frame still owns the host operation. No intervening
// host call can consume its error. A callback that already consumed it must save it.
void system_error(const yukilinker_c_host_services& services, yukilinker_c_error* output,
                  uint32_t code, uint32_t stage, const char* fallback) {
  error(output, code, stage);
  output->execution = YUKILINKER_C_EXECUTION_UNKNOWN;
  const char* text = services.system_dlerror(services.user);
  (void)snprintf(output->system_message, sizeof(output->system_message), "%s",
                 text ? text : fallback);
}

struct Pending {
  ModuleState* runtime;
  Slot* slot;
  pthread_t thread = pthread_self();
  Pending* next;
  bool linked = true;
  bool host_entered = false;
  Pending(ModuleState* runtime_, Slot* slot_)
      : runtime(runtime_), slot(slot_), next(runtime_->pending) {
    runtime->pending = this;
    ++slot->pending;
  }
  void finish() {
    if (!linked) return;
    Pending** link = &runtime->pending;
    while (*link && *link != this) link = &(*link)->next;
    if (*link) *link = next;
    --slot->pending;
    linked = false;
  }
  ~Pending() { finish(); }
  Pending(const Pending&) = delete;
  Pending& operator=(const Pending&) = delete;
  Pending(Pending&&) = delete;
  Pending& operator=(Pending&&) = delete;
};

// The slot word identifies the owner's table. The generation packs an entry
// index and a monotonically increasing serial. Owner storage remains a tombstone,
// so a handle from another Core (even a retired one) can never match this table.
// Validate both full-width words before indexing; neither word is a public index.
Slot* find(Runtime* runtime, yukilinker_c_handle handle) {
  auto* data = runtime->modules;
  const uint64_t index = handle.generation & UINT64_C(0xffff);
  if (!data || !data->slots || handle.slot != reinterpret_cast<uintptr_t>(runtime) ||
      !handle.generation || index >= data->limit)
    return nullptr;
  auto* slot = &data->slots[index];
  return slot->generation == handle.generation ? slot : nullptr;
}

yukilinker_c_handle identity(const Slot* slot) {
  return {reinterpret_cast<uintptr_t>(g_runtime_owner), slot->generation};
}

ImageState* private_image(const Slot& slot) {
  auto* image = image_by_generation(slot.image.generation);
  return image && image->namespace_id == slot.image.namespace_id ? image : nullptr;
}

__attribute__((always_inline)) inline void info(const Slot& slot,
                                                yukilinker_c_handle_info* output) {
  *output = {};
  output->size = sizeof(*output);
  output->handle = identity(&slot);
  output->backend = slot.backend;
  output->state = slot.state;
  output->reference = slot.reference;
  output->retention_reasons = slot.reasons;
  output->pending_operations = slot.pending;
  if (slot.backend == YUKILINKER_C_BACKEND_SYSTEM) {
    // These are Bionic integration capabilities, conditional on a system-loaded
    // runtime/host/module closure and the platform's own support.
    output->capabilities =
        YUKILINKER_C_SYSTEM_TLS | YUKILINKER_C_SHARED_CXX | YUKILINKER_C_CROSS_DSO_CFI;
    return;
  }
  output->capabilities = YUKILINKER_C_PRIVATE_NAMESPACE | YUKILINKER_C_PRIVATE_TLS;
  output->private_image = slot.image;
  if (auto* image = private_image(slot)) {
    output->private_nodelete_reasons = image->nodelete_reasons;
    output->private_mapping_unmapped = image->mapping_released;
  } else if (slot.image.generation)
    output->private_mapping_unmapped = 1;
}

void* context_create(void* opaque, const char* path, yukilinker_c_dependency_provider provider,
                     void* user) {
  OperationGuard operation;
  if (!owner(opaque)) return nullptr;
  void* token = loader_create_context(path, provider, user);
  if (token) {
    auto* context = context_from_opaque(token);
    context->strict_private = true;
    context->caller_stubs = true;
  }
  return token;
}

void* namespace_create(void* opaque, void* token) {
  OperationGuard operation;
  if (!owner(opaque)) return nullptr;
  void* result = registry_namespace_create(token);
  if (result) {
    find_namespace(result)->context->strict_private = true;
    find_namespace(result)->context->caller_stubs = true;
  }
  return result;
}

void* runtime_create(const yukilinker_c_host_services* services) {
  void* result = owner_runtime_create(services);
  if (result) {
    OperationGuard operation;
    g_default_context.strict_private = true;
    g_default_context.caller_stubs = true;
  }
  return result;
}

// A module wrapper or an in-flight operation keeps the owner alive. Explicit
// contexts and namespace resources are checked by owner retirement.
bool can_retire_modules() { return !modules_has_holders(); }

uint32_t runtime_destroy(void* opaque) {
  {
    OperationGuard operation;
    if (opaque != g_runtime_owner || !opaque) return 0;
    if (!can_retire_modules()) return 0;
  }
  return owner_runtime_destroy(opaque);
}

uint32_t try_shutdown(void* opaque, yukilinker_c_error_detail* output) {
  {
    OperationGuard operation;
    if (opaque == g_runtime_owner && opaque && !can_retire_modules()) {
      if (output) {
        *output = {};
        output->code = YUKILINKER_C_BUSY;
        output->stage = YUKILINKER_C_RETIREMENT;
      }
      return 0;
    }
  }
  return owner_try_shutdown(opaque, output);
}

uint32_t set_limits(void* opaque, uint32_t handles, uint64_t bytes) {
  OperationGuard operation;
  auto* runtime = owner(opaque);
  if (!runtime) return YUKILINKER_C_STALE_TOKEN;
  if (!handles || handles > 65536 || bytes > SIZE_MAX) return YUKILINKER_C_UNSUPPORTED;
  auto* data = state(runtime);
  if (!data) return YUKILINKER_C_NO_MEMORY;
  if (data->slots && handles != data->limit) return YUKILINKER_C_BUSY;
  if (!caller_stub_set_budget(static_cast<size_t>(bytes))) return YUKILINKER_C_BUSY;
  data->limit = handles;
  return YUKILINKER_C_OK;
}

uint32_t query_stats(void* opaque, yukilinker_c_runtime_stats* output) {
  OperationGuard operation;
  if (!opaque || opaque != g_runtime_owner) return YUKILINKER_C_STALE_TOKEN;
  if (!output) return YUKILINKER_C_INVALID_HANDLE;
  *output = {};
  output->size = sizeof(*output);
  output->handle_limit = YUKILINKER_C_DEFAULT_HANDLE_LIMIT;
  if (auto* data = g_runtime_owner->modules) {
    output->handle_limit = data->limit;
    output->handle_bytes = sizeof(ModuleState) + (data->slots ? data->limit * sizeof(Slot) : 0);
    if (data->slots)
      for (size_t i = 0; i < data->limit; ++i) {
        const auto& slot = data->slots[i];
        output->handles += slot.generation != 0;
        output->uncertain_handles += slot.generation && slot.state == YUKILINKER_C_HANDLE_UNCERTAIN;
        output->pending_operations += slot.pending;
      }
  }
  size_t pages = 0, bytes = 0, used = 0, capacity = 0;
  caller_stub_stats(&pages, &bytes);
  caller_stub_slots(&used, &capacity);
  output->stub_budget_bytes = caller_stub_budget();
  output->stub_mapped_bytes = bytes;
  output->stub_used_slots = used;
  output->stub_capacity = capacity;
  const auto loader = loader_query_stats();
  output->images = loader.images;
  output->mapped_bytes = loader.mapped_bytes;
  output->arena_bytes = loader.arena_bytes;
  output->peak_arena_bytes = loader.peak_arena_bytes;
  output->tls_blocks = loader.tls_blocks;
  output->thread_destructors = loader.thread_destructors;
  return YUKILINKER_C_OK;
}

bool valid_options(const yukilinker_c_open_options* options) {
  if (!options || options->size != sizeof(*options) ||
      options->namespace_scope && options->context || options->file_backed > 1 ||
      options->force_load > 1 || options->trusted_runtime > 1)
    return false;
  const uint32_t flags = options->flags;
  if (flags & ~(RTLD_NOW | RTLD_LAZY | RTLD_GLOBAL | RTLD_NODELETE | RTLD_NOLOAD)) return false;
#if defined(__LP64__)
  if ((flags & (RTLD_NOW | RTLD_LAZY)) == 0 ||
      (flags & (RTLD_NOW | RTLD_LAZY)) == (RTLD_NOW | RTLD_LAZY))
    return false;
#endif
  if (options->backend == YUKILINKER_C_BACKEND_PRIVATE) return true;
  return options->backend == YUKILINKER_C_BACKEND_SYSTEM && !options->context &&
         !options->file_backed && !options->force_load && !options->trusted_runtime &&
         !options->namespace_scope;
}

yukilinker_c_handle open(void* opaque, const char* path, int fd, bool from_fd,
                         const yukilinker_c_open_options* options, yukilinker_c_error* output) {
  OperationGuard operation;
  error(output, YUKILINKER_C_OK, YUKILINKER_C_SOURCE);
  auto* runtime = owner(opaque);
  if (!runtime) {
    error(output, YUKILINKER_C_STALE_TOKEN, YUKILINKER_C_SOURCE);
    return {};
  }
  // A callback may have ended while holding Bionic's loader lock. Nothing here
  // can repair that lock or determine ownership. Do not enter more module/host
  // code (PRIVATE dependencies can call Bionic too); queries remain available.
  if (system_uncertain(runtime->modules)) {
    error(output, YUKILINKER_C_BUSY, YUKILINKER_C_SOURCE);
    return {};
  }
  if (!valid_options(options) || (from_fd ? fd < 0 : !path)) {
    error(output, YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_SOURCE);
    return {};
  }
  auto* data = state(runtime);
  if (!data) {
    error(output, YUKILINKER_C_NO_MEMORY, YUKILINKER_C_SOURCE);
    return {};
  }
  void* context =
      options->namespace_scope ? static_cast<void*>(options->namespace_scope) : options->context;
  if (options->backend == YUKILINKER_C_BACKEND_PRIVATE) {
    if (!context) {
      context = &g_default_context;
    }
    auto* scope = context ? context_from_opaque(context) : nullptr;
    if (!scope || !scope->strict_private) {
      error(output, context ? YUKILINKER_C_UNSUPPORTED : YUKILINKER_C_NO_MEMORY,
            YUKILINKER_C_SOURCE);
      return {};
    }
  }
  if (!data->slots) data->slots = static_cast<Slot*>(calloc(data->limit, sizeof(Slot)));
  Slot* slot = nullptr;
  if (data->slots && g_next_generation < (UINT64_MAX >> 16))
    for (size_t i = 0; i < data->limit; ++i)
      if (!data->slots[i].generation) {
        slot = &data->slots[i];
        break;
      }
  if (!slot) {
    error(output, YUKILINKER_C_NO_MEMORY, YUKILINKER_C_SOURCE);
    return {};
  }
  *slot = {};
  slot->generation = (g_next_generation++ << 16) | static_cast<uint64_t>(slot - data->slots);
  slot->backend = options->backend;
  Pending pending(data, slot);
  yukilinker_c_error result{};
  error(&result, YUKILINKER_C_OK, YUKILINKER_C_SOURCE);
  void* raw = nullptr;
  if (options->backend == YUKILINKER_C_BACKEND_PRIVATE) {
    LoadOptions request{sizeof(request),          options->flags,  options->file_backed,
                        options->trusted_runtime, options->origin, context,
                        options->force_load};
    yukilinker_c_error_detail detail{};
    raw =
        from_fd ? loader_open_fd(fd, &request, &detail) : loader_open_path(path, &request, &detail);
    private_error(&result, detail);
    if (raw) {
      auto* image = state_of(static_cast<SoHandle*>(raw));
      slot->image = {image->generation, image->namespace_id};
    }
  } else {
    const yukilinker_c_host_services services = host_services();
    android_dlextinfo ext{};
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    ext.library_fd = fd;
    yukilinker_c_host_dlopen_request request{};
    request.size = sizeof(request);
    request.flags = static_cast<int>(options->flags);
    request.name = from_fd ? (options->origin ? options->origin : "[yukilinker-system-fd]") : path;
    request.extension = from_fd ? &ext : nullptr;
    CallbackGuard callback(nullptr);
    (void)services.system_dlerror(services.user);
    result.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
    raw = services.system_dlopen(services.user, &request);
    if (!raw)
      system_error(services, &result, YUKILINKER_C_IO_ERROR, YUKILINKER_C_INITIALIZATION,
                   "host system_dlopen failed without an error string");
  }
  pending.finish();
  if (pending.host_entered) result.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
  if (output) *output = result;
  if (!raw) {
    *slot = {};
    return {};
  }
  slot->raw = raw;
  slot->reference = YUKILINKER_C_REFERENCE_OWNED;
  slot->state = YUKILINKER_C_HANDLE_READY;
  return identity(slot);
}

yukilinker_c_handle open_path(void* runtime, const char* path,
                              const yukilinker_c_open_options* options,
                              yukilinker_c_error* output) {
  return open(runtime, path, -1, false, options, output);
}
yukilinker_c_handle open_fd(void* runtime, int fd, const yukilinker_c_open_options* options,
                            yukilinker_c_error* output) {
  return open(runtime, nullptr, fd, true, options, output);
}

yukilinker_c_lookup_result lookup(void* opaque, yukilinker_c_handle handle, const char* name,
                                  const char* version) {
  OperationGuard operation;
  yukilinker_c_lookup_result result{};
  auto* runtime = owner(opaque);
  auto* slot = runtime ? find(runtime, handle) : nullptr;
  error(&result.error, runtime ? YUKILINKER_C_INVALID_HANDLE : YUKILINKER_C_STALE_TOKEN,
        YUKILINKER_C_METADATA);
  if (!slot || !name) return result;
  if (slot->state != YUKILINKER_C_HANDLE_READY || system_uncertain(runtime->modules)) {
    error(&result.error, YUKILINKER_C_BUSY, YUKILINKER_C_METADATA);
    return result;
  }
  Pending pending(runtime->modules, slot);
  if (slot->backend == YUKILINKER_C_BACKEND_PRIVATE) {
    const auto value = loader_lookup(slot->raw, name, version);
    result.address = value.address;
    result.found = value.found;
    private_error(&result.error, value.error);
  } else {
    const auto services = host_services();
    CallbackGuard callback(nullptr);
    (void)services.system_dlerror(services.user);
    error(&result.error, YUKILINKER_C_OK, YUKILINKER_C_METADATA);
    result.error.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
    result.found =
        services.system_dlsym(services.user, slot->raw, name, version, &result.address) != 0;
    if (!result.found)
      system_error(services, &result.error, YUKILINKER_C_SYMBOL_MISSING, YUKILINKER_C_METADATA,
                   "host system_dlsym failed without an error string");
  }
  if (pending.host_entered) result.error.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
  if (slot->state == YUKILINKER_C_HANDLE_UNCERTAIN) {
    result.address = nullptr;
    result.found = 0;
    error(&result.error, YUKILINKER_C_BUSY, YUKILINKER_C_METADATA);
    result.error.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
  }
  return result;
}

uint32_t query(void* opaque, yukilinker_c_handle handle, yukilinker_c_handle_info* output) {
  OperationGuard operation;
  auto* runtime = owner(opaque);
  if (!runtime) return YUKILINKER_C_STALE_TOKEN;
  auto* slot = find(runtime, handle);
  if (!slot || !output) return YUKILINKER_C_INVALID_HANDLE;
  info(*slot, output);
  return YUKILINKER_C_OK;
}

uint32_t handles(void* opaque, yukilinker_c_handle_info* output, size_t capacity, size_t* count) {
  OperationGuard operation;
  auto* runtime = owner(opaque);
  if (!runtime) return YUKILINKER_C_STALE_TOKEN;
  if (!count || (capacity && !output)) return YUKILINKER_C_INVALID_HANDLE;
  *count = 0;
  if (auto* data = runtime->modules; data && data->slots)
    for (size_t i = 0; i < data->limit; ++i) {
      auto& slot = data->slots[i];
      if (!slot.generation) continue;
      if (*count < capacity) info(slot, &output[*count]);
      ++*count;
    }
  return *count <= capacity ? YUKILINKER_C_OK : YUKILINKER_C_BUFFER_TOO_SMALL;
}

yukilinker_c_close_result close(void* opaque, yukilinker_c_handle handle) {
  OperationGuard operation;
  yukilinker_c_close_result result{};
  auto* runtime = owner(opaque);
  auto* slot = runtime ? find(runtime, handle) : nullptr;
  error(&result.error, runtime ? YUKILINKER_C_INVALID_HANDLE : YUKILINKER_C_STALE_TOKEN,
        YUKILINKER_C_RETIREMENT);
  if (!slot) return result;
  result.handle_valid = 1;
  info(*slot, &result.info);
  if (slot->pending || slot->state == YUKILINKER_C_HANDLE_UNCERTAIN ||
      system_uncertain(runtime->modules)) {
    error(&result.error, YUKILINKER_C_BUSY, YUKILINKER_C_RETIREMENT);
    if (slot->state == YUKILINKER_C_HANDLE_UNCERTAIN || exit_cleanup_uncertain()) {
      result.error.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
      result.error.detail.cleanup_incomplete = 1;
    }
    return result;
  }
  Pending pending(runtime->modules, slot);
  slot->state = YUKILINKER_C_HANDLE_CLOSING;
  error(&result.error, YUKILINKER_C_OK, YUKILINKER_C_RETIREMENT);
  if (slot->backend == YUKILINKER_C_BACKEND_SYSTEM) {
    const auto services = host_services();
    int status = -1;
    CleanupCall call;
    yukilinker_c_error_detail detail{};
    bool returned;
    {
      returned = cleanup_call(
          nullptr, call,
          [&] {
            (void)services.system_dlerror(services.user);
            result.error.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
            status = services.system_dlclose(services.user, slot->raw);
            if (status != 0)
              system_error(services, &result.error, YUKILINKER_C_IO_ERROR, YUKILINKER_C_RETIREMENT,
                           "host system_dlclose failed without an error string");
          },
          &detail);
    }
    if (!returned) {
      private_error(&result.error, detail);
      slot->state = YUKILINKER_C_HANDLE_READY;
      slot->reasons |= YUKILINKER_C_RETENTION_CLEANUP;
    } else {
      slot->reference =
          status == 0 ? YUKILINKER_C_REFERENCE_RELEASED : YUKILINKER_C_REFERENCE_UNKNOWN;
      slot->state = status == 0 ? YUKILINKER_C_HANDLE_RETAINED : YUKILINKER_C_HANDLE_UNCERTAIN;
      if (status != 0)
        slot->reasons |= YUKILINKER_C_RETENTION_CLEANUP | YUKILINKER_C_RETENTION_SYSTEM_UNKNOWN;
    }
  } else {
    if (slot->reference == YUKILINKER_C_REFERENCE_OWNED && private_image(*slot)) {
      yukilinker_c_error_detail detail{};
      (void)api_close_locked(slot->raw, &detail);
      private_error(&result.error, detail);
    } else if (private_image(*slot) && !collect_images()) {
      error(&result.error, YUKILINKER_C_IO_ERROR, YUKILINKER_C_RETIREMENT);
      result.error.detail.cleanup_incomplete = 1;
    }
    slot->reference = YUKILINKER_C_REFERENCE_RELEASED;
    slot->state = YUKILINKER_C_HANDLE_RETAINED;
    slot->reasons = private_image(*slot) ? YUKILINKER_C_RETENTION_PRIVATE_HOLD : 0;
    if (result.error.detail.cleanup_incomplete) slot->reasons |= YUKILINKER_C_RETENTION_CLEANUP;
  }
  pending.finish();
  if (pending.host_entered) result.error.execution = YUKILINKER_C_EXECUTION_UNKNOWN;
  info(*slot, &result.info);
  if ((slot->backend == YUKILINKER_C_BACKEND_SYSTEM &&
       slot->reference == YUKILINKER_C_REFERENCE_RELEASED) ||
      (slot->backend == YUKILINKER_C_BACKEND_PRIVATE && !private_image(*slot))) {
    result.handle_valid = 0;
    *slot = {};
  }
  return result;
}

uint32_t forget(void* opaque, yukilinker_c_handle handle) {
  OperationGuard operation;
  auto* runtime = owner(opaque);
  if (!runtime) return YUKILINKER_C_STALE_TOKEN;
  auto* slot = find(runtime, handle);
  if (!slot) return YUKILINKER_C_INVALID_HANDLE;
  if (slot->backend != YUKILINKER_C_BACKEND_PRIVATE ||
      slot->reference != YUKILINKER_C_REFERENCE_RELEASED || slot->pending ||
      slot->state == YUKILINKER_C_HANDLE_UNCERTAIN)
    return YUKILINKER_C_BUSY;
  *slot = {};
  return YUKILINKER_C_OK;
}

void abandon(Pending* pending, uint32_t reason) {
  pending->slot->state = YUKILINKER_C_HANDLE_UNCERTAIN;
  pending->slot->reference = YUKILINKER_C_REFERENCE_UNKNOWN;
  pending->slot->reasons |= reason;
  if (pending->slot->backend == YUKILINKER_C_BACKEND_SYSTEM || pending->host_entered)
    pending->slot->reasons |= YUKILINKER_C_RETENTION_SYSTEM_UNKNOWN;
  pending->finish();
}
}  // namespace

bool modules_has_holders() {
  if (!g_runtime_owner || !g_runtime_owner->modules) return false;
  auto* data = g_runtime_owner->modules;
  if (data->pending) return true;
  if (data->slots)
    for (size_t i = 0; i < data->limit; ++i)
      if (data->slots[i].generation) return true;
  return false;
}

void modules_abandon_thread(pthread_t thread) {
  if (!g_runtime_owner || !g_runtime_owner->modules) return;
  for (auto* pending = g_runtime_owner->modules->pending; pending;) {
    auto* next = pending->next;
    if (pthread_equal(thread, pending->thread)) abandon(pending, YUKILINKER_C_RETENTION_ABANDONED);
    pending = next;
  }
}

void modules_after_fork() {
  if (!g_runtime_owner || !g_runtime_owner->modules) return;
  for (auto* pending = g_runtime_owner->modules->pending; pending;) {
    auto* next = pending->next;
    if (!pthread_equal(pthread_self(), pending->thread))
      abandon(pending, YUKILINKER_C_RETENTION_FORK);
    pending = next;
  }
}

void modules_free_state() {
  if (!g_runtime_owner || !g_runtime_owner->modules) return;
  auto* data = g_runtime_owner->modules;
  free(data->slots);
  data->~ModuleState();
  free(data);
  g_runtime_owner->modules = nullptr;
  __atomic_store_n(&g_tracking, false, __ATOMIC_RELEASE);
}

void modules_note_system_call_locked() {
  if (!__atomic_load_n(&g_tracking, __ATOMIC_ACQUIRE)) return;
  if (!g_runtime_owner || !g_runtime_owner->modules) return;
  for (auto* pending = g_runtime_owner->modules->pending; pending; pending = pending->next)
    if (pthread_equal(pending->thread, pthread_self())) pending->host_entered = true;
}

void modules_note_system_call() {
  if (!__atomic_load_n(&g_tracking, __ATOMIC_ACQUIRE)) return;
  OperationGuard operation;
  modules_note_system_call_locked();
}

void* module_runtime_create(const yukilinker_c_host_services* services) {
  return runtime_create(services);
}
uint32_t module_runtime_destroy(void* opaque) { return runtime_destroy(opaque); }
uint32_t module_try_shutdown(void* opaque, yukilinker_c_error_detail* output) {
  return try_shutdown(opaque, output);
}
uint32_t module_set_limits(void* opaque, uint32_t handles, uint64_t bytes) {
  return set_limits(opaque, handles, bytes);
}
uint32_t module_query_stats(void* opaque, yukilinker_c_runtime_stats* output) {
  return query_stats(opaque, output);
}
void* module_context_create(void* opaque, const char* path,
                            yukilinker_c_dependency_provider provider, void* user) {
  return context_create(opaque, path, provider, user);
}
void* module_namespace_create(void* opaque, void* token) { return namespace_create(opaque, token); }
yukilinker_c_handle module_open_path(void* runtime, const char* path,
                                     const yukilinker_c_open_options* options,
                                     yukilinker_c_error* output) {
  return open_path(runtime, path, options, output);
}
yukilinker_c_handle module_open_fd(void* runtime, int fd, const yukilinker_c_open_options* options,
                                   yukilinker_c_error* output) {
  return open_fd(runtime, fd, options, output);
}
yukilinker_c_lookup_result module_lookup(void* opaque, yukilinker_c_handle handle, const char* name,
                                         const char* version) {
  return lookup(opaque, handle, name, version);
}
uint32_t module_query(void* opaque, yukilinker_c_handle handle, yukilinker_c_handle_info* output) {
  return query(opaque, handle, output);
}
uint32_t module_handles(void* opaque, yukilinker_c_handle_info* output, size_t capacity,
                        size_t* count) {
  return handles(opaque, output, capacity, count);
}
yukilinker_c_close_result module_close(void* opaque, yukilinker_c_handle handle) {
  return close(opaque, handle);
}
uint32_t module_forget(void* opaque, yukilinker_c_handle handle) { return forget(opaque, handle); }
uint32_t module_release_host(void* opaque, yukilinker_c_handle handle) {
  OperationGuard operation;
  auto* runtime = owner(opaque);
  if (!runtime) return YUKILINKER_C_STALE_TOKEN;
  auto* slot = find(runtime, handle);
  if (!slot) return YUKILINKER_C_INVALID_HANDLE;
  if (slot->backend != YUKILINKER_C_BACKEND_PRIVATE) return YUKILINKER_C_UNSUPPORTED;
  if (slot->pending || slot->reference != YUKILINKER_C_REFERENCE_OWNED ||
      system_uncertain(runtime->modules))
    return YUKILINKER_C_BUSY;
  // Dependency close callbacks may reenter this handle or end their thread. Keep
  // the same pending-operation guard used by open/lookup/close until handoff ends.
  Pending pending(runtime->modules, slot);
  const bool released = loader_release_host(slot->raw) != 0;
  pending.finish();
  if (!released) return YUKILINKER_C_BUSY;
  *slot = {};
  return YUKILINKER_C_OK;
}

uint32_t module_image(void* opaque, yukilinker_c_handle handle, yukilinker_c_image_record* output) {
  OperationGuard operation;
  auto* runtime = owner(opaque);
  if (!runtime) return YUKILINKER_C_STALE_TOKEN;
  auto* slot = find(runtime, handle);
  if (!slot) return YUKILINKER_C_INVALID_HANDLE;
  if (slot->backend != YUKILINKER_C_BACKEND_PRIVATE) return YUKILINKER_C_UNSUPPORTED;
  return registry_image_query(slot->image, output);
}

}  // namespace yukilinker::internal
