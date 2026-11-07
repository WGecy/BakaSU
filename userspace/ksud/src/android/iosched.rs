//! IO 调度器固化
//!
//! 管理器固化时写 /data/adb/ksu/io_scheduler (内容 = 调度器名),
//! ksud 每次开机自动应用 (boot 完成后, 覆盖厂商 init 的 cpq 写入).
//! 附加监听窗口: 厂商晚写入被纠正 (每 1s 检查, 保持则提前结束).
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

/// 读回 sysfs 验证设备当前生效的调度器
fn is_applied(dev: &str, name: &str) -> bool {
    fs::read_to_string(scheduler_path(dev)).is_ok_and(|cur| cur.contains(&format!("[{name}]")))
}

/// 对所有 sd*/nvme* 块设备写入调度器, 返回写入成功 (读回已验证) 的设备名
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
        // echo 写 sysfs 无回显, 必须读回验证真实状态
        let _ = fs::write(scheduler_path(&dev), name);
        if is_applied(&dev, name) {
            applied.push(dev);
        }
    }
    applied
}

/// 监听窗口: 厂商 init 会晚写调度器, 定期检查并纠正
///
/// 必须在 boot-completed 进程内同步执行: ksud 由 init 一次性 exec, 后台线程会随进程退出被回收.
/// 监听目标是首次写成功的设备, 不随后续纠正结果收缩 —— 否则某轮纠正只成功一台时,
/// 剩下的设备会被移出监听范围, 导致 "all(applied)" 提前成立而漏纠.
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

/// 开关文件存在时应用固化调度器 (开机自动调用)
pub(crate) fn apply_if_enabled() {
    let Ok(saved) = fs::read_to_string(FLAG_FILE) else {
        return; // 未固化
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

/// 供 CLI 手动应用 (测试用)
pub(crate) fn apply_now(name: &str) -> Result<()> {
    if apply_all(name).is_empty() {
        anyhow::bail!("应用调度器 {name} 失败");
    }
    fs::create_dir_all(ADB_DIR).context("创建 /data/adb/ksu 失败")?;
    fs::write(FLAG_FILE, name).context("写标志文件失败")?;
    info!("iosched: 已应用并固化 {name}");
    Ok(())
}

/// 清除固化
pub(crate) fn clear() -> Result<()> {
    fs::remove_file(FLAG_FILE).ok();
    Ok(())
}
