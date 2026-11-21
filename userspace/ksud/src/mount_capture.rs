//! Opt-in capture of a boot that a mount provider stopped.
//!
//! `ksud mount capture on` makes the boot that injects spawn a detached helper; if that
//! boot never completes, the helper dumps what the kernel reports as blocked into a
//! bounded log.

use std::os::fd::RawFd;
use std::path::Path;
use std::process::Command;
use std::time::Duration;

use anyhow::{Context, Result, bail};

use crate::android::utils;
use crate::mount_guard;
use crate::{assets, defs};

#[must_use]
pub fn enabled() -> Option<u64> {
    let content = std::fs::read_to_string(defs::MOUNT_CAPTURE_FLAG).ok()?;
    Some(mount_guard::parse_capture_delay(&content))
}

pub fn command(action: &str, seconds: u64) -> Result<()> {
    match action {
        "on" => {
            let delay = seconds.clamp(1, mount_guard::CAPTURE_MAX_DELAY_SECS);
            std::fs::write(defs::MOUNT_CAPTURE_FLAG, format!("delay={delay}\n"))
                .with_context(|| format!("write {}", defs::MOUNT_CAPTURE_FLAG))?;
            println!(
                "capture armed: a boot that never finishes is dumped to {} after {delay}s",
                defs::MOUNT_CAPTURE_LOG
            );
            Ok(())
        }
        "off" => {
            match std::fs::remove_file(defs::MOUNT_CAPTURE_FLAG) {
                Ok(()) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => return Err(error).context("remove the capture flag"),
            }
            println!("capture disarmed");
            Ok(())
        }
        "dump" => dump(&mount_guard::current_boot_id()),
        other => bail!("unknown capture action {other:?}; expected one of: on, off, dump"),
    }
}

pub fn spawn(lock_fd: RawFd) {
    if !utils::create_daemon_with(true, || {
        unsafe { libc::close(lock_fd) };
        Ok(())
    })
    .unwrap_or(false)
    {
        return;
    }
    let boot_id = mount_guard::current_boot_id();
    note(&format!("--- capture armed for boot={boot_id}\n"));
    if let Err(error) = dump(&boot_id) {
        note(&format!("capture failed: {error:#}\n"));
    }
    unsafe { libc::_exit(0) };
}

pub fn dump(boot_id: &str) -> Result<()> {
    let delay = enabled().unwrap_or(mount_guard::CAPTURE_DELAY_SECS);
    std::thread::sleep(Duration::from_secs(delay));

    let inflight = mount_guard::read(Path::new(defs::MOUNT_INFLIGHT));
    let stuck = inflight
        .as_ref()
        .is_some_and(|record| record.boot_id == boot_id);
    if !stuck {
        note(&format!(
            "boot {boot_id} finished its boot stage; nothing to capture\n"
        ));
        return Ok(());
    }

    let step = inflight.map_or_else(String::new, |record| record.describe());
    let output = Command::new(assets::BUSYBOX_PATH)
        .args(["sh", "-c", PROBE])
        .output()
        .context("run the capture probe")?;

    let mut body = format!("=== capture boot={boot_id} delay={delay}s stuck_on=\"{step}\" ===\n");
    body.push_str(&String::from_utf8_lossy(&output.stdout));
    if !output.stderr.is_empty() {
        body.push_str("--- probe stderr ---\n");
        body.push_str(&String::from_utf8_lossy(&output.stderr));
    }
    mount_guard::append_log(
        Path::new(defs::MOUNT_CAPTURE_LOG),
        &body,
        defs::MOUNT_CAPTURE_LOG_MAX_BYTES,
    )
}

fn note(line: &str) {
    if let Err(error) = mount_guard::append_log(
        Path::new(defs::MOUNT_CAPTURE_LOG),
        line,
        defs::MOUNT_CAPTURE_LOG_MAX_BYTES,
    ) {
        log::warn!("mount capture: could not write the capture log: {error:#}");
    }
}

const PROBE: &str = include_str!("mount_capture.sh");
