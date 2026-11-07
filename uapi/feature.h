#ifndef __KSU_UAPI_FEATURE_H
#define __KSU_UAPI_FEATURE_H

enum ksu_feature_id {
    KSU_FEATURE_SU_COMPAT = 0,
    KSU_FEATURE_KERNEL_UMOUNT = 1,
    KSU_FEATURE_SULOG = 2,
    KSU_FEATURE_ADB_ROOT = 3,
    KSU_FEATURE_SELINUX_HIDE = 4,
    // 5 was webview_zygote_umount (v3.3.0), keep it unused so stale configs don't apply
    KSU_FEATURE_SHELL_SU_ADB_ONLY = 6,

    // Downstream extensions number from 100 up, keeping 0-99 for upstream
    // KernelSU so the two ABI families never collide. The numeric values must
    // match YukiSU exactly: userspace (zygiskd, ksud, the manager) toggles the
    // feature by id, and prebuilt payloads are shared.
    KSU_FEATURE_YUKIZYGISK = 103,

    KSU_FEATURE_MAX
};

#endif
