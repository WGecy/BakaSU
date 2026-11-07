// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

const yukilinker_c_host_services* g_host_services = &kDefaultHostServices;
Runtime* g_runtime_owner = nullptr;
#if YUKILINKER_FULL
ShutdownPhase g_shutdown_phase = ShutdownPhase::kActive;
bool g_shutdown_running = false;
pthread_t g_shutdown_thread{};
bool g_host_import_leaked = false;
size_t g_host_closes_in_flight = 0;
#endif
size_t g_page_size = 0;
bool g_page_size_from_image = false;
MetadataPage* g_metadata_pages = nullptr;
size_t g_metadata_bytes = 0;
size_t g_peak_metadata_bytes = 0;
#if YUKILINKER_FULL
pthread_mutex_t g_metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
ThreadTlsState* g_thread_states = nullptr;
TlsTemplate* g_tls_templates = nullptr;
uintptr_t* g_tls_generation_chunks[kTlsGenerationChunks]{};
size_t g_tls_generation_chunk_count = 0;
uintptr_t g_next_tls_generation = 1;
pthread_mutex_t g_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t g_thread_tls_key;
bool g_thread_tls_key_valid = false;
bool g_tls_stopped = false;
pthread_mutex_t g_exit_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t g_initializing_key;
bool g_initializing_key_valid = false;
#endif
Context g_default_context;
Context* g_contexts = nullptr;
NamespaceToken* g_namespaces = nullptr;
TokenIndex<NamespaceToken> g_namespace_index;
#if YUKILINKER_FULL
TokenIndex<ImageState> g_image_index;
TokenIndex<ImageState> g_generation_index;
#endif
uint64_t g_next_namespace_id = 1;
yukilinker_c_error_detail* g_operation_error = nullptr;
OperationRefusal* g_operation_refusal = nullptr;
OperationRefusal* g_private_call_slot = nullptr;
const LoadOptions* g_open_options = nullptr;
ImageState* g_loading_parent = nullptr;
uint64_t g_next_image_generation = 1;
uint64_t g_initialization_order = 0;
uint64_t g_graph_visit = 0;
int g_relocation_pass = 1;
bool g_collecting = false;
bool g_collect_again = false;
uint64_t g_collection_pass = 0;
#if YUKILINKER_FULL
pthread_t g_collecting_thread{};
pthread_mutex_t g_operation_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
pthread_key_t g_operation_depth_key;
pthread_key_t g_callback_key;
pthread_key_t g_snapshot_key;
pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_keys_retired = false;
bool g_fork_locked = false;
bool g_operation_depth_valid = false;
size_t g_active_operations = 0;
size_t g_lock_waiters = 0;
bool g_runtime_creating = false;
pthread_t g_runtime_creating_thread{};
LoadWaiter* g_load_waiters = nullptr;
pthread_cond_t g_load_wait_cond = PTHREAD_COND_INITIALIZER;
uint64_t g_wait_timeout_ns = 0;
WaitSlot g_wait_slots[kWaitSlots];
uint32_t g_wait_overflow = 0;
WaitOverflow* g_wait_overflows = nullptr;
SnapshotGuard* g_active_snapshots = nullptr;
#endif
RetainedSource* g_retained_sources = nullptr;
ImageState* g_first_image = nullptr;
ImageState* g_last_image = nullptr;
uint64_t g_image_adds = 0;
uint64_t g_image_subs = 0;
uint64_t g_event_sequence = 0;
yukilinker_c_runtime_event g_runtime_events[kRuntimeEventCapacity]{};
#if YUKILINKER_FULL
pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

}  // namespace yukilinker::internal
