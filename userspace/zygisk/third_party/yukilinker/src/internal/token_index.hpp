// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "config.hpp"

namespace yukilinker::internal {

// An open-addressing index from a random 64-bit token to the record that holds it, so issuing and
// finding a token does not walk every live one. Linear probing with tombstones. An insert that
// would pass half load rebuilds without tombstones, at a capacity sized for the live records
// alone: it grows only when they need it, and shrinks when an erase leaves the table at most a
// sixteenth full. A growth leaves the table more than a sixteenth full, so the next erase does not
// shrink it straight back; a steady count of records keeps one capacity. A rebuild copies only
// live entries, so a token that was erased never comes back.
// Tokens are random, so the low bits spread them. The owner serializes every call (the operation
// lock). A failed rebuild leaves the index as it was; a shrink that cannot allocate keeps the
// larger table.
template <typename T>
class TokenIndex {
 public:
  TokenIndex() = default;
  TokenIndex(const TokenIndex&) = delete;
  TokenIndex& operator=(const TokenIndex&) = delete;

  T* find(uint64_t token) const {
    if (capacity_ == 0 || token == 0) return nullptr;
    for (size_t i = slot(token);; i = (i + 1) & (capacity_ - 1)) {
      const Slot& entry = slots_[i];
      if (entry.token == 0) return nullptr;
      if (entry.token == token && entry.record) return entry.record;
    }
  }

  // False when the index cannot grow: nothing was added.
  bool insert(uint64_t token, T* record) {
    if ((used_ + 1) * 2 > capacity_ && !rebuild(fitting(live_ + 1))) return false;
    size_t i = slot(token);
    while (slots_[i].token != 0 && slots_[i].record) i = (i + 1) & (capacity_ - 1);
    if (slots_[i].token == 0) ++used_;
    slots_[i] = {token, record};
    ++live_;
    return true;
  }

  void erase(uint64_t token) {
    if (capacity_ == 0) return;
    for (size_t i = slot(token);; i = (i + 1) & (capacity_ - 1)) {
      Slot& entry = slots_[i];
      if (entry.token == 0) return;
      if (entry.token == token && entry.record) {
        // A tombstone: the token stays, the record goes, and probes walk past it.
        entry.record = nullptr;
        --live_;
        break;
      }
    }
    // Empty again: release a dynamic table and discard every tombstone. The fixed minimum
    // belongs to the index itself; repeated single-image opens require no allocation.
    if (live_ == 0) {
      if (slots_ != local_) {
        free(slots_);
      }
      memset(local_, 0, sizeof(local_));
      slots_ = local_;
      capacity_ = kMinimumCapacity;
      used_ = 0;
      return;
    }
    // Mostly empty: a smaller table, so a burst of records leaves nothing behind once they go.
    if (capacity_ > kMinimumCapacity && live_ * 16 <= capacity_) (void)rebuild(fitting(live_));
  }

 private:
  struct Slot {
    uint64_t token;
    T* record;
  };

  static constexpr size_t kMinimumCapacity = 16;

  // The smallest power of two of at least kMinimumCapacity that keeps count records at most a
  // quarter full, so the next rebuild is count more inserts away.
  static size_t fitting(size_t count) {
    size_t capacity = kMinimumCapacity;
    while (capacity / 4 < count) capacity *= 2;
    return capacity;
  }

  size_t slot(uint64_t token) const { return slot_in(token, capacity_); }

  static size_t slot_in(uint64_t token, size_t capacity) {
    return static_cast<size_t>(token ^ (token >> 32)) & (capacity - 1);
  }

  bool rebuild(size_t capacity) {
    auto* rebuilt = local_;
    if (capacity > kMinimumCapacity) {
      rebuilt = static_cast<Slot*>(calloc(capacity, sizeof(Slot)));
    }
    if (!rebuilt) return false;
    Slot saved[kMinimumCapacity];
    const Slot* source = slots_;
    if (rebuilt == local_) {
      if (slots_ == local_) {
        memcpy(saved, local_, sizeof(saved));
        source = saved;
      }
      memset(local_, 0, sizeof(local_));
    }
    for (size_t i = 0; i < capacity_; ++i) {
      if (source[i].token == 0 || !source[i].record) continue;
      size_t j = slot_in(source[i].token, capacity);
      while (rebuilt[j].token != 0) j = (j + 1) & (capacity - 1);
      rebuilt[j] = source[i];
    }
    if (slots_ != local_) {
      free(slots_);
    }
    slots_ = rebuilt;
    capacity_ = capacity;
    used_ = live_;
    return true;
  }

  Slot local_[kMinimumCapacity]{};
  Slot* slots_ = local_;
  size_t capacity_ = kMinimumCapacity;
  // Slots ever filled (live records and tombstones), and live records.
  size_t used_ = 0;
  size_t live_ = 0;
};

}  // namespace yukilinker::internal
