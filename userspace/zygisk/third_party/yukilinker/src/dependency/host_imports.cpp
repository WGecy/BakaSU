// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/host_imports.hpp"

#include "../internal/registry.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

// One namespace_host_import: a system library, the handle that keeps it loaded until the
// namespace is destroyed, and the ':'-separated symbol names its images may bind to there.
struct HostImport {
  HostImport* next = nullptr;
  void* handle = nullptr;
  uint32_t flags = 0;
  char library[256]{};
  char symbols[4096]{};
};

namespace {

constexpr size_t kMaxHostImports = 16;
// ImageState::host_import_mask has a bit per import.
static_assert(kMaxHostImports <= 32);

uint32_t missing_namespace(const void* opaque) {
  const auto value = reinterpret_cast<uintptr_t>(opaque);
  return (value & 3) == 3 && value / 4 != 0 ? YUKILINKER_C_STALE_TOKEN
                                            : YUKILINKER_C_INVALID_HANDLE;
}

// Every entry 1-255 bytes without ':' or '@', none empty, 4095 bytes in all.
bool valid_symbol_list(const char* names) {
  if (names == nullptr || names[0] == '\0') return false;
  const size_t total = strnlen(names, sizeof(HostImport::symbols));
  if (total == sizeof(HostImport::symbols)) return false;
  size_t entry = 0;
  for (size_t i = 0; i <= total; ++i) {
    if (i == total || names[i] == ':') {
      if (entry == 0 || entry > 255) return false;
      entry = 0;
    } else if (names[i] == '@') {
      return false;
    } else {
      ++entry;
    }
  }
  return true;
}

bool lists(const HostImport& import, const char* name) {
  const size_t length = strlen(name);
  for (const char* cursor = import.symbols; *cursor != '\0';) {
    const char* end = strchr(cursor, ':');
    const size_t entry = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
    if (entry == length && memcmp(cursor, name, length) == 0) return true;
    if (!end) break;
    cursor = end + 1;
  }
  return false;
}

// The provider's dynamic symbol tables, read from its mapped image. Only valid inside the
// dl_iterate_phdr callback that read them: once it returns, the system linker may unload the
// object, so nothing read through them outlives that call. Every read goes through readable()
// first: the system linker checked the object when it loaded it, but a table that points past its
// own segments would still fault here, inside its lock.
struct HostTables {
  const struct dl_phdr_info* info = nullptr;
  uintptr_t bias = 0;
  const ElfW(Sym) * symbols = nullptr;
  const char* strings = nullptr;
  size_t string_bytes = 0;
  const uint32_t* gnu_hash = nullptr;
  const uint32_t* sysv_hash = nullptr;
  const ElfW(Versym) * versions = nullptr;
  const ElfW(Verdef) * definitions = nullptr;
  size_t definition_count = 0;
  size_t soname_offset = SIZE_MAX;
  // The table's last NUL plus one: a string at an offset below it ends inside the table.
  size_t string_terminated = 0;
};

// Whether bytes [address, address + size) lie inside one readable PT_LOAD of the object.
bool readable(const HostTables& tables, const void* address, size_t size) {
  const auto start = reinterpret_cast<uintptr_t>(address);
  if (start < tables.bias) return false;
  const uint64_t offset = start - tables.bias;
  for (size_t i = 0; i < tables.info->dlpi_phnum; ++i) {
    const ElfW(Phdr) & header = tables.info->dlpi_phdr[i];
    if (header.p_type == PT_LOAD && (header.p_flags & PF_R) && offset >= header.p_vaddr &&
        elf::range(header.p_memsz, offset - header.p_vaddr, size))
      return true;
  }
  return false;
}

// count elements of T at address, aligned for T, overflow-checked.
template <typename T>
bool readable_array(const HostTables& tables, const T* address, size_t count) {
  size_t bytes = 0;
  return reinterpret_cast<uintptr_t>(address) % alignof(T) == 0 &&
         !multiply_overflow(count, sizeof(T), &bytes) && readable(tables, address, bytes);
}

const char* base_name(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// A pointer the dynamic section gives as an address. Bionic's linker never relocates d_ptr in place
// (it adds the load bias as it reads), so the bias is added here too.
template <typename T>
const T* dynamic_pointer(uintptr_t bias, ElfW(Addr) value) {
  uintptr_t address = 0;
  return __builtin_add_overflow(bias, value, &address) ? nullptr
                                                       : reinterpret_cast<const T*>(address);
}

// Reads the dynamic array, at most count entries of it (PT_DYNAMIC's p_memsz), and checks that the
// string table it names is readable and NUL-terminated. A table that is not leaves strings null.
void read_tables(const struct dl_phdr_info* info, const ElfW(Dyn) * entry, size_t count,
                 HostTables* tables) {
  *tables = {};
  tables->info = info;
  tables->bias = info->dlpi_addr;
  const uintptr_t bias = tables->bias;
  for (size_t i = 0; i < count && entry->d_tag != DT_NULL; ++i, ++entry) {
    const ElfW(Addr) value = entry->d_un.d_ptr;
    switch (entry->d_tag) {
      case DT_SYMTAB:
        tables->symbols = dynamic_pointer<ElfW(Sym)>(bias, value);
        break;
      case DT_STRTAB:
        tables->strings = dynamic_pointer<char>(bias, value);
        break;
      case DT_STRSZ:
        tables->string_bytes = entry->d_un.d_val;
        break;
      case DT_GNU_HASH:
        tables->gnu_hash = dynamic_pointer<uint32_t>(bias, value);
        break;
      case DT_HASH:
        tables->sysv_hash = dynamic_pointer<uint32_t>(bias, value);
        break;
      case DT_VERSYM:
        tables->versions = dynamic_pointer<ElfW(Versym)>(bias, value);
        break;
      case DT_VERDEF:
        tables->definitions = dynamic_pointer<ElfW(Verdef)>(bias, value);
        break;
      case DT_VERDEFNUM:
        tables->definition_count = entry->d_un.d_val;
        break;
      case DT_SONAME:
        tables->soname_offset = entry->d_un.d_val;
        break;
      default:
        break;
    }
  }
  if (tables->strings && tables->string_bytes != 0 &&
      readable(*tables, tables->strings, tables->string_bytes)) {
    if (const void* last = memrchr(tables->strings, 0, tables->string_bytes))
      tables->string_terminated =
          static_cast<size_t>(static_cast<const char*>(last) - tables->strings) + 1;
  }
  if (tables->string_terminated == 0) tables->strings = nullptr;
}

const char* string_of(const HostTables& tables, size_t offset) {
  return offset < tables.string_terminated ? tables.strings + offset : nullptr;
}

// Reads the tables of a system object named as library: by the same path or file name, or a soname
// that is that file name. Two files may share either, so every such object is a candidate. One
// without symbol or hash tables cannot define a symbol.
bool named_tables(const struct dl_phdr_info* info, const char* library, HostTables* tables) {
  // The first PT_DYNAMIC, the one Bionic's phdr_table_get_dynamic_section takes.
  const ElfW(Phdr)* dynamic = nullptr;
  for (size_t i = 0; i < info->dlpi_phnum && !dynamic; ++i)
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dynamic = &info->dlpi_phdr[i];
  if (!dynamic) return false;
  *tables = {};
  tables->info = info;
  tables->bias = info->dlpi_addr;
  const auto* entries = dynamic_pointer<ElfW(Dyn)>(info->dlpi_addr, dynamic->p_vaddr);
  const size_t count = dynamic->p_memsz / sizeof(ElfW(Dyn));
  if (!entries || count == 0 || !readable_array(*tables, entries, count)) return false;
  read_tables(info, entries, count, tables);
  const char* wanted = base_name(library);
  const char* soname = tables->strings ? string_of(*tables, tables->soname_offset) : nullptr;
  const bool named = (info->dlpi_name && (strcmp(info->dlpi_name, library) == 0 ||
                                          strcmp(base_name(info->dlpi_name), wanted) == 0)) ||
                     (soname && strcmp(soname, wanted) == 0);
  return named && tables->symbols && tables->strings && (tables->gnu_hash || tables->sysv_hash);
}

uint32_t host_gnu_hash(const char* name) {
  uint32_t hash = 5381;
  for (const auto* cursor = reinterpret_cast<const uint8_t*>(name); *cursor != 0; ++cursor)
    hash = hash * 33 + *cursor;
  return hash;
}

uint32_t host_sysv_hash(const char* name) {
  uint32_t hash = 0;
  for (const auto* cursor = reinterpret_cast<const uint8_t*>(name); *cursor != 0; ++cursor) {
    hash = (hash << 4) + *cursor;
    const uint32_t high = hash & 0xf0000000U;
    hash ^= high;
    hash ^= high >> 24;
  }
  return hash;
}

// A version record at offset from record, if all of it is readable.
template <typename T>
const T* host_record(const HostTables& tables, const void* record, size_t offset) {
  uintptr_t address = 0;
  if (__builtin_add_overflow(reinterpret_cast<uintptr_t>(record), offset, &address) ||
      address % alignof(T) != 0 ||
      !readable(tables, reinterpret_cast<const void*>(address), sizeof(T)))
    return nullptr;
  return reinterpret_cast<const T*>(address);
}

// Bionic's find_verdef_version_index over the provider's Verdef chain: 1 (global) if none, also
// when the chain leaves the object's readable bytes.
unsigned host_version_index(const HostTables& tables, const char* version) {
  const ElfW(Verdef)* definition = host_record<ElfW(Verdef)>(tables, tables.definitions, 0);
  for (size_t i = 0; definition && i < tables.definition_count; ++i) {
    if (definition->vd_cnt != 0) {
      const auto* aux = host_record<ElfW(Verdaux)>(tables, definition, definition->vd_aux);
      const char* name = aux ? string_of(tables, aux->vda_name) : nullptr;
      if (name && strcmp(name, version) == 0) return definition->vd_ndx;
    }
    if (definition->vd_next == 0) break;
    definition = host_record<ElfW(Verdef)>(tables, definition, definition->vd_next);
  }
  return 1;
}

// The provider's own entry for name, by its hash table: the version rule of eligible_symbol, but
// without the visibility filter, so that a hidden or local match can be reported as refused.
// Walks of a hash chain stop after this many entries: a chain the system linker accepted is never
// this long, and a cycle in one ends here instead of holding the system linker's lock forever.
constexpr size_t kMaxHostChain = size_t{1} << 20;

const ElfW(Sym) * host_entry(const HostTables& tables, const char* name, const char* version) {
  const auto matches = [&](size_t index) {
    const ElfW(Sym)* symbol = tables.symbols + index;
    if (!readable(tables, symbol, sizeof(*symbol))) return false;
    const char* entry_name = string_of(tables, symbol->st_name);
    if (symbol->st_shndx == SHN_UNDEF || !entry_name || strcmp(entry_name, name) != 0) return false;
    if (!tables.versions) return true;
    if (!readable(tables, tables.versions + index, sizeof(ElfW(Versym)))) return false;
    if (!version) return (tables.versions[index] & 0x8000) == 0;
    return (tables.versions[index] & 0x7fff) == host_version_index(tables, version);
  };
  if (const uint32_t* table = tables.gnu_hash) {
    constexpr uint32_t kBloomBits = sizeof(ElfW(Addr)) * 8;
    if (!readable_array(tables, table, 4)) return nullptr;
    const uint32_t bucket_count = table[0];
    const uint32_t first_hashed = table[1];
    const uint32_t bloom_words = table[2];
    const uint32_t shift = table[3];
    // As Bionic requires: a power-of-two bloom filter, and a shift within a word.
    if (bucket_count == 0 || bloom_words == 0 || (bloom_words & (bloom_words - 1)) != 0 ||
        shift >= kBloomBits)
      return nullptr;
    const auto* bloom = reinterpret_cast<const ElfW(Addr)*>(table + 4);
    if (!readable_array(tables, bloom, bloom_words)) return nullptr;
    const auto* buckets = reinterpret_cast<const uint32_t*>(bloom + bloom_words);
    if (!readable_array(tables, buckets, bucket_count)) return nullptr;
    const uint32_t* chains = buckets + bucket_count;
    const uint32_t hash = host_gnu_hash(name);
    const ElfW(Addr) word = bloom[(hash / kBloomBits) & (bloom_words - 1)];
    if ((1 & (word >> (hash % kBloomBits)) & (word >> ((hash >> shift) % kBloomBits))) == 0)
      return nullptr;
    uint32_t index = buckets[hash % bucket_count];
    if (index == 0 || index < first_hashed) return nullptr;
    // A chain ends at its odd entry.
    for (size_t steps = 0; steps < kMaxHostChain; ++steps, ++index) {
      const uint32_t* chain = chains + (index - first_hashed);
      if (!readable(tables, chain, sizeof(*chain))) return nullptr;
      if (((*chain ^ hash) >> 1) == 0 && matches(index)) return &tables.symbols[index];
      if ((*chain & 1) != 0) return nullptr;
    }
    return nullptr;
  }
  const uint32_t* table = tables.sysv_hash;
  if (!readable_array(tables, table, 2) || table[0] == 0) return nullptr;
  const uint32_t bucket_count = table[0];
  const uint32_t chain_count = table[1];
  const uint32_t* buckets = table + 2;
  if (!readable_array(tables, buckets, bucket_count)) return nullptr;
  const uint32_t* chains = buckets + bucket_count;
  if (!readable_array(tables, chains, chain_count)) return nullptr;
  uint32_t index = buckets[host_sysv_hash(name) % bucket_count];
  for (size_t steps = 0; index != 0 && index < chain_count && steps < kMaxHostChain;
       ++steps, index = chains[index])
    if (matches(index)) return &tables.symbols[index];
  return nullptr;
}

// Whether a relocation of this type may take a host symbol of this type.
// The relocation type a private dlsym asks with: it takes any of FUNC, OBJECT and NOTYPE.
constexpr uint32_t kLookupReference = UINT32_MAX - 1;

bool type_fits(uint32_t relocation_type, unsigned symbol_type) {
  if (relocation_type == kTlsReference ||
      (symbol_type != STT_FUNC && symbol_type != STT_OBJECT && symbol_type != STT_NOTYPE))
    return false;
#if defined(__aarch64__)
  if (relocation_type == R_AARCH64_JUMP_SLOT) return symbol_type != STT_OBJECT;
#else
  if (relocation_type == R_ARM_JUMP_SLOT) return symbol_type != STT_OBJECT;
#endif
  return true;
}

// Why a candidate's own entry cannot be bound, or null when it can. Read from the symbol table
// alone, before any host call, so that no resolver runs for a refused symbol.
// A refusal: the reason logged, and the stable yukilinker_c_host_import_reason a host reads.
struct Refusal {
  const char* text = nullptr;
  uint32_t reason = 0;
  explicit operator bool() const { return text != nullptr; }
};

Refusal refusal(const ElfW(Sym) & symbol, uint32_t relocation_type) {
  const unsigned binding = ELF64_ST_BIND(symbol.st_info);
  const unsigned type = ELF64_ST_TYPE(symbol.st_info);
  const unsigned visibility = symbol.st_other & 3;
  if (binding != STB_GLOBAL && binding != STB_WEAK)
    return {"not global or weak", YUKILINKER_C_HOST_IMPORT_NOT_GLOBAL_OR_WEAK};
  if (visibility == STV_HIDDEN || visibility == STV_INTERNAL)
    return {"hidden", YUKILINKER_C_HOST_IMPORT_HIDDEN};
  if (type == STT_TLS) return {"a TLS symbol", YUKILINKER_C_HOST_IMPORT_TLS};
  if (type == STT_GNU_IFUNC) return {"an IFUNC", YUKILINKER_C_HOST_IMPORT_IFUNC};
  // An absolute value names no byte of the library, which the binding is checked against; a
  // common or extended index names no section of it either.
  if (symbol.st_shndx == SHN_ABS) return {"an absolute symbol", YUKILINKER_C_HOST_IMPORT_ABSOLUTE};
  if (symbol.st_shndx >= SHN_LORESERVE)
    return {"of a reserved section index", YUKILINKER_C_HOST_IMPORT_RESERVED_SECTION};
  if (!type_fits(relocation_type, type))
    return {"of a type the relocation cannot take", YUKILINKER_C_HOST_IMPORT_TYPE_MISMATCH};
  return {};
}

// Whether the system's dlsym could hand the symbol out, and run code or allocate TLS for it doing
// so: checked over every candidate before that call, as which one the handle names is unknown.
Refusal lookup_refusal(const ElfW(Sym) & symbol, uint32_t relocation_type) {
  const unsigned binding = ELF64_ST_BIND(symbol.st_info);
  const unsigned type = ELF64_ST_TYPE(symbol.st_info);
  const unsigned visibility = symbol.st_other & 3;
  if (relocation_type == kTlsReference)
    return {"of a type the relocation cannot take", YUKILINKER_C_HOST_IMPORT_TYPE_MISMATCH};
  // The system's dlsym hands out any defined GLOBAL or WEAK symbol, whatever its visibility
  // (Bionic's is_symbol_global_and_defined), so a hidden one counts here too.
  (void)visibility;
  if (binding != STB_GLOBAL && binding != STB_WEAK) return {};
  if (type == STT_TLS) return {"a TLS symbol", YUKILINKER_C_HOST_IMPORT_TLS};
  if (type == STT_GNU_IFUNC) return {"an IFUNC", YUKILINKER_C_HOST_IMPORT_IFUNC};
  return {};
}

// An address as the system's dlsym may hand it out, without what the CPU ignores: on AArch64 the
// top byte (TBI), where a memory tag lives.
uintptr_t untagged(const void* address) {
  const auto value = reinterpret_cast<uintptr_t>(address);
#if defined(__aarch64__)
  return value & ((uintptr_t{1} << 56) - 1);
#else
  return value;
#endif
}

// One PT_LOAD of a candidate, copied while the system linker held it.
struct HostSegment {
  uint64_t vaddr;
  uint64_t memory_bytes;
  uint64_t file_bytes;
  uint32_t flags;
};

// A system object the import's library name could mean, by value: its bias, its PT_LOADs and its
// own entry for the symbol. No pointer into it is kept.
struct HostCandidate {
  uintptr_t bias;
  bool defined;
  ElfW(Sym) symbol;
  size_t first_segment;
  size_t segment_count;
};

template <typename T>
bool reserve(T** items, size_t* capacity, size_t needed) {
  if (needed <= *capacity) return true;
  size_t next = *capacity != 0 ? *capacity : 4;
  while (next < needed) {
    if (next > SIZE_MAX / 2 / sizeof(T)) return false;
    next *= 2;
  }
  void* grown = realloc(static_cast<void*>(*items), next * sizeof(T));
  if (!grown) return false;
  *items = static_cast<T*>(grown);
  *capacity = next;
  return true;
}

// Every candidate, however many, in dl_iterate_phdr's order.
struct CandidateSearch {
  const char* library;
  const char* name;
  const char* version;
  HostCandidate* candidates = nullptr;
  size_t count = 0;
  size_t capacity = 0;
  HostSegment* segments = nullptr;
  size_t segment_count = 0;
  size_t segment_capacity = 0;
  bool no_memory = false;

  CandidateSearch(const char* library_name, const char* symbol, const char* symbol_version)
      : library(library_name), name(symbol), version(symbol_version) {}
  ~CandidateSearch() {
    free(static_cast<void*>(candidates));
    free(static_cast<void*>(segments));
  }
  CandidateSearch(const CandidateSearch&) = delete;
  CandidateSearch& operator=(const CandidateSearch&) = delete;
};

// Reads everything a binding is checked against while the system linker holds the object.
int find_candidates(struct dl_phdr_info* info, size_t, void* opaque) {
  auto* search = static_cast<CandidateSearch*>(opaque);
  HostTables tables;
  if (!named_tables(info, search->library, &tables)) return 0;
  size_t loads = 0;
  for (size_t i = 0; i < info->dlpi_phnum; ++i)
    if (info->dlpi_phdr[i].p_type == PT_LOAD) ++loads;
  if (!reserve(&search->candidates, &search->capacity, search->count + 1) ||
      loads > SIZE_MAX - search->segment_count ||
      !reserve(&search->segments, &search->segment_capacity, search->segment_count + loads)) {
    search->no_memory = true;
    return 1;
  }
  HostCandidate& candidate = search->candidates[search->count++];
  candidate = {};
  candidate.bias = tables.bias;
  candidate.first_segment = search->segment_count;
  candidate.segment_count = loads;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) & header = info->dlpi_phdr[i];
    if (header.p_type == PT_LOAD)
      search->segments[search->segment_count++] = {header.p_vaddr, header.p_memsz, header.p_filesz,
                                                   header.p_flags};
  }
  if (const ElfW(Sym)* entry = host_entry(tables, search->name, search->version)) {
    candidate.defined = true;
    candidate.symbol = *entry;
  }
  return 0;
}

// Whether bytes at address lie in one of the candidate's PT_LOAD segments with flags, within its
// file-backed part when file_backed.
bool maps(const CandidateSearch& search, const HostCandidate& candidate, uintptr_t address,
          uint32_t flags, bool file_backed) {
  if (address < candidate.bias) return false;
  const uint64_t offset = address - candidate.bias;
  for (size_t i = 0; i < candidate.segment_count; ++i) {
    const HostSegment& segment = search.segments[candidate.first_segment + i];
    const uint64_t bytes = file_backed ? segment.file_bytes : segment.memory_bytes;
    if ((segment.flags & flags) == flags && offset >= segment.vaddr &&
        elf::range(bytes, offset - segment.vaddr, 1))
      return true;
  }
  return false;
}

// Why the owner's symbol at address is not what its type says: code must lie in an executable,
// file-backed segment, data in a readable one.
Refusal placement_refusal(const CandidateSearch& search, const HostCandidate& owner,
                          uintptr_t address, uint32_t relocation_type) {
  const unsigned type = ELF64_ST_TYPE(owner.symbol.st_info);
#if defined(__aarch64__)
  const bool call = relocation_type == R_AARCH64_JUMP_SLOT;
#else
  const bool call = relocation_type == R_ARM_JUMP_SLOT;
#endif
  if (type == STT_FUNC || (type == STT_NOTYPE && call)) {
#if defined(__arm__)
    // A Thumb function's value has bit 0 set.
    address &= ~uintptr_t{1};
#endif
    return maps(search, owner, address, PF_X, true)
               ? Refusal{}
               : Refusal{"not in the library's code", YUKILINKER_C_HOST_IMPORT_NOT_CODE};
  }
  return maps(search, owner, address, PF_R, false)
             ? Refusal{}
             : Refusal{"not in the library's data", YUKILINKER_C_HOST_IMPORT_NOT_DATA};
}

// The context a token names: null is the default context, as for namespace_create and
// LoadOptions.context. A context pointer is no token.
Context* import_context(void* opaque) {
  if (!opaque) return &g_default_context;
  NamespaceToken* token = find_namespace(opaque);
  return token ? token->context : nullptr;
}

}  // namespace

uint32_t api_namespace_host_import(void* opaque, const yukilinker_c_host_import* import) {
  OperationGuard operation;
  Context* context = import_context(opaque);
  if (!context) return missing_namespace(opaque);
  if (!import || import->size != sizeof(*import) || import->flags != 0 || !import->library ||
      import->library[0] == '\0' ||
      strnlen(import->library, sizeof(HostImport::library)) == sizeof(HostImport::library) ||
      !valid_symbol_list(import->symbols))
    return YUKILINKER_C_UNSUPPORTED;
  if (context->links_frozen || context_sealed(context)) return YUKILINKER_C_BUSY;
  size_t count = 0;
  HostImport** tail = &context->host_imports;
  for (; *tail; tail = &(*tail)->next, ++count)
    if (strcmp((*tail)->library, import->library) == 0) return YUKILINKER_C_UNSUPPORTED;
  if (count == kMaxHostImports) return YUKILINKER_C_UNSUPPORTED;
  auto* entry = allocate_object<HostImport>();
  if (!entry) return YUKILINKER_C_NO_MEMORY;
  (void)snprintf(entry->library, sizeof(entry->library), "%s", import->library);
  (void)snprintf(entry->symbols, sizeof(entry->symbols), "%s", import->symbols);
  // RTLD_NOLOAD: an import names a library the host already loaded, so registering runs no code.
  entry->handle =
      host_dlopen_reported(entry->library, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD, nullptr);
  if (!entry->handle) {
    free_object(entry);
    return YUKILINKER_C_DEPENDENCY_MISSING;
  }
  // host_dlopen dropped the lock: the namespace may be gone, frozen or holding this library now.
  uint32_t result = YUKILINKER_C_OK;
  if (import_context(opaque) != context) {
    result = missing_namespace(opaque);
  } else if (context->links_frozen || context_sealed(context)) {
    result = YUKILINKER_C_BUSY;
  } else {
    count = 0;
    tail = &context->host_imports;
    for (; *tail; tail = &(*tail)->next, ++count)
      if (strcmp((*tail)->library, entry->library) == 0) result = YUKILINKER_C_UNSUPPORTED;
    if (count == kMaxHostImports) result = YUKILINKER_C_UNSUPPORTED;
  }
  if (result != YUKILINKER_C_OK) {
    // The reference RTLD_NOLOAD took is given back; a failure leaves it with the system linker,
    // and the shutdown reports it.
    ++g_host_closes_in_flight;
    const int closed = host_dlclose(entry->handle);
    --g_host_closes_in_flight;
    if (closed != 0) {
      g_host_import_leaked = true;
      record_event(nullptr, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR,
                   entry->library);
      host_log_error("yukilinker: host import %s could not be closed", entry->library);
    }
    free_object(entry);
    return result;
  }
  *tail = entry;
  return YUKILINKER_C_OK;
}

uint32_t api_namespace_host_imports(void* opaque, yukilinker_c_host_import_record* records,
                                    size_t capacity, size_t* count) {
  OperationGuard operation;
  if (!count) return YUKILINKER_C_INVALID_HANDLE;
  Context* context = import_context(opaque);
  if (!context) return missing_namespace(opaque);
  size_t imports = 0;
  for (auto* entry = context->host_imports; entry; entry = entry->next) ++imports;
  *count = imports;
  if (capacity < imports || (imports && !records)) return YUKILINKER_C_BUFFER_TOO_SMALL;
  size_t index = 0;
  for (auto* entry = context->host_imports; entry; entry = entry->next) {
    yukilinker_c_host_import_record& record = records[index++];
    record = {};
    record.flags = entry->flags;
    memcpy(record.library, entry->library, sizeof(record.library));
    memcpy(record.symbols, entry->symbols, sizeof(record.symbols));
  }
  return YUKILINKER_C_OK;
}

namespace {

// The imports of every context that went (a destroyed namespace, the default context's shutdown)
// and are not closed yet. A context's list moves here before any of it is closed, so what a release
// that stopped short left (its thread ended in a host call, or a fork child has no such thread) is
// still found: the next release, and at the latest the shutdown's, closes it.
HostImport* g_retiring_host_imports = nullptr;

}  // namespace

bool release_host_imports(Context* context) {
  HostImport** tail = &g_retiring_host_imports;
  while (*tail) tail = &(*tail)->next;
  *tail = context->host_imports;
  context->host_imports = nullptr;
  // One entry at a time, each unlinked before its dlclose drops the lock: no other thread finds a
  // freed entry, and no entry is closed twice. The one under way when a thread ends is left to the
  // system linker, whose dlclose it was. The caller has sealed or unlinked context, so no
  // registration adds to its list meanwhile.
  bool closed = true;
  while (auto* entry = g_retiring_host_imports) {
    g_retiring_host_imports = entry->next;
    // Counted while the lock is dropped: a thread that ends inside this dlclose never says whether
    // the library was closed, and the shutdown reports it as a close that failed.
    ++g_host_closes_in_flight;
    const int result = host_dlclose(entry->handle);
    --g_host_closes_in_flight;
    if (result == 0) {
      free_object(entry);
      continue;
    }
    // The system kept the library loaded; nothing here can retry it. Recorded, and the entry
    // freed, before the log calls the host: a shutdown that completes later reports it whatever
    // happens there.
    g_host_import_leaked = true;
    record_event(nullptr, YUKILINKER_C_EVENT_CLEANUP_FAILURE, YUKILINKER_C_IO_ERROR,
                 entry->library);
    if (g_operation_error) g_operation_error->cleanup_incomplete = 1;
    closed = false;
    char library[sizeof(entry->library)];
    memcpy(library, entry->library, sizeof(library));
    free_object(entry);
    host_log_error("yukilinker: host import %s could not be closed", library);
  }
  return closed;
}

namespace {

// The binding of name through context's imports in mask (bit i the i-th import). image is the
// image a host call is made for (CallbackGuard), or null.
HostImportResult find_host_import(Context* context, ImageState* image, uint32_t mask,
                                  const char* name, const char* version, uint32_t relocation_type) {
  HostImportResult result;
  uint32_t index = 0;
  for (HostImport* entry = context->host_imports; entry; entry = entry->next, ++index) {
    if ((mask & (uint32_t{1} << index)) == 0 || !lists(*entry, name)) continue;
    result.listed = true;
    Refusal reason;
    void* address = nullptr;
    {
      // Every object the library name could mean, and each one's own entry for the symbol, copied
      // inside the system linker's callback.
      CandidateSearch search(entry->library, name, version);
      (void)host_iterate_phdr(find_candidates, &search);
      if (search.no_memory) {
        // The first failure is the operation's: a NO_MEMORY, not a refusal, so no reason is
        // recorded (failed, not refused).
        fail(YUKILINKER_C_NO_MEMORY,
             relocation_type == kLookupReference ? YUKILINKER_C_METADATA : YUKILINKER_C_RELOCATIONS,
             name, version);
        result.failed = true;
        return result;
      }
      size_t defining = 0;
      for (size_t i = 0; i < search.count; ++i)
        if (search.candidates[i].defined) ++defining;
      // Not defined by any of them (a library one needs may define it): no binding.
      if (defining == 0) continue;
      // Before the host call, over every candidate: whichever one the import's handle turns out
      // to be, its dlsym runs no resolver and touches no TLS for this name.
      for (size_t i = 0; i < search.count && !reason; ++i)
        if (search.candidates[i].defined)
          reason = lookup_refusal(search.candidates[i].symbol, relocation_type);
      if (!reason) {
        // Then the host's own answer for the import's handle.
        bool found;
        {
          CallbackGuard callback_scope(image);
          found = host_dlsym(entry->handle, name, version, &address);
        }
        // The one candidate whose mappings hold the address must be the one whose own entry
        // gives it: bias + st_value, with no wrap, and where its type says.
        const uintptr_t plain = untagged(address);
        size_t owner = SIZE_MAX;
        size_t owners = 0;
        for (size_t i = 0; found && i < search.count; ++i)
          if (maps(search, search.candidates[i], plain, 0, false)) {
            owner = i;
            ++owners;
          }
        uintptr_t expected = 0;
        if (!found)
          reason = {"not found by the system's dlsym",
                    YUKILINKER_C_HOST_IMPORT_NOT_FOUND_BY_SYSTEM};
        else if (owners == 0)
          reason = {"outside the library's mappings", YUKILINKER_C_HOST_IMPORT_OUTSIDE_MAPPINGS};
        else if (owners != 1)
          reason = {"in the mappings of two libraries of that name",
                    YUKILINKER_C_HOST_IMPORT_TWO_OWNERS};
        else if (!search.candidates[owner].defined ||
                 __builtin_add_overflow(search.candidates[owner].bias,
                                        search.candidates[owner].symbol.st_value, &expected) ||
                 expected != plain)
          reason = {"not the symbol the system's dlsym returns",
                    YUKILINKER_C_HOST_IMPORT_NOT_SYSTEM_VALUE};
        else if (!(reason = refusal(search.candidates[owner].symbol, relocation_type)))
          reason = placement_refusal(search, search.candidates[owner], plain, relocation_type);
      }
    }
    if (reason) {
      host_log_error("yukilinker: host import %s from %s refused: %s", name, entry->library,
                     reason.text);
      result.refused = true;
      result.reason = reason.reason;
      (void)snprintf(result.library, sizeof(result.library), "%s", entry->library);
      return result;
    }
    result.found = true;
    result.address = address;
    result.handle = entry->handle;
    result.index = index;
    return result;
  }
  return result;
}

}  // namespace

HostImportResult resolve_host_import(ImageState* image, const char* name, const char* version,
                                     uint32_t relocation_type) {
  return find_host_import(image->context, image, UINT32_MAX, name, version, relocation_type);
}

HostImportResult lookup_host_import(Context* context, ImageState* caller, uint32_t mask,
                                    const char* name, const char* version) {
  return find_host_import(context, caller, mask, name, version, kLookupReference);
}

}  // namespace yukilinker::internal
