//! Feature ids shared by the feature manager and boot-patch (which also runs on the host).

#![cfg_attr(not(target_os = "android"), allow(dead_code))]

use anyhow::{Result, bail};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum FeatureId {
    SuCompat = 0,
    KernelUmount = 1,
    Sulog = 2,
    AdbRoot = 3,
    SelinuxHide = 4,
    // 5 was webview_zygote_umount (v3.3.0), keep it unused so stale configs don't apply
    ShellSuAdbOnly = 6,
    // 103 起为下游扩展编号 (与 YukiSU 保持一致, 0-99 留给上游 KernelSU)
    Yukizygisk = 103,
}

impl FeatureId {
    pub const ALL: [Self; 7] = [
        Self::SuCompat,
        Self::KernelUmount,
        Self::Sulog,
        Self::AdbRoot,
        Self::SelinuxHide,
        Self::ShellSuAdbOnly,
        Self::Yukizygisk,
    ];

    pub const fn from_u32(id: u32) -> Option<Self> {
        match id {
            0 => Some(Self::SuCompat),
            1 => Some(Self::KernelUmount),
            2 => Some(Self::Sulog),
            3 => Some(Self::AdbRoot),
            4 => Some(Self::SelinuxHide),
            6 => Some(Self::ShellSuAdbOnly),
            103 => Some(Self::Yukizygisk),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::SuCompat => "su_compat",
            Self::KernelUmount => "kernel_umount",
            Self::Sulog => "sulog",
            Self::AdbRoot => "adb_root",
            Self::SelinuxHide => "selinux_hide",
            Self::ShellSuAdbOnly => "shell_su_adb_only",
            Self::Yukizygisk => "yukizygisk",
        }
    }

    pub const fn description(self) -> &'static str {
        match self {
            Self::SuCompat => {
                "SU Compatibility Mode - allows authorized apps to gain root via traditional 'su' command"
            }
            Self::KernelUmount => {
                "Kernel Umount - controls whether kernel automatically unmounts modules when not needed"
            }
            Self::Sulog => {
                "SU Log - streams kernel sulog events to userspace and persists them to disk"
            }
            Self::AdbRoot => "ADB Root - Enable adbd root",
            Self::SelinuxHide => {
                "SELinux Hide - sanitize /sys/fs/selinux access results for app UIDs"
            }
            Self::ShellSuAdbOnly => {
                "Shell SU ADB Only - when shell is granted root, additionally require the process to be spawned by adbd or a KernelSU root process (ksu domain)"
            }
            Self::Yukizygisk => {
                "YukiZygisk - built-in kernel level Zygisk (zygote/native injection, module image policy)"
            }
        }
    }
}

pub fn parse_feature_id(name: &str) -> Result<FeatureId> {
    match name {
        "su_compat" | "0" => Ok(FeatureId::SuCompat),
        "kernel_umount" | "1" => Ok(FeatureId::KernelUmount),
        "sulog" | "2" => Ok(FeatureId::Sulog),
        "adb_root" | "3" => Ok(FeatureId::AdbRoot),
        "selinux_hide" | "4" => Ok(FeatureId::SelinuxHide),
        "shell_su_adb_only" | "6" => Ok(FeatureId::ShellSuAdbOnly),
        "yukizygisk" | "103" => Ok(FeatureId::Yukizygisk),
        _ => bail!("Unknown feature: {name}"),
    }
}
