// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/cleanup_call.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

namespace {

// The largest page a host may ask for: 16 KiB, the largest an arm64 Android kernel uses. The
// runtime itself is linked for it (-z max-page-size=16384), so it could not load itself on a
// larger page, and a library built for Android 15's 16 KiB pages would not load either.
constexpr size_t kMaxHostPageSize = size_t{16} * 1024;

}  // namespace

// The page the kernel itself mmaps with, independent of any host override.
size_t system_page_size() { return kernel_page_size(); }

bool validate_host_services(const yukilinker_c_host_services* services) {
  if (services == nullptr || services->size != sizeof(yukilinker_c_host_services) ||
      services->version != YUKILINKER_C_HOST_SERVICES_VERSION)
    return false;
  return services->system_dlopen != nullptr && services->system_dlsym != nullptr &&
         services->system_dlclose != nullptr && services->system_dladdr != nullptr &&
         services->system_dlerror != nullptr && services->system_iterate_phdr != nullptr;
}

// Asks the host hook without latching. The hook is host code, so it runs as a callback: the
// mutation lock is dropped and a frame is opened, which is what makes an owner-changing call it
// issues come back as YUKILINKER_C_BUSY instead of destroying the owner under its caller.
bool query_host_page_size(const yukilinker_c_host_services& services, size_t* requested,
                          bool* present) {
  *present = services.page_size != nullptr;
  if (!*present) return true;
  size_t value = 0;
  {
    CallbackGuard callback_scope(nullptr);
    value = services.page_size(services.user);
  }
  // Validate before latching: a rejected value must not become the sticky one.
  if (!is_power_of_two(value) || value < system_page_size() || value > kMaxHostPageSize)
    return false;
  *requested = value;
  return true;
}

bool adopt_host_page_size(size_t requested, bool present) {
  if (!present) return true;
  // Committed once an image registered with it (commit_page_size) or a create adopted it.
  if (g_page_size != 0) {
    if (g_page_size == requested) {
      // Adopted now: no failed open takes it back any more.
      g_page_size_from_image = false;
      return true;
    }
    // runtime_create has no error output: the log is how a host learns why it was refused.
    host_log_error("yukilinker: page size %zu refused: %zu is fixed by an image already loaded",
                   requested, g_page_size);
    return false;
  }
  g_page_size = requested;
  g_page_size_from_image = false;
  return true;
}

void* host_dlopen(const char* name, int flags, const void* extension, const ImageState* requester) {
#if defined(YUKILINKER_RUNTIME)
  modules_note_system_call();
#endif
  // The host may re-enter the runtime from here.
  CallbackGuard callback_scope(nullptr);
  const yukilinker_c_host_services& services = host_services();
  yukilinker_c_host_dlopen_request request{};
  request.size = sizeof(request);
  request.flags = flags;
  request.name = name;
  request.extension = extension;
  if (requester != nullptr) request.requester = {requester->generation, requester->namespace_id};
  return services.system_dlopen(services.user, &request);
}

void* host_dlopen_reported(const char* name, int flags, const ImageState* requester) {
  void* handle = host_dlopen(name, flags, nullptr, requester);
  if (handle == nullptr) (void)host_dlerror();
  return handle;
}

bool host_dlsym(void* handle, const char* name, const char* version, void** address) {
#if defined(YUKILINKER_RUNTIME)
  modules_note_system_call();
#endif
  // The host may re-enter the runtime from here.
  CallbackGuard callback_scope(nullptr);
  const yukilinker_c_host_services& services = host_services();
  void* result = nullptr;
  if (services.system_dlsym(services.user, handle, name, version, &result) == 0) {
    (void)services.system_dlerror(services.user);
    return false;
  }
  *address = result;
  return true;
}

int host_dlclose(void* handle) {
#if defined(YUKILINKER_RUNTIME)
  modules_note_system_call();
#endif
  // The host may re-enter the runtime from here.
  CallbackGuard callback_scope(nullptr);
  const yukilinker_c_host_services& services = host_services();

  return services.system_dlclose(services.user, handle);
}

HostCloseResult host_dlclose_checked(void* handle, ImageState* image, CleanupCall& call) {
  int status = -1;
  const bool returned = cleanup_call(image, call, [&] {
    const auto& services = host_services();

    status = services.system_dlclose(services.user, handle);
  });
  return {returned, status};
}

int host_dladdr(const void* address, Dl_info* info) {
  // The host may re-enter the runtime from here.
  CallbackGuard callback_scope(nullptr);
  const yukilinker_c_host_services& services = host_services();
  return services.system_dladdr(services.user, address, info);
}

char* host_dlerror() {
  // The host may re-enter the runtime from here.
  CallbackGuard callback_scope(nullptr);
  const yukilinker_c_host_services& services = host_services();
  return services.system_dlerror(services.user);
}

int host_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data) {
  // The host may re-enter the runtime from here.
  CallbackGuard callback_scope(nullptr);
  const yukilinker_c_host_services& services = host_services();
  return services.system_iterate_phdr(services.user, callback, data);
}

int host_close_fd(int fd) {
  // The host may re-enter the runtime from here.
  CallbackGuard callback_scope(nullptr);
  const yukilinker_c_host_services& services = host_services();
  return services.close_fd != nullptr ? services.close_fd(services.user, fd) : close(fd);
}

void host_fork_hook(uint32_t phase) {
  const yukilinker_c_host_services& services = host_services();
  if (services.fork_hook == nullptr) return;
  // module_fork calls the hook from a host-call frame that dropped the operation lock, before the
  // fork locks are taken and after they are given back. This frame only counts the call.
  CallbackGuard callback_scope(nullptr, true);
  services.fork_hook(services.user, phase);
}

void host_log_error(const char* format, ...) {
  const yukilinker_c_host_services& services = host_services();
  if (services.log == nullptr) return;
  // A log callback that re-enters the runtime must not run with the mutation lock held or with
  // this thread's depth counting: the call it is reporting may still be mid-flight.
  CallbackGuard callback_scope(nullptr);
  char message[512]{};
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (written > 0)
    services.log(services.user, 3, message,
                 static_cast<size_t>(written) < sizeof(message) ? static_cast<size_t>(written)
                                                                : sizeof(message) - 1);
}

}  // namespace yukilinker::internal
