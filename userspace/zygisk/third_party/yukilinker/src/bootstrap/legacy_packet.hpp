// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "yukilinker.h"
namespace yukilinker::bootstrap {
// Snapshot of YukiZygisk 509417ca, kernel/uapi/yukizygisk.h (Apache-2.0,
// Rouyashiki <https://rouyashiki.org>). Only the frozen bootstrap packet v3 transport is needed
// here.
using PacketHeader = yukilinker_c_early_packet_header;
using PacketEntry = yukilinker_c_early_packet_entry;
static_assert(sizeof(PacketHeader) == 24);
static_assert(sizeof(PacketEntry) == 844);
struct Descriptor {
  int fd = -1;
  uint64_t device = 0, inode = 0;
  bool capture(int value);
  bool close_owned();
};
struct Packet {
  Descriptor descriptors[66];
  size_t count = 0;
  pid_t process = 0;
  bool capture(int fd, int core_fd);
  bool close_owned();
};
}  // namespace yukilinker::bootstrap
