// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once
#include "runtime_model.hpp"
namespace yukilinker::internal {
void* module_runtime_create(const yukilinker_c_host_services* services);
uint32_t module_runtime_destroy(void* opaque);
uint32_t module_try_shutdown(void* opaque, yukilinker_c_error_detail* output);
uint32_t module_set_limits(void* opaque, uint32_t handles, uint64_t bytes);
uint32_t module_query_stats(void* opaque, yukilinker_c_runtime_stats* output);
void* module_context_create(void* opaque, const char* path,
                            yukilinker_c_dependency_provider provider, void* user);
void* module_namespace_create(void* opaque, void* token);
yukilinker_c_handle module_open_path(void* runtime, const char* path,
                                     const yukilinker_c_open_options* options,
                                     yukilinker_c_error* output);
yukilinker_c_handle module_open_fd(void* runtime, int fd, const yukilinker_c_open_options* options,
                                   yukilinker_c_error* output);
yukilinker_c_lookup_result module_lookup(void* opaque, yukilinker_c_handle handle, const char* name,
                                         const char* version);
uint32_t module_query(void* opaque, yukilinker_c_handle handle, yukilinker_c_handle_info* output);
uint32_t module_handles(void* opaque, yukilinker_c_handle_info* output, size_t capacity,
                        size_t* count);
yukilinker_c_close_result module_close(void* opaque, yukilinker_c_handle handle);
uint32_t module_forget(void* opaque, yukilinker_c_handle handle);
uint32_t module_release_host(void*, yukilinker_c_handle);
uint32_t module_image(void*, yukilinker_c_handle, yukilinker_c_image_record*);
}  // namespace yukilinker::internal
