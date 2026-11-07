// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <limits>

#include "../internal/caller_stubs.hpp"
#include "../internal/host_imports.hpp"
#include "../internal/registry.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

namespace {

// Drops every edge and load root that names an image about to be freed. Collection frees an image
// only once nothing it keeps reaches it, so the edges cleared there belong to images that retire in
// the same pass. The failure sweep can still free an image that a kept member of its group needs:
// that edge becomes unresolved and the scope falls back to the image itself, and its relocated
// pointers into the freed image are not repaired.
void forget_image(const ImageState* image) {
  for (auto* other = g_first_image; other; other = other->next) {
    if (other->load_root == image) other->load_root = other;
    for (auto* dep = other->dependencies; dep; dep = dep->next)
      if (dep->image == image) dep->image = nullptr;
  }
}

// No image's load root has this generation: generations stay far below it.
constexpr uint64_t kNoGroup = UINT64_MAX;

// A generation fits a pointer (a process-exit entry's argument) and stays far below kNoGroup.
constexpr uint64_t kMaxImageGeneration = (UINTPTR_MAX - 2) / 4 - 1;

#if YUKILINKER_FULL
// As Bionic's generate_handle: random, and never a registered image's or a pseudo-handle. The low
// bits stay 2, apart from aligned soinfo pointers and odd Bionic handles. Taken in the lock hold
// that registers the image, so no other image can take it meanwhile. 0 when the entropy source
// keeps repeating itself.
uintptr_t fresh_token() {
  return random_token<uintptr_t>(2, 3, [](uintptr_t value) {
    if (value == reinterpret_cast<uintptr_t>(RTLD_NEXT) ||
        value == reinterpret_cast<uintptr_t>(RTLD_DEFAULT))
      return true;
    return g_image_index.find(value) != nullptr;
  });
}
#endif  // #if YUKILINKER_FULL

bool in_group(const ImageState* image, uint64_t first_generation) {
  return image->load_root->generation == first_generation;
}

bool graph_ready(ImageState* image, uint64_t first_generation, uint64_t visit) {
  if (image->graph_visit == visit) return true;
  image->graph_visit = visit;
  if (image->phase == Phase::Preparing && !in_group(image, first_generation)) return false;
  // initialize_graph does not descend into these either.
  if (image->phase == Phase::Ready || image->phase == Phase::Initializing) return true;
  for (auto* dep = image->dependencies; dep; dep = dep->next)
    if (dep->image && !graph_ready(dep->image, first_generation, visit)) return false;
  return true;
}

// A callback can reach an image that another open of the same thread is still preparing. It is
// not relocated yet, and the constructors run over everything the root reaches, so such an image
// stops the open before any resolver or constructor runs.
bool graph_ready(ImageState* root, uint64_t first_generation) {
  return graph_ready(root, first_generation, ++g_graph_visit);
}

// An open that reuses a registered image runs the constructors it has not run yet, so every
// image it would initialize is checked first, before any of their code runs: none may still be
// preparing or have failed, and the slots a resolver wrote must hold code.
bool reused_graph_valid(ImageState* image, uint64_t visit) {
  if (image->graph_visit == visit) return true;
  image->graph_visit = visit;
  if (image->phase == Phase::Ready || image->phase == Phase::Initializing) return true;
  if (image->phase == Phase::Preparing) return fail(YUKILINKER_C_BUSY, YUKILINKER_C_INITIALIZATION);
  if (image->phase == Phase::Failed) return fail(image->failure_code, image->failure_stage);
  if (!validate_lifecycle(image, false))
    return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_INITIALIZATION);
  for (auto* dep = image->dependencies; dep; dep = dep->next)
    if (dep->image && !reused_graph_valid(dep->image, visit)) return false;
  return true;
}

// Without a reported code the stage is unknown. errno is kept only where a code is derived from
// it.
void record_failure(ImageState* image) {
  const yukilinker_c_error_detail* error =
      g_operation_error && g_operation_error->code != YUKILINKER_C_OK ? g_operation_error : nullptr;
  image->failure_code = error ? error->code : YUKILINKER_C_UNSUPPORTED;
  image->failure_stage = error ? error->stage : 0;
  image->failure_stage_known = error != nullptr;
  image->failure_errno = error && errno_derived(error->code) ? error->system_errno : 0;
}

}  // namespace

// Everything outside the registry that names an image: an observer, the thread that is loading
// it, or a TLS block or thread destructor that was taken for it. Collection and the failure
// sweep both have to see all of them before the image may be freed.
bool image_is_pinned(const ImageState* image) {
  return image->snapshot_refs || image->lease_refs || image->active_calls || image->tls_users ||
         image->thread_destructors;
}

bool free_image_state(ImageState* image) {
#if YUKILINKER_FULL
  if (image->token != 0) {
    g_image_index.erase(image->token);
    g_generation_index.erase(image->generation);
  }
#endif  // #if YUKILINKER_FULL
  const bool released = release_metadata(image->memory.metadata);
  // Names the image it was for, which is freed below; the page it left is shutdown's to retry.
  if (!released) record_event(image, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR);
  free_object(image);
  return released;
}

void release_failed_image(ImageState* image) {
  if (image == nullptr) return;
  unregister_hwasan_globals(image);
  discard_deferred_relocations(image);
  deactivate_tls(image);
  // The image is freed without waiting for its users, so every thread's block for it is
  // detached: the block outlives the slot, but it must not name a freed image.
  detach_image_tls_blocks(image);
  // Drops every edge to this image before the calls below drop the lock, so no lookup can walk
  // into it; an unregistered image gains no new edge.
  forget_image(image);
  // Even a failed, unpublished load needs a persistent owner while a host
  // close can reenter or end its thread. Keep it in the registry as a dead copy.
  bool system_dependencies = false;
  for (auto* dependency = image->dependencies; dependency; dependency = dependency->next)
    system_dependencies |= dependency->system_handle != nullptr;
  if (system_dependencies && !image->linked) {
    image->references = image->pending_references = 0;
    image->load_root = image;
    image->phase = Phase::Closing;
    image->releasing = true;
    (void)commit_page_size();
    register_image(image);
  }
  if (!close_dependencies(image)) {
    image->references = image->pending_references = 0;
    image->load_root = image;
    image->phase = Phase::Closing;
    image->releasing = false;
    return;
  }
  if (image->memory.reservation != nullptr) {
    const bool released = image->memory.caller_reserved
                              ? mmap(image->memory.reservation, image->memory.span, PROT_NONE,
                                     MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED
                              : munmap(image->memory.reservation, image->memory.span) == 0;
    if (!released) {
      image->references = 0;
      image->pending_references = 0;
      image->load_root = image;
      image->phase = Phase::Closing;
      // A dead copy now, which a later open skips instead of refusing.
      image->releasing = false;
      // It was laid out with the page size, which stays fixed while it is registered.
      if (!image->linked) {
        (void)commit_page_size();
        register_image(image);
      }
      if (g_operation_error) g_operation_error->cleanup_incomplete = 1;
      record_event(image, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR);
      image->cleanup_reported = true;
      return;
    }
    image->memory.reservation = nullptr;
  }
#if defined(YUKILINKER_RUNTIME)
  // Every caller of this ran no code of the image's group: a stub address went only into the
  // image's own relocated words, unmapped just now, so nothing can call the page any more.
  discard_unpublished_caller_stubs(image);
#endif
  if (image->public_handle != nullptr) {
    image->public_handle->load_bias = nullptr;
    image->public_handle->map_size = 0;
    image->public_handle->private_state = nullptr;
  }
  unregister_image(image);
  (void)free_image_state(image);
}

namespace {

// The target of /proc/self/fd/<fd> when it is a whole absolute path of a file that still exists.
bool name_by_descriptor(int fd, char* name, size_t capacity) {
  char link[32];
  (void)snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  const ssize_t length = readlink(link, name, capacity - 1);
  if (length <= 0 || static_cast<size_t>(length) >= capacity - 1) return false;
  name[length] = '\0';
  static constexpr char kDeleted[] = " (deleted)";
  constexpr size_t kSuffix = sizeof(kDeleted) - 1;
  return name[0] == '/' &&
         (static_cast<size_t>(length) < kSuffix || strcmp(name + length - kSuffix, kDeleted) != 0);
}

}  // namespace

SoHandle* load_image(int memfd, const char* vma_name, bool file_backed, uint64_t source_offset,
                     const LoadPlacement* placement, uint64_t* group, bool by_path) {
  OperationGuard operation;
  if (exit_cleanup_uncertain()) {
    fail(YUKILINKER_C_BUSY, YUKILINKER_C_RETIREMENT);
    return nullptr;
  }
#if defined(YUKILINKER_CFI)
  // Bionic registers only system-loaded mappings with cross-DSO CFI. This build
  // variant must not execute private constructors or hand out unregistered code.
  fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_SOURCE, "CFI runtime requires SYSTEM");
  return nullptr;
#endif
  struct stat status{};
  // fstat refuses a negative descriptor too, with EBADF, so the error carries a fresh errno.
  if (fstat(memfd, &status) != 0) {
    fail(YUKILINKER_C_IO_ERROR, YUKILINKER_C_SOURCE);
    return nullptr;
  }
  const uint64_t total_size = static_cast<uint64_t>(status.st_size);
  // Only a regular file (a memfd is one): a directory, FIFO or device is no image, whatever its
  // st_size, and a refusal here carries no errno. No image was loaded from one, so under
  // RTLD_NOLOAD it is the NOLOAD miss below.
  if (!S_ISREG(status.st_mode) && g_open_options && (g_open_options->flags & RTLD_NOLOAD)) {
    fail(YUKILINKER_C_DEPENDENCY_MISSING, YUKILINKER_C_SOURCE);
    return nullptr;
  }
  if (!S_ISREG(status.st_mode) || status.st_size <= 0 || source_offset >= total_size ||
      source_offset > INT64_MAX || (source_offset % loader_page_size()) != 0 ||
      total_size - source_offset > SIZE_MAX) {
    fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_SOURCE);
    return nullptr;
  }
  if (g_next_image_generation > kMaxImageGeneration ||
      (vma_name && strlen(vma_name) >= sizeof(ImageState::origin))) {
    fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_SOURCE);
    return nullptr;
  }
  Context* context = g_loading_parent ? g_loading_parent->context
                                      : (g_open_options && g_open_options->context
                                             ? context_from_opaque(g_open_options->context)
                                             : &g_default_context);
  const bool deduplicate = !g_open_options || !g_open_options->force_load || g_loading_parent;
  ImageState* existing = nullptr;
  // When this open's wait for another thread began, across its rounds.
  uint64_t waited_since = 0;
  for (;;) {
    // A context under retirement admits nothing, not even a reference to what it still holds; a
    // wait below drops the lock, so this is checked again each time round.
    if (context_sealed(context)) {
      fail(YUKILINKER_C_BUSY, YUKILINKER_C_RETIREMENT);
      return nullptr;
    }
    // A Closing match counts only after every other match: the collection under way may still
    // retire it. A dead copy is no match at all; the open loads a fresh copy beside it. A Failed
    // match counts only when no live copy matches: a force_load may have loaded one since.
    ImageState* closing = nullptr;
    ImageState* failed = nullptr;
    for (auto* image = g_first_image; deduplicate && image && !existing; image = image->next) {
      if (image->context != context || image->device != status.st_dev ||
          image->inode != status.st_ino || image->source_offset != source_offset ||
          dead_copy(image))
        continue;
      if (image->phase == Phase::Closing) {
        if (!closing) closing = image;
      } else if (image->phase == Phase::Failed) {
        if (!failed) failed = image;
      } else {
        existing = image;
      }
    }
    if (!existing) existing = failed;
    if (!existing) existing = closing;
#if YUKILINKER_FULL
    if (existing && !accessible_image(existing) &&
        wait_for_load(existing, context, nullptr, &waited_since)) {
      existing = nullptr;
      continue;
    }
#endif  // #if YUKILINKER_FULL
    break;
  }
  if (existing) {
    if (!accessible_image(existing)) {
      // A failed image stays registered only while something keeps it; its open failed for good,
      // so a reopen gets the recorded failure, not BUSY as if it were still loading.
      if (existing->phase == Phase::Failed && existing->failure_stage_known)
        fail(existing->failure_code, existing->failure_stage);
      else
        fail(YUKILINKER_C_BUSY, YUKILINKER_C_INITIALIZATION);
      return nullptr;
    }
    // Only its own group may share an image that is still preparing; a root open must not reach
    // one at all. No image of the new open exists yet, so it has no group.
    if (g_loading_parent ? existing->phase == Phase::Preparing &&
                               existing->load_root != g_loading_parent->load_root
                         : !graph_ready(existing, g_next_image_generation)) {
      fail(YUKILINKER_C_BUSY, YUKILINKER_C_INITIALIZATION);
      return nullptr;
    }
    // A root open's reference stays pending until dlopen_memfd_offset returns the handle.
    if (!g_loading_parent) {
      ++existing->references;
      ++existing->pending_references;
    }
    // A dependency outside this open's group gets the flags once the whole group has loaded, and
    // a root open that reuses an image in dlopen_memfd_offset, which takes them back if it fails.
    const bool outside_group =
        !g_loading_parent || existing->load_root != g_loading_parent->load_root;
    if (g_open_options && !outside_group) {
      existing->flags |= g_open_options->flags & (RTLD_GLOBAL | RTLD_NODELETE);
      if (g_open_options->flags & RTLD_NODELETE)
        existing->nodelete_reasons |= YUKILINKER_C_NODELETE_REQUEST;
    }
    return existing->public_handle;
  }
  // As Bionic: a NOLOAD miss is an error of its own, not a bad ELF.
  if (g_open_options && (g_open_options->flags & RTLD_NOLOAD)) {
    fail(YUKILINKER_C_DEPENDENCY_MISSING, YUKILINKER_C_SOURCE);
    return nullptr;
  }
  const uint32_t load_depth = g_loading_parent ? g_loading_parent->load_depth + 1 : 0;
  if (load_depth > kMaxLoadDepth) {
    fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_DEPENDENCIES, vma_name);
    return nullptr;
  }
  size_t file_size = static_cast<size_t>(total_size - source_offset);
  SourceMapping source_mapping(memfd, file_size, source_offset);
  void* source = source_mapping.get();
  if (!source) {
    // O_PATH and write-only descriptors, directories and the like: the errno tells them apart.
    fail_errno(YUKILINKER_C_SOURCE, source_mapping.error);
    return nullptr;
  }

  SourceLayout layout;
  if (!inspect_source(source, file_size, &layout)) {
    fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_METADATA);
    return nullptr;
  }

  ImageState* image = allocate_object<ImageState>();
  if (image == nullptr) {
    fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_METADATA);
    return nullptr;
  }
  SoHandle* handle = image->public_handle;
  handle->private_state = image;
  image->memory.metadata = initialize_inline_metadata(image->memory.metadata_storage);
  image->context = context;
#if YUKILINKER_FULL
  image->loading_thread = pthread_self();
#endif
  image->device = status.st_dev;
  image->inode = status.st_ino;
  image->source_offset = source_offset;
  image->generation = g_next_image_generation++;
  if (group && !g_loading_parent) *group = image->generation;
  image->namespace_id = context->namespace_id;
  image->load_depth = load_depth;
  image->references = g_loading_parent ? 0 : 1;
  image->pending_references = image->references;
  image->load_root = g_loading_parent ? g_loading_parent->load_root : image;
  image->flags = g_open_options ? g_open_options->flags : RTLD_NOW;
  if (image->flags & RTLD_NODELETE) image->nodelete_reasons |= YUKILINKER_C_NODELETE_REQUEST;
  image->host = g_open_options && g_open_options->trusted_runtime;
  // Deduplication stays by device and inode; only the name follows the file. Without /proc the
  // image keeps the name it was opened by.
  if (!by_path || !name_by_descriptor(memfd, image->origin, sizeof(image->origin))) {
    if (vma_name) (void)snprintf(image->origin, sizeof(image->origin), "%s", vma_name);
  }

  if (!create_address_space(memfd, static_cast<const uint8_t*>(source), file_size, layout,
                            file_backed, source_offset, &image->memory, placement)) {
    // Every failure there recorded its own code; this one is only for a path that did not.
    fail_no_errno(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION);
    release_failed_image(image);
    return nullptr;
  }
  handle->load_bias = image->memory.bias;
  handle->map_size = image->memory.span;

  DynamicParse dynamic;
  bool metadata_ready =
      discover_tls_segment(image) && parse_dynamic_tags(image, layout.dynamic_index, &dynamic);
  if (metadata_ready && !validate_init_fini(image)) {
    fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_INITIALIZATION);
    release_failed_image(image);
    return nullptr;
  }
  metadata_ready =
      metadata_ready && establish_symbol_count(image, dynamic) && validate_versions(image);
  if (metadata_ready && dynamic.runpath_offset != SIZE_MAX) {
    image->runpath = string_at(image, dynamic.runpath_offset);
    metadata_ready = image->runpath != nullptr;
  }
  if (metadata_ready && dynamic.soname_offset != SIZE_MAX) {
    image->soname = string_at(image, dynamic.soname_offset);
    metadata_ready = image->soname != nullptr;
  }
  // A descriptor opened with no name (an unnamed memfd, open_fd without an origin) would leave the
  // name empty, which dl_iterate_phdr and dladdr report and their callers take for the main
  // program. Its soname stands in, or a name made of its generation. Neither starts with '/', so
  // neither becomes a directory to search for dependencies or to expand $ORIGIN to.
  if (image->origin[0] == '\0') {
    if (image->soname && image->soname[0] != '/' && strlen(image->soname) < sizeof(image->origin))
      (void)snprintf(image->origin, sizeof(image->origin), "%s", image->soname);
    else
      (void)snprintf(image->origin, sizeof(image->origin), "[yukilinker:%llu]",
                     static_cast<unsigned long long>(image->generation));
  }
  const char* system_symbol =
      metadata_ready && !image->host ? requires_system_runtime(image) : nullptr;
  if (system_symbol) {
    // fail copies the name before the image's strings are unmapped.
    fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA, system_symbol);
    release_failed_image(image);
    return nullptr;
  }
  if (metadata_ready) {
    if (!validate_hwasan_globals(image)) {
      release_failed_image(image);
      return nullptr;
    }
    // The tables are checked before TLS is activated or any dependency, private or system, is
    // opened. Pass -1 reads only the image itself and never drops the lock.
    const int pass = g_relocation_pass;
    g_relocation_pass = -1;
    const bool relocatable = relocate_image(image);
    g_relocation_pass = pass;
    if (!relocatable) {
      fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS);
      release_failed_image(image);
      return nullptr;
    }
  }
#if defined(YUKILINKER_RUNTIME)
  // Before any relocation binds: an image of a managed namespace gets its caller stubs, or the open
  // fails here, before anything of it ran.
  if (metadata_ready && !make_caller_stubs(image)) {
    release_failed_image(image);
    return nullptr;
  }
#endif
  if (!metadata_ready || !activate_tls(image)) {
    fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_METADATA);
    release_failed_image(image);
    return nullptr;
  }
#if YUKILINKER_FULL
  image->token = fresh_token();
  // Indexed before it is registered: register_image cannot fail, and unregister_image erases it.
  const bool indexed = image->token != 0 && g_image_index.insert(image->token, image);
  if (!indexed || !g_generation_index.insert(image->generation, image)) {
    if (indexed) g_image_index.erase(image->token);
    image->token = 0;
    fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_SOURCE);
    release_failed_image(image);
    return nullptr;
  }
#endif
  // The first image registered fixes the page size it was laid out with. Nothing since the layout
  // dropped the lock, so the value is the one inspect_source and create_address_space read.
  (void)commit_page_size();
  register_image(image);
  if (!source_mapping.reset()) return nullptr;
  if (!open_dependencies(image, dynamic)) return nullptr;
  if (!verneed_files_resolved(image)) return nullptr;
  return handle;
}

bool initialize_graph(ImageState* image) {
  if (image->phase == Phase::Ready || image->phase == Phase::Initializing) return true;
  // A nested open failed to initialize it; the open that loaded it fails as well.
  if (image->phase == Phase::Failed) return fail(image->failure_code, image->failure_stage);
  image->phase = Phase::Initializing;
  record_event(image, YUKILINKER_C_EVENT_CONSTRUCTOR);
  bool initialized = true;
  for (auto* dep = image->dependencies; initialized && dep; dep = dep->next)
    initialized = !dep->image || initialize_graph(dep->image);
  if (initialized) {
    image->executed = true;
    if (g_operation_error) g_operation_error->execution_started = 1;
    initialized = run_initializers(image);
  }
  if (!initialized) {
    // Left Initializing, every later walk would take it for done.
    image->phase = Phase::Failed;
    record_failure(image);
    return false;
  }
  image->phase = Phase::Ready;
  record_event(image, YUKILINKER_C_EVENT_READY);
  image->initialization_order = ++g_initialization_order;
  return true;
}

AddedOpenFlags add_open_flags(ImageState* image, uint32_t requested) {
  AddedOpenFlags added;
  added.flags = requested & (RTLD_GLOBAL | RTLD_NODELETE) & ~image->flags;
  added.nodelete_request =
      (requested & RTLD_NODELETE) && !(image->nodelete_reasons & YUKILINKER_C_NODELETE_REQUEST);
  image->flags |= added.flags;
  if (added.nodelete_request) image->nodelete_reasons |= YUKILINKER_C_NODELETE_REQUEST;
  return added;
}

void remove_open_flags(ImageState* image, AddedOpenFlags added) {
  image->flags &= ~added.flags;
  if (added.nodelete_request) image->nodelete_reasons &= ~YUKILINKER_C_NODELETE_REQUEST;
}

bool initialize_reused(ImageState* image) {
  if (reused_graph_valid(image, ++g_graph_visit) && initialize_graph(image)) return true;
  revoke_reference(image);
  return false;
}

#if YUKILINKER_FULL
namespace {

// True while this thread runs inside a call to the host, a frame without an image: the system
// linker may hold its own lock around it (a dl_iterate_phdr callback, a system constructor), and
// the open or collection this thread would wait for may need that lock to finish.
bool inside_host_call() {
  for (auto* frame = static_cast<const CallbackGuard*>(pthread_getspecific(g_callback_key)); frame;
       frame = frame->previous)
    if (!frame->caller_call && frame->image == nullptr) return true;
  return false;
}

// The monotonic clock in nanoseconds, or 0 when it cannot be read (errno says why).
uint64_t monotonic_ns() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000000000U + static_cast<uint64_t>(now.tv_nsec);
}

// A wait's *since once a bounded round ended other than by a wakeup: the deadline passed, or the
// wait failed. The next round is refused before the lock drops, so a wait that reports a timeout
// or a failure at once cannot turn into a loop.
constexpr uint64_t kWaitAbandoned = UINT64_MAX;

// A bounded wait that cannot read the clock cannot tell when its deadline passes: refused, with
// the clock's errno as the operation's failure (IO_ERROR at INITIALIZATION, where an open reports
// a wait it may not make), which the caller's own refusal then does not replace.
bool refuse_without_clock() {
  fail_errno(YUKILINKER_C_INITIALIZATION, errno);
  return false;
}

// Waits until the next broadcast for what another thread (first) is doing: its open, or its
// collection. False, without waiting and without dropping the lock, when this thread must not
// wait: see wait_for_load. With a wait timeout (g_wait_timeout_ns), false too once the deadline
// is past or after a round whose bounded wait timed out or failed: the caller treats it as a wait
// it may not make (YUKILINKER_C_BUSY for an open); and when the clock cannot be read, recording
// YUKILINKER_C_IO_ERROR with the clock's errno first. True whenever the lock was dropped, a
// timed-out or failed wait included: what the caller read before may be gone, so it looks again,
// and its next round is refused before the lock drops. A caller that waits again passes the start
// of its first round in *since (0 before it), so the bound covers the whole wait, not each round.
// The thread waited for is not touched. awaited is the image an open waits for, for load_waits;
// null for a collection.
bool wait_for_thread(pthread_t first, Context* context, ImageState* caller,
                     const ImageState* awaited = nullptr, uint64_t* since = nullptr) {
  const pthread_t self = pthread_self();
  const uintptr_t depth = operation_depth();
  // Without the keys an operation keeps the lock it holds and cannot hand it over; a fork holds
  // the runtime's other locks, which the operation waited for needs.
  if (!g_operation_depth_valid || depth == 0 || g_fork_locked || inside_host_call()) return false;
  // The waiters form chains, never cycles: one that would end at this thread is refused. A close
  // waiting for a collection is a link too, so a collection that waits for this thread's open
  // is never waited for in turn.
  for (pthread_t owner = first;;) {
    if (pthread_equal(owner, self)) return false;
    const LoadWaiter* waiter = g_load_waiters;
    while (waiter && !pthread_equal(waiter->thread, owner)) waiter = waiter->next;
    if (!waiter) break;
    owner = waiter->owner;
  }
  uint64_t timeout = g_wait_timeout_ns;
  if (since && *since == kWaitAbandoned) return false;
  uint64_t start = since && *since ? *since : monotonic_ns();
  // A bound needs the clock: without it the deadline could never be found to pass.
  if (start == 0 && timeout != 0) return refuse_without_clock();
  if (since) *since = start;
  timespec deadline{};
  if (timeout != 0) {
    uint64_t end;
    if (__builtin_add_overflow(start, timeout, &end)) end = UINT64_MAX;
    const uint64_t now = monotonic_ns();
    if (now == 0) return refuse_without_clock();
    if (now >= end) return false;
    // A deadline past what time_t holds (32 bits on armeabi-v7a) is no bound at all: waited for
    // without one rather than wrapped into the past.
    if (end / 1000000000U > static_cast<uint64_t>(std::numeric_limits<time_t>::max())) {
      timeout = 0;
    } else {
      deadline.tv_sec = static_cast<time_t>(end / 1000000000U);
      deadline.tv_nsec = static_cast<long>(end % 1000000000U);
    }
  }
  // Listed for load_waits while it waits: the slot carries when the whole wait began.
  const int slot = wait_slot_enter(YUKILINKER_C_LOAD_WAIT_WAIT, pthread_gettid_np(first),
                                   awaited ? awaited->generation : 0,
                                   awaited ? awaited->namespace_id : 0, start);
  LoadWaiter waiter{self, first, context, g_load_waiters, caller};
  g_load_waiters = &waiter;
  if (context) ++context->waiting_opens;
  if (caller) ++caller->active_calls;
  HeldOperation held;
  held.put_aside();
  // The recursive lock is held depth times; the wait releases the last one.
  for (uintptr_t i = 1; i < depth; ++i) pthread_mutex_unlock(&g_operation_mutex);
  // After a wakeup, spurious or not, the caller looks again and, if it still has to, waits again
  // against the same deadline. A bounded wait that timed out or failed ends the bound: the caller
  // looks once more, and its next round is refused, whether the deadline really passed or the wait
  // reported so at once.
  const int waited = timeout != 0 ? pthread_cond_clockwait(&g_load_wait_cond, &g_operation_mutex,
                                                           CLOCK_MONOTONIC, &deadline)
                                  : pthread_cond_wait(&g_load_wait_cond, &g_operation_mutex);
  if (waited != 0 && since) *since = kWaitAbandoned;
  for (uintptr_t i = 1; i < depth; ++i) pthread_mutex_lock(&g_operation_mutex);
  held.take_back();
  if (caller) --caller->active_calls;
  if (context) --context->waiting_opens;
  LoadWaiter** link = &g_load_waiters;
  while (*link != &waiter) link = &(*link)->next;
  *link = waiter.next;
  wait_slot_leave(slot);
  return true;
}

}  // namespace

int wait_slot_enter(uint32_t kind, int32_t owner, uint64_t generation, uint64_t namespace_id,
                    uint64_t start_ns) {
  const int32_t tid = gettid();
  for (size_t i = 0; i < kWaitSlots; ++i) {
    WaitSlot& slot = g_wait_slots[i];
    int32_t free_tid = 0;
    if (!__atomic_compare_exchange_n(&slot.thread, &free_tid, tid, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED))
      continue;
    // Odd while written: a reader that sees an odd or changed sequence retries.
    __atomic_add_fetch(&slot.sequence, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&slot.kind, kind, __ATOMIC_RELAXED);
    __atomic_store_n(&slot.owner, owner, __ATOMIC_RELAXED);
    __atomic_store_n(&slot.generation, generation, __ATOMIC_RELAXED);
    __atomic_store_n(&slot.namespace_id, namespace_id, __ATOMIC_RELAXED);
    __atomic_store_n(&slot.start_ns, start_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&slot.sequence, 1, __ATOMIC_ACQ_REL);
    return static_cast<int>(i);
  }
  // No slot: counted, and listed by thread when a record can be had. One that cannot be is not
  // counted either: the reader then misses that thread, never reports one that is gone.
  auto* overflow = static_cast<WaitOverflow*>(calloc(1, sizeof(WaitOverflow)));
  if (!overflow) return -2;
  overflow->thread = tid;
  overflow->next = g_wait_overflows;
  g_wait_overflows = overflow;
  __atomic_add_fetch(&g_wait_overflow, 1U, __ATOMIC_ACQ_REL);
  return -1;
}

namespace {

// Gives back one overflow of thread, if it still has one (a thread exit may have taken it).
void leave_overflow(int32_t thread) {
  for (WaitOverflow** link = &g_wait_overflows; *link; link = &(*link)->next) {
    if ((*link)->thread != thread) continue;
    WaitOverflow* gone = *link;
    *link = gone->next;
    free(gone);
    __atomic_sub_fetch(&g_wait_overflow, 1U, __ATOMIC_ACQ_REL);
    return;
  }
}

}  // namespace

void wait_slot_leave(int slot) {
  if (slot == -2) return;
  if (slot < 0) {
    leave_overflow(gettid());
    return;
  }
  WaitSlot& entry = g_wait_slots[slot];
  __atomic_add_fetch(&entry.sequence, 1, __ATOMIC_ACQ_REL);
  __atomic_store_n(&entry.kind, 0U, __ATOMIC_RELAXED);
  __atomic_add_fetch(&entry.sequence, 1, __ATOMIC_ACQ_REL);
  __atomic_store_n(&entry.thread, 0, __ATOMIC_RELEASE);
}

namespace {

void forget_slots(int32_t thread, bool all_others) {
  for (size_t i = 0; i < kWaitSlots; ++i) {
    const int32_t holder = __atomic_load_n(&g_wait_slots[i].thread, __ATOMIC_ACQUIRE);
    if (holder != 0 && (all_others ? holder != thread : holder == thread))
      wait_slot_leave(static_cast<int>(i));
  }
  for (WaitOverflow** link = &g_wait_overflows; *link;) {
    WaitOverflow* entry = *link;
    if (all_others ? entry->thread == thread : entry->thread != thread) {
      link = &entry->next;
      continue;
    }
    *link = entry->next;
    free(entry);
    __atomic_sub_fetch(&g_wait_overflow, 1U, __ATOMIC_ACQ_REL);
  }
}

}  // namespace

void wait_slots_forget(int32_t thread) { forget_slots(thread, false); }

void wait_slots_keep_forker(int32_t forker) {
  forget_slots(forker, true);
  // The forking thread's slots and overflows were entered with its tid in the parent; its own
  // leave, here, looks them up by the child's.
  const int32_t self = gettid();
  for (size_t i = 0; i < kWaitSlots; ++i) {
    WaitSlot& slot = g_wait_slots[i];
    if (__atomic_load_n(&slot.thread, __ATOMIC_ACQUIRE) != forker) continue;
    __atomic_add_fetch(&slot.sequence, 1, __ATOMIC_ACQ_REL);
    // A load names its own thread as the owner.
    if (__atomic_load_n(&slot.owner, __ATOMIC_RELAXED) == forker)
      __atomic_store_n(&slot.owner, self, __ATOMIC_RELAXED);
    __atomic_store_n(&slot.thread, self, __ATOMIC_RELEASE);
    __atomic_add_fetch(&slot.sequence, 1, __ATOMIC_ACQ_REL);
  }
  for (auto* entry = g_wait_overflows; entry; entry = entry->next)
    if (entry->thread == forker) entry->thread = self;
}

uint64_t monotonic_now_ns() { return monotonic_ns(); }

void wait_slot_name(int slot, uint64_t generation, uint64_t namespace_id) {
  if (slot < 0) return;
  WaitSlot& entry = g_wait_slots[slot];
  __atomic_add_fetch(&entry.sequence, 1, __ATOMIC_ACQ_REL);
  __atomic_store_n(&entry.generation, generation, __ATOMIC_RELAXED);
  __atomic_store_n(&entry.namespace_id, namespace_id, __ATOMIC_RELAXED);
  __atomic_add_fetch(&entry.sequence, 1, __ATOMIC_ACQ_REL);
}

namespace {}  // namespace

bool wait_for_load(const ImageState* image, Context* context, ImageState* caller, uint64_t* since) {
  if (!awaited_image(image)) return false;
  // A loading image is waited for on the thread loading it, a Closing one on the collecting one.
  const bool loading = image_loading(image);
  return wait_for_thread(loading ? image->loading_thread : g_collecting_thread, context, caller,
                         loading ? image : nullptr, since);
}

void notify_load_waiters() { (void)pthread_cond_broadcast(&g_load_wait_cond); }
#endif  // #if YUKILINKER_FULL

namespace {

// Runs relocation pass 0 or 1 over every image the open added.
bool relocate_group(uint64_t first_generation, int pass) {
  g_relocation_pass = pass;
  for (auto* image = g_first_image; image; image = image->next) {
    if (!in_group(image, first_generation)) continue;
    if (!relocate_image(image)) return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS);
  }
  return true;
}

// Pass 2 runs the resolvers, and a resolver may call into the images its image needs, so an image
// is replayed only after every image it needs; inside a cycle the walk decides. Resolvers drop the
// lock and an edge can be cleared meanwhile, so each edge is read only when the walk reaches it.
bool replay_image(ImageState* image, uint64_t first_generation) {
  if (!in_group(image, first_generation) || image->pass2_started) return true;
  image->pass2_started = true;
  for (auto* dep = image->dependencies; dep; dep = dep->next)
    if (dep->image && !replay_image(dep->image, first_generation)) return false;
  if (!relocate_image(image) || !protect_relro(image))
    return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION);
  image->phase = Phase::Relocated;
  record_event(image, YUKILINKER_C_EVENT_RELOCATE);
  return true;
}

bool replay_group(ImageState* root, uint64_t first_generation) {
  // Ordinary relocations can alter note bytes; validate the whole group again
  // before any hook publishes shadow or any resolver can execute.
  for (auto* image = g_first_image; image; image = image->next)
    if (in_group(image, first_generation) && !validate_hwasan_globals(image)) return false;
  // An IFUNC can call into any member of its group, including a dependency cycle.
  for (auto* image = g_first_image; image; image = image->next)
    if (in_group(image, first_generation) && !register_hwasan_globals(image)) return false;
  g_relocation_pass = 2;
  if (!replay_image(root, first_generation)) return false;
  // Images the root no longer reaches follow in registration order.
  for (auto* image = g_first_image; image; image = image->next)
    if (!replay_image(image, first_generation)) return false;
  return true;
}

bool validate_group_lifecycle(uint64_t first_generation, bool skip_deferred) {
  for (auto* image = g_first_image; image; image = image->next)
    if (in_group(image, first_generation) && !validate_lifecycle(image, skip_deferred))
      return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_INITIALIZATION);
  return true;
}

// The open's own flags, never the root's DF_1 bits, reach the dependencies it reused.
void apply_dependency_flags(uint64_t first_generation) {
  const uint32_t flags =
      g_open_options ? g_open_options->flags & (RTLD_GLOBAL | RTLD_NODELETE) : 0U;
  if (flags == 0) return;
  for (auto* image = g_first_image; image; image = image->next) {
    if (!in_group(image, first_generation)) continue;
    for (auto* dep = image->dependencies; dep; dep = dep->next) {
      if (!dep->apply_open_flags || !dep->image) continue;
      dep->image->flags |= flags;
      if (flags & RTLD_NODELETE) dep->image->nodelete_reasons |= YUKILINKER_C_NODELETE_REQUEST;
    }
  }
}

// A nested open can fail to initialize a member after the root's walk passed it, or one the walk
// never reaches again because what needs it is READY already. The open of the group fails with
// that member's error then, and keeps the group as for any failure after code ran.
bool group_initialized(uint64_t first_generation) {
  for (auto* image = g_first_image; image; image = image->next)
    if (in_group(image, first_generation) && image->phase == Phase::Failed)
      return fail(image->failure_code, image->failure_stage);
  return true;
}

}  // namespace

SoHandle* dlopen_memfd_offset(int memfd, const char* name, bool file_backed, uint64_t source_offset,
                              const LoadPlacement* placement, const RelroOptions* relro,
                              bool by_path) {
  OperationGuard operation;
#if YUKILINKER_FULL
  // The initializers need this key. Without it the open fails here, before any of its code has
  // run, instead of at the initializers, where the group would stay pinned for having run.
  if (!ensure_initializing_key()) {
    if (g_keys_retired)
      fail(YUKILINKER_C_BUSY, YUKILINKER_C_RETIREMENT);
    else
      fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_SOURCE);
    return nullptr;
  }
  // Whatever its outcome, a thread waiting for this open looks again once it is done.
  struct Finished {
    ~Finished() { notify_load_waiters(); }
  } finished;
  // Listed for load_waits while it loads: a root open, on this thread, until it returns. The
  // root's generation is known only once load_image creates it, so the slot names none yet.
  struct Loading {
    int slot = -2;
    Loading() {
      if (!g_loading_parent)
        slot = wait_slot_enter(YUKILINKER_C_LOAD_WAIT_LOAD, gettid(), 0, 0, monotonic_now_ns());
    }
    ~Loading() { wait_slot_leave(slot); }
    Loading(const Loading&) = delete;
    Loading& operator=(const Loading&) = delete;
  } loading;
#endif  // #if YUKILINKER_FULL
  // The group is known once load_image creates its root. Until then the lock can drop (a wait, a
  // log callback) and another open can take the next generation, so it is not read up front.
  // A root that deduplicated, or a load that failed before creating one, has no group.
  uint64_t first_generation = kNoGroup;
  auto* handle =
      load_image(memfd, name, file_backed, source_offset, placement, &first_generation, by_path);
  if (handle) {
    auto* root = image_of(handle);
#if YUKILINKER_FULL
    wait_slot_name(loading.slot, root->generation, root->namespace_id);
#endif
    // load_image ran pass -1 on each image. Resolvers run only in pass 2, and RELRO is shared
    // only after it, so every check that needs no code to run comes first. Slots a resolver
    // writes are checked once it has run. A root that deduplicated to an existing image shares
    // nothing: its RELRO was laid out when it first loaded.
    const bool ready =
#if defined(YUKILINKER_BOOTSTRAP)
        validate_bootstrap_core(root) &&
#endif
        relocate_group(first_generation, 0) && relocate_group(first_generation, 1) &&
        (graph_ready(root, first_generation) ||
         fail(YUKILINKER_C_BUSY, YUKILINKER_C_INITIALIZATION)) &&
        validate_group_lifecycle(first_generation, true) && replay_group(root, first_generation) &&
        validate_group_lifecycle(first_generation, false) &&
        (!relro || !in_group(root, first_generation) || share_relro(root, *relro) ||
         fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION));
    g_relocation_pass = 1;
    const bool own_group = in_group(root, first_generation);
    bool initialized = false;
    if (ready && own_group) {
      initialized = initialize_graph(root) && group_initialized(first_generation);
    } else if (ready) {
      // As Bionic, the flags are the image's before its constructors run; a failure takes back
      // what this open added.
      const AddedOpenFlags added = add_open_flags(
          root, g_open_options ? g_open_options->flags : static_cast<uint32_t>(RTLD_NOW));
      initialized = initialize_reused(root);
      if (!initialized) remove_open_flags(root, added);
    }
    if (initialized) {
      apply_dependency_flags(first_generation);
      // The handle goes back now, without the lock dropping again: its reference is the caller's.
      --root->pending_references;
      return handle;
    }
    // A reused root that was never initialized still holds this open's reference.
    if (!ready && !own_group) revoke_reference(root);
  }
  bool executed = false;
  bool observed = false;
  for (auto* image = g_first_image; image; image = image->next)
    if (image->load_root->generation == first_generation) {
      executed |= image->executed;
      // The loading thread's callbacks can pin its own unready images.
      observed |= image_is_pinned(image);
    }
  // The sweep below drops the lock while it closes dependencies. Before the first release, TLS is
  // deactivated for the whole group (a block taken afterwards would name an image that is about to
  // be freed; an inactive template cannot gain a block), and every member is marked releasing: no
  // lookup hands it out any more, and an open that would reach it gets YUKILINKER_C_BUSY on this
  // thread, whose host callbacks run meanwhile. Phases stay, so another thread's open waits for
  // this one.
  if (!executed && !observed)
    for (auto* image = g_first_image; image; image = image->next)
      if (image->load_root->generation == first_generation) {
        image->releasing = true;
        deactivate_tls(image);
      }
  uint64_t upper_generation = UINT64_MAX;
  for (;;) {
    auto* image = g_last_image;
    while (image && (image->generation >= upper_generation ||
                     image->load_root->generation != first_generation))
      image = image->previous;
    if (!image) break;
    upper_generation = image->generation;
    // A callback that ran while an earlier image was released can pin this one, so the sweep
    // rechecks each image instead of trusting the snapshot taken above.
    if (image->load_root->generation == first_generation &&
        (executed || observed || image_is_pinned(image))) {
      discard_deferred_relocations(image);
      // Code that ran keeps the group for good; otherwise collection retires it once the last
      // pin is released.
      if (executed) {
        image->flags |= RTLD_NODELETE;
        image->nodelete_reasons |= YUKILINKER_C_NODELETE_FAILED_EXECUTION;
      }
      // Only the references of opens that have not returned go, which here is this open's own on
      // its root: one a constructor's open handed out stays, so its holder can still close it.
      image->references -= image->pending_references;
      image->pending_references = 0;
      image->phase = Phase::Failed;
      // Kept: a Failed image like any other from here on.
      image->releasing = false;
      record_failure(image);
      record_event(image, YUKILINKER_C_EVENT_LOAD_FAILURE, image->failure_code);
    } else if (image->load_root->generation == first_generation) {
      release_failed_image(image);
    }
  }
  // A page size that only this open's images committed goes with them: every image kept (FAILED,
  // or a dead copy whose munmap failed) is still registered and keeps it.
  release_image_page_size(g_first_image != nullptr);
  return nullptr;
}

void* dlsym(SoHandle* handle, const char* name) {
  OperationGuard operation;
  ImageState* image = state_of(handle);
  const ElfW(Sym)* symbol = find_defined_symbol(image, name);
  if (symbol == nullptr) return nullptr;
#if YUKILINKER_FULL
  if (ELF64_ST_TYPE(symbol->st_info) == STT_TLS) {
    if (!image->tls.active) return nullptr;
    TlsIndex index{image->tls.module_id, symbol->st_value};
    return yuki_tls_get_addr(&index);
  }
#endif  // #if YUKILINKER_FULL
  SymbolResolution resolution = materialize_defined_symbol(image, *symbol);
  return resolution.valid ? reinterpret_cast<void*>(resolution.address) : nullptr;
}

namespace {
void mark_image(ImageState* image) {
  if (!image || image->marked) return;
  image->marked = true;
  if (image->load_root != image) mark_image(image->load_root);
  for (auto* dep = image->dependencies; dep; dep = dep->next) mark_image(dep->image);
}

// As Bionic's soinfo::can_unload: RTLD_GLOBAL (or DF_1_GLOBAL) keeps an image as RTLD_NODELETE
// does, whatever its open count. Only an explicit retirement of its context, which has sealed it
// (Destroying), may unload it.
bool kept_global(const ImageState* image) {
  return (image->flags & RTLD_GLOBAL) && image->context->retirement != Retirement::kDestroying;
}

// Wakes the opens waiting for an image this collection was retiring. The lock is held, and a
// waiter links itself before it lets the lock go, so none is missed.
void wake_collect_waiters() {
#if YUKILINKER_FULL
  if (g_load_waiters) notify_load_waiters();
#endif
}

// A return-address hold keeps a Closing image until its scope's explicit retirement, which seals
// the scope (Destroying) first: from there no code of it is admitted, and its caller guarantees no
// code of it still runs (namespace_create).
bool reentry_kept(const ImageState* image) {
  return image->reentry_hold == ImageState::ReentryHold::kReturnAddress &&
         image->context->retirement != Retirement::kDestroying;
}

// Every external user the free loop rechecks: a callback can admit one while collection drops the
// lock.
bool held_image(const ImageState* image) {
  return image->references || image->active_calls || image->caller_calls || image->snapshot_refs ||
         image->lease_refs || image->execution_leases || image->unwind_leases ||
         image->cfi_leases || image->host_bridge_leases || image->tls_users ||
         image->thread_destructors;
}

void keep_callback_image(ImageState* image) {
  // A frame's hold ends with that frame. A return-address hold lasts until the
  // scope retires, because the image's code can run after the runtime returns.
  if (image->caller_calls)
    image->reentry_hold = ImageState::ReentryHold::kReturnAddress;
  else if (image->reentry_hold == ImageState::ReentryHold::kNone && held_image(image))
    image->reentry_hold = ImageState::ReentryHold::kFrame;
  image->nodelete_reasons |= YUKILINKER_C_NODELETE_CALLBACK_REENTRY;
  image->closing_pass = 0;
  wake_collect_waiters();
}

void reach_image(ImageState* image, uint64_t visit) {
  if (!image || image->graph_visit == visit) return;
  image->graph_visit = visit;
  reach_image(image->load_root, visit);
  for (auto* dep = image->dependencies; dep; dep = dep->next) reach_image(dep->image, visit);
}

// Whether an image the free loop does not free reaches target through load roots and dependency
// edges: one that is not Closing, or a Closing one a callback pinned again. Its relocated pointers
// may lead into target, so target stays mapped as long as it does.
bool reached_from_kept(const ImageState* target) {
  const uint64_t visit = ++g_graph_visit;
  for (auto* image = g_first_image; image; image = image->next) {
    if (image == target || image->graph_visit == visit ||
        (image->phase == Phase::Closing && !held_image(image) && !reentry_kept(image)))
      continue;
    reach_image(image, visit);
    if (target->graph_visit == visit) return true;
  }
  return false;
}

// The generations of a pass's Closing images: each is looked up again after every lock drop, so a
// pointer the drop may have freed is never followed. Empty when the array cannot be had, and the
// caller then scans the list as before.
struct ClosingSet {
  struct Entry {
    uint64_t generation;
    uint64_t order;
  };
  // Small dependency groups need no heap allocation. The set remains local to
  // this collection; callbacks only retain generations, never these entries.
  Entry local[8];
  Entry* entries = local;
  size_t count = 0;
  ClosingSet() = default;
  ClosingSet(const ClosingSet&) = delete;
  ClosingSet& operator=(const ClosingSet&) = delete;
  ~ClosingSet() {
    if (entries != local) free(entries);
  }

  // The Closing images that satisfy keep, in registration order.
  template <typename Keep>
  bool build(Keep keep) {
    if (entries != local) free(entries);
    entries = local;
    count = 0;
    size_t total = 0;
    for (auto* image = g_first_image; image; image = image->next) total += keep(image);
    if (total == 0) return true;
    if (total > sizeof(local) / sizeof(local[0])) {
      entries = static_cast<Entry*>(calloc(total, sizeof(Entry)));
      if (!entries) return false;
    }
    for (auto* image = g_first_image; image; image = image->next)
      if (keep(image)) entries[count++] = {image->generation, image->initialization_order};
    return true;
  }

  // Latest initialized first, as the finalizers run. A heap sort: no memory of its own and
  // O(n log n) whatever order a large group was registered in.
  void sort_by_initialization() {
    // A min-heap on order, popped to the end, leaves the largest order first.
    auto sift = [this](size_t root, size_t size) {
      for (size_t child; (child = root * 2 + 1) < size; root = child) {
        if (child + 1 < size && entries[child + 1].order < entries[child].order) ++child;
        if (!(entries[child].order < entries[root].order)) return;
        const Entry swap = entries[root];
        entries[root] = entries[child];
        entries[child] = swap;
      }
    };
    for (size_t i = count / 2; i-- != 0;) sift(i, count);
    for (size_t end = count; end > 1; --end) {
      const Entry swap = entries[0];
      entries[0] = entries[end - 1];
      entries[end - 1] = swap;
      sift(0, end - 1);
    }
  }
};

// One pass of a collection. It makes Closing only images registered before the collection began
// (first_new): one loaded meanwhile, by a finalizer, a host hook or another thread, waits for the
// next collection, so libraries whose finalizers load and close each other cannot keep it going.
// Such an image keeps what it needs as well, or its relocations would lead into a finalized image.
// A metadata page left mapped clears *complete and the pass goes on. A reservation left mapped
// keeps its image as a dead copy, for a later collection to retry, and the pass goes on with the
// other images; it then returns false.
bool collect_pass(uint64_t first_new, bool* complete) {
  bool unmapped = true;
  // Most collections (a lease, snapshot or close that leaves every image kept) find nothing to
  // retire: an image no root keeps is one that no reference, pin or flag holds itself. Without
  // such an image and without a Closing one left from before, nothing can become Closing, and the
  // mark below is skipped. One held only by calls still counts: the mark decides what they keep.
  bool candidate = false;
  {
    for (auto* image = g_first_image; image && !candidate; image = image->next)
      candidate = image->phase == Phase::Closing ||
                  (image->generation < first_new && !image->references && !image->snapshot_refs &&
                   !image->lease_refs && !image->tls_users && !image->thread_destructors &&
                   !(image->flags & RTLD_NODELETE) && !reentry_kept(image) && !kept_global(image) &&
                   image->phase != Phase::Preparing && image->phase != Phase::Relocated &&
                   image->phase != Phase::Initializing);
  }
  if (!candidate) return true;
  {
    for (auto* image = g_first_image; image; image = image->next) image->marked = false;
    // What holds an image for good, or until its holder lets it go: a reference, a pin, a flag, an
    // open under way, a scope's retirement still ahead of a return-address hold.
    for (auto* image = g_first_image; image; image = image->next)
      if (image->references || image->snapshot_refs || image->lease_refs || image->tls_users ||
          image->thread_destructors || (image->flags & RTLD_NODELETE) || reentry_kept(image) ||
          kept_global(image) || image->phase == Phase::Preparing ||
          image->phase == Phase::Relocated || image->phase == Phase::Initializing ||
          image->generation >= first_new)
        mark_image(image);
    // An image nothing else keeps, held only by a call its own code made into the runtime: that
    // code runs on once the call returns, and no count sees it end. It is kept with a
    // return-address hold from here until its scope's retirement, and so is what it needs.
    for (auto* image = g_first_image; image; image = image->next)
      if (!image->marked && image->caller_calls) {
        image->reentry_hold = ImageState::ReentryHold::kReturnAddress;
        image->nodelete_reasons |= YUKILINKER_C_NODELETE_CALLBACK_REENTRY;
        mark_image(image);
      }
    // A runtime->image->runtime frame (or a host call the runtime makes for the image) holds it
    // while it lasts: the image's code returns before the frame does, so nothing is left once it
    // ends.
    for (auto* image = g_first_image; image; image = image->next)
      if (!image->marked && image->active_calls) mark_image(image);
    for (auto* image = g_last_image; image; image = image->previous)
      if (!image->marked && image->phase != Phase::Closing && image->generation < first_new) {
        image->phase = Phase::Closing;
        image->closing_pass = g_collection_pass;
        record_event(image, YUKILINKER_C_EVENT_CLOSE);
      }
  }
  // Finalizers run latest initialized first, each one dropping the lock. No image becomes Closing
  // and initialized meanwhile (only this pass makes images Closing), so the order taken here is
  // the one a scan before each finalizer would find; each image is looked up again and rechecked.
  // One whose finalizers another run is still running (process exit, on another thread) is left
  // to it; the free loop below keeps it until they are done.
  const auto finalizable = [](const ImageState* image) {
    return image->phase == Phase::Closing && image->lifecycle.initialized &&
           !image->lifecycle.fini_running &&
           image->lifecycle.blocked_cleanup_pass != g_collection_pass;
  };
  ClosingSet closing;
  if (closing.build(finalizable)) {
    closing.sort_by_initialization();
    for (size_t i = 0; i < closing.count; ++i) {
      auto* image = image_by_generation(closing.entries[i].generation);
      if (image && finalizable(image)) run_finalizers(image);
    }
  }
  for (;;) {
    ImageState* last = nullptr;
    for (auto* image = g_first_image; image; image = image->next)
      if (finalizable(image) && (!last || image->initialization_order > last->initialization_order))
        last = image;
    if (!last) break;
    run_finalizers(last);
  }
  // The calls below drop the lock, and another thread's failure sweep can free a neighbour
  // meanwhile, so each round looks up the next image again instead of keeping a pointer.
  for (auto* image = g_first_image; image; image = image->next)
    image->retire_pending = image->phase == Phase::Closing;
  const bool listed =
      closing.build([](const ImageState* image) { return image->phase == Phase::Closing; });
  for (size_t next = closing.count;;) {
    ImageState* image = nullptr;
    if (listed) {
      // Last registered first, as the scan below; one freed meanwhile is skipped.
      while (next != 0 && !image) {
        image = image_by_generation(closing.entries[--next].generation);
        if (image && !image->retire_pending) image = nullptr;
      }
    }
    // Once the set is done, or without it: the list itself. Every image handled has its flag
    // cleared, so this finds only one the index cannot name (a dead copy left before it had a
    // token).
    if (!image) {
      image = g_last_image;
      while (image && !image->retire_pending) image = image->previous;
    }
    if (!image) break;
    image->retire_pending = false;
    if (image->phase == Phase::Closing) {
      // A callback can admit a new observer while collection temporarily drops
      // the mutation lock. Recheck all external users before freeing metadata.
      if (held_image(image) || reentry_kept(image)) {
        // A dead copy from here on: an open that waited for it loads a fresh one. Not RTLD_NODELETE
        // (reentry_hold says why it stays): a frame's hold ends with the frame, and the first
        // collection that finds the image unheld retires it; a hold by a call its own code made
        // ends only with its scope's retirement.
        keep_callback_image(image);
      } else if (image->lifecycle.initialized || reached_from_kept(image)) {
        if (image->lifecycle.blocked_cleanup_pass == g_collection_pass) {
          *complete = false;
          unmapped = false;
        }
        // Its finalizers have not all run (another run is under way, or one stopped where its
        // thread ended), or a kept image still reaches it, as a dependency or as its load root:
        // it stays mapped with its edges, for the collection that finishes the finalizers, or a
        // dead copy that retires once nothing kept reaches it.
        image->closing_pass = 0;
        wake_collect_waiters();
      } else {
        if (!retire_process_exit(image)) {
          *complete = false;
          unmapped = false;
          image->closing_pass = 0;
          wake_collect_waiters();
          continue;
        }
        // libc removal dropped the lock. A new observer can now pin this dead
        // copy; never carry the earlier free-loop decision across that call.
        if (held_image(image) || reentry_kept(image)) {
          keep_callback_image(image);
          continue;
        }
        if (image->lifecycle.initialized || reached_from_kept(image)) {
          image->closing_pass = 0;
          wake_collect_waiters();
          continue;
        }
        deactivate_tls(image);
        // The lock stays held from here to unregister_image, so no other thread sees the image
        // in between, and no other thread's snapshot holds it (held_image above). releasing
        // hides it from a signal handler on this thread whose private dl_iterate_phdr runs
        // between the munmap and unregister_image.
        image->releasing = true;
        unregister_hwasan_globals(image);
        const bool released = [&] {
          if (image->mapping_released) return true;

          if (image->memory.caller_reserved) {
          } else {
          }
          return image->memory.caller_reserved
                     ? mmap(image->memory.reservation, image->memory.span, PROT_NONE,
                            MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED
                     : munmap(image->memory.reservation, image->memory.span) == 0;
        }();
        if (!released) {
          image->releasing = false;
          if (g_operation_error) g_operation_error->cleanup_incomplete = 1;
          // Each retry still fails the call that made it; only the event is recorded once.
          if (!image->cleanup_reported)
            record_event(image, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR);
          image->cleanup_reported = true;
          unmapped = fail(YUKILINKER_C_IO_ERROR, YUKILINKER_C_RETIREMENT);
          image->closing_pass = 0;
          wake_collect_waiters();
          continue;
        }
        // An image kept earlier marked what only it reaches; those get another pass.
        if (image->marked) g_collect_again = true;
#if defined(YUKILINKER_RUNTIME)
        // A group whose open a thread abandoned before any of its code ran: no stub address left
        // the image's relocated words, unmapped just now, so its stub page goes too.
        if (image->stubs_unpublished) discard_unpublished_caller_stubs(image);
#endif
        // The image is unmapped already. Drop every edge to it before the calls below drop the
        // lock, so no lookup can walk into it; an unregistered image gains no new edge.
        forget_image(image);
        // Only metadata remains visible. No later query, retry or callback may
        // dereference names or headers from the mapping, or unmap it twice.
        image->mapping_released = true;
        image->memory.reservation = nullptr;
        image->memory.bias = nullptr;
        image->memory.span = 0;
        image->memory.program_headers = nullptr;
        image->memory.program_header_count = 0;
        image->soname = image->runpath = nullptr;
        image->public_handle->load_bias = nullptr;
        image->public_handle->map_size = 0;
        image->load_root = image;
        for (auto* dependency = image->dependencies; dependency; dependency = dependency->next) {
          dependency->image = nullptr;
          dependency->name = nullptr;
        }
        // Gone: an open that waited for it looks again once the calls below drop the lock.
        wake_collect_waiters();
        if (!close_dependencies(image)) {
          *complete = false;
          unmapped = false;
          image->closing_pass = 0;
          continue;
        }
        unregister_image(image);
        discard_exit_callbacks(image);
        if (!free_image_state(image)) *complete = false;
      }
    }
  }
  return unmapped;
}

bool collect_images_internal() {
  if (exit_cleanup_uncertain()) {
    if (g_operation_error) g_operation_error->cleanup_incomplete = 1;
    return fail(YUKILINKER_C_BUSY, YUKILINKER_C_RETIREMENT);
  }
  // Collections never nest: a nested one would rewrite the phases and free the images this one
  // walks. A request made meanwhile, from a callback or from another thread while the lock is
  // dropped, makes the collection under way run another pass instead.
  if (g_collecting) {
    g_collect_again = true;
    return true;
  }
  struct CollectGuard {
    CollectGuard() {
      g_collecting = true;
#if YUKILINKER_FULL
      g_collecting_thread = pthread_self();
#endif
    }
    CollectGuard(const CollectGuard&) = delete;
    CollectGuard& operator=(const CollectGuard&) = delete;
    CollectGuard(CollectGuard&&) = delete;
    CollectGuard& operator=(CollectGuard&&) = delete;
    // What this collection left Closing is a dead copy from now on.
    ~CollectGuard() {
      g_collecting = false;
      wake_collect_waiters();
    }
  } collecting;
  const uint64_t first_new = g_next_image_generation;
  bool complete = true;
  // A failed unmap ends no pass and drops no request made meanwhile.
  bool unmapped = true;
  do {
    // Each pass clears the request first, so a stale one is never read.
    g_collect_again = false;
    ++g_collection_pass;
    unmapped &= collect_pass(first_new, &complete);
  } while (g_collect_again);
  return unmapped && complete;
}
}  // namespace

bool collect_images() { return collect_images_internal(); }

#if YUKILINKER_FULL
namespace {

// Whether an explicit retirement may unload every image of scope at once (retire_scope_images).
// Reads only.
bool scope_retirable(const Context* scope) {
  for (auto* image = g_first_image; image; image = image->next)
    if (image->context == scope && ((image->flags & RTLD_NODELETE) || held_image(image) ||
                                    image_loading(image) || image->releasing))
      return false;
  // What an image of another context reaches through a DT_NEEDED edge, a symbol provider, a
  // namespace link or its load root.
  const uint64_t visit = ++g_graph_visit;
  for (auto* image = g_first_image; image; image = image->next) {
    if (image->context == scope) continue;
    for (auto* dep = image->dependencies; dep; dep = dep->next) reach_image(dep->image, visit);
    if (image->load_root != image) reach_image(image->load_root, visit);
  }
  for (auto* image = g_first_image; image; image = image->next)
    if (image->context == scope && image->graph_visit == visit) return false;
  return true;
}

}  // namespace

uint32_t retire_scope_images(Context* scope) {
  const bool sealed = scope->retirement == Retirement::kDestroying;
  // A retirement of scope may be under way on another thread, its finalizers running unlocked.
  if (sealed && g_collecting) return YUKILINKER_C_BUSY;
  bool any = false;
  for (auto* image = g_first_image; image && !any; image = image->next)
    any = image->context == scope;
  if (!any) return YUKILINKER_C_OK;
  // A sealed context was partly retired by an earlier call; it resumes without the holder checks,
  // as nothing can have been added to it since.
  if (!sole_holder() || g_collecting || (!sealed && !scope_retirable(scope)))
    return YUKILINKER_C_BUSY;
  // Sealed before the collection drops the lock in a finalizer, and never unsealed: from here on
  // a refusal could no longer mean that nothing changed.
  scope->retirement = Retirement::kDestroying;
  // Another context's dead copy is that context's business; only what is left of scope counts.
  (void)collect_images_internal();
  for (auto* image = g_first_image; image; image = image->next)
    if (image->context == scope) return YUKILINKER_C_IO_ERROR;
  return YUKILINKER_C_OK;
}

bool other_thread_states() {
  const auto* own = g_thread_tls_key_valid
                        ? static_cast<const ThreadTlsState*>(pthread_getspecific(g_thread_tls_key))
                        : nullptr;
  for (const ThreadTlsState* state = g_thread_states; state; state = state->next)
    if (state != own || state->first || state->destructors) return true;
  return false;
}
#endif  // #if YUKILINKER_FULL

#if YUKILINKER_FULL
namespace {
// Another thread's private dl_iterate_phdr whose snapshot keeps image, or null.
SnapshotGuard* iteration_keeping(const ImageState* image) {
  for (auto* snapshot = g_active_snapshots; snapshot; snapshot = snapshot->next_active) {
    if (pthread_equal(snapshot->thread, pthread_self())) continue;
    for (size_t i = 0; i < snapshot->count; ++i)
      if (snapshot->images[i] == image) return snapshot;
  }
  return nullptr;
}

// The outermost snapshot of the thread that took snapshot: it ends last, so a close made while
// any of them pins an image collects when it ends. It owns that collection whatever its kind: a
// waitable one (module_dl_iterate_phdr) and abandon_snapshots read collect_after, and a
// non-waitable one (synchronous iterate_images) collects whenever it ends. A waitable snapshot
// inside a non-waitable one must not collect for the close instead, as the outer one still pins the
// image. Another thread's chain is read under the lock, which every guard takes to unlink.
SnapshotGuard* outermost_of(SnapshotGuard* snapshot) {
  while (snapshot && snapshot->previous) snapshot = snapshot->previous;
  return snapshot;
}

// This thread's outermost active snapshot, or null.
SnapshotGuard* outermost_snapshot() {
  return g_operation_depth_valid
             ? outermost_of(static_cast<SnapshotGuard*>(pthread_getspecific(g_snapshot_key)))
             : nullptr;
}
}  // namespace
#endif  // #if YUKILINKER_FULL

bool collect_after_close(ImageState* image) {
#if YUKILINKER_FULL
  // The close released the last reference while another thread's collection dropped the lock in a
  // callback. As Bionic's dlclose waits for the loader lock and unloads on the closing thread, it
  // waits for that collection to end and then collects here, so the destructors of what it
  // released run on this thread. The image stays pinned while it waits, or a later pass of that
  // collection would retire it there. A close that must not wait (wait_for_thread) asks that
  // collection for another pass instead, which retires the image on the collecting thread.
  // Another thread's private dl_iterate_phdr that keeps the image is waited for the same way, as
  // Bionic's dlclose waits for the dl_iterate_phdr holding g_dl_mutex; a close that must not wait
  // has that iteration collect when it ends. A close inside this thread's own iteration has it
  // collect when its outermost one ends: until then the snapshot keeps the image.
  if (!image->references && image->phase != Phase::Closing) {
    uint64_t since = 0;
    for (;;) {
      if (g_collecting && !pthread_equal(g_collecting_thread, pthread_self())) {
        if (!wait_for_thread(g_collecting_thread, nullptr, image, nullptr, &since)) break;
        continue;
      }
      auto* iteration = iteration_keeping(image);
      if (!iteration) break;
      if (wait_for_thread(iteration->thread, nullptr, image, nullptr, &since)) continue;
      // That thread's outermost snapshot owns the collection (outermost_of); it need not be
      // waitable.
      outermost_of(iteration)->collect_after = true;
      break;
    }
    if (auto* outer = outermost_snapshot()) outer->collect_after = true;
  }
#else
  (void)image;
#endif
  return collect_images_internal();
}

#if YUKILINKER_FULL
namespace {

// The opens of a thread that ended inside one of them (a constructor or a resolver that called
// pthread_exit) never finish their images, and another thread's open would wait for them for good.
// Every image that thread was still loading fails as its open's failure sweep would have failed
// it. Once any of them ran code they all stay registered as Failed with RTLD_NODELETE: a nested
// open's images may be what the outer group's relocations lead into. Otherwise they are Failed and
// unpinned, and the next collection retires them; true then.
bool fail_abandoned_opens(pthread_t self) {
  bool executed = false;
  bool found = false;
  for (auto* image = g_first_image; image; image = image->next)
    if (image_loading(image) && pthread_equal(image->loading_thread, self)) {
      found = true;
      executed |= image->executed;
    }
  if (!found) return false;
  host_log_error("yukilinker: a thread ended inside an open");
  for (auto* image = g_first_image; image; image = image->next) {
    if (!image_loading(image) || !pthread_equal(image->loading_thread, self)) continue;
    discard_deferred_relocations(image);
    if (executed) {
      image->flags |= RTLD_NODELETE;
      image->nodelete_reasons |= YUKILINKER_C_NODELETE_FAILED_EXECUTION;
    } else {
      image->stubs_unpublished = true;
    }
    image->failure_code = YUKILINKER_C_UNSUPPORTED;
    image->failure_stage = image->phase == Phase::Initializing ? YUKILINKER_C_INITIALIZATION
                                                               : YUKILINKER_C_RELOCATIONS;
    image->failure_stage_known = true;
    image->failure_errno = 0;
    // The dead thread's opens never return their handles; a handle one of them did return (a
    // constructor that opened its own library and passed the handle on) stays closable.
    image->references -= image->pending_references;
    image->pending_references = 0;
    image->phase = Phase::Failed;
    record_event(image, YUKILINKER_C_EVENT_LOAD_FAILURE, image->failure_code);
  }
  return !executed;
}

// What a thread's open or wait listed and never ends, as it does not return: its load_waits slots,
// and its waiter (a pthread_exit from a signal handler that interrupted the wait), whose node lives
// on the stack being freed, with the context and image the wait kept.
void abandon_waits() {
  wait_slots_forget(gettid());
  for (LoadWaiter** link = &g_load_waiters; *link;) {
    LoadWaiter* waiter = *link;
    if (!pthread_equal(waiter->thread, pthread_self())) {
      link = &waiter->next;
      continue;
    }
    *link = waiter->next;
    if (waiter->context) --waiter->context->waiting_opens;
    if (waiter->caller) --waiter->caller->active_calls;
  }
}

}  // namespace

// A thread that ends while it holds operation admissions outside any callback frame, or while an
// operation_lock of its own still waits for the mutex: only from a signal handler that called
// pthread_exit, as a wait for another thread had dropped the lock or the mutex was contended. Its
// admissions and waits go, and the rest as for a thread that ends inside a callback; with frames
// left, their own destructor does that.
void abandon_operation(void* value) {
  const auto bits = reinterpret_cast<uintptr_t>(value);
  const uintptr_t waits = bits >> kLockWaitShift;
  if (waits != 0) __atomic_sub_fetch(&g_lock_waiters, waits, __ATOMIC_ACQ_REL);
  __atomic_sub_fetch(&g_active_operations, (bits & kDepthMask) + waits, __ATOMIC_ACQ_REL);
  if (pthread_getspecific(g_callback_key) != nullptr) {
    OperationGuard operation;
    abandon_waits();
    if (g_load_waiters) notify_load_waiters();
    return;
  }
  abandon_callback_frames(nullptr);
}

// A thread that ends inside module code (pthread_exit from a callback; Bionic has no
// pthread_cancel) leaves its frames open. Bionic runs the key destructors before it frees the
// stack, so the frames are still readable: give back their admissions and pins, fail the images
// its opens were loading, and end the collection or create this thread had under way. What that
// collection left Closing is retired by the next one. A longjmp out of a callback is not
// supported.
void abandon_callback_frames(void* innermost) {
  OperationGuard operation;
  abandon_exit_cleanup(pthread_self(), false);
#if defined(YUKILINKER_RUNTIME)
  modules_abandon_thread(pthread_self());
#endif
  abandon_waits();
  size_t admissions = 0;
  for (auto* frame = static_cast<CallbackGuard*>(innermost); frame;) {
    CallbackGuard* previous = frame->previous;
    if (frame->caller_call) {
      // A call the thread's image code made into the runtime: its count goes with its marker. A
      // marker with no image is one whose call is over (its unlink failed), counted back already.
      if (frame->image) --frame->image->caller_calls;
      free(frame);
    } else {
      if (frame->saved_state) admissions += frame->depth;
      if (frame->image) --frame->image->active_calls;
    }
    frame = previous;
  }
  __atomic_sub_fetch(&g_active_operations, admissions, __ATOMIC_ACQ_REL);
  const pthread_t self = pthread_self();
  if (g_runtime_creating && pthread_equal(g_runtime_creating_thread, self))
    g_runtime_creating = false;
  // A shutdown past its boundary stays there; the next call resumes it.
  abandon_shutdown_run(self);
  // A finalization this thread ended inside goes on with the next run, from the step after.
  for (auto* image = g_first_image; image; image = image->next)
    if (image->lifecycle.fini_running && pthread_equal(image->lifecycle.fini_thread, self))
      image->lifecycle.fini_running = false;
  const bool collecting = g_collecting && pthread_equal(g_collecting_thread, self);
  if (collecting) {
    g_collecting = false;
    host_log_error("yukilinker: a thread ended inside a collection");
  }
  const bool released = fail_abandoned_opens(self);
  if (g_load_waiters) notify_load_waiters();
  // What that collection made Closing retires now, as the close that started it asked, and so does
  // a failed group that ran no code; inside an iteration this thread also left, once
  // abandon_snapshots has given back its pins.
  if (!collecting && !released) return;
  if (auto* outer = outermost_snapshot())
    outer->collect_after = true;
  else
    (void)collect_images();
}

void abandon_snapshots(void* innermost) {
  OperationGuard operation;
  bool collect = false;
  for (auto* snapshot = static_cast<SnapshotGuard*>(innermost); snapshot;) {
    SnapshotGuard* previous = snapshot->previous;
    for (size_t i = 0; i < snapshot->count; ++i) --snapshot->images[i]->active_calls;
    if (snapshot->waitable) {
      SnapshotGuard** link = &g_active_snapshots;
      while (*link && *link != snapshot) link = &(*link)->next_active;
      if (*link) *link = snapshot->next_active;
    }
    collect |= snapshot->collect_after;
    // Each guard starts its iteration's calloc block.
    free(snapshot);
    snapshot = previous;
  }
  // A close on another thread may wait for this iteration.
  if (g_load_waiters) notify_load_waiters();
  if (collect) (void)collect_images();
}
#endif  // #if YUKILINKER_FULL

void dlclose(SoHandle* handle) {
  OperationGuard operation;
  ImageState* image = state_of(handle);
  if (!image || !committed_references(image)) return;
  --image->references;
  collect_images();
}

namespace {

// What earlier failed munmaps left behind: metadata pages and source mappings. Only leaks are
// retried, so a failure (YUKILINKER_C_IO_ERROR, recorded) changes nothing else, and a later call
// retries.
bool release_leftovers() {
  metadata_lock();
  MetadataPage* pages = g_metadata_pages;
  g_metadata_pages = nullptr;
  metadata_unlock();
  if (!release_metadata(pages)) {
    record_event(nullptr, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR);
    return false;
  }
  while (g_retained_sources) {
    auto* source = g_retained_sources;
    if (munmap(source->address, source->bytes) != 0) {
      report_cleanup_failure();
      record_event(nullptr, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR);
      return false;
    }
    g_retained_sources = source->next;
    free_object(source);
  }
  return true;
}

}  // namespace

#if YUKILINKER_FULL
uint32_t begin_shutdown() {
  if (g_shutdown_phase != ShutdownPhase::kActive) return YUKILINKER_C_OK;
  // Every check reads only and none drops the lock, so nothing can be added between them and the
  // boundary below: a refusal changes nothing, the host imports included.
  bool blocked = !sole_holder() || g_collecting || g_contexts != nullptr;
#if defined(YUKILINKER_RUNTIME)
  blocked = blocked || registry_has_holders() || modules_has_holders();
#endif
  for (auto* image = g_first_image; !blocked && image; image = image->next)
    blocked = image->context != &g_default_context;
  blocked = blocked || !scope_retirable(&g_default_context);
  const bool thread_state = !blocked && other_thread_states();
  if (blocked || thread_state) {
    // A host call drops the lock; nothing follows the refusal, so it may.
    if (thread_state) host_log_error("yukilinker: shutdown blocked by thread state");
    return YUKILINKER_C_BUSY;
  }
  // Leaks only: a failure changes nothing else.
  if (!release_leftovers()) return YUKILINKER_C_IO_ERROR;
  // The boundary, in the lock hold of the checks. From here on the default context and the owner
  // admit nothing new, and only a shutdown resumes.
  g_shutdown_phase = ShutdownPhase::kDestroying;
  g_default_context.retirement = Retirement::kDestroying;
  if (g_runtime_owner != nullptr) g_runtime_owner->active = false;
  return YUKILINKER_C_OK;
}

bool finish_shutdown() {
  g_shutdown_running = true;
  g_shutdown_thread = pthread_self();
  // Not run when the thread ends inside a destructor: abandon_callback_frames clears it then.
  struct Running {
    ~Running() { g_shutdown_running = false; }
    Running() = default;
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
    Running(Running&&) = delete;
    Running& operator=(Running&&) = delete;
  } running;
  // Each step is done once: a retired image is gone, and an import leaves the list before its
  // dlclose drops the lock.
  if (retire_scope_images(&g_default_context) != YUKILINKER_C_OK) return false;
#if defined(YUKILINKER_RUNTIME)
  (void)release_host_imports(&g_default_context);
#endif
  if (!shutdown()) return false;
  g_shutdown_phase = ShutdownPhase::kShutDown;
  return true;
}

bool shutdown_unfinishable() {
  // What no later call can retire: an RTLD_NODELETE image left in the sealed default context.
  for (auto* image = g_first_image; image; image = image->next)
    if (image->context == &g_default_context && (image->flags & RTLD_NODELETE)) return true;
  return false;
}
#endif  // #if YUKILINKER_FULL

bool shutdown() {
  OperationGuard operation;
  if (g_contexts) return false;
#if defined(YUKILINKER_RUNTIME)
  // Only the runtime links the registry; the bootstrap loader has no snapshots or leases.
  if (registry_has_holders() || modules_has_holders()) return false;
#endif
#if YUKILINKER_FULL
  if (!sole_holder()) return false;
  // This thread's state goes if it holds nothing. Another thread's stays: its exit runs runtime
  // code (the TLS key destructor), so the runtime has to outlive that thread.
  if (g_thread_tls_key_valid)
    release_empty_thread_tls_state(
        static_cast<ThreadTlsState*>(pthread_getspecific(g_thread_tls_key)));
  if (g_thread_states) {
    host_log_error("yukilinker: shutdown blocked by thread state");
    return false;
  }
#endif
  registry_lock();
  const bool images_empty = g_first_image == nullptr;
  registry_unlock();
  if (!images_empty) {
    host_log_error("yukilinker: shutdown blocked by loaded images");
    return false;
  }
  // What earlier failed munmaps left behind goes first, while nothing is stopped yet: a failure
  // here leaves the runtime whole, and a later call tries again.
  if (!release_leftovers()) return false;
#if YUKILINKER_FULL
  {
    ScopedSignalBlocker signals;
    pthread_mutex_lock(&g_tls_mutex);
    g_tls_stopped = true;
    for (TlsTemplate* tls = g_tls_templates; tls != nullptr; tls = tls->next) tls->active = false;
    g_tls_templates = nullptr;
    // No thread has a block left (it would still have a state), so no lock-free read reaches a
    // chunk; each is unpublished before it is freed all the same.
    for (size_t i = 0; i < g_tls_generation_chunk_count; ++i) {
      uintptr_t* chunk = g_tls_generation_chunks[i];
      __atomic_store_n(&g_tls_generation_chunks[i], static_cast<uintptr_t*>(nullptr),
                       __ATOMIC_RELEASE);
      free(chunk);
    }
    g_tls_generation_chunk_count = 0;
    pthread_mutex_unlock(&g_tls_mutex);
  }

  // From here on no key is created again, whatever becomes of the rest of shutdown.
  pthread_mutex_lock(&g_key_mutex);
  g_keys_retired = true;
  pthread_mutex_unlock(&g_key_mutex);
  if (__atomic_load_n(&g_thread_tls_key_valid, __ATOMIC_ACQUIRE)) {
    auto* thread = static_cast<ThreadTlsState*>(pthread_getspecific(g_thread_tls_key));
    (void)pthread_setspecific(g_thread_tls_key, nullptr);
    // Cleared before the key goes: Bionic hands a deleted key's index to the next key created,
    // and the lock-free path must never read another library's value through it.
    __atomic_store_n(&g_thread_tls_key_valid, false, __ATOMIC_RELEASE);
    pthread_key_delete(g_thread_tls_key);
    destroy_thread_tls_state(thread);
  }
  retire_dl_error_key();
  if (g_initializing_key_valid) {
    pthread_key_delete(g_initializing_key);
    __atomic_store_n(&g_initializing_key_valid, false, __ATOMIC_RELEASE);
  }
  if (g_operation_depth_valid) {
    // initialize_operation_key created all three; valid means none of them failed.
    pthread_key_delete(g_snapshot_key);
    pthread_key_delete(g_callback_key);
    pthread_key_delete(g_operation_depth_key);
    __atomic_store_n(&g_operation_depth_valid, false, __ATOMIC_RELEASE);
  }
#endif
  return true;
}

}  // namespace yukilinker::internal
