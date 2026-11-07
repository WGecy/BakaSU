// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <link.h>

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "elf_model.hpp"
#include "metadata_arena.hpp"

namespace yukilinker::internal {

struct ImageState;

struct LoadPlacement {
  void* address = nullptr;
  size_t size = 0;
  bool hint = false;
};

struct RelroOptions {
  int fd = -1;
  bool write = false;
  bool use = false;
};

struct AddressSpace {
  alignas(max_align_t) unsigned char metadata_storage[kInlineMetadataSize]{};
  MetadataPage* metadata = nullptr;
  void* reservation = nullptr;
  size_t span = 0;
  uintptr_t lowest_vaddr = 0;
  uint8_t* bias = nullptr;
  ElfW(Phdr) * program_headers = nullptr;
  size_t program_header_count = 0;
  bool file_backed = false;
  bool bti_guarded = false;
  // The range belongs to the caller (ANDROID_DLEXT_RESERVED_ADDRESS*): retiring the image maps it
  // back to PROT_NONE instead of unmapping it, as the system linker does.
  bool caller_reserved = false;
  // Set once protect_relro has made every PT_GNU_RELRO range read-only.
  bool relro_protected = false;
  uint64_t source_offset = 0;
  // Where the program header table lies in the file, so a failure can name the header it read.
  uint64_t program_header_offset = 0;
  uint32_t program_header_size = 0;
  uint32_t relro_flags = 0;
  uint64_t relro_written_bytes = 0;
  uint64_t relro_shared_bytes = 0;
};

static inline bool address_space_contains(const AddressSpace& memory, const void* pointer,
                                          size_t bytes) {
  if (memory.reservation == nullptr || pointer == nullptr) return false;
  uintptr_t begin = reinterpret_cast<uintptr_t>(memory.reservation);
  uintptr_t point = reinterpret_cast<uintptr_t>(pointer);
  if (point < begin || point - begin > memory.span) return false;
  return bytes <= memory.span - (point - begin);
}

bool inspect_source(const void* source, size_t file_size, SourceLayout* layout);
bool inspect_properties(const void* source, size_t file_size, SourceLayout* layout);
// PROT_BTI when the ARM64 kernel advertises support; zero on other hardware.
int bti_protection();
// The file offset of program header index of memory's image, or 0 when it would overflow.
uint64_t program_header_file_offset(const AddressSpace& memory, size_t index);
bool create_address_space(int fd, const uint8_t* source, size_t file_size,
                          const SourceLayout& layout, bool file_backed, uint64_t source_offset,
                          AddressSpace* memory, const LoadPlacement* placement);
// The page-rounded range of one PT_GNU_RELRO header, inside the image's reservation.
bool relro_range(const ImageState* image, const ElfW(Phdr) & header, uintptr_t* start,
                 uintptr_t* end);
bool protect_relro(ImageState* image);
bool share_relro(ImageState* image, const RelroOptions& options);

}  // namespace yukilinker::internal
