// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#ifndef YUKILINKER_FULL
#define YUKILINKER_FULL 0
#endif

#if !defined(__aarch64__)
#error "caller_stub.S is the arm64 caller stub"
#endif

#if YUKILINKER_FULL

/*
 * The caller-dependent imports of an image in a managed namespace bind to a
 * slab of per-image stubs (caller_stubs.cpp). Each stub is the template below with its two literals
 * patched: it loads the binding-cell address into x16 and branches to one of the entries, which pass
 * it on as the last argument of the C++ function that reads the generation under the operation
 * lock. x16 and x17 are the intra-procedure scratch registers a PLT clobbers anyway; x30 is the
 * import's own return.
 */
.text
.p2align 3
.hidden yuki_caller_stub_template
.global yuki_caller_stub_template
.type yuki_caller_stub_template, %object
yuki_caller_stub_template:
    hint #34
    ldr x16, 1f
    ldr x17, 2f
    br x17
    .p2align 3
1:  .quad 0
2:  .quad 0
.size yuki_caller_stub_template, . - yuki_caller_stub_template

/* The runtime is BTI-built: each entry is reached by br x17, so it starts with a landing pad. */
.macro caller_entry name, target, argument
.hidden \name
.global \name
.type \name, %function
.p2align 2
\name:
    hint #34
    mov \argument, x16
    b \target
.size \name, . - \name
.endm

caller_entry yuki_caller_dlopen, yuki_caller_dlopen_from, x2
caller_entry yuki_caller_android_dlopen_ext, yuki_caller_android_dlopen_ext_from, x3
caller_entry yuki_caller_dlsym, yuki_caller_dlsym_from, x2
caller_entry yuki_caller_dlvsym, yuki_caller_dlvsym_from, x3
caller_entry yuki_caller_dl_iterate_phdr, yuki_caller_dl_iterate_phdr_from, x2

#endif  // YUKILINKER_FULL

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
