// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#ifndef YUKILINKER_FULL
#define YUKILINKER_FULL 0
#endif

#if !defined(__arm__)
#error "caller_stub.S is the arm32 caller stub"
#endif

#if YUKILINKER_FULL

/*
 * The caller-dependent imports of an image in a managed namespace bind to a
 * slab of per-image stubs (caller_stubs.cpp). Each stub is the ARM template below with its two
 * literals patched: it loads the binding-cell address into ip (r12) and jumps to one of the entries,
 * which pass it on as the last argument of the C++ function that reads the generation under the
 * operation lock. ip is the intra-procedure scratch register a PLT clobbers anyway; lr is the
 * import's own return. A PLT
 * enters the stub in ARM state (its address is even); in ARM state pc reads 8 ahead, so each ldr
 * below reads the word two after it, and ldr pc switches to Thumb for the entry.
 */
.syntax unified
.arm
.text
.p2align 2
.hidden yuki_caller_stub_template
.global yuki_caller_stub_template
.type yuki_caller_stub_template, %object
yuki_caller_stub_template:
    ldr ip, [pc, #0]
    ldr pc, [pc, #0]
    .word 0
    .word 0
.size yuki_caller_stub_template, . - yuki_caller_stub_template

/* Thumb, as the runtime's own code is: the stub's literal holds the entry's address with the Thumb
 * bit, and the C++ function is reached with b.w, with no change of state. */
.thumb
.macro caller_entry name, target, argument
.hidden \name
.global \name
.type \name, %function
.thumb_func
.p2align 2
\name:
    mov \argument, ip
    b.w \target
.size \name, . - \name
.endm

caller_entry yuki_caller_dlopen, yuki_caller_dlopen_from, r2
caller_entry yuki_caller_android_dlopen_ext, yuki_caller_android_dlopen_ext_from, r3
caller_entry yuki_caller_dlsym, yuki_caller_dlsym_from, r2
caller_entry yuki_caller_dlvsym, yuki_caller_dlvsym_from, r3
caller_entry yuki_caller_dl_iterate_phdr, yuki_caller_dl_iterate_phdr_from, r2

#endif  // YUKILINKER_FULL
