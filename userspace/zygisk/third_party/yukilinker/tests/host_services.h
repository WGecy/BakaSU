// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#ifndef YUKILINKER_TEST_HOST_SERVICES_H
#define YUKILINKER_TEST_HOST_SERVICES_H

#include <android/dlext.h>
#include <link.h>
#include <stdio.h>
#include <yukilinker.h>

static __thread char test_saved_error[512];
static __thread int test_pending_error;

/* Bionic selects a namespace from the caller's return address. */
#define TEST_HOST_CALL __attribute__((noinline, disable_tail_calls))
static TEST_HOST_CALL void* test_open(void* user, const yukilinker_c_host_dlopen_request* request) {
  (void)user;
  if (request->extension)
    return android_dlopen_ext(request->name, request->flags,
                              (const android_dlextinfo*)request->extension);
  return dlopen(request->name, request->flags);
}
static TEST_HOST_CALL int test_lookup(void* user, void* handle, const char* name,
                                      const char* version, void** address) {
  (void)user;
  dlerror();
  *address = version ? dlvsym(handle, name, version) : dlsym(handle, name);
  const char* error = dlerror();
  test_pending_error = error != NULL;
  if (error) snprintf(test_saved_error, sizeof(test_saved_error), "%s", error);
  return error == NULL;
}
static TEST_HOST_CALL int test_close(void* user, void* handle) {
  (void)user;
  return dlclose(handle);
}
static TEST_HOST_CALL int test_address(void* user, const void* address, Dl_info* info) {
  (void)user;
  return dladdr(address, info);
}
static TEST_HOST_CALL char* test_error(void* user) {
  (void)user;
  if (test_pending_error) {
    test_pending_error = 0;
    return test_saved_error;
  }
  return dlerror();
}
static TEST_HOST_CALL int test_iterate(void* user,
                                       int (*visit)(struct dl_phdr_info*, size_t, void*),
                                       void* data) {
  (void)user;
  return dl_iterate_phdr(visit, data);
}
static yukilinker_c_host_services test_host_services(void) {
  yukilinker_c_host_services services = {0};
  services.size = sizeof(services);
  services.version = YUKILINKER_C_HOST_SERVICES_VERSION;
  services.system_dlopen = test_open;
  services.system_dlsym = test_lookup;
  services.system_dlclose = test_close;
  services.system_dladdr = test_address;
  services.system_dlerror = test_error;
  services.system_iterate_phdr = test_iterate;
  return services;
}
#undef TEST_HOST_CALL
#endif
