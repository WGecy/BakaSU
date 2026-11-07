// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once
#include "runtime_model.hpp"
namespace yukilinker::internal {
void* owner_runtime_create(const void* host_services);
uint32_t owner_runtime_destroy(void* opaque);
uint32_t owner_try_shutdown(void* opaque, yukilinker_c_error_detail* error);
uint32_t owner_host_services_bind(void* opaque, const void* host_services);
uint32_t owner_context_destroy(void* opaque, void* context);
uint32_t owner_snapshot_acquire(void* opaque, void* context, uint64_t* token, size_t* count);
uint32_t owner_snapshot_image(void* opaque, uint64_t token, size_t index,
                              yukilinker_c_image_record* record);
uint32_t owner_snapshot_headers(void* runtime, uint64_t token, size_t index, void* headers,
                                size_t capacity, size_t* required);
uint32_t owner_snapshot_release(void* runtime, uint64_t token);
uint32_t owner_image_acquire(void* runtime, yukilinker_c_image_id id, uint32_t kind,
                             uint64_t* lease);
uint32_t owner_image_query(void* runtime, yukilinker_c_image_id id,
                           yukilinker_c_image_record* record);
uint32_t owner_image_release(void* runtime, uint64_t lease);
uint32_t owner_read_events(void* runtime, uint64_t cursor, yukilinker_c_runtime_event* events,
                           size_t capacity, size_t* written, uint64_t* next_cursor);
uint32_t owner_namespace_destroy(void* runtime, void* token);
uint64_t owner_namespace_id(void* runtime, void* token);
uint32_t owner_snapshot_mapping(void* runtime, uint64_t token, size_t index,
                                yukilinker_c_mapping_record* record);
uint32_t owner_image_detail(void* runtime, yukilinker_c_image_id id,
                            yukilinker_c_image_detail* detail);
uint32_t owner_image_dependencies(void* runtime, yukilinker_c_image_id id,
                                  yukilinker_c_dependency_record* records, size_t capacity,
                                  size_t* count);
uint32_t owner_image_program_headers(void* runtime, yukilinker_c_image_id id, void* headers,
                                     size_t capacity, size_t* required);
uint32_t owner_namespace_link(void* runtime, void* from, void* to, uint32_t flags,
                              const char* shared_sonames);
uint32_t owner_namespace_links(void* runtime, void* from,
                               yukilinker_c_namespace_link_record* records, size_t capacity,
                               size_t* count);
uint32_t owner_namespace_host_import(void* runtime, void* namespace_token,
                                     const yukilinker_c_host_import* import);
uint32_t owner_host_import_refusal(void* opaque, yukilinker_c_host_import_refusal* refusal);
uint32_t owner_wait_timeout(void* opaque, uint64_t nanoseconds);
uint32_t owner_load_waits(void* opaque, yukilinker_c_load_wait* records, size_t capacity,
                          size_t* count);
uint32_t owner_namespace_host_imports(void* runtime, void* namespace_token,
                                      yukilinker_c_host_import_record* records, size_t capacity,
                                      size_t* count);
}  // namespace yukilinker::internal
