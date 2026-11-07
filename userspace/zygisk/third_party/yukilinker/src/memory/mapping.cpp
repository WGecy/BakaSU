// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

// A segment's protection, never writable and executable at once: elf::inspect refuses a PT_LOAD
// with both PF_W and PF_X, and should one reach here anyway it maps without PROT_EXEC.
int protection_for(uint32_t flags) {
  int protection = 0;
  if ((flags & PF_R) != 0) protection |= PROT_READ;
  if ((flags & PF_W) != 0)
    protection |= PROT_WRITE;
  else if ((flags & PF_X) != 0)
    protection |= PROT_EXEC;
  return protection;
}

namespace {

// A failed mprotect, while its errno still holds: EACCES and EPERM are policy (a noexec mount,
// SELinux execmem or execmod), ENOMEM a mapping limit; neither says the image is unsupported.
bool fail_protection() { return fail_errno(YUKILINKER_C_PROTECTION, errno); }

}  // namespace

uint64_t program_header_file_offset(const AddressSpace& memory, size_t index) {
  uint64_t step = 0;
  uint64_t offset = 0;
  if (__builtin_mul_overflow(static_cast<uint64_t>(index), memory.program_header_size, &step) ||
      __builtin_add_overflow(memory.program_header_offset, step, &offset))
    return 0;
  return offset;
}

bool file_region_valid(size_t file_size, ElfW(Off) offset, ElfW(Xword) bytes) {
  if (offset > file_size) return false;
  return bytes <= file_size - static_cast<size_t>(offset);
}

bool inspect_source(const void* source, size_t file_size, SourceLayout* layout) {
  elf::Layout<ElfW(Ehdr), ElfW(Phdr)> parsed;
  if (!elf::inspect(source, file_size, loader_page_size(), sizeof(void*) == 8 ? EM_AARCH64 : EM_ARM,
                    &parsed)) {
    elf::Layout<ElfW(Ehdr), ElfW(Phdr)> small;
    if (loader_page_size() != 4096 &&
        elf::inspect(source, file_size, 4096, sizeof(void*) == 8 ? EM_AARCH64 : EM_ARM, &small)) {
      // A valid image whose PT_LOAD alignment is below the latched page; Bionic refuses it too
      // (without its app compat mode, which this runtime does not have).
      fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
      host_log_error(
          "yukilinker: PT_LOAD aligned below the %zu-byte page size; relink the library with "
          "-Wl,-z,max-page-size=%zu",
          loader_page_size(), loader_page_size());
    } else if (parsed.unsupported || small.unsupported) {
      // Legal, and loaded by the system linker: the host may fall back to it.
      fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_METADATA);
      host_log_error(
          "yukilinker: PT_LOAD segments that share a page, or a p_align that is no power of two, "
          "are not supported");
    } else {
      host_log_error("yukilinker: invalid shared object for this ABI");
    }
    return false;
  }
  *layout = {parsed.header,
             parsed.headers,
             parsed.count,
             static_cast<uintptr_t>(parsed.low),
             static_cast<uintptr_t>(parsed.high),
             parsed.dynamic_index};
  return inspect_properties(source, file_size, layout);
}

bool map_one_segment(int fd, const uint8_t* source, size_t file_size, AddressSpace* memory,
                     const ElfW(Phdr) & ph, size_t index) {
  uintptr_t segment_page = page_floor(ph.p_vaddr);
  size_t page_prefix = static_cast<size_t>(ph.p_vaddr - segment_page);
  // The range checks below cannot fail after elf::inspect; each names the program header it read
  // (its file offset in yukilinker_c_error_detail.offset) and carries no errno.
  const uint64_t header_offset = program_header_file_offset(*memory, index);
  uintptr_t memory_end;
  if (!page_ceil(page_prefix + ph.p_memsz, &memory_end))
    return fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_PROTECTION, header_offset);
  size_t mapped_bytes = static_cast<size_t>(memory_end);
  uintptr_t destination_value = reinterpret_cast<uintptr_t>(memory->bias) + segment_page;
  void* destination = reinterpret_cast<void*>(destination_value);

  // Relocations write only into PF_W segments and DT_TEXTREL is refused, so any other segment
  // takes its final protection as soon as its bytes are in place: no code page is writable while
  // the image relocates.
  const int final_protection =
      protection_for(ph.p_flags) | ((memory->bti_guarded && (ph.p_flags & PF_X)) ? 0x10 : 0);
  if (!memory->file_backed) {
    if (mmap(destination, mapped_bytes, PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
      return fail_errno(YUKILINKER_C_PROTECTION, errno);
    if (ph.p_filesz != 0) {
      if (!file_region_valid(file_size, ph.p_offset, ph.p_filesz))
        return fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_PROTECTION, header_offset);
      memcpy(reinterpret_cast<uint8_t*>(destination) + page_prefix, source + ph.p_offset,
             ph.p_filesz);
    }
    // Any other protection is set here, as the file path maps it: no later pass restores it.
    if (final_protection == (PROT_READ | PROT_WRITE)) return true;
    // ic ivau (and the ARM cacheflush syscall) needs accessible memory. Publish
    // execute-only/BTI protection only after maintaining the writable, NX copy.
    if ((final_protection & PROT_EXEC) != 0)
      __builtin___clear_cache(static_cast<char*>(destination),
                              static_cast<char*>(destination) + mapped_bytes);
    if (mprotect(destination, mapped_bytes, final_protection) != 0) return fail_protection();
    return true;
  }

  // As Bionic, the file is mapped to the end of the kernel page holding the segment's last file
  // byte, and anonymous zero pages follow. A page size latched above the kernel's must not round
  // it further: a kernel page wholly past the end of the file faults when touched.
  const size_t data_end = page_prefix + static_cast<size_t>(ph.p_filesz);
  const size_t kernel_page = system_page_size();
  const size_t file_mapping_bytes = (data_end + kernel_page - 1) & ~(kernel_page - 1);
  const uint64_t aligned_file_offset = ph.p_offset - page_prefix;
  if (aligned_file_offset > INT64_MAX - memory->source_offset)
    return fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_PROTECTION, header_offset);

  // A writable segment loses the rest of its last file page, whatever the file holds there, as in
  // Bionic. Any other clears only its bss, the one write a read-only segment gets; Bionic clears
  // nothing there. That write is made without PROT_EXEC, so no page is writable and executable.
  size_t zero_bytes = file_mapping_bytes - data_end;
  if ((ph.p_flags & PF_W) == 0)
    zero_bytes = smaller(zero_bytes, static_cast<size_t>(ph.p_memsz - ph.p_filesz));
  const int file_protection = zero_bytes != 0 ? PROT_READ | PROT_WRITE : final_protection;
  if (file_mapping_bytes != 0 &&
      mmap64(destination, file_mapping_bytes, file_protection, MAP_FIXED | MAP_PRIVATE, fd,
             static_cast<off64_t>(memory->source_offset + aligned_file_offset)) == MAP_FAILED)
    return fail_errno(YUKILINKER_C_PROTECTION, errno);
  if (zero_bytes != 0) {
    memset(reinterpret_cast<uint8_t*>(destination) + data_end, 0, zero_bytes);
    if (file_protection != final_protection) {
      if ((final_protection & PROT_EXEC) != 0)
        __builtin___clear_cache(static_cast<char*>(destination),
                                static_cast<char*>(destination) + file_mapping_bytes);
      if (mprotect(destination, file_mapping_bytes, final_protection) != 0)
        return fail_protection();
    }
  }

  if (mapped_bytes > file_mapping_bytes) {
    void* bss = reinterpret_cast<uint8_t*>(destination) + file_mapping_bytes;
    if (mmap(bss, mapped_bytes - file_mapping_bytes, final_protection,
             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
      return fail_errno(YUKILINKER_C_PROTECTION, errno);
  }
  return true;
}

bool validate_reserved_mapping(void* address, size_t size) {
  const auto start = reinterpret_cast<uintptr_t>(address);
  if (!start || !size || start % loader_page_size() || size % loader_page_size() ||
      size > UINTPTR_MAX - start)
    return false;
  FILE* maps = fopen("/proc/self/maps", "re");
  if (!maps) return false;
  const uintptr_t end = start + size;
  uintptr_t cursor = start;
  char line[512];
  while (cursor < end && fgets(line, sizeof(line), maps)) {
    unsigned long long low = 0, high = 0;
    char permissions[5]{};
    if (sscanf(line, "%llx-%llx %4s", &low, &high, permissions) != 3 || high <= cursor) continue;
    if (low > cursor || strcmp(permissions, "---p") != 0) break;
    cursor = high >= end ? end : static_cast<uintptr_t>(high);
  }
  (void)fclose(maps);
  return cursor == end;
}

bool create_address_space(int fd, const uint8_t* source, size_t file_size,
                          const SourceLayout& layout, bool file_backed, uint64_t source_offset,
                          AddressSpace* memory, const LoadPlacement* placement) {
  // The page size in force for this lock hold: inspect_source read the same value. load_image
  // commits it once the image registers, so a load that fails here or later fixes nothing.
  size_t alignment = loader_page_size();
  size_t span = layout.highest_vaddr - layout.lowest_vaddr;
  for (size_t i = 0; i < layout.program_header_count; ++i)
    if (layout.program_headers[i].p_type == PT_LOAD)
      alignment = std::max(alignment, static_cast<size_t>(layout.program_headers[i].p_align));
  void* reservation = nullptr;
  if (placement && span <= placement->size) {
    const auto start = reinterpret_cast<uintptr_t>(placement->address);
    if (((start - layout.lowest_vaddr) & (alignment - 1)) != 0 ||
        !validate_reserved_mapping(placement->address, placement->size))
      return fail_no_errno(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION);
    reservation = placement->address;
    memory->caller_reserved = true;
  } else {
    // The caller's range is too small for the image and no other place is allowed.
    if (placement && !placement->hint)
      return fail_no_errno(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_PROTECTION);
    // mmap only rounds to the kernel page, while the image is placed so that
    // lowest_vaddr lands on an alignment boundary. When the requested alignment
    // exceeds the kernel page, the two differ and a full alignment of slack is
    // needed for the prefix trim below.
    const size_t kernel_page = system_page_size();
    const size_t slack = alignment > kernel_page ? alignment - kernel_page : 0;
    size_t reserved_span;
    // An LP32 span near the address-space size: no address range can hold it.
    if (add_overflow(span, slack, &reserved_span))
      return fail_no_errno(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_PROTECTION);
    reservation = mmap(nullptr, reserved_span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) return fail_errno(YUKILINKER_C_PROTECTION, errno);
    memory->reservation = reservation;
    memory->span = reserved_span;
    const auto start = reinterpret_cast<uintptr_t>(reservation);
    const size_t prefix = (layout.lowest_vaddr - start) & (alignment - 1);
    const size_t suffix = reserved_span - prefix - span;
    if ((prefix && munmap(reservation, prefix) != 0) ||
        (suffix && munmap(reinterpret_cast<void*>(start + prefix + span), suffix) != 0))
      return fail_errno(YUKILINKER_C_PROTECTION, errno);
    reservation = reinterpret_cast<void*>(start + prefix);
  }
  memory->reservation = reservation;
  memory->span = span;
  memory->lowest_vaddr = layout.lowest_vaddr;
  memory->bias =
      reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(reservation) - layout.lowest_vaddr);
  memory->program_header_count = layout.program_header_count;
  memory->file_backed = file_backed;
  memory->bti_guarded = layout.bti_compatible && bti_protection() != 0;
  memory->source_offset = source_offset;
  memory->program_header_offset = layout.header->e_phoff;
  memory->program_header_size = layout.header->e_phentsize;

  size_t phdr_bytes;
  if (multiply_overflow(layout.program_header_count, sizeof(ElfW(Phdr)), &phdr_bytes))
    return fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_PROTECTION,
                         memory->program_header_offset);
  // metadata_allocate's own refusals (an overflow) leave errno alone; its mmap sets it.
  errno = 0;
  memory->program_headers = static_cast<ElfW(Phdr)*>(
      metadata_allocate(phdr_bytes, alignof(ElfW(Phdr)), &memory->metadata));
  if (memory->program_headers == nullptr)
    return errno != 0 ? fail_errno(YUKILINKER_C_PROTECTION, errno)
                      : fail_no_errno(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_PROTECTION);
  memcpy(memory->program_headers, layout.program_headers, phdr_bytes);

  for (size_t i = 0; i < layout.program_header_count; ++i) {
    const ElfW(Phdr) & ph = layout.program_headers[i];
    if (ph.p_type == PT_LOAD && ph.p_memsz != 0 &&
        !map_one_segment(fd, source, file_size, memory, ph, i)) {
      return false;
    }
  }
  return true;
}

bool relro_range(const ImageState* image, const ElfW(Phdr) & header, uintptr_t* start,
                 uintptr_t* end) {
  const uintptr_t bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  if (header.p_vaddr > UINTPTR_MAX - bias) return false;
  const uintptr_t unrounded_start = bias + header.p_vaddr;
  if (header.p_memsz > UINTPTR_MAX - unrounded_start) return false;
  *start = page_floor(unrounded_start);
  return page_ceil(unrounded_start + header.p_memsz, end) && *end > *start &&
         address_space_contains(image->memory, reinterpret_cast<void*>(*start), *end - *start);
}

bool protect_relro(ImageState* image) {
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const ElfW(Phdr) & ph = image->memory.program_headers[i];
    if (ph.p_type != PT_GNU_RELRO || ph.p_memsz == 0) continue;
    uintptr_t start;
    uintptr_t end;
    if (!relro_range(image, ph, &start, &end))
      return fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_PROTECTION,
                           program_header_file_offset(image->memory, i));
    if (mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ) != 0)
      return fail_protection();
  }
  image->memory.relro_protected = true;
  return true;
}

bool share_relro(ImageState* image, const RelroOptions& options) {
  image->memory.relro_flags =
      (options.write ? YUKILINKER_C_MAPPING_WRITE_RELRO : 0) | YUKILINKER_C_MAPPING_USE_RELRO;
  const size_t page_size = loader_page_size();
  auto* copy = options.use && !options.write ? static_cast<uint8_t*>(malloc(page_size)) : nullptr;
  if (options.use && !options.write && !copy) return fail_errno(YUKILINKER_C_PROTECTION, ENOMEM);
  off64_t file_offset = 0;
  bool success = true;
  for (size_t i = 0; i < image->memory.program_header_count && success; ++i) {
    const ElfW(Phdr) & ph = image->memory.program_headers[i];
    if (ph.p_type != PT_GNU_RELRO || ph.p_memsz == 0) continue;
    uintptr_t start;
    uintptr_t end;
    if (!relro_range(image, ph, &start, &end)) {
      success = fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_PROTECTION,
                              program_header_file_offset(image->memory, i));
      break;
    }
    for (uintptr_t page = start; page < end; page += page_size) {
      // The RELRO file would pass the largest offset a file can have.
      if (file_offset > INT64_MAX - static_cast<int64_t>(page_size)) {
        success = fail_no_errno(YUKILINKER_C_IO_ERROR, YUKILINKER_C_PROTECTION);
        break;
      }
      auto* address = reinterpret_cast<uint8_t*>(page);
      if (options.write) {
        if (g_operation_error) g_operation_error->execution_started = 1;
        size_t written = 0;
        while (written < page_size) {
          const ssize_t result = pwrite64(options.fd, address + written, page_size - written,
                                          file_offset + static_cast<off64_t>(written));
          if (result < 0 && errno == EINTR) continue;
          // A write of nothing is no system error: the file took no more bytes.
          if (result < 0) {
            success = fail_errno(YUKILINKER_C_PROTECTION, errno);
            break;
          }
          if (result == 0) {
            success = fail_no_errno(YUKILINKER_C_IO_ERROR, YUKILINKER_C_PROTECTION);
            break;
          }
          written += static_cast<size_t>(result);
          image->memory.relro_written_bytes += static_cast<size_t>(result);
        }
      } else {
        size_t read_bytes = 0;
        while (read_bytes < page_size) {
          const ssize_t result = pread64(options.fd, copy + read_bytes, page_size - read_bytes,
                                         file_offset + static_cast<off64_t>(read_bytes));
          if (result < 0 && errno == EINTR) continue;
          if (result < 0) {
            success = fail_errno(YUKILINKER_C_PROTECTION, errno);
            break;
          }
          if (result == 0) break;
          read_bytes += static_cast<size_t>(result);
        }
        if (success && read_bytes < page_size) {
          file_offset += static_cast<off64_t>(page_size);
          continue;
        }
        if (success && memcmp(address, copy, page_size) != 0) {
          file_offset += static_cast<off64_t>(page_size);
          continue;
        }
      }
      if (success && mmap64(address, page_size, PROT_READ, MAP_PRIVATE | MAP_FIXED, options.fd,
                            file_offset) == MAP_FAILED)
        success = fail_errno(YUKILINKER_C_PROTECTION, errno);
      if (!success) break;
      image->memory.relro_shared_bytes += page_size;
      file_offset += static_cast<off64_t>(page_size);
    }
  }
  free(copy);
  return success;
}

}  // namespace yukilinker::internal
