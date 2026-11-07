// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once
#include <elf.h>
#include <link.h>
#include <pthread.h>

#include <cstddef>
#include <cstdint>

#include "cleanup_state.hpp"
#include "config.hpp"

namespace yukilinker::internal {

struct ImageState;
class RelocationTargets;

#if defined(__LP64__)
using ElfRelocation = ElfW(Rela);
constexpr ElfW(Sxword) kDynamicRelTag = DT_RELA;
constexpr ElfW(Sxword) kDynamicRelSizeTag = DT_RELASZ;
constexpr ElfW(Sxword) kDynamicRelEntryTag = DT_RELAENT;
// The other ABI's table and its size, which the system linker refuses rather than ignores.
constexpr ElfW(Sxword) kForeignRelTag = DT_REL;
constexpr ElfW(Sxword) kForeignRelSizeTag = DT_RELSZ;
#else
using ElfRelocation = ElfW(Rel);
constexpr ElfW(Sxword) kDynamicRelTag = DT_REL;
constexpr ElfW(Sxword) kDynamicRelSizeTag = DT_RELSZ;
constexpr ElfW(Sxword) kDynamicRelEntryTag = DT_RELENT;
constexpr ElfW(Sxword) kForeignRelTag = DT_RELA;
constexpr ElfW(Sxword) kForeignRelSizeTag = DT_RELASZ;
#endif

struct SourceLayout {
  const ElfW(Ehdr) * header = nullptr;
  const ElfW(Phdr) * program_headers = nullptr;
  size_t program_header_count = 0;
  uintptr_t lowest_vaddr = 0;
  uintptr_t highest_vaddr = 0;
  size_t dynamic_index = SIZE_MAX;
  bool bti_compatible = false;
};

struct DynamicParse {
  size_t runpath_offset = SIZE_MAX;
  size_t soname_offset = SIZE_MAX;
  const ElfW(Dyn) * entries = nullptr;
  size_t entry_limit = 0;
  const uint32_t* sysv_hash = nullptr;
  const uint32_t* gnu_hash = nullptr;
  size_t init_array_bytes = 0;
  size_t fini_array_bytes = 0;
  bool terminated = false;
};

struct SymbolResolution {
  uintptr_t address = 0;
  bool valid = false;
};

struct SymbolTarget {
  ImageState* image = nullptr;
  const ElfW(Sym) * symbol = nullptr;
  void* address = nullptr;
  bool found = false;
  // The search itself failed (no memory for its queue); the error is recorded. Not "missing":
  // a weak reference must not bind to 0 for it.
  bool failed = false;
};

enum class GlobalLookup { None, Dynamic, Relocation };

using LifecycleFunction = void (*)();

struct Lifecycle {
  LifecycleFunction init = nullptr;
  LifecycleFunction* init_array = nullptr;
  size_t init_count = 0;
  LifecycleFunction fini = nullptr;
  LifecycleFunction* fini_array = nullptr;
  size_t fini_count = 0;
  // Set once the constructors ran; cleared only once every finalizer step has run.
  bool initialized = false;
  bool process_exit_registered = false;
  bool hwasan_globals = false;
  bool hwasan_registered = false;
  // Its address is this image's libc DSO cookie. The image stays registered and
  // pinned until removal returns; the callback argument remains a generation.
  CleanupCall process_exit;
  // Finalization has begun; fini_steps_left are the steps not begun yet: fini_array from its last
  // entry, then DT_FINI (step 0). Each step is taken off before it runs, so a thread that ends
  // inside one (pthread_exit) leaves the next run to go on from the step after, and none runs
  // twice. The image's __cxa_atexit callbacks come last, each unlinked before it runs.
  bool finalizing = false;
  size_t fini_steps_left = 0;
  // A run is under way, on fini_thread; another run (process exit, a collection) waits its turn.
  // A thread that ends inside a step, or a fork child without that thread, clears it.
  bool fini_running = false;
  pthread_t fini_thread{};
  // A usual CRT needs one cookie, without a separate allocation. Reentrant or
  // multiple-cookie finalization uses metadata nodes that outlive the mapping.
  DsoCleanup dso_cleanup;
  uint64_t blocked_cleanup_pass = 0;
  bool cleanup_record_failed = false;
};

// The first definition of a version name, in an open-addressed table keyed by the name's hash.
struct VersionSlot {
  const char* name;
  uint32_t index;
};

// The name one version index stands for, in a table sorted by index: the compact form of a
// version table whose indexes are too sparse for a direct one. order is the record's place in
// its chain, which breaks ties so the first name of an index sorts first.
struct VersionName {
  uint32_t index;
  uint32_t order;
  const char* name;
};

// The names of one image's version indexes (Vernaux or Verdef), the first in chain order for
// each. A direct table holds count names, one per index from 0; a compact one holds count
// VersionName entries sorted by index, one per index that names anything, and is searched by
// bisection. The form is chosen once, when the table is built.
struct VersionTable {
  const char** direct = nullptr;
  const VersionName* compact = nullptr;
  size_t count = 0;
};

struct DynamicSymbols {
  const ElfW(Half) * versions = nullptr;
  const ElfW(Verneed) * needs = nullptr;
  const ElfW(Verdef) * definitions = nullptr;
  size_t need_count = 0;
  size_t definition_count = 0;
  // Built once by validate_versions over the validated chains, so each version check is one
  // step, or a bisection of a sparse table, instead of a walk: the name each Vernaux and Verdef
  // index stands for (the first in chain order, null where none), and the first Verdef index of
  // each name.
  VersionTable need_names;
  VersionTable definition_names;
  VersionSlot* definition_slots = nullptr;
  size_t definition_slot_mask = 0;
  const ElfW(Sym) * entries = nullptr;
  size_t count = 0;
  const char* strings = nullptr;
  size_t string_bytes = 0;
  // One past the last NUL of the string table (0: none), found once by parse_dynamic_tags: a name
  // at an offset below it ends inside the table, so no lookup scans for its end again.
  size_t string_terminated = 0;
  // The image's hash tables, which find_defined_symbol reads; either may be absent.
  const uint32_t* sysv_hash = nullptr;
  const uint32_t* gnu_hash = nullptr;
};

// An IRELATIVE relocation, or one whose target is an IFUNC. Pass 1 resolves it and pass 2 runs
// the resolver; the provider is null for IRELATIVE.
struct DeferredRelocation {
  ElfW(Addr) offset;
  ElfW(Sxword) addend;
  ImageState* provider;
  const ElfW(Sym) * symbol;
  uint32_t type;
};
// A readable and writable PT_LOAD's bytes, [begin, end) of the image's addresses.
struct WritableRange {
  uintptr_t begin;
  uintptr_t end;
};
struct RelocationSet {
  // The bytes pass -1 saw written so far, while it runs (relocate_image's own).
  RelocationTargets* targets = nullptr;
  // The readable and writable segments that map bytes, by address, disjoint (elf::inspect keeps
  // them off each other's pages): a relocation's segment is found by binary search, however many
  // program headers the image has. Set up by pass -1; the first kInlineRanges live here, more in
  // the image's metadata.
  static constexpr size_t kInlineRanges = 4;
  WritableRange inline_ranges[kInlineRanges] = {};
  WritableRange* writable = nullptr;
  size_t writable_count = 0;
  // The range the last relocation wrote into, checked first by the next one. Only the loading
  // thread relocates an image.
  size_t writable_hint = 0;
  // Pass -1 found a TLS relocation, or has not run. Pass 0 checks only those, so an image whose
  // tables pass -1 found without one skips it.
  bool tls = true;
  DeferredRelocation* deferred = nullptr;
  size_t deferred_count = 0;
  size_t deferred_capacity = 0;
  const uint8_t* packed = nullptr;
  size_t packed_bytes = 0;
  const ElfRelocation* rel = nullptr;
  size_t rel_bytes = 0;
  const ElfRelocation* plt = nullptr;
  size_t plt_bytes = 0;
  const ElfW(Addr) * relr = nullptr;
  size_t relr_bytes = 0;
};

}  // namespace yukilinker::internal
