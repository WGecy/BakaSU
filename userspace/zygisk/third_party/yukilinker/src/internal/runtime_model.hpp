// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once

// Umbrella for the private headers. It keeps the system headers it has always included, so a
// translation unit that includes only this still compiles.

#include <android/dlext.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#if defined(__aarch64__)
#include <sys/ifunc.h>
#endif
#include <unistd.h>

#include "../../include/yukilinker.h"
#include "address_space.hpp"
#include "call_barrier.hpp"
#include "checked_math.hpp"
#include "config.hpp"
#include "context.hpp"
#include "elf_model.hpp"
#include "elf_reader.hpp"
#include "event_log.hpp"
#include "host.hpp"
#include "image_list.hpp"
#include "image_state.hpp"
#include "lifecycle_model.hpp"
#include "loader_pipeline.hpp"
#include "loader_types.hpp"
#include "metadata_arena.hpp"
#include "operation_error.hpp"
#include "operation_guards.hpp"
#include "retained_source.hpp"
#include "runtime_owner.hpp"
#include "tls_model.hpp"
