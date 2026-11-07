// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "checked_math.hpp"
#include "config.hpp"
#include "operation_error.hpp"

namespace yukilinker::internal {

constexpr size_t kMetadataPageSize = size_t{64} * 1024;
constexpr size_t kInlineMetadataSize = 2048;

struct MetadataPage {
  MetadataPage* next;
  size_t mapped_size;
  size_t cursor;
  enum class Storage { Mapping, Image } storage;
};

// Only pages a failed munmap left behind, kept for shutdown; every image has its own arena.
extern MetadataPage* g_metadata_pages;
extern size_t g_metadata_bytes;
extern size_t g_peak_metadata_bytes;

#if YUKILINKER_FULL
extern pthread_mutex_t g_metadata_mutex;
#endif  // #if YUKILINKER_FULL

static inline void metadata_lock() {
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_metadata_mutex);
#endif  // #if YUKILINKER_FULL
}

static inline void metadata_unlock() {
#if YUKILINKER_FULL
  pthread_mutex_unlock(&g_metadata_mutex);
#endif  // #if YUKILINKER_FULL
}

// Image-owned storage stays alive through release_metadata. Only mmap-owned pages may enter
// the failed-unmap list. Both kinds contribute their full capacity to the existing statistics.
static inline MetadataPage* initialize_inline_metadata(void* storage) {
  auto* page = new (storage)
      MetadataPage{nullptr, kInlineMetadataSize,
                   (sizeof(MetadataPage) + alignof(max_align_t) - 1) & ~(alignof(max_align_t) - 1),
                   MetadataPage::Storage::Image};
  metadata_lock();
  g_metadata_bytes += kInlineMetadataSize;
  g_peak_metadata_bytes = std::max(g_peak_metadata_bytes, g_metadata_bytes);
  metadata_unlock();
  return page;
}

static inline size_t metadata_offset(const MetadataPage* page, size_t alignment) {
  const auto base = reinterpret_cast<uintptr_t>(page);
  uintptr_t cursor, aligned;
  if (add_overflow(base, page->cursor, &cursor) || add_overflow(cursor, alignment - 1, &aligned))
    return SIZE_MAX;
  return (aligned & ~(uintptr_t(alignment) - 1)) - base;
}

// Null with errno set (ENOMEM for its own limits, mmap's errno otherwise): a caller's fail() then
// reports this allocation, never an earlier call's errno.
static inline void* metadata_allocate(size_t bytes, size_t alignment, MetadataPage** arena) {
  if (bytes == 0 || !is_power_of_two(alignment)) {
    errno = ENOMEM;
    return nullptr;
  }

  metadata_lock();

  size_t header = sizeof(MetadataPage);
  size_t header_aligned = (header + alignof(max_align_t) - 1) & ~(alignof(max_align_t) - 1);
  MetadataPage* page = *arena;
  while (page != nullptr) {
    size_t offset = metadata_offset(page, alignment);
    if (offset <= page->mapped_size && bytes <= page->mapped_size - offset) {
      page->cursor = offset + bytes;
      void* result = reinterpret_cast<uint8_t*>(page) + offset;
      memset(result, 0, bytes);
      metadata_unlock();
      return result;
    }
    page = page->next;
  }

  size_t required;
  if (add_overflow(header_aligned, alignment - 1, &required) ||
      add_overflow(required, bytes, &required)) {
    metadata_unlock();
    errno = ENOMEM;
    return nullptr;
  }
  uintptr_t rounded;
  if (!page_ceil(required, &rounded)) {
    metadata_unlock();
    errno = ENOMEM;
    return nullptr;
  }
  size_t mapping_size = rounded < kMetadataPageSize ? kMetadataPageSize : rounded;

  void* mapping =
      mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    const int error = errno;
    metadata_unlock();
    errno = error;
    return nullptr;
  }

  page = static_cast<MetadataPage*>(mapping);
  g_metadata_bytes += mapping_size;
  g_peak_metadata_bytes = std::max(g_peak_metadata_bytes, g_metadata_bytes);
  page->next = *arena;
  page->mapped_size = mapping_size;
  page->cursor = header_aligned;
  page->storage = MetadataPage::Storage::Mapping;
  *arena = page;

  size_t offset = metadata_offset(page, alignment);
  page->cursor = offset + bytes;
  void* result = reinterpret_cast<uint8_t*>(page) + offset;
  memset(result, 0, bytes);

  metadata_unlock();
  return result;
}

template <typename T>
T* metadata_object(MetadataPage** arena) {
  void* memory = metadata_allocate(sizeof(T), alignof(T), arena);
  return memory == nullptr ? nullptr : new (memory) T{};
}

// False when a page stays mapped; it is parked for shutdown, which tries it again.
[[nodiscard]] static inline bool release_metadata(MetadataPage* pages) {
  bool released = true;
  while (pages) {
    MetadataPage* next = pages->next;
    const size_t bytes = pages->mapped_size;

    const bool mapped = pages->storage == MetadataPage::Storage::Mapping;
    const bool unmapped = !mapped || munmap(pages, bytes) == 0;
    // Before the lock, so the error keeps munmap's errno.
    if (!unmapped) report_cleanup_failure();
    metadata_lock();
    if (unmapped) {
      g_metadata_bytes -= bytes;
    } else {
      pages->next = g_metadata_pages;
      g_metadata_pages = pages;
      released = false;
    }
    metadata_unlock();
    pages = next;
  }
  return released;
}

template <typename T>
T* allocate_object() {
  void* storage = calloc(1, sizeof(T));
  return storage ? new (storage) T{} : nullptr;
}
template <typename T>
void free_object(T* object) {
  if (object) {
    object->~T();
    free(object);
  }
}

}  // namespace yukilinker::internal
