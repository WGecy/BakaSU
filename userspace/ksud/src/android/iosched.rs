use std::fs;
use std::time::Duration;

use anyhow::{Context, Result};
use log::{info, warn};

const ADB_DIR: &str = "/data/adb/ksu";
const FLAG_FILE: &str = "/data/adb/ksu/io_scheduler";
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
            warn!("iosched: 读取 {BLOCK_DIR} 失败: {e}");
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

// 必须在 boot-completed 进程内同步执行 (ksud 由 init 一次性 exec); 监听目标取首次写成功的设备,
// 不随后续纠正结果收缩, 否则中途只纠正成功一台时剩余设备会被移出监听范围而漏纠.
fn guard(name: &str, targets: &[String]) {
    for _ in 0..LISTEN_ROUNDS {
        std::thread::sleep(LISTEN_INTERVAL);
        if targets.iter().all(|dev| is_applied(dev, name)) {
            info!("iosched: 保持 {name}, 提前结束监听");
            return;
        }
        if !apply_all(name).is_empty() {
            info!("iosched: 监听纠正, 已写回 {name}");
        }
    }
}

pub(crate) fn apply_if_enabled() {
    let Ok(saved) = fs::read_to_string(FLAG_FILE) else {
        return;
    };
    let name = saved.trim();
    if name.is_empty() {
        return;
    }
    info!("iosched: 应用固化调度器 {name}");
    let applied = apply_all(name);
    if applied.is_empty() {
        warn!("iosched: 应用 {name} 失败");
    } else {
        guard(name, &applied);
    }
}

pub(crate) fn apply_now(name: &str) -> Result<()> {
    if apply_all(name).is_empty() {
        anyhow::bail!("应用调度器 {name} 失败");
    }
    fs::create_dir_all(ADB_DIR).context("创建 /data/adb/ksu 失败")?;
    fs::write(FLAG_FILE, name).context("写标志文件失败")?;
    info!("iosched: 已应用并固化 {name}");
    Ok(())
}

pub(crate) fn clear() -> Result<()> {
    fs::remove_file(FLAG_FILE).ok();
    Ok(())
}
