use std::fs;
use std::time::Duration;

use anyhow::{Context, Result};
use log::{info, warn};

use crate::defs::{IO_SCHEDULER_FLAG, WORKING_DIR};

const BLOCK_DIR: &str = "/sys/block";
const LISTEN_INTERVAL: Duration = Duration::from_secs(1);
const LISTEN_ROUNDS: u32 = 5;

fn scheduler_path(dev: &str) -> String {
    format!("{BLOCK_DIR}/{dev}/queue/scheduler")
}

fn is_applied(dev: &str, name: &str) -> bool {
    fs::read_to_string(scheduler_path(dev)).is_ok_and(|cur| cur.contains(&format!("[{name}]")))
}

fn apply_all(name: &str) -> Vec<String> {
    let blocks = match fs::read_dir(BLOCK_DIR) {
        Ok(b) => b,
        Err(e) => {
            warn!("iosched: cannot read {BLOCK_DIR}: {e}");
            return Vec::new();
        }
    };
    let mut applied = Vec::new();
    for entry in blocks.flatten() {
        let dev = entry.file_name().to_string_lossy().into_owned();
        if !dev.starts_with("sd") && !dev.starts_with("nvme") {
            continue;
        }
        let _ = fs::write(scheduler_path(&dev), name);
        if is_applied(&dev, name) {
            applied.push(dev);
        }
    }
    applied
}

fn guard(name: &str, targets: &[String]) {
    for _ in 0..LISTEN_ROUNDS {
        std::thread::sleep(LISTEN_INTERVAL);
        if targets.iter().all(|dev| is_applied(dev, name)) {
            info!("iosched: {name} held, stopped watching early");
            return;
        }
        if !apply_all(name).is_empty() {
            info!("iosched: rewrote {name} while watching");
        }
    }
}

pub(crate) fn apply_if_enabled() {
    let Ok(saved) = fs::read_to_string(IO_SCHEDULER_FLAG) else {
        return;
    };
    let name = saved.trim();
    if name.is_empty() {
        return;
    }
    info!("iosched: applying frozen scheduler {name}");
    let applied = apply_all(name);
    if applied.is_empty() {
        warn!("iosched: could not apply {name}");
    } else {
        guard(name, &applied);
    }
}

pub(crate) fn apply_now(name: &str) -> Result<()> {
    if apply_all(name).is_empty() {
        anyhow::bail!("iosched: no block device accepted {name}");
    }
    fs::create_dir_all(WORKING_DIR).context("iosched: cannot create the working directory")?;
    fs::write(IO_SCHEDULER_FLAG, name).context("iosched: cannot write the flag file")?;
    info!("iosched: applied and froze {name}");
    Ok(())
}

pub(crate) fn clear() -> Result<()> {
    fs::remove_file(IO_SCHEDULER_FLAG).ok();
    Ok(())
}

#[derive(clap::Subcommand, Debug)]
pub(crate) enum IoschedOp {
    /// Apply a scheduler to every sd*/nvme* device and re-apply it on boot
    Apply {
        /// scheduler name as listed by the kernel, e.g. mq-deadline
        scheduler: String,
    },
    /// Drop the pinned scheduler; the next boot keeps the kernel default
    Clear,
}

pub(crate) fn run(command: IoschedOp) -> Result<()> {
    match command {
        IoschedOp::Apply { scheduler } => apply_now(&scheduler),
        IoschedOp::Clear => clear(),
    }
}
