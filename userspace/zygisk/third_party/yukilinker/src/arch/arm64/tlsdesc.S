/*
 * Copyright (C) 2019 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef YUKILINKER_FULL
#define YUKILINKER_FULL 0
#endif

#if !defined(__aarch64__)
#error "YukiLinker supports arm64 only"
#endif

#if YUKILINKER_FULL

/* TLSDESC preserves the argument and vector registers across the C slow path. */
.text
.p2align 2
.hidden yuki_tlsdesc_dynamic_entry
.global yuki_tlsdesc_dynamic_entry
.type yuki_tlsdesc_dynamic_entry, %function
yuki_tlsdesc_dynamic_entry:
.cfi_startproc
    hint #34
    sub sp, sp, #672
    .cfi_def_cfa_offset 672
    stp x29, x30, [sp, #0]
    .cfi_rel_offset x29, 0
    .cfi_rel_offset x30, 8
    mov x29, sp

    stp x1,  x2,  [sp, #16]
    .cfi_rel_offset x1, 16
    .cfi_rel_offset x2, 24
    stp x3,  x4,  [sp, #32]
    .cfi_rel_offset x3, 32
    .cfi_rel_offset x4, 40
    stp x5,  x6,  [sp, #48]
    .cfi_rel_offset x5, 48
    .cfi_rel_offset x6, 56
    stp x7,  x8,  [sp, #64]
    .cfi_rel_offset x7, 64
    .cfi_rel_offset x8, 72
    stp x9,  x10, [sp, #80]
    .cfi_rel_offset x9, 80
    .cfi_rel_offset x10, 88
    stp x11, x12, [sp, #96]
    .cfi_rel_offset x11, 96
    .cfi_rel_offset x12, 104
    stp x13, x14, [sp, #112]
    .cfi_rel_offset x13, 112
    .cfi_rel_offset x14, 120
    stp x15, x16, [sp, #128]
    .cfi_rel_offset x15, 128
    .cfi_rel_offset x16, 136
    str x17,      [sp, #144]
    .cfi_rel_offset x17, 144

    stp q0,  q1,  [sp, #160]
    .cfi_rel_offset q0, 160
    .cfi_rel_offset q1, 176
    stp q2,  q3,  [sp, #192]
    .cfi_rel_offset q2, 192
    .cfi_rel_offset q3, 208
    stp q4,  q5,  [sp, #224]
    .cfi_rel_offset q4, 224
    .cfi_rel_offset q5, 240
    stp q6,  q7,  [sp, #256]
    .cfi_rel_offset q6, 256
    .cfi_rel_offset q7, 272
    stp q8,  q9,  [sp, #288]
    .cfi_rel_offset q8, 288
    .cfi_rel_offset q9, 304
    stp q10, q11, [sp, #320]
    .cfi_rel_offset q10, 320
    .cfi_rel_offset q11, 336
    stp q12, q13, [sp, #352]
    .cfi_rel_offset q12, 352
    .cfi_rel_offset q13, 368
    stp q14, q15, [sp, #384]
    .cfi_rel_offset q14, 384
    .cfi_rel_offset q15, 400
    stp q16, q17, [sp, #416]
    .cfi_rel_offset q16, 416
    .cfi_rel_offset q17, 432
    stp q18, q19, [sp, #448]
    .cfi_rel_offset q18, 448
    .cfi_rel_offset q19, 464
    stp q20, q21, [sp, #480]
    .cfi_rel_offset q20, 480
    .cfi_rel_offset q21, 496
    stp q22, q23, [sp, #512]
    .cfi_rel_offset q22, 512
    .cfi_rel_offset q23, 528
    stp q24, q25, [sp, #544]
    .cfi_rel_offset q24, 544
    .cfi_rel_offset q25, 560
    stp q26, q27, [sp, #576]
    .cfi_rel_offset q26, 576
    .cfi_rel_offset q27, 592
    stp q28, q29, [sp, #608]
    .cfi_rel_offset q28, 608
    .cfi_rel_offset q29, 624
    stp q30, q31, [sp, #640]
    .cfi_rel_offset q30, 640
    .cfi_rel_offset q31, 656

    bl yuki_tlsdesc_dynamic_body

    ldp q30, q31, [sp, #640]
    ldp q28, q29, [sp, #608]
    ldp q26, q27, [sp, #576]
    ldp q24, q25, [sp, #544]
    ldp q22, q23, [sp, #512]
    ldp q20, q21, [sp, #480]
    ldp q18, q19, [sp, #448]
    ldp q16, q17, [sp, #416]
    ldp q14, q15, [sp, #384]
    ldp q12, q13, [sp, #352]
    ldp q10, q11, [sp, #320]
    ldp q8,  q9,  [sp, #288]
    ldp q6,  q7,  [sp, #256]
    ldp q4,  q5,  [sp, #224]
    ldp q2,  q3,  [sp, #192]
    ldp q0,  q1,  [sp, #160]

    ldr x17,      [sp, #144]
    ldp x15, x16, [sp, #128]
    ldp x13, x14, [sp, #112]
    ldp x11, x12, [sp, #96]
    ldp x9,  x10, [sp, #80]
    ldp x7,  x8,  [sp, #64]
    ldp x5,  x6,  [sp, #48]
    ldp x3,  x4,  [sp, #32]
    ldp x1,  x2,  [sp, #16]
    ldp x29, x30, [sp, #0]
    add sp, sp, #672
    .cfi_def_cfa_offset 0
    .cfi_restore x1
    .cfi_restore x2
    .cfi_restore x3
    .cfi_restore x4
    .cfi_restore x5
    .cfi_restore x6
    .cfi_restore x7
    .cfi_restore x8
    .cfi_restore x9
    .cfi_restore x10
    .cfi_restore x11
    .cfi_restore x12
    .cfi_restore x13
    .cfi_restore x14
    .cfi_restore x15
    .cfi_restore x16
    .cfi_restore x17
    .cfi_restore x29
    .cfi_restore x30
    .cfi_restore q0
    .cfi_restore q1
    .cfi_restore q2
    .cfi_restore q3
    .cfi_restore q4
    .cfi_restore q5
    .cfi_restore q6
    .cfi_restore q7
    .cfi_restore q8
    .cfi_restore q9
    .cfi_restore q10
    .cfi_restore q11
    .cfi_restore q12
    .cfi_restore q13
    .cfi_restore q14
    .cfi_restore q15
    .cfi_restore q16
    .cfi_restore q17
    .cfi_restore q18
    .cfi_restore q19
    .cfi_restore q20
    .cfi_restore q21
    .cfi_restore q22
    .cfi_restore q23
    .cfi_restore q24
    .cfi_restore q25
    .cfi_restore q26
    .cfi_restore q27
    .cfi_restore q28
    .cfi_restore q29
    .cfi_restore q30
    .cfi_restore q31
    ret
.cfi_endproc
.size yuki_tlsdesc_dynamic_entry, .-yuki_tlsdesc_dynamic_entry

.p2align 2
.hidden yuki_tlsdesc_weak_entry
.global yuki_tlsdesc_weak_entry
.type yuki_tlsdesc_weak_entry, %function
yuki_tlsdesc_weak_entry:
.cfi_startproc
    hint #34
    str x1, [sp, #-16]!
    .cfi_def_cfa_offset 16
    .cfi_rel_offset x1, 0
    ldr x1, [x0, #8]
    mrs x0, tpidr_el0
    sub x0, x1, x0
    ldr x1, [sp], #16
    .cfi_def_cfa_offset 0
    .cfi_restore x1
    ret
.cfi_endproc
.size yuki_tlsdesc_weak_entry, .-yuki_tlsdesc_weak_entry

#endif

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
