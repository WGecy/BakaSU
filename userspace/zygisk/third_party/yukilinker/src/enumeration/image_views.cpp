// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/enumeration.hpp"
#include "../internal/runtime_model.hpp"

namespace yukilinker::internal {

namespace {

yukilinker_c_image_info image_info(const ImageState* image) {
  return {reinterpret_cast<uintptr_t>(image->memory.bias),
          reinterpret_cast<uintptr_t>(image->memory.reservation), image->memory.span, image->origin,
          image->generation};
}

}  // namespace

int api_query_address(const void* address, yukilinker_c_image_info* info) {
  OperationGuard operation;
  for (auto* image = g_first_image; image; image = image->next) {
    if (!accessible_image(image)) continue;
    const auto point = reinterpret_cast<uintptr_t>(address);
    const auto bias = reinterpret_cast<uintptr_t>(image->memory.bias);
    if (point >= bias &&
        elf::segment_contains(image->memory.program_headers, image->memory.program_header_count,
                              point - bias, 1, 0)) {
      if (info) *info = image_info(image);
      return 1;
    }
  }
  return 0;
}

int api_iterate_images(void* context, yukilinker_c_image_visitor visitor, void* user) {
  OperationGuard operation;
  if (!visitor) return 0;
  Context* selected = context ? context_from_opaque(context) : nullptr;
  if (context && !selected) return -1;
#if YUKILINKER_FULL
  // Past a shutdown's boundary its pins would keep what the shutdown retires.
  if (shutdown_begun()) return -1;
#endif
  size_t count = 0;
  for (auto* image = g_first_image; image; image = image->next) {
    if (!accessible_image(image) || (selected && image->context != selected)) continue;
    ++count;
  }
  if (count > kMaxSnapshotBytes / sizeof(ImageState*)) return -1;
  // The snapshot and its array share one block: see SnapshotGuard.
  void* block = calloc(1, sizeof(SnapshotGuard) + (count ? count : 1) * sizeof(ImageState*));
  if (!block) return -1;
  auto** images = reinterpret_cast<ImageState**>(static_cast<char*>(block) + sizeof(SnapshotGuard));
  size_t index = 0;
  for (auto* image = g_first_image; image; image = image->next)
    if (accessible_image(image) && (!selected || image->context == selected))
      images[index++] = image;
  int result = 0;
  // Keep the snapshot alive without exposing the pin as a caller-owned
  // reference that a reentrant visitor can consume through close().
  auto* snapshot = new (block) SnapshotGuard(images, count);
  for (size_t i = 0; i < count && !result; ++i) {
    auto info = image_info(images[i]);
    CallbackGuard callback_scope(images[i]);
    result = call_module(visitor, &info, user);
  }
  snapshot->~SnapshotGuard();
  free(block);
  // Unconditional: a close made inside this iteration, here or on another thread, marks the
  // outermost snapshot, which may be this one, and leaves the collection to it (outermost_of).
  collect_images();
  return result;
}

}  // namespace yukilinker::internal
