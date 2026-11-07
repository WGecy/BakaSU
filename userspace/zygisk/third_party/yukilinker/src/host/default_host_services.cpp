// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include <dlfcn.h>
#include <link.h>

#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

namespace {

void* default_dlopen(void*, const yukilinker_c_host_dlopen_request* request) {
  if (request->extension != nullptr)
    return ::android_dlopen_ext(request->name, request->flags,
                                static_cast<const android_dlextinfo*>(request->extension));
  return ::dlopen(request->name, request->flags);
}

int default_dlsym(void*, void* handle, const char* name, const char* version, void** address) {
  ::dlerror();
  void* result = version != nullptr ? ::dlvsym(handle, name, version) : ::dlsym(handle, name);
  if (::dlerror() != nullptr) return 0;
  *address = result;
  return 1;
}

int default_dlclose(void*, void* handle) { return ::dlclose(handle); }

int default_dladdr(void*, const void* address, Dl_info* info) { return ::dladdr(address, info); }

char* default_dlerror(void*) { return ::dlerror(); }

int default_iterate_phdr(void*, int (*callback)(struct dl_phdr_info*, size_t, void*), void* data) {
  return ::dl_iterate_phdr(callback, data);
}

}  // namespace

const yukilinker_c_host_services kDefaultHostServices{sizeof(yukilinker_c_host_services),
                                                      YUKILINKER_C_HOST_SERVICES_VERSION,
                                                      nullptr,
                                                      default_dlopen,
                                                      default_dlsym,
                                                      default_dlclose,
                                                      default_dladdr,
                                                      default_dlerror,
                                                      default_iterate_phdr,
                                                      nullptr,
                                                      nullptr,
                                                      nullptr,
                                                      nullptr};

}  // namespace yukilinker::internal
