// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#include "../internal/caller_stubs.hpp"

#include <sys/mman.h>

#include "../internal/runtime_model.hpp"

extern "C" {
// caller_stub.S: the template each stub copies and the entries its literal names.
extern const uint8_t yuki_caller_stub_template[];
void yuki_caller_dlopen();
void yuki_caller_android_dlopen_ext();
void yuki_caller_dlsym();
void yuki_caller_dlvsym();
void yuki_caller_dl_iterate_phdr();
}

namespace yukilinker::internal {

namespace {

// One stub: code followed by its binding-cell address and its entry address.
#if defined(__aarch64__)
constexpr size_t kStubCode = 16;
constexpr size_t kStubBytes = 32;
constexpr size_t kGenerationAt = 16;
constexpr size_t kEntryAt = 24;
#else
constexpr size_t kStubCode = 8;
constexpr size_t kStubBytes = 16;
constexpr size_t kGenerationAt = 8;
constexpr size_t kEntryAt = 12;
#endif
constexpr size_t kImageStubBytes = kCallerStubs * kStubBytes;
static_assert(sizeof(CallerStubPage) <= 4096);
static_assert(16384 / kImageStubBytes <= kCallerStubMaxSlots);
size_t g_stub_budget = kCallerStubDefaultBudget;

struct StubName {
  const char* name;
  void (*entry)();
};
constexpr StubName kStubNames[kCallerStubs] = {
    {"dlopen", yuki_caller_dlopen},
    {"android_dlopen_ext", yuki_caller_android_dlopen_ext},
    {"dlsym", yuki_caller_dlsym},
    {"dlvsym", yuki_caller_dlvsym},
    {"dl_iterate_phdr", yuki_caller_dl_iterate_phdr}};

int stub_index(const char* name) {
  for (size_t i = 0; i < kCallerStubs; ++i)
    if (strcmp(name, kStubNames[i].name) == 0) return static_cast<int>(i);
  return -1;
}

// Whether image imports any of the stub names: an undefined dynamic symbol of that name.
bool imports_stub_names(const ImageState* image) {
  for (size_t i = 0; i < image->symbols.count; ++i) {
    const auto& symbol = image->symbols.entries[i];
    if (symbol.st_shndx != SHN_UNDEF || !symbol_name_valid(image->symbols, symbol)) continue;
    if (stub_index(image->symbols.strings + symbol.st_name) >= 0) return true;
  }
  return false;
}

}  // namespace

CallerStubPage* g_caller_stub_pages = nullptr;

bool make_caller_stubs(ImageState* image) {
  if (!image->context->caller_stubs || image->host || !imports_stub_names(image)) return true;
  for (auto* record = g_caller_stub_pages; record; record = record->next) {
    if (!record->executable || record->used == record->capacity) continue;
    for (size_t i = 0; i < record->capacity; ++i) {
      if (record->generations[i]) continue;
      record->generations[i] = static_cast<uintptr_t>(image->generation);
      ++record->used;
      image->caller_stubs = record->base + i * kImageStubBytes;
      return true;
    }
  }
  const size_t page = loader_page_size();
  size_t pages = 0, bytes = 0;
  caller_stub_stats(&pages, &bytes);
  if (bytes > g_stub_budget || 2 * page > g_stub_budget - bytes) {
    host_log_error("yukilinker: caller stub budget exhausted (%zu bytes)", g_stub_budget);
    return fail(YUKILINKER_C_NO_MEMORY, YUKILINKER_C_PROTECTION);
  }
  void* memory =
      mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    return fail_errno(YUKILINKER_C_PROTECTION, errno);
  }
  auto* base = static_cast<uint8_t*>(memory);
  auto* record = new (base + page) CallerStubPage{};
  record->base = base;
  record->bytes = 2 * page;
  record->capacity = page / kImageStubBytes;
  record->next = g_caller_stub_pages;
  g_caller_stub_pages = record;
  // Register ownership before protection changes: the open's failure sweep also reclaims a slab
  // whose allocating thread ends inside mprotect. No import can execute before this succeeds.
  record->generations[0] = static_cast<uintptr_t>(image->generation);
  record->used = 1;
  image->caller_stubs = base;
  for (size_t slot = 0; slot < record->capacity; ++slot) {
    const auto binding = reinterpret_cast<uintptr_t>(&record->generations[slot]);
    for (size_t i = 0; i < kCallerStubs; ++i) {
      uint8_t* stub = base + slot * kImageStubBytes + i * kStubBytes;
      memcpy(stub, yuki_caller_stub_template, kStubCode);
      const auto entry = reinterpret_cast<uintptr_t>(kStubNames[i].entry);
      memcpy(stub + kGenerationAt, &binding, sizeof(binding));
      memcpy(stub + kEntryAt, &entry, sizeof(entry));
    }
  }
  // Publish once. Subsequent allocations update only the data page, never active instructions.
  if (mprotect(memory, page, PROT_READ | PROT_EXEC | bti_protection()) != 0) {
    return fail_errno(YUKILINKER_C_PROTECTION, errno);
  }
  __builtin___clear_cache(reinterpret_cast<char*>(base), reinterpret_cast<char*>(base + page));
  record->executable = true;
  return true;
}

void discard_unpublished_caller_stubs(ImageState* image) {
  if (!image->caller_stubs) return;
  for (CallerStubPage** link = &g_caller_stub_pages; *link; link = &(*link)->next) {
    CallerStubPage* page = *link;
    const uintptr_t address = reinterpret_cast<uintptr_t>(image->caller_stubs);
    const uintptr_t base = reinterpret_cast<uintptr_t>(page->base);
    if (address < base || address - base >= page->capacity * kImageStubBytes) continue;
    const size_t slot = (address - base) / kImageStubBytes;
    page->generations[slot] = 0;
    --page->used;
    if (page->used) break;
    page->executable = false;
    CallerStubPage* next = page->next;
    // The record lives in the second page, so take its successor before unmapping it.
    if (munmap(page->base, page->bytes) != 0) {
      report_cleanup_failure();
      break;
    }
    *link = next;
    break;
  }
  image->caller_stubs = nullptr;
}

void* caller_stub_for(const ImageState* image, const char* name) {
  if (!image->caller_stubs || !name) return nullptr;
  const int index = stub_index(name);
  if (index < 0) return nullptr;
  // On armeabi-v7a the stub is ARM code, so its address stays even: the import enters it in ARM
  // state.
  return image->caller_stubs + static_cast<size_t>(index) * kStubBytes;
}

bool release_caller_stub_pages() {
  bool released = true;
  CallerStubPage** link = &g_caller_stub_pages;
  while (auto* page = *link) {
    CallerStubPage* next = page->next;
    if (munmap(page->base, page->bytes) != 0) {
      released = false;
      link = &page->next;
      continue;
    }
    *link = next;
  }
  return released;
}

void caller_stub_stats(size_t* pages, size_t* bytes) {
  *pages = 0;
  *bytes = 0;
  for (auto* page = g_caller_stub_pages; page; page = page->next) {
    *pages += 2;
    *bytes += page->bytes;
  }
}

uintptr_t caller_stub_generation(uintptr_t binding) {
  for (auto* page = g_caller_stub_pages; page; page = page->next) {
    const auto start = reinterpret_cast<uintptr_t>(page->generations);
    if (!page->executable || binding < start ||
        binding - start >= page->capacity * sizeof(uintptr_t) ||
        (binding - start) % sizeof(uintptr_t) != 0)
      continue;
    return page->generations[(binding - start) / sizeof(uintptr_t)];
  }
  return 0;
}

bool caller_stub_set_budget(size_t bytes) {
  size_t pages = 0, mapped = 0;
  caller_stub_stats(&pages, &mapped);
  if (bytes < 2 * loader_page_size() || bytes < mapped) return false;
  g_stub_budget = bytes;
  return true;
}

size_t caller_stub_budget() { return g_stub_budget; }

void caller_stub_slots(size_t* used, size_t* capacity) {
  *used = 0;
  *capacity = 0;
  for (auto* page = g_caller_stub_pages; page; page = page->next) {
    *used += page->used;
    if (page->executable) *capacity += page->capacity;
  }
}

}  // namespace yukilinker::internal
