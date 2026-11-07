#pragma once

// YukiZygisk daemon-side constants.
//
// The C++ daemon is built outside ksud (which is Rust in this fork), so the
// handful of paths and kernel entry points it needs are declared here instead
// of being pulled from ksud's own header. Values mirror defs.rs so both sides
// always agree.

#include <cstdint>
#include <string>

extern "C" {
#include "uapi/feature.h"
}
#include "uapi/supercall.h"
#include "uapi/yukizygisk.h"

namespace ksud {

// Deployed payload directory: the kernel reads the first-stage and core
// libraries from here as ksu_cred and hands them to target processes through
// memfd, so target processes never open these paths directly.
constexpr const char *YUKIZYGISK_DIR = "/data/adb/ksu/lib/yukizygisk/";
constexpr const char *ZCORE64_PATH = "/data/adb/ksu/lib/yukizygisk/libzygisk64.so";
constexpr const char *ZCORE32_PATH = "/data/adb/ksu/lib/yukizygisk/libzygisk32.so";
constexpr const char *ZNCORE64_PATH = "/data/adb/ksu/lib/yukizygisk/libyukizncore64.so";
constexpr const char *ZNCORE32_PATH = "/data/adb/ksu/lib/yukizygisk/libyukizncore32.so";
// Independent SDK bootstrap; each Core embeds its own private runtime.
constexpr const char *ZYLOADER64_PATH = "/data/adb/ksu/lib/yukizygisk/libyloader64.so";
constexpr const char *ZYLOADER32_PATH = "/data/adb/ksu/lib/yukizygisk/libyloader32.so";
constexpr const char *ZYGISKD64_PATH = "/data/adb/ksu/bin/zygiskd64";
constexpr const char *ZYGISKD32_PATH = "/data/adb/ksu/bin/zygiskd32";

// Runtime state stays separate from deployed payloads so updates cannot
// replace user configuration or diagnostic logs.
constexpr const char *YUKIZYGISK_STATE_DIR = "/data/adb/ksu/yukizygisk";
constexpr const char *YUKIZYGISK_CONFIG_PATH =
    "/data/adb/ksu/yukizygisk/yzconfig.json";
constexpr const char *YUKIZYGISK_DIAGNOSTICS_DIR =
    "/data/adb/ksu/yukizygisk/diagnostics";
constexpr const char *YUKIZYGISK_CURRENT_DIAGNOSTICS_DIR =
    "/data/adb/ksu/yukizygisk/diagnostics/current";
constexpr const char *YUKIZYGISK_OLD_DIAGNOSTICS_DIR =
    "/data/adb/ksu/yukizygisk/diagnostics/old";
constexpr const char *YUKIZYGISK_LOG_DIR =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs";
constexpr const char *YUKIZYGISK_LOG64_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd64.log";
constexpr const char *YUKIZYGISK_ROLLED_LOG64_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd64.1.log";
constexpr const char *YUKIZYGISK_LOG32_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd32.log";
constexpr const char *YUKIZYGISK_ROLLED_LOG32_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/logs/zygiskd32.1.log";
constexpr const char *YUKIZYGISK_LINKER64_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/linker64.json";
constexpr const char *YUKIZYGISK_LINKER32_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/linker32.json";
constexpr const char *YUKIZYGISK_DIAGNOSTIC_EVIDENCE_PATH =
    "/data/adb/ksu/yukizygisk/diagnostics/current/evidence";
constexpr const char *YUKIZYGISK_LEGACY_LOG_DIR = "/data/adb/ksu/yukizygisk/log";

// Shared ksud paths used by the SELinux helper below.
constexpr const char *DAEMON_PATH = "/data/adb/ksud";
constexpr const char *MODULE_DIR = "/data/adb/modules/";

// Kernel entry point. Implemented by the daemon binary (standalone.cpp),
// because the driver fd is resolved differently per launch path.
int ksuctl(int request, void *arg);

} // namespace ksud
