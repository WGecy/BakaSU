//! 模块规则收集与注入 (替代 maxsteeel/nomount 的 metamount.sh + rule-paths.sh)
//!
//! 三段式收集, 与参考实现的三个 find 轮次一一对应:
//! 1. 目录/字符设备/.replace/.remove → whiteout 规则 (隐藏底层同名项);
//! 2. 非空 .replace 目录 → opaque 规则 (目录不合并底层内容);
//! 3. 普通文件/符号链接 → 重定向规则.
//!
//! .replace 目录为空 (除 .replace/.remove 外无条目) 时退化为 whiteout,
//! 与 rule-paths.sh 的 kind=path 分支一致; 目录带 trusted.overlay.opaque=y
//! 属性时同样按 whiteout 处理.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use log::{info, warn};

use super::{NmClient, NmRule, NmRuleSpec};

/// 模块目录前缀 (供 snapshot 统计活跃模块数)
pub const MODULES_DIR_PREFIX: &str = "/data/adb/modules/";

const MODULES_DIR: &str = "/data/adb/modules";
/// 参考实现自身作为一个模块存在, 注入时要跳过
const NOMOUNT_MODULE_ID: &str = "nomount";

/// 需要遍历的模块分区 (与 metamount.sh 的 TARGET_PARTITIONS 一致)
pub const TARGET_PARTITIONS: [&str; 21] = [
    "system",
    "system_ext",
    "vendor",
    "odm",
    "product",
    "apex",
    "oem",
    "optics",
    "prism",
    "mi_ext",
    "my_bigball",
    "my_carrier",
    "my_company",
    "my_engineering",
    "my_heytap",
    "my_manifest",
    "my_preload",
    "my_product",
    "my_region",
    "my_reserve",
    "my_stock",
];

/// 模块内 system/<分区>/... 会被重映射为 /<分区>/... 的分区名
const REMAP_PARTITIONS: [&str; 9] = [
    "vendor",
    "system_ext",
    "product",
    "odm",
    "apex",
    "oem",
    "optics",
    "prism",
    "mi_ext",
];

/// 模块在管理器里的展示信息
#[derive(Debug, Default, Clone)]
pub struct ModuleInfo {
    pub id: String,
    pub name: String,
    pub version: String,
    pub author: String,
    pub description: String,
    pub disabled: bool,
    pub skip_mount: bool,
    /// 该模块会产生的规则条数
    pub file_count: usize,
    /// 当前在内核里生效的规则条数
    pub loaded: usize,
}

// ---------------------------------------------------------------------------
// 路径与分区判定
// ---------------------------------------------------------------------------

fn module_dir(id: &str) -> PathBuf {
    Path::new(MODULES_DIR).join(id)
}

/// 设备上确实存在该分区才注入, 避免往不存在的挂载点写规则
fn partition_present(partition: &str) -> bool {
    Path::new(&format!("/{partition}")).is_dir()
        || Path::new(&format!("/system/{partition}")).is_dir()
}

fn has_marker(dir: &Path, name: &str) -> bool {
    dir.join(name).exists()
}

/// 模块是否应跳过注入 (禁用 / 待卸载 / 标记 skip_mount / 参考实现自身)
fn should_skip(mod_dir: &Path, id: &str) -> bool {
    if id == NOMOUNT_MODULE_ID {
        return true;
    }
    has_marker(mod_dir, "disable")
        || has_marker(mod_dir, "remove")
        || has_marker(mod_dir, "skip_mount")
}

/// 目录里除 .replace / .remove 之外是否还有内容
fn has_real_content(dir: &Path) -> bool {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name != ".replace" && name != ".remove" {
            return true;
        }
    }
    false
}

/// 目录是否被标记为不透明 (overlayfs 语义)
fn dir_is_opaque(path: &Path) -> bool {
    match extattr::lgetxattr(path, "trusted.overlay.opaque") {
        Ok(value) => {
            let text = String::from_utf8_lossy(&value);
            text.trim().trim_matches('"') == "y"
        }
        Err(_) => false,
    }
}

/// 模块内绝对路径 → 虚拟路径 (含 system/<分区> 的重映射)
fn virtual_path(mod_dir: &Path, path: &Path) -> String {
    let rel = path
        .strip_prefix(mod_dir)
        .unwrap_or(path)
        .to_string_lossy()
        .replace('\\', "/");
    let full = format!("/{}", rel.trim_start_matches('/'));
    remap_system_partition(&full)
}

fn remap_system_partition(v: &str) -> String {
    let Some(rest) = v.strip_prefix("/system/") else {
        return v.to_string();
    };
    let (partition, tail) = match rest.split_once('/') {
        Some((p, t)) => (p, t),
        None => (rest, ""),
    };
    let is_remap = REMAP_PARTITIONS.contains(&partition) || partition.starts_with("my_");
    if is_remap {
        if tail.is_empty() {
            format!("/{partition}")
        } else {
            format!("/{partition}/{tail}")
        }
    } else {
        v.to_string()
    }
}

fn strip_marker(path: &str, marker: &str) -> String {
    path.strip_suffix(marker).unwrap_or(path).to_string()
}

// ---------------------------------------------------------------------------
// 规则收集
// ---------------------------------------------------------------------------

/// 收集一个模块会产生的全部规则 (顺序: whiteout → opaque → 重定向)
pub fn collect_module_specs(mod_dir: &Path) -> Vec<NmRuleSpec> {
    let mut whiteouts = Vec::new();
    let mut opaques = Vec::new();
    let mut redirects = Vec::new();

    for partition in TARGET_PARTITIONS {
        let root = mod_dir.join(partition);
        if !root.is_dir() || !partition_present(partition) {
            continue;
        }
        walk(&root, mod_dir, &mut whiteouts, &mut opaques, &mut redirects);
    }

    whiteouts.extend(opaques);
    whiteouts.extend(redirects);
    whiteouts
}

fn walk(
    dir: &Path,
    mod_dir: &Path,
    whiteouts: &mut Vec<NmRuleSpec>,
    opaques: &mut Vec<NmRuleSpec>,
    redirects: &mut Vec<NmRuleSpec>,
) {
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) => {
            warn!("nomount: 无法读取目录 {}: {e}", dir.display());
            return;
        }
    };

    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name();
        let name = name.to_string_lossy().to_string();
        let v = virtual_path(mod_dir, &path);

        // .replace: 空目录表示底层的同名目录要整个隐藏, 非空表示不合并底层内容
        if name == ".replace" {
            let target = strip_marker(&v, "/.replace");
            if has_real_content(dir) {
                opaques.push(NmRuleSpec::opaque(
                    target,
                    dir.to_string_lossy().to_string(),
                ));
            } else {
                whiteouts.push(NmRuleSpec::whiteout(target));
            }
            continue;
        }
        // .remove: 删除底层的同名项
        if name == ".remove" {
            whiteouts.push(NmRuleSpec::whiteout(strip_marker(&v, "/.remove")));
            continue;
        }

        let meta = match std::fs::symlink_metadata(&path) {
            Ok(m) => m,
            Err(_) => continue,
        };
        let ft = meta.file_type();

        if ft.is_symlink() {
            // 指向目标的真实路径由内核 LOOKUP_FOLLOW 解析
            redirects.push(NmRuleSpec::redirect(v, path.to_string_lossy().to_string()));
        } else if ft.is_dir() {
            if dir_is_opaque(&path) {
                whiteouts.push(NmRuleSpec::whiteout(v));
            }
            walk(&path, mod_dir, whiteouts, opaques, redirects);
        } else if ft.is_file() {
            redirects.push(NmRuleSpec::redirect(v, path.to_string_lossy().to_string()));
        } else {
            // 字符/块设备等特殊文件: 只做白化, 不做重定向
            whiteouts.push(NmRuleSpec::whiteout(v));
        }
    }
}

/// 模块会产生/已存在的全部虚拟路径 (卸载与统计用)
fn collect_module_paths(mod_dir: &Path) -> HashSet<String> {
    collect_module_specs(mod_dir)
        .into_iter()
        .map(|s| s.virtual_path)
        .collect()
}

// ---------------------------------------------------------------------------
// 模块枚举与注入
// ---------------------------------------------------------------------------

fn list_module_dirs() -> Vec<(String, PathBuf)> {
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir(MODULES_DIR) else {
        return out;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let id = entry.file_name().to_string_lossy().to_string();
        out.push((id, path));
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    out
}

/// 开机/启用时批量注入全部模块规则 (安全模式开启时直接跳过)
pub fn inject_modules() -> Result<()> {
    if super::safe_mode() {
        info!("nomount: 安全模式已开启, 跳过本次模块注入");
        return Ok(());
    }

    let mut specs = Vec::new();
    for (id, dir) in list_module_dirs() {
        if should_skip(&dir, &id) {
            continue;
        }
        specs.extend(collect_module_specs(&dir));
    }
    if specs.is_empty() {
        return Ok(());
    }
    let applied = NmClient::new()
        .context("连接 NoMount 内核模块失败")?
        .add_rules(&specs)?;
    info!("nomount: 注入 {applied} 条模块规则");
    Ok(())
}

/// 热挂载单个模块, 返回新增规则条数
pub fn load_module(id: &str) -> Result<usize> {
    let dir = module_dir(id);
    if !dir.is_dir() {
        bail!("模块不存在: {id}");
    }
    let specs = collect_module_specs(&dir);
    if specs.is_empty() {
        return Ok(0);
    }
    let applied = NmClient::new()?.add_rules(&specs)?;
    info!("nomount: 热挂载模块 {id}, 新增 {applied} 条规则");
    Ok(applied)
}

/// 热卸载单个模块, 返回删除的规则条数
pub fn unload_module(id: &str) -> Result<usize> {
    let dir = module_dir(id);
    if !dir.is_dir() {
        bail!("模块不存在: {id}");
    }
    let targets = collect_module_paths(&dir);
    if targets.is_empty() {
        return Ok(0);
    }
    let client = NmClient::new()?;
    // 以内核里的真实规则为准, 才能拿到正确的 UID 区间用于精确匹配
    let rules: Vec<NmRule> = client
        .list_rules()?
        .into_iter()
        .filter(|rule| targets.contains(&rule.virtual_path))
        .collect();
    if rules.is_empty() {
        return Ok(0);
    }
    let removed = client.del_rules(&rules)?;
    info!("nomount: 热卸载模块 {id}, 删除 {removed} 条规则");
    Ok(removed)
}

/// 模块开关 (写/删 disable 标记, 下次开新生效)
pub fn set_module_disabled(id: &str, disabled: bool) -> Result<()> {
    let dir = module_dir(id);
    if !dir.is_dir() {
        bail!("模块不存在: {id}");
    }
    let marker = dir.join("disable");
    if disabled {
        std::fs::write(&marker, "")?;
    } else if marker.exists() {
        std::fs::remove_file(&marker)?;
    }
    Ok(())
}

/// 所有已安装模块及其规则统计
pub fn modules() -> Result<Vec<ModuleInfo>> {
    let kernel_rules = NmClient::new()
        .and_then(|c| c.list_rules())
        .unwrap_or_default();
    let mut out = Vec::new();

    for (id, dir) in list_module_dirs() {
        let file_count = collect_module_specs(&dir).len();
        if file_count == 0 {
            continue;
        }
        let loaded = kernel_rules
            .iter()
            .filter(|rule| rule.real_path.starts_with(&format!("{MODULES_DIR}/{id}/")))
            .count();
        let prop = read_module_prop(&dir);
        out.push(ModuleInfo {
            id: id.clone(),
            name: prop.get("name").cloned().unwrap_or_else(|| id.clone()),
            version: prop.get("version").cloned().unwrap_or_default(),
            author: prop.get("author").cloned().unwrap_or_default(),
            description: prop.get("description").cloned().unwrap_or_default(),
            disabled: has_marker(&dir, "disable"),
            skip_mount: has_marker(&dir, "skip_mount"),
            file_count,
            loaded,
        });
    }
    Ok(out)
}

fn read_module_prop(dir: &Path) -> std::collections::HashMap<String, String> {
    let mut map = std::collections::HashMap::new();
    let Ok(content) = std::fs::read_to_string(dir.join("module.prop")) else {
        return map;
    };
    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some((key, value)) = line.split_once('=') {
            map.insert(key.trim().to_string(), value.trim().to_string());
        }
    }
    map
}
