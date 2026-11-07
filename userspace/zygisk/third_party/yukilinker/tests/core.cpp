// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "control.hpp"
#include "yukilinker.h"

#if __has_feature(hwaddress_sanitizer)
extern "C" void __hwasan_library_unloaded(ElfW(Addr), const ElfW(Phdr) *, ElfW(Half));
#endif

// The source integration helper must not disable the Core's language features.
#if !defined(__EXCEPTIONS) || !defined(__GXX_RTTI)
#error runtime source options leaked into the Core
#endif
namespace {
BootControl* control;
void* loader;
void** dependencies;
size_t dependency_count;
bool adopted, transferred, committed;
const void* api;
yukilinker_c_runtime* owner;
yukilinker_c_native_services native_services{};
yukilinker_c_handle native_module{};
char native_error[256]{};
[[gnu::noinline, clang::disable_tail_calls]] void* open_system(
    void*, const yukilinker_c_host_dlopen_request* request) {
  if (native_services.open) {
    ++control->native_opens;
    return native_services.open(native_services.context, request, native_error,
                                sizeof(native_error));
  }
  return dlopen(request->name, request->flags);
}
[[gnu::noinline, clang::disable_tail_calls]] int lookup_system(void*, void* handle,
                                                               const char* name,
                                                               const char* version,
                                                               void** address) {
  dlerror();
  *address = version ? dlvsym(handle, name, version) : dlsym(handle, name);
  return dlerror() == nullptr;
}
[[gnu::noinline, clang::disable_tail_calls]] int close_system(void*, void* handle) {
  return dlclose(handle);
}
[[gnu::noinline, clang::disable_tail_calls]] int address_system(void*, const void* address,
                                                                Dl_info* info) {
  return dladdr(address, info);
}
[[gnu::noinline, clang::disable_tail_calls]] char* error_system(void*) { return dlerror(); }
[[gnu::noinline, clang::disable_tail_calls]] int phdr_system(
    void*, int (*visit)(dl_phdr_info*, size_t, void*), void* data) {
  return dl_iterate_phdr(visit, data);
}
const yukilinker_c_host_services host{sizeof(host),   YUKILINKER_C_HOST_SERVICES_VERSION,
                                      nullptr,        open_system,
                                      lookup_system,  close_system,
                                      address_system, error_system,
                                      phdr_system,    nullptr,
                                      nullptr,        nullptr,
                                      nullptr};
void exit_callback(void*) { ++control->exit_callbacks; }
extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" void __cxa_finalize(void*);
extern "C" void* __dso_handle;
bool in_core(const void* pointer) {
  const uintptr_t p = reinterpret_cast<uintptr_t>(pointer);
  return p >= control->base && p - control->base < control->span;
}
int exercise(const char* fixtures) {
  if (native_module.generation) {
    const auto symbol = yukilinker_c_lookup(owner, native_module, "module_answer", nullptr);
    assert(symbol.found && reinterpret_cast<int (*)()>(symbol.address)() == 42);
    const auto closed = yukilinker_c_close(owner, native_module);
    assert(!closed.handle_valid && closed.error.detail.code == YUKILINKER_C_OK);
    native_module = {};
  }
  char path[4096];
  snprintf(path, sizeof(path), "%s/liblinker_module.so", fixtures);
  yukilinker_c_open_options options{
      sizeof(options), YUKILINKER_C_BACKEND_PRIVATE, RTLD_NOW, 0, nullptr, nullptr, 0, 0};
  for (uint32_t backend = YUKILINKER_C_BACKEND_PRIVATE; backend <= YUKILINKER_C_BACKEND_SYSTEM;
       ++backend) {
    options.backend = backend;
    yukilinker_c_error error{};
    const auto handle = yukilinker_c_open_path(owner, path, &options, &error);
    if (!handle.generation) return 0;
    const auto symbol = yukilinker_c_lookup(owner, handle, "module_answer", nullptr);
    if (!symbol.found || reinterpret_cast<int (*)()>(symbol.address)() != 42) return 0;
    const auto closed = yukilinker_c_close(owner, handle);
    if (closed.handle_valid || closed.error.detail.code != YUKILINKER_C_OK) return 0;
  }
  // TLS and both callback classes must remain usable after bootstrap unload.
  snprintf(path, sizeof(path), "%s/liblinker_lifecycle.so", fixtures);
  options.backend = YUKILINKER_C_BACKEND_PRIVATE;
  const auto tls = yukilinker_c_open_path(owner, path, &options, nullptr);
  if (!tls.generation) return 0;
  struct Work {
    int* (*slot)();
    int (*thread_exit)(int*);
    int runs = 0;
  } work{};
  work.slot =
      reinterpret_cast<int* (*)()>(yukilinker_c_lookup(owner, tls, "module_slot", nullptr).address);
  work.thread_exit = reinterpret_cast<int (*)(int*)>(
      yukilinker_c_lookup(owner, tls, "module_thread_exit", nullptr).address);
  auto at_exit = reinterpret_cast<int (*)(int*)>(
      yukilinker_c_lookup(owner, tls, "module_exit", nullptr).address);
  if (!work.slot || !work.thread_exit || !at_exit) return 0;
  int exits = 0;
  if (at_exit(&exits)) return 0;
  pthread_t thread;
  if (pthread_create(
          &thread, nullptr,
          [](void* value) -> void* {
            auto* w = static_cast<Work*>(value);
            assert(*w->slot() == 42 && w->thread_exit(&w->runs) == 0);
            return nullptr;
          },
          &work) ||
      pthread_join(thread, nullptr) || work.runs != 1)
    return 0;
  const auto closed = yukilinker_c_close(owner, tls);
  if (closed.handle_valid || exits != 1) return 0;
  return 1;
}
int retire() {
  if (owner && !yukilinker_c_runtime_destroy(owner)) return 0;
  owner = nullptr;
  __cxa_finalize(&__dso_handle);
  void** handles = dependencies;
  const size_t count = dependency_count;
  dependencies = nullptr;
  dependency_count = 0;
#if __has_feature(hwaddress_sanitizer)
  const auto base = control->base;
  const auto* header = reinterpret_cast<const ElfW(Ehdr)*>(base);
  const auto* phdr = reinterpret_cast<const ElfW(Phdr)*>(base + header->e_phoff);
  // No Core-owned globals are accessed after their shadow has been removed.
  __hwasan_library_unloaded(base, phdr, header->e_phnum);
#endif
  for (size_t i = count; i; --i) {
    if (handles[i - 1] && dlclose(handles[i - 1])) return 0;
    handles[i - 1] = nullptr;
  }
  free(handles);
  return 1;
}
__attribute__((constructor)) void construct() {
  const char* value = getenv("YUKILINKER_TEST_CONTROL");
  if (!value) return;  // ordinary system-loaded host used by ABI/unload regression tests
  control = reinterpret_cast<BootControl*>(strtoull(value, nullptr, 16));
  ++control->constructors;
  if (control->mode == BootCase::ReusePacket) {
    assert(close(control->reused_fd) == 0);
    const int replacement = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(replacement >= 0);
    if (replacement != control->reused_fd) {
      assert(dup2(replacement, control->reused_fd) == control->reused_fd);
      close(replacement);
    }
  }
  if (control->mode == BootCase::HoldConstructor) {
    __atomic_store_n(&control->entered, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&control->release, __ATOMIC_ACQUIRE)) usleep(1000);
  }
  if (control->mode == BootCase::Reenter) {
    errno = 0;
    control->bootstrap(control->retry_fd, 0);
    assert(errno == EBUSY);
  }
}
}  // namespace
#define CORE_EXPORT extern "C" __attribute__((visibility("default")))
CORE_EXPORT bool zygisk_bootstrap_bind_native_v1(const yukilinker_c_native_services* incoming) {
  ++control->native_binds;
  if (native_services.open || control->mode == BootCase::RejectNativeBinding || !incoming ||
      incoming->size != sizeof(*incoming) || incoming->version != 1 ||
      incoming->origin.process != static_cast<uint32_t>(getpid()) || !incoming->open)
    return false;
  native_services = *incoming;
  return true;
}
CORE_EXPORT bool zygisk_adopt_anonymous_image(uintptr_t base, size_t span, void* reference) {
  if (adopted || control->mode == BootCase::ReusePacket) return false;
  control->base = base;
  control->span = span;
  if (!in_core(reinterpret_cast<void*>(exercise))) return false;
  loader = reference;
  adopted = true;
  ++control->adopted;
  return true;
}
CORE_EXPORT void zygisk_core_entry(const char*, void* loader_anchor, void*, void*, int packet) {
  ++control->entries;
  if (packet) {
    yukilinker_c_early_packet_header header{};
    assert(pread(packet - 1, &header, sizeof(header), 0) == sizeof(header));
    for (uint32_t i = 0; i < header.count; ++i) {
      yukilinker_c_early_packet_entry item{};
      assert(pread(packet - 1, &item, sizeof(item), sizeof(header) + i * sizeof(item)) ==
             sizeof(item));
      assert(close(item.fd) == 0);
    }
    assert(close(header.endpoint_fd) == 0 && close(packet - 1) == 0);
    ++control->packet_transfers;
  }
  Dl_info info{};
  assert(dladdr(loader_anchor, &info));
  control->loader_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  api = reinterpret_cast<const void*>(yukilinker_c_abi_version);
  owner = yukilinker_c_runtime_create(&host);
  if (native_services.open) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/liblinker_module.so", getenv("YUKILINKER_TEST_FIXTURES"));
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    yukilinker_c_open_options options{};
    options.size = sizeof(options);
    options.backend = YUKILINKER_C_BACKEND_SYSTEM;
    options.flags = RTLD_NOW | RTLD_LOCAL;
    options.origin = path;
    native_module = yukilinker_c_open_fd(owner, fd, &options, nullptr);
    assert(native_module.generation && close(fd) == 0);
    native_services.open = nullptr;
    native_services.context = nullptr;
  }
  control->api = api;
  control->owner = owner;
  control->open = yukilinker_c_open_path;
  control->query = yukilinker_c_query;
  control->close = yukilinker_c_close;
  control->exercise = exercise;
  control->retire = retire;
  assert(__cxa_atexit(exit_callback, nullptr, &__dso_handle) == 0);
}
CORE_EXPORT bool zygisk_bootstrap_handoff_ready() {
  return owner && !native_services.open && !native_services.context &&
         control->mode != BootCase::RejectReady && in_core(api) &&
         in_core(reinterpret_cast<void*>(yukilinker_c_open_path)) &&
         in_core(reinterpret_cast<void*>(yukilinker_c_runtime_destroy)) && in_core(&host) &&
         in_core(reinterpret_cast<void*>(host.system_dlopen));
}
CORE_EXPORT bool zygisk_bootstrap_take_dependencies(void* const* handles, size_t count) {
  ++control->takes;
  if (transferred || control->mode == BootCase::RejectDependencies) return false;
  auto** copy = count ? static_cast<void**>(malloc(count * sizeof(void*))) : nullptr;
  if (count && !copy) return false;
  if (count) memcpy(copy, handles, count * sizeof(void*));
  dependencies = copy;
  dependency_count = count;
  control->dependencies = count;
  transferred = true;
  if (control->mode == BootCase::CleanupFailure) control->arm_cleanup_failure();
  return true;
}
CORE_EXPORT bool zygisk_bootstrap_finish_handoff(bool required) {
  ++control->finishes;
  if (committed || (required && !transferred) || control->mode == BootCase::RejectFinish)
    return false;
  return committed = true;
}
CORE_EXPORT void zygisk_finalize_loader(int, int) {
  assert(committed && loader);
  assert(dlclose(loader) == 0);
  loader = nullptr;
  ++control->finalized;
}
CORE_EXPORT bool zygisk_set_tango_stub(uint32_t stub, uint32_t size) {
  if (control->tango_stub || !stub || size != YUKILINKER_C_TANGO_BOOTSTRAP_SIZE) return false;
  control->tango_stub = stub;
  control->tango_size = size;
  if (control->mode == BootCase::ForkTango) {
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) return true;  // return through bootstrap; the driver checks child ownership
    int status = 0;
    assert(waitpid(child, &status, 0) == child && status == 0);
  }
  return true;
}
CORE_EXPORT void zygisk_finalize_loader_tango(uint32_t, uint32_t, uint32_t, int, int) {
  assert(control->tango_stub && control->tango_size == YUKILINKER_C_TANGO_BOOTSTRAP_SIZE);
  ++control->tango_finalized;
  zygisk_finalize_loader(0, 0);
}
