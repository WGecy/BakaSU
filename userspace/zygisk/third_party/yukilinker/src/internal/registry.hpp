// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

// Registry views and managed namespace operations. registry.cpp is built into the runtime only; the
// bootstrap loader does not carry it.

#include "runtime_model.hpp"

namespace yukilinker::internal {

NamespaceToken* find_namespace(void* opaque);
// True while a snapshot or lease is outstanding; shutdown refuses then.
bool registry_has_holders();
uint32_t query_handle_record(void* handle, yukilinker_c_image_record* record);
// Copied image detail queries; an id is visible exactly when image_query would return it.
uint32_t query_image_detail(yukilinker_c_image_id id, yukilinker_c_image_detail* detail);
uint32_t query_image_dependencies(yukilinker_c_image_id id, yukilinker_c_dependency_record* records,
                                  size_t capacity, size_t* count);
uint32_t query_image_program_headers(yukilinker_c_image_id id, void* output, size_t bytes,
                                     size_t* required);

void* registry_namespace_create(void* opaque);
uint32_t registry_namespace_destroy(void* opaque);
uint64_t registry_namespace_id(void* opaque);
// Namespace links (yukilinker.h documents both).
uint32_t api_namespace_link(void* from, void* to, uint32_t flags, const char* shared_sonames);
uint32_t api_namespace_links(void* from, yukilinker_c_namespace_link_record* records,
                             size_t capacity, size_t* count);

uint32_t registry_snapshot_acquire(void* context, uint64_t* token, size_t* count);
uint32_t registry_snapshot_image(uint64_t token, size_t index, yukilinker_c_image_record* record);
uint32_t registry_snapshot_headers(uint64_t token, size_t index, void* output, size_t bytes,
                                   size_t* required);
uint32_t registry_snapshot_mapping(uint64_t token, size_t index,
                                   yukilinker_c_mapping_record* record);
uint32_t registry_snapshot_release(uint64_t token);
uint32_t registry_image_acquire_kind(yukilinker_c_image_id id, uint32_t kind, uint64_t* lease);
uint32_t registry_image_acquire(yukilinker_c_image_id id, uint64_t* lease);
uint32_t registry_image_query(yukilinker_c_image_id id, yukilinker_c_image_record* record);
uint32_t registry_image_release(uint64_t lease);
uint32_t registry_read_events(uint64_t since, yukilinker_c_runtime_event* output, size_t capacity,
                              size_t* written, uint64_t* next);

}  // namespace yukilinker::internal
