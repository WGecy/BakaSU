//! ksud zygisk 子命令 (对齐 Rouyashiki/YukiZygisk 的 yzctl 语法)

use anyhow::{Result, bail};
use clap::{Args, Subcommand};
use serde_json::json;

use super::{ABI_32, ABI_64, config, daemon, query_runtime, snapshot};

#[derive(Debug, Args)]
pub struct ZygiskArgs {
    #[command(subcommand)]
    pub command: ZygiskSubCommands,
}

#[derive(Debug, Subcommand)]
pub enum ZygiskSubCommands {
    /// 显示内核侧运行时状态
    Status {
        /// 以 JSON 输出
        #[arg(long)]
        json: bool,
    },
    /// 确保 zygiskd 守护进程运行
    EnsureDaemon {
        /// 目标 ABI: 32 / 64 / all
        #[arg(long, default_value = "all")]
        abi: String,
    },
    /// 通知内核重新扫描模块
    Reload,
    /// yzconfig.json 配置管理
    Config {
        #[command(subcommand)]
        command: ConfigSubCommands,
    },
    /// 重新生成 early-native 快照
    RefreshSnapshot,
    /// 开启内核级 Zygisk
    Enable,
    /// 关闭内核级 Zygisk
    Disable,
}

#[derive(Debug, Subcommand)]
pub enum ConfigSubCommands {
    /// 打印当前配置
    Get,
    /// 修改配置 (KEY VALUE ...)
    Set {
        #[arg(required = true, num_args = 2..)]
        pairs: Vec<String>,
    },
}

pub fn run_main(args: ZygiskArgs) -> Result<()> {
    match args.command {
        ZygiskSubCommands::Status { json: as_json } => status(as_json),
        ZygiskSubCommands::EnsureDaemon { abi } => {
            let abi = parse_abi(&abi)?;
            daemon::ensure(abi)?;
            println!("yukizygisk daemon ensured (abi {abi})");
            Ok(())
        }
        ZygiskSubCommands::Reload => {
            super::reload()?;
            println!("YukiZygisk reload signalled");
            if let Err(err) = snapshot::refresh() {
                log::warn!("yukizygisk early: snapshot refresh failed: {err:#}");
            }
            Ok(())
        }
        ZygiskSubCommands::Config { command } => match command {
            ConfigSubCommands::Get => {
                let cfg = config::read()?;
                println!("{}", serde_json::to_string_pretty(&cfg.to_json())?);
                Ok(())
            }
            ConfigSubCommands::Set { pairs } => {
                let cfg = config::update(&pairs)?;
                if let Err(err) = super::reload() {
                    log::warn!("yukizygisk: reload after config change failed: {err:#}");
                }
                if let Err(err) = snapshot::refresh() {
                    log::warn!("yukizygisk early: snapshot refresh failed: {err:#}");
                }
                println!("{}", serde_json::to_string_pretty(&cfg.to_json())?);
                Ok(())
            }
        },
        ZygiskSubCommands::RefreshSnapshot => {
            snapshot::refresh()?;
            println!("YukiZygisk early snapshot refreshed");
            Ok(())
        }
        ZygiskSubCommands::Enable => {
            super::set_enabled(true)?;
            println!("YukiZygisk enabled");
            Ok(())
        }
        ZygiskSubCommands::Disable => {
            super::set_enabled(false)?;
            println!("YukiZygisk disabled");
            Ok(())
        }
    }
}

fn parse_abi(text: &str) -> Result<u32> {
    match text {
        "all" | "0" => Ok(0),
        "32" => Ok(ABI_32),
        "64" => Ok(ABI_64),
        other => bail!("ABI must be 32, 64 or all, got {other}"),
    }
}

fn abi_name(abi: u8) -> &'static str {
    match abi {
        1 => "armeabi-v7a",
        2 => "arm64-v8a",
        _ => "unknown",
    }
}

fn kind_name(kind: u8) -> &'static str {
    match kind {
        1 => "zygote",
        2 => "native",
        _ => "unknown",
    }
}

fn state_name(state: u8) -> &'static str {
    match state {
        1 => "detected",
        2 => "redirected",
        3 => "injected",
        4 => "failed",
        5 => "safemode",
        6 => "exited",
        _ => "unknown",
    }
}

fn injection_name(state: u8, injection: u8) -> &'static str {
    if state == 6 {
        return "exited";
    }
    match injection {
        1 => "injected",
        2 => "failed",
        3 => "skipped",
        _ => "unknown",
    }
}

fn target_type_name(target_type: u8) -> &'static str {
    match target_type {
        1 => "name",
        2 => "path",
        _ => "unknown",
    }
}

fn status(as_json: bool) -> Result<()> {
    let supported = super::is_supported();
    let enabled = super::is_enabled();
    let runtime = query_runtime()?;
    let cfg = config::read()?;

    if as_json {
        let records: Vec<_> = runtime
            .records
            .iter()
            .map(|record| {
                json!({
                    "pid": record.pid,
                    "generation": record.generation,
                    "restarts": record.restarts,
                    "kind": kind_name(record.kind),
                    "kind_id": record.kind,
                    "state": state_name(record.state),
                    "state_id": record.state,
                    "injection_state": injection_name(record.state, record.injection_state()),
                    "abi": abi_name(record.abi),
                    "abi_id": record.abi,
                    "target_type": target_type_name(record.target_type),
                    "target_type_id": record.target_type,
                    "flags": record.flags,
                    "process": record.process,
                    "target": record.target,
                    "module": record.module,
                })
            })
            .collect();
        let payload = json!({
            "supported": supported,
            "enabled": enabled,
            "generation": runtime.generation,
            "count": runtime.records.len(),
            "safe_mode": runtime.safe_mode != 0,
            "zygote_crashes": runtime.zygote_crashes,
            "safe_mode_zygote": runtime.safe_mode_zygote,
            "capabilities": runtime.capabilities,
            "config": cfg.to_json(),
            "runtime": records,
        });
        println!("{}", serde_json::to_string_pretty(&payload)?);
        return Ok(());
    }

    println!("Generation:       {}", runtime.generation);
    println!("Kernel support:   {supported}");
    println!("Enabled:          {enabled}");
    println!("Safe mode:        {}", runtime.safe_mode != 0);
    println!("Zygote crashes:   {}", runtime.zygote_crashes);
    println!("Active targets:   {}", runtime.records.len());
    println!("Capabilities:     0x{:x}", runtime.capabilities);
    println!(
        "Config:           {}",
        serde_json::to_string(&cfg.to_json())?
    );
    if !runtime.safe_mode_zygote.is_empty() {
        println!("Safe mode zygote: {}", runtime.safe_mode_zygote);
    }
    if runtime.records.is_empty() {
        println!("No injected target yet.");
        return Ok(());
    }
    println!("PID\tGEN\tABI\tKIND\tSTATE\tINJECTION\tPROCESS\tTARGET\tMODULE");
    for record in &runtime.records {
        println!(
            "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            record.pid,
            record.generation,
            abi_name(record.abi),
            kind_name(record.kind),
            state_name(record.state),
            injection_name(record.state, record.injection_state()),
            record.process,
            record.target,
            record.module,
        );
    }
    Ok(())
}
