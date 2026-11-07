// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <link.h>
#include <sys/auxv.h>

#include <cstddef>
#include <cstdint>

#include "config.hpp"

namespace yukilinker::internal {

// The page size mappings are laid out with, once committed; 0 until then.
extern size_t g_page_size;
// g_page_size was committed by an image that registered, not by runtime_create: a failed open that
// leaves no image behind takes the commit back (release_image_page_size).
extern bool g_page_size_from_image;

static inline bool is_power_of_two(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static inline size_t kernel_page_size() {
  const unsigned long aux_page_size = getauxval(AT_PAGESZ);
  return aux_page_size >= 4096 && is_power_of_two(aux_page_size)
             ? static_cast<size_t>(aux_page_size)
             : 4096;
}

// The committed page size, or the kernel's while none is: reading it commits nothing.
static inline size_t loader_page_size() {
  return g_page_size != 0 ? g_page_size : kernel_page_size();
}

// Called as the first image registers (load_image): from then on the page size is fixed, also for
// a runtime_create on another thread while this open drops the lock.
static inline size_t commit_page_size() {
  if (g_page_size == 0) {
    g_page_size = kernel_page_size();
    g_page_size_from_image = true;
  }
  return g_page_size;
}

// Takes back an image's commit once nothing it laid out is left (no image registered, kept or
// dead): an open that fails, before or after it registers, leaves a host free to ask for another
// size at runtime_create. A size runtime_create adopted stays.
static inline void release_image_page_size(bool images_left) {
  if (g_page_size_from_image && !images_left) {
    g_page_size = 0;
    g_page_size_from_image = false;
  }
}

static inline bool add_overflow(size_t a, size_t b, size_t* out) {
  if (a > SIZE_MAX - b) return true;
  *out = a + b;
  return false;
}

static inline bool multiply_overflow(size_t a, size_t b, size_t* out) {
  if (a != 0 && b > SIZE_MAX / a) return true;
  *out = a * b;
  return false;
}

static inline bool add_signed_offset(uintptr_t base, ElfW(Sxword) offset, uintptr_t* out) {
  if (offset >= 0) {
    uintptr_t positive = static_cast<uintptr_t>(offset);
    if (positive > UINTPTR_MAX - base) return false;
    *out = base + positive;
    return true;
  }
  uintptr_t magnitude = static_cast<uintptr_t>(-(offset + 1)) + 1;
  if (magnitude > base) return false;
  *out = base - magnitude;
  return true;
}

// base + addend in the address width, wrapping, as the system linker computes a relocated word: a
// REL addend of 0x80000000 or 0xffffffff on LP32 (sym - 1) and S + A of an undefined weak symbol
// with a negative addend are legal values. The word is the image's to read, never the loader's.
static inline uintptr_t apply_addend(uintptr_t base, ElfW(Sxword) addend) {
  return base + static_cast<uintptr_t>(addend);
}

static inline uintptr_t page_floor(uintptr_t value) {
  return value & ~(static_cast<uintptr_t>(loader_page_size()) - 1);
}

static inline bool page_ceil(uintptr_t value, uintptr_t* out) {
  size_t page_size = loader_page_size();
  if (value > UINTPTR_MAX - (page_size - 1)) return false;
  *out = page_floor(value + page_size - 1);
  return true;
}

static inline size_t smaller(size_t a, size_t b) { return a < b ? a : b; }

}  // namespace yukilinker::internal
