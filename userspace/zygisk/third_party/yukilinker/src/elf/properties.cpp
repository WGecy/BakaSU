// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {
namespace {

constexpr uint32_t kGnuProperty = 0x6474e553;
constexpr uint32_t kFeatureAnd = 0xc0000000;
constexpr uint32_t kBti = 1;
constexpr uint32_t kPac = 2;

uint32_t word(const uint8_t* source) {
  uint32_t value;
  memcpy(&value, source, sizeof(value));
  return value;
}

bool invalid(uint64_t offset) {
  return fail_no_errno(YUKILINKER_C_INVALID_ELF, YUKILINKER_C_PROTECTION, offset);
}

// Properties describe loaded bytes. Require their file and virtual ranges to name the same
// readable PT_LOAD contents; do not trust a second, conflicting copy of the note in the file.
bool loaded_note(const SourceLayout& layout, const ElfW(Phdr) & note) {
  for (size_t i = 0; i < layout.program_header_count; ++i) {
    const auto& load = layout.program_headers[i];
    if (load.p_type != PT_LOAD || !(load.p_flags & PF_R) || note.p_vaddr < load.p_vaddr) continue;
    const auto relative = note.p_vaddr - load.p_vaddr;
    if (elf::range(load.p_filesz, relative, note.p_filesz) &&
        note.p_offset == load.p_offset + relative)
      return true;
  }
  return false;
}

}  // namespace

int bti_protection() {
#if defined(__aarch64__)
  // HWCAP2_BTI: GNU FEATURE_1_BTI advertises compatibility, not a CPU requirement. Match
  // Bionic's capability gate; when enabled, a refused protection change never falls back.
  return (getauxval(AT_HWCAP2) & (1UL << 17)) ? 0x10 /* PROT_BTI */ : 0;
#else
  return 0;
#endif
}

bool inspect_properties(const void* source, size_t file_size, SourceLayout* layout) {
  bool found = false;
  for (size_t i = 0; i < layout->program_header_count; ++i) {
    const auto& ph = layout->program_headers[i];
    const uint64_t header_offset = layout->header->e_phoff + i * sizeof(ph);
    if (ph.p_type == PT_GNU_STACK && (ph.p_flags & PF_X))
      return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION, "PT_GNU_STACK(PF_X)", nullptr,
                  header_offset);
    if (ph.p_type != kGnuProperty) continue;
    constexpr size_t alignment = sizeof(ElfW(Addr));
    if (found || ph.p_filesz < 16 || ph.p_filesz != ph.p_memsz || ph.p_align != alignment ||
        ph.p_offset % alignment || ph.p_vaddr % alignment ||
        !elf::range(file_size, ph.p_offset, ph.p_filesz) || !loaded_note(*layout, ph))
      return invalid(header_offset);
    found = true;
    const auto* bytes = static_cast<const uint8_t*>(source) + ph.p_offset;
    const uint32_t descriptor_size = word(bytes + 4);
    if (word(bytes) != 4 || word(bytes + 8) != 5 || memcmp(bytes + 12, "GNU", 4) != 0 ||
        descriptor_size != ph.p_filesz - 16 || descriptor_size % alignment)
      return invalid(ph.p_offset);
    size_t at = 16;
    uint32_t previous = 0;
    bool first = true;
    while (at < ph.p_filesz) {
      const uint64_t offset = ph.p_offset + at;
      if (ph.p_filesz - at < 8) return invalid(offset);
      const uint32_t type = word(bytes + at);
      const uint32_t size = word(bytes + at + 4);
      // GNU property entries must be sorted and unique. Use subtraction before rounding to
      // keep a malicious pr_datasz from overflowing size_t on ARM32.
      if ((!first && type <= previous) || size > ph.p_filesz - at - 8) return invalid(offset);
      previous = type;
      first = false;
      const uint64_t step = (uint64_t{8} + size + alignment - 1) & ~(uint64_t{alignment} - 1);
      if (step > ph.p_filesz - at) return invalid(offset);
      if (type == kFeatureAnd) {
        if (size != 4) return invalid(offset);
        const uint32_t features = word(bytes + at + 8);
#if defined(__aarch64__)
        // PAC return-address hints need no loader fixups. GCS and future feature bits have no
        // private runtime contract yet, so explicitly refuse them instead of dropping them.
        if (features & ~(kBti | kPac))
          return fail(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION,
                      features & 4 ? "GNU_PROPERTY_AARCH64_FEATURE_1_GCS"
                                   : "GNU_PROPERTY_AARCH64_FEATURE_1_AND",
                      nullptr, offset);
        layout->bti_compatible = (features & kBti) != 0;
#else
        if (features)
          return fail_no_errno(YUKILINKER_C_UNSUPPORTED, YUKILINKER_C_PROTECTION, offset);
#endif
      }
      at += static_cast<size_t>(step);
    }
  }
  return true;
}

}  // namespace yukilinker::internal
