// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once
#include "../../include/yukilinker.h"
namespace yukilinker::internal {
typedef struct LoadOptions {
  uint32_t size;
  uint32_t flags;
  uint32_t file_backed;
  // Reserved for host cores containing their own runtime. A declaration, not a permission: the
  // image and the dependencies this open loads bind their imports to the system instead of the
  // runtime's interposed entry points. A later open of the file in the same context reuses it.
  uint32_t trusted_runtime;
  // The image's name, which dl_iterate_phdr, dladdr and yukilinker_c_image_record report. A path
  // open names the file it opened. For open_fd without one, the image's soname stands in, or
  // "[yukilinker:<generation>]" when it has none; neither is a directory to search.
  const char* origin;
  void* context;
  uint32_t force_load;
} LoadOptions;

typedef struct LookupValue {
  void* address;
  uint32_t found;
  yukilinker_c_error_detail error;
} LookupValue;

typedef struct LoaderStats {
  uint64_t images;
  uint64_t mapped_bytes;
  uint64_t arena_bytes;
  uint64_t peak_arena_bytes;
  uint64_t tls_blocks;
  uint64_t thread_destructors;
} LoaderStats;

enum CloseStatus { YUKILINKER_C_CLOSED, YUKILINKER_C_RETAINED, YUKILINKER_C_CLOSE_ERROR };
}  // namespace yukilinker::internal
