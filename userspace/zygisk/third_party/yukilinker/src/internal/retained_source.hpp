// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <sys/mman.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "metadata_arena.hpp"
#include "operation_error.hpp"

namespace yukilinker::internal {

struct RetainedSource {
  void* address = nullptr;
  size_t bytes = 0;
  RetainedSource* next = nullptr;
};
extern RetainedSource* g_retained_sources;

struct SourceMapping {
  RetainedSource* record = nullptr;
  // The errno of a failed mapping, kept before free_object can change it.
  int error = 0;
  SourceMapping(int fd, size_t bytes, uint64_t offset) : record(allocate_object<RetainedSource>()) {
    if (!record) {
      error = errno;
      return;
    }
    record->bytes = bytes;
    record->address =
        mmap64(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, static_cast<off64_t>(offset));
    if (record->address == MAP_FAILED) {
      error = errno;
      free_object(record);
      record = nullptr;
    }
  }
  ~SourceMapping() { reset(); }
  SourceMapping(const SourceMapping&) = delete;
  SourceMapping& operator=(const SourceMapping&) = delete;
  SourceMapping(SourceMapping&&) = delete;
  SourceMapping& operator=(SourceMapping&&) = delete;
  [[nodiscard]] void* get() const { return record ? record->address : nullptr; }
  bool reset() {
    if (!record) return true;
    auto* owned = record;
    record = nullptr;
    if (munmap(owned->address, owned->bytes) != 0) {
      owned->next = g_retained_sources;
      g_retained_sources = owned;
      report_cleanup_failure();
      return false;
    }
    free_object(owned);
    return true;
  }
};

}  // namespace yukilinker::internal
