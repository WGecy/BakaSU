//! IO 调度器固化
//!
//! 管理器固化时写 /data/adb/ksu/io_scheduler (内容 = 调度器名),
//! ksud 每次开机自动应用 (boot 完成后, 覆盖厂商 init 的 cpq 写入).
//! 附加 10s 监听窗口: 厂商晚写入被纠正 (每 2s 检查, 保持则提前结束).
use std::fs;
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result};
use log::{info, warn};

const FLAG_FILE: &str = "/data/adb/ksu/io_scheduler";

/// 开关文件存在时应用固化调度器 (开机自动调用)
pub(crate) fn apply_if_enabled() -> Result<()> {
    let name = match fs::read_to_string(FLAG_FILE) {
        Ok(n) => n.trim().to_string(),
        Err(_) => return Ok(()), // 未固化
    };
    if name.is_empty() {
        return Ok(());
    }
    info!("iosched: 应用固化调度器 {name}");
    let applied = apply_scheduler(&name);
    if applied {
        // 10s 监听窗口: 厂商 init 会晚写 cpq, 每 2s 检查纠正
        let name2 = name.clone();
        thread::spawn(move || {
            for _ in 0..5 {
                thread::sleep(Duration::from_secs(2));
                match fs::read_to_string("/sys/block/sda/queue/scheduler") {
                    Ok(cur) if cur.contains(&format!("[{name2}]")) => {
                        info!("iosched: 保持 {name2}, 提前结束监听");
                        return;
                    }
                    _ => {
                        if apply_scheduler(&name2) {
                            info!("iosched: 监听纠正, 已写回 {name2}");
                        }
                    }
                }
            }
        });
    } else {
        warn!("iosched: 应用 {name} 失败");
    }
    Ok(())
}

fn apply_scheduler(name: &str) -> bool {
    let mut success = false;
    let blocks = match fs::read_dir("/sys/block") {
        Ok(b) => b,
        Err(e) => {
            warn!("iosched: 读取 /sys/block 失败: {e}");
            return false;
        }
    };
    for entry in blocks.flatten() {
        let dev = entry.file_name().to_string_lossy().into_owned();
        if !dev.starts_with("sd") && !dev.starts_with("nvme") {
            continue;
        }
        let path = format!("/sys/block/{dev}/queue/scheduler");
        let _ = std::fs::write(&path, name);
        // 读回验证
        #[allow(clippy::collapsible_if)]
        if let Ok(cur) = fs::read_to_string(&path) {
            if cur.contains(&format!("[{name}]")) {
                success = true;
            }
        }
    }
    success
}

/// 供 CLI 手动应用 (测试用)
pub(crate) fn apply_now(name: &str) -> Result<()> {
    if apply_scheduler(name) {
        fs::write(FLAG_FILE, name).context("写标志文件失败")?;
        info!("iosched: 已应用并固化 {name}");
    } else {
        anyhow::bail!("应用调度器 {name} 失败");
    }
    Ok(())
}

/// 清除固化
pub(crate) fn clear() -> Result<()> {
    let _ = fs::remove_file(FLAG_FILE);
    Ok(())
}
