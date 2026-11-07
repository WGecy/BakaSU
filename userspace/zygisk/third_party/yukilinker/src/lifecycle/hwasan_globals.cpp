// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/runtime_model.hpp"

#if __has_feature(hwaddress_sanitizer)
extern "C" void __hwasan_library_loaded(ElfW(Addr), const ElfW(Phdr) *, ElfW(Half));
extern "C" void __hwasan_library_unloaded(ElfW(Addr), const ElfW(Phdr) *, ElfW(Half));
#endif

namespace yukilinker::internal {
namespace {
bool invalid_note(uintptr_t at) {
  return fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_METADATA, at);
}
bool readable(ImageState* image, uintptr_t at, size_t bytes) {
  return runtime_pointer<uint8_t>(image, at, bytes) != nullptr;
}
uint32_t read_word(const uint8_t* p) {
  uint32_t value;
  memcpy(&value, p, 4);
  return value;
}
bool overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn) {
  return an && bn && (a >= b ? a - b < bn : b - a < an);
}
bool metadata_target(ImageState* image, uintptr_t at, size_t bytes, uintptr_t descriptors,
                     size_t descriptor_bytes) {
  if (overlaps(at, bytes, descriptors, descriptor_bytes)) return true;
  const uintptr_t bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  const auto touches = [&](const void* pointer, size_t size) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    return pointer && address >= bias && overlaps(at, bytes, address - bias, size);
  };
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const auto& ph = image->memory.program_headers[i];
    if ((ph.p_type == PT_NOTE || ph.p_type == PT_DYNAMIC || ph.p_type == PT_TLS) &&
        overlaps(at, bytes, ph.p_vaddr, ph.p_memsz))
      return true;
  }
  const auto& symbols = image->symbols;
  if (touches(symbols.entries, symbols.count * sizeof(ElfW(Sym))) ||
      touches(symbols.strings, symbols.string_bytes) ||
      touches(symbols.versions, symbols.count * sizeof(ElfW(Half))) ||
      touches(image->lifecycle.init_array, image->lifecycle.init_count * sizeof(void*)) ||
      touches(image->lifecycle.fini_array, image->lifecycle.fini_count * sizeof(void*)))
    return true;
  if (symbols.sysv_hash) {
    const uint64_t size = (uint64_t{2} + symbols.sysv_hash[0] + symbols.sysv_hash[1]) * 4;
    if (size > SIZE_MAX || touches(symbols.sysv_hash, static_cast<size_t>(size))) return true;
  }
  if (symbols.gnu_hash) {
    if (symbols.gnu_hash[1] > symbols.count) return true;
    const uint64_t size = 16 + uint64_t(symbols.gnu_hash[2]) * sizeof(ElfW(Addr)) +
                          uint64_t(symbols.gnu_hash[0]) * 4 +
                          uint64_t(symbols.count - symbols.gnu_hash[1]) * 4;
    if (size > SIZE_MAX || touches(symbols.gnu_hash, static_cast<size_t>(size))) return true;
  }
  return false;
}
}  // namespace

// compiler-rt's private hook trusts every note and descriptor it traverses.
// Validate its entire input before invoking it, including notes preceding LLVM.
bool validate_hwasan_globals(ImageState* image) {
  auto& memory = image->memory;
  bool found = false;
  const uintptr_t bias = reinterpret_cast<uintptr_t>(memory.bias);
  for (size_t i = 0; i < memory.program_header_count; ++i) {
    const auto& ph = memory.program_headers[i];
    if (ph.p_type != PT_NOTE || !ph.p_memsz) continue;
    if (ph.p_vaddr % 4 || !readable(image, ph.p_vaddr, ph.p_memsz)) return invalid_note(ph.p_vaddr);
    size_t offset = 0;
    while (offset < ph.p_memsz) {
      const uintptr_t at = ph.p_vaddr + offset;
      const size_t left = ph.p_memsz - offset;
      if (left < sizeof(ElfW(Nhdr))) return invalid_note(at);
      const auto* note = memory.bias + at;
      const uint32_t namesz = read_word(note), descsz = read_word(note + 4);
      const uint32_t type = read_word(note + 8);
      const uint64_t name_bytes = (uint64_t(namesz) + 3) & ~uint64_t{3};
      const uint64_t desc_bytes = (uint64_t(descsz) + 3) & ~uint64_t{3};
      const uint64_t total = sizeof(ElfW(Nhdr)) + name_bytes + desc_bytes;
      if (total > left) return invalid_note(at);
      const auto* name = note + sizeof(ElfW(Nhdr));
      // compiler-rt uses strcmp when type == 3, so even a non-LLVM name must terminate.
      if (type == 3 && (!namesz || !memchr(name, 0, namesz))) return invalid_note(at);
      if (type == 3 && namesz >= 5 && memcmp(name, "LLVM", 5) == 0) {
        if (found || descsz != 8) return invalid_note(at);
        found = true;
        const auto* desc = name + name_bytes;
        int32_t first, last;
        memcpy(&first, desc, 4);
        memcpy(&last, desc + 4, 4);
        uintptr_t begin, end;
        if (!add_signed_offset(at, first, &begin) || !add_signed_offset(at, last, &end) ||
            end < begin || begin % 4 || (end - begin) % 8 ||
            (end != begin && !readable(image, begin, end - begin)))
          return invalid_note(at);
        if (end != begin && (memory.span > (uint64_t{1} << 32) || bias > (uint64_t{1} << 48) ||
                             memory.lowest_vaddr > (uint64_t{1} << 48) - bias ||
                             memory.span > (uint64_t{1} << 48) - bias - memory.lowest_vaddr))
          return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA, "HWASan code model");
        for (uintptr_t entry = begin; entry < end; entry += 8) {
          int32_t relative;
          memcpy(&relative, memory.bias + entry, 4);
          const uint32_t info = read_word(memory.bias + entry + 4);
          const size_t bytes = info & 0xffffff;
          uintptr_t target;
          const size_t rounded = (bytes + size_t{15}) & ~size_t{15};
          if (!bytes || !(info >> 24) || !add_signed_offset(entry, relative, &target) ||
              target % 16 || !readable(image, target, rounded))
            return invalid_note(entry);
          // Loader metadata must remain readable through its untagged pointers.
          if (metadata_target(image, target, rounded, begin, end - begin))
            return invalid_note(entry);
          // A deferred write through an untagged loader pointer cannot target
          // an already tagged global. Reject that combination before resolvers.
          for (size_t j = 0; j < image->relocations.deferred_count; ++j)
            if (overlaps(target, rounded, image->relocations.deferred[j].offset,
                         sizeof(ElfW(Addr))))
              return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_RELOCATIONS,
                          "HWASan tagged IFUNC destination");
          // The compiler stores a short global's tag in its final granule byte.
          if (bytes % 16 && memory.bias[target + rounded - 1] != (info >> 24))
            return invalid_note(entry);
        }
        image->lifecycle.hwasan_globals = true;
      }
      offset += static_cast<size_t>(total);
    }
  }
  if (!found) return image->lifecycle.hwasan_globals ? invalid_note(0) : true;
#if __has_feature(hwaddress_sanitizer)
  if (memory.program_header_count > UINT16_MAX)
    return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA, "HWASan program headers");
  return true;
#else
  return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA,
              "HWASan globals require instrumented loader");
#endif
}

bool register_hwasan_globals(ImageState* image) {
#if __has_feature(hwaddress_sanitizer)
  if (image->lifecycle.hwasan_globals && !image->lifecycle.hwasan_registered) {
    // No user callbacks: compiler-rt only walks the validated notes and writes shadow.
    __hwasan_library_loaded(reinterpret_cast<ElfW(Addr)>(image->memory.bias),
                            image->memory.program_headers, image->memory.program_header_count);
    image->lifecycle.hwasan_registered = true;
  }
#else
  (void)image;
#endif
  return true;
}

void unregister_hwasan_globals(ImageState* image) {
#if __has_feature(hwaddress_sanitizer)
  if (image->lifecycle.hwasan_registered) {
    __hwasan_library_unloaded(reinterpret_cast<ElfW(Addr)>(image->memory.bias),
                              image->memory.program_headers, image->memory.program_header_count);
    image->lifecycle.hwasan_registered = false;
  }
#else
  (void)image;
#endif
}
}  // namespace yukilinker::internal
