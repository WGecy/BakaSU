//! early-native 快照生成 (对齐 YukiSU yukizygisk_snapshot.cpp)
//!
//! preinit 阶段 (zygote 启动前、无 ksu 服务) 由内核直接读取该快照完成首阶段
//! native 模块加载，因此快照必须是纯二进制布局：
//! magic/version/header_size/entry_size 都必须与 uapi/yukizygisk.h 完全一致。

use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};

use super::{
    ENTRY_ABI32, ENTRY_ABI64, NATIVE_MODULE_ID_MAX, NATIVE_MODULE_PATH_MAX, NATIVE_TARGET_MAX,
    NATIVE_TARGET_NAME, NATIVE_TARGET_PATH, NATIVE_TARGET_VALUE_MAX, SNAPSHOT_ENTRY_SIZE,
    SNAPSHOT_FLAG_ABI32, SNAPSHOT_FLAG_ABI64, SNAPSHOT_FLAG_ENABLED, SNAPSHOT_HEADER_SIZE,
    SNAPSHOT_MAGIC, SNAPSHOT_PAYLOAD_FILES, SNAPSHOT_VERSION,
};
use crate::{android::module, defs};

const SNAPSHOT_DIR_NAME: &str = "yukizygisk";
const MANIFEST_NAME: &str = "native_snapshot.bin";
const MODULES_DIR_NAME: &str = "modules";
const LINKER64_PATH: &str = "/system/bin/linker64";
const LINKER32_PATH: &str = "/system/bin/linker";

/// ELF 相关常量。
const SHT_DYNSYM: u32 = 11;
const ELFCLASS32: u8 = 1;
const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1;
const EI_CLASS: usize = 4;
const EI_DATA: usize = 5;
const EI_NIDENT: usize = 16;

const DLOPEN_SYMBOLS: [&str; 2] = ["__loader_android_dlopen_ext", "android_dlopen_ext"];
const DLSYM_SYMBOLS: [&str; 2] = ["__loader_dlsym", "dlsym"];

/// 一条待写入快照的模块条目。
#[derive(Debug, Clone)]
struct SnapshotEntry {
    target_type: u8,
    flags: u8,
    module_id: String,
    target: String,
    lib_path: PathBuf,
}

/// early-native 快照所在的 preinit 根目录。
///
/// 内核只认 watchdog 与 default 两档；ksud 的 preinit 目录可能是 vold，
/// 此时必须回落到内核可读的目录，否则内核读不到快照。
fn preinit_root() -> &'static str {
    let preferred = module::preinit_ksu_dir();
    if preferred == defs::PREINIT_DIR_VOLD {
        if Path::new("/metadata/watchdog").is_dir() {
            return defs::PREINIT_DIR_WATCHDOG;
        }
        return defs::PREINIT_DIR_DEFAULT;
    }
    preferred
}

/// 内核可读的 preinit 根 (vold 不是内核可读目录，无需处理)。
fn all_roots() -> [&'static str; 2] {
    [defs::PREINIT_DIR_WATCHDOG, defs::PREINIT_DIR_DEFAULT]
}

fn snapshot_dir(root: &str) -> PathBuf {
    Path::new(root).join(SNAPSHOT_DIR_NAME)
}

fn metadata_con(root: &str) -> &'static str {
    if root == defs::PREINIT_DIR_VOLD {
        defs::VOLD_METADATA_FILE_CON
    } else {
        defs::METADATA_FILE_CON
    }
}

fn set_label(path: &Path, con: &str) {
    if let Err(err) = crate::android::restorecon::lsetfilecon(path, con) {
        log::warn!(
            "yukizygisk early: restorecon {} failed: {err:#}",
            path.display()
        );
    }
}

fn write_private(path: &Path, data: &[u8], con: &str) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(path, data).with_context(|| format!("write {}", path.display()))?;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o644))?;
    set_label(path, con);
    Ok(())
}

/// 删除两个 preinit 根下的快照目录。
pub fn clear() -> Result<()> {
    for root in all_roots() {
        let dir = snapshot_dir(root);
        match std::fs::remove_dir_all(&dir) {
            Ok(()) => log::info!("yukizygisk early: removed {}", dir.display()),
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => {}
            Err(err) => log::warn!("yukizygisk early: remove {} failed: {err}", dir.display()),
        }
    }
    Ok(())
}

/// 读取 ELF 的 class；不是合法 ELF 时返回 None。
fn elf_class(path: &Path) -> Option<u8> {
    use std::io::Read;

    let mut header = [0u8; EI_NIDENT];
    let mut file = std::fs::File::open(path).ok()?;
    file.read_exact(&mut header).ok()?;
    if header[0] != 0x7f || header[1] != b'E' || header[2] != b'L' || header[3] != b'F' {
        return None;
    }
    if header[EI_DATA] != ELFDATA2LSB {
        return None;
    }
    match header[EI_CLASS] {
        ELFCLASS32 | ELFCLASS64 => Some(header[EI_CLASS]),
        _ => None,
    }
}

/// 解析 zn_modules.txt 的一行。
fn parse_native_module_line(module_id: &str, base: &Path, line: &str) -> Option<SnapshotEntry> {
    if module_id.len() >= NATIVE_MODULE_ID_MAX {
        return None;
    }
    let trimmed = line.trim();
    if trimmed.is_empty() || trimmed.starts_with('#') {
        return None;
    }
    let mut tokens = trimmed.split_whitespace();
    let first = tokens.next()?;
    let (target_type, target) = first
        .strip_prefix("name=")
        .map(|rest| (NATIVE_TARGET_NAME, rest))
        .or_else(|| {
            first
                .strip_prefix("path=")
                .map(|rest| (NATIVE_TARGET_PATH, rest))
        })?;
    let target = target.to_string();
    if target.is_empty() || target.len() >= NATIVE_TARGET_VALUE_MAX {
        return None;
    }

    let next = tokens.next()?;
    if next == "companion" {
        // companion 库不参与 early load。
        return None;
    }
    let expanded = next
        .replace("${moduleId}", module_id)
        .replace("$moduleId", module_id);
    let lib_path = if expanded.starts_with('/') {
        PathBuf::from(expanded)
    } else {
        base.join(expanded)
    };
    if !lib_path.is_file() {
        log::warn!(
            "yukizygisk early: module {} library {} missing, skipped",
            module_id,
            lib_path.display()
        );
        return None;
    }
    let class = elf_class(&lib_path)?;
    Some(SnapshotEntry {
        target_type,
        flags: if class == ELFCLASS32 {
            ENTRY_ABI32
        } else {
            ENTRY_ABI64
        },
        module_id: module_id.to_string(),
        target,
        lib_path,
    })
}

/// 收集当前启用的模块根目录（modules_update 优先于 modules）。
fn collect_active_module_roots() -> Vec<(String, PathBuf)> {
    let mut roots: Vec<(String, PathBuf)> = Vec::new();
    for dir in [defs::MODULE_UPDATE_DIR, defs::MODULE_DIR] {
        let Ok(entries) = std::fs::read_dir(dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_dir() {
                continue;
            }
            let Some(name) = entry.file_name().to_str().map(str::to_string) else {
                continue;
            };
            if !is_safe_component(&name) || roots.iter().any(|(id, _)| *id == name) {
                continue;
            }
            if path.join(defs::DISABLE_FILE_NAME).exists()
                || path.join(defs::REMOVE_FILE_NAME).exists()
            {
                continue;
            }
            roots.push((name, path));
        }
    }
    roots
}

fn is_safe_component(name: &str) -> bool {
    !name.is_empty()
        && name.len() < NATIVE_MODULE_ID_MAX
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

/// 扫描所有模块的 zn_modules.txt，生成快照条目。
fn scan_native_modules() -> Vec<SnapshotEntry> {
    let mut entries = Vec::new();
    for (module_id, base) in collect_active_module_roots() {
        let manifest = base.join("zn_modules.txt");
        let Ok(text) = std::fs::read_to_string(&manifest) else {
            continue;
        };
        for line in text.lines() {
            if entries.len() >= NATIVE_TARGET_MAX {
                return entries;
            }
            if let Some(entry) = parse_native_module_line(&module_id, &base, line) {
                entries.push(entry);
            }
        }
    }
    entries
}

/// 是否存在 32 位 native 目标（决定是否需要 32 位守护进程）。
pub fn has_native_abi32_target() -> bool {
    scan_native_modules()
        .iter()
        .any(|entry| entry.flags & ENTRY_ABI32 != 0)
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    let slice = data.get(offset..offset + 4)?;
    Some(u32::from_le_bytes(slice.try_into().ok()?))
}

fn read_u16(data: &[u8], offset: usize) -> Option<u16> {
    let slice = data.get(offset..offset + 2)?;
    Some(u16::from_le_bytes(slice.try_into().ok()?))
}

fn read_u64(data: &[u8], offset: usize) -> Option<u64> {
    let slice = data.get(offset..offset + 8)?;
    Some(u64::from_le_bytes(slice.try_into().ok()?))
}

/// 在 linker 的 .dynsym 中解析符号的 st_value（相对 linker 的偏移）。
fn resolve_linker_symbol(path: &str, class: u8, want: &str) -> Option<u64> {
    let data = std::fs::read(path).ok()?;
    let is64 = class == ELFCLASS64;
    let (shoff, shentsize, shnum) = if is64 {
        (
            read_u64(&data, 0x28)? as usize,
            read_u16(&data, 0x3a)? as usize,
            read_u16(&data, 0x3c)? as usize,
        )
    } else {
        (
            read_u32(&data, 0x20)? as usize,
            read_u16(&data, 0x2e)? as usize,
            read_u16(&data, 0x30)? as usize,
        )
    };
    if shentsize == 0 || shnum == 0 || shoff == 0 {
        return None;
    }
    if shoff.checked_add(shentsize.checked_mul(shnum)?)? > data.len() {
        return None;
    }

    // (sh_type, sh_offset, sh_size, sh_link, sh_entsize)
    let section = |index: usize| -> Option<(u32, usize, usize, u32, usize)> {
        let base = shoff + index.checked_mul(shentsize)?;
        if is64 {
            Some((
                read_u32(&data, base)?,
                read_u64(&data, base + 24)? as usize,
                read_u64(&data, base + 32)? as usize,
                read_u32(&data, base + 40)?,
                read_u64(&data, base + 56)? as usize,
            ))
        } else {
            Some((
                read_u32(&data, base + 4)?,
                read_u32(&data, base + 16)? as usize,
                read_u32(&data, base + 20)? as usize,
                read_u32(&data, base + 24)?,
                read_u32(&data, base + 36)? as usize,
            ))
        }
    };

    for index in 0..shnum {
        let Some((sh_type, sh_offset, sh_size, sh_link, sh_entsize)) = section(index) else {
            continue;
        };
        if sh_type != SHT_DYNSYM || sh_entsize == 0 {
            continue;
        }
        let Some((_, str_offset, str_size, _, _)) = section(sh_link as usize) else {
            continue;
        };
        let Some(strs) = data.get(str_offset..str_offset.checked_add(str_size)?) else {
            continue;
        };
        let Some(syms) = data.get(sh_offset..sh_offset.checked_add(sh_size)?) else {
            continue;
        };

        for sym_index in 0..(syms.len() / sh_entsize) {
            let sym_base = sym_index * sh_entsize;
            let Some(st_name) = read_u32(syms, sym_base) else {
                continue;
            };
            if !string_matches(strs, st_name as usize, want) {
                continue;
            }
            let value = if is64 {
                read_u64(syms, sym_base + 8)?
            } else {
                read_u32(syms, sym_base + 4)? as u64
            };
            if value != 0 {
                return Some(value);
            }
        }
    }
    None
}

fn string_matches(table: &[u8], start: usize, want: &str) -> bool {
    let Some(rest) = table.get(start..) else {
        return false;
    };
    let Some(end) = rest.iter().position(|byte| *byte == 0) else {
        return false;
    };
    rest[..end] == *want.as_bytes()
}

fn resolve_first(path: &str, class: u8, candidates: &[&str]) -> u64 {
    for candidate in candidates {
        if let Some(value) = resolve_linker_symbol(path, class, candidate) {
            return value;
        }
    }
    0
}

fn file_size(path: &str) -> u64 {
    std::fs::metadata(path).map(|meta| meta.len()).unwrap_or(0)
}

/// 生成快照 header 的原始字节表示 (72 字节)。
#[allow(clippy::too_many_arguments)]
fn encode_header(
    load_flags: u16,
    flags: u32,
    count: u32,
    dlopen: u64,
    dlsym: u64,
    linker_size: u64,
    dlopen32: u64,
    dlsym32: u64,
    linker32_size: u64,
) -> [u8; SNAPSHOT_HEADER_SIZE] {
    let mut header = [0u8; SNAPSHOT_HEADER_SIZE];
    header[0..4].copy_from_slice(&SNAPSHOT_MAGIC.to_le_bytes());
    header[4..6].copy_from_slice(&SNAPSHOT_VERSION.to_le_bytes());
    header[6..8].copy_from_slice(&(SNAPSHOT_HEADER_SIZE as u16).to_le_bytes());
    header[8..10].copy_from_slice(&(SNAPSHOT_ENTRY_SIZE as u16).to_le_bytes());
    header[10..12].copy_from_slice(&load_flags.to_le_bytes());
    header[12..16].copy_from_slice(&flags.to_le_bytes());
    header[16..20].copy_from_slice(&count.to_le_bytes());
    header[24..32].copy_from_slice(&dlopen.to_le_bytes());
    header[32..40].copy_from_slice(&dlsym.to_le_bytes());
    header[40..48].copy_from_slice(&linker_size.to_le_bytes());
    header[48..56].copy_from_slice(&dlopen32.to_le_bytes());
    header[56..64].copy_from_slice(&dlsym32.to_le_bytes());
    header[64..72].copy_from_slice(&linker32_size.to_le_bytes());
    header
}

fn encode_entry(entry: &SnapshotEntry, arc_name: &str) -> [u8; SNAPSHOT_ENTRY_SIZE] {
    let mut buf = [0u8; SNAPSHOT_ENTRY_SIZE];
    buf[0] = entry.target_type;
    buf[1] = entry.flags;
    fill_cstr(&mut buf[4..4 + NATIVE_MODULE_ID_MAX], &entry.module_id);
    fill_cstr(&mut buf[68..68 + NATIVE_TARGET_VALUE_MAX], &entry.target);
    fill_cstr(&mut buf[324..324 + NATIVE_MODULE_PATH_MAX], arc_name);
    buf
}

fn fill_cstr(slot: &mut [u8], value: &str) {
    let limit = slot.len().saturating_sub(1).min(value.len());
    slot[..limit].copy_from_slice(&value.as_bytes()[..limit]);
}

/// 把已落盘的 payload 复制进快照目录。
fn stage_payload(snapshot_path: &Path, con: &str) -> Result<()> {
    for name in SNAPSHOT_PAYLOAD_FILES {
        let source = format!("{}{}", defs::YUKIZYGISK_LIB_DIR, name);
        let data = std::fs::read(&source).with_context(|| format!("read payload {source}"))?;
        write_private(&snapshot_path.join(name), &data, con)?;
    }
    Ok(())
}

/// 重新生成 early-native 快照。
pub fn refresh() -> Result<()> {
    if !super::is_enabled() || crate::android::utils::is_safe_mode() {
        clear()?;
        log::info!("yukizygisk early: snapshot cleared (disabled)");
        return Ok(());
    }
    let config = super::config::load();
    if !config.early_load {
        clear()?;
        log::info!("yukizygisk early: snapshot cleared (early_load off)");
        return Ok(());
    }

    let root = preinit_root();
    let dir = snapshot_dir(root);
    let con = metadata_con(root);
    let modules_dir = dir.join(MODULES_DIR_NAME);
    let _ = std::fs::remove_dir_all(&modules_dir);
    std::fs::create_dir_all(&modules_dir)
        .with_context(|| format!("create {}", modules_dir.display()))?;
    let _ = std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700));
    set_label(&dir, con);

    if let Err(err) = stage_payload(&dir, con) {
        clear()?;
        bail!("yukizygisk early: payload unavailable, snapshot cleared: {err:#}");
    }
    for legacy in ["libzygisk.so", "libyukizncore.so", "libyukilinker.so"] {
        let _ = std::fs::remove_file(dir.join(legacy));
    }

    let entries = scan_native_modules();
    let mut staged: Vec<(SnapshotEntry, String)> = Vec::with_capacity(entries.len());
    let mut has32 = false;
    let mut has64 = false;
    for (index, entry) in entries.into_iter().enumerate() {
        let arc_name = format!("{index:03}-{}.so", entry.module_id);
        let dst = modules_dir.join(&arc_name);
        let Ok(data) = std::fs::read(&entry.lib_path) else {
            log::warn!(
                "yukizygisk early: read {} failed, skipped",
                entry.lib_path.display()
            );
            continue;
        };
        if write_private(&dst, &data, con).is_err() {
            continue;
        }
        if entry.flags & ENTRY_ABI32 != 0 {
            has32 = true;
        } else {
            has64 = true;
        }
        staged.push((entry, dst.to_string_lossy().into_owned()));
    }

    let mut dlopen = 0u64;
    let mut dlsym = 0u64;
    let mut linker_size = 0u64;
    let mut dlopen32 = 0u64;
    let mut dlsym32 = 0u64;
    let mut linker32_size = 0u64;
    if has64 {
        dlopen = resolve_first(LINKER64_PATH, ELFCLASS64, &DLOPEN_SYMBOLS);
        dlsym = resolve_first(LINKER64_PATH, ELFCLASS64, &DLSYM_SYMBOLS);
        linker_size = file_size(LINKER64_PATH);
    }
    if has32 {
        dlopen32 = resolve_first(LINKER32_PATH, ELFCLASS32, &DLOPEN_SYMBOLS);
        dlsym32 = resolve_first(LINKER32_PATH, ELFCLASS32, &DLSYM_SYMBOLS);
        linker32_size = file_size(LINKER32_PATH);
    }

    if !staged.is_empty()
        && ((has64 && (dlopen == 0 || dlsym == 0 || linker_size == 0))
            || (has32 && (dlopen32 == 0 || dlsym32 == 0 || linker32_size == 0)))
    {
        clear()?;
        bail!("yukizygisk early: linker offsets unavailable, snapshot cleared");
    }

    let mut flags = SNAPSHOT_FLAG_ENABLED;
    if has32 {
        flags |= SNAPSHOT_FLAG_ABI32;
    }
    if has64 {
        flags |= SNAPSHOT_FLAG_ABI64;
    }
    let header = encode_header(
        config.load_flags(),
        flags,
        staged.len() as u32,
        dlopen,
        dlsym,
        linker_size,
        dlopen32,
        dlsym32,
        linker32_size,
    );

    let mut manifest =
        Vec::with_capacity(SNAPSHOT_HEADER_SIZE + staged.len() * SNAPSHOT_ENTRY_SIZE);
    manifest.extend_from_slice(&header);
    for (entry, arc_path) in &staged {
        manifest.extend_from_slice(&encode_entry(entry, arc_path));
    }

    let target = dir.join(MANIFEST_NAME);
    let tmp = dir.join(format!(".{MANIFEST_NAME}.tmp"));
    write_private(&tmp, &manifest, con)?;
    std::fs::rename(&tmp, &target).with_context(|| format!("rename {}", target.display()))?;

    let other = if root == defs::PREINIT_DIR_WATCHDOG {
        defs::PREINIT_DIR_DEFAULT
    } else {
        defs::PREINIT_DIR_WATCHDOG
    };
    let _ = std::fs::remove_dir_all(snapshot_dir(other));

    log::info!(
        "yukizygisk early: snapshot refreshed modules={} has32={has32} has64={has64}",
        staged.len()
    );
    Ok(())
}
