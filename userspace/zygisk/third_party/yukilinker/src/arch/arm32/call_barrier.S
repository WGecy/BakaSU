// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#if !defined(__arm__)
#error "call_barrier.S is the arm32 barrier"
#endif

/*
 * uintptr_t yuki_call_barrier(const void* function, uintptr_t a0, uintptr_t a1, uintptr_t a2)
 * calls function(a0, a1, a2). Its EHABI entry names a personality that fails the search phase, so
 * an exception escaping module code ends in std::terminate here instead of unwinding through
 * runtime frames, which have no cleanups. EHABI's _Unwind_Backtrace calls the personality for every
 * frame, so a backtrace stops at this one.
 */
.syntax unified
.arm
.text
.p2align 2
.hidden yuki_call_barrier
.global yuki_call_barrier
.type yuki_call_barrier, %function
yuki_call_barrier:
.fnstart
.personality yuki_barrier_personality
    push {r4, lr}
    .save {r4, lr}
    mov r4, r0
    mov r0, r1
    mov r1, r2
    mov r2, r3
    blx r4
    pop {r4, pc}
.fnend
.size yuki_call_barrier, .-yuki_call_barrier

/* Every state gets _URC_FAILURE (9): _Unwind_RaiseException returns and the thrower terminates. */
.p2align 2
.hidden yuki_barrier_personality
.global yuki_barrier_personality
.type yuki_barrier_personality, %function
yuki_barrier_personality:
.fnstart
.cantunwind
    mov r0, #9
    bx lr
.fnend
.size yuki_barrier_personality, .-yuki_barrier_personality

.section .note.GNU-stack, "", %progbits
