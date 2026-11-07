//! YukiZygisk 内核通信客户端 (移植自 Rouyashiki/YukiZygisk)
//!
//! 与 nomount 相同，数据面完全下沉到内核模块 (kernel/feature/yukizygisk/)，
//! userspace 只负责内核做不了的三件事：
//! 1. 把内置 payload 落到 /data/adb/ksu/lib/yukizygisk/；
//! 2. 在 post-fs-data 阶段生成 early-native 快照，供 preinit 阶段加载；
//! 3. 拉起 per-ABI 的 zygiskd 守护进程。
//!
//! 全部 ABI 定义见 uapi/yukizygisk.h。

pub mod cli;
pub mod config;
pub mod daemon;
pub mod snapshot;

use std::os::unix::fs::PermissionsExt;
use std::ptr;

use anyhow::Result;

use crate::{
    android::{ksucalls, restorecon, uapi, utils},
    assets, defs,
};

/// uapi/feature.h: KSU_FEATURE_YUKIZYGISK
pub const FEATURE_ID: u32 = 103;

/// uapi/yukizygisk.h: YZ_RUNTIME_ABI_32
pub const ABI_32: u32 = 1;
/// uapi/yukizygisk.h: YZ_RUNTIME_ABI_64
pub const ABI_64: u32 = 2;

/// uapi/yukizygisk.h: YZ_RUNTIME_CAP_ENDPOINT_STATE
pub const CAP_ENDPOINT_STATE: u32 = 1 << 5;
/// uapi/yukizygisk.h: YZ_RUNTIME_CAP_NATIVE_LIBC_BOOTSTRAP_V1
pub const CAP_NATIVE_LIBC_BOOTSTRAP_V1: u32 = 1 << 6;
/// zygiskd 启动要求内核至少具备这两个能力位。
pub const REQUIRED_CAPABILITIES: u32 = CAP_ENDPOINT_STATE | CAP_NATIVE_LIBC_BOOTSTRAP_V1;

/// uapi/yukizygisk.h: YZ_LOAD_CONFIG_VALID
pub const LOAD_CONFIG_VALID: u16 = 1 << 0;
/// uapi/yukizygisk.h: YZ_LOAD_YUKILINKER
pub const LOAD_CONFIG_YUKILINKER: u16 = 1 << 1;
/// uapi/yukizygisk.h: YZ_LOAD_ANONYMOUS
pub const LOAD_CONFIG_ANONYMOUS: u16 = 1 << 2;

/// uapi/yukizygisk.h: YZ_ENDPOINT_VERSION
const ENDPOINT_VERSION: u32 = 1;

/// uapi/yukizygisk.h: YZ_EARLY_NATIVE_MAGIC
pub const SNAPSHOT_MAGIC: u32 = 0x5945_4e5a;
/// uapi/yukizygisk.h: YZ_EARLY_NATIVE_VERSION
pub const SNAPSHOT_VERSION: u16 = 3;
/// uapi/yukizygisk.h: YZ_EARLY_NATIVE_FLAG_ENABLED
pub const SNAPSHOT_FLAG_ENABLED: u32 = 1 << 0;
/// uapi/yukizygisk.h: YZ_EARLY_NATIVE_FLAG_ABI32
pub const SNAPSHOT_FLAG_ABI32: u32 = 1 << 1;
/// uapi/yukizygisk.h: YZ_EARLY_NATIVE_FLAG_ABI64
pub const SNAPSHOT_FLAG_ABI64: u32 = 1 << 2;
/// uapi/yukizygisk.h: YZ_EARLY_NATIVE_ENTRY_ABI32
pub const ENTRY_ABI32: u8 = 1 << 0;
/// uapi/yukizygisk.h: YZ_EARLY_NATIVE_ENTRY_ABI64
pub const ENTRY_ABI64: u8 = 1 << 1;

/// uapi/yukizygisk.h: YZ_NATIVE_TARGET_NAME
pub const NATIVE_TARGET_NAME: u8 = 1;
/// uapi/yukizygisk.h: YZ_NATIVE_TARGET_PATH
pub const NATIVE_TARGET_PATH: u8 = 2;

/// uapi/yukizygisk.h: YZ_NATIVE_TARGET_MAX
pub const NATIVE_TARGET_MAX: usize = 64;
/// uapi/yukizygisk.h: YZ_NATIVE_TARGET_VALUE_MAX
pub const NATIVE_TARGET_VALUE_MAX: usize = 256;
/// uapi/yukizygisk.h: YZ_NATIVE_MODULE_ID_MAX
pub const NATIVE_MODULE_ID_MAX: usize = 64;
/// uapi/yukizygisk.h: YZ_NATIVE_MODULE_PATH_MAX
pub const NATIVE_MODULE_PATH_MAX: usize = 512;

/// uapi/yukizygisk.h: YZ_RUNTIME_RECORD_MAX
pub const RUNTIME_RECORD_MAX: usize = 128;

/// sizeof(struct yz_early_native_snapshot_header)，须与内核校验一致 (72)。
pub const SNAPSHOT_HEADER_SIZE: usize = 72;
/// sizeof(struct yz_early_native_entry)，须与内核校验一致 (836)。
pub const SNAPSHOT_ENTRY_SIZE: usize = 836;

/// 内核 early-native 阶段直接 load 的 payload (不含 zygiskd 本身)。
pub const PAYLOAD_FILES: [&str; 6] = [
    "libyloader64.so",
    "libyloader32.so",
    "libzygisk64.so",
    "libzygisk32.so",
    "libyukizncore64.so",
    "libyukizncore32.so",
];

/// early 快照目录内需要与 lib 目录保持一致的 payload。
pub const SNAPSHOT_PAYLOAD_FILES: [&str; 4] = [
    "libyloader64.so",
    "libyloader32.so",
    "libyukizncore64.so",
    "libyukizncore32.so",
];

/// 旧版遗留的 payload 名称，升级时清理。
const LEGACY_PAYLOAD_FILES: [&str; 5] = [
    "libzygisk.so",
    "libyukizncore.so",
    "libyukilinker.so",
    "libyukilinker32.so",
    "libyukilinker64.so",
];

const _: () =
    assert!(std::mem::size_of::<uapi::yz_early_native_snapshot_header>() == SNAPSHOT_HEADER_SIZE);
const _: () = assert!(std::mem::size_of::<uapi::yz_early_native_entry>() == SNAPSHOT_ENTRY_SIZE);

/// 读取内核 feature 表中的 yukizygisk 状态。
pub fn feature_state() -> Option<ksucalls::FeatureState> {
    ksucalls::get_feature(FEATURE_ID).ok()
}

/// 内核是否提供该功能。
pub fn is_supported() -> bool {
    feature_state().is_some_and(|state| state.supported)
}

/// 功能是否已启用 (内核支持且开关打开)。
pub fn is_enabled() -> bool {
    feature_state().is_some_and(|state| state.supported && state.value != 0)
}

/// 写入内核 feature 开关，并按需准备或清理 userspace 侧资源。
pub fn set_enabled(enabled: bool) -> Result<()> {
    crate::android::feature::set_feature("yukizygisk", if enabled { 1 } else { 0 })?;
    if enabled {
        ensure_payload()?;
        push_config();
        if let Err(err) = snapshot::refresh() {
            log::warn!("yukizygisk early: snapshot refresh failed: {err:#}");
        }
        if let Err(err) = daemon::ensure(0) {
            log::warn!("yukizygisk daemon: ensure failed: {err:#}");
        }
    } else {
        if let Err(err) = reload() {
            log::warn!("yukizygisk: kernel reload after disable failed: {err:#}");
        }
        if let Err(err) = snapshot::clear() {
            log::warn!("yukizygisk early: snapshot clear failed: {err:#}");
        }
    }
    Ok(())
}

/// 把内置 payload 解到 /data/adb/ksu/lib/yukizygisk/。
///
/// 返回实际落盘的文件数；未嵌入任何 payload 资产时返回 0 (例如 x86_64 构建)，
/// 此时内核会打印 staging failed 并静默跳过注入，不影响其他功能。
pub fn ensure_payload() -> Result<usize> {
    utils::ensure_dir_exists(defs::YUKIZYGISK_LIB_DIR)?;
    let mut staged = 0usize;
    for name in PAYLOAD_FILES {
        let Ok(data) = assets::get_asset(name) else {
            continue;
        };
        let path = format!("{}{}", defs::YUKIZYGISK_LIB_DIR, name);
        std::fs::write(&path, &data)?;
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644))?;
        if let Err(err) = restorecon::lsetfilecon(&path, restorecon::SYSTEM_LIB_CON) {
            log::warn!("yukizygisk: restorecon {} failed: {err:#}", path);
        }
        staged += 1;
    }
    for name in LEGACY_PAYLOAD_FILES {
        let _ = std::fs::remove_file(format!("{}{}", defs::YUKIZYGISK_LIB_DIR, name));
    }
    if staged == 0 {
        log::warn!("yukizygisk: no payload asset embedded, kernel injection skipped");
    }
    Ok(staged)
}

/// post-fs-data 阶段的统一入口：payload + 快照 + 守护进程。
pub fn ensure_ready() -> Result<()> {
    if !is_enabled() {
        return Ok(());
    }
    if utils::is_safe_mode() {
        log::info!("yukizygisk: safe mode, skip payload and daemon");
        return Ok(());
    }
    ensure_payload()?;
    push_config();
    if let Err(err) = snapshot::refresh() {
        log::warn!("yukizygisk early: snapshot refresh failed: {err:#}");
    }
    if let Err(err) = daemon::ensure(0) {
        log::warn!("yukizygisk daemon: ensure failed: {err:#}");
    }
    Ok(())
}

/// 内核下发的运行时快照。
#[derive(Debug, Clone, Default)]
pub struct RuntimeSnapshot {
    pub generation: u32,
    pub safe_mode: u32,
    pub zygote_crashes: u32,
    pub capabilities: u32,
    pub safe_mode_zygote: String,
    pub records: Vec<RuntimeRecord>,
}

/// 单条内核运行时记录 (struct yz_runtime_record)。
#[derive(Debug, Clone, Default)]
pub struct RuntimeRecord {
    pub pid: u32,
    pub generation: u32,
    pub restarts: u32,
    pub kind: u8,
    pub state: u8,
    pub abi: u8,
    pub target_type: u8,
    pub flags: u32,
    pub process: String,
    pub target: String,
    pub module: String,
}

impl RuntimeRecord {
    /// 注入状态 (需要 YZ_RUNTIME_CAP_INJECTION_STATE)。
    pub fn injection_state(&self) -> u8 {
        ((self.flags & uapi::YZ_RUNTIME_F_INJECTION_STATE_MASK)
            >> uapi::YZ_RUNTIME_F_INJECTION_STATE_SHIFT) as u8
    }
}

/// 读取内核运行时表 (KSU_IOCTL_YZ_GET_RUNTIME)。
pub fn query_runtime() -> Result<RuntimeSnapshot> {
    let mut records: Vec<uapi::yz_runtime_record> =
        vec![unsafe { std::mem::zeroed() }; RUNTIME_RECORD_MAX];
    let mut cmd: uapi::yz_runtime_query_cmd = unsafe { std::mem::zeroed() };
    cmd.capacity = RUNTIME_RECORD_MAX as u32;
    cmd.entries = records.as_mut_ptr() as u64;
    ksucalls::ksuctl(uapi::KSU_IOCTL_YZ_GET_RUNTIME, &raw mut cmd)?;

    let count = (cmd.count as usize).min(RUNTIME_RECORD_MAX);
    let mut snapshot = RuntimeSnapshot {
        generation: cmd.generation,
        safe_mode: cmd.safe_mode,
        zygote_crashes: cmd.zygote_crashes,
        capabilities: cmd.capabilities,
        safe_mode_zygote: unsafe {
            cstr_to_string(
                cmd.safe_mode_zygote.as_ptr().cast(),
                cmd.safe_mode_zygote.len(),
            )
        },
        records: Vec::with_capacity(count),
    };
    for record in records.iter().take(count) {
        snapshot.records.push(RuntimeRecord {
            pid: record.pid,
            generation: record.generation,
            restarts: record.restarts,
            kind: record.kind,
            state: record.state,
            abi: record.abi,
            target_type: record.target_type,
            flags: record.flags,
            process: unsafe {
                cstr_to_string(record.process.as_ptr().cast(), record.process.len())
            },
            target: unsafe { cstr_to_string(record.target.as_ptr().cast(), record.target.len()) },
            module: unsafe {
                cstr_to_string(record.module_id.as_ptr().cast(), record.module_id.len())
            },
        });
    }
    Ok(snapshot)
}

/// 内核安全模式状态 (struct yz_safemode_status_cmd)。
#[derive(Debug, Clone, Default)]
pub struct SafeMode {
    pub active: bool,
    pub zygote_crashes: u32,
    pub zygote: String,
}

/// 读取内核侧安全模式状态 (KSU_IOCTL_YZ_GET_SAFEMODE)。
pub fn get_safemode() -> Result<SafeMode> {
    let mut cmd: uapi::yz_safemode_status_cmd = unsafe { std::mem::zeroed() };
    ksucalls::ksuctl(uapi::KSU_IOCTL_YZ_GET_SAFEMODE, &raw mut cmd)?;
    Ok(SafeMode {
        active: cmd.active != 0,
        zygote_crashes: cmd.zygote_crashes,
        zygote: unsafe { cstr_to_string(cmd.zygote.as_ptr().cast(), cmd.zygote.len()) },
    })
}

/// 单个 ABI 的 endpoint 发布状态 (struct yz_endpoint_state)。
#[derive(Debug, Clone, Default)]
pub struct Endpoint {
    pub available: bool,
    pub pid: u32,
    pub generation: u64,
    pub start_boottime: u64,
    pub name: String,
}

/// 通过 KSU_IOCTL_YZ_GET_ENDPOINT_STATE_FD 读取 zygiskd 的 endpoint 状态。
///
/// 内核把一页只读共享内存以 fd 交出，映射后按 seqlock 语义读取；
/// available != 0 即表示该 ABI 的 zygiskd 已注册监听。
pub fn endpoint_state(abi: u32) -> Result<Endpoint> {
    let mut cmd: uapi::yz_endpoint_fd_cmd = unsafe { std::mem::zeroed() };
    cmd.version = ENDPOINT_VERSION;
    cmd.abi = abi;
    cmd.fd = -1;
    ksucalls::ksuctl(uapi::KSU_IOCTL_YZ_GET_ENDPOINT_STATE_FD, &raw mut cmd)?;
    if cmd.fd < 0 {
        anyhow::bail!("zygiskd: endpoint state fd unavailable for abi {abi}");
    }
    let fd = cmd.fd;

    let state_size = std::mem::size_of::<uapi::yz_endpoint_state>();
    let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    let page = if page > 0 { page as usize } else { 4096 };
    let map_size = page.max(state_size);
    let addr = unsafe {
        libc::mmap(
            ptr::null_mut(),
            map_size,
            libc::PROT_READ,
            libc::MAP_SHARED,
            fd,
            0,
        )
    };
    unsafe { libc::close(fd) };
    if addr == libc::MAP_FAILED {
        anyhow::bail!(
            "zygiskd: mmap endpoint state failed: {}",
            std::io::Error::last_os_error()
        );
    }

    let base = addr.cast::<uapi::yz_endpoint_state>();
    let mut state = unsafe { ptr::read_volatile(base) };
    for _ in 0..8 {
        let next = unsafe { ptr::read_volatile(base) };
        if next.sequence == state.sequence && state.sequence % 2 == 0 {
            break;
        }
        state = next;
    }
    unsafe { libc::munmap(addr, map_size) };

    if state.version != ENDPOINT_VERSION || state.size as usize != state_size {
        anyhow::bail!(
            "zygiskd: unexpected endpoint state version={} size={}",
            state.version,
            state.size
        );
    }
    Ok(Endpoint {
        available: state.available != 0,
        pid: state.pid,
        generation: state.generation,
        start_boottime: state.start_boottime,
        name: unsafe { cstr_to_string(state.name.as_ptr().cast(), state.name.len()) },
    })
}

/// 通知内核重新扫描模块列表 (KSU_IOCTL_YZ_RELOAD)。
pub fn reload() -> Result<()> {
    ksucalls::ksuctl::<u8>(uapi::KSU_IOCTL_YZ_RELOAD, ptr::null_mut())?;
    Ok(())
}

/// 把 yzconfig.json 中与内核相关的开关下发给内核。
///
/// zygiskd 启动后也会下发一次，这里提前做是为了让 preinit 阶段的
/// early-native 加载在守护进程尚未就绪时就拿到正确策略。
pub fn push_config() {
    let config = config::load();
    if let Err(err) = set_yukilinker(config.yukilinker) {
        log::warn!("yukizygisk: push yukilinker flag failed: {err:#}");
    }
}

/// 设置是否启用 YukiLinker 首阶段加载器。
pub fn set_yukilinker(enabled: bool) -> Result<()> {
    let mut cmd: uapi::yz_yukilinker_cmd = unsafe { std::mem::zeroed() };
    cmd.enabled = u32::from(enabled);
    ksucalls::ksuctl(uapi::KSU_IOCTL_YZ_SET_YUKILINKER, &raw mut cmd)?;
    Ok(())
}

/// 读取定长 C 字符串缓冲区。
///
/// SAFETY: buf 必须指向至少 len 字节的可读内存。
pub(crate) unsafe fn cstr_to_string(buf: *const u8, len: usize) -> String {
    if buf.is_null() || len == 0 {
        return String::new();
    }
    let bytes = unsafe { std::slice::from_raw_parts(buf, len) };
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}
