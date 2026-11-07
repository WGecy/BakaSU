// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

namespace {

// A search candidate, only if it is a regular file: a FIFO, directory or device of that name is
// passed over as if it were missing. O_NONBLOCK keeps a FIFO from blocking open() while the
// operation lock is held; it changes nothing for a regular file. A failed fstat keeps the
// descriptor, and load_image's own fstat reports it.
int open_candidate(const char* path) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return -1;
  struct stat status{};
  if (fstat(fd, &status) == 0 && !S_ISREG(status.st_mode)) {
    host_close_fd(fd);
    return -1;
  }
  return fd;
}

}  // namespace

int search_paths(ImageState* image, const char* list, const char* name, char* path,
                 size_t capacity) {
  if (!list) return -1;
  for (const char* cursor = list; *cursor;) {
    const char* end = strchr(cursor, ':');
    const size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
    char directory[4096]{};
    if (length < sizeof(directory)) {
      size_t token = strncmp(cursor, "$ORIGIN", 7) == 0     ? 7
                     : strncmp(cursor, "${ORIGIN}", 9) == 0 ? 9
                                                            : 0;
      if (token && token <= length && (token == length || cursor[token] == '/')) {
        const char* slash = strrchr(image->origin, '/');
        if (image->origin[0] == '/' && slash) {
          const size_t prefix = static_cast<size_t>(slash - image->origin);
          if (prefix + length - token < sizeof(directory)) {
            (void)snprintf(directory, sizeof(directory), "%.*s%.*s", static_cast<int>(prefix),
                           image->origin, static_cast<int>(length - token), cursor + token);
          }
        }
      } else if (length && cursor[0] == '/') {
        (void)snprintf(directory, sizeof(directory), "%.*s", static_cast<int>(length), cursor);
      }
      const size_t bytes = strlen(directory);
      if (bytes && bytes + strlen(name) + 2 <= capacity) {
        (void)snprintf(path, capacity, "%s/%s", directory, name);
        int fd = open_candidate(path);
        if (fd >= 0) return fd;
      }
    }
    if (!end) break;
    cursor = end + 1;
  }
  return -1;
}

// A name the provider answered for, or an absolute one, is its own origin: a copy into path could
// cut it short and, for an absolute name, open another file. load_image refuses an origin that
// does not fit, and open() one longer than PATH_MAX.
int open_private_dependency(ImageState* image, const char* name, char* path, size_t capacity,
                            const char** origin, bool* by_path) {
  *origin = path;
  *by_path = false;
  if (image->context->provider) {
    int fd;
    {
      CallbackGuard callback_scope(image);
      fd = call_module(image->context->provider, image->context->user, image->origin, name);
    }
    if (fd >= 0) {
      *origin = name;
      return fd;
    }
  }
  *by_path = true;
  if (strchr(name, '/')) {
    if (name[0] != '/') return -1;
    *origin = name;
    // The name was asked for, not searched: anything but a regular file fails in load_image.
    return open(name, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  }
  const char* slash = image->runpath ? nullptr : strrchr(image->origin, '/');
  if (image->origin[0] == '/' && slash) {
    const size_t length = static_cast<size_t>(slash - image->origin);
    if (length + strlen(name) + 2 <= capacity) {
      memcpy(path, image->origin, length);
      (void)snprintf(path + length, capacity - length, "/%s", name);
      int fd = open_candidate(path);
      if (fd >= 0) return fd;
    }
  }
  int fd = search_paths(image, image->runpath, name, path, capacity);
  return fd >= 0 ? fd : search_paths(image, image->context->search_path, name, path, capacity);
}

namespace {

// Where a dependency's path is built. It lives on the heap: the frame that searches stays live
// while the dependency loads its own dependencies, one level per DT_NEEDED step.
struct SearchPath {
  char bytes[4096];
};

}  // namespace

#if YUKILINKER_FULL
namespace {

// Every name, or exactly one of the link's ':'-separated entries.
bool link_shares(const NamespaceLink& link, const char* name) {
  if ((link.flags & YUKILINKER_C_NAMESPACE_LINK_ALL) != 0) return true;
  const size_t length = strlen(name);
  for (const char* cursor = link.shared; *cursor != '\0';) {
    const char* end = strchr(cursor, ':');
    const size_t entry = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
    if (entry == length && memcmp(cursor, name, length) == 0) return true;
    if (!end) break;
    cursor = end + 1;
  }
  return false;
}

}  // namespace

LinkResolution resolve_through_links(ImageState* requester, const char* name, int flags) {
  LinkResolution result;
  bool shared = false;
  bool system_link = false;
  // When a wait for another thread's open began, across the rounds below.
  uint64_t waited_since = 0;
  for (const NamespaceLink* link = requester->context->links; link; link = link->next) {
    if (!link->target) system_link = true;
    if (!link_shares(*link, name)) continue;
    shared = true;
    if (link->target) {
      for (;;) {
        // A namespace under retirement hands out nothing; a wait below drops the lock.
        if (context_sealed(link->target)) {
          result.outcome = LinkOutcome::kBusy;
          return result;
        }
        const ImageState* loading = nullptr;
        for (auto* candidate = g_first_image; candidate && !loading; candidate = candidate->next) {
          if (candidate->context != link->target ||
              (strcmp(candidate->origin, name) != 0 &&
               (!candidate->soname || strcmp(candidate->soname, name) != 0)))
            continue;
          if (candidate->phase == Phase::Ready) {
            result.outcome = LinkOutcome::kImage;
            result.image = candidate;
            return result;
          }
          // One this thread's failure sweep is releasing is refused below like its own load.
          if (image_loading(candidate) || released_here(candidate)) loading = candidate;
        }
        if (!loading) break;
        // Another thread's open: wait for it, then look again. This thread's own is refused.
        if (!wait_for_load(loading, requester->context, requester, &waited_since)) {
          result.outcome = LinkOutcome::kBusy;
          return result;
        }
      }
      continue;
    }
    void* handle;
    {
      CallbackGuard callback_scope(requester);
      handle = host_dlopen_reported(name, flags | RTLD_NOLOAD, requester);
    }
    if (!handle && (flags & RTLD_NOLOAD) == 0) {
      {
        CallbackGuard callback_scope(requester);
        handle = host_dlopen_reported(name, flags, requester);
      }
      // Only a load that succeeded ran the library's constructors.
      if (handle && g_operation_error) g_operation_error->execution_started = 1;
    }
    if (handle) {
      result.outcome = LinkOutcome::kSystem;
      result.system_handle = handle;
      return result;
    }
  }
  if (system_link) result.outcome = shared ? LinkOutcome::kMissing : LinkOutcome::kDenied;
  return result;
}
#endif  // #if YUKILINKER_FULL

bool open_dependencies(ImageState* image, const DynamicParse& parse) {
  Dependency** tail = &image->dependencies;
  for (size_t i = 0; i < parse.entry_limit; ++i) {
    const auto& entry = parse.entries[i];
    if (entry.d_tag == DT_NULL) break;
    if (entry.d_tag != DT_NEEDED) continue;
    const char* name = string_at(image, entry.d_un.d_val);
    if (!name) return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_DEPENDENCIES);
    auto* dependency = metadata_object<Dependency>(&image->memory.metadata);
    if (!dependency) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_DEPENDENCIES);
    dependency->name = name;
    *tail = dependency;
    tail = &dependency->next;
    bool released = false;
    for (auto* candidate = g_first_image; candidate; candidate = candidate->next) {
      if (candidate->context != image->context || !candidate->soname ||
          strcmp(candidate->soname, name) != 0)
        continue;
      if (accessible_image(candidate)) {
        dependency->image = candidate;
        // As for a match by path: outside the group, the flags wait for the open to succeed.
        dependency->apply_open_flags = candidate->load_root != image->load_root;
        break;
      }
      released |= released_here(candidate);
    }
    if (dependency->image) continue;
    // The only match is a member that the failure sweep of this thread's own open is releasing.
    if (released) return fail(YUKILINKER_C_BUSY, YUKILINKER_C_DEPENDENCIES, name);
    auto* path = allocate_object<SearchPath>();
    if (!path) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_DEPENDENCIES, name);
    const char* origin = nullptr;
    bool by_path = false;
    int fd =
        open_private_dependency(image, name, path->bytes, sizeof(path->bytes), &origin, &by_path);
    if (fd >= 0) {
      ImageState* previous = g_loading_parent;
      g_loading_parent = image;
      auto* handle =
          load_image(fd, origin, image->memory.file_backed, 0, nullptr, nullptr, by_path);
      g_loading_parent = previous;
      // origin may point into path; load_image has copied it.
      free_object(path);
      host_close_fd(fd);
      if (!handle) return false;
      dependency->image = image_of(handle);
      dependency->apply_open_flags = dependency->image->load_root != image->load_root;
    } else {
      free_object(path);
#if YUKILINKER_FULL
      const LinkResolution linked = resolve_through_links(image, name, RTLD_NOW | RTLD_LOCAL);
      switch (linked.outcome) {
        case LinkOutcome::kImage:
          dependency->image = linked.image;
          dependency->linked = true;
          continue;
        case LinkOutcome::kSystem:
          dependency->system_handle = linked.system_handle;
          dependency->linked = true;
          continue;
        case LinkOutcome::kBusy:
          return fail(YUKILINKER_C_BUSY, YUKILINKER_C_DEPENDENCIES, name);
        case LinkOutcome::kDenied:
          // A refusal, not a failed call: no errno belongs to it.
          errno = 0;
          return fail(YUKILINKER_C_PERMISSION_DENIED, YUKILINKER_C_DEPENDENCIES, name);
        case LinkOutcome::kMissing:
          return fail(YUKILINKER_C_DEPENDENCY_MISSING, YUKILINKER_C_DEPENDENCIES, name);
        case LinkOutcome::kUnlinked:
          break;
      }
#endif  // #if YUKILINKER_FULL
      {
        CallbackGuard callback_scope(image);
        dependency->system_handle =
            host_dlopen_reported(name, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD, image);
      }
      if (!dependency->system_handle) {
        if (image->namespace_id != 0)
          return fail(YUKILINKER_C_DEPENDENCY_MISSING, YUKILINKER_C_DEPENDENCIES, name);
        {
          CallbackGuard callback_scope(image);
          dependency->system_handle = host_dlopen_reported(name, RTLD_NOW | RTLD_LOCAL, image);
        }
        // Only a load that succeeded ran the library's constructors. The guard has restored the
        // error record by now.
        if (dependency->system_handle && g_operation_error)
          g_operation_error->execution_started = 1;
      }
      if (!dependency->system_handle)
        return fail(YUKILINKER_C_DEPENDENCY_MISSING, YUKILINKER_C_DEPENDENCIES, name);
    }
  }
  return true;
}

namespace {

// The name a resolved dependency answers to: a private image's DT_SONAME (its file name when it
// has none, as Bionic names a library without one), or, for the system linker's library, the
// DT_NEEDED name it was found by, which it matched against that library's soname.
bool dependency_named(const Dependency& dependency, const char* name) {
  if (dependency.symbol_provider) return false;
  if (const ImageState* target = dependency.image) {
    if (target->soname) return strcmp(target->soname, name) == 0;
    const char* slash = strrchr(target->origin, '/');
    return strcmp(slash ? slash + 1 : target->origin, name) == 0;
  }
  return dependency.system_handle && dependency.name && strcmp(dependency.name, name) == 0;
}

}  // namespace

bool verneed_files_resolved(ImageState* image) {
  const auto& symbols = image->symbols;
  const auto* need = symbols.needs;
  for (size_t i = 0; i < symbols.need_count; ++i) {
    // validate_versions walked the same chain; every vn_file it names is a terminated string.
    const char* file = string_at(image, need->vn_file);
    bool found = false;
    for (const Dependency* dependency = image->dependencies; dependency && !found;
         dependency = dependency->next)
      found = file && dependency_named(*dependency, file);
    // As Bionic's VersionTracker: a version required of a library the image does not need.
    if (!found) return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_METADATA, file);
    if (i + 1 != symbols.need_count) need = next_verneed(image, need);
  }
  return true;
}

bool close_dependencies(ImageState* image) {
  for (Dependency* dependency = image->dependencies; dependency != nullptr;
       dependency = dependency->next) {
    if (dependency->system_handle != nullptr) {
#if YUKILINKER_FULL
      ++g_host_closes_in_flight;
#endif
      const auto closed =
          host_dlclose_checked(dependency->system_handle, image, dependency->close_call);
#if YUKILINKER_FULL
      --g_host_closes_in_flight;
#endif
      // The host has not taken the reference. Keep both it and the persistent
      // record, including when the private image has already been unmapped.
      if (!closed.entered) return false;
      dependency->system_handle = nullptr;
      // The system kept the library loaded. The image may be unmapped already, and the name with
      // it, so the event names the image that needed the library, by its origin, without an id.
      // The image is retired all the same, so the close succeeds; the shutdown reports the
      // library, as a host import it failed to close.
      if (closed.status != 0) {
#if YUKILINKER_FULL
        g_host_import_leaked = true;
#endif
        record_event(nullptr, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR,
                     image->origin);
        host_log_error("yukilinker: a system dependency of %s could not be closed", image->origin);
      }
    }
  }
  return true;
}

}  // namespace yukilinker::internal
