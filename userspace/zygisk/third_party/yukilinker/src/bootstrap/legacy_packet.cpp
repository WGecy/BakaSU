// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rouyashiki <https://rouyashiki.org>

// Packet layout and transfer semantics derive from YukiZygisk 509417ca,
// userspace/zygisk/endpoint.cpp (Apache-2.0, Rouyashiki <https://rouyashiki.org>).
#include "legacy_packet.hpp"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
namespace yukilinker::bootstrap {
namespace {
bool read_at(int fd, void* data, size_t size, off_t offset) {
  ssize_t n;
  do {
    n = pread(fd, data, size, offset);
  } while (n < 0 && errno == EINTR);
  return n == static_cast<ssize_t>(size);
}
}  // namespace
bool Descriptor::capture(int value) {
  struct stat status{};
  if (value < 0 || fstat(value, &status) != 0) return false;
  fd = value;
  device = status.st_dev;
  inode = status.st_ino;
  return true;
}
bool Descriptor::close_owned() {
  struct stat status{};
  if (fd < 0) return true;
  if (fstat(fd, &status) != 0) {
    if (errno != EBADF) return false;
    fd = -1;
    return true;
  }
  // Another owner has replaced this number. Never close the replacement.
  const bool same = device == static_cast<uint64_t>(status.st_dev) &&
                    inode == static_cast<uint64_t>(status.st_ino);
  const bool closed = !same || close(fd) == 0;
  // close errors are not retried by number: Linux may already have consumed it.
  fd = -1;
  return closed;
}
bool Packet::capture(int fd, int core_fd) {
  process = getpid();
  if (fd < 0) return true;
  if (fd == core_fd || !descriptors[0].capture(fd)) return false;
  count = 1;
  PacketHeader header{};
  if (!read_at(fd, &header, sizeof(header), 0) || header.magic != YUKILINKER_C_EARLY_PACKET_MAGIC ||
      header.version != YUKILINKER_C_EARLY_PACKET_VERSION || header.header_size != sizeof(header) ||
      header.entry_size != sizeof(PacketEntry) ||
      header.count > YUKILINKER_C_EARLY_PACKET_MAX_ENTRIES || header.endpoint_fd < 0 ||
      header.reserved)
    return false;
  const auto add = [&](int value) {
    if (value < 0 || value == core_fd) return false;
    for (size_t i = 0; i < count; ++i)
      if (descriptors[i].fd == value) return false;
    if (!descriptors[count].capture(value)) return false;
    ++count;
    return true;
  };
  if (!add(header.endpoint_fd)) return false;
  for (uint32_t i = 0; i < header.count; ++i) {
    PacketEntry entry{};
    if (!read_at(fd, &entry, sizeof(entry), sizeof(header) + off_t(i) * sizeof(entry)) ||
        !add(entry.fd))
      return false;
  }
  return true;
}
bool Packet::close_owned() {
  if (process != getpid()) return false;  // never clean another process's in-flight ownership
  bool closed = true;
  while (count != 0) closed = descriptors[--count].close_owned() && closed;
  return closed;
}
}  // namespace yukilinker::bootstrap
