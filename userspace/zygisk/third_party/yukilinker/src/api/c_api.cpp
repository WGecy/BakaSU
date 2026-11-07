// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/enumeration.hpp"
#include "../internal/module_api.hpp"
#include "../internal/owner_api.hpp"

using namespace yukilinker::internal;

extern "C" uint32_t yukilinker_c_abi_version(void) { return YUKILINKER_C_ABI_VERSION; }

extern "C" uint32_t yukilinker_c_has_feature(uint32_t feature) {
  return feature >= YUKILINKER_C_FEATURE_PACKED_RELOCS &&
         feature <= YUKILINKER_C_FEATURE_CONTROLLED_FORK;
}

extern "C" yukilinker_c_runtime* yukilinker_c_runtime_create(
    const yukilinker_c_host_services* services) {
  return reinterpret_cast<yukilinker_c_runtime*>(module_runtime_create(services));
}

extern "C" uint32_t yukilinker_c_runtime_destroy(yukilinker_c_runtime* runtime) {
  return module_runtime_destroy(runtime);
}

extern "C" uint32_t yukilinker_c_try_shutdown(yukilinker_c_runtime* runtime,
                                              yukilinker_c_error_detail* error) {
  return module_try_shutdown(runtime, error);
}

extern "C" uint32_t yukilinker_c_host_services_bind(yukilinker_c_runtime* runtime,
                                                    const yukilinker_c_host_services* services) {
  return owner_host_services_bind(runtime, services);
}

extern "C" uint32_t yukilinker_c_set_limits(yukilinker_c_runtime* runtime, uint32_t handle_limit,
                                            uint64_t stub_bytes) {
  return module_set_limits(runtime, handle_limit, stub_bytes);
}

extern "C" uint32_t yukilinker_c_query_stats(yukilinker_c_runtime* runtime,
                                             yukilinker_c_runtime_stats* output) {
  return module_query_stats(runtime, output);
}

extern "C" yukilinker_c_context* yukilinker_c_context_create(
    yukilinker_c_runtime* runtime, const char* search_path,
    yukilinker_c_dependency_provider provider, void* user) {
  return reinterpret_cast<yukilinker_c_context*>(
      module_context_create(runtime, search_path, provider, user));
}

extern "C" uint32_t yukilinker_c_context_destroy(yukilinker_c_runtime* runtime,
                                                 yukilinker_c_context* context) {
  return owner_context_destroy(runtime, context);
}

extern "C" yukilinker_c_namespace* yukilinker_c_namespace_create(yukilinker_c_runtime* runtime,
                                                                 yukilinker_c_context* parent) {
  return reinterpret_cast<yukilinker_c_namespace*>(module_namespace_create(runtime, parent));
}

extern "C" uint32_t yukilinker_c_namespace_destroy(yukilinker_c_runtime* runtime,
                                                   yukilinker_c_namespace* scope) {
  return owner_namespace_destroy(runtime, scope);
}

extern "C" uint64_t yukilinker_c_namespace_id(yukilinker_c_runtime* runtime,
                                              yukilinker_c_namespace* scope) {
  return owner_namespace_id(runtime, scope);
}

extern "C" uint32_t yukilinker_c_namespace_link(yukilinker_c_runtime* runtime,
                                                yukilinker_c_namespace* from,
                                                yukilinker_c_namespace* to, uint32_t flags,
                                                const char* shared_sonames) {
  return owner_namespace_link(runtime, from, to, flags, shared_sonames);
}

extern "C" uint32_t yukilinker_c_namespace_links(yukilinker_c_runtime* runtime,
                                                 yukilinker_c_namespace* from,
                                                 yukilinker_c_namespace_link_record* records,
                                                 size_t capacity, size_t* count) {
  return owner_namespace_links(runtime, from, records, capacity, count);
}

extern "C" uint32_t yukilinker_c_namespace_host_import(yukilinker_c_runtime* runtime,
                                                       yukilinker_c_namespace* scope,
                                                       const yukilinker_c_host_import* import) {
  return owner_namespace_host_import(runtime, scope, import);
}

extern "C" uint32_t yukilinker_c_namespace_host_imports(yukilinker_c_runtime* runtime,
                                                        yukilinker_c_namespace* scope,
                                                        yukilinker_c_host_import_record* records,
                                                        size_t capacity, size_t* count) {
  return owner_namespace_host_imports(runtime, scope, records, capacity, count);
}

extern "C" uint32_t yukilinker_c_query_host_import_refusal(
    yukilinker_c_runtime* runtime, yukilinker_c_host_import_refusal* refusal) {
  return owner_host_import_refusal(runtime, refusal);
}

extern "C" uint32_t yukilinker_c_wait_timeout(yukilinker_c_runtime* runtime, uint64_t nanoseconds) {
  return owner_wait_timeout(runtime, nanoseconds);
}

extern "C" uint32_t yukilinker_c_load_waits(yukilinker_c_runtime* runtime,
                                            yukilinker_c_load_wait* records, size_t capacity,
                                            size_t* count) {
  return owner_load_waits(runtime, records, capacity, count);
}

extern "C" yukilinker_c_handle yukilinker_c_open_path(yukilinker_c_runtime* runtime,
                                                      const char* path,
                                                      const yukilinker_c_open_options* options,
                                                      yukilinker_c_error* error) {
  return module_open_path(runtime, path, options, error);
}

extern "C" yukilinker_c_handle yukilinker_c_open_fd(yukilinker_c_runtime* runtime, int fd,
                                                    const yukilinker_c_open_options* options,
                                                    yukilinker_c_error* error) {
  return module_open_fd(runtime, fd, options, error);
}

extern "C" yukilinker_c_lookup_result yukilinker_c_lookup(yukilinker_c_runtime* runtime,
                                                          yukilinker_c_handle handle,
                                                          const char* name, const char* version) {
  return module_lookup(runtime, handle, name, version);
}

extern "C" uint32_t yukilinker_c_query(yukilinker_c_runtime* runtime, yukilinker_c_handle handle,
                                       yukilinker_c_handle_info* output) {
  return module_query(runtime, handle, output);
}

extern "C" uint32_t yukilinker_c_handles(yukilinker_c_runtime* runtime,
                                         yukilinker_c_handle_info* output, size_t capacity,
                                         size_t* count) {
  return module_handles(runtime, output, capacity, count);
}

extern "C" yukilinker_c_close_result yukilinker_c_close(yukilinker_c_runtime* runtime,
                                                        yukilinker_c_handle handle) {
  return module_close(runtime, handle);
}

extern "C" uint32_t yukilinker_c_forget(yukilinker_c_runtime* runtime, yukilinker_c_handle handle) {
  return module_forget(runtime, handle);
}

extern "C" uint32_t yukilinker_c_release_host(yukilinker_c_runtime* runtime,
                                              yukilinker_c_handle handle) {
  return module_release_host(runtime, handle);
}

extern "C" uint32_t yukilinker_c_handle_image(yukilinker_c_runtime* runtime,
                                              yukilinker_c_handle handle,
                                              yukilinker_c_image_record* output) {
  return module_image(runtime, handle, output);
}

extern "C" uint32_t yukilinker_c_snapshot_acquire(yukilinker_c_runtime* runtime,
                                                  yukilinker_c_context* context,
                                                  yukilinker_c_namespace* scope,
                                                  yukilinker_c_snapshot* token, size_t* count) {
  if (context && scope) return YUKILINKER_C_INVALID_HANDLE;
  return owner_snapshot_acquire(runtime, scope ? static_cast<void*>(scope) : context,
                                token ? &token->value : nullptr, count);
}

extern "C" uint32_t yukilinker_c_snapshot_image(yukilinker_c_runtime* runtime,
                                                yukilinker_c_snapshot token, size_t index,
                                                yukilinker_c_image_record* output) {
  return owner_snapshot_image(runtime, token.value, index, output);
}

extern "C" uint32_t yukilinker_c_snapshot_program_headers(yukilinker_c_runtime* runtime,
                                                          yukilinker_c_snapshot token, size_t index,
                                                          void* headers, size_t capacity,
                                                          size_t* required) {
  return owner_snapshot_headers(runtime, token.value, index, headers, capacity, required);
}

extern "C" uint32_t yukilinker_c_snapshot_mapping(yukilinker_c_runtime* runtime,
                                                  yukilinker_c_snapshot token, size_t index,
                                                  yukilinker_c_mapping_record* output) {
  return owner_snapshot_mapping(runtime, token.value, index, output);
}

extern "C" uint32_t yukilinker_c_snapshot_release(yukilinker_c_runtime* runtime,
                                                  yukilinker_c_snapshot* token) {
  if (!token) return YUKILINKER_C_INVALID_HANDLE;
  const auto code = owner_snapshot_release(runtime, token->value);
  if (code == YUKILINKER_C_OK || code == YUKILINKER_C_IO_ERROR) token->value = 0;
  return code;
}

extern "C" uint32_t yukilinker_c_image_acquire(yukilinker_c_runtime* runtime,
                                               yukilinker_c_image_id id, uint32_t kind,
                                               yukilinker_c_lease* lease) {
  return owner_image_acquire(runtime, id, kind, lease ? &lease->value : nullptr);
}

extern "C" uint32_t yukilinker_c_image_release(yukilinker_c_runtime* runtime,
                                               yukilinker_c_lease* lease) {
  if (!lease) return YUKILINKER_C_INVALID_HANDLE;
  const auto code = owner_image_release(runtime, lease->value);
  if (code == YUKILINKER_C_OK || code == YUKILINKER_C_IO_ERROR) lease->value = 0;
  return code;
}

extern "C" uint32_t yukilinker_c_image_query(yukilinker_c_runtime* runtime,
                                             yukilinker_c_image_id id,
                                             yukilinker_c_image_record* record) {
  return owner_image_query(runtime, id, record);
}

extern "C" uint32_t yukilinker_c_query_image_detail(yukilinker_c_runtime* runtime,
                                                    yukilinker_c_image_id id,
                                                    yukilinker_c_image_detail* detail) {
  return owner_image_detail(runtime, id, detail);
}

extern "C" uint32_t yukilinker_c_image_dependencies(yukilinker_c_runtime* runtime,
                                                    yukilinker_c_image_id id,
                                                    yukilinker_c_dependency_record* records,
                                                    size_t capacity, size_t* count) {
  return owner_image_dependencies(runtime, id, records, capacity, count);
}

extern "C" uint32_t yukilinker_c_image_program_headers(yukilinker_c_runtime* runtime,
                                                       yukilinker_c_image_id id, void* headers,
                                                       size_t capacity, size_t* required) {
  return owner_image_program_headers(runtime, id, headers, capacity, required);
}

extern "C" uint32_t yukilinker_c_read_events(yukilinker_c_runtime* runtime, uint64_t cursor,
                                             yukilinker_c_runtime_event* events, size_t capacity,
                                             size_t* written, uint64_t* next_cursor) {
  return owner_read_events(runtime, cursor, events, capacity, written, next_cursor);
}

extern "C" uint32_t yukilinker_c_query_address(yukilinker_c_runtime* runtime, const void* address,
                                               yukilinker_c_image_info* output) {
  OperationGuard operation;
  if (runtime != reinterpret_cast<yukilinker_c_runtime*>(g_runtime_owner) || !g_runtime_owner ||
      !g_runtime_owner->active)
    return 0;
  return api_query_address(address, output);
}

extern "C" int yukilinker_c_iterate_images(yukilinker_c_runtime* runtime,
                                           yukilinker_c_context* context,
                                           yukilinker_c_namespace* scope,
                                           yukilinker_c_image_visitor visitor, void* user) {
  OperationGuard operation;
  if (runtime != reinterpret_cast<yukilinker_c_runtime*>(g_runtime_owner) || !g_runtime_owner ||
      !g_runtime_owner->active || (context && scope) || !visitor)
    return -1;
  return api_iterate_images(scope ? static_cast<void*>(scope) : context, visitor, user);
}

extern "C" void yukilinker_c_prepare_fork(yukilinker_c_runtime* runtime) {
  if (runtime && runtime == reinterpret_cast<yukilinker_c_runtime*>(
                                __atomic_load_n(&g_runtime_owner, __ATOMIC_ACQUIRE)))
    loader_prepare_fork();
}

extern "C" void yukilinker_c_after_fork(yukilinker_c_runtime* runtime, int child) {
  if (runtime && runtime == reinterpret_cast<yukilinker_c_runtime*>(
                                __atomic_load_n(&g_runtime_owner, __ATOMIC_ACQUIRE)))
    loader_after_fork(child);
}

extern "C" void yukilinker_c_report_unreleased(yukilinker_c_runtime* runtime, const char* object) {
  OperationGuard operation;
  if (runtime && runtime == reinterpret_cast<yukilinker_c_runtime*>(g_runtime_owner) &&
      g_runtime_owner->active)
    host_log_error("%s", object ? object : "yukilinker: unreleased C++ ownership");
}
