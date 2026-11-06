//! ksud nomount 子命令 (对齐 maxsteeel/nomount 的 nm CLI 语法)

use anyhow::{Result, bail};
use clap::{Args, Subcommand};
use serde_json::json;

use super::NmRuleSpec;
use super::add_specs;
use super::clear;
use super::clear_rules;
use super::clear_uids;
use super::delete_rules;
use super::exclude_add;
use super::exclude_list;
use super::exclude_remove;
use super::is_enabled;
use super::isolated_blocking;
use super::list_rules;
use super::list_uids;
use super::mount;
use super::parse_uid_range;
use super::safe_mode;
use super::set_enabled;
use super::set_isolated_blocking;
use super::set_safe_mode;
use super::snapshot;
use super::version;

#[derive(Debug, Args)]
pub struct NoMountArgs {
    #[command(subcommand)]
    pub command: NoMountSubCommands,
}

#[derive(Debug, Subcommand)]
pub enum NoMountSubCommands {
    /// 查看内核是否支持 NoMount 及协议版本
    Status,
    /// 列出内核中的重定向规则
    List {
        #[arg(long)]
        json: bool,
    },
    /// 添加一条规则 (虚拟路径 [真实路径])
    Add {
        /// 虚拟路径, 例如 /system/app/Foo
        virtual_path: String,
        /// 真实路径; 使用 --whiteout 时可省略
        real_path: Option<String>,
        /// 隐藏该虚拟路径 (不重定向到任何真实路径)
        #[arg(long)]
        whiteout: bool,
        /// 目录不透明 (不合并底层目录内容)
        #[arg(long)]
        opaque: bool,
        /// 规则只对该 UID 生效
        #[arg(long, value_name = "UID")]
        uid: Option<u32>,
        /// 规则生效的 UID 区间, 形如 10000-19999 或 all
        #[arg(long, value_name = "RANGE")]
        uid_range: Option<String>,
        /// 在生效区间内排除的 UID 子区间, 形如 10100-10199
        #[arg(long, value_name = "RANGE")]
        uid_excluded: Option<String>,
    },
    /// 删除规则 (按虚拟路径匹配, 可一次给多个)
    Remove {
        #[arg(required = true)]
        virtual_paths: Vec<String>,
    },
    /// 清空规则与 UID 排除列表
    Clear,
    /// 只清空规则
    ClearRules,
    /// 只清空 UID 排除列表
    ClearUids,
    /// 立即按当前模块状态重新注入规则
    Inject,
    /// 列出已安装模块及其规则统计
    Modules {
        #[arg(long)]
        json: bool,
    },
    /// 热挂载一个模块
    Load { module_id: String },
    /// 热卸载一个模块
    Unload { module_id: String },
    /// 总开关 (0=关闭, 1=开启)
    SetEnabled { enabled: u8 },
    /// 查询总开关
    IsEnabled,
    /// 一次性输出状态/模块/规则/排除列表的聚合 JSON
    Snapshot,
    /// 设置模块的 disable 标记 (0=启用, 1=禁用)
    ModuleDisable { module_id: String, disabled: u8 },
    /// 查看 UID 排除列表
    ExcludeList {
        #[arg(long)]
        json: bool,
    },
    /// 添加 UID 排除
    ExcludeAdd { uid: u32 },
    /// 移除 UID 排除
    ExcludeRemove { uid: u32 },
    /// 安全模式: 存在标志时下次开机不注入 (不传参数则查询)
    SafeMode { state: Option<String> },
    /// 隔离进程 (UID 90000-99999) 拦截开关 (不传参数则查询)
    Isolated { state: Option<String> },
    /// 只查看 UID 排除列表的内核态
    UidList {
        #[arg(long)]
        json: bool,
    },
    /// 查看内核 NoMount 版本串
    Version,
}

pub fn run_main(args: NoMountArgs) -> Result<()> {
    match args.command {
        NoMountSubCommands::Status => match version() {
            Ok(v) => {
                println!("supported: true");
                println!("version: {v}");
                println!("enabled: {}", is_enabled());
                println!("safe_mode: {}", safe_mode());
            }
            Err(e) => {
                println!("supported: false");
                println!("error: {e}");
            }
        },
        NoMountSubCommands::Version => {
            println!("{}", version()?);
        }
        NoMountSubCommands::List { json: as_json } => {
            let rules = list_rules()?;
            if as_json {
                println!("{}", rules_to_json(&rules));
            } else if rules.is_empty() {
                println!("(no rules)");
            } else {
                for rule in &rules {
                    println!("{}", rule_to_line(rule));
                }
            }
        }
        NoMountSubCommands::Add {
            virtual_path,
            real_path,
            whiteout,
            opaque,
            uid,
            uid_range,
            uid_excluded,
        } => {
            if whiteout && opaque {
                bail!("--whiteout 与 --opaque 互斥");
            }
            let spec = build_spec(
                virtual_path,
                real_path,
                whiteout,
                opaque,
                uid,
                uid_range,
                uid_excluded,
            )?;
            let applied = add_specs(std::slice::from_ref(&spec))?;
            if applied == 0 {
                bail!("规则未下发 (路径非法或过长)");
            }
            // 手动添加的规则要能跨重启保留
            super::persist_spec(&spec);
            println!("ok");
        }
        NoMountSubCommands::Remove { virtual_paths } => {
            remove_by_virtual(&virtual_paths)?;
            println!("ok");
        }
        NoMountSubCommands::Clear => {
            clear()?;
            println!("ok");
        }
        NoMountSubCommands::ClearRules => {
            clear_rules()?;
            println!("ok");
        }
        NoMountSubCommands::ClearUids => {
            clear_uids()?;
            println!("ok");
        }
        NoMountSubCommands::Inject => {
            mount::inject_modules()?;
            super::replay_custom_rules()?;
            println!("injected");
        }
        NoMountSubCommands::Modules { json: as_json } => {
            let modules = mount::modules()?;
            if as_json {
                let items: Vec<_> = modules
                    .iter()
                    .map(|m| {
                        json!({
                            "id": m.id,
                            "name": m.name,
                            "version": m.version,
                            "author": m.author,
                            "description": m.description,
                            "disabled": m.disabled,
                            "skip_mount": m.skip_mount,
                            "file_count": m.file_count,
                            "loaded": m.loaded,
                        })
                    })
                    .collect();
                println!("{}", serde_json::to_string(&items)?);
            } else if modules.is_empty() {
                println!("(no modules)");
            } else {
                for m in &modules {
                    println!(
                        "{} {} [{}] files={} loaded={}{}{}",
                        m.id,
                        m.name,
                        if m.version.is_empty() {
                            "-"
                        } else {
                            &m.version
                        },
                        m.file_count,
                        m.loaded,
                        if m.disabled { " disabled" } else { "" },
                        if m.skip_mount { " skip_mount" } else { "" }
                    );
                }
            }
        }
        NoMountSubCommands::Load { module_id } => {
            let n = mount::load_module(&module_id)?;
            println!("loaded: {n}");
        }
        NoMountSubCommands::Unload { module_id } => {
            let n = mount::unload_module(&module_id)?;
            println!("unloaded: {n}");
        }
        NoMountSubCommands::SetEnabled { enabled } => {
            let want = enabled != 0;
            set_enabled(want)?;
            println!("{}", is_enabled());
        }
        NoMountSubCommands::IsEnabled => {
            println!("{}", is_enabled());
        }
        NoMountSubCommands::Snapshot => {
            println!("{}", snapshot()?);
        }
        NoMountSubCommands::ModuleDisable {
            module_id,
            disabled,
        } => {
            mount::set_module_disabled(&module_id, disabled != 0)?;
            println!("ok");
        }
        NoMountSubCommands::ExcludeList { json: as_json } => {
            let uids = exclude_list()?;
            if as_json {
                println!("{}", serde_json::to_string(&uids)?);
            } else if uids.is_empty() {
                println!("(no excluded uids)");
            } else {
                for uid in uids {
                    println!("{uid}");
                }
            }
        }
        NoMountSubCommands::ExcludeAdd { uid } => {
            exclude_add(uid)?;
            println!("ok");
        }
        NoMountSubCommands::ExcludeRemove { uid } => {
            exclude_remove(uid)?;
            println!("ok");
        }
        NoMountSubCommands::SafeMode { state } => match state.as_deref() {
            None => println!("{}", safe_mode()),
            Some(s) => {
                let on = parse_toggle(s)?;
                set_safe_mode(on)?;
                println!("{}", safe_mode());
            }
        },
        NoMountSubCommands::Isolated { state } => match state.as_deref() {
            None => println!("{}", isolated_blocking()?),
            Some(s) => {
                let on = parse_toggle(s)?;
                set_isolated_blocking(on)?;
                println!("{}", isolated_blocking()?);
            }
        },
        NoMountSubCommands::UidList { json: as_json } => {
            let uids = list_uids()?;
            if as_json {
                println!("{}", serde_json::to_string(&uids)?);
            } else {
                for uid in uids {
                    println!("{uid}");
                }
            }
        }
    }
    Ok(())
}

fn parse_toggle(text: &str) -> Result<bool> {
    match text.trim().to_ascii_lowercase().as_str() {
        "on" | "1" | "true" | "yes" => Ok(true),
        "off" | "0" | "false" | "no" => Ok(false),
        other => bail!("无法识别的开关值: {other} (可用 on/off)"),
    }
}

fn build_spec(
    virtual_path: String,
    real_path: Option<String>,
    whiteout: bool,
    opaque: bool,
    uid: Option<u32>,
    uid_range: Option<String>,
    uid_excluded: Option<String>,
) -> Result<NmRuleSpec> {
    if whiteout {
        if real_path.is_some() {
            bail!("--whiteout 规则不接受真实路径");
        }
        if opaque {
            bail!("--whiteout 与 --opaque 互斥");
        }
    }
    let mut spec = if whiteout {
        NmRuleSpec::whiteout(virtual_path)
    } else {
        let real = real_path.ok_or_else(|| anyhow::anyhow!("缺少真实路径 (或使用 --whiteout)"))?;
        if opaque {
            NmRuleSpec::opaque(virtual_path, real)
        } else {
            NmRuleSpec::redirect(virtual_path, real)
        }
    };

    if uid.is_some() && uid_range.is_some() {
        bail!("--uid 与 --uid-range 不能同时使用");
    }
    if let Some(single) = uid {
        spec = spec.with_uid_range(single, single);
    }
    if let Some(range) = uid_range.as_ref() {
        let (start, end) = parse_uid_range(range)?;
        spec = spec.with_uid_range(start, end);
    }
    if let Some(range) = uid_excluded.as_ref() {
        let (start, end) = parse_uid_range(range)?;
        if start == 0 && end == 0 {
            bail!("--uid-excluded 需要一个有效区间");
        }
        spec = spec.with_uid_excluded(start, end);
    }
    spec.validate_public()?;
    Ok(spec)
}

/// 按虚拟路径删除规则 (支持 UID 区间规则的精确匹配)
fn remove_by_virtual(virtual_paths: &[String]) -> Result<()> {
    let rules = list_rules()?;
    let matched: Vec<_> = rules
        .into_iter()
        .filter(|r| virtual_paths.iter().any(|v| v == &r.virtual_path))
        .collect();
    if !matched.is_empty() {
        delete_rules(&matched)?;
    }
    super::persist_remove(virtual_paths);
    Ok(())
}

fn rule_to_line(rule: &super::NmRule) -> String {
    let mut notes = Vec::new();
    if rule.is_whiteout() {
        notes.push("whiteout");
    }
    if rule.is_opaque() {
        notes.push("opaque");
    }
    if rule.is_virtual_dir() {
        notes.push("virtual-dir");
    } else if rule.is_dir() {
        notes.push("dir");
    }
    if !rule.is_default_scope() {
        notes.push("uid-scope");
    }
    let suffix = if notes.is_empty() {
        String::new()
    } else {
        format!(" ({})", notes.join(","))
    };
    if rule.is_whiteout() {
        format!("{} [whiteout]{}", rule.virtual_path, suffix)
    } else {
        format!("{} -> {}{}", rule.virtual_path, rule.real_path, suffix)
    }
}

fn rules_to_json(rules: &[super::NmRule]) -> String {
    let items: Vec<_> = rules
        .iter()
        .map(|rule| {
            json!({
                "virtual": rule.virtual_path,
                "real": rule.real_path,
                "flags": rule.flags,
                "whiteout": rule.is_whiteout(),
                "opaque": rule.is_opaque(),
                "virtual_dir": rule.is_virtual_dir(),
                "is_dir": rule.is_dir(),
                "uid": rule.uid,
                "uid_end": rule.uid_end,
                "deny_start": rule.deny_start,
                "deny_end": rule.deny_end,
            })
        })
        .collect();
    serde_json::to_string(&items).unwrap_or_else(|_| "[]".to_string())
}

/// 转义 JSON 字符串 (供手写 JSON 使用)
pub fn escape_json(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 8);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}
