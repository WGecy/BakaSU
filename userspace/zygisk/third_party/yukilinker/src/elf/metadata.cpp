// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

bool mapped_bytes(ImageState* image, const void* pointer, size_t bytes) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  const uintptr_t bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  return address >= bias && address_space_contains(image->memory, pointer, bytes) &&
         elf::segment_contains(image->memory.program_headers, image->memory.program_header_count,
                               address - bias, bytes, PF_R);
}

template <typename T>
const T* dynamic_pointer(ImageState* image, const ElfW(Dyn) & entry) {
  return runtime_pointer<T>(image, entry.d_un.d_ptr);
}

bool parse_dynamic_tags(ImageState* image, size_t dynamic_index, DynamicParse* parse) {
  const ElfW(Phdr) & dynamic_ph = image->memory.program_headers[dynamic_index];
  parse->entries = runtime_pointer<ElfW(Dyn)>(image, dynamic_ph.p_vaddr);
  parse->entry_limit = dynamic_ph.p_memsz / sizeof(ElfW(Dyn));
  if (parse->entries == nullptr || parse->entry_limit == 0 ||
      !mapped_bytes(image, parse->entries, parse->entry_limit * sizeof(ElfW(Dyn))))
    return false;

  for (size_t i = 0; i < parse->entry_limit; ++i) {
    const ElfW(Dyn) & entry = parse->entries[i];
    if (entry.d_tag == DT_NULL) {
      parse->terminated = true;
      break;
    }
    switch (entry.d_tag) {
#if defined(__aarch64__)
      case 0x70000003:  // DT_AARCH64_PAC_PLT needs authenticated GOT entries.
        return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION, "DT_AARCH64_PAC_PLT");
      case 0x70000009:  // DT_AARCH64_MEMTAG_MODE
      case 0x7000000b:  // DT_AARCH64_MEMTAG_HEAP
      case 0x7000000c:  // DT_AARCH64_MEMTAG_STACK
      case 0x7000000d:  // DT_AARCH64_MEMTAG_GLOBALS
      case 0x7000000f:  // DT_AARCH64_MEMTAG_GLOBALSSZ
        // Private mappings have no MTE layout, tagging or host-process mode agreement.
        return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION, "DT_AARCH64_MEMTAG");
#endif
      case 0x6000000f:
      case 0x60000011:
        if (entry.d_tag != (sizeof(void*) == 8 ? 0x60000011 : 0x6000000f)) return false;
        // A packed table outside the image is malformed; one too small for its header is
        // refused when it is applied (apply_packed), as in the system linker.
        image->relocations.packed = dynamic_pointer<uint8_t>(image, entry);
        if (!image->relocations.packed) return false;
        break;
      case 0x60000010:
      case 0x60000012:
        if (entry.d_tag != (sizeof(void*) == 8 ? 0x60000012 : 0x60000010)) return false;
        image->relocations.packed_bytes = entry.d_un.d_val;
        break;
      case DT_VERSYM:
        image->symbols.versions = dynamic_pointer<ElfW(Half)>(image, entry);
        if (!image->symbols.versions) return false;
        break;
      case DT_VERNEED:
        image->symbols.needs = dynamic_pointer<ElfW(Verneed)>(image, entry);
        if (!image->symbols.needs) return false;
        break;
      case DT_VERNEEDNUM:
        image->symbols.need_count = entry.d_un.d_val;
        break;
      case DT_VERDEF:
        image->symbols.definitions = dynamic_pointer<ElfW(Verdef)>(image, entry);
        if (!image->symbols.definitions) return false;
        break;
      case DT_VERDEFNUM:
        image->symbols.definition_count = entry.d_un.d_val;
        break;
      case DT_SYMBOLIC:
        image->symbolic = true;
        break;
      case DT_FLAGS_1:
        if (entry.d_un.d_val & DF_1_NODELETE) {
          image->flags |= RTLD_NODELETE;
          image->nodelete_reasons |= YUKILINKER_C_NODELETE_ELF;
        }
        if (entry.d_un.d_val & DF_1_GLOBAL) {
          image->global_group = true;
          image->flags |= RTLD_GLOBAL;
        }
        break;
      case DT_TEXTREL:
        return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
      case DT_FLAGS:
        if (entry.d_un.d_val & DF_TEXTREL)
          return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
        image->symbolic |= (entry.d_un.d_val & DF_SYMBOLIC) != 0;
        break;
      case DT_RUNPATH:
        parse->runpath_offset = entry.d_un.d_val;
        break;
      case DT_SONAME:
        parse->soname_offset = entry.d_un.d_val;
        break;
      case DT_SYMTAB:
        image->symbols.entries = dynamic_pointer<ElfW(Sym)>(image, entry);
        break;
      case DT_STRTAB:
        image->symbols.strings = dynamic_pointer<char>(image, entry);
        break;
      case DT_STRSZ:
        image->symbols.string_bytes = entry.d_un.d_val;
        break;
      // Every entry-size and kind entry is checked, as in the system linker: a later valid
      // duplicate does not repair an invalid one.
      case DT_SYMENT:
        if (entry.d_un.d_val != sizeof(ElfW(Sym))) return false;
        break;
      // A table outside the image is malformed, not absent, even when the other one is valid.
      case DT_HASH:
        parse->sysv_hash = dynamic_pointer<uint32_t>(image, entry);
        if (!parse->sysv_hash) return false;
        break;
      case DT_GNU_HASH:
        parse->gnu_hash = dynamic_pointer<uint32_t>(image, entry);
        if (!parse->gnu_hash) return false;
        break;
      case kDynamicRelTag:
        image->relocations.rel = dynamic_pointer<ElfRelocation>(image, entry);
        break;
      case kDynamicRelSizeTag:
        image->relocations.rel_bytes = entry.d_un.d_val;
        break;
      case kDynamicRelEntryTag:
        if (entry.d_un.d_val != sizeof(ElfRelocation)) return false;
        break;
      case kForeignRelTag:
      case kForeignRelSizeTag:
        return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
      case DT_JMPREL:
        image->relocations.plt = dynamic_pointer<ElfRelocation>(image, entry);
        break;
      case DT_PLTRELSZ:
        image->relocations.plt_bytes = entry.d_un.d_val;
        break;
      case DT_PLTREL:
        if (entry.d_un.d_val != static_cast<decltype(entry.d_un.d_val)>(kDynamicRelTag))
          return false;
        break;
      case DT_RELR:
#if DT_ANDROID_RELR != DT_RELR
      case DT_ANDROID_RELR:
#endif  // #if DT_ANDROID_RELR != DT_RELR
        image->relocations.relr = dynamic_pointer<ElfW(Addr)>(image, entry);
        break;
      case DT_RELRSZ:
#if DT_ANDROID_RELRSZ != DT_RELRSZ
      case DT_ANDROID_RELRSZ:
#endif  // #if DT_ANDROID_RELRSZ != DT_RELRSZ
        image->relocations.relr_bytes = entry.d_un.d_val;
        break;
      case DT_RELRENT:
#if DT_ANDROID_RELRENT != DT_RELRENT
      case DT_ANDROID_RELRENT:
#endif  // #if DT_ANDROID_RELRENT != DT_RELRENT
        if (entry.d_un.d_val != sizeof(ElfW(Addr))) return false;
        break;
      case DT_INIT: {
        auto* pointer =
            entry.d_un.d_ptr == 0 ? nullptr : runtime_pointer<uint8_t>(image, entry.d_un.d_ptr);
        if (entry.d_un.d_ptr != 0 && pointer == nullptr) return false;
        image->lifecycle.init = reinterpret_cast<LifecycleFunction>(pointer);
        break;
      }
      case DT_INIT_ARRAY:
        image->lifecycle.init_array = runtime_pointer<LifecycleFunction>(image, entry.d_un.d_ptr);
        if (entry.d_un.d_ptr != 0 && image->lifecycle.init_array == nullptr) return false;
        break;
      case DT_INIT_ARRAYSZ:
        parse->init_array_bytes = entry.d_un.d_val;
        break;
      case DT_FINI: {
        auto* pointer =
            entry.d_un.d_ptr == 0 ? nullptr : runtime_pointer<uint8_t>(image, entry.d_un.d_ptr);
        if (entry.d_un.d_ptr != 0 && pointer == nullptr) return false;
        image->lifecycle.fini = reinterpret_cast<LifecycleFunction>(pointer);
        break;
      }
      case DT_FINI_ARRAY:
        image->lifecycle.fini_array = runtime_pointer<LifecycleFunction>(image, entry.d_un.d_ptr);
        if (entry.d_un.d_ptr != 0 && image->lifecycle.fini_array == nullptr) return false;
        break;
      case DT_FINI_ARRAYSZ:
        parse->fini_array_bytes = entry.d_un.d_val;
        break;
      default:
        break;
    }
  }

  if (!parse->terminated || image->symbols.entries == nullptr ||
      image->symbols.strings == nullptr || image->symbols.string_bytes == 0)
    return false;

  if (parse->init_array_bytes % sizeof(LifecycleFunction) != 0 ||
      parse->fini_array_bytes % sizeof(LifecycleFunction) != 0)
    return false;
  image->lifecycle.init_count = parse->init_array_bytes / sizeof(LifecycleFunction);
  image->lifecycle.fini_count = parse->fini_array_bytes / sizeof(LifecycleFunction);
  if ((parse->init_array_bytes != 0 &&
       (image->lifecycle.init_array == nullptr ||
        !mapped_bytes(image, image->lifecycle.init_array, parse->init_array_bytes))) ||
      (parse->fini_array_bytes != 0 &&
       (image->lifecycle.fini_array == nullptr ||
        !mapped_bytes(image, image->lifecycle.fini_array, parse->fini_array_bytes))))
    return false;

  if (image->relocations.packed_bytes != 0 &&
      !mapped_bytes(image, image->relocations.packed, image->relocations.packed_bytes))
    return false;
  if (!mapped_bytes(image, image->symbols.strings, image->symbols.string_bytes)) return false;
  // One scan of the whole table, from its end.
  if (const void* last = memrchr(image->symbols.strings, 0, image->symbols.string_bytes))
    image->symbols.string_terminated =
        static_cast<size_t>(static_cast<const char*>(last) - image->symbols.strings) + 1;
  if ((image->relocations.rel_bytes != 0 &&
       (image->relocations.rel == nullptr ||
        !mapped_bytes(image, image->relocations.rel, image->relocations.rel_bytes))) ||
      (image->relocations.plt_bytes != 0 &&
       (image->relocations.plt == nullptr ||
        !mapped_bytes(image, image->relocations.plt, image->relocations.plt_bytes))) ||
      (image->relocations.relr_bytes != 0 &&
       (image->relocations.relr == nullptr ||
        !mapped_bytes(image, image->relocations.relr, image->relocations.relr_bytes))))
    return false;
  return true;
}

size_t symbol_count_from_sysv(ImageState* image, const uint32_t* table) {
  if (table == nullptr || !mapped_bytes(image, table, 2 * sizeof(uint32_t))) return 0;
  uint32_t bucket_count = table[0];
  uint32_t chain_count = table[1];
  size_t words;
  if (add_overflow(2, bucket_count, &words) || add_overflow(words, chain_count, &words) ||
      multiply_overflow(words, sizeof(uint32_t), &words) || !mapped_bytes(image, table, words))
    return 0;
  if (!bucket_count || !chain_count) return 0;
  const uint32_t* buckets = table + 2;
  const uint32_t* chains = buckets + bucket_count;
  size_t walked = 0;
  for (uint32_t i = 0; i < bucket_count; ++i) {
    uint32_t index = buckets[i];
    while (index) {
      if (index >= chain_count || ++walked > chain_count) return 0;
      index = chains[index];
    }
  }
  return chain_count;
}

size_t symbol_count_from_gnu(ImageState* image, const uint32_t* table) {
  if (table == nullptr || !mapped_bytes(image, table, 4 * sizeof(uint32_t))) return 0;
  uint32_t bucket_count = table[0];
  uint32_t first_hashed_symbol = table[1];
  uint32_t bloom_words = table[2];
  if (bucket_count == 0 || !is_power_of_two(bloom_words) || table[3] >= 32) return 0;

  const auto* bloom = reinterpret_cast<const ElfW(Addr)*>(table + 4);
  size_t bloom_bytes;
  if (multiply_overflow(bloom_words, sizeof(ElfW(Addr)), &bloom_bytes) ||
      !mapped_bytes(image, bloom, bloom_bytes))
    return 0;
  const auto* buckets =
      reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(bloom) + bloom_bytes);
  size_t bucket_bytes;
  if (multiply_overflow(bucket_count, sizeof(uint32_t), &bucket_bytes) ||
      !mapped_bytes(image, buckets, bucket_bytes))
    return 0;
  const uint32_t* chains = buckets + bucket_count;

  uint32_t largest_bucket = 0;
  for (uint32_t i = 0; i < bucket_count; ++i) largest_bucket = std::max(buckets[i], largest_bucket);
  if (largest_bucket < first_hashed_symbol) return first_hashed_symbol;

  size_t symbol_index = largest_bucket;
  for (;;) {
    size_t chain_index = symbol_index - first_hashed_symbol;
    size_t displacement;
    if (multiply_overflow(chain_index, sizeof(uint32_t), &displacement) ||
        displacement > UINTPTR_MAX - reinterpret_cast<uintptr_t>(chains))
      return 0;
    const auto* chain =
        reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(chains) + displacement);
    if (!mapped_bytes(image, chain, sizeof(*chain))) return 0;
    uint32_t value = *chain;
    ++symbol_index;
    // A lookup can start at any bucket, so the whole chain array has to be there.
    if ((value & 1U) != 0)
      return mapped_bytes(image, chains, (symbol_index - first_hashed_symbol) * sizeof(uint32_t))
                 ? symbol_index
                 : 0;
  }
}

bool establish_symbol_count(ImageState* image, const DynamicParse& parse) {
  size_t sysv_count = symbol_count_from_sysv(image, parse.sysv_hash);
  size_t gnu_count = symbol_count_from_gnu(image, parse.gnu_hash);
  if ((parse.sysv_hash && !sysv_count) || (parse.gnu_hash && !gnu_count) ||
      (sysv_count && gnu_count && gnu_count > sysv_count))
    return false;
  image->symbols.count = sysv_count > gnu_count ? sysv_count : gnu_count;
  image->symbols.sysv_hash = parse.sysv_hash;
  image->symbols.gnu_hash = parse.gnu_hash;
  if (image->symbols.count > size_t{1024} * 1024) return false;
  size_t symbol_bytes;
  return image->symbols.count != 0 &&
         !multiply_overflow(image->symbols.count, sizeof(ElfW(Sym)), &symbol_bytes) &&
         mapped_bytes(image, image->symbols.entries, symbol_bytes);
}

// A name ends inside the table exactly when it starts before the table's last NUL.
bool symbol_name_valid(const DynamicSymbols& symbols, const ElfW(Sym) & symbol) {
  return symbol.st_name < symbols.string_terminated;
}

const char* string_at(const ImageState* image, size_t offset) {
  const auto& symbols = image->symbols;
  return offset < symbols.string_terminated ? symbols.strings + offset : nullptr;
}

template <typename T>
const T* version_pointer(ImageState* image, const void* base, size_t offset) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(base);
  if (offset > UINTPTR_MAX - address || (address + offset) % alignof(T)) return nullptr;
  auto* result = reinterpret_cast<const T*>(address + offset);
  return mapped_bytes(image, result, sizeof(T)) ? result : nullptr;
}

const ElfW(Verneed) * next_verneed(ImageState* image, const ElfW(Verneed) * need) {
  return version_pointer<ElfW(Verneed)>(image, need, need->vn_next);
}

namespace {

uint32_t gnu_name_hash(const char* name) {
  uint32_t hash = 5381;
  for (const auto* cursor = reinterpret_cast<const uint8_t*>(name); *cursor != 0; ++cursor)
    hash = hash * 33 + *cursor;
  return hash;
}

// Walk the chains validate_versions has checked, in chain order.
template <typename Visit>
void walk_needs(ImageState* image, Visit visit) {
  const auto& symbols = image->symbols;
  const auto* need = symbols.needs;
  for (size_t i = 0; i < symbols.need_count; ++i) {
    const auto* aux = version_pointer<ElfW(Vernaux)>(image, need, need->vn_aux);
    for (size_t j = 0; j < need->vn_cnt; ++j) {
      visit(*aux);
      if (j + 1 != need->vn_cnt) aux = version_pointer<ElfW(Vernaux)>(image, aux, aux->vna_next);
    }
    if (i + 1 != symbols.need_count)
      need = version_pointer<ElfW(Verneed)>(image, need, need->vn_next);
  }
}

template <typename Visit>
void walk_definitions(ImageState* image, Visit visit) {
  const auto& symbols = image->symbols;
  const auto* def = symbols.definitions;
  for (size_t i = 0; i < symbols.definition_count; ++i) {
    visit(*def, *version_pointer<ElfW(Verdaux)>(image, def, def->vd_aux));
    if (i + 1 != symbols.definition_count)
      def = version_pointer<ElfW(Verdef)>(image, def, def->vd_next);
  }
}

template <typename T>
T* version_table(ImageState* image, size_t count) {
  size_t bytes;
  if (multiply_overflow(count, sizeof(T), &bytes)) {
    errno = ENOMEM;
    return nullptr;
  }
  return static_cast<T*>(metadata_allocate(bytes, alignof(T), &image->memory.metadata));
}

// A direct table while its indexes are dense enough: the largest at most four per record and a
// few spare. Past that a sorted compact table costs less, however large the indexes.
bool direct_version_table(size_t records, size_t largest) {
  size_t limit;
  return !multiply_overflow(records, 4, &limit) && !add_overflow(limit, 16, &limit) &&
         largest <= limit;
}

bool version_name_before(const VersionName& a, const VersionName& b) {
  return a.index != b.index ? a.index < b.index : a.order < b.order;
}

void sift_version_name(VersionName* names, size_t root, size_t count) {
  for (size_t child; (child = root * 2 + 1) < count; root = child) {
    if (child + 1 < count && version_name_before(names[child], names[child + 1])) ++child;
    if (!version_name_before(names[root], names[child])) return;
    const VersionName swap = names[root];
    names[root] = names[child];
    names[child] = swap;
  }
}

void sort_version_names(VersionName* names, size_t count) {
  for (size_t i = count / 2; i-- != 0;) sift_version_name(names, i, count);
  for (size_t end = count; end > 1; --end) {
    const VersionName swap = names[0];
    names[0] = names[end - 1];
    names[end - 1] = swap;
    sift_version_name(names, 0, end - 1);
  }
}

// Builds names for one kind of record from the (index, name) pairs walk visits, in chain order:
// the first name of each index wins. records is how many pairs walk visits.
template <typename Walk>
bool index_version_names(ImageState* image, VersionTable* names, size_t records, Walk walk) {
  size_t ids = 0;
  walk([&](uint32_t id, const char*) {
    if (id >= ids) ids = id + 1;
  });
  if (ids == 0) return true;
  if (direct_version_table(records, ids - 1)) {
    const char** direct = version_table<const char*>(image, ids);
    if (!direct) return false;
    walk([&](uint32_t id, const char* name) {
      if (!direct[id]) direct[id] = name;
    });
    names->direct = direct;
    names->count = ids;
    return true;
  }
  // The compact form: one entry per record in chain order, heap-sorted by (index, chain order) so
  // the first name of each index comes first, then only that one is kept. A heap sort needs no
  // memory of its own and stays O(n log n) for any order of the records.
  auto* compact = version_table<VersionName>(image, records);
  if (!compact) return false;
  size_t count = 0;
  walk([&](uint32_t id, const char* name) {
    compact[count] = {id, static_cast<uint32_t>(count), name};
    ++count;
  });
  sort_version_names(compact, count);
  size_t kept = 0;
  for (size_t i = 0; i < count; ++i)
    if (kept == 0 || compact[kept - 1].index != compact[i].index) compact[kept++] = compact[i];
  names->compact = compact;
  names->count = kept;
  return true;
}

// The name of index id in names, or null.
const char* version_name(const VersionTable& names, unsigned id) {
  if (names.direct) return id < names.count ? names.direct[id] : nullptr;
  size_t low = 0;
  size_t high = names.count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (names.compact[middle].index < id)
      low = middle + 1;
    else
      high = middle;
  }
  return low < names.count && names.compact[low].index == id ? names.compact[low].name : nullptr;
}

// Every version index the image's own tables give a version name: each Vernaux's and each
// non-BASE Verdef's. YukiLinker takes each index to stand for one version: two records that give
// one index are refused, in one table or across both (Bionic keeps the last, the tables this
// replaces kept the first). Indexes 0 and 1 keep their meaning (local, global) and are not
// compared: a Vernaux never takes one, and the BASE Verdef, the file's own, names no version.
bool version_indexes_unique(ImageState* image, size_t records) {
  if (records < 2) return true;
  size_t bytes;
  if (multiply_overflow(records, sizeof(uint16_t), &bytes)) return false;
  auto* indexes = static_cast<uint16_t*>(malloc(bytes));
  if (!indexes) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_METADATA);
  size_t count = 0;
  const auto add = [&](uint16_t index) {
    if (index > 1 && count < records) indexes[count++] = index;
  };
  walk_needs(image, [&](const ElfW(Vernaux) & aux) { add(aux.vna_other & 0x7fff); });
  walk_definitions(image, [&](const ElfW(Verdef) & def, const ElfW(Verdaux) &) {
    if (!(def.vd_flags & VER_FLG_BASE)) add(def.vd_ndx & 0x7fff);
  });
  qsort(indexes, count, sizeof(uint16_t), [](const void* left, const void* right) {
    return static_cast<int>(*static_cast<const uint16_t*>(left)) -
           static_cast<int>(*static_cast<const uint16_t*>(right));
  });
  bool unique = true;
  for (size_t i = 1; i < count && unique; ++i) unique = indexes[i] != indexes[i - 1];
  free(indexes);
  return unique;
}

// Indexes the checked chains once, as Bionic's VersionTracker does, so a version check costs one
// step (a bisection for sparse indexes) however long they are: the name of each Vernaux and Verdef
// index (the first in chain order, which the walks this replaces found), and the first Verdef index
// of each name. An index is at most 0x7fff; a table is bounded by the chains' own length.
bool index_versions(ImageState* image) {
  auto& symbols = image->symbols;
  size_t need_records = 0;
  walk_needs(image, [&](const ElfW(Vernaux) &) { ++need_records; });
  if (!index_version_names(image, &symbols.need_names, need_records, [&](auto visit) {
        walk_needs(image, [&](const ElfW(Vernaux) & aux) {
          visit(aux.vna_other & 0x7fff, string_at(image, aux.vna_name));
        });
      }))
    return false;
  if (!index_version_names(
          image, &symbols.definition_names, symbols.definition_count, [&](auto visit) {
            walk_definitions(image, [&](const ElfW(Verdef) & def, const ElfW(Verdaux) & aux) {
              if (!(def.vd_flags & VER_FLG_BASE))
                visit(def.vd_ndx & 0x7fff, string_at(image, aux.vda_name));
            });
          }))
    return false;
  if (symbols.definition_names.count == 0) return true;
  // At most half full, so every probe ends at an empty slot.
  size_t slots = 8;
  while (slots < symbols.definition_count * 2) slots *= 2;
  symbols.definition_slots = version_table<VersionSlot>(image, slots);
  if (!symbols.definition_slots) return false;
  symbols.definition_slot_mask = slots - 1;
  walk_definitions(image, [&](const ElfW(Verdef) & def, const ElfW(Verdaux) & aux) {
    // The file's own version (VER_FLG_BASE) names the file, never a symbol version: a requested
    // version of that name does not match it, as in Bionic's find_verdef_version_index.
    if (def.vd_flags & VER_FLG_BASE) return;
    const char* name = string_at(image, aux.vda_name);
    const uint32_t id = def.vd_ndx & 0x7fff;
    const size_t mask = symbols.definition_slot_mask;
    for (size_t slot = gnu_name_hash(name) & mask;; slot = (slot + 1) & mask) {
      auto& entry = symbols.definition_slots[slot];
      if (!entry.name) {
        entry = {name, id};
        break;
      }
      if (strcmp(entry.name, name) == 0) break;
    }
  });
  return true;
}

}  // namespace

// validate_versions has indexed the chains before anything asks.
const char* symbol_version(ImageState* image, size_t index, bool requirement) {
  const auto& symbols = image->symbols;
  if (!symbols.versions) return nullptr;
  const unsigned id = symbols.versions[index] & 0x7fff;
  if (id <= 1) return nullptr;
  return version_name(requirement ? symbols.need_names : symbols.definition_names, id);
}

namespace {

// One version record's bytes, for the overlap check, and what kind of record the walk read there.
struct VersionRecord {
  uintptr_t begin;
  size_t size;
  uint8_t kind;
};

enum VersionRecordKind : uint8_t { kVerneed, kVernaux, kVerdef, kVerdaux };

// How the records a walk visited lie: apart; two of one kind that start together (two entries
// that reach the same record, which the specification allows and Bionic walks once per entry); or
// any other overlap, which is malformed.
enum class VersionLayout { kDisjoint, kShared, kOverlapping };

// Records every record a walk visits, at most capacity, and finds any two that share a byte.
// Owns the malloc'd table: a temporary the image's arena would keep for its whole life.
class VersionRecords {
 public:
  VersionRecords(VersionRecord* records, size_t capacity)
      : records_(records), capacity_(capacity) {}
  ~VersionRecords() { free(records_); }
  VersionRecords(const VersionRecords&) = delete;
  VersionRecords& operator=(const VersionRecords&) = delete;

  bool add(const void* record, size_t size, VersionRecordKind kind) {
    if (!records_ || count_ == capacity_) return false;
    records_[count_++] = {reinterpret_cast<uintptr_t>(record), size, kind};
    return true;
  }

  VersionLayout layout() {
    qsort(records_, count_, sizeof(VersionRecord), [](const void* left, const void* right) {
      const auto a = static_cast<const VersionRecord*>(left)->begin;
      const auto b = static_cast<const VersionRecord*>(right)->begin;
      return a < b ? -1 : a > b ? 1 : 0;
    });
    VersionLayout result = VersionLayout::kDisjoint;
    for (size_t i = 1; i < count_; ++i) {
      const VersionRecord& previous = records_[i - 1];
      const VersionRecord& record = records_[i];
      if (record.begin - previous.begin >= previous.size) continue;
      if (record.begin != previous.begin || record.kind != previous.kind)
        return VersionLayout::kOverlapping;
      result = VersionLayout::kShared;
    }
    return result;
  }

 private:
  VersionRecord* records_;
  size_t capacity_;
  size_t count_ = 0;
};

// The distinct file bytes the readable segments map: version records the file does not hold cannot
// be distinct, whatever p_memsz says, and two segments that map the same bytes hold them once. The
// union of their file ranges [p_offset, p_offset + p_filesz), never more than the file itself. A
// record past them reads zeros, which the version checks refuse at once. This only bounds the
// record count; every record is still read through mapped_bytes. elf::inspect checked each range
// against the file, so no end overflows.
size_t file_backed_readable_bytes(const ImageState* image) {
  size_t total = 0;
  // The end of the union so far: each step takes the next range by start, so what lies before
  // covered is counted once. At most e_phnum (4096) steps of e_phnum.
  uint64_t covered = 0;
  for (;;) {
    const ElfW(Phdr)* next = nullptr;
    uint64_t next_end = 0;
    for (size_t i = 0; i < image->memory.program_header_count; ++i) {
      const ElfW(Phdr) & header = image->memory.program_headers[i];
      if (header.p_type != PT_LOAD || !(header.p_flags & PF_R) || header.p_filesz == 0) continue;
      const uint64_t end = uint64_t{header.p_offset} + header.p_filesz;
      if (end <= covered) continue;
      // The range that starts first, past what is covered; on a tie, the one that reaches furthest.
      const uint64_t start = header.p_offset < covered ? covered : header.p_offset;
      const uint64_t next_start = next ? (next->p_offset < covered ? covered : next->p_offset) : 0;
      if (!next || start < next_start || (start == next_start && end > next_end)) {
        next = &header;
        next_end = end;
      }
    }
    if (!next) return total;
    const uint64_t start = next->p_offset < covered ? covered : next->p_offset;
    if (add_overflow(total, static_cast<size_t>(next_end - start), &total)) return SIZE_MAX;
    covered = next_end;
  }
}

// The most Vernaux records an image may promise. Every Vernaux names its own index in 2..0x7fff
// (validate_versions refuses a repeat), so the Verneed side holds at most 0x7ffe of them. Real
// libraries use a few hundred.
constexpr size_t kMaxNeedRecords = 0x7ffe;

// The records the counts promise: one per Verneed plus its Vernaux records, and one per Verdef plus
// its first Verdaux, the only one read (the version's name; the rest name parents, which neither
// Bionic nor the runtime reads). Each side is bounded by what a valid image can hold, whatever the
// file's size, sparse or not; and all of them must fit the file's readable bytes at the smallest
// record size, or the chains would have to share records. So the tables built from the count, and
// the walks that follow, stay small.
bool version_record_count(ImageState* image, size_t* count) {
  const auto& symbols = image->symbols;
  size_t needs = 0;
  const auto* need = symbols.needs;
  for (size_t i = 0; i < symbols.need_count; ++i) {
    if (!need || !mapped_bytes(image, need, sizeof(*need))) return false;
    needs += need->vn_cnt;
    if (needs > kMaxNeedRecords) return false;
    if (i + 1 != symbols.need_count) {
      if (need->vn_next < sizeof(*need)) return false;
      need = version_pointer<ElfW(Verneed)>(image, need, need->vn_next);
    }
  }
  // Each Verdef is read with one Verdaux, so the Verdef side takes two records an entry whatever
  // vd_cnt says (definition_count is at most 65535). Walked once here as well, so a chain that
  // leaves the mapped bytes, or reads zeros, is refused before anything is allocated.
  const auto* def = symbols.definitions;
  for (size_t i = 0; i < symbols.definition_count; ++i) {
    if (!def || !mapped_bytes(image, def, sizeof(*def)) || !def->vd_cnt) return false;
    if (i + 1 != symbols.definition_count) {
      if (def->vd_next < sizeof(*def)) return false;
      def = version_pointer<ElfW(Verdef)>(image, def, def->vd_next);
    }
  }
  // Every Verneed has at least one Vernaux (validate_versions), so the entry count is bounded too.
  if (symbols.need_count > needs) return false;
  const size_t total = symbols.need_count + needs + 2 * symbols.definition_count;
  if (total > file_backed_readable_bytes(image) / sizeof(ElfW(Verdaux))) return false;
  *count = total;
  return true;
}

// The longest version name (vn_file, vna_name, vda_name) the runtime takes, without its NUL: a
// resource limit of YukiLinker's own, which Bionic does not have.
constexpr size_t kMaxVersionName = 4096;

// Every version name's offset, checked once the walks are done: sorted, so a name that ends at a
// NUL another one already reached costs nothing, and each scan stops at kMaxVersionName + 1
// bytes. Nothing reads a version name (strlen, strcmp, a hash) before this has passed.
class VersionNames {
 public:
  VersionNames(size_t* offsets, size_t capacity) : offsets_(offsets), capacity_(capacity) {}
  ~VersionNames() { free(offsets_); }
  VersionNames(const VersionNames&) = delete;
  VersionNames& operator=(const VersionNames&) = delete;

  bool add(size_t offset) {
    if (!offsets_ || count_ == capacity_) return false;
    offsets_[count_++] = offset;
    return true;
  }

  bool bounded(const DynamicSymbols& symbols) {
    qsort(offsets_, count_, sizeof(size_t), [](const void* left, const void* right) {
      const auto a = *static_cast<const size_t*>(left);
      const auto b = *static_cast<const size_t*>(right);
      return a < b ? -1 : a > b ? 1 : 0;
    });
    // The first NUL at or past the previous offset; offsets before it end there.
    size_t nul = 0;
    bool have_nul = false;
    for (size_t i = 0; i < count_; ++i) {
      const size_t offset = offsets_[i];
      if (offset >= symbols.string_terminated) return false;
      if (!have_nul || nul < offset) {
        const size_t room = symbols.string_terminated - offset;
        const size_t window = room < kMaxVersionName + 1 ? room : kMaxVersionName + 1;
        const void* end = memchr(symbols.strings + offset, 0, window);
        if (!end) return false;
        nul = static_cast<size_t>(static_cast<const char*>(end) - symbols.strings);
        have_nul = true;
      }
      if (nul - offset > kMaxVersionName) return false;
    }
    return true;
  }

 private:
  size_t* offsets_;
  size_t capacity_;
  size_t count_ = 0;
};

}  // namespace

// YukiLinker refuses version chains that share a record, which the ELF specification does not
// forbid outright: two Verneed entries whose vn_aux reach one Vernaux chain, a chain that joins
// another's tail, or a Verdaux two Verdefs name. Bionic accepts them and walks the shared chain
// once per entry; refused here (YUKILINKER_C_UNSUPPORTED, so a host may fall back), every walk
// visits each record once. A record that overlaps another of another kind or at another start, such
// as an aux record inside a Verneed, is malformed (YUKILINKER_C_INVALID_ELF).
bool validate_versions(ImageState* image) {
  auto& symbols = image->symbols;
  if (symbols.versions &&
      !mapped_bytes(image, symbols.versions, symbols.count * sizeof(ElfW(Half))))
    return false;
  if (symbols.need_count > 65535 || symbols.definition_count > 65535) return false;
  size_t record_count = 0;
  if (!version_record_count(image, &record_count)) return false;
  size_t record_bytes = 0;
  if (multiply_overflow(record_count, sizeof(VersionRecord), &record_bytes)) return false;
  auto* storage = static_cast<VersionRecord*>(record_bytes != 0 ? malloc(record_bytes) : nullptr);
  if (record_bytes != 0 && !storage) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_METADATA);
  VersionRecords records(storage, record_count);
  // At most one name per record: vn_file, vna_name or vda_name.
  auto* name_storage =
      static_cast<size_t*>(record_count != 0 ? malloc(record_count * sizeof(size_t)) : nullptr);
  if (record_count != 0 && !name_storage)
    return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_METADATA);
  VersionNames names(name_storage, record_count);
  const auto* need = symbols.needs;
  for (size_t i = 0; i < symbols.need_count; ++i) {
    if (need->vn_version != 1 || !need->vn_cnt || !names.add(need->vn_file) ||
        need->vn_aux < sizeof(*need) || !records.add(need, sizeof(*need), kVerneed))
      return false;
    const auto* aux = version_pointer<ElfW(Vernaux)>(image, need, need->vn_aux);
    for (size_t j = 0; j < need->vn_cnt; ++j) {
      if (!aux || !names.add(aux->vna_name) || (aux->vna_other & 0x7fff) <= 1 ||
          !records.add(aux, sizeof(*aux), kVernaux))
        return false;
      if (j + 1 != need->vn_cnt) {
        if (aux->vna_next < sizeof(*aux)) return false;
        aux = version_pointer<ElfW(Vernaux)>(image, aux, aux->vna_next);
      } else if (aux->vna_next != 0)
        return false;
    }
    if (i + 1 != symbols.need_count)
      need = version_pointer<ElfW(Verneed)>(image, need, need->vn_next);
    else if (need->vn_next != 0)
      return false;
  }
  const auto* def = symbols.definitions;
  for (size_t i = 0; i < symbols.definition_count; ++i) {
    if (def->vd_version != 1 || !def->vd_cnt || def->vd_aux < sizeof(*def) ||
        !records.add(def, sizeof(*def), kVerdef))
      return false;
    // Only the first Verdaux is read: it names the version. One that promises more points on to
    // them; the last one ends the chain.
    const auto* aux = version_pointer<ElfW(Verdaux)>(image, def, def->vd_aux);
    if (!aux || !names.add(aux->vda_name) || !records.add(aux, sizeof(*aux), kVerdaux) ||
        (def->vd_cnt == 1 ? aux->vda_next != 0 : aux->vda_next < sizeof(*aux)))
      return false;
    if (i + 1 != symbols.definition_count)
      def = version_pointer<ElfW(Verdef)>(image, def, def->vd_next);
    else if (def->vd_next != 0)
      return false;
  }
  // Every record read above is well formed. Two entries that reach one record (a chain another
  // entry's chain joins, a Verdaux two Verdefs name) are legal, and the system linker loads them:
  // a refusal it can fall back from. Any other overlap is malformed.
  switch (records.layout()) {
    case VersionLayout::kDisjoint:
      break;
    case VersionLayout::kShared:
      host_log_error("yukilinker: version records shared between entries are not supported");
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
    case VersionLayout::kOverlapping:
      return false;
  }
  if (!names.bounded(symbols)) return false;
  if (!version_indexes_unique(image, record_count)) return false;
  if (!index_versions(image)) return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_METADATA);
  for (size_t i = 0; i < symbols.count; ++i) {
    if (!symbol_name_valid(symbols, symbols.entries[i])) return false;
    if (symbols.versions && (symbols.versions[i] & 0x7fff) > 1 &&
        !symbol_version(image, i, symbols.entries[i].st_shndx == SHN_UNDEF))
      return false;
  }
  return true;
}

const char* requires_system_runtime(const ImageState* image) {
  for (size_t i = 0; i < image->symbols.count; ++i) {
    const auto& symbol = image->symbols.entries[i];
    const char* name = string_at(image, symbol.st_name);
    if (!name) return "";
    if (strcmp(name, "__cfi_check") == 0 || strncmp(name, "__cfi_slowpath", 14) == 0 ||
        strcmp(name, "__cxa_throw") == 0 || strncmp(name, "_Unwind_", 8) == 0 ||
        strcmp(name, "__gxx_personality_v0") == 0 || strcmp(name, "__emutls_get_address") == 0 ||
        strncmp(name, "__emutls_v.", 11) == 0)
      return name;
  }
  return nullptr;
}

namespace {

uint32_t sysv_name_hash(const char* name) {
  uint32_t hash = 0;
  for (const auto* cursor = reinterpret_cast<const uint8_t*>(name); *cursor != 0; ++cursor) {
    hash = (hash << 4) + *cursor;
    const uint32_t high = hash & 0xf0000000U;
    hash ^= high;
    hash ^= high >> 24;
  }
  return hash;
}

// Bionic's find_verdef_version_index: the index of the definition named version, or 1 (global)
// when the image defines no such version.
unsigned defined_version_index(ImageState* image, const char* version) {
  const auto& symbols = image->symbols;
  if (!symbols.definition_slots) return 1;
  const size_t mask = symbols.definition_slot_mask;
  for (size_t slot = gnu_name_hash(version) & mask;; slot = (slot + 1) & mask) {
    const auto& entry = symbols.definition_slots[slot];
    if (!entry.name) return 1;
    if (strcmp(entry.name, version) == 0) return entry.index;
  }
}

}  // namespace

// Bionic's dlsym still finds an image while dlclose runs its destructors, until soinfo_unload
// removes it. retiring admits a non-TLS symbol of an image this thread's collection is retiring;
// a TLS one would pin it again (tls_users).
bool eligible_symbol(ImageState* image, size_t index, const char* name, const char* version,
                     bool external, bool retiring) {
  const auto& symbol = image->symbols.entries[index];
  const auto binding = ELF64_ST_BIND(symbol.st_info);
  const auto visibility = (symbol.st_other & 3);
  const bool visible =
      accessible_image(image) ||
      (retiring && ELF64_ST_TYPE(symbol.st_info) != STT_TLS && retiring_here(image));
  if (!visible || symbol.st_shndx == SHN_UNDEF || !symbol_name_valid(image->symbols, symbol) ||
      strcmp(image->symbols.strings + symbol.st_name, name) != 0 ||
      (external && ((binding != STB_GLOBAL && binding != STB_WEAK) || visibility == STV_HIDDEN ||
                    visibility == STV_INTERNAL)))
    return false;
  // Bionic's check_symbol_version: without a version table every request matches.
  const auto* versions = image->symbols.versions;
  if (!versions) return true;
  if (!version) return !(versions[index] & 0x8000);
  return (versions[index] & 0x7fff) == defined_version_index(image, version);
}

// A name reaches a symbol only through the image's own hash table, as in the system linker: the GNU
// table when there is one (its bloom filter, then the bucket's chain, whose entries carry the
// name's hash), else the SysV table. establish_symbol_count validated both tables' bounds.
const ElfW(Sym) * find_defined_symbol(ImageState* image, const char* name, const char* version,
                                      bool external, bool retiring) {
  if (!image || !name) return nullptr;
  const auto& symbols = image->symbols;
  if (const uint32_t* table = symbols.gnu_hash) {
    constexpr uint32_t kBloomBits = sizeof(ElfW(Addr)) * 8;
    const uint32_t bucket_count = table[0];
    const uint32_t first_hashed = table[1];
    const auto* bloom = reinterpret_cast<const ElfW(Addr)*>(table + 4);
    const auto* buckets = reinterpret_cast<const uint32_t*>(bloom + table[2]);
    const uint32_t* chains = buckets + bucket_count;
    const uint32_t hash = gnu_name_hash(name);
    const ElfW(Addr) word = bloom[(hash / kBloomBits) & (table[2] - 1)];
    if ((1 & (word >> (hash % kBloomBits)) & (word >> ((hash >> table[3]) % kBloomBits))) == 0)
      return nullptr;
    size_t index = buckets[hash % bucket_count];
    if (index == 0 || index < first_hashed) return nullptr;
    for (; index < symbols.count; ++index) {
      const uint32_t chain = chains[index - first_hashed];
      if (((chain ^ hash) >> 1) == 0 &&
          eligible_symbol(image, index, name, version, external, retiring))
        return &symbols.entries[index];
      if ((chain & 1) != 0) break;
    }
    return nullptr;
  }
  if (const uint32_t* table = symbols.sysv_hash) {
    const uint32_t* buckets = table + 2;
    const uint32_t* chains = buckets + table[0];
    for (size_t index = buckets[sysv_name_hash(name) % table[0]];
         index != 0 && index < symbols.count; index = chains[index])
      if (eligible_symbol(image, index, name, version, external, retiring))
        return &symbols.entries[index];
  }
  return nullptr;
}

const ElfW(Sym) * symbol_at(const ImageState* image, uint32_t index) {
  return image != nullptr && index < image->symbols.count ? &image->symbols.entries[index]
                                                          : nullptr;
}

}  // namespace yukilinker::internal
