// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace yukilinker::elf {

inline bool power_of_two(uint64_t value) { return value != 0 && (value & (value - 1)) == 0; }

inline bool range(uint64_t limit, uint64_t offset, uint64_t size) {
  return offset <= limit && size <= limit - offset;
}

template <typename Phdr>
bool segment_contains(const Phdr* headers, size_t count, uint64_t address, uint64_t bytes,
                      uint32_t flags) {
  for (size_t i = 0; i < count; ++i) {
    const auto& ph = headers[i];
    if (ph.p_type == PT_LOAD && (ph.p_flags & flags) == flags && address >= ph.p_vaddr &&
        range(ph.p_memsz, address - ph.p_vaddr, bytes))
      return true;
  }
  return false;
}

template <typename Ehdr, typename Phdr>
struct Layout {
  const Ehdr* header = nullptr;
  const Phdr* headers = nullptr;
  size_t count = 0;
  uint64_t low = 0;
  uint64_t high = 0;
  size_t dynamic_index = SIZE_MAX;
  // Set when inspect refused only what the specification allows and this loader does not do: two
  // PT_LOADs that share a page without sharing a byte, or a p_align that is no power of two. A
  // malformed header refuses with this clear.
  bool unsupported = false;
};

template <typename Ehdr, typename Phdr>
bool inspect(const void* source, size_t bytes, size_t page_size, uint16_t machine,
             Layout<Ehdr, Phdr>* out) {
  if (!source || !out || bytes < sizeof(Ehdr) || !power_of_two(page_size) || page_size < 4096 ||
      reinterpret_cast<uintptr_t>(source) % alignof(Ehdr) != 0)
    return false;
  out->unsupported = false;
  const auto* header = static_cast<const Ehdr*>(source);
  if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
      header->e_ident[EI_CLASS] != (sizeof(Ehdr) == sizeof(Elf64_Ehdr) ? ELFCLASS64 : ELFCLASS32) ||
      header->e_ident[EI_DATA] != ELFDATA2LSB || header->e_ident[EI_VERSION] != EV_CURRENT ||
      header->e_version != EV_CURRENT || header->e_type != ET_DYN || header->e_machine != machine ||
      header->e_ehsize != sizeof(Ehdr) || header->e_phentsize != sizeof(Phdr) || !header->e_phnum ||
      header->e_phnum > 4096 || header->e_phoff % alignof(Phdr) != 0 ||
      !range(bytes, header->e_phoff, uint64_t{header->e_phnum} * sizeof(Phdr)))
    return false;
  const auto* headers =
      reinterpret_cast<const Phdr*>(static_cast<const uint8_t*>(source) + header->e_phoff);
  uint64_t low = UINT64_MAX;
  uint64_t high = 0;
  size_t dynamic_index = SIZE_MAX;
  // Legal layouts this loader does not map, decided once every check for a malformed one passed.
  bool unsupported = false;
  constexpr uint64_t address_max = std::numeric_limits<decltype(Phdr::p_vaddr)>::max();
  for (size_t i = 0; i < header->e_phnum; ++i) {
    const auto& ph = headers[i];
    if (ph.p_type == PT_DYNAMIC) {
      if (dynamic_index != SIZE_MAX || !ph.p_memsz) return false;
      dynamic_index = i;
    }
    if (ph.p_type != PT_LOAD) continue;
    // Bionic refuses a segment aligned below a page of 16 KiB or more (without its app compat
    // mode), however the segments happen to fall into page frames.
    if (page_size >= 16384 && ph.p_align > 1 && ph.p_align < page_size) return false;
    if (ph.p_filesz > ph.p_memsz || !range(bytes, ph.p_offset, ph.p_filesz) ||
        !range(address_max, ph.p_vaddr, ph.p_memsz) ||
        ph.p_vaddr % page_size != ph.p_offset % page_size ||
        (ph.p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
      return false;
    // The ELF specification asks for a power of two; Bionic only warns. One that is, and still
    // puts the address and the offset out of step, is malformed.
    if (ph.p_align > 1) {
      if (!power_of_two(ph.p_align))
        unsupported = true;
      else if (ph.p_vaddr % ph.p_align != ph.p_offset % ph.p_align)
        return false;
    }
    if (!ph.p_memsz) continue;
    const uint64_t begin = ph.p_vaddr & ~(uint64_t{page_size} - 1);
    const uint64_t end = ph.p_vaddr + ph.p_memsz;
    if (end > address_max - (page_size - 1)) return false;
    const uint64_t rounded_end = (end + page_size - 1) & ~(uint64_t{page_size} - 1);
    for (size_t j = 0; j < i; ++j) {
      const auto& other = headers[j];
      if (other.p_type != PT_LOAD || !other.p_memsz) continue;
      // Two segments that write one byte are malformed; two that only share a page are legal
      // (Bionic maps the page with both protections), and this loader does not merge them.
      if (ph.p_vaddr < other.p_vaddr + other.p_memsz && other.p_vaddr < end) return false;
      const uint64_t other_begin = other.p_vaddr & ~(uint64_t{page_size} - 1);
      const uint64_t other_end =
          (other.p_vaddr + other.p_memsz + page_size - 1) & ~(uint64_t{page_size} - 1);
      if (begin < other_end && other_begin < rounded_end) unsupported = true;
    }
    low = begin < low ? begin : low;
    high = rounded_end > high ? rounded_end : high;
  }
  if (high <= low || dynamic_index == SIZE_MAX || high - low > SIZE_MAX) return false;
  const auto& dynamic = headers[dynamic_index];
  if (!segment_contains(headers, header->e_phnum, dynamic.p_vaddr, dynamic.p_memsz, PF_R))
    return false;
  for (size_t i = 0; i < header->e_phnum; ++i) {
    const auto& ph = headers[i];
    if (ph.p_type == PT_GNU_RELRO && ph.p_memsz &&
        !segment_contains(headers, header->e_phnum, ph.p_vaddr, ph.p_memsz, PF_R | PF_W))
      return false;
  }
  if (unsupported) {
    out->unsupported = true;
    return false;
  }
  *out = {header, headers, header->e_phnum, low, high, dynamic_index};
  return true;
}

class SlebReader {
 public:
  SlebReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool read(int64_t* result) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 10; ++i) {
      if (!data_ || cursor_ == size_) return false;
      const unsigned byte = data_[cursor_++];
      const unsigned payload = byte & 127;
      const unsigned shift = i * 7;
      if (i == 9) {
        if ((byte & 128) || (payload != 0 && payload != 127)) return false;
        value |= uint64_t{payload & 1} << 63;
      } else {
        value |= uint64_t{payload} << shift;
      }
      if (!(byte & 128)) {
        if (shift + 7 < 64 && (byte & 64)) value |= UINT64_MAX << (shift + 7);
        *result = value >> 63 ? -static_cast<int64_t>(~value) - 1 : static_cast<int64_t>(value);
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] bool finished() const { return cursor_ == size_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t cursor_ = 0;
};

inline bool add_signed(int64_t a, int64_t b, int64_t* result) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
  *result = a + b;
  return true;
}

template <bool Rela, typename Callback>
bool packed(const uint8_t* data, size_t size, unsigned bits, Callback callback) {
  if (!data || size < 4 || memcmp(data, "APS2", 4) != 0 || (bits != 32 && bits != 64)) return false;
  SlebReader reader(data + 4, size - 4);
  int64_t count, offset;
  if (!reader.read(&count) || !reader.read(&offset) || count < 0 ||
      count > int64_t{16} * 1024 * 1024 || offset < 0)
    return false;
  int64_t addend = 0;
  while (count != 0) {
    int64_t group, flags, delta = 0, info = 0, value;
    if (!reader.read(&group) || !reader.read(&flags) || group <= 0 || group > count || flags < 0 ||
        (flags & ~15) || (!Rela && (flags & 8)) || ((flags & 2) && !reader.read(&delta)) ||
        ((flags & 1) && !reader.read(&info)))
      return false;
    if constexpr (Rela) {
      if ((flags & 12) == 12) {
        if (!reader.read(&value) || !add_signed(addend, value, &addend)) return false;
      } else if ((flags & 12) != 8) {
        addend = 0;
      }
    }
    for (int64_t i = 0; i < group; ++i) {
      if ((!(flags & 2) && !reader.read(&delta)) || !add_signed(offset, delta, &offset) ||
          offset < 0 || (!(flags & 1) && !reader.read(&info)) || info < 0)
        return false;
      if constexpr (Rela) {
        if ((flags & 12) == 8 && (!reader.read(&value) || !add_signed(addend, value, &addend)))
          return false;
      }
      if (bits == 32 && (uint64_t(offset) > UINT32_MAX || uint64_t(info) > UINT32_MAX ||
                         addend < INT32_MIN || addend > INT32_MAX))
        return false;
      if (!callback(uint64_t(offset), uint64_t(info), addend)) return false;
    }
    count -= group;
  }
  return reader.finished();
}

}  // namespace yukilinker::elf
