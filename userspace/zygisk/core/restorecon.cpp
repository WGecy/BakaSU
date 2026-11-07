#include "restorecon.hpp"

#include "../defs.hpp"

#include <sys/xattr.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace ksud {

namespace {

constexpr const char *SELINUX_XATTR = "security.selinux";

void report(const char *what, const fs::path &path) {
  // The daemon's own logger is not available from this translation unit, and a
  // failure here is never fatal to injection; keep the trace on stderr.
  fprintf(stderr, "yukizygisk: %s: %s: %s\n", what, path.c_str(),
          strerror(errno));
}

} // namespace

bool lsetfilecon(const fs::path &path, const std::string &con) {
  const int ret =
      lsetxattr(path.c_str(), SELINUX_XATTR, con.c_str(), con.length() + 1, 0);
  if (ret != 0) {
    report("failed to set SELinux context", path);
    return false;
  }
  return true;
}

std::string lgetfilecon(const fs::path &path) {
  std::array<char, 256> buf{};
  const ssize_t len =
      lgetxattr(path.c_str(), SELINUX_XATTR, buf.data(), buf.size() - 1);
  if (len < 0)
    return "";
  buf[static_cast<size_t>(len)] = '\0';
  return {buf.data()};
}

bool setsyscon(const fs::path &path) { return lsetfilecon(path, SYSTEM_CON); }

bool restore_syscon(const fs::path &dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec))
    return !ec;

  for (auto it = fs::recursive_directory_iterator(dir, ec);
       it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
    if (!setsyscon(it->path()))
      report("failed to restore context", it->path());
  }
  if (ec) {
    fprintf(stderr, "yukizygisk: error walking %s: %s\n", dir.c_str(),
            ec.message().c_str());
    return false;
  }
  return true;
}

bool restore_syscon_if_unlabeled(const fs::path &dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec))
    return !ec;

  for (auto it = fs::recursive_directory_iterator(dir, ec);
       it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
    const std::string con = lgetfilecon(it->path());
    if (it->path() == DAEMON_PATH) {
      if (con != KSU_CON && !lsetfilecon(it->path(), KSU_CON))
        report("failed to migrate daemon context", it->path());
      continue;
    }
    if (con.empty() || con == UNLABEL_CON) {
      if (!lsetfilecon(it->path(), SYSTEM_CON))
        report("failed to restore context", it->path());
    }
  }
  if (ec) {
    fprintf(stderr, "yukizygisk: error walking %s: %s\n", dir.c_str(),
            ec.message().c_str());
    return false;
  }
  return true;
}

bool restorecon() {
  bool success = true;

  if (!lsetfilecon(DAEMON_PATH, KSU_CON)) {
    report("failed to set context for daemon", DAEMON_PATH);
    success = false;
  }

  if (!restore_syscon_if_unlabeled(MODULE_DIR)) {
    report("failed to restore contexts for module directory", MODULE_DIR);
    success = false;
  }

  return success;
}

bool restorecon(const fs::path &path, bool recursive) {
  std::error_code ec;
  if (!fs::exists(path, ec) || ec)
    return false;

  const char *context = SYSTEM_CON;
  if (path == DAEMON_PATH)
    context = KSU_CON;
  else if (path == "/data/adb")
    context = ADB_CON;

  if (recursive) {
    const bool directory = fs::is_directory(path, ec);
    if (ec)
      return false;
    if (directory)
      return restore_syscon_if_unlabeled(path);
  }
  return lsetfilecon(path, context);
}

} // namespace ksud
