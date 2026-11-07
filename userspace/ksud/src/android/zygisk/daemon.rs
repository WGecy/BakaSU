//! zygiskd 守护进程管理 (对齐 YukiSU yukizygisk_daemon.cpp)

use std::ffi::CString;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;

use anyhow::{Context, Result, bail};
use const_format::concatcp;

use super::{ABI_32, ABI_64, snapshot};
use crate::{android::utils, defs};

/// zygiskd{64,32}：由 CI 打进 ksud 资产的独立可执行文件。
const ZYGISKD64_PATH: &str = concatcp!(defs::BINARY_DIR, "zygiskd64");
const ZYGISKD32_PATH: &str = concatcp!(defs::BINARY_DIR, "zygiskd32");

/// zygiskd 通过 ready 管道回传的单字节状态。
const READY_OK: u8 = b'1';
const READY_FAILED: u8 = b'0';
const READY_REBOOT: u8 = b'R';

/// 等待守护进程就绪的上限（毫秒）。
const SPAWN_TIMEOUT_MS: libc::c_int = 5000;

/// 串行化并发拉起（post-fs-data 与 feature 回调可能同时触发）。
struct OperationLock(libc::c_int);

impl Drop for OperationLock {
    fn drop(&mut self) {
        unsafe {
            libc::close(self.0);
        }
    }
}

fn lock_operation() -> Result<OperationLock> {
    let dir = defs::YUKIZYGISK_RECOVERY_DIR;
    utils::ensure_dir_exists(dir)?;
    let _ = std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700));

    let path = CString::new(format!("{dir}ensure.lock"))?;
    let mode = 0o600 as libc::mode_t;
    let flags =
        libc::O_RDWR | libc::O_CREAT | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK;
    // Android 的 32 位目标上 mode_t 是 u16，而 open() 的变参必须是 c_uint。
    let fd = unsafe { libc::open(path.as_ptr(), flags, libc::c_uint::from(mode)) };
    if fd < 0 {
        bail!(
            "yukizygisk daemon: open ensure.lock failed: {}",
            std::io::Error::last_os_error()
        );
    }
    let lock = OperationLock(fd);

    let mut stat: libc::stat = unsafe { std::mem::zeroed() };
    if unsafe { libc::fstat(fd, &raw mut stat) } != 0 {
        bail!(
            "yukizygisk daemon: stat ensure.lock failed: {}",
            std::io::Error::last_os_error()
        );
    }
    // S_IFMT/S_IFREG 的类型是 mode_t (32 位目标上是 u16)，而 st_mode 恒为 u32，
    // 所以这里用等值的字面量，避免在两个 ABI 上各写一套类型转换。
    const S_IFMT_BITS: u32 = 0o170000;
    const S_IFREG_BITS: u32 = 0o100000;
    if stat.st_mode & S_IFMT_BITS != S_IFREG_BITS || stat.st_uid != 0 || stat.st_nlink != 1 {
        bail!("yukizygisk daemon: ensure.lock is not a root-owned single-link regular file");
    }
    if unsafe { libc::flock(fd, libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        bail!(
            "yukizygisk daemon: another ensure operation is in progress: {}",
            std::io::Error::last_os_error()
        );
    }
    Ok(lock)
}

fn is_executable(path: &Path) -> bool {
    std::fs::metadata(path)
        .is_ok_and(|meta| meta.is_file() && meta.permissions().mode() & 0o111 != 0)
}

/// 是否需要 32 位守护进程。
fn needs_daemon32() -> bool {
    let app32 = Path::new("/system/bin/app_process32");
    if Path::new("/system_ext/bin/tango_translator").exists() && is_executable(app32) {
        return true;
    }
    if let Some(value) = utils::getprop("ro.zygote") {
        if value.contains("32") {
            return true;
        }
    } else if is_executable(app32) {
        return true;
    }
    snapshot::has_native_abi32_target()
}

/// 拉起单个 ABI 的 zygiskd，等待其通过 ready 管道确认。
fn spawn_daemon(path: &str) -> Result<()> {
    let c_path = CString::new(path)?;
    let c_key = CString::new("YUKIZYGISK_READY_FD")?;

    let mut fds = [0 as libc::c_int; 2];
    if unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC) } != 0 {
        bail!(
            "yukizygisk daemon: pipe2 failed: {}",
            std::io::Error::last_os_error()
        );
    }
    let read_fd = fds[0];
    let mut write_fd = fds[1];
    // 保证 ready fd 不会与 0/1/2 冲突，后续 reset stdio 才安全。
    if write_fd <= 2 {
        let dup = unsafe { libc::fcntl(write_fd, libc::F_DUPFD, 3) };
        if dup >= 0 {
            unsafe { libc::close(write_fd) };
            write_fd = dup;
        }
    }
    let c_value = CString::new(write_fd.to_string())?;
    let argv = [c_path.as_ptr(), std::ptr::null()];

    let pid = unsafe { libc::fork() };
    if pid < 0 {
        unsafe {
            libc::close(read_fd);
            libc::close(write_fd);
        }
        bail!(
            "yukizygisk daemon: fork failed: {}",
            std::io::Error::last_os_error()
        );
    }

    if pid == 0 {
        unsafe {
            libc::close(read_fd);
            let flags = libc::fcntl(write_fd, libc::F_GETFD);
            if flags >= 0 {
                libc::fcntl(write_fd, libc::F_SETFD, flags & !libc::FD_CLOEXEC);
            }
            libc::setsid();
            libc::setpgid(0, 0);
            libc::signal(libc::SIGCHLD, libc::SIG_DFL);
            libc::signal(libc::SIGPIPE, libc::SIG_DFL);
            libc::setenv(c_key.as_ptr(), c_value.as_ptr(), 1);
            libc::chdir(c"/".as_ptr());
            libc::execv(c_path.as_ptr(), argv.as_ptr());
            let byte = [READY_FAILED];
            libc::write(write_fd, byte.as_ptr().cast(), 1);
            libc::_exit(127);
        }
    }

    unsafe { libc::close(write_fd) };
    let mut poll_fd = libc::pollfd {
        fd: read_fd,
        events: libc::POLLIN,
        revents: 0,
    };
    let ready = unsafe { libc::poll(&mut poll_fd, 1, SPAWN_TIMEOUT_MS) };
    let mut byte = [0u8; 1];
    let read_ret = if ready > 0 {
        unsafe { libc::read(read_fd, byte.as_mut_ptr().cast(), 1) }
    } else {
        0
    };
    unsafe { libc::close(read_fd) };

    if ready <= 0 || read_ret != 1 {
        unsafe { libc::kill(pid, libc::SIGKILL) };
        bail!("yukizygisk daemon: {path} did not report readiness in time");
    }
    match byte[0] {
        READY_OK => Ok(()),
        READY_REBOOT => bail!("yukizygisk daemon: {path} requires a reboot to take effect"),
        _ => {
            unsafe { libc::kill(pid, libc::SIGKILL) };
            bail!("yukizygisk daemon: {path} reported startup failure")
        }
    }
}

fn ensure_one(abi: u32) -> Result<()> {
    let path = if abi == ABI_32 {
        ZYGISKD32_PATH
    } else {
        ZYGISKD64_PATH
    };
    match super::endpoint_state(abi) {
        Ok(endpoint) if endpoint.available && endpoint.pid != 0 => {
            log::info!(
                "yukizygisk daemon: abi {abi} already running (pid {} generation {} start {})",
                endpoint.pid,
                endpoint.generation,
                endpoint.start_boottime
            );
            if !endpoint.name.is_empty() {
                log::debug!("yukizygisk daemon: abi {abi} endpoint {}", endpoint.name);
            }
            return Ok(());
        }
        Ok(_) => {}
        Err(err) => log::debug!("yukizygisk daemon: endpoint probe abi {abi} failed: {err:#}"),
    }
    if !is_executable(Path::new(path)) {
        bail!("yukizygisk daemon: {path} is missing or not executable");
    }
    spawn_daemon(path)?;
    log::info!("yukizygisk daemon: abi {abi} started");
    Ok(())
}

/// 确保 zygiskd 已就绪。
///
/// abi: 0 = 全部（按需），1 = 仅 32 位，2 = 仅 64 位。
pub fn ensure(abi: u32) -> Result<()> {
    if unsafe { libc::geteuid() } != 0 {
        bail!("yukizygisk daemon: requires root");
    }
    if abi != 0 && abi != ABI_32 && abi != ABI_64 {
        bail!("yukizygisk daemon: invalid abi {abi}");
    }
    if !super::is_enabled() {
        bail!("yukizygisk daemon: feature is disabled");
    }
    if utils::is_safe_mode() {
        bail!("yukizygisk daemon: system safe mode is active");
    }
    let safemode = super::get_safemode().context("yukizygisk daemon: query kernel safe mode")?;
    if safemode.active {
        bail!(
            "yukizygisk daemon: kernel safe mode is active for {} ({} zygote crashes)",
            safemode.zygote,
            safemode.zygote_crashes
        );
    }
    let runtime = super::query_runtime().context("yukizygisk daemon: query kernel runtime")?;
    let missing = super::REQUIRED_CAPABILITIES & !runtime.capabilities;
    if missing != 0 {
        bail!(
            "yukizygisk daemon: kernel/userspace version mismatch (missing capabilities 0x{missing:x})"
        );
    }
    if let Err(err) = utils::switch_mnt_ns(1) {
        log::warn!("yukizygisk daemon: switch mount namespace failed: {err:#}");
    }

    let _lock = lock_operation()?;
    let require32 = needs_daemon32();
    let mut targets = Vec::new();
    if abi == 0 || abi == ABI_64 {
        targets.push(ABI_64);
    }
    if (abi == 0 || abi == ABI_32) && require32 {
        targets.push(ABI_32);
    }
    if targets.is_empty() {
        log::info!("yukizygisk daemon: no 32-bit runtime required on this device");
        return Ok(());
    }
    for target in targets {
        ensure_one(target)?;
    }
    Ok(())
}
