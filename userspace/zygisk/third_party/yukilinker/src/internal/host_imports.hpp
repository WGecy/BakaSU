// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

// Explicit host imports (yukilinker.h, namespace_host_import). host_imports.cpp is built into the
// runtime only; the bootstrap loader neither registers nor resolves them.

#include "runtime_model.hpp"

namespace yukilinker::internal {

struct HostImportResult {
  void* address = nullptr;
  // The import's handle, which keeps the provider loaded while the namespace lives.
  void* handle = nullptr;
  bool found = false;
  // Some import of the namespace lists the name.
  bool listed = false;
  // A listed host symbol failed a check; the reason was logged and nothing binds to it.
  bool refused = false;
  // The lookup itself could not finish (NO_MEMORY, recorded); nothing binds and no reason applies.
  bool failed = false;
  // The import's position in its context's list (stable: imports freeze before any image
  // registers), so a found binding can be recorded in ImageState::host_import_mask.
  uint32_t index = 0;
  // A refusal's yukilinker_c_host_import_reason and the import's library, copied: the import may go
  // once the lock drops.
  uint32_t reason = 0;
  char library[256]{};
};

// How a lookup reaches the imports: a relocation binds and records, a private dlsym looks up
// (FUNC, OBJECT or NOTYPE; TLS and IFUNC refused, the reason in dlerror).
enum class HostImportUse { kRelocation, kLookup };

uint32_t api_namespace_host_import(void* opaque, const yukilinker_c_host_import* import);
uint32_t api_namespace_host_imports(void* opaque, yukilinker_c_host_import_record* records,
                                    size_t capacity, size_t* count);
// Drops a context's imports and the handles that kept their libraries loaded, together with what an
// earlier release that stopped short left. False when the host failed to close one: that library
// stays loaded, the entry is gone either way, and g_host_import_leaked is set.
bool release_host_imports(Context* context);
// The binding of an undefined symbol of image that its private scope did not resolve, through the
// imports of image's namespace. May drop the lock (host calls).
HostImportResult resolve_host_import(ImageState* image, const char* name, const char* version,
                                     uint32_t relocation_type);
// A private dlsym's view of context's imports: those in mask only (bit i the i-th import), or all
// of them when mask is ~0. Never runs a resolver; a refused symbol is reported as refused.
HostImportResult lookup_host_import(Context* context, ImageState* caller, uint32_t mask,
                                    const char* name, const char* version);

}  // namespace yukilinker::internal
