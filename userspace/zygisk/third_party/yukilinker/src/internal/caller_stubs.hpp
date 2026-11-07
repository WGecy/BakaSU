// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

// Per-image caller stubs for images in managed contexts (caller_stubs.cpp, runtime only). Their
// dlopen, android_dlopen_ext, dlsym, dlvsym and dl_iterate_phdr imports bind to stubs that carry
// the image's generation, so the runtime learns the caller from the import binding instead of the
// return address. A tail call or a function pointer of the image then still acts for it.

#include <cstddef>
#include <cstdint>

namespace yukilinker::internal {

struct ImageState;

constexpr size_t kCallerStubs = 5;
constexpr size_t kCallerStubDefaultBudget = 8 * 1024 * 1024;
constexpr size_t kCallerStubMaxSlots = 256;

// A slab has one immutable RX code page followed by a RW page containing this record. Code
// literals address generations[]; only the entry's C++ code reads those cells, under the operation
// lock. A published cell is never reassigned. Zero denotes an unused, unexposed slot.
struct CallerStubPage {
  uint8_t* base;
  size_t bytes;
  CallerStubPage* next;
  size_t capacity;
  size_t used;
  bool executable;
  uintptr_t generations[kCallerStubMaxSlots];
};
extern CallerStubPage* g_caller_stub_pages;

// Maps image's stubs when it uses a managed context and imports any of the names; true and
// nothing done otherwise. A refused mapping fails the open (PROTECTION, by errno): such an image
// never falls back to the return address.
bool make_caller_stubs(ImageState* image);
// Reclaims image's slot when its open fails and its group ran no code and was not pinned
// (release_failed_image): a stub address went only into the image's own relocated words, which are
// unmapped first, so it was never handed out. Empty slabs are unmapped. Published slots stay
// until the runtime is destroyed. A slab whose munmap fails stays listed and unavailable.
void discard_unpublished_caller_stubs(ImageState* image);
// The stub image's import of name binds to, or null when it has none for that name.
void* caller_stub_for(const ImageState* image, const char* name);
// Unmaps every stub page; false when one could not be unmapped (it stays listed).
bool release_caller_stub_pages();
void caller_stub_stats(size_t* pages, size_t* bytes);
// All functions require the operation lock. The budget covers both code and data mappings.
uintptr_t caller_stub_generation(uintptr_t binding);
bool caller_stub_set_budget(size_t bytes);
size_t caller_stub_budget();
void caller_stub_slots(size_t* used, size_t* capacity);

}  // namespace yukilinker::internal
