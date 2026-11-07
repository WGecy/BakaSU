// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// clang-format off: exercise repeated standalone C inclusion.
#include <yukilinker.h>
#include <yukilinker.h>
// clang-format on
#include "host_services.h"

_Static_assert(sizeof(yukilinker_c_handle) == 16, "module handle layout");
_Static_assert(offsetof(yukilinker_c_handle, generation) == 8, "handle generation offset");
_Static_assert(sizeof(yukilinker_c_early_packet_header) == 24, "packet header layout");
_Static_assert(sizeof(yukilinker_c_early_packet_entry) == 844, "packet entry layout");
_Static_assert(YUKILINKER_C_EARLY_PACKET_VERSION == 3, "packet wire version");

__attribute__((visibility("default"))) int yukilinker_test_run(const char* directory) {
  assert(yukilinker_c_abi_version() == 1);
  assert(yukilinker_c_has_feature(YUKILINKER_C_FEATURE_RUNTIME_OWNER));
  assert(!yukilinker_c_has_feature(UINT32_MAX));
  yukilinker_c_host_services services = test_host_services();
  yukilinker_c_runtime* runtime = yukilinker_c_runtime_create(&services);
  assert(runtime && !yukilinker_c_runtime_create(&services));
  assert(yukilinker_c_host_services_bind(runtime, NULL) == YUKILINKER_C_OK);
  assert(yukilinker_c_host_services_bind(runtime, &services) == YUKILINKER_C_OK);
  char path[4096];
  snprintf(path, sizeof(path), "%s/liblinker_module.so", directory);
  yukilinker_c_open_options options = {0};
  options.size = sizeof(options);
  options.flags = RTLD_NOW;
  for (uint32_t backend = YUKILINKER_C_BACKEND_PRIVATE; backend <= YUKILINKER_C_BACKEND_SYSTEM;
       ++backend) {
    options.backend = backend;
    yukilinker_c_error error = {0};
    yukilinker_c_handle handle = yukilinker_c_open_path(runtime, path, &options, &error);
#if defined(YUKILINKER_TEST_CFI)
    if (backend == YUKILINKER_C_BACKEND_PRIVATE) {
      assert(!handle.generation && error.detail.code == YUKILINKER_C_UNSUPPORTED);
      continue;
    }
#endif
    if (!handle.generation)
      fprintf(stderr, "open: %u %s %s\n", error.detail.code, error.detail.object,
              error.detail.symbol);
    assert(handle.generation);
    yukilinker_c_lookup_result symbol = yukilinker_c_lookup(runtime, handle, "module_answer", NULL);
    int (*answer)(void) = NULL;
    _Static_assert(sizeof(answer) == sizeof(symbol.address), "Android function pointer layout");
    memcpy(&answer, &symbol.address, sizeof(answer));
    assert(symbol.found && answer() == 42);
    assert(yukilinker_c_lookup(runtime, handle, "module_answer", "MODULE_API").found);
    assert(!yukilinker_c_lookup(runtime, handle, "module_answer", "UNKNOWN").found);
    yukilinker_c_handle_info info = {0};
    yukilinker_c_handle stale = handle;
    ++stale.generation;
    assert(yukilinker_c_query(runtime, stale, &info) == YUKILINKER_C_INVALID_HANDLE);
    assert(yukilinker_c_query((yukilinker_c_runtime*)(uintptr_t)1, handle, &info) ==
           YUKILINKER_C_STALE_TOKEN);
    yukilinker_c_close_result result = yukilinker_c_close(runtime, handle);
    assert(!result.handle_valid && result.info.reference == YUKILINKER_C_REFERENCE_RELEASED);
    assert(result.info.private_mapping_unmapped == (backend == YUKILINKER_C_BACKEND_PRIVATE));
    assert(yukilinker_c_query(runtime, handle, &info) == YUKILINKER_C_INVALID_HANDLE);
  }
#if !defined(YUKILINKER_TEST_CFI)
  options.backend = YUKILINKER_C_BACKEND_PRIVATE;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  assert(fd >= 0);
  yukilinker_c_handle handle = yukilinker_c_open_fd(runtime, fd, &options, NULL);
  assert(handle.generation && fcntl(fd, F_GETFD) >= 0);
  assert(close(fd) == 0);
  assert(!yukilinker_c_close(runtime, handle).handle_valid);
  fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  assert(fd >= 0);
  yukilinker_c_error error = {0};
  handle = yukilinker_c_open_fd(runtime, fd, &options, &error);
  assert(!handle.generation && error.detail.code == YUKILINKER_C_INVALID_ELF);
  assert(!error.detail.execution_started && !error.detail.cleanup_incomplete);
  assert(fcntl(fd, F_GETFD) >= 0 && close(fd) == 0);
#endif
  assert(yukilinker_c_runtime_destroy(runtime));
  assert(!yukilinker_c_runtime_destroy(runtime));
  puts("PASS C API and module ownership");
  return 0;
}

#if !defined(YUKILINKER_TEST_SHARED)
int main(int argc, char** argv) {
  assert(argc == 2);
  return yukilinker_test_run(argv[1]);
}
#endif
