// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

// Enumeration views. image_views.cpp backs the public image queries and is shared with the
// bootstrap loader; program_headers.cpp and phdr_bridge.cpp are built into the runtime only, and
// caller_image is defined in loader_api.cpp. Every entry expects the caller to hold an
// OperationGuard.

#include <stddef.h>
#include <stdint.h>

#include "../../include/yukilinker.h"
#include "config.hpp"
#include "loader_types.hpp"

struct dl_phdr_info;

namespace yukilinker::internal {

struct ImageState;

int api_query_address(const void* address, yukilinker_c_image_info* info);
int api_iterate_images(void* context, yukilinker_c_image_visitor visitor, void* user);

// The registered image whose loaded segments hold an address, in any phase, or null; callers
// that need a usable image check accessible_image themselves.
ImageState* caller_image(void* address);

#if YUKILINKER_FULL
// Copies an image's program headers. required must not be null; *required is set unless the byte
// count overflows (YUKILINKER_C_NO_MEMORY), and a missing or short output yields
// YUKILINKER_C_BUFFER_TOO_SMALL.
uint32_t copy_program_headers(const ImageState* image, void* output, size_t bytes,
                              size_t* required);

using PrivateDlIterateCallback = int (*)(struct dl_phdr_info*, size_t, void*);
// dl_iterate_phdr as private images see it: every system image first, then the private ones the
// system did not report.
int module_dl_iterate_phdr(PrivateDlIterateCallback callback, void* user);
// The caller is resolved and kept under the operation lock by the entry point.
int module_dl_iterate_phdr_for(ImageState* caller, PrivateDlIterateCallback callback, void* user);
#if defined(__arm__)
// dl_unwind_find_exidx as private images see it: a private pc yields its image's PT_ARM_EXIDX.
uintptr_t module_dl_unwind_find_exidx(uintptr_t pc, int* pcount);
#endif
#endif  // #if YUKILINKER_FULL

}  // namespace yukilinker::internal
