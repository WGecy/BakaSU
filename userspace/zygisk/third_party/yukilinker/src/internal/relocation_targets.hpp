// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace yukilinker::internal {

// The image bytes relocation pass -1 saw a relocation write, so that two which overlap are refused
// before any is applied. One bit per byte, in leaves that each cover 1 KiB of the image, reached
// through a radix tree of 4 KiB nodes only as deep as the image's highest writable address needs.
// A leaf or node exists only once a target lands under it, so the cost follows the targets, never
// the span a segment declares, and each step is constant time whatever the addresses. Leaves and
// nodes are carved from 64 KiB blocks, freed together. The owner serializes every call (the
// operation lock).
class RelocationTargets {
 public:
  enum class Mark { kNew, kInvalid, kTooMany, kNoMemory };

  // Bound the number of marked targets independently of radix-tree storage.
  static constexpr size_t kMaxTargets = size_t{16} * 1024 * 1024;
  // The memory the leaves and nodes may take, so that a refused open stays within 16 MiB and is
  // refused before it has marked more than a few tens of thousands of scattered targets. Targets
  // side by side cost an eighth of the bytes they cover plus the nodes above them: about 7.8
  // million pointer-sized ones on LP64 (60 MiB of image), 16.2 million on LP32 (62 MiB).
  // Scattered targets cost a leaf each, and a node each once they are further apart than a node
  // covers (512 KiB on LP64, 1 MiB on LP32): about 49 thousand a page apart (57 thousand on LP32),
  // about 1900 each under a node of its own. Past the bound the open is refused
  // (UNSUPPORTED/RELOCATIONS).
  static constexpr size_t kMaxBytes = size_t{8} * 1024 * 1024;

  RelocationTargets() = default;
  RelocationTargets(const RelocationTargets&) = delete;
  RelocationTargets& operator=(const RelocationTargets&) = delete;
  RelocationTargets(RelocationTargets&&) = delete;
  RelocationTargets& operator=(RelocationTargets&&) = delete;
  ~RelocationTargets() { release(); }

  // Starts over for targets below end, the end of the highest writable segment.
  void begin(uintptr_t end) {
    release();
    end_ = end;
    const uintptr_t last = end ? (end - 1) >> kLeafShift : 0;
    levels_ = 1;
    while (levels_ * kFanoutBits < kIndexBits && (last >> (levels_ * kFanoutBits)) != 0) ++levels_;
  }

  // Records [offset, offset + width). kInvalid when a byte of it was recorded before, or when it
  // reaches past the end begin was given. A refusal records none of it: every word is checked
  // before any is set (a relocation writes at most two words, a TLS descriptor's).
  // Inlined, with the common case of a leaf found in the cache: pass -1 marks every relocation.
  [[gnu::always_inline]] Mark mark(uintptr_t offset, size_t width) {
    if (count_ == kMaxTargets) return Mark::kTooMany;
    uintptr_t end;
    if (width == 0 || width > kMaxWidth || __builtin_add_overflow(offset, width, &end) ||
        end > end_)
      return Mark::kInvalid;
    uint64_t* words[kMaxChunks];
    uint64_t masks[kMaxChunks];
    size_t chunks = 0;
    for (uintptr_t at = offset; at != end; ++chunks) {
      Mark failure = Mark::kInvalid;
      const uintptr_t index = at >> kLeafShift;
      uint64_t* leaf = cached_ && index == cached_index_ ? cached_ : leaf_for(index, &failure);
      if (!leaf) return failure;
      const size_t bit = static_cast<size_t>(at & (kLeafBytes - 1));
      const size_t room = 64 - (bit & 63);
      const size_t take = end - at < room ? static_cast<size_t>(end - at) : room;
      const uint64_t mask = (take == 64 ? ~uint64_t{0} : (uint64_t{1} << take) - 1) << (bit & 63);
      uint64_t* word = &leaf[bit >> 6];
      if (*word & mask) return Mark::kInvalid;
      words[chunks] = word;
      masks[chunks] = mask;
      at += take;
    }
    for (size_t i = 0; i < chunks; ++i) *words[i] |= masks[i];
    ++count_;
    return Mark::kNew;
  }

  // Frees every block.
  void release() {
    while (blocks_) {
      void* next = *static_cast<void**>(blocks_);
      free(blocks_);
      blocks_ = next;
    }
    root_ = nullptr;
    cached_ = nullptr;
    used_ = kBlockBytes;
    bytes_ = 0;
    count_ = 0;
  }

 private:
  static constexpr unsigned kLeafShift = 10;
  static constexpr uintptr_t kLeafBytes = uintptr_t{1} << kLeafShift;
  static constexpr size_t kLeafSize = kLeafBytes / 8;
  static constexpr size_t kNodeSize = 4096;
  static constexpr size_t kFanout = kNodeSize / sizeof(void*);
  static constexpr unsigned kFanoutBits = sizeof(void*) == 8 ? 9 : 10;
  static constexpr unsigned kIndexBits = sizeof(uintptr_t) * 8 - kLeafShift;
  static constexpr size_t kBlockBytes = size_t{64} * 1024;
  // The widest write a relocation makes is two words. A bitmap word covers 64 bytes, and a leaf
  // starts on one, so such a write spans at most two bitmap words.
  static constexpr size_t kMaxWidth = 2 * sizeof(void*);
  static constexpr size_t kMaxChunks = 2;
  static_assert(kMaxWidth <= 64, "a write spans at most kMaxChunks bitmap words");
  // A block starts with the link to the one before it; leaves and nodes follow, 64-bit aligned.
  static constexpr size_t kBlockHeader = sizeof(uint64_t);
  static_assert((size_t{1} << kFanoutBits) == kFanout, "a node indexes kFanoutBits bits");
  static_assert(kMaxBytes % kBlockBytes == 0, "the bound is whole blocks");

  // Zeroed memory for a leaf or a node, from the current block or a new one.
  void* allocate(size_t size, Mark* failure) {
    if (size > kBlockBytes - used_) {
      if (kBlockBytes > kMaxBytes - bytes_) {
        *failure = Mark::kTooMany;
        return nullptr;
      }
      void* block = calloc(1, kBlockBytes);
      if (!block) {
        *failure = Mark::kNoMemory;
        return nullptr;
      }
      *static_cast<void**>(block) = blocks_;
      blocks_ = block;
      bytes_ += kBlockBytes;
      used_ = kBlockHeader;
    }
    void* memory = static_cast<uint8_t*>(blocks_) + used_;
    used_ += size;
    return memory;
  }

  // The leaf for index, made when none exists yet, and cached for the next mark.
  [[gnu::noinline]] uint64_t* leaf_for(uintptr_t index, Mark* failure) {
    void** slot = &root_;
    for (unsigned level = levels_; level != 0; --level) {
      if (!*slot) *slot = allocate(kNodeSize, failure);
      if (!*slot) return nullptr;
      const size_t at = static_cast<size_t>(index >> ((level - 1) * kFanoutBits)) & (kFanout - 1);
      slot = &static_cast<void**>(*slot)[at];
    }
    if (!*slot) *slot = allocate(kLeafSize, failure);
    if (!*slot) return nullptr;
    cached_index_ = index;
    cached_ = static_cast<uint64_t*>(*slot);
    return cached_;
  }

  void* blocks_ = nullptr;
  size_t used_ = kBlockBytes;
  void* root_ = nullptr;
  uintptr_t end_ = 0;
  unsigned levels_ = 1;
  uint64_t* cached_ = nullptr;
  uintptr_t cached_index_ = 0;
  size_t bytes_ = 0;
  size_t count_ = 0;
};

}  // namespace yukilinker::internal
