// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../../include/yukilinker.h"
#include "loader_types.hpp"

// Field offsets, field types and alignment of the records the runtime copies out whole, and of
// the inputs a host builds against. Most records have no size negotiation and
// the inputs are checked against sizeof, so a reordered, retyped or repacked field breaks every
// host built against the old header. Offsets alone miss a same-width retype (uint64_t to double)
// and a pragma pack; the type and alignof lines catch those. Checked in the production build
// for both runtime and bootstrap.
namespace yukilinker::internal::abi_layout {

#if defined(__LP64__)
constexpr size_t kWord = 8;
// uint64_t fields after pointer-sized ones start on an 8-byte boundary on both ABIs.
constexpr size_t kRecordCounters = 48;
constexpr size_t kLookupError = 16;
constexpr size_t kInfoGeneration = 32;
#else
constexpr size_t kWord = 4;
constexpr size_t kRecordCounters = 40;
constexpr size_t kLookupError = 8;
constexpr size_t kInfoGeneration = 16;
#endif
static_assert(sizeof(void*) == kWord && sizeof(size_t) == kWord && sizeof(uintptr_t) == kWord);

static_assert(offsetof(yukilinker_c_image_id, generation) == 0);
static_assert(offsetof(yukilinker_c_image_id, namespace_id) == 8);
static_assert(sizeof(yukilinker_c_image_id) == 16);
static_assert(alignof(yukilinker_c_image_id) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_image_id::generation), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_id::namespace_id), uint64_t>);

static_assert(offsetof(yukilinker_c_error_detail, code) == 0);
static_assert(offsetof(yukilinker_c_error_detail, stage) == 4);
static_assert(offsetof(yukilinker_c_error_detail, system_errno) == 8);
static_assert(offsetof(yukilinker_c_error_detail, relocation_type) == 12);
static_assert(offsetof(yukilinker_c_error_detail, offset) == 16);
static_assert(offsetof(yukilinker_c_error_detail, machine) == 24);
static_assert(offsetof(yukilinker_c_error_detail, execution_started) == 28);
static_assert(offsetof(yukilinker_c_error_detail, cleanup_incomplete) == 32);
static_assert(offsetof(yukilinker_c_error_detail, object) == 36 &&
              sizeof(yukilinker_c_error_detail::object) == 256);
static_assert(offsetof(yukilinker_c_error_detail, symbol) == 292 &&
              sizeof(yukilinker_c_error_detail::symbol) == 128);
static_assert(offsetof(yukilinker_c_error_detail, version) == 420 &&
              sizeof(yukilinker_c_error_detail::version) == 128);
static_assert(sizeof(yukilinker_c_error_detail) == 552);
static_assert(alignof(yukilinker_c_error_detail) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::code), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::stage), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::system_errno), int32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::relocation_type), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::offset), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::machine), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::execution_started), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::cleanup_incomplete), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::object), char[256]>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::symbol), char[128]>);
static_assert(std::is_same_v<decltype(yukilinker_c_error_detail::version), char[128]>);

static_assert(offsetof(yukilinker_c_image_info, load_bias) == 0);
static_assert(offsetof(yukilinker_c_image_info, mapping_start) == kWord);
static_assert(offsetof(yukilinker_c_image_info, mapping_size) == kWord * 2);
static_assert(offsetof(yukilinker_c_image_info, name) == kWord * 3);
static_assert(offsetof(yukilinker_c_image_info, generation) == kInfoGeneration);
static_assert(sizeof(yukilinker_c_image_info) == kInfoGeneration + 8);
static_assert(alignof(yukilinker_c_image_info) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_image_info::load_bias), uintptr_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_info::mapping_start), uintptr_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_info::mapping_size), size_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_info::name), const char*>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_info::generation), uint64_t>);

constexpr size_t kC = kRecordCounters;
static_assert(offsetof(yukilinker_c_image_record, size) == 0);
static_assert(offsetof(yukilinker_c_image_record, phase) == 4);
static_assert(offsetof(yukilinker_c_image_record, id) == 8);
static_assert(offsetof(yukilinker_c_image_record, load_bias) == 24);
static_assert(offsetof(yukilinker_c_image_record, mapping_start) == 24 + kWord);
static_assert(offsetof(yukilinker_c_image_record, mapping_size) == 24 + kWord * 2);
static_assert(offsetof(yukilinker_c_image_record, device) == kC);
static_assert(offsetof(yukilinker_c_image_record, inode) == kC + 8);
static_assert(offsetof(yukilinker_c_image_record, source_offset) == kC + 16);
static_assert(offsetof(yukilinker_c_image_record, flags) == kC + 24);
static_assert(offsetof(yukilinker_c_image_record, program_header_count) == kC + 32);
static_assert(offsetof(yukilinker_c_image_record, tls_module_id) == kC + 40);
static_assert(offsetof(yukilinker_c_image_record, tls_generation) == kC + 48);
static_assert(offsetof(yukilinker_c_image_record, open_references) == kC + 56);
static_assert(offsetof(yukilinker_c_image_record, tls_references) == kC + 64);
static_assert(offsetof(yukilinker_c_image_record, destructor_references) == kC + 72);
static_assert(offsetof(yukilinker_c_image_record, callback_references) == kC + 80);
static_assert(offsetof(yukilinker_c_image_record, snapshot_references) == kC + 88);
static_assert(offsetof(yukilinker_c_image_record, lease_references) == kC + 96);
static_assert(offsetof(yukilinker_c_image_record, execution_leases) == kC + 104);
static_assert(offsetof(yukilinker_c_image_record, unwind_leases) == kC + 112);
static_assert(offsetof(yukilinker_c_image_record, cfi_leases) == kC + 120);
static_assert(offsetof(yukilinker_c_image_record, host_bridge_leases) == kC + 128);
static_assert(offsetof(yukilinker_c_image_record, dependency_references) == kC + 136);
static_assert(offsetof(yukilinker_c_image_record, private_dependencies) == kC + 144);
static_assert(offsetof(yukilinker_c_image_record, system_dependencies) == kC + 152);
static_assert(offsetof(yukilinker_c_image_record, nodelete_reasons) == kC + 160);
static_assert(offsetof(yukilinker_c_image_record, failure_code) == kC + 164);
static_assert(offsetof(yukilinker_c_image_record, origin) == kC + 168);
static_assert(sizeof(yukilinker_c_image_record::origin) == 4096);
static_assert(sizeof(yukilinker_c_image_record) == kC + 168 + 4096);
static_assert(alignof(yukilinker_c_image_record) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::size), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::phase), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::id), yukilinker_c_image_id>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::load_bias), uintptr_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::mapping_start), uintptr_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::mapping_size), size_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::device), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::inode), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::source_offset), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::flags), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::program_header_count), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::tls_module_id), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::tls_generation), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::open_references), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::tls_references), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::destructor_references), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::callback_references), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::snapshot_references), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::lease_references), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::execution_leases), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::unwind_leases), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::cfi_leases), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::host_bridge_leases), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::dependency_references), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::private_dependencies), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::system_dependencies), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::nodelete_reasons), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::failure_code), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_record::origin), char[4096]>);

static_assert(offsetof(yukilinker_c_mapping_record, size) == 0);
static_assert(offsetof(yukilinker_c_mapping_record, flags) == 4);
static_assert(offsetof(yukilinker_c_mapping_record, relro_written_bytes) == 8);
static_assert(offsetof(yukilinker_c_mapping_record, relro_shared_bytes) == 16);
static_assert(sizeof(yukilinker_c_mapping_record) == 24);
static_assert(alignof(yukilinker_c_mapping_record) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_mapping_record::size), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_mapping_record::flags), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_mapping_record::relro_written_bytes), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_mapping_record::relro_shared_bytes), uint64_t>);

static_assert(offsetof(yukilinker_c_runtime_event, sequence) == 0);
static_assert(offsetof(yukilinker_c_runtime_event, id) == 8);
static_assert(offsetof(yukilinker_c_runtime_event, type) == 24);
static_assert(offsetof(yukilinker_c_runtime_event, phase) == 28);
static_assert(offsetof(yukilinker_c_runtime_event, error_code) == 32);
static_assert(offsetof(yukilinker_c_runtime_event, reserved) == 36);
static_assert(offsetof(yukilinker_c_runtime_event, origin) == 40 &&
              sizeof(yukilinker_c_runtime_event::origin) == 128);
static_assert(sizeof(yukilinker_c_runtime_event) == 168);
static_assert(alignof(yukilinker_c_runtime_event) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_runtime_event::sequence), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_runtime_event::id), yukilinker_c_image_id>);
static_assert(std::is_same_v<decltype(yukilinker_c_runtime_event::type), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_runtime_event::phase), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_runtime_event::error_code), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_runtime_event::reserved), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_runtime_event::origin), char[128]>);

static_assert(offsetof(yukilinker_c_dependency_record, kind) == 0);
static_assert(offsetof(yukilinker_c_dependency_record, flags) == 4);
static_assert(offsetof(yukilinker_c_dependency_record, target) == 8);
static_assert(offsetof(yukilinker_c_dependency_record, name) == 24);
static_assert(sizeof(yukilinker_c_dependency_record::name) == 256);
static_assert(sizeof(yukilinker_c_dependency_record) == 280);
static_assert(alignof(yukilinker_c_dependency_record) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_dependency_record::kind), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_dependency_record::flags), uint32_t>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_dependency_record::target), yukilinker_c_image_id>);
static_assert(std::is_same_v<decltype(yukilinker_c_dependency_record::name), char[256]>);

static_assert(offsetof(yukilinker_c_namespace_link_record, flags) == 0);
static_assert(offsetof(yukilinker_c_namespace_link_record, reserved) == 4);
static_assert(offsetof(yukilinker_c_namespace_link_record, target_namespace_id) == 8);
static_assert(offsetof(yukilinker_c_namespace_link_record, shared_sonames) == 16);
static_assert(sizeof(yukilinker_c_namespace_link_record::shared_sonames) == 4096);
static_assert(sizeof(yukilinker_c_namespace_link_record) == 4112);
static_assert(alignof(yukilinker_c_namespace_link_record) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_namespace_link_record::flags), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_namespace_link_record::reserved), uint32_t>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_namespace_link_record::target_namespace_id), uint64_t>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_namespace_link_record::shared_sonames), char[4096]>);

static_assert(offsetof(yukilinker_c_host_import, size) == 0);
static_assert(offsetof(yukilinker_c_host_import, flags) == 4);
static_assert(offsetof(yukilinker_c_host_import, library) == 8);
static_assert(offsetof(yukilinker_c_host_import, symbols) == 8 + kWord);
static_assert(sizeof(yukilinker_c_host_import) == 8 + 2 * kWord);
static_assert(alignof(yukilinker_c_host_import) == kWord);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import::size), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import::flags), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import::library), const char*>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import::symbols), const char*>);

static_assert(offsetof(yukilinker_c_host_import_record, flags) == 0);
static_assert(offsetof(yukilinker_c_host_import_record, reserved) == 4);
static_assert(offsetof(yukilinker_c_host_import_record, library) == 8);
static_assert(offsetof(yukilinker_c_host_import_record, symbols) == 264);
static_assert(sizeof(yukilinker_c_host_import_record) == 4360);
static_assert(alignof(yukilinker_c_host_import_record) == 4);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import_record::flags), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import_record::reserved), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import_record::library), char[256]>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_import_record::symbols), char[4096]>);

// yukilinker_c_image_detail is size-negotiated and may grow at the end; its first version is
// pinned.
static_assert(offsetof(yukilinker_c_image_detail, size) == 0);
static_assert(offsetof(yukilinker_c_image_detail, flags) == 4);
static_assert(offsetof(yukilinker_c_image_detail, id) == 8);
static_assert(offsetof(yukilinker_c_image_detail, failure_code) == 24);
static_assert(offsetof(yukilinker_c_image_detail, failure_stage) == 28);
static_assert(offsetof(yukilinker_c_image_detail, failure_errno) == 32);
static_assert(offsetof(yukilinker_c_image_detail, mapping_flags) == 36);
static_assert(offsetof(yukilinker_c_image_detail, namespace_parent_id) == 40);
static_assert(offsetof(yukilinker_c_image_detail, tls_initial_image) == 48);
static_assert(offsetof(yukilinker_c_image_detail, tls_file_bytes) == 56);
static_assert(offsetof(yukilinker_c_image_detail, tls_memory_bytes) == 64);
static_assert(offsetof(yukilinker_c_image_detail, tls_alignment) == 72);
static_assert(offsetof(yukilinker_c_image_detail, tls_block) == 80);
static_assert(offsetof(yukilinker_c_image_detail, relro_segment_count) == 88);
static_assert(offsetof(yukilinker_c_image_detail, relro_start) == 96);
static_assert(offsetof(yukilinker_c_image_detail, relro_size) == 104);
static_assert(offsetof(yukilinker_c_image_detail, relro_written_bytes) == 112);
static_assert(offsetof(yukilinker_c_image_detail, relro_shared_bytes) == 120);
static_assert(sizeof(yukilinker_c_image_detail) == 128);
static_assert(alignof(yukilinker_c_image_detail) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::size), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::flags), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::id), yukilinker_c_image_id>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::failure_code), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::failure_stage), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::failure_errno), int32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::mapping_flags), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::namespace_parent_id), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::tls_initial_image), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::tls_file_bytes), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::tls_memory_bytes), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::tls_alignment), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::tls_block), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::relro_segment_count), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::relro_start), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::relro_size), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::relro_written_bytes), uint64_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_image_detail::relro_shared_bytes), uint64_t>);

// Inputs whose size must equal sizeof (open options, host table) or that the runtime fills (the
// dlopen request), and the bootstrap options, whose size must be at least sizeof.

constexpr size_t kRequester = (8 + kWord * 2 + 7) & ~size_t{7};
static_assert(offsetof(yukilinker_c_host_dlopen_request, size) == 0);
static_assert(offsetof(yukilinker_c_host_dlopen_request, flags) == 4);
static_assert(offsetof(yukilinker_c_host_dlopen_request, name) == 8);
static_assert(offsetof(yukilinker_c_host_dlopen_request, extension) == 8 + kWord);
static_assert(offsetof(yukilinker_c_host_dlopen_request, requester) == kRequester);
static_assert(sizeof(yukilinker_c_host_dlopen_request) == kRequester + 16);
static_assert(alignof(yukilinker_c_host_dlopen_request) == 8);
static_assert(std::is_same_v<decltype(yukilinker_c_host_dlopen_request::size), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_dlopen_request::flags), int32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_dlopen_request::name), const char*>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_dlopen_request::extension), const void*>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_host_dlopen_request::requester), yukilinker_c_image_id>);

static_assert(offsetof(yukilinker_c_host_services, size) == 0);
static_assert(offsetof(yukilinker_c_host_services, version) == 4);
static_assert(offsetof(yukilinker_c_host_services, user) == 8);
static_assert(offsetof(yukilinker_c_host_services, system_dlopen) == 8 + kWord * 1);
static_assert(offsetof(yukilinker_c_host_services, system_dlsym) == 8 + kWord * 2);
static_assert(offsetof(yukilinker_c_host_services, system_dlclose) == 8 + kWord * 3);
static_assert(offsetof(yukilinker_c_host_services, system_dladdr) == 8 + kWord * 4);
static_assert(offsetof(yukilinker_c_host_services, system_dlerror) == 8 + kWord * 5);
static_assert(offsetof(yukilinker_c_host_services, system_iterate_phdr) == 8 + kWord * 6);
static_assert(offsetof(yukilinker_c_host_services, log) == 8 + kWord * 7);
static_assert(offsetof(yukilinker_c_host_services, page_size) == 8 + kWord * 8);
static_assert(offsetof(yukilinker_c_host_services, close_fd) == 8 + kWord * 9);
static_assert(offsetof(yukilinker_c_host_services, fork_hook) == 8 + kWord * 10);
static_assert(sizeof(yukilinker_c_host_services) == 8 + kWord * 11);
static_assert(alignof(yukilinker_c_host_services) == kWord);
static_assert(std::is_same_v<decltype(yukilinker_c_host_services::size), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_services::version), uint32_t>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_services::user), void*>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_host_services::system_dlopen), yukilinker_c_host_dlopen>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_host_services::system_dlsym), yukilinker_c_host_dlsym>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_services::system_dlclose),
                             yukilinker_c_host_dlclose>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_host_services::system_dladdr), yukilinker_c_host_dladdr>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_services::system_dlerror),
                             yukilinker_c_host_dlerror>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_services::system_iterate_phdr),
                             yukilinker_c_host_iterate_phdr>);
static_assert(std::is_same_v<decltype(yukilinker_c_host_services::log), yukilinker_c_host_log>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_host_services::page_size), yukilinker_c_host_page_size>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_host_services::close_fd), yukilinker_c_host_close_fd>);
static_assert(
    std::is_same_v<decltype(yukilinker_c_host_services::fork_hook), yukilinker_c_host_fork_hook>);
// The host entry types themselves, which the field types above only name.
static_assert(
    std::is_same_v<yukilinker_c_host_log, void (*)(void*, uint32_t, const char*, size_t)>);
static_assert(std::is_same_v<yukilinker_c_host_dlopen,
                             void* (*)(void*, const yukilinker_c_host_dlopen_request*)>);
static_assert(std::is_same_v<yukilinker_c_host_dlsym,
                             int (*)(void*, void*, const char*, const char*, void**)>);
static_assert(std::is_same_v<yukilinker_c_host_dlclose, int (*)(void*, void*)>);
static_assert(std::is_same_v<yukilinker_c_host_dladdr, int (*)(void*, const void*, Dl_info*)>);
static_assert(std::is_same_v<yukilinker_c_host_dlerror, char* (*)(void*)>);
static_assert(std::is_same_v<yukilinker_c_host_iterate_phdr,
                             int (*)(void*, int (*)(dl_phdr_info*, size_t, void*), void*)>);
static_assert(std::is_same_v<yukilinker_c_host_page_size, size_t (*)(void*)>);
static_assert(std::is_same_v<yukilinker_c_host_close_fd, int (*)(void*, int)>);
static_assert(std::is_same_v<yukilinker_c_host_fork_hook, void (*)(void*, uint32_t)>);
static_assert(
    std::is_same_v<yukilinker_c_dependency_provider, int (*)(void*, const char*, const char*)>);
static_assert(
    std::is_same_v<yukilinker_c_image_visitor, int (*)(const yukilinker_c_image_info*, void*)>);

static_assert(offsetof(yukilinker_c_load_wait, size) == 0);
static_assert(offsetof(yukilinker_c_load_wait, kind) == 4);
static_assert(offsetof(yukilinker_c_load_wait, thread) == 8);
static_assert(offsetof(yukilinker_c_load_wait, owner) == 12);
static_assert(offsetof(yukilinker_c_load_wait, image) == 16);
static_assert(offsetof(yukilinker_c_load_wait, elapsed_ns) == 32);
static_assert(sizeof(yukilinker_c_load_wait) == 40);
static_assert(alignof(yukilinker_c_load_wait) == 8);
static_assert(YUKILINKER_C_LOAD_WAIT_LOAD == 1 && YUKILINKER_C_LOAD_WAIT_WAIT == 2);
static_assert(offsetof(yukilinker_c_host_import_refusal, size) == 0);
static_assert(offsetof(yukilinker_c_host_import_refusal, reason) == 4);
static_assert(offsetof(yukilinker_c_host_import_refusal, library) == 8);
static_assert(offsetof(yukilinker_c_host_import_refusal, symbol) == 264);
static_assert(offsetof(yukilinker_c_host_import_refusal, version) == 392);
static_assert(sizeof(yukilinker_c_host_import_refusal) == 520);
static_assert(alignof(yukilinker_c_host_import_refusal) == 4);
static_assert(YUKILINKER_C_HOST_IMPORT_NOT_GLOBAL_OR_WEAK == 1 &&
              YUKILINKER_C_HOST_IMPORT_HIDDEN == 2 && YUKILINKER_C_HOST_IMPORT_TLS == 3 &&
              YUKILINKER_C_HOST_IMPORT_IFUNC == 4 && YUKILINKER_C_HOST_IMPORT_ABSOLUTE == 5 &&
              YUKILINKER_C_HOST_IMPORT_RESERVED_SECTION == 6 &&
              YUKILINKER_C_HOST_IMPORT_TYPE_MISMATCH == 7 &&
              YUKILINKER_C_HOST_IMPORT_NOT_FOUND_BY_SYSTEM == 8 &&
              YUKILINKER_C_HOST_IMPORT_OUTSIDE_MAPPINGS == 9 &&
              YUKILINKER_C_HOST_IMPORT_TWO_OWNERS == 10 &&
              YUKILINKER_C_HOST_IMPORT_NOT_SYSTEM_VALUE == 11 &&
              YUKILINKER_C_HOST_IMPORT_NOT_CODE == 12 && YUKILINKER_C_HOST_IMPORT_NOT_DATA == 13);

// Values a host compares against. An enumerator inserted in the middle, a value reused or a
// capability bit moved or shared breaks every host built against the old header without any crash
// to show it, so every value is pinned here.
static_assert(YUKILINKER_C_OK == 0 && YUKILINKER_C_INVALID_ELF == 1 &&
              YUKILINKER_C_UNSUPPORTED == 2 && YUKILINKER_C_DEPENDENCY_MISSING == 3 &&
              YUKILINKER_C_SYMBOL_MISSING == 4 && YUKILINKER_C_PERMISSION_DENIED == 5 &&
              YUKILINKER_C_NO_MEMORY == 6 && YUKILINKER_C_IO_ERROR == 7 && YUKILINKER_C_BUSY == 8 &&
              YUKILINKER_C_INVALID_HANDLE == 9 && YUKILINKER_C_RETRY == 10 &&
              YUKILINKER_C_BUFFER_TOO_SMALL == 11 && YUKILINKER_C_STALE_TOKEN == 12);
static_assert(YUKILINKER_C_SOURCE == 0 && YUKILINKER_C_METADATA == 1 &&
              YUKILINKER_C_DEPENDENCIES == 2 && YUKILINKER_C_RELOCATIONS == 3 &&
              YUKILINKER_C_PROTECTION == 4 && YUKILINKER_C_INITIALIZATION == 5 &&
              YUKILINKER_C_RETIREMENT == 6);
static_assert(YUKILINKER_C_CLOSED == 0 && YUKILINKER_C_RETAINED == 1 &&
              YUKILINKER_C_CLOSE_ERROR == 2);
static_assert(YUKILINKER_C_IMAGE_MAPPING == 0 && YUKILINKER_C_IMAGE_RELOCATED == 1 &&
              YUKILINKER_C_IMAGE_INITIALIZING == 2 && YUKILINKER_C_IMAGE_READY == 3 &&
              YUKILINKER_C_IMAGE_CLOSING == 4 && YUKILINKER_C_IMAGE_RETIRED == 5 &&
              YUKILINKER_C_IMAGE_FAILED == 6);
static_assert(YUKILINKER_C_EVENT_PUBLISH == 0 && YUKILINKER_C_EVENT_RELOCATE == 1 &&
              YUKILINKER_C_EVENT_CONSTRUCTOR == 2 && YUKILINKER_C_EVENT_READY == 3 &&
              YUKILINKER_C_EVENT_CLOSE == 4 && YUKILINKER_C_EVENT_DESTRUCTOR == 5 &&
              YUKILINKER_C_EVENT_RETIRE == 6 && YUKILINKER_C_EVENT_CLEANUP_FAILURE == 7 &&
              YUKILINKER_C_EVENT_LOAD_FAILURE == 8);
static_assert(YUKILINKER_C_NODELETE_REQUEST == 1U && YUKILINKER_C_NODELETE_ELF == 2U &&
              YUKILINKER_C_NODELETE_FAILED_EXECUTION == 4U &&
              YUKILINKER_C_NODELETE_CALLBACK_REENTRY == 8U && YUKILINKER_C_NODELETE_FORK == 16U &&
              YUKILINKER_C_NODELETE_SELF_CLOSE == 32U);
static_assert(YUKILINKER_C_LEASE_OBSERVER == 0 && YUKILINKER_C_LEASE_EXECUTION == 1 &&
              YUKILINKER_C_LEASE_UNWIND == 2 && YUKILINKER_C_LEASE_CFI == 3 &&
              YUKILINKER_C_LEASE_HOST_BRIDGE == 4);
static_assert(YUKILINKER_C_MAPPING_FILE_BACKED == 1U &&
              YUKILINKER_C_MAPPING_CALLER_RESERVED == 2U &&
              YUKILINKER_C_MAPPING_WRITE_RELRO == 4U && YUKILINKER_C_MAPPING_USE_RELRO == 8U);
static_assert(YUKILINKER_C_DETAIL_FAILURE_STAGE == 1U &&
              YUKILINKER_C_DETAIL_NAMESPACE_PARENT == 2U &&
              YUKILINKER_C_DETAIL_RELRO_PROTECTED == 4U);
static_assert(YUKILINKER_C_DEPENDENCY_PRIVATE == 0 && YUKILINKER_C_DEPENDENCY_SYSTEM == 1 &&
              YUKILINKER_C_DEPENDENCY_UNRESOLVED == 2);
static_assert(YUKILINKER_C_DEPENDENCY_PROVIDER == 1U &&
              YUKILINKER_C_DEPENDENCY_NAME_TRUNCATED == 2U && YUKILINKER_C_DEPENDENCY_LINKED == 4U);
static_assert(YUKILINKER_C_NAMESPACE_LINK_SYSTEM == 1U && YUKILINKER_C_NAMESPACE_LINK_ALL == 2U);
static_assert(YUKILINKER_C_HOST_FORK_PREPARE == 0 && YUKILINKER_C_HOST_FORK_PARENT == 1 &&
              YUKILINKER_C_HOST_FORK_CHILD == 2);

// Module handles and complete operation results.
static_assert(sizeof(yukilinker_c_handle) == 16 && alignof(yukilinker_c_handle) == 8);
static_assert(offsetof(yukilinker_c_handle, generation) == 8);
static_assert(sizeof(yukilinker_c_error) == 1072 && offsetof(yukilinker_c_error, detail) == 8);
static_assert(offsetof(yukilinker_c_error, system_message) == 560);
static_assert(sizeof(yukilinker_c_handle_info) == 72 &&
              offsetof(yukilinker_c_handle_info, handle) == 8);
static_assert(offsetof(yukilinker_c_handle_info, capabilities) == 40);
static_assert(offsetof(yukilinker_c_handle_info, private_image) == 48);
static_assert(sizeof(yukilinker_c_open_options) == 24 + 3 * kWord);
static_assert(offsetof(yukilinker_c_open_options, context) == 16);
static_assert(offsetof(yukilinker_c_open_options, force_load) == 16 + 3 * kWord);
static_assert(sizeof(yukilinker_c_lookup_result) == 1072 + (kWord == 8 ? 16 : 8));
static_assert(sizeof(yukilinker_c_close_result) == 1152);
static_assert(offsetof(yukilinker_c_close_result, handle_valid) == 1144);
static_assert(sizeof(yukilinker_c_runtime_stats) == 120);

}  // namespace yukilinker::internal::abi_layout
