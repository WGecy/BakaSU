// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/relocation_targets.hpp"
#include "../internal/runtime_model.hpp"
#if defined(YUKILINKER_RUNTIME)
#include "../internal/caller_stubs.hpp"
#include "../internal/host_imports.hpp"
#endif

namespace yukilinker::internal {

uintptr_t invoke_ifunc(ImageState* image, uintptr_t resolver) {
  CallbackGuard callback_scope(image);
  callback_scope.runs_image_code = true;
#if defined(__aarch64__)
  __ifunc_arg_t arguments{};
  arguments._size = sizeof(arguments);
  arguments._hwcap = getauxval(AT_HWCAP);
  arguments._hwcap2 = getauxval(AT_HWCAP2);
  arguments._size = offsetof(__ifunc_arg_t, _hwcap2) + sizeof(arguments._hwcap2);
  return call_module(resolver, arguments._hwcap | _IFUNC_ARG_HWCAP,
                     reinterpret_cast<uintptr_t>(&arguments));
#else
  // Bionic's arm32 resolvers take AT_HWCAP.
  return call_module(resolver, getauxval(AT_HWCAP), 0);
#endif  // #if defined(__aarch64__)
}

SymbolResolution materialize_defined_symbol(ImageState* image, const ElfW(Sym) & symbol) {
  if (symbol.st_shndx == SHN_UNDEF) return {};
  uintptr_t address;
  if (symbol.st_shndx == SHN_ABS) {
    address = symbol.st_value;
  } else {
    auto* pointer = runtime_pointer<uint8_t>(image, symbol.st_value);
    if (pointer != nullptr) {
      address = reinterpret_cast<uintptr_t>(pointer);
    } else {
      // A function may live in a legal execute-only segment. The metadata
      // reader needs PF_R; materializing a function address does not read it.
      const auto type = ELF64_ST_TYPE(symbol.st_info);
      if ((type != STT_FUNC && type != STT_GNU_IFUNC) ||
          __builtin_add_overflow(reinterpret_cast<uintptr_t>(image->memory.bias),
                                 static_cast<uintptr_t>(symbol.st_value), &address) ||
          !executable_pointer(image, address))
        return {};
    }
  }
  if (ELF64_ST_TYPE(symbol.st_info) == STT_GNU_IFUNC) {
    if (!executable_pointer(image, address)) return {};
    image->executed = true;
    if (g_operation_error) g_operation_error->execution_started = 1;
    address = invoke_ifunc(image, address);
  }
  return {address, true};
}

namespace {

struct ScopeNode {
  ImageState* image;
  void* system;
};

// One breadth-first walk's queue, as Bionic's walk_dependencies_tree keeps, without a cap. It
// belongs to that walk alone: the walk drops the lock for a system lookup, and a lookup that runs
// meanwhile, on this thread or another, has its own. The first kInlineNodes nodes live in the
// walk's frame, so a small scope allocates nothing; a wider one moves to the heap and doubles. A
// hash index over the queue (at most half full) queues each node once.
class ScopeQueue {
 public:
  ScopeQueue() = default;
  ~ScopeQueue() {
    if (nodes_ != inline_nodes_) free(nodes_);
  }
  ScopeQueue(const ScopeQueue&) = delete;
  ScopeQueue& operator=(const ScopeQueue&) = delete;
  ScopeQueue(ScopeQueue&&) = delete;
  ScopeQueue& operator=(ScopeQueue&&) = delete;

  size_t count() const { return count_; }
  ScopeNode at(size_t index) const { return nodes_[index]; }

  // Queues the node unless it is queued already. False only when the queue cannot grow.
  bool push(ImageState* image, void* system) {
    const ScopeNode node{image, system};
    if (*slot_for(node) != 0) return true;
    if (count_ == capacity_ && !grow()) return false;
    nodes_[count_] = node;
    *slot_for(node) = ++count_;
    return true;
  }

 private:
  static constexpr size_t kInlineNodes = 16;

  static size_t hash(const ScopeNode& node) {
    const uint64_t value =
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(node.image)) * 0x9e3779b97f4a7c15ULL) ^
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(node.system)) * 0xc2b2ae3d27d4eb4fULL);
    return static_cast<size_t>(value ^ (value >> 29));
  }

  // The slot that indexes node, or the empty one where its index goes.
  size_t* slot_for(const ScopeNode& node) {
    const size_t mask = (2 * capacity_) - 1;
    for (size_t slot = hash(node) & mask;; slot = (slot + 1) & mask) {
      const size_t entry = slots_[slot];
      if (entry == 0 ||
          (nodes_[entry - 1].image == node.image && nodes_[entry - 1].system == node.system))
        return &slots_[slot];
    }
  }

  // Twice the nodes in one heap block, then the index rebuilt over them.
  bool grow() {
    // A node and its two index slots.
    constexpr size_t kNodeBytes = sizeof(ScopeNode) + (2 * sizeof(size_t));
    // capacity_ starts at kInlineNodes and only doubles.
    if (capacity_ < kInlineNodes || capacity_ > SIZE_MAX / 2 / kNodeBytes) {
      errno = ENOMEM;
      return false;
    }
    const size_t capacity = capacity_ * 2;
    const size_t node_bytes = capacity * sizeof(ScopeNode);
    const size_t bytes = capacity * kNodeBytes;
    auto* block = static_cast<unsigned char*>(calloc(1, bytes));
    if (block == nullptr) return false;
    auto* nodes = reinterpret_cast<ScopeNode*>(block);
    memcpy(nodes, nodes_, count_ * sizeof(ScopeNode));
    if (nodes_ != inline_nodes_) free(nodes_);
    nodes_ = nodes;
    slots_ = reinterpret_cast<size_t*>(block + node_bytes);
    capacity_ = capacity;
    for (size_t i = 0; i < count_; ++i) *slot_for(nodes_[i]) = i + 1;
    return true;
  }

  // Only nodes_[0, count_) is read, so the inline nodes are not cleared.
  ScopeNode inline_nodes_[kInlineNodes];
  size_t inline_slots_[2 * kInlineNodes]{};
  // capacity_ nodes, and 2 * capacity_ slots holding a node's index + 1 (0: empty).
  ScopeNode* nodes_ = inline_nodes_;
  size_t* slots_ = inline_slots_;
  size_t count_ = 0;
  size_t capacity_ = kInlineNodes;
};

// A walk that could not grow its queue. A relocation's lookup fails at its stage, any other at
// the lookup's.
SymbolTarget scope_failed(GlobalLookup globals) {
  fail(YUKILINKER_C_NO_MEMORY,
       globals == GlobalLookup::Relocation ? YUKILINKER_C_RELOCATIONS : YUKILINKER_C_METADATA);
  SymbolTarget target;
  target.failed = true;
  return target;
}

// The breadth-first walk of a lookup's local scope from root, shared by search_scope and
// walk_handle_scope so that a lookup and the imports it may see cover the same images. Each node
// goes to visit (a system object or a private image) once; a private image of another context (a
// link's target) is visited, but its edges are not followed, as in the system linker. Returns
// what visit returns once it stops the walk, empty when the walk ends, and grown_out when the
// queue cannot grow.
template <typename Visit>
SymbolTarget walk_scope(ImageState* root, const SymbolTarget& grown_out, Visit visit) {
  ScopeQueue queue;
  if (!queue.push(root, nullptr)) return grown_out;
  for (size_t index = 0; index < queue.count(); ++index) {
    // A copy: the queue may move as it grows.
    const ScopeNode node = queue.at(index);
    bool stop = false;
    SymbolTarget found = visit(node, &stop);
    if (stop) return found;
    if (node.system || node.image->context != root->context) continue;
    for (auto* dependency = node.image->dependencies; dependency; dependency = dependency->next) {
      // An edge that is not resolved yet, or whose private target retired, adds no scope.
      if (!dependency->image && !dependency->system_handle) continue;
      if (!queue.push(dependency->image, dependency->system_handle)) return grown_out;
    }
  }
  return {};
}

}  // namespace

SymbolTarget search_scope(ImageState* root, const char* name, const char* version,
                          GlobalLookup globals, ImageState* after) {
  if (!root) return {};
  // As Bionic's dlsym_linear_lookup for RTLD_NEXT: the global pass starts past `after` in load
  // order, and the local group walk skips again up to and including `after`.
  bool skipping = after != nullptr;
  // A private dlsym also sees what this thread's collection is retiring, as Bionic's does while
  // dlclose runs the destructors: one may look itself up. A relocation never binds to it.
  const bool retiring = globals == GlobalLookup::Dynamic;
  auto local = [&](ImageState* candidate) -> SymbolTarget {
    if (skipping) {
      if (candidate == after) skipping = false;
      return {};
    }
    const auto* symbol = find_defined_symbol(candidate, name, version, true, retiring);
    return symbol ? SymbolTarget{candidate, symbol, nullptr, true} : SymbolTarget{};
  };
  const auto in_global_scope = [globals](const ImageState* candidate) {
    return globals == GlobalLookup::Relocation
               ? candidate->global_group
               : globals == GlobalLookup::Dynamic && (candidate->flags & RTLD_GLOBAL);
  };
  if (globals != GlobalLookup::None) {
    bool before = after != nullptr;
    for (auto* candidate = g_first_image; candidate; candidate = candidate->next) {
      if (before) {
        before = candidate != after;
        continue;
      }
      if (candidate->context == root->context && in_global_scope(candidate)) {
        const auto* symbol = find_defined_symbol(candidate, name, version, true, retiring);
        if (symbol) return {candidate, symbol, nullptr, true};
      }
    }
  }
  SymbolTarget grown_out;
  grown_out.failed = true;
  SymbolTarget target = walk_scope(root, grown_out, [&](const ScopeNode& node, bool* stop) {
    if (node.system) {
      if (skipping) return SymbolTarget{};
      // Bionic's dlsym can run a system IFUNC resolver, so the lookup itself counts.
      if (g_operation_error) g_operation_error->execution_started = 1;
      CallbackGuard callback_scope(root);
      void* address = nullptr;
      if (!host_dlsym(node.system, name, version, &address)) return SymbolTarget{};
      *stop = true;
      return SymbolTarget{nullptr, nullptr, address, true};
    }
    // The global pass searched only root's namespace; an image a link reached is searched here,
    // but its own dependencies are not, as in the system linker. Past `after`, a global image
    // before it in load order was not searched yet.
    const bool foreign = node.image->context != root->context;
    auto found =
        in_global_scope(node.image) && !foreign && !after ? SymbolTarget{} : local(node.image);
    *stop = found.found;
    return found;
  });
  return target.failed ? scope_failed(globals) : target;
}

bool walk_handle_scope(ImageState* root, bool (*visit)(ImageState*, void*), void* user) {
  if (!root) return true;
  SymbolTarget grown_out;
  grown_out.failed = true;
  return !walk_scope(root, grown_out, [&](const ScopeNode& node, bool* stop) {
            if (!node.system) *stop = !visit(node.image, user);
            return SymbolTarget{};
          }).failed;
}

SymbolTarget symbol_target(ImageState* image, uint32_t index, uint32_t relocation_type) {
  const auto* symbol = symbol_at(image, index);
  if (!symbol || !symbol_name_valid(image->symbols, *symbol)) return {};
  const char* name = image->symbols.strings + symbol->st_name;
  const char* version = symbol_version(image, index, symbol->st_shndx == SHN_UNDEF);
  const auto visibility = (symbol->st_other & 3);
  if (symbol->st_shndx != SHN_UNDEF &&
      (ELF64_ST_BIND(symbol->st_info) == STB_LOCAL || visibility != STV_DEFAULT || image->symbolic))
    return {image, symbol, nullptr, true};
  if (symbol->st_shndx == SHN_UNDEF) {
    // A trusted image binds to the system, but its TLS references resolve only to private
    // images, so the module ids it hands __tls_get_addr are the runtime's.
#if defined(YUKILINKER_RUNTIME)
    // An image of a managed namespace calls these through its own stubs, which name it.
    if (void* stub = caller_stub_for(image, name)) return {nullptr, nullptr, stub, true};
#endif
    if (!image->host || strcmp(name, "__tls_get_addr") == 0)
      if (void* replacement = module_api_symbol(name)) return {nullptr, nullptr, replacement, true};
  }
  auto target = search_scope(image->load_root ? image->load_root : image, name, version,
                             GlobalLookup::Relocation);
#if defined(YUKILINKER_RUNTIME)
  if (!target.found && !target.failed && symbol->st_shndx == SHN_UNDEF &&
      image->context->host_imports) {
    // The system's global group is never searched; only a host import of the namespace may
    // supply what the private scope and the DT_NEEDED edges did not.
    const HostImportResult imported = resolve_host_import(image, name, version, relocation_type);
    if (imported.found) {
      image->host_import_mask |= uint32_t{1} << imported.index;
      return {nullptr, nullptr, imported.address, true};
    }
    if (imported.failed) {
      // NO_MEMORY is recorded already.
      target.failed = true;
    } else if (imported.refused) {
      fail_refused(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, imported.reason,
                   imported.library, name, version);
      target.failed = true;
    } else if (ELF64_ST_BIND(symbol->st_info) != STB_WEAK) {
      host_log_error(
          "yukilinker: %s%s%s not found for %s; the system's global group is searched only for "
          "a namespace's host imports, and %s",
          name, version ? "@" : "", version ? version : "", image->origin,
          imported.listed ? "no import supplies it" : "no import lists it");
    }
  }
#else
  (void)relocation_type;
#endif
  return target;
}

bool retain_symbol_provider(ImageState* consumer, ImageState* provider) {
  if (!provider || consumer == provider) return true;
  for (auto* dep = consumer->dependencies; dep; dep = dep->next)
    if (dep->image == provider) return true;
  auto* dependency = metadata_object<Dependency>(&consumer->memory.metadata);
  if (!dependency) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_RELOCATIONS);
  dependency->image = provider;
  dependency->symbol_provider = true;
  Dependency** tail = &consumer->dependencies;
  while (*tail) tail = &(*tail)->next;
  *tail = dependency;
  return true;
}

// Takes the target the caller already looked up, so a relocation is resolved exactly once.
SymbolResolution resolve_symbol(ImageState* image, uint32_t index, const SymbolTarget& target) {
  if (target.found) {
    if (target.image) {
      // A private target always names its symbol (every lookup that finds an image does); one
      // without is refused rather than read.
      if (!target.symbol || ELF64_ST_TYPE(target.symbol->st_info) == STT_TLS ||
          !retain_symbol_provider(image, target.image))
        return {};
      return materialize_defined_symbol(target.image, *target.symbol);
    }
    return {reinterpret_cast<uintptr_t>(target.address), true};
  }
  // STN_UNDEF: the symbol value is 0 (gABI), as in the system linker.
  if (index == 0) return {0, true};
  const auto* symbol = symbol_at(image, index);
  if (symbol && ELF64_ST_BIND(symbol->st_info) == STB_WEAK) return {0, true};
  fail(YUKILINKER_C_SYMBOL_MISSING, YUKILINKER_C_RELOCATIONS,
       symbol ? string_at(image, symbol->st_name) : nullptr,
       symbol ? symbol_version(image, index, true) : nullptr);
  return {};
}

#if YUKILINKER_FULL
bool resolve_tls_reference(ImageState* image, uint32_t symbol_index, ElfW(Sxword) addend,
                           TlsReference* result) {
  *result = {};
  if (symbol_index == 0) {
    uintptr_t offset;
    if (!image->tls.active) return false;
    if (!add_signed_offset(0, addend, &offset) || offset >= image->tls.memory_bytes)
      return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS);
    result->module_id = image->tls.module_id;
    result->byte_offset = offset;
    result->generation = image->tls.generation;
    return true;
  }

  const ElfW(Sym)* symbol = symbol_at(image, symbol_index);
  if (symbol == nullptr) return false;
  auto target = symbol_target(image, symbol_index, kTlsReference);
  if (target.failed) return false;
  if (!target.found) {
    if (ELF64_ST_BIND(symbol->st_info) != STB_WEAK) return false;
    result->unresolved_weak = true;
    result->byte_offset = static_cast<uintptr_t>(addend);
    return true;
  }
  if (!target.image || !target.symbol || ELF64_ST_TYPE(target.symbol->st_info) != STT_TLS ||
      !target.image->tls.active)
    return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS,
                string_at(image, symbol->st_name));
  if (!retain_symbol_provider(image, target.image)) return false;
  image = target.image;
  symbol = target.symbol;
  uintptr_t offset;
  // Past the provider's TLS block: a toolchain error, not a missing capability.
  if (!add_signed_offset(symbol->st_value, addend, &offset) || offset >= image->tls.memory_bytes)
    return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS,
                string_at(image, symbol->st_name));
  result->module_id = image->tls.module_id;
  result->byte_offset = offset;
  result->generation = image->tls.generation;
  return true;
}
#endif  // #if YUKILINKER_FULL

// A relocated word may lie at any address in a writable segment: lld emits unaligned ones for
// packed structs, and the system linker stores them in place. Words go through memcpy. The bytes
// [offset, offset + width) must lie in one readable and writable PT_LOAD (elf::inspect keeps
// segments from sharing a page, so no two hold one byte). The range the previous call found is
// tried first, then the sorted ranges are searched: a few steps whatever order the targets come in
// and however many program headers the image has.
void* relocation_destination(ImageState* image, ElfW(Addr) offset,
                             size_t width = sizeof(ElfW(Addr))) {
  auto& set = image->relocations;
  const WritableRange* ranges = set.writable;
  const auto holds = [&](size_t i) {
    return offset >= ranges[i].begin &&
           elf::range(ranges[i].end - ranges[i].begin, offset - ranges[i].begin, width);
  };
  if (set.writable_hint >= set.writable_count || !holds(set.writable_hint)) {
    // The last range that begins at or below offset is the only one that can hold it.
    size_t low = 0;
    size_t high = set.writable_count;
    while (low < high) {
      const size_t middle = low + (high - low) / 2;
      if (ranges[middle].begin <= offset)
        low = middle + 1;
      else
        high = middle;
    }
    if (low == 0 || !holds(low - 1)) return nullptr;
    set.writable_hint = low - 1;
  }
  const uintptr_t bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  if (offset > UINTPTR_MAX - bias) return nullptr;
  auto* pointer = reinterpret_cast<uint8_t*>(bias + offset);
  return address_space_contains(image->memory, pointer, width) ? pointer : nullptr;
}

ElfW(Addr) load_word(const void* slot) {
  ElfW(Addr) value;
  memcpy(&value, slot, sizeof(value));
  return value;
}

void store_word(void* slot, uintptr_t value) {
  const auto word = static_cast<ElfW(Addr)>(value);
  memcpy(slot, &word, sizeof(word));
}

uint32_t relocation_type(const ElfRelocation& relocation) {
#if defined(__LP64__)
  uint32_t type = ELF64_R_TYPE(relocation.r_info);
  return type;
#else
  return ELF32_R_TYPE(relocation.r_info);
#endif  // #if defined(__LP64__)
}

uint32_t relocation_symbol(const ElfRelocation& relocation) {
#if defined(__LP64__)
  return ELF64_R_SYM(relocation.r_info);
#else
  return ELF32_R_SYM(relocation.r_info);
#endif  // #if defined(__LP64__)
}

ElfW(Sxword) relocation_addend(const ElfRelocation& relocation, ElfW(Addr) value) {
#if defined(__LP64__)
  (void)value;
  return relocation.r_addend;
#else
  // The word in place, as the system linker reads it: its bits, which apply_addend adds in the
  // address width. A TLS offset reads it as a signed value, but only within its block.
  (void)relocation;
  return static_cast<ElfW(Sword)>(value);
#endif  // #if defined(__LP64__)
}

// Collects the readable and writable segments that map bytes, sorted by address, for
// relocation_destination. An empty one maps nothing (elf::inspect skips it) and holds no target.
// False when the metadata for more than kInlineRanges cannot be had.
bool collect_writable_ranges(ImageState* image) {
  auto& set = image->relocations;
  size_t count = 0;
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const ElfW(Phdr) & header = image->memory.program_headers[i];
    if (header.p_type == PT_LOAD && (header.p_flags & (PF_R | PF_W)) == (PF_R | PF_W) &&
        header.p_memsz != 0)
      ++count;
  }
  WritableRange* ranges = set.inline_ranges;
  if (count > RelocationSet::kInlineRanges) {
    ranges = static_cast<WritableRange*>(metadata_allocate(
        count * sizeof(WritableRange), alignof(WritableRange), &image->memory.metadata));
    if (ranges == nullptr) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_RELOCATIONS);
  }
  size_t filled = 0;
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const ElfW(Phdr) & header = image->memory.program_headers[i];
    if (header.p_type != PT_LOAD || (header.p_flags & (PF_R | PF_W)) != (PF_R | PF_W) ||
        header.p_memsz == 0)
      continue;
    // elf::inspect checked that no segment's end overflows. Insertion keeps the order: there are a
    // handful of these.
    const WritableRange range{static_cast<uintptr_t>(header.p_vaddr),
                              static_cast<uintptr_t>(header.p_vaddr + header.p_memsz)};
    size_t at = filled++;
    for (; at != 0 && ranges[at - 1].begin > range.begin; --at) ranges[at] = ranges[at - 1];
    ranges[at] = range;
  }
  set.writable = ranges;
  set.writable_count = count;
  set.writable_hint = 0;
  return true;
}

// The end of the highest writable segment: every relocation target lies below it.
uintptr_t writable_end(const ImageState* image) {
  const auto& set = image->relocations;
  return set.writable_count != 0 ? set.writable[set.writable_count - 1].end : 0;
}

// Inlined: pass -1 calls it for every relocation, and each call it saves is one signed return.
[[gnu::always_inline]] inline bool record_relocation_target(ImageState* image, uintptr_t address,
                                                            size_t width, uint32_t type) {
  auto* targets = image->relocations.targets;
  if (g_relocation_pass != -1 || !targets) return true;
  switch (targets->mark(address, width)) {
    case RelocationTargets::Mark::kNew:
      return true;
    case RelocationTargets::Mark::kTooMany:
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, nullptr, nullptr, address,
                  type);
    case RelocationTargets::Mark::kNoMemory:
      return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_RELOCATIONS, nullptr, nullptr, address,
                  type);
    case RelocationTargets::Mark::kInvalid:
      break;
  }
  // Two relocations write one byte: the image is refused before either is applied.
  return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS, nullptr, nullptr, address, type);
}

// Relocation types whose value is the target symbol's address.
bool symbol_value_type(uint32_t type) {
#if defined(__aarch64__)
  return type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT;
#else
  return type == R_ARM_ABS32 || type == R_ARM_REL32 || type == R_ARM_GLOB_DAT ||
         type == R_ARM_JUMP_SLOT;
#endif
}

// Stores a resolved symbol address the way a symbol_value_type relocation asks for. The sum wraps
// in the address width, as the system linker's does.
bool write_symbol_value(uint32_t type, void* destination, uintptr_t address, ElfW(Sxword) addend) {
#if defined(__aarch64__)
  (void)type;
  store_word(destination, apply_addend(address, addend));
#else
  if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT) {
    store_word(destination, address);
    return true;
  }
  uintptr_t value = apply_addend(address, addend);
  if (type == R_ARM_REL32) value -= reinterpret_cast<uintptr_t>(destination);
  store_word(destination, value);
#endif
  return true;
}

bool defer_relocation(ImageState* image, const DeferredRelocation& entry) {
  // The edge keeps the provider alive until pass 2 runs its resolver.
  if (entry.provider && !retain_symbol_provider(image, entry.provider)) return false;
  auto& set = image->relocations;
  if (set.deferred_count == set.deferred_capacity) {
    const size_t capacity = set.deferred_capacity ? set.deferred_capacity * 2 : 16;
    if (capacity > size_t{16} * 1024 * 1024)
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS);
    auto* deferred = static_cast<DeferredRelocation*>(
        realloc(set.deferred, capacity * sizeof(DeferredRelocation)));
    if (!deferred) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_RELOCATIONS);
    set.deferred = deferred;
    set.deferred_capacity = capacity;
  }
  set.deferred[set.deferred_count++] = entry;
  return true;
}

// The consumer's edge pins the provider, and collection clears that edge before it frees one.
bool provider_retained(const ImageState* consumer, const ImageState* provider) {
  if (consumer == provider) return true;
  for (auto* dep = consumer->dependencies; dep; dep = dep->next)
    if (dep->image == provider) return true;
  return false;
}

// Pass 2 runs the resolvers pass 1 recorded, without looking any symbol up again: a lookup can
// drop the lock, and the scope it sees can change between passes.
bool replay_deferred_relocations(ImageState* image) {
  const auto& set = image->relocations;
  for (size_t i = 0; i < set.deferred_count; ++i) {
    const DeferredRelocation entry = set.deferred[i];
    void* destination = relocation_destination(image, entry.offset);
    if (destination == nullptr) return false;
    if (entry.provider == nullptr) {
      // The resolver's address wraps as the system linker's does; it must be the image's code.
      const uintptr_t resolver =
          apply_addend(reinterpret_cast<uintptr_t>(image->memory.bias), entry.addend);
      if (!executable_pointer(image, resolver)) return false;
      image->executed = true;
      if (g_operation_error) g_operation_error->execution_started = 1;
      store_word(destination, invoke_ifunc(image, resolver));
      continue;
    }
    if (!provider_retained(image, entry.provider)) return false;
    const SymbolResolution resolution = materialize_defined_symbol(entry.provider, *entry.symbol);
    if (!resolution.valid ||
        !write_symbol_value(entry.type, destination, resolution.address, entry.addend))
      return false;
  }
  return true;
}

void discard_deferred_relocations(ImageState* image) {
  auto& set = image->relocations;
  free(set.deferred);
  set.deferred = nullptr;
  set.deferred_count = set.deferred_capacity = 0;
}

// The symbol pass 1 last looked up for one image. Relocations against one symbol are usually
// adjacent, and the system linker keeps the same one-entry cache. It lives on relocate_image's
// stack, and a TLS lookup, the only other thing in pass 1 that can drop the lock, empties it.
struct LookupCache {
  uint32_t index = 0;  // 0 is empty: STN_UNDEF is never looked up.
  SymbolTarget target;
};

bool apply_relocation(ImageState* image, const ElfRelocation& relocation, LookupCache* cache) {
  uint32_t type = relocation_type(relocation);
  uint32_t symbol_index = relocation_symbol(relocation);
  if (type == 0) return true;
  size_t width = sizeof(ElfW(Addr));
#if defined(__aarch64__)
  // A TLS descriptor is two words; both are checked before either is recorded or written.
  if (type == R_AARCH64_TLSDESC) width *= 2;
  // AAELF64 leaves these two numbers to the platform. Old Android and current
  // NDK headers disagree; a strict PRIVATE context never guesses from the device API.
  if (image->context->strict_private && (type == 1028 || type == 1029))
    return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, "AArch64 TLS_IMPDEF", nullptr,
                relocation.r_offset, type);
#endif
  void* destination = relocation_destination(image, relocation.r_offset, width);
  if (destination == nullptr)
    return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS, nullptr, nullptr,
                relocation.r_offset, type);
  if (!record_relocation_target(image, relocation.r_offset, width, type)) return false;
  ElfW(Sxword) addend = relocation_addend(relocation, load_word(destination));
#if defined(__aarch64__)
  const bool indirect = type == R_AARCH64_IRELATIVE;
  const bool static_tls = type == R_AARCH64_TLS_TPREL64;
  const bool tls = type >= R_AARCH64_TLS_DTPREL64 && type <= R_AARCH64_TLSDESC;
  const bool supported = type == R_AARCH64_RELATIVE || indirect || tls || type == R_AARCH64_ABS64 ||
                         type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT;
#else
  const bool indirect = type == R_ARM_IRELATIVE;
  // The reduced loader has no TLS; it keeps refusing initial-exec TLS as an unknown type.
  const bool static_tls = YUKILINKER_FULL && type == R_ARM_TLS_TPOFF32;
  const bool tls = type == R_ARM_TLS_DTPMOD32 || type == R_ARM_TLS_DTPOFF32 || static_tls;
  // R_ARM_TLS_DESC is refused, as the system linker refuses it on ARM32.
  const bool supported = type == R_ARM_RELATIVE || indirect || tls || type == R_ARM_ABS32 ||
                         type == R_ARM_REL32 || type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT;
#endif
  if (!supported)
    return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, nullptr, nullptr,
                relocation.r_offset, type);
  if (g_relocation_pass == -1) {
    if (tls) image->relocations.tls = true;
    const auto* symbol = symbol_at(image, symbol_index);
    if (symbol_index && !symbol) return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS);
    // Initial-exec TLS has no static block here; only a weak symbol that stays undefined works.
    if (static_tls && (!symbol || ELF64_ST_BIND(symbol->st_info) != STB_WEAK))
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, nullptr, nullptr,
                  relocation.r_offset, type);
    return true;
  }
  // Pass 0 checks TLS references only; every other symbol is looked up once, in pass 1.
  if (g_relocation_pass == 0) {
#if YUKILINKER_FULL
    if (tls) {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, addend, &reference)) {
        name_relocation(relocation.r_offset, type);
        return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, nullptr, nullptr,
                    relocation.r_offset, type);
      }
      if (static_tls && !reference.unresolved_weak)
        return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, nullptr, nullptr,
                    relocation.r_offset, type);
    }
#else
    if (tls) return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS);
#endif
    return true;
  }
  if (tls) cache->index = 0;
  SymbolTarget target{};
  if (symbol_index && !tls) {
    // Nothing dropped the lock since the entry was filled. A private provider must still be
    // pinned by this image's edge, the check pass 2 makes.
    if (symbol_index == cache->index &&
        (!cache->target.image || provider_retained(image, cache->target.image))) {
      target = cache->target;
    } else {
      target = symbol_target(image, symbol_index, type);
      const auto* import = symbol_at(image, symbol_index);
      // A walk that failed has recorded NO_MEMORY, which this does not replace; a weak import
      // must not bind to 0 for it.
      if (!import || target.failed ||
          (!target.found && ELF64_ST_BIND(import->st_info) != STB_WEAK)) {
        name_relocation(relocation.r_offset, type);
        return fail(YUKILINKER_C_SYMBOL_MISSING, YUKILINKER_C_RELOCATIONS,
                    import ? string_at(image, import->st_name) : nullptr,
                    import ? symbol_version(image, symbol_index, true) : nullptr,
                    relocation.r_offset, type);
      }
      *cache = {symbol_index, target};
    }
  }
  // Pass 1 writes everything that runs no code and records the rest for pass 2.
  if (indirect)
    return defer_relocation(image, {relocation.r_offset, addend, nullptr, nullptr, type});
  if (symbol_value_type(type) && target.symbol &&
      ELF64_ST_TYPE(target.symbol->st_info) == STT_GNU_IFUNC)
    return defer_relocation(image,
                            {relocation.r_offset, addend, target.image, target.symbol, type});

#if defined(__aarch64__)
  switch (type) {
    case R_AARCH64_NONE:
      return true;
    case R_AARCH64_RELATIVE: {
      store_word(destination,
                 apply_addend(reinterpret_cast<uintptr_t>(image->memory.bias), addend));
      return true;
    }
#if YUKILINKER_FULL
    case R_AARCH64_TLSDESC: {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, addend, &reference)) return false;
      void* argument = static_cast<uint8_t*>(destination) + sizeof(uintptr_t);
      if (reference.unresolved_weak) {
        store_word(destination, reinterpret_cast<uintptr_t>(&yuki_tlsdesc_weak_entry));
        store_word(argument, reference.byte_offset);
        return true;
      }
      auto* cookie = metadata_object<TlsDescriptorIndex>(&image->memory.metadata);
      if (cookie == nullptr) return false;
      cookie->module = reference.module_id;
      cookie->offset = reference.byte_offset;
      cookie->generation = reference.generation;
      store_word(destination, reinterpret_cast<uintptr_t>(&yuki_tlsdesc_dynamic_entry));
      store_word(argument, reinterpret_cast<uintptr_t>(cookie));
      return true;
    }
    case R_AARCH64_TLS_DTPREL64: {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, addend, &reference)) return false;
      store_word(destination, reference.byte_offset);
      return true;
    }
    case R_AARCH64_TLS_DTPMOD64: {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, 0, &reference)) return false;
      store_word(destination, reference.unresolved_weak ? 0 : reference.module_id);
      return true;
    }
    case R_AARCH64_TLS_TPREL64: {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, addend, &reference)) return false;
      if (reference.unresolved_weak) {
        store_word(destination, reference.byte_offset);
        return true;
      }
      // Pass 0 saw it unresolved; the scope changed while a lookup dropped the lock.
      host_log_error("yukilinker: static TLS access model is unsupported");
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, nullptr, nullptr,
                  relocation.r_offset, type);
    }
#endif  // #if YUKILINKER_FULL
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT: {
      SymbolResolution symbol = resolve_symbol(image, symbol_index, target);
      return symbol.valid && write_symbol_value(type, destination, symbol.address, addend);
    }
    default:
      host_log_error("yukilinker: unsupported AArch64 relocation %u", type);
      return false;
  }
#else
  switch (type) {
    case R_ARM_NONE:
      return true;
    case R_ARM_RELATIVE: {
      store_word(destination,
                 apply_addend(reinterpret_cast<uintptr_t>(image->memory.bias), addend));
      return true;
    }
    case R_ARM_ABS32:
    case R_ARM_REL32:
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT: {
      SymbolResolution symbol = resolve_symbol(image, symbol_index, target);
      return symbol.valid && write_symbol_value(type, destination, symbol.address, addend);
    }
#if YUKILINKER_FULL
    case R_ARM_TLS_DTPMOD32: {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, 0, &reference)) return false;
      store_word(destination, reference.unresolved_weak ? 0 : reference.module_id);
      return true;
    }
    case R_ARM_TLS_DTPOFF32: {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, addend, &reference)) return false;
      store_word(destination, reference.byte_offset);
      return true;
    }
    case R_ARM_TLS_TPOFF32: {
      TlsReference reference;
      if (!resolve_tls_reference(image, symbol_index, addend, &reference)) return false;
      if (reference.unresolved_weak) {
        // The in-place addend, as on AArch64.
        store_word(destination, reference.byte_offset);
        return true;
      }
      // Pass 0 saw it unresolved; the scope changed while a lookup dropped the lock.
      host_log_error("yukilinker: ARM static TLS access model is unsupported");
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS, nullptr, nullptr,
                  relocation.r_offset, type);
    }
#endif  // #if YUKILINKER_FULL
    default:
      host_log_error("yukilinker: unsupported ARM relocation %u", type);
      return false;
  }
#endif  // #if defined(__aarch64__)
}

bool apply_relocation_span(ImageState* image, const ElfRelocation* entries, size_t bytes,
                           LookupCache* cache) {
  if (bytes == 0) return true;
  if (entries == nullptr || bytes % sizeof(ElfRelocation) != 0) return false;
  size_t count = bytes / sizeof(ElfRelocation);
  for (size_t i = 0; i < count; ++i)
    if (!apply_relocation(image, entries[i], cache)) return false;
  return true;
}

bool patch_relative_word(ImageState* image, ElfW(Addr) offset) {
#if defined(__aarch64__)
  constexpr uint32_t kRelative = R_AARCH64_RELATIVE;
#else
  constexpr uint32_t kRelative = R_ARM_RELATIVE;
#endif
  void* word = relocation_destination(image, offset);
  if (word == nullptr)
    return fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS, nullptr, nullptr, offset,
                kRelative);
  if (!record_relocation_target(image, offset, sizeof(ElfW(Addr)), kRelative)) return false;
  if (g_relocation_pass == 1)
    store_word(word, load_word(word) + reinterpret_cast<uintptr_t>(image->memory.bias));
  return true;
}

bool apply_relr_span(ImageState* image) {
  const RelocationSet& relocations = image->relocations;
  if (relocations.relr_bytes == 0) return true;
  if (relocations.relr == nullptr || relocations.relr_bytes % sizeof(ElfW(Addr)) != 0) return false;

  size_t count = relocations.relr_bytes / sizeof(ElfW(Addr));
  ElfW(Addr) next_offset = 0;
  bool have_base = false;
  constexpr size_t kBitmapBits = (sizeof(ElfW(Addr)) * 8) - 1;
  for (size_t i = 0; i < count; ++i) {
    ElfW(Addr) encoded = relocations.relr[i];
    if ((encoded & 1U) == 0) {
      if (!patch_relative_word(image, encoded)) return false;
      if (encoded > UINTPTR_MAX - sizeof(ElfW(Addr))) return false;
      next_offset = encoded + sizeof(ElfW(Addr));
      have_base = true;
      continue;
    }
    if (!have_base) return false;
    ElfW(Addr) bitmap = encoded >> 1;
    for (size_t bit = 0; bit < kBitmapBits; ++bit) {
      size_t displacement = bit * sizeof(ElfW(Addr));
      if ((bitmap & (static_cast<ElfW(Addr)>(1) << bit)) != 0 &&
          (next_offset > UINTPTR_MAX - displacement ||
           !patch_relative_word(image, next_offset + displacement)))
        return false;
    }
    constexpr size_t kBitmapSpan = kBitmapBits * sizeof(ElfW(Addr));
    if (next_offset > UINTPTR_MAX - kBitmapSpan) return false;
    next_offset += kBitmapSpan;
  }
  return true;
}

bool apply_packed(ImageState* image, LookupCache* cache) {
  // A present table is decoded whatever its size: one too small for "APS2" fails.
  if (image->relocations.packed == nullptr) return true;
  return elf::packed<sizeof(void*) == 8>(
      image->relocations.packed, image->relocations.packed_bytes, sizeof(void*) * 8,
      [image, cache](uint64_t offset, uint64_t info, int64_t addend) {
        ElfRelocation relocation{};
        relocation.r_offset = static_cast<ElfW(Addr)>(offset);
        relocation.r_info = static_cast<decltype(relocation.r_info)>(info);
#if defined(__LP64__)
        relocation.r_addend = addend;
#else
        (void)addend;
#endif
        return apply_relocation(image, relocation, cache);
      });
}

bool relocate_image(ImageState* image) {
  if (g_relocation_pass == 2) {
    const bool replayed = replay_deferred_relocations(image);
    discard_deferred_relocations(image);
    return replayed || fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS);
  }
  auto& set = image->relocations;
  // Pass 0 checks TLS relocations only: nothing to do in tables pass -1 found without one.
  if (g_relocation_pass == 0 && !set.tls) return true;
  // One entry serves every table of the image in this pass, as in the system linker.
  LookupCache cache;
  // Pass -1 records every byte a relocation writes and refuses the first one written twice. It
  // runs first for every image and finds the writable ranges every later pass uses.
  RelocationTargets targets;
  if (g_relocation_pass == -1) {
    if (!collect_writable_ranges(image)) return false;
    targets.begin(writable_end(image));
    set.targets = &targets;
    set.tls = false;
  }
  const bool valid =
      apply_relr_span(image) && apply_packed(image, &cache) &&
      apply_relocation_span(image, image->relocations.rel, image->relocations.rel_bytes, &cache) &&
      apply_relocation_span(image, image->relocations.plt, image->relocations.plt_bytes, &cache);
  set.targets = nullptr;
  // A table pass -1 refused may still hold a TLS relocation past the one it stopped at.
  if (!valid) set.tls = true;
  return valid || fail(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_RELOCATIONS);
}

}  // namespace yukilinker::internal
