//! YukiZygisk 配置 (yzconfig.json) 读写，对齐 userspace/zygisk/settings.hpp。

use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

use anyhow::{Context, Result, bail};
use serde_json::{Map, Value, json};

use super::{LOAD_CONFIG_ANONYMOUS, LOAD_CONFIG_VALID, LOAD_CONFIG_YUKILINKER};
use crate::{android::utils, defs};

/// yzconfig.json 默认值，必须与 userspace/zygisk/load_policy.hpp 的 defaults 一致。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Config {
    pub yukilinker: bool,
    pub anonymous_memory: bool,
    pub early_load: bool,
    pub denylist_mode: u8,
    pub dmesg_log: bool,
    pub crash_protection: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            yukilinker: true,
            anonymous_memory: true,
            early_load: false,
            denylist_mode: 0,
            dmesg_log: false,
            crash_protection: true,
        }
    }
}

impl Config {
    /// 换算成内核 early-native 快照使用的 load_flags。
    pub fn load_flags(&self) -> u16 {
        let mut flags = LOAD_CONFIG_VALID;
        if self.yukilinker {
            flags |= LOAD_CONFIG_YUKILINKER;
        }
        if self.anonymous_memory {
            flags |= LOAD_CONFIG_ANONYMOUS;
        }
        flags
    }

    pub fn to_json(self) -> Value {
        json!({
            "yukilinker": self.yukilinker,
            "anonymous_memory": self.anonymous_memory,
            "early_load": self.early_load,
            "denylist_mode": self.denylist_mode,
            "dmesg_log": self.dmesg_log,
            "crash_protection": self.crash_protection,
        })
    }

    fn apply(&mut self, root: &Map<String, Value>) {
        if let Some(Value::Bool(value)) = root.get("yukilinker") {
            self.yukilinker = *value;
        }
        if let Some(Value::Bool(value)) = root.get("anonymous_memory") {
            self.anonymous_memory = *value;
        }
        if let Some(Value::Bool(value)) = root.get("early_load") {
            self.early_load = *value;
        }
        if let Some(Value::Bool(value)) = root.get("dmesg_log") {
            self.dmesg_log = *value;
        }
        if let Some(Value::Bool(value)) = root.get("crash_protection") {
            self.crash_protection = *value;
        }
        if let Some(Value::Number(number)) = root.get("denylist_mode")
            && let Some(value) = number.as_u64()
        {
            self.denylist_mode = value.min(2) as u8;
        }
    }
}

/// 读取配置，失败时回退到默认值（仅供内部使用，不打断 post-fs-data）。
pub fn load() -> Config {
    match read() {
        Ok(config) => config,
        Err(err) => {
            log::warn!("yukizygisk config: {err:#}, using defaults");
            Config::default()
        }
    }
}

/// 读取配置；文件不存在时返回默认值。
pub fn read() -> Result<Config> {
    let path = defs::YUKIZYGISK_CONFIG_PATH;
    let mut config = Config::default();
    let data = match std::fs::read(path) {
        Ok(data) => data,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(config),
        Err(err) => return Err(err).with_context(|| format!("read {path}")),
    };
    let root: Value = serde_json::from_slice(&data).with_context(|| format!("parse {path}"))?;
    if let Value::Object(map) = &root {
        config.apply(map);
    }
    Ok(config)
}

/// 原子写入配置（tmp + fsync + rename + dir fsync）。
pub fn write(config: &Config) -> Result<()> {
    let dir = defs::YUKIZYGISK_DIR;
    utils::ensure_dir_exists(dir)?;
    let _ = std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700));

    let path = defs::YUKIZYGISK_CONFIG_PATH;
    let tmp = format!("{path}.tmp.{}", std::process::id());
    let body = serde_json::to_vec_pretty(&config.to_json())?;
    {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&tmp)
            .with_context(|| format!("create {tmp}"))?;
        std::io::Write::write_all(&mut file, &body).with_context(|| format!("write {tmp}"))?;
        file.sync_all().with_context(|| format!("fsync {tmp}"))?;
    }
    std::fs::rename(&tmp, path).with_context(|| format!("rename {tmp} -> {path}"))?;
    if let Ok(handle) = std::fs::File::open(dir) {
        let _ = handle.sync_all();
    }
    Ok(())
}

/// 按 KEY VALUE 序列更新配置（数量必须为偶数）。
pub fn update(tokens: &[String]) -> Result<Config> {
    if tokens.is_empty() || tokens.len() % 2 != 0 {
        bail!("config requires KEY VALUE pairs");
    }
    let mut config = read()?;
    for [key, value] in tokens.as_chunks::<2>().0 {
        apply_pair(&mut config, key, value)?;
    }
    write(&config)?;
    Ok(config)
}

fn apply_pair(config: &mut Config, key: &str, value: &str) -> Result<()> {
    match key {
        "yukilinker" => config.yukilinker = parse_bool(key, value)?,
        "anonymous_memory" => config.anonymous_memory = parse_bool(key, value)?,
        "early_load" => config.early_load = parse_bool(key, value)?,
        "dmesg_log" => config.dmesg_log = parse_bool(key, value)?,
        "crash_protection" => config.crash_protection = parse_bool(key, value)?,
        "denylist_mode" => {
            config.denylist_mode = match value {
                "0" => 0,
                "1" => 1,
                "2" => 2,
                other => bail!("denylist_mode must be 0, 1 or 2, got {other}"),
            };
        }
        other => bail!("unknown yzconfig key: {other}"),
    }
    Ok(())
}

fn parse_bool(key: &str, value: &str) -> Result<bool> {
    match value {
        "true" => Ok(true),
        "false" => Ok(false),
        other => bail!("{key} expects true or false, got {other}"),
    }
}
