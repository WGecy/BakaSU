// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

// Bootstrap handoff and Tango protection derive from YukiZygisk 509417ca
// (Apache-2.0, Rouyashiki <https://rouyashiki.org>). The load pipeline belongs to this SDK.
#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../internal/runtime_model.hpp"
#include "legacy_packet.hpp"
#include "yukilinker.h"

namespace {
using namespace yukilinker::internal;
using yukilinker::bootstrap::Descriptor;
using yukilinker::bootstrap::Packet;
enum class BootstrapPhase : uint32_t { Ordinary = 1, NativeBinding, NativeBound, NativeActive };
uint64_t process_claim(uint32_t process, BootstrapPhase phase) {
  return (uint64_t{process} << 32) | static_cast<uint32_t>(phase);
}
bool native_claim(uint64_t claim) {
  const auto phase = static_cast<BootstrapPhase>(static_cast<uint32_t>(claim));
  return phase == BootstrapPhase::NativeBinding || phase == BootstrapPhase::NativeBound ||
         phase == BootstrapPhase::NativeActive;
}
// Binding and every entry compete on this same PID/phase word. Native origin
// storage is published by the release transition from NativeBinding to NativeBound.
alignas(8) uint64_t g_claim = 0;
bool g_tango_required = false;
bool g_native_required = false;
void* g_native_reference = nullptr;
yukilinker_c_native_origin g_native_origin{};
yukilinker_c_native_completion* g_native_completion = nullptr;
bool g_native_open = false;
uint32_t g_native_calls = 0;
struct TangoBootstrap {
  uint32_t got, original, stub;
};
struct BootstrapTarget {
  yukilinker_c_core_finalize normal = nullptr;
  yukilinker_c_core_finalize_tango tango = nullptr;
  pid_t process = 0;
  bool rolled_back = false;
};
void loader_anchor() {}

__attribute__((noinline, disable_tail_calls)) void* native_open(
    void*, const yukilinker_c_host_dlopen_request* request, char* error, size_t capacity) {
  if (!request || request->size != sizeof(*request) || !error || !capacity ||
      !__atomic_load_n(&g_native_open, __ATOMIC_ACQUIRE) ||
      static_cast<uint32_t>(getpid()) != g_native_origin.process) {
    if (error && capacity) snprintf(error, capacity, "native bootstrap window is closed");
    return nullptr;
  }
  __atomic_add_fetch(&g_native_calls, 1U, __ATOMIC_ACQ_REL);
  dlerror();
  // Keep the real return address inside this Bionic-registered loader. The
  // explicit post-call bookkeeping also forbids a sibling/tail call.
  void* result = android_dlopen_ext(request->name, request->flags,
                                    static_cast<const android_dlextinfo*>(request->extension));
  const char* message = dlerror();
  snprintf(error, capacity, "%s", message ? message : (result ? "" : "Bionic open failed"));
  __atomic_sub_fetch(&g_native_calls, 1U, __ATOMIC_ACQ_REL);
  return result;
}

void* own_reference(bool& cleanup_complete) {
  Dl_info self{};
  if (!dladdr(reinterpret_cast<void*>(loader_anchor), &self) || !self.dli_fname) return nullptr;
  const char* names[] = {self.dli_fname,
                         sizeof(void*) == 8 ? "libyloader64.so" : "libyloader32.so"};
  for (const char* name : names) {
    void* handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) continue;
    Dl_info provider{};
    void* entry = ::dlsym(handle, "yuki_bootstrap");
    const bool same = entry && dladdr(entry, &provider) && provider.dli_fbase == self.dli_fbase;
    if (::dlclose(handle) != 0) {  // balance only the extra NOLOAD reference
      cleanup_complete = false;
      return nullptr;
    }
    if (same) return handle;  // the caller's original reference remains live
  }
  return nullptr;
}

template <typename T>
T core_symbol(SoHandle* core, const char* name) {
  void* address = yukilinker::internal::dlsym(core, name);
  const auto* image = state_of(core);
  uintptr_t pc = reinterpret_cast<uintptr_t>(address);
#if defined(__arm__)
  pc &= ~uintptr_t{1};
#endif
  if (!image || !address) return nullptr;
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const auto& ph = image->memory.program_headers[i];
    const uintptr_t first = reinterpret_cast<uintptr_t>(image->memory.bias) + ph.p_vaddr;
    if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X) && pc >= first && pc - first < ph.p_memsz)
      return reinterpret_cast<T>(address);
  }
  return nullptr;  // entry points must be executable code in this Core, not an import
}

bool handoff(SoHandle* core, yukilinker_c_core_take_dependencies take, pid_t process) {
  auto* image = state_of(core);
  if (!image || image->tls.active || image->tls.memory_bytes || image != g_first_image ||
      image != g_last_image || image->active_calls || image->snapshot_refs || image->lease_refs ||
      image->references != 1)
    return false;
  size_t count = 0;
  for (auto* dep = image->dependencies; dep; dep = dep->next) {
    if (!dep->system_handle || dep->image || count == SIZE_MAX / sizeof(void*)) return false;
    ++count;
  }
  auto** handles = count ? static_cast<void**>(calloc(count, sizeof(void*))) : nullptr;
  if (count && !handles) return false;
  size_t i = 0;
  for (auto* dep = image->dependencies; dep; dep = dep->next) handles[i++] = dep->system_handle;
  const bool accepted = take(handles, count);
  free(handles);
  if (getpid() != process || !accepted) return false;
  // Transfer first; never close these references while discarding the reduced loader's state.
  for (auto* dep = image->dependencies; dep; dep = dep->next) dep->system_handle = nullptr;
  unregister_image(image);
  core->private_state = nullptr;
  // Core's constructors/finalizers and HWASan shadow remain Core-owned. Metadata only.
  const bool freed = free_image_state(image);
  return shutdown() && freed;
}

struct TangoProtection {
  uintptr_t page = 0;
  size_t size = 0;
  int original = 0;
};
TangoProtection g_tango_protection;

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static bool tango_mapping_protection(uintptr_t page, size_t size, int& protection) {
  FILE* maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr) return false;
  char line[256];
  bool beginning = true;
  bool valid = true;
  bool found = false;
  unsigned long long previous_end = 0;
  while (fgets(line, sizeof(line), maps) != nullptr) {
    const bool complete = strchr(line, '\n') != nullptr;
    if (!beginning) {
      beginning = complete;
      continue;
    }
    beginning = complete;
    unsigned long long low = 0;
    unsigned long long high = 0;
    char permissions[5]{};
    // Kernel-generated maps addresses fit the native address width.
    // NOLINTNEXTLINE(cert-err34-c)
    if (sscanf(line, "%llx-%llx %4s", &low, &high, permissions) != 3 || low >= high ||
        low < previous_end) {
      valid = false;
      break;
    }
    previous_end = high;
    if (page < low || page >= high || size > high - page) continue;
    if (found || permissions[0] != 'r' || permissions[2] != '-' || permissions[3] != 'p' ||
        (permissions[1] != '-' && permissions[1] != 'w')) {
      valid = false;
      break;
    }
    protection = PROT_READ | (permissions[1] == 'w' ? PROT_WRITE : 0);
    found = true;
  }
  valid = valid && beginning && ferror(maps) == 0;
  const int closed = fclose(maps);
  return valid && closed == 0 && found;
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static bool finish_tango_protection() {
  if (g_tango_protection.page == 0) return true;
  int actual = 0;
  if (!tango_mapping_protection(g_tango_protection.page, g_tango_protection.size, actual))
    return false;
  if (actual != g_tango_protection.original) {
    if (actual != (g_tango_protection.original | PROT_WRITE) ||
        mprotect(reinterpret_cast<void*>(g_tango_protection.page), g_tango_protection.size,
                 g_tango_protection.original) != 0)
      return false;
  }
  g_tango_protection = {};
  return true;
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static bool restore_tango_slot(const TangoBootstrap& tango) {
  const long queried = sysconf(_SC_PAGESIZE);
  if (queried <= 0 || (queried & (queried - 1)) != 0 || tango.got == 0 || (tango.got & 3U) != 0 ||
      tango.original == 0 || tango.stub == 0 ||
      tango.stub > UINT32_MAX - YUKILINKER_C_TANGO_BOOTSTRAP_SIZE)
    return false;
  const size_t size = static_cast<size_t>(queried);
  const uintptr_t page = static_cast<uintptr_t>(tango.got) & ~(size - 1);
  if (g_tango_protection.page != 0 &&
      (g_tango_protection.page != page || !finish_tango_protection()))
    return false;
  int protection = 0;
  if (!tango_mapping_protection(page, size, protection)) return false;
  auto* slot = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(tango.got));
  const uint32_t current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (current == tango.original) return true;
  if (current != (tango.stub | 1U)) return false;
  if ((protection & PROT_WRITE) == 0) {
    if (mprotect(reinterpret_cast<void*>(page), size, protection | PROT_WRITE) != 0) return false;
    g_tango_protection = {page, size, protection};
  }
  uint32_t expected = tango.stub | 1U;
  const bool restored = __atomic_compare_exchange_n(slot, &expected, tango.original, false,
                                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  const bool protected_again = g_tango_protection.page == 0 ||
                               mprotect(reinterpret_cast<void*>(page), size, protection) == 0;
  if (protected_again) g_tango_protection = {};
  return protected_again && (restored || expected == tango.original);
}

BootstrapTarget prepare_bootstrap(int core_fd, int packet_argument, const TangoBootstrap* tango,
                                  bool native = false) {
  const uint64_t current = __atomic_load_n(&g_claim, __ATOMIC_ACQUIRE);
  const auto phase = static_cast<BootstrapPhase>(static_cast<uint32_t>(current));
  if (!native && native_claim(current)) {
    errno = EPROTO;
    return {};
  }
  if (native && ((phase != BootstrapPhase::NativeBound && phase != BootstrapPhase::NativeActive) ||
                 g_native_origin.process != static_cast<uint32_t>(getpid()))) {
    errno = EPROTO;
    return {};
  }
  if (core_fd < 0 || packet_argument < 0 || (packet_argument && packet_argument - 1 == core_fd)) {
    errno = EINVAL;
    return {};
  }
  const pid_t process = getpid();
  uint64_t expected = native ? process_claim(process, BootstrapPhase::NativeBound) : 0;
  const uint64_t claim =
      process_claim(process, native ? BootstrapPhase::NativeActive : BootstrapPhase::Ordinary);
  if (!__atomic_compare_exchange_n(&g_claim, &expected, claim, false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    errno = !native && native_claim(expected) ? EPROTO : EBUSY;
    return {};  // no ownership acquired; a fork child's descriptors are untouched too
  }
  BootstrapTarget target;
  target.process = process;
  const auto rollback = [&] {
    target.rolled_back = true;
    // Native returns its reference to the injector. Keep admission closed while
    // publishing that outcome and until the external owner releases the loader.
    if (!native) __atomic_store_n(&g_claim, uint64_t{0}, __ATOMIC_RELEASE);
  };
  Descriptor source;
  Packet packet;
  if (!source.capture(core_fd)) {
    rollback();
    return target;
  }
  if (!packet.capture(packet_argument ? packet_argument - 1 : -1, core_fd)) {
    const bool closed = source.close_owned();
    if (packet.close_owned() && closed) rollback();
    errno = EPROTO;
    return target;
  }
  bool reference_cleanup_complete = true;
  void* loader = native ? g_native_reference : own_reference(reference_cleanup_complete);
  if (!loader || (tango && !restore_tango_slot(*tango))) {
    const bool source_closed = source.close_owned();
    const bool packet_closed = packet.close_owned();
    // A failed Tango protection restoration must not be forgotten.
    if (reference_cleanup_complete && source_closed && packet_closed &&
        (!tango || finish_tango_protection()))
      rollback();
    errno = EIO;
    return target;
  }
  const char* name = sizeof(void*) == 8 ? "/data/adb/yukizygisk/lib/libzygisk64.so"
                                        : "/data/adb/yukizygisk/lib/libzygisk32.so";
  LoadOptions options{};
  options.size = sizeof(options);
  options.flags = RTLD_NOW;
  options.trusted_runtime = 1;  // Core imports must not bind to stage-one stubs
  options.origin = name;
  yukilinker_c_error_detail error{};
  g_tango_required = tango != nullptr;
  g_native_required = native;
  auto* core = static_cast<SoHandle*>(loader_open_fd(core_fd, &options, &error));
  if (getpid() != process) {
    errno = EBUSY;
    return target;
  }
  const bool source_closed = source.close_owned();
  if (!core) {
    const bool packet_closed = packet.close_owned();
    if (source_closed && packet_closed && !error.execution_started && !error.cleanup_incomplete &&
        shutdown())
      rollback();
    errno = error.system_errno ? error.system_errno : ENOEXEC;
    return target;
  }
  if (!source_closed) {
    packet.close_owned();
    errno = EIO;
    return target;
  }
  const auto entry = core_symbol<yukilinker_c_core_entry>(core, "zygisk_core_entry");
  const auto adopt = core_symbol<yukilinker_c_core_adopt>(core, "zygisk_adopt_anonymous_image");
  const auto ready = core_symbol<yukilinker_c_core_ready>(core, "zygisk_bootstrap_handoff_ready");
  const auto take =
      core_symbol<yukilinker_c_core_take_dependencies>(core, "zygisk_bootstrap_take_dependencies");
  const auto finish =
      core_symbol<yukilinker_c_core_finish_handoff>(core, "zygisk_bootstrap_finish_handoff");
  const auto finalizer = core_symbol<yukilinker_c_core_finalize>(core, "zygisk_finalize_loader");
  const auto finalizer_tango =
      core_symbol<yukilinker_c_core_finalize_tango>(core, "zygisk_finalize_loader_tango");
  const auto set_tango = core_symbol<yukilinker_c_core_set_tango>(core, "zygisk_set_tango_stub");
  const auto bind_native =
      core_symbol<yukilinker_c_core_bind_native>(core, "zygisk_bootstrap_bind_native_v1");
  if (!entry || !adopt || !ready || !take || !finish || !finalizer || (native && !bind_native) ||
      (tango && (!finalizer_tango || !set_tango))) {
    packet.close_owned();
    errno = ENOEXEC;  // constructors ran: preserve Core and its dependencies
    return target;
  }
  if (tango && !set_tango(tango->stub, YUKILINKER_C_TANGO_BOOTSTRAP_SIZE)) {
    packet.close_owned();
    errno = EBUSY;
    return target;
  }
  if (tango && getpid() != process) {
    errno = EBUSY;
    return target;  // a child must not adopt the parent's in-flight mapping/reference
  }
  const uintptr_t bias = reinterpret_cast<uintptr_t>(core->load_bias);
  const size_t span = core->map_size;
  if (native) {
    const yukilinker_c_native_services services{sizeof(services),
                                                YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION,
                                                g_native_origin, nullptr, native_open};
    if (!bind_native(&services) || getpid() != process) {
      packet.close_owned();
      errno = EPROTO;
      return target;
    }
  }
  if (!adopt(bias, span, loader)) {
    packet.close_owned();
    errno = EBUSY;
    return target;
  }
  if (getpid() != process) {
    errno = EBUSY;
    return target;
  }
  if (native) g_native_reference = nullptr;  // Core now owns the original reference.
  // From this call the Core owns the packet, even when initialization refuses readiness.
  __atomic_store_n(&g_native_open, native, __ATOMIC_RELEASE);
  entry(name, reinterpret_cast<void*>(loader_anchor), reinterpret_cast<void*>(bias),
        reinterpret_cast<void*>(span), packet_argument);
  __atomic_store_n(&g_native_open, false, __ATOMIC_RELEASE);
  if (__atomic_load_n(&g_native_calls, __ATOMIC_ACQUIRE) || getpid() != process || !ready() ||
      getpid() != process || !handoff(core, take, process) || !finish(true) ||
      getpid() != process) {
    errno = EBUSY;
    return target;
  }
  errno = 0;
  target.normal = finalizer;
  target.tango = finalizer_tango;
  return target;
}
}  // namespace

namespace yukilinker::internal {
bool validate_bootstrap_core(ImageState* image) {
  // The original adoption ABI uses one value as ELF bias and mapping start.
  if (image->memory.lowest_vaddr || image->tls.active || image->tls.memory_bytes)
    return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
  Dl_info self{};
  if (!::dladdr(reinterpret_cast<void*>(loader_anchor), &self))
    return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
  for (auto* dep = image->dependencies; dep; dep = dep->next) {
    if (!dep->system_handle || dep->image)
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_DEPENDENCIES);
    // A dependency on stage one (including a transitive one) would keep it
    // mapped after the Core finalizer released the original reference.
    void* entry = ::dlsym(dep->system_handle, "yuki_bootstrap");
    Dl_info provider{};
    if (entry && ::dladdr(entry, &provider) && provider.dli_fbase == self.dli_fbase)
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_DEPENDENCIES, "Core depends on bootstrap");
  }
  const char* names[] = {"zygisk_core_entry",
                         "zygisk_adopt_anonymous_image",
                         "zygisk_bootstrap_handoff_ready",
                         "zygisk_bootstrap_take_dependencies",
                         "zygisk_bootstrap_finish_handoff",
                         "zygisk_finalize_loader",
                         "zygisk_set_tango_stub",
                         "zygisk_finalize_loader_tango",
                         "zygisk_bootstrap_bind_native_v1"};
  for (size_t i = 0; i < 9; ++i) {
    if ((i == 6 || i == 7) && !g_tango_required) continue;
    if (i == 8 && !g_native_required) continue;
    const auto* symbol = find_defined_symbol(image, names[i]);
    bool executable = false;
    if (symbol && ELF64_ST_TYPE(symbol->st_info) == STT_FUNC) {
      uintptr_t address = symbol->st_value;
#if defined(__arm__)
      address &= ~uintptr_t{1};
#endif
      for (size_t j = 0; j < image->memory.program_header_count; ++j) {
        const auto& ph = image->memory.program_headers[j];
        executable |= ph.p_type == PT_LOAD && (ph.p_flags & PF_X) && address >= ph.p_vaddr &&
                      address - ph.p_vaddr < ph.p_memsz;
      }
    }
    if (!executable) return fail(YUKILINKER_C_SYMBOL_MISSING, YUKILINKER_C_METADATA, names[i]);
  }
  return true;
}
}  // namespace yukilinker::internal

extern "C" [[gnu::visibility("default")]] void yuki_bootstrap(int core_fd, int packet_argument) {
  const auto target = prepare_bootstrap(core_fd, packet_argument, nullptr);
  if (target.normal) [[clang::musttail]]
    return target.normal(0, 0);
}
extern "C" [[gnu::visibility("default")]] bool yuki_bootstrap_bind_native_v1(
    const yukilinker_c_native_binding* binding) {
  if (!binding || binding->size != sizeof(*binding) ||
      binding->version != YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION ||
      binding->origin.size != sizeof(binding->origin) ||
      binding->origin.version != YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION ||
      binding->origin.process != static_cast<uint32_t>(getpid()) || !binding->origin.generation ||
      !binding->loader_reference || !binding->completion ||
      binding->completion->size != sizeof(*binding->completion) ||
      binding->completion->version != YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION ||
      binding->completion->process != binding->origin.process ||
      binding->completion->generation != binding->origin.generation ||
      !binding->origin.linker_base || !binding->origin.libc_base || !binding->origin.libc_anchor ||
      !memchr(binding->origin.libc_path, 0, sizeof(binding->origin.libc_path)) ||
      __atomic_load_n(&g_claim, __ATOMIC_ACQUIRE))
    return false;
  Dl_info provider{};
  if (::dlsym(binding->loader_reference, "yuki_bootstrap_native_v1") !=
          reinterpret_cast<void*>(yuki_bootstrap_native_v1) ||
      ::dlsym(binding->loader_reference, "getpid") !=
          reinterpret_cast<void*>(static_cast<uintptr_t>(binding->origin.libc_anchor)) ||
      !::dladdr(reinterpret_cast<void*>(static_cast<uintptr_t>(binding->origin.libc_anchor)),
                &provider) ||
      reinterpret_cast<uintptr_t>(provider.dli_fbase) != binding->origin.libc_base ||
      !provider.dli_fname || strcmp(provider.dli_fname, binding->origin.libc_path))
    return false;
  uint64_t expected = 0;
  const uint64_t claim = process_claim(binding->origin.process, BootstrapPhase::NativeBinding);
  if (!__atomic_compare_exchange_n(&g_claim, &expected, claim, false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE))
    return false;
  g_native_origin = binding->origin;
  g_native_reference = binding->loader_reference;
  g_native_completion = binding->completion;
  g_native_completion->ownership = YUKILINKER_C_NATIVE_LOADER_RETAINED;
  __atomic_store_n(&g_claim, process_claim(binding->origin.process, BootstrapPhase::NativeBound),
                   __ATOMIC_RELEASE);
  return true;
}

extern "C" [[gnu::visibility("default")]] void yuki_bootstrap_native_v1(int core_fd,
                                                                        int packet_argument) {
  const auto target = prepare_bootstrap(core_fd, packet_argument, nullptr, true);
  if (target.process != getpid()) return;
  auto* completion = g_native_completion;
  g_native_completion = nullptr;
  if (completion) {
    completion->error = target.normal ? 0 : (errno ? errno : EIO);
    completion->ownership = g_native_reference ? YUKILINKER_C_NATIVE_LOADER_RETAINED
                                               : YUKILINKER_C_NATIVE_LOADER_ADOPTED;
    // Only the admitted operation can return its reference after a complete
    // rollback; never dlclose our own return chain.
    if (g_native_reference && target.rolled_back) {
      completion->ownership = YUKILINKER_C_NATIVE_LOADER_RETURNED;
      g_native_reference = nullptr;
    }
  }
  if (target.normal) [[clang::musttail]]
    return target.normal(0, 0);
}
#if defined(__arm__)
extern "C" [[gnu::visibility("default")]] void yuki_bootstrap_tango(uint32_t got, uint32_t original,
                                                                    uint32_t stub, int core_fd,
                                                                    int packet_argument) {
  const TangoBootstrap tango{got, original, stub};
  const auto target = prepare_bootstrap(core_fd, packet_argument, &tango);
  if (target.tango) [[clang::musttail]]
    return target.tango(0, 0, 0, 0, 0);
}
#endif
