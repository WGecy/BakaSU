// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

#include <elf.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>

#include "../../include/yukilinker.h"
#include "config.hpp"
#include "loader_types.hpp"

namespace yukilinker::internal {

extern yukilinker_c_error_detail* g_operation_error;

// The host-import refusal that decided the operation's failure, beside g_operation_error: set only
// together with the operation's first failure (fail keeps only that one), so a refusal a later
// candidate replaced, or one after another failure, is never kept. Strings are copied in.
struct OperationRefusal {
  bool set = false;
  uint32_t reason = 0;
  char library[256]{};
  char symbol[128]{};
  char version[128]{};
};
extern OperationRefusal* g_operation_refusal;
// The refusal slot of the private dl* call the lock holder is in, or null. The public frames that
// call opens (an open, a lookup) record into it, since the call reports their failure as its own;
// an operation deeper (a callback's) opens its own slot. Saved with the operation state wherever
// the lock drops (HeldOperation).
extern OperationRefusal* g_private_call_slot;

// The codes a failed system call produces; errno means nothing next to any other code.
static inline bool errno_derived(uint32_t code) {
  return code == YUKILINKER_C_IO_ERROR || code == YUKILINKER_C_NO_MEMORY ||
         code == YUKILINKER_C_PERMISSION_DENIED;
}

static inline bool fail(uint32_t code, uint32_t stage, const char* symbol = nullptr,
                        const char* version = nullptr, uint64_t offset = 0, uint32_t type = 0) {
  if (g_operation_error && g_operation_error->code == YUKILINKER_C_OK) {
    auto& error = *g_operation_error;
    error.code = code;
    error.stage = stage;
    error.system_errno = errno_derived(code) ? errno : 0;
    error.machine = sizeof(void*) == 8 ? EM_AARCH64 : EM_ARM;
    error.offset = offset;
    error.relocation_type = type;
    if (symbol) (void)snprintf(error.symbol, sizeof(error.symbol), "%s", symbol);
    if (version) (void)snprintf(error.version, sizeof(error.version), "%s", version);
  }
  return false;
}

// fail, recording why a host import refused symbol when this is the operation's first failure.
static inline bool fail_refused(uint32_t code, uint32_t stage, uint32_t reason, const char* library,
                                const char* symbol, const char* version) {
  const bool first = g_operation_error && g_operation_error->code == YUKILINKER_C_OK;
  fail(code, stage, symbol, version);
  if (first && g_operation_refusal) {
    auto& refusal = *g_operation_refusal;
    refusal.set = true;
    refusal.reason = reason;
    (void)snprintf(refusal.library, sizeof(refusal.library), "%s", library ? library : "");
    (void)snprintf(refusal.symbol, sizeof(refusal.symbol), "%s", symbol ? symbol : "");
    (void)snprintf(refusal.version, sizeof(refusal.version), "%s", version ? version : "");
  }
  return false;
}

// Names the relocation a failure already recorded at YUKILINKER_C_RELOCATIONS was for (a refusal or
// a failed walk inside symbol_target, which does not know it), when it named none.
static inline void name_relocation(uint64_t offset, uint32_t type) {
  if (!g_operation_error || g_operation_error->code == YUKILINKER_C_OK ||
      g_operation_error->stage != YUKILINKER_C_RELOCATIONS ||
      g_operation_error->relocation_type != 0)
    return;
  g_operation_error->offset = offset;
  g_operation_error->relocation_type = type;
}

static inline void report_cleanup_failure() {
  if (g_operation_error) g_operation_error->cleanup_incomplete = 1;
  fail(YUKILINKER_C_IO_ERROR, YUKILINKER_C_RETIREMENT);
}

// A failed system call, by the errno it left: EACCES and EPERM are policy (a noexec mount,
// SELinux, a sealed or write-only descriptor), ENOMEM a limit (of the address space or of the
// mappings a process may have, not necessarily of physical memory), anything else an I/O error.
// The errno is kept with the code. Callers save errno right after the call, before anything else
// can change it.
static inline bool fail_errno(uint32_t stage, int error) {
  errno = error;
  return fail(error == EACCES || error == EPERM ? YUKILINKER_C_PERMISSION_DENIED
              : error == ENOMEM                 ? YUKILINKER_C_NO_MEMORY
                                                : YUKILINKER_C_IO_ERROR,
              stage);
}

// A failure that no system call reported (an overflow, a size that does not fit, a check): no
// errno goes with it, whatever errno holds, even for a code fail would otherwise give one.
static inline bool fail_no_errno(uint32_t code, uint32_t stage, uint64_t offset = 0) {
  const bool fresh = g_operation_error && g_operation_error->code == YUKILINKER_C_OK;
  fail(code, stage, nullptr, nullptr, offset);
  if (fresh) g_operation_error->system_errno = 0;
  return false;
}

}  // namespace yukilinker::internal
