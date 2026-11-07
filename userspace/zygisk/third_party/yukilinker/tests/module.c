// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

static int value;
__attribute__((constructor)) static void initialize(void) { value = 42; }
__attribute__((visibility("default"))) int module_answer(void) { return value; }
