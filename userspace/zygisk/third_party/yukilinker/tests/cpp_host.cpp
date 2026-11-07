// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// clang-format off: exercise mixed and repeated inclusion.
#include <yukilinker.h>
#include <yukilinker.hpp>
#include <yukilinker.hpp>
#include <yukilinker.h>
// clang-format on

#include <utility>

#include "host_services.h"

#if defined(YUKILINKER_TEST_NOEXCEPT)
#if defined(__EXCEPTIONS) || defined(__GXX_RTTI)
#error Consumer language flags were overridden
#endif
#else
#if !defined(__EXCEPTIONS) || !defined(__GXX_RTTI)
#error Runtime flags leaked into the consumer
#endif
#endif
static_assert(!std::is_copy_constructible_v<yukilinker::Module>);
static_assert(!std::is_move_assignable_v<yukilinker::Module>);
static_assert(!std::is_move_assignable_v<yukilinker::Runtime>);

#if !defined(YUKILINKER_TEST_CFI)
void exercise_lifecycle(yukilinker::Runtime& runtime, const char* directory) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/liblinker_lifecycle.so", directory);
  auto loaded = runtime.open_path(path);
  assert(loaded);
  auto& module = loaded.value;
  struct Thread {
    int* (*slot)();
    int (*on_exit)(int*);
    int calls = 0;
  } work{module.symbol<int* (*)()>("module_slot").value,
         module.symbol<int (*)(int*)>("module_thread_exit").value};
  int exits = 0, forks = 0;
  assert(module.symbol<int (*)(int*)>("module_exit").value(&exits) == 0);
  assert(module.symbol<int (*)(int*)>("module_fork").value(&forks) == 0);
  pthread_t thread;
  assert(pthread_create(
             &thread, nullptr,
             [](void* opaque) -> void* {
               auto* value = static_cast<Thread*>(opaque);
               assert(*value->slot() == 42 && value->on_exit(&value->calls) == 0);
               return nullptr;
             },
             &work) == 0);
  assert(pthread_join(thread, nullptr) == 0 && work.calls == 1);
  yukilinker_c_prepare_fork(runtime.native_handle());
  const pid_t child = fork();
  assert(child >= 0);
  yukilinker_c_after_fork(runtime.native_handle(), child == 0);
  if (!child) _exit(forks == 1 ? 0 : 1);
  int status = 0;
  assert(waitpid(child, &status, 0) == child && status == 0);
  assert(!module.close().handle_valid() && exits == 1);
  const pid_t retired = fork();
  assert(retired >= 0);
  if (!retired) _exit(forks == 0 ? 0 : 1);
  assert(waitpid(retired, &status, 0) == retired && status == 0);
}
#endif

namespace {
unsigned diagnostics;
void* fail_mapping;
yukilinker_c_runtime* transfer_owner;
yukilinker_c_handle transfer_handle;
bool transfer_active;
unsigned transfer_reentries;
void log_unreleased(void*, uint32_t, const char*, size_t) { ++diagnostics; }
int close_dependency(void* user, void* dependency) {
  if (transfer_active) {
    transfer_active = false;
    const auto refused = yukilinker_c_close(transfer_owner, transfer_handle);
    assert(refused.handle_valid && refused.error.detail.code == YUKILINKER_C_BUSY);
    assert(refused.info.reference == YUKILINKER_C_REFERENCE_OWNED);
    assert(yukilinker_c_release_host(transfer_owner, transfer_handle) == YUKILINKER_C_BUSY);
    ++transfer_reentries;
  }
  return test_close(user, dependency);
}
}  // namespace
extern "C" int __real_munmap(void*, size_t);
extern "C" int __wrap_munmap(void* mapping, size_t bytes) {
  if (mapping == fail_mapping && mapping) {
    fail_mapping = nullptr;
    errno = ENOMEM;
    return -1;
  }
  return __real_munmap(mapping, bytes);
}

extern "C" __attribute__((visibility("default"))) int yukilinker_test_run(const char* directory) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/liblinker_module.so", directory);
  auto services = test_host_services();
  services.log = log_unreleased;
  services.system_dlclose = close_dependency;
  auto source = yukilinker::Runtime::create(services);
  assert(source);
  auto runtime = std::move(source);
  assert(!source && runtime);
#if defined(YUKILINKER_TEST_CFI)
  auto refused = runtime.open_path(path);
  assert(!refused && refused.error.code() == yukilinker::Code::Unsupported);
  auto module = runtime.open_path(path, yukilinker::OpenOptions{yukilinker::Backend::System});
  assert(module && module.value.symbol<int (*)()>("module_answer").value() == 42);
  auto address = module.value.symbol<int (*)()>("module_answer").value;
  const pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    signal(SIGILL, [](int) { _exit(90); });
    signal(SIGTRAP, [](int) { _exit(90); });
    reinterpret_cast<int (*)(int)>(address)(7);
    _exit(91);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 90);
  assert(!module.value.close().handle_valid() && runtime.destroy());
  puts("PASS C++ SYSTEM Core and CFI rejection");
  return 0;
#else
  auto context_source = runtime.context(directory);
  assert(context_source);
  auto context = std::move(context_source);
  assert(!context_source);
  auto namespace_source = runtime.namespace_create(&context);
  assert(namespace_source);
  auto scope = std::move(namespace_source);
  assert(!namespace_source);
  assert(scope.link(nullptr, YUKILINKER_C_NAMESPACE_LINK_SYSTEM |
                                 YUKILINKER_C_NAMESPACE_LINK_ALL) == yukilinker::Code::Ok);
  yukilinker::OpenOptions options;
  options.native.namespace_scope = scope.native_handle();
  auto opened = runtime.open_path(path, options);
  if (!opened)
    fprintf(stderr, "open failed %u/%u %s %s\n", opened.error.native.detail.code,
            opened.error.native.detail.stage, opened.error.native.detail.object,
            opened.error.native.detail.symbol);
  assert(opened && opened.value);
  auto module = std::move(opened.value);
  assert(!opened.value);
  yukilinker::Module swapped;
  swapped.swap(module);
  assert(!module && swapped);
  auto symbol = swapped.symbol<int (*)()>("module_answer");
  assert(symbol && symbol.value() == 42);
  auto record = swapped.image();
  assert(record);
  auto snapshot = runtime.snapshot(nullptr, &scope);
  assert(snapshot && snapshot.value.size());
  auto moved_snapshot = std::move(snapshot.value);
  assert(!snapshot.value);
  auto image = moved_snapshot.image(0);
  assert(image);
  auto lease = runtime.lease(record.value.id, yukilinker::LeaseKind::Execution);
  assert(lease);
  auto moved_lease = std::move(lease.value);
  assert(!lease.value);
  auto closed = swapped.close();
  assert(closed.handle_valid() && closed.reference() == yukilinker::Reference::Released);
  assert(closed.retention_reasons() && !closed.private_mapping_unmapped());
  assert(!runtime.destroy());
  assert(moved_snapshot.release() == yukilinker::Code::Ok);
  // The final lease release consumes its token despite a subsequent failed unmap.
  // A close of the retained module then retries cleanup without dropping a reference twice.
  fail_mapping = reinterpret_cast<void*>(record.value.mapping_start);
  assert(moved_lease.release() == yukilinker::Code::IoError);
  assert(!moved_lease && swapped);
  closed = swapped.close();
  assert(!closed.handle_valid() && closed.private_mapping_unmapped() && !swapped);

  // A missed destructor release must leave the original ownership recoverable.
  yukilinker_c_handle retained{};
  const auto before = diagnostics;
  {
    auto missed = runtime.open_path(path, options);
    assert(missed);
    retained = missed.value.native_handle();
  }
  assert(diagnostics == before + 1);
  auto stats = runtime.stats();
  assert(stats && stats.value.handles == 1);
  auto recovered = yukilinker::Module::adopt(runtime.native_handle(), retained);
  assert(recovered.symbol<int (*)()>("module_answer").value() == 42);
  assert(!recovered.close().handle_valid());

  auto private_again = runtime.open_path(path, options);
  assert(private_again);
  auto held = runtime.lease(private_again.value.image().value.id);
  assert(held);
  assert(private_again.value.close().handle_valid());
  assert(private_again.value.forget() == yukilinker::Code::Ok && !private_again.value);
  assert(held.value.release() == yukilinker::Code::Ok);
  exercise_lifecycle(runtime, directory);
  assert(scope.destroy() == yukilinker::Code::Ok && !scope);
  assert(context.destroy() && !context);
  auto system = runtime.open_path(path, yukilinker::OpenOptions{yukilinker::Backend::System});
  assert(system);
  closed = system.value.close();
  assert(!closed.handle_valid() && closed.reference() == yukilinker::Reference::Released);
  assert(!closed.private_mapping_unmapped());
  yukilinker::OpenOptions trusted;
  trusted.native.trusted_runtime = 1;
  auto transferred = runtime.open_path(path, trusted);
  assert(transferred);
  const auto mapping = transferred.value.image();
  const auto answer = transferred.value.symbol<int (*)()>("module_answer");
  assert(mapping && answer);
  transfer_owner = runtime.native_handle();
  transfer_handle = transferred.value.native_handle();
  transfer_active = true;
  assert(transferred.value.release_host() == yukilinker::Code::Ok);
  assert(!transfer_active && transfer_reentries == 1 && answer.value() == 42);
  assert(munmap(reinterpret_cast<void*>(mapping.value.mapping_start), mapping.value.mapping_size) ==
         0);
  auto* identity = runtime.native_handle();
  const auto owner_diagnostics = diagnostics;
  {
    auto forgotten = std::move(runtime);
    assert(forgotten && !runtime);
  }
  assert(diagnostics == owner_diagnostics + 1);
  auto restored = yukilinker::Runtime::adopt(identity);
  runtime.swap(restored);
  assert(runtime.stats());
  assert(runtime.try_shutdown().complete);
  assert(runtime.destroy() && !runtime);
  puts("PASS C++ ownership, cleanup retry and lifecycle");
  return 0;
#endif
}

#if !defined(YUKILINKER_TEST_SHARED)
int main(int argc, char** argv) {
  assert(argc == 2);
  return yukilinker_test_run(argv[1]);
}
#endif
