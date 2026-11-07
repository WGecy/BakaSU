// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../include/yukilinker.h"
#include "config.hpp"
#include "loader_types.hpp"
#include "token_index.hpp"

namespace yukilinker::internal {

struct NamespaceLink;
struct HostImport;

// An explicit retirement of a context (retire_scope_images). Its read-only checks never drop the
// lock, so a refused retirement leaves it Open. Destroying from the first step that changes
// anything, and for good: a retirement that stopped short (a failed unmap, a thread that ended in
// a destructor) is resumed by the next destroy, and the context admits nothing new meanwhile.
enum class Retirement : uint8_t { kOpen, kDestroying };

struct Context {
  Context* next = nullptr;
  // g_contexts is doubly linked, so a destroy unlinks without a walk.
  Context* previous = nullptr;
  // Registered images of this context (register_image, unregister_image).
  size_t images = 0;
  char search_path[4096]{};
  yukilinker_c_dependency_provider provider = nullptr;
  void* user = nullptr;
  size_t snapshot_refs = 0;
  size_t namespace_refs = 0;
  uint64_t namespace_id = 0;
  Context* namespace_parent = nullptr;
  // namespace_link's links in the order they were added, and how many links lead here.
  NamespaceLink* links = nullptr;
  size_t link_refs = 0;
  // namespace_host_import's imports in the order they were added (host_imports.cpp, runtime only).
  HostImport* host_imports = nullptr;
  // Set when the first image registers here; the links and host imports are fixed from then on.
  bool links_frozen = false;
  Retirement retirement = Retirement::kOpen;
  // Opens in this context waiting for another thread's open; the context outlives them.
  size_t waiting_opens = 0;
  // Managed contexts route image calls to dlopen, android_dlopen_ext, dlsym and dlvsym
  // through caller stubs (caller_stubs.hpp). The reduced loader leaves this disabled.
  bool caller_stubs = false;
  // PRIVATE's explicit compatibility contract; fixed before any image loads.
  bool strict_private = false;
};

// Whether context is under an explicit retirement: nothing may be added to it (an image, a
// reference, a pin, a namespace, a link). Read under the operation lock.
static inline bool context_sealed(const Context* context) {
  return context != nullptr && context->retirement != Retirement::kOpen;
}

constexpr size_t kMaxNamespaceLinks = 16;

// One link of a namespace's Context: to another namespace's Context, or to the system linker
// (target null). shared holds the ':'-separated sonames as given, empty for
// YUKILINKER_C_NAMESPACE_LINK_ALL.
struct NamespaceLink {
  NamespaceLink* next = nullptr;
  Context* target = nullptr;
  uint32_t flags = 0;
  char shared[4096]{};
};

extern Context g_default_context;
extern Context* g_contexts;

struct NamespaceToken;
extern NamespaceToken* g_namespaces;
// Every live namespace by its opaque token, so a lookup costs no walk of the others.
extern TokenIndex<NamespaceToken> g_namespace_index;
extern uint64_t g_next_namespace_id;

struct NamespaceToken {
  uint64_t id = 0;
  void* opaque = nullptr;
  Context* context = nullptr;
  NamespaceToken* next = nullptr;
  NamespaceToken* previous = nullptr;
};

// The context lists are doubly linked: these add at the head and unlink in one step.
static inline void link_context(Context* context) {
  context->previous = nullptr;
  context->next = g_contexts;
  if (g_contexts) g_contexts->previous = context;
  g_contexts = context;
}

static inline void unlink_context(Context* context) {
  if (context->previous)
    context->previous->next = context->next;
  else
    g_contexts = context->next;
  if (context->next) context->next->previous = context->previous;
  context->next = context->previous = nullptr;
}

static inline void link_namespace(NamespaceToken* token) {
  token->previous = nullptr;
  token->next = g_namespaces;
  if (g_namespaces) g_namespaces->previous = token;
  g_namespaces = token;
}

static inline void unlink_namespace(NamespaceToken* token) {
  if (token->previous)
    token->previous->next = token->next;
  else
    g_namespaces = token->next;
  if (token->next) token->next->previous = token->previous;
  token->next = token->previous = nullptr;
}

static inline Context* context_from_opaque(void* opaque) {
  if (!opaque || opaque == &g_default_context) return &g_default_context;
  for (auto* context = g_contexts; context; context = context->next)
    if (context == opaque) return context;
  if (auto* token = g_namespace_index.find(reinterpret_cast<uintptr_t>(opaque)))
    return token->context;
  return nullptr;
}

static inline bool valid_context(void* opaque) { return context_from_opaque(opaque) != nullptr; }

}  // namespace yukilinker::internal
