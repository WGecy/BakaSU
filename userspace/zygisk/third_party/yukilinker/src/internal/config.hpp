// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

// Every private header that tests YUKILINKER_FULL includes this before the first test, so no
// translation unit sees a struct laid out with a different value.
#ifndef YUKILINKER_FULL
#define YUKILINKER_FULL 1
#endif
