// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/enumeration.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

uint32_t copy_program_headers(const ImageState* image, void* output, size_t bytes,
                              size_t* required) {
  if (image->memory.program_header_count > SIZE_MAX / sizeof(ElfW(Phdr)))
    return YUKILINKER_C_NO_MEMORY;
  const size_t needed = image->memory.program_header_count * sizeof(ElfW(Phdr));
  *required = needed;
  if (!output || bytes < needed) return YUKILINKER_C_BUFFER_TOO_SMALL;
  memcpy(output, image->memory.program_headers, needed);
  return YUKILINKER_C_OK;
}

}  // namespace yukilinker::internal
