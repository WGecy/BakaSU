//! Durable record of the last risky module-mount step a boot performed.
//!
//! Loading an embedded module or injecting the module tree can stop a boot before
//! anything is logged, so the step is written down before it runs and cleared once it
//! is known to have finished. A surviving record means the next boot must not repeat it.

use std::io::Write as _;
use std::path::Path;

use anyhow::{Context, Result};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stage {
    Load,
    Inject,
}

pub const CAPTURE_DELAY_SECS: u64 = 150;

pub const CAPTURE_MAX_DELAY_SECS: u64 = 900;

#[must_use]
pub fn parse_capture_delay(content: &str) -> u64 {
    content
        .lines()
        .filter_map(|line| line.split_once('='))
        .filter(|(key, _)| *key == "delay")
        .filter_map(|(_, value)| value.trim().parse::<u64>().ok())
        .next_back()
        .unwrap_or(CAPTURE_DELAY_SECS)
        .min(CAPTURE_MAX_DELAY_SECS)
}

impl Stage {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Load => "load",
            Self::Inject => "inject",
        }
    }

    #[must_use]
    pub fn parse(value: &str) -> Option<Self> {
        match value.trim() {
            "load" => Some(Self::Load),
            "inject" => Some(Self::Inject),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Inflight {
    pub boot_id: String,
    pub generation: u64,
    pub stage: Stage,
    pub provider: String,
    pub detail: String,
}

impl Inflight {
    #[must_use]
    pub fn describe(&self) -> String {
        let detail = if self.detail.is_empty() {
            String::new()
        } else {
            format!(" ({})", self.detail)
        };
        format!(
            "{} {}{detail} in boot {} generation {}",
            self.stage.as_str(),
            self.provider,
            self.boot_id,
            self.generation,
        )
    }
}

#[must_use]
pub fn current_boot_id() -> String {
    std::fs::read_to_string("/proc/sys/kernel/random/boot_id")
        .map_or_else(|_| String::new(), |value| value.trim().to_string())
}

pub fn begin(
    path: &Path,
    stage: Stage,
    provider: &str,
    detail: &str,
    boot_id: &str,
    generation: u64,
) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).with_context(|| format!("create {}", parent.display()))?;
    }
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |since| since.as_secs());
    let mut file =
        std::fs::File::create(path).with_context(|| format!("create {}", path.display()))?;
    file.write_all(
        format!(
            "boot_id={boot_id}\ngeneration={generation}\nstage={}\nprovider={}\ndetail={}\ntime={stamp}\n",
            stage.as_str(),
            sanitize(provider),
            sanitize(detail),
        )
        .as_bytes(),
    )
    .with_context(|| format!("write {}", path.display()))?;
    file.sync_all()
        .with_context(|| format!("sync {}", path.display()))
}

#[must_use]
pub fn read(path: &Path) -> Option<Inflight> {
    let content = std::fs::read_to_string(path).ok()?;
    let mut boot_id = None;
    let mut generation = None;
    let mut stage = None;
    let mut provider = None;
    let mut detail = None;
    for line in content.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        match key {
            "boot_id" => boot_id = Some(value.to_string()),
            "generation" => generation = value.parse().ok(),
            "stage" => stage = Stage::parse(value),
            "provider" => provider = Some(value.to_string()),
            "detail" => detail = Some(value.to_string()),
            _ => {}
        }
    }
    Some(Inflight {
        boot_id: boot_id?,
        generation: generation.unwrap_or(0),
        stage: stage?,
        provider: provider?,
        detail: detail.unwrap_or_default(),
    })
}

#[must_use]
pub fn died(path: &Path) -> bool {
    let Ok(epoch) = crate::startup_epoch::current_or_create() else {
        return false;
    };
    died_at(path, &epoch)
}

fn died_at(path: &Path, epoch: &crate::startup_epoch::StartupEpoch) -> bool {
    read(path).is_some_and(|record| {
        record.boot_id != epoch.boot_id
            || (record.generation != 0 && record.generation != epoch.generation)
    })
}

pub fn clear(path: &Path) {
    if let Err(error) = try_clear(path) {
        log::warn!("mount guard: {error:#}");
    }
}

pub fn try_clear(path: &Path) -> Result<()> {
    remove(path, "")
}

pub fn clear_if_ours(path: &Path) -> Result<()> {
    let epoch = crate::startup_epoch::current_or_create()?;
    clear_if_ours_at(path, &epoch)
}

pub fn clear_if_ours_at(path: &Path, epoch: &crate::startup_epoch::StartupEpoch) -> Result<()> {
    let ours = read(path).is_none_or(|record| {
        record.boot_id == epoch.boot_id
            && (record.generation == 0 || record.generation == epoch.generation)
    });
    if ours {
        try_clear(path)?;
    }
    Ok(())
}

fn remove(path: &Path, note: &str) -> Result<()> {
    match std::fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => {
            Err(error).with_context(|| format!("could not clear {}{note}", path.display()))
        }
    }
}

fn sanitize(value: &str) -> String {
    value.replace(['\n', '\r'], " ")
}

pub fn append_log(path: &Path, block: &str, max_bytes: u64) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).with_context(|| format!("create {}", parent.display()))?;
    }
    let size = std::fs::metadata(path).map_or(0, |meta| meta.len());
    if size > 0 && size.saturating_add(block.len() as u64) > max_bytes {
        let rotated = path.with_extension("old");
        let _ = std::fs::remove_file(&rotated);
        std::fs::rename(path, &rotated)
            .with_context(|| format!("rotate {} to {}", path.display(), rotated.display()))?;
    }
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .with_context(|| format!("open {}", path.display()))?;
    file.write_all(block.as_bytes())
        .with_context(|| format!("append {}", path.display()))?;
    let _ = file.sync_all();
    Ok(())
}

#[cfg(test)]
#[path = "mount_guard_tests.rs"]
mod tests;
