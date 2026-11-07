//! NoMount 内核通信客户端 (移植自 maxsteeel/nomount userspace/src/nm.c)
//!
//! v2.2.0 起内核废弃了 NETLINK_GENERIC 通道, 改用 keyring 的 add_key 钩子:
//! 用户态把一页 (4096 字节, 页对齐) 负载的地址当作 add_key 的 payload 交给内核,
//! 内核在该页内解析并回写请求结果. add_key 本身固定返回 -ECANCELED,
//! 真正的执行结果只看负载里的 status 字段 (0=成功, 负值=-errno).

pub mod cli;
pub mod mount;

use std::mem::size_of;

use anyhow::{Result, bail};

/// 负载魔数 "NOMOUNT" (对应内核 NM_MAGIC_SIG)
const NM_MAGIC: u64 = 0x004E_4F4D_4F55_4E54;

/// 整个负载固定占一页
const NM_PAYLOAD_SIZE: usize = 4096;
/// 头部 (magic 8 + cmd 4 + target_uid 4 + status 4 + arg1 4 + data_size 4)
const NM_HEADER_SIZE: usize = 28;
/// 数据区大小
const NM_BUFFER_SIZE: usize = NM_PAYLOAD_SIZE - NM_HEADER_SIZE;

/// 规则头: flags/uid/uid_end/deny_start/deny_end 各 4 字节 + v_len/r_len 各 2 字节
const NM_RULE_HDR_SIZE: usize = 24;
/// 删除规则头: uid/uid_end/deny_start/deny_end 各 4 字节 + v_len 2 字节
const NM_DEL_HDR_SIZE: usize = 18;

/// 规则目标是一个目录 (内核按真实路径 stat 后自动补)
pub const NM_FLAG_IS_DIR: u32 = 1 << 0;
/// 纯虚拟目录占位 (删除子规则后残留的 stub, 不可删除)
pub const NM_FLAG_VIRTUAL_DIR: u32 = 1 << 1;
/// 隐藏该虚拟路径 (whiteout)
pub const NM_FLAG_WHITEOUT: u32 = 1 << 2;
/// 目录不透明 (不合并底层目录内容)
pub const NM_FLAG_OPAQUE: u32 = 1 << 3;

const NM_CMD_GET_VERSION: u32 = 1;
const NM_CMD_ADD_RULE: u32 = 2;
const NM_CMD_DEL_RULE: u32 = 3;
const NM_CMD_ADD_UID: u32 = 4;
const NM_CMD_DEL_UID: u32 = 5;
const NM_CMD_CLEAR_ALL: u32 = 6;
const NM_CMD_CLEAR_RULES: u32 = 7;
const NM_CMD_CLEAR_UIDS: u32 = 8;
const NM_CMD_GET_LIST: u32 = 9;
const NM_CMD_GET_UIDS: u32 = 10;
const NM_CMD_BLOCK_ISOLATED_UIDS: u32 = 11;
const NM_CMD_GET_ISOLATED_STATE: u32 = 12;

const _: () = assert!(NM_HEADER_SIZE + NM_BUFFER_SIZE == NM_PAYLOAD_SIZE);

/// 内核侧 struct nm_payload (packed), 必须与之一字不差
#[repr(C)]
struct NmPayload {
    magic: u64,
    cmd: u32,
    target_uid: u32,
    status: i32,
    arg1: u32,
    data_size: u32,
    buffer: [u8; NM_BUFFER_SIZE],
}

/// 负载必须是页对齐且不跨页 — 内核要求 offset_in_page(addr) + 4096 <= PAGE_SIZE
#[repr(C, align(4096))]
struct NmWorkspace {
    payload: NmPayload,
}

/// 内核里的一条重定向规则
#[derive(Debug, Clone)]
pub struct NmRule {
    pub virtual_path: String,
    pub real_path: String,
    pub flags: u32,
    pub uid: u32,
    pub uid_end: u32,
    pub deny_start: u32,
    pub deny_end: u32,
}

impl NmRule {
    pub fn is_dir(&self) -> bool {
        self.flags & NM_FLAG_IS_DIR != 0
    }

    pub fn is_virtual_dir(&self) -> bool {
        self.flags & NM_FLAG_VIRTUAL_DIR != 0
    }

    pub fn is_whiteout(&self) -> bool {
        self.flags & NM_FLAG_WHITEOUT != 0
    }

    pub fn is_opaque(&self) -> bool {
        self.flags & NM_FLAG_OPAQUE != 0
    }

    /// 默认规则 (对全部 UID 生效)
    pub fn is_default_scope(&self) -> bool {
        self.uid == 0 && self.uid_end == 0
    }
}

/// 对外输出的规则视图 (字段名与内核结构无关, 供 CLI 与管理器读取)
#[derive(serde::Serialize)]
pub struct RuleView<'a> {
    #[serde(rename = "virtual")]
    pub virtual_path: &'a str,
    #[serde(rename = "real")]
    pub real_path: &'a str,
    pub flags: u32,
    pub whiteout: bool,
    pub opaque: bool,
    pub virtual_dir: bool,
    pub is_dir: bool,
    pub uid: u32,
    pub uid_end: u32,
    pub deny_start: u32,
    pub deny_end: u32,
}

impl<'a> From<&'a NmRule> for RuleView<'a> {
    fn from(rule: &'a NmRule) -> Self {
        RuleView {
            virtual_path: &rule.virtual_path,
            real_path: &rule.real_path,
            flags: rule.flags,
            whiteout: rule.is_whiteout(),
            opaque: rule.is_opaque(),
            virtual_dir: rule.is_virtual_dir(),
            is_dir: rule.is_dir(),
            uid: rule.uid,
            uid_end: rule.uid_end,
            deny_start: rule.deny_start,
            deny_end: rule.deny_end,
        }
    }
}

/// 待下发给内核的规则 (新增时用, 带完整 flags/UID 区间)
#[derive(Debug, Clone)]
pub struct NmRuleSpec {
    pub virtual_path: String,
    pub real_path: String,
    pub flags: u32,
    pub uid: u32,
    pub uid_end: u32,
    pub deny_start: u32,
    pub deny_end: u32,
}

impl NmRuleSpec {
    /// 普通重定向 (虚拟路径 → 真实路径)
    pub fn redirect(virtual_path: impl Into<String>, real_path: impl Into<String>) -> Self {
        Self {
            virtual_path: virtual_path.into(),
            real_path: real_path.into(),
            flags: 0,
            uid: 0,
            uid_end: 0,
            deny_start: 0,
            deny_end: 0,
        }
    }

    /// 隐藏虚拟路径
    pub fn whiteout(virtual_path: impl Into<String>) -> Self {
        Self {
            virtual_path: virtual_path.into(),
            real_path: String::new(),
            flags: NM_FLAG_WHITEOUT,
            uid: 0,
            uid_end: 0,
            deny_start: 0,
            deny_end: 0,
        }
    }

    /// 不透明目录重定向
    pub fn opaque(virtual_path: impl Into<String>, real_path: impl Into<String>) -> Self {
        Self {
            virtual_path: virtual_path.into(),
            real_path: real_path.into(),
            flags: NM_FLAG_OPAQUE,
            uid: 0,
            uid_end: 0,
            deny_start: 0,
            deny_end: 0,
        }
    }

    /// 限定生效的 UID 区间
    pub fn with_uid_range(mut self, start: u32, end: u32) -> Self {
        self.uid = start;
        self.uid_end = end;
        self
    }

    /// 限定区间内被排除的 UID 子区间
    pub fn with_uid_excluded(mut self, start: u32, end: u32) -> Self {
        self.deny_start = start;
        self.deny_end = end;
        self
    }

    /// 参数校验 (下发给内核与写持久化之前都走这里)
    pub fn validate(&self) -> Result<()> {
        if self.virtual_path.is_empty() {
            bail!("虚拟路径不能为空");
        }
        if self.flags & NM_FLAG_WHITEOUT == 0
            && self.flags & NM_FLAG_VIRTUAL_DIR == 0
            && self.real_path.is_empty()
        {
            bail!("真实路径不能为空");
        }
        if self.flags & NM_FLAG_WHITEOUT != 0 && self.flags & NM_FLAG_OPAQUE != 0 {
            bail!("--whiteout 与 --opaque 互斥");
        }
        if self.deny_start != 0 || self.deny_end != 0 {
            if self.deny_start == 0 || self.deny_end == 0 {
                bail!("UID 排除区间必须同时给出起止值");
            }
            if self.deny_start > self.deny_end {
                bail!("UID 排除区间起点不能大于终点");
            }
            if self.deny_start < self.uid || self.deny_end > self.uid_end {
                bail!("UID 排除区间必须落在生效 UID 区间内");
            }
        }
        Ok(())
    }
}

/// NoMount 通道句柄 (每次命令都是一次独立的 add_key 往返, 无长连接)。
/// 通道无状态, 构造时不探版本: 未集成 NoMount 的内核会在 dispatch 后以
/// "内核未响应 NoMount 请求" 报错, 与探测失败是同一个结论, 却省一次往返。
#[derive(Default)]
pub struct NmClient;

impl NmClient {
    pub const fn new() -> Self {
        NmClient
    }

    /// 批量添加规则 (自动按 4068 字节分片)
    pub fn add_rules(&self, rules: &[NmRuleSpec]) -> Result<usize> {
        let mut records = Vec::with_capacity(rules.len());
        let mut skipped = 0usize;
        for rule in rules {
            if rule.validate().is_err() {
                skipped += 1;
                continue;
            }
            match encode_add_rule(rule) {
                Some(rec) => records.push(rec),
                None => skipped += 1,
            }
        }
        if skipped > 0 {
            log::warn!("nomount: 跳过 {skipped} 条非法或超长规则");
        }
        send_records(NM_CMD_ADD_RULE, &records)?;
        Ok(rules.len() - skipped)
    }

    /// 批量删除规则 (按规则自身的 uid/deny 区间精确定位)
    pub fn del_rules(&self, rules: &[NmRule]) -> Result<usize> {
        let mut records = Vec::with_capacity(rules.len());
        let mut skipped = 0usize;
        for rule in rules {
            // 纯虚拟目录占位规则删不掉, 内核会直接返回 0, 跳过省一次往返
            if rule.is_virtual_dir() {
                skipped += 1;
                continue;
            }
            match encode_del_rule(rule) {
                Some(rec) => records.push(rec),
                None => skipped += 1,
            }
        }
        send_records(NM_CMD_DEL_RULE, &records)?;
        Ok(rules.len() - skipped)
    }

    /// 规则全量列表 (分页读取, 内核每次最多回 4068 字节)
    pub fn list_rules(&self) -> Result<Vec<NmRule>> {
        let mut rules = Vec::new();
        let mut cursor = 0u32;
        loop {
            let mut ws = new_payload(NM_CMD_GET_LIST);
            ws.payload.arg1 = cursor;
            dispatch(&mut ws)?;
            check_status(ws.payload.status)?;
            let next = ws.payload.arg1;
            let data = payload_bytes(&ws);
            decode_rules(data, &mut rules)?;
            if data.is_empty() || next <= cursor {
                break;
            }
            cursor = next;
        }
        Ok(rules)
    }

    /// UID 排除列表 (该 uid 的进程不做路径重定向)
    pub fn list_uids(&self) -> Result<Vec<u32>> {
        let mut uids = Vec::new();
        let mut cursor = 0u32;
        loop {
            let mut ws = new_payload(NM_CMD_GET_UIDS);
            ws.payload.arg1 = cursor;
            dispatch(&mut ws)?;
            check_status(ws.payload.status)?;
            let next = ws.payload.arg1;
            let data = payload_bytes(&ws);
            let (whole, _) = data.as_chunks::<4>();
            uids.extend(whole.iter().map(|chunk| u32::from_ne_bytes(*chunk)));
            if data.is_empty() || next <= cursor {
                break;
            }
            cursor = next;
        }
        uids.sort_unstable();
        uids.dedup();
        Ok(uids)
    }

    /// 清空全部 (规则 + UID 列表)
    pub fn clear(&self) -> Result<()> {
        self.send_simple(NM_CMD_CLEAR_ALL, 0)
    }

    /// 只清空规则
    pub fn clear_rules(&self) -> Result<()> {
        self.send_simple(NM_CMD_CLEAR_RULES, 0)
    }

    /// 只清空 UID 排除列表
    pub fn clear_uids(&self) -> Result<()> {
        self.send_simple(NM_CMD_CLEAR_UIDS, 0)
    }

    /// UID 排除: 该 uid 的进程不做路径重定向
    pub fn block_uid(&self, uid: u32) -> Result<()> {
        self.set_uid(NM_CMD_ADD_UID, uid)
    }

    pub fn unblock_uid(&self, uid: u32) -> Result<()> {
        self.set_uid(NM_CMD_DEL_UID, uid)
    }

    /// 拦截隔离进程 (90000-99999), 内核进程内静态开关, 重启失效
    pub fn set_block_isolated(&self, blocked: bool) -> Result<()> {
        self.send_simple(NM_CMD_BLOCK_ISOLATED_UIDS, u32::from(blocked))
    }

    /// 查询隔离进程拦截开关
    pub fn block_isolated(&self) -> Result<bool> {
        let mut ws = new_payload(NM_CMD_GET_ISOLATED_STATE);
        dispatch(&mut ws)?;
        check_status(ws.payload.status)?;
        let text = String::from_utf8_lossy(payload_bytes(&ws));
        Ok(text.trim() == "1")
    }

    fn send_simple(&self, cmd: u32, arg1: u32) -> Result<()> {
        let mut ws = new_payload(cmd);
        ws.payload.arg1 = arg1;
        dispatch(&mut ws)?;
        check_status(ws.payload.status)
    }

    fn set_uid(&self, cmd: u32, uid: u32) -> Result<()> {
        let mut ws = new_payload(cmd);
        ws.payload.target_uid = uid;
        dispatch(&mut ws)?;
        check_status(ws.payload.status)
    }
}

/// 构造全零负载并写好魔数 (缓冲区内是给内核的数据)
fn new_payload(cmd: u32) -> Box<NmWorkspace> {
    Box::new(NmWorkspace {
        payload: NmPayload {
            magic: NM_MAGIC,
            cmd,
            target_uid: 0,
            status: -1,
            arg1: 0,
            data_size: 0,
            buffer: [0u8; NM_BUFFER_SIZE],
        },
    })
}

/// 内核回写的有效数据区 (data_size 是内核给出的长度, 夹紧到缓冲区大小)
fn payload_bytes(ws: &NmWorkspace) -> &[u8] {
    &ws.payload.buffer[..(ws.payload.data_size as usize).min(NM_BUFFER_SIZE)]
}

/// 调用 add_key("nomount", "trigger", &addr, 8, -1) 把负载交给内核
fn dispatch(ws: &mut NmWorkspace) -> Result<()> {
    ws.payload.status = -1;
    let mut addr = &ws.payload as *const NmPayload as u64;
    // 内核的 key preparse 钩子固定返回 -ECANCELED, 返回值本身无意义
    unsafe {
        libc::syscall(
            libc::SYS_add_key,
            c"nomount".as_ptr(),
            c"trigger".as_ptr(),
            &mut addr as *mut u64 as *const libc::c_void,
            size_of::<u64>(),
            -1i32,
        );
    }
    Ok(())
}

/// 内核返回的负 errno
///
/// 单独成型以便调用方按 errno 判定幂等场景 (-EEXIST / -ENOENT), 不必解析错误文本。
#[derive(Debug)]
struct KernelError(i32);

impl std::fmt::Display for KernelError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "内核返回错误: errno {}", -self.0)
    }
}

impl std::error::Error for KernelError {}

/// uid 已在排除列表里: 内核返回 -EEXIST, 对调用方来说等同成功
fn ignore_uid_exists(error: anyhow::Error) -> Result<()> {
    ignore_errno(error, -libc::EEXIST)
}

/// uid 不在排除列表里: 内核返回 -ENOENT
fn ignore_uid_missing(error: anyhow::Error) -> Result<()> {
    ignore_errno(error, -libc::ENOENT)
}

fn ignore_errno(error: anyhow::Error, expected: i32) -> Result<()> {
    let ignored = matches!(
        error.downcast_ref::<KernelError>(),
        Some(KernelError(code)) if *code == expected
    );
    if ignored { Ok(()) } else { Err(error) }
}

fn check_status(status: i32) -> Result<()> {
    match status {
        0 => Ok(()),
        // -1 是我们自己写的初值: 内核根本没进处理流程 (缺少 CAP_SYS_ADMIN 或未集成 NoMount)
        -1 => bail!("内核未响应 NoMount 请求 (未集成 NoMount 或缺 CAP_SYS_ADMIN)"),
        code => Err(KernelError(code).into()),
    }
}

fn read_version() -> Result<String> {
    let mut ws = new_payload(NM_CMD_GET_VERSION);
    dispatch(&mut ws)?;
    check_status(ws.payload.status)?;
    let text = String::from_utf8_lossy(payload_bytes(&ws))
        .trim()
        .to_string();
    if text.is_empty() {
        bail!("内核没有返回 NoMount 版本号");
    }
    Ok(text)
}

fn parse_version(text: &str) -> u32 {
    let head: String = text.chars().take_while(|c| c.is_ascii_digit()).collect();
    head.parse().unwrap_or(0)
}

/// 单条新增规则记录: nm_rule_hdr + virtual + real
fn encode_add_rule(rule: &NmRuleSpec) -> Option<Vec<u8>> {
    let vb = rule.virtual_path.as_bytes();
    let rb = rule.real_path.as_bytes();
    if vb.is_empty() {
        return None;
    }
    if rule.flags & NM_FLAG_WHITEOUT == 0 && rb.is_empty() {
        return None;
    }
    let total = NM_RULE_HDR_SIZE + vb.len() + rb.len();
    if vb.len() > u16::MAX as usize || rb.len() > u16::MAX as usize || total > NM_BUFFER_SIZE {
        return None;
    }
    let mut out = Vec::with_capacity(total);
    out.extend_from_slice(&rule.flags.to_ne_bytes());
    out.extend_from_slice(&rule.uid.to_ne_bytes());
    out.extend_from_slice(&rule.uid_end.to_ne_bytes());
    out.extend_from_slice(&rule.deny_start.to_ne_bytes());
    out.extend_from_slice(&rule.deny_end.to_ne_bytes());
    out.extend_from_slice(&(vb.len() as u16).to_ne_bytes());
    out.extend_from_slice(&(rb.len() as u16).to_ne_bytes());
    out.extend_from_slice(vb);
    out.extend_from_slice(rb);
    Some(out)
}

/// 单条删除规则记录: nm_del_hdr + virtual
fn encode_del_rule(rule: &NmRule) -> Option<Vec<u8>> {
    let vb = rule.virtual_path.as_bytes();
    if vb.is_empty() || vb.len() > u16::MAX as usize {
        return None;
    }
    if NM_DEL_HDR_SIZE + vb.len() > NM_BUFFER_SIZE {
        return None;
    }
    let mut out = Vec::with_capacity(NM_DEL_HDR_SIZE + vb.len());
    out.extend_from_slice(&rule.uid.to_ne_bytes());
    out.extend_from_slice(&rule.uid_end.to_ne_bytes());
    out.extend_from_slice(&rule.deny_start.to_ne_bytes());
    out.extend_from_slice(&rule.deny_end.to_ne_bytes());
    out.extend_from_slice(&(vb.len() as u16).to_ne_bytes());
    out.extend_from_slice(vb);
    Some(out)
}

/// 把记录按 4068 字节切块并逐块发送
fn send_records(cmd: u32, records: &[Vec<u8>]) -> Result<()> {
    if records.is_empty() {
        return Ok(());
    }
    let mut chunk: Vec<u8> = Vec::new();
    let mut chunks: Vec<Vec<u8>> = Vec::new();
    for record in records {
        if !chunk.is_empty() && chunk.len() + record.len() > NM_BUFFER_SIZE {
            chunks.push(std::mem::take(&mut chunk));
        }
        chunk.extend_from_slice(record);
    }
    if !chunk.is_empty() {
        chunks.push(chunk);
    }

    for data in chunks {
        let mut ws = new_payload(cmd);
        ws.payload.data_size = data.len() as u32;
        ws.payload.buffer[..data.len()].copy_from_slice(&data);
        dispatch(&mut ws)?;
        check_status(ws.payload.status)?;
        // 内核回写已消费的偏移, 不完整说明尾部被丢弃
        if ws.payload.arg1 as usize != data.len() {
            bail!(
                "内核只处理了 {} / {} 字节的 NoMount 请求",
                ws.payload.arg1,
                data.len()
            );
        }
    }
    Ok(())
}

/// 解析 GET_LIST 回传的数据区: 逐条 nm_rule_hdr + virtual + real
fn decode_rules(data: &[u8], out: &mut Vec<NmRule>) -> Result<()> {
    let mut off = 0usize;
    while data.len() >= off + NM_RULE_HDR_SIZE {
        let flags = u32::from_ne_bytes(data[off..off + 4].try_into()?);
        let uid = u32::from_ne_bytes(data[off + 4..off + 8].try_into()?);
        let uid_end = u32::from_ne_bytes(data[off + 8..off + 12].try_into()?);
        let deny_start = u32::from_ne_bytes(data[off + 12..off + 16].try_into()?);
        let deny_end = u32::from_ne_bytes(data[off + 16..off + 20].try_into()?);
        let v_len = u16::from_ne_bytes(data[off + 20..off + 22].try_into()?) as usize;
        let r_len = u16::from_ne_bytes(data[off + 22..off + 24].try_into()?) as usize;
        off += NM_RULE_HDR_SIZE;
        if off + v_len + r_len > data.len() {
            bail!("NoMount 规则列表数据残缺");
        }
        let virtual_path = String::from_utf8_lossy(&data[off..off + v_len]).into_owned();
        off += v_len;
        let real_path = String::from_utf8_lossy(&data[off..off + r_len]).into_owned();
        off += r_len;
        out.push(NmRule {
            virtual_path,
            real_path,
            flags,
            uid,
            uid_end,
            deny_start,
            deny_end,
        });
    }
    Ok(())
}

/// 解析 "a-b" / "a" / "all" 形式的 UID 区间
pub fn parse_uid_range(text: &str) -> Result<(u32, u32)> {
    let text = text.trim();
    if text.eq_ignore_ascii_case("all") {
        return Ok((0, u32::MAX));
    }
    if let Some((start, end)) = text.split_once('-') {
        let start: u32 = start
            .trim()
            .parse()
            .map_err(|_| anyhow::anyhow!("UID 区间起点非法: {text}"))?;
        let end: u32 = end
            .trim()
            .parse()
            .map_err(|_| anyhow::anyhow!("UID 区间终点非法: {text}"))?;
        if start > end {
            return Ok((end, start));
        }
        Ok((start, end))
    } else {
        let uid: u32 = text
            .parse()
            .map_err(|_| anyhow::anyhow!("UID 非法: {text}"))?;
        Ok((uid, uid))
    }
}

// ---------------------------------------------------------------------------
// 顶层接口 (供 cli / init_event 使用)
// ---------------------------------------------------------------------------

/// 内核版本串 (含 commit 计数), 界面/诊断用
pub fn version() -> Result<String> {
    read_version()
}

pub fn list_rules() -> Result<Vec<NmRule>> {
    NmClient::new().list_rules()
}

pub fn list_uids() -> Result<Vec<u32>> {
    NmClient::new().list_uids()
}

/// 添加规则 (只下发到内核, 不写持久化)
pub fn add_specs(specs: &[NmRuleSpec]) -> Result<usize> {
    NmClient::new().add_rules(specs)
}

/// 删除规则 (按内核返回的完整规则描述精确定位)
pub fn delete_rules(rules: &[NmRule]) -> Result<usize> {
    NmClient::new().del_rules(rules)
}

pub fn clear_rules() -> Result<()> {
    NmClient::new().clear_rules()
}

pub fn clear_uids() -> Result<()> {
    NmClient::new().clear_uids()
}

/// 清空内核全部规则与 UID 列表, 同时丢弃自定义规则持久化
pub fn clear() -> Result<()> {
    NmClient::new().clear()?;
    let _ = std::fs::remove_file(CUSTOM_RULES_FILE);
    write_exclusions(&[])?;
    Ok(())
}

/// UID 排除: 指定 uid 的进程不做路径重定向
pub fn block_uid(uid: u32) -> Result<()> {
    NmClient::new().block_uid(uid)
}

pub fn unblock_uid(uid: u32) -> Result<()> {
    NmClient::new().unblock_uid(uid)
}

// ---------------------------------------------------------------------------
// 总开关
// ---------------------------------------------------------------------------

/// 总开关状态文件 (1=启用)
pub const ENABLED_FILE: &str = "/data/adb/ksu/nomount/enabled";
/// 安全模式标志: 存在则下次开机跳过注入 (等价于参考实现的 NOMOUNT_DATA/disable)
pub const SAFE_MODE_FILE: &str = "/data/adb/ksu/nomount/disable";
/// 隔离进程拦截的持久化标志
pub const ISOLATED_FLAG_FILE: &str = "/data/adb/ksu/nomount/.block_isolated_uids";

const DATA_DIR: &str = "/data/adb/ksu/nomount";

fn ensure_data_dir() -> Result<()> {
    std::fs::create_dir_all(DATA_DIR)?;
    Ok(())
}

pub fn is_enabled() -> bool {
    std::fs::read_to_string(ENABLED_FILE)
        .map(|s| s.trim() == "1")
        .unwrap_or(false)
}

pub fn set_enabled(enabled: bool) -> Result<()> {
    ensure_data_dir()?;
    if enabled {
        // 启用: 注入模块规则 + 重放自定义规则 + 恢复开机状态
        mount::inject_modules()?;
        replay_custom_rules()?;
        apply_boot_state()?;
    } else {
        // 禁用: 清空全部规则
        NmClient::new().clear()?;
    }
    // 先做再记: 上面任何一步失败都不留下"已启用但没有任何规则"的状态
    std::fs::write(ENABLED_FILE, if enabled { "1" } else { "0" })?;
    Ok(())
}

/// 安全模式: 打开后下次开机不注入任何规则 (排查卡开机)
pub fn safe_mode() -> bool {
    std::path::Path::new(SAFE_MODE_FILE).exists()
}

pub fn set_safe_mode(enabled: bool) -> Result<()> {
    ensure_data_dir()?;
    if enabled {
        std::fs::write(SAFE_MODE_FILE, "")?;
    } else {
        let _ = std::fs::remove_file(SAFE_MODE_FILE);
    }
    Ok(())
}

/// 隔离进程拦截开关持久化 + 立即应用
pub fn set_isolated_blocking(enabled: bool) -> Result<()> {
    ensure_data_dir()?;
    NmClient::new().set_block_isolated(enabled)?;
    if enabled {
        std::fs::write(ISOLATED_FLAG_FILE, "1")?;
    } else {
        let _ = std::fs::remove_file(ISOLATED_FLAG_FILE);
    }
    Ok(())
}

pub fn isolated_blocking() -> Result<bool> {
    NmClient::new().block_isolated()
}

fn persisted_isolated_flag() -> bool {
    std::path::Path::new(ISOLATED_FLAG_FILE).exists()
}

/// 开机/启用后恢复隔离进程拦截 (内核开关是内存态, 重启丢失)
pub fn apply_isolated_state() -> Result<()> {
    if persisted_isolated_flag() {
        NmClient::new().set_block_isolated(true)?;
    }
    Ok(())
}

/// 开机恢复: 排除列表 + 隔离拦截开关 (在模块注入之后调用)
pub fn apply_boot_state() -> Result<()> {
    replay_exclusions()?;
    apply_isolated_state()?;
    Ok(())
}

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------

/// 持久化排除列表 (uid 空白/逗号分隔)
pub const EXCLUSION_FILE: &str = "/data/adb/ksu/nomount/.exclusion_list";

/// 手动自定义规则持久化文件 (flags\tuid\tuid_end\tdeny_start\tdeny_end\tv\tr 每行)
pub const CUSTOM_RULES_FILE: &str = "/data/adb/ksu/nomount/custom_rules";

fn read_custom_rules() -> Vec<NmRuleSpec> {
    let content = match std::fs::read_to_string(CUSTOM_RULES_FILE) {
        Ok(c) => c,
        Err(_) => return Vec::new(),
    };
    let mut specs = Vec::new();
    for line in content.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let parts: Vec<&str> = line.split('\t').collect();
        if parts.len() == 7 {
            let parsed = parts[..5]
                .iter()
                .map(|p| p.trim().parse::<u32>().ok())
                .collect::<Option<Vec<u32>>>();
            if let Some(nums) = parsed {
                let spec = NmRuleSpec {
                    flags: nums[0],
                    uid: nums[1],
                    uid_end: nums[2],
                    deny_start: nums[3],
                    deny_end: nums[4],
                    virtual_path: parts[5].to_string(),
                    real_path: parts[6].to_string(),
                };
                if !spec.virtual_path.is_empty() {
                    specs.push(spec);
                }
                continue;
            }
        }
        // 兼容旧格式: v<TAB>r
        if parts.len() == 2 {
            let (v, r) = (parts[0], parts[1]);
            if !v.is_empty() && !r.is_empty() {
                specs.push(NmRuleSpec::redirect(v, r));
            }
        }
    }
    specs
}

fn write_custom_rules(specs: &[NmRuleSpec]) -> Result<()> {
    let content = specs
        .iter()
        .map(|s| {
            format!(
                "{}\t{}\t{}\t{}\t{}\t{}\t{}",
                s.flags, s.uid, s.uid_end, s.deny_start, s.deny_end, s.virtual_path, s.real_path
            )
        })
        .collect::<Vec<_>>()
        .join("\n");
    ensure_data_dir()?;
    std::fs::write(CUSTOM_RULES_FILE, content)?;
    Ok(())
}

/// 记录一条自定义规则 (同虚拟路径 + 同 UID 区间覆盖旧记录)
fn persist_add_specs(specs: &[NmRuleSpec]) -> Result<()> {
    let mut stored = read_custom_rules();
    for spec in specs {
        stored.retain(|old| {
            !(old.virtual_path == spec.virtual_path
                && old.uid == spec.uid
                && old.uid_end == spec.uid_end
                && old.deny_start == spec.deny_start
                && old.deny_end == spec.deny_end)
        });
        stored.push(spec.clone());
    }
    write_custom_rules(&stored)
}

fn persist_remove_rules(virtuals: &[String]) -> Result<()> {
    let mut stored = read_custom_rules();
    stored.retain(|old| !virtuals.contains(&old.virtual_path));
    write_custom_rules(&stored)
}

/// 持久化一条手动添加的规则 (cli 用)
pub fn persist_spec(spec: &NmRuleSpec) -> Result<()> {
    persist_add_specs(std::slice::from_ref(spec))
}

/// 持久化删除 (cli 用)
pub fn persist_remove(virtuals: &[String]) -> Result<()> {
    persist_remove_rules(virtuals)
}

/// 开机重放手动自定义规则 (在模块注入后调用)
pub fn replay_custom_rules() -> Result<()> {
    let specs = read_custom_rules();
    if specs.is_empty() {
        return Ok(());
    }
    let applied = NmClient::new().add_rules(&specs)?;
    log::info!("nomount: 重放 {applied} 条自定义规则");
    Ok(())
}

fn read_exclusions() -> Result<Vec<u32>> {
    let content = match std::fs::read_to_string(EXCLUSION_FILE) {
        Ok(c) => c,
        Err(_) => return Ok(Vec::new()),
    };
    let mut uids = Vec::new();
    for part in content.split(|c: char| c.is_whitespace() || c == ',') {
        if let Ok(uid) = part.trim().parse::<u32>()
            && !uids.contains(&uid)
        {
            uids.push(uid);
        }
    }
    Ok(uids)
}

fn write_exclusions(uids: &[u32]) -> Result<()> {
    ensure_data_dir()?;
    let content = uids
        .iter()
        .map(|u| u.to_string())
        .collect::<Vec<_>>()
        .join("\n");
    std::fs::write(EXCLUSION_FILE, content)?;
    Ok(())
}

/// 排除列表 (持久化文件为准, 内核列表可能因重启清空)
pub fn exclude_list() -> Result<Vec<u32>> {
    let mut uids = read_exclusions()?;
    if let Ok(kernel_uids) = list_uids() {
        for uid in kernel_uids {
            if !uids.contains(&uid) {
                uids.push(uid);
            }
        }
    }
    uids.sort_unstable();
    Ok(uids)
}

pub fn exclude_add(uid: u32) -> Result<()> {
    let mut uids = read_exclusions()?;
    if !uids.contains(&uid) {
        uids.push(uid);
        uids.sort_unstable();
    }
    // 内核列表可能已经有这个 uid (重启后重新下发), 重复添加不算失败
    block_uid(uid).or_else(ignore_uid_exists)?;
    write_exclusions(&uids)
}

pub fn exclude_remove(uid: u32) -> Result<()> {
    let mut uids = read_exclusions()?;
    uids.retain(|&u| u != uid);
    // 内核里本来就没有这个 uid 时同样视为成功
    unblock_uid(uid).or_else(ignore_uid_missing)?;
    write_exclusions(&uids)
}

/// 开机把持久化的排除列表重新写进内核 (内核 UID 列表同样是内存态)
///
/// 单个 uid 失败不能让后面的 uid 全部丢掉, 因此只在一条都没成功时上报首个错误。
pub fn replay_exclusions() -> Result<()> {
    let uids = read_exclusions()?;
    let mut first_error = None;
    let mut applied = 0usize;
    for uid in uids {
        match block_uid(uid).or_else(ignore_uid_exists) {
            Ok(()) => applied += 1,
            Err(e) => {
                first_error.get_or_insert(e);
            }
        };
    }
    match first_error {
        Some(e) if applied == 0 => Err(e),
        Some(e) => {
            log::warn!("nomount: {applied} 个排除 uid 已恢复, 其余失败: {e}");
            Ok(())
        }
        None => Ok(()),
    }
}

// ---------------------------------------------------------------------------
// 聚合快照 (管理器一次调用拿全部数据)
// ---------------------------------------------------------------------------

/// 聚合快照 (一次调用把状态/模块/规则/排除列表全部交给管理器)
///
/// 字段名就是管理器的 JSON 契约 (`nomount/snapshot`), 改名会让管理器读到空值。
#[derive(serde::Serialize)]
struct Snapshot<'a> {
    supported: bool,
    version: u32,
    enabled: bool,
    safe_mode: bool,
    isolated_blocked: bool,
    rule_count: usize,
    active_module_count: usize,
    /// 仅在内核不支持时出现, 说明原因
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
    modules: Vec<mount::ModuleInfo>,
    rules: Vec<RuleView<'a>>,
    exclusions: Vec<u32>,
}

/// 聚合快照 JSON: 状态 + 模块 + 规则 + 排除列表
pub fn snapshot() -> Result<String> {
    let version_text = read_version();
    let supported = version_text.is_ok();
    let version = version_text
        .as_deref()
        .map(parse_version)
        .unwrap_or_default();

    let rules = if supported {
        list_rules().unwrap_or_default()
    } else {
        Vec::new()
    };
    let modules = if supported {
        mount::modules().unwrap_or_default()
    } else {
        Vec::new()
    };
    let exclusions = read_exclusions().unwrap_or_default();
    let isolated = if supported {
        isolated_blocking().unwrap_or_else(|_| persisted_isolated_flag())
    } else {
        persisted_isolated_flag()
    };

    // 生效中的模块 = 产生了规则的模块, 参考实现自身不算
    let active_modules = rules
        .iter()
        .filter_map(|rule| {
            let rest = rule.real_path.strip_prefix(mount::MODULES_DIR_PREFIX)?;
            let (id, _) = rest.split_once('/')?;
            (id != mount::NOMOUNT_MODULE_ID).then_some(id)
        })
        .collect::<std::collections::HashSet<_>>()
        .len();

    let snapshot = Snapshot {
        supported,
        version,
        enabled: is_enabled(),
        safe_mode: safe_mode(),
        isolated_blocked: isolated,
        rule_count: rules.len(),
        active_module_count: active_modules,
        error: version_text
            .err()
            .map(|e| e.to_string())
            .filter(|e| !e.is_empty()),
        modules,
        rules: rules.iter().map(RuleView::from).collect(),
        exclusions,
    };
    Ok(serde_json::to_string(&snapshot)?)
}
