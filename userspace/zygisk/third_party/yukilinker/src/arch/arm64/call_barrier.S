// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#if !defined(__aarch64__)
#error "call_barrier.S is the arm64 barrier"
#endif

/*
 * uintptr_t yuki_call_barrier(const void* function, uintptr_t a0, uintptr_t a1, uintptr_t a2)
 * calls function(a0, a1, a2). Its frame names a personality that fails the search phase, so an
 * exception escaping module code ends in std::terminate here instead of unwinding through runtime
 * frames, which have no cleanups. A backtrace still walks through it: _Unwind_Backtrace never calls
 * the personality. (The arm32 barrier's EHABI personality stops a backtrace there.)
 */
.text
.p2align 2
.hidden yuki_call_barrier
.global yuki_call_barrier
.type yuki_call_barrier, %function
yuki_call_barrier:
.cfi_startproc
.cfi_personality 0x1b, yuki_barrier_personality
    hint #34
    stp x29, x30, [sp, #-16]!
    .cfi_def_cfa_offset 16
    .cfi_rel_offset x29, 0
    .cfi_rel_offset x30, 8
    mov x29, sp
    mov x16, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    blr x16
    ldp x29, x30, [sp], #16
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    ret
.cfi_endproc
.size yuki_call_barrier, .-yuki_call_barrier

/* (version, actions, ...): the search phase (_UA_SEARCH_PHASE, bit 0) gets _URC_FATAL_PHASE1_ERROR
 * (3), so _Unwind_RaiseException returns and the thrower terminates; a cleanup or forced phase
 * gets _URC_FATAL_PHASE2_ERROR (2). */
.p2align 2
.hidden yuki_barrier_personality
.global yuki_barrier_personality
.type yuki_barrier_personality, %function
yuki_barrier_personality:
.cfi_startproc
    hint #34
    and w1, w1, #1
    mov w0, #2
    add w0, w0, w1
    ret
.cfi_endproc
.size yuki_barrier_personality, .-yuki_barrier_personality

.section .note.GNU-stack, "", %progbits
// GNU_PROPERTY_AARCH64_FEATURE_1_AND: BTI (1), PAC (2) and GCS (4) as the compiler's
// -mbranch-protection sets them, so this object never clears a feature the C++ objects carry.
#define YUKI_FEATURE_BTI 0
#define YUKI_FEATURE_PAC 0
#define YUKI_FEATURE_GCS 0
#if defined(__ARM_FEATURE_BTI_DEFAULT) && __ARM_FEATURE_BTI_DEFAULT
#undef YUKI_FEATURE_BTI
#define YUKI_FEATURE_BTI 1
#endif
#if defined(__ARM_FEATURE_PAC_DEFAULT) && __ARM_FEATURE_PAC_DEFAULT
#undef YUKI_FEATURE_PAC
#define YUKI_FEATURE_PAC 2
#endif
#if defined(__ARM_FEATURE_GCS_DEFAULT) && __ARM_FEATURE_GCS_DEFAULT
#undef YUKI_FEATURE_GCS
#define YUKI_FEATURE_GCS 4
#endif
#if YUKI_FEATURE_BTI | YUKI_FEATURE_PAC | YUKI_FEATURE_GCS
.section .note.gnu.property, "a"
.balign 8
.long 4        // name size = "GNU\0"
.long 0x10     // desc size (16 bytes)
.long 5        // NT_GNU_PROPERTY_TYPE_0
.asciz "GNU"
.long 0xc0000000 // GNU_PROPERTY_AARCH64_FEATURE_1_AND
.long 4          // pr_datasz
.long YUKI_FEATURE_BTI | YUKI_FEATURE_PAC | YUKI_FEATURE_GCS
.long 0          // padding
#endif
