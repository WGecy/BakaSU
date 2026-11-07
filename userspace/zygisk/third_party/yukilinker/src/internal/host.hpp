// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <dlfcn.h>
#include <link.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/yukilinker.h"
#include "loader_types.hpp"

namespace yukilinker::internal {

struct ImageState;
struct CleanupCall;
struct HostCloseResult {
  bool entered;
  int status;
};

// The defaults call libdl directly; they are the only runtime code allowed to.
extern const yukilinker_c_host_services kDefaultHostServices;
// Never null. Points at the defaults or at the copy held by the runtime owner.
extern const yukilinker_c_host_services* g_host_services;

static inline const yukilinker_c_host_services& host_services() {
  return *__atomic_load_n(&g_host_services, __ATOMIC_ACQUIRE);
}

// Callers must ensure no runtime operation can observe the previous table.
static inline void publish_host_services(const yukilinker_c_host_services* services) {
  __atomic_store_n(&g_host_services, services != nullptr ? services : &kDefaultHostServices,
                   __ATOMIC_RELEASE);
}

bool validate_host_services(const yukilinker_c_host_services* services);
// Asks the host page_size hook, if any. `present` reports whether the hook exists, so a
// caller that already asked does not have to call it twice.
bool query_host_page_size(const yukilinker_c_host_services& services, size_t* requested,
                          bool* present);
bool adopt_host_page_size(size_t requested, bool present);
// The kernel's own mmap granularity, unaffected by a host page_size override.
size_t system_page_size();

void* host_dlopen(const char* name, int flags, const void* extension, const ImageState* requester);
// A system open whose failure the runtime reports itself (a RTLD_NOLOAD probe, a dependency): a
// failure also consumes the host's dlerror, so a later private dlerror cannot return it.
void* host_dlopen_reported(const char* name, int flags, const ImageState* requester);
// A miss consumes the host's dlerror as well: every caller reports its own.
bool host_dlsym(void* handle, const char* name, const char* version, void** address);
int host_dlclose(void* handle);
HostCloseResult host_dlclose_checked(void* handle, ImageState* image, CleanupCall& call);
int host_dladdr(const void* address, Dl_info* info);
char* host_dlerror();
int host_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data);
int host_close_fd(int fd);
void host_fork_hook(uint32_t phase);
void host_log_error(const char* format, ...) __attribute__((format(printf, 1, 2)));

}  // namespace yukilinker::internal
