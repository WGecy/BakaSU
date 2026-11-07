// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../include/yukilinker.h"
#include "address_space.hpp"
#include "config.hpp"
#include "elf_model.hpp"
#include "image_state.hpp"
#include "loader_types.hpp"

namespace yukilinker::internal {

#if defined(YUKILINKER_BOOTSTRAP)
// Checks stage-one's fixed Core contract before resolvers or constructors run.
bool validate_bootstrap_core(ImageState* image);
#endif

bool parse_dynamic_tags(ImageState* image, size_t dynamic_index, DynamicParse* parse);
bool establish_symbol_count(ImageState* image, const DynamicParse& parse);
bool validate_versions(ImageState* image);
// The first symbol that needs the process's own unwinder, C++ ABI or CFI runtime ("" for an
// unreadable name), or null.
const char* requires_system_runtime(const ImageState* image);
const char* string_at(const ImageState* image, size_t offset);
// The Verneed after need in its chain, which validate_versions checked: inside the image, aligned.
const ElfW(Verneed) * next_verneed(ImageState* image, const ElfW(Verneed) * need);
// retiring: an image retiring_here counts as well (a private dlsym of a pseudo-handle).
const ElfW(Sym) * find_defined_symbol(ImageState* image, const char* name,
                                      const char* version = nullptr, bool external = true,
                                      bool retiring = false);
const ElfW(Sym) * symbol_at(const ImageState* image, uint32_t index);
const char* symbol_version(ImageState* image, size_t index, bool undefined);
bool symbol_name_valid(const DynamicSymbols& symbols, const ElfW(Sym) & symbol);

// A root load (no loading parent) stores the generation of the image it creates in *group, as
// soon as it exists; an open's group is the images whose load root has that generation.
// by_path: the runtime opened memfd itself by path, so the image takes the name the kernel has for
// the file (Bionic's realpath_fd); a descriptor from the host or a provider keeps the name given.
SoHandle* load_image(int memfd, const char* vma_name, bool file_backed, uint64_t source_offset = 0,
                     const LoadPlacement* placement = nullptr, uint64_t* group = nullptr,
                     bool by_path = false);
bool open_dependencies(ImageState* image, const DynamicParse& parse);
bool close_dependencies(ImageState* image);
int open_private_dependency(ImageState* image, const char* name, char* path, size_t capacity,
                            const char** origin, bool* by_path);
#if YUKILINKER_FULL
// What a name the requester's namespace could not resolve itself found through its links.
enum class LinkOutcome {
  kUnlinked,  // no link applies: the namespace's own rules decide
  kImage,     // a Ready image of a linked namespace
  kSystem,    // the system linker, through a system link
  kBusy,      // a linked namespace's image of that name is still loading
  kDenied,    // the namespace has a system link and no link shares the name
  kMissing,   // a link shares the name, but nothing it leads to has it
};
struct LinkResolution {
  LinkOutcome outcome = LinkOutcome::kUnlinked;
  ImageState* image = nullptr;
  void* system_handle = nullptr;
};
// One hop through the requester's namespace links, in the order they were added. A system link
// asks the host without loading first and then with flags; it may drop the lock.
LinkResolution resolve_through_links(ImageState* requester, const char* name, int flags);
#endif  // #if YUKILINKER_FULL

SymbolResolution materialize_defined_symbol(ImageState* image, const ElfW(Sym) & symbol);
SymbolTarget search_scope(ImageState* root, const char* name, const char* version,
                          GlobalLookup globals, ImageState* after = nullptr);
// Visits, in search_scope's order without its global pass, every private image a lookup through
// root's handle searches (GlobalLookup::None): root, what its dependency edges reach in its
// context, and a link's target without its own edges. visit returns false to stop. False only
// when the walk's queue cannot grow.
bool walk_handle_scope(ImageState* root, bool (*visit)(ImageState*, void*), void* user);
// The relocation type that asks, or kTlsReference for a TLS reference; a host import binds only a
// symbol the type can take.
constexpr uint32_t kTlsReference = UINT32_MAX;
SymbolTarget symbol_target(ImageState* image, uint32_t index, uint32_t relocation_type);
SymbolResolution resolve_symbol(ImageState* image, uint32_t index, const SymbolTarget& target);
bool relocate_image(ImageState* image);
bool validate_hwasan_globals(ImageState* image);
bool register_hwasan_globals(ImageState* image);
void unregister_hwasan_globals(ImageState* image);
void discard_deferred_relocations(ImageState* image);
void* module_api_symbol(const char* name);
int module_thread_atexit(void (*function)(void*), void* argument, void* dso);

void release_failed_image(ImageState* image);
// Frees an unregistered image's metadata, handle and state. False when a metadata page stays
// mapped: it is parked for shutdown, and a CLEANUP_FAILURE event that names no image says so.
[[nodiscard]] bool free_image_state(ImageState* image);
// A reference from outside the registry: an observer, the loading thread, a TLS block or a
// thread destructor. Both the failure sweep and collection have to see all of them.
bool image_is_pinned(const ImageState* image);
// Runs the constructors a registered image and what it needs have not run yet, as Bionic's dlopen
// does for the library it returns. Fails before any code runs when one of them is still
// preparing, has failed, or holds a slot that is not code; once code ran, a failure leaves those
// images Failed. Either way it drops the one reference the caller took for this open.
bool initialize_reused(ImageState* image);
// Every Verneed's vn_file names a library this image's DT_NEEDED entries resolved to (by its
// soname); INVALID_ELF/METADATA, naming the file, otherwise. Run once open_dependencies is done.
bool verneed_files_resolved(ImageState* image);
// What an open that reuses an image added of its RTLD_GLOBAL and RTLD_NODELETE, so that a failed
// open can take exactly that back.
struct AddedOpenFlags {
  uint32_t flags = 0;
  bool nodelete_request = false;
};
AddedOpenFlags add_open_flags(ImageState* image, uint32_t requested);
void remove_open_flags(ImageState* image, AddedOpenFlags added);
#if YUKILINKER_FULL
// An open that reaches an image another thread is still loading, or one another thread's
// collection is retiring, waits until an open finishes or that collection frees or keeps the
// image, as Bionic's dlopen waits for the loader lock, and then looks again: true means it
// waited (until a broadcast or the wait timeout), and the image may be gone. False means it must
// not wait, and the lock was never dropped: the image is neither, it is this thread's own load or
// collection, this thread is inside a host call or a fork, the wait timeout has passed, or waiting
// would close a cycle of threads that wait for each other's opens or collections (a close waits
// for a collection: collect_after_close). The context and the caller, when one is named, are kept
// alive while the lock is dropped.
// since (optional) carries when the caller's wait began, across the rounds of its loop, so a wait
// timeout bounds the whole wait.
bool wait_for_load(const ImageState* image, Context* context, ImageState* caller,
                   uint64_t* since = nullptr);
// Wakes every waiting open; an open calls it when it finishes, whatever the outcome, and a
// collection when it frees or keeps an image an open may wait for.
void notify_load_waiters();
// How long wait_for_thread waits before it gives up (0: no bound), in nanoseconds.
extern uint64_t g_wait_timeout_ns;

// The threads that load a root open or wait for another thread, for load_waits: a fixed table
// each thread writes only its own slot of, under a sequence number (odd while it is written), so a
// reader copies it without any lock. A slot is claimed with a compare-and-swap of its tid.
constexpr size_t kWaitSlots = 64;
struct WaitSlot {
  uint32_t sequence;
  int32_t thread;
  uint32_t kind;
  int32_t owner;
  uint64_t generation;
  uint64_t namespace_id;
  uint64_t start_ns;
};
extern WaitSlot g_wait_slots[kWaitSlots];
// Loads and waits that found no free slot, counted while they last. Each is also listed by its
// thread's tid in an overflow record (calloc'd, under the operation lock, which every enter and
// leave holds), so a thread's exit or a fork child gives back exactly the ones of the threads that
// are gone; the count alone is what the lock-free reader reads.
extern uint32_t g_wait_overflow;
struct WaitOverflow {
  int32_t thread;
  WaitOverflow* next;
};
extern WaitOverflow* g_wait_overflows;

// Claims and fills a slot for this thread, or counts an overflow; returns the slot, -1 for an
// overflow, or -2 when not even that could be recorded (nothing to give back).
int wait_slot_enter(uint32_t kind, int32_t owner, uint64_t generation, uint64_t namespace_id,
                    uint64_t start_ns);
// Names the image a load slot is for, once its root exists.
void wait_slot_name(int slot, uint64_t generation, uint64_t namespace_id);
uint64_t monotonic_now_ns();
// Ends what wait_slot_enter began.
void wait_slot_leave(int slot);
// Frees every slot of thread (an exiting thread) and drops its overflow counts.
void wait_slots_forget(int32_t thread);
// In a fork child: frees every slot and overflow of a thread other than the forking one (forker,
// its tid in the parent), and gives the forking thread's own the child's tid.
void wait_slots_keep_forker(int32_t forker);
// The explicit retirement of every image of scope, RTLD_GLOBAL ones included, which no close
// unloads (kept_global): context_destroy, namespace_destroy and try_shutdown share it.
// YUKILINKER_C_BUSY, changing nothing, unless this is the only operation, no collection runs and
// nothing outside those images still needs one of them: an open, pin, callback, RTLD_NODELETE, an
// open loading it, or an image of another context reaching it. Past those checks scope is sealed
// for good (Retirement::kDestroying) and admits nothing new; YUKILINKER_C_OK once it holds no
// image, YUKILINKER_C_IO_ERROR when one of its own is left (a failed unmap), for the next call to
// resume. Code still running in those images, or pointers taken from them, are beyond what the
// runtime counts: the caller vouches that none is left.
uint32_t retire_scope_images(Context* scope);
// Whether another thread holds a TLS state, or this thread's holds a block or record.
bool other_thread_states();
// The runtime's retirement (ShutdownPhase). begin_shutdown checks, in one lock hold, everything
// that could refuse it (YUKILINKER_C_BUSY, nothing changed), retries what earlier failed munmaps
// left (YUKILINKER_C_IO_ERROR, nothing else changed), and crosses into kDestroying; YUKILINKER_C_OK
// once past it. finish_shutdown then retires the default context's images, releases its host
// imports and shuts down; false when a step stopped short, for the next call to resume.
// shutdown_unfinishable tells a stop no later call can resume.
uint32_t begin_shutdown();
bool finish_shutdown();
bool shutdown_unfinishable();
#endif  // #if YUKILINKER_FULL
SoHandle* dlopen_memfd_offset(int memfd, const char* name, bool file_backed, uint64_t source_offset,
                              const LoadPlacement* placement = nullptr,
                              const RelroOptions* relro = nullptr, bool by_path = false);
void* dlsym(SoHandle* handle, const char* name);
void dlclose(SoHandle* handle);
bool shutdown();
bool collect_images();
// The collection a close asks for once it has dropped its reference. When that left the image
// unreferenced while another thread's collection is under way, it waits for that collection where
// an open would wait for it (wait_for_load), and then collects on this thread.
bool collect_after_close(ImageState* image);
// The caller has validated the owner and holds an OperationGuard throughout.
// CallbackGuard still saves/releases/restores that admission around external code.
CloseStatus api_close_locked(void* handle, yukilinker_c_error_detail* error);
void* loader_create_context(const char* path, yukilinker_c_dependency_provider provider,
                            void* user);
int loader_destroy_context(void* opaque);
void* loader_open_fd(int fd, const LoadOptions* options, yukilinker_c_error_detail* output);
void* loader_open_path(const char* path, const LoadOptions* options,
                       yukilinker_c_error_detail* error);
LookupValue loader_lookup(void* handle, const char* name, const char* version);
CloseStatus loader_close(void* handle, yukilinker_c_error_detail* output);
void loader_prepare_fork();
void loader_after_fork(int child);
LoaderStats loader_query_stats();
int loader_release_host(void* handle);
int runtime_try_shutdown(yukilinker_c_error_detail* error);
#if YUKILINKER_FULL
// Deletes the private dlerror key once shutdown has retired the keys; frees this thread's record.
void retire_dl_error_key();
// Copies the calling thread's last refusal (yukilinker.h, host_import_refusal).
uint32_t api_host_import_refusal(yukilinker_c_host_import_refusal* refusal);
// Removes the runtime's atfork handlers and process-exit entries once shutdown has retired the
// keys. libc takes its atfork lock, which a fork holds while it waits for the operation lock, so
// the caller must not hold that.
void retire_fork_handlers();
// Removes every process-exit entry from libc (retirement or unload); none runs finalizers.
void remove_process_exit_entries();
#endif  // #if YUKILINKER_FULL

}  // namespace yukilinker::internal
