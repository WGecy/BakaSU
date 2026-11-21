//! Pure, host-testable planning for Folk Mount.
//!
//! Only filesystem-agnostic tree logic lives here: module filtering, first-wins
//! merging, partition remapping, whiteout/opaque detection and the tmpfs-vs-bind
//! decision. All I/O goes through [`PlanFs`], so host tests can drive it.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use log::{error, warn};

pub const DISABLE_FILE_NAME: &str = "disable";
pub const REMOVE_FILE_NAME: &str = "remove";
pub const SKIP_MOUNT_FILE_NAME: &str = "skip_mount";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum EntryKind {
    File,
    Directory,
    Symlink,
    CharDevice,
    Other,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EntryMeta {
    pub kind: EntryKind,
    pub rdev: u64,
}

impl EntryMeta {
    #[must_use]
    pub const fn new(kind: EntryKind, rdev: u64) -> Self {
        Self { kind, rdev }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NodeFileType {
    RegularFile,
    Directory,
    Symlink,
    Whiteout,
}

impl NodeFileType {
    #[must_use]
    pub const fn from_entry_meta(meta: EntryMeta) -> Option<Self> {
        match meta.kind {
            EntryKind::File => Some(Self::RegularFile),
            EntryKind::Directory => Some(Self::Directory),
            EntryKind::Symlink => Some(Self::Symlink),
            EntryKind::CharDevice if meta.rdev == 0 => Some(Self::Whiteout),
            EntryKind::CharDevice | EntryKind::Other => None,
        }
    }

    #[must_use]
    pub fn needs_tmpfs_vs_real(self, real: RealEntry) -> bool {
        match self {
            Self::Symlink => true,
            Self::Whiteout => real.exists,
            Self::RegularFile | Self::Directory => real
                .kind
                .is_none_or(|real_type| real_type != self || real_type == Self::Symlink),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RealEntry {
    pub kind: Option<NodeFileType>,
    pub exists: bool,
}

impl RealEntry {
    #[cfg(test)]
    #[must_use]
    pub const fn missing() -> Self {
        Self {
            kind: None,
            exists: false,
        }
    }

    #[cfg(test)]
    #[must_use]
    pub const fn file() -> Self {
        Self {
            kind: Some(NodeFileType::RegularFile),
            exists: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Node {
    pub name: String,
    pub file_type: NodeFileType,
    pub children: BTreeMap<String, Self>,
    pub module_path: Option<PathBuf>,
    pub replace: bool,
    pub skip: bool,
}

impl Node {
    #[must_use]
    pub fn new_root(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            file_type: NodeFileType::Directory,
            children: BTreeMap::new(),
            module_path: None,
            replace: false,
            skip: false,
        }
    }

    fn from_entry(fs: &impl PlanFs, name: &str, path: &Path) -> Result<Option<Self>> {
        let Ok(meta) = fs.symlink_metadata(path) else {
            return Ok(None);
        };
        let Some(file_type) = NodeFileType::from_entry_meta(meta) else {
            return Ok(None);
        };
        let replace = file_type == NodeFileType::Directory && fs.read_opaque(path)?;
        Ok(Some(Self {
            name: name.to_string(),
            file_type,
            children: BTreeMap::new(),
            module_path: Some(path.to_path_buf()),
            replace,
            skip: false,
        }))
    }

    pub fn collect_module_files(&mut self, fs: &impl PlanFs, dir: &Path) -> Result<bool> {
        let mut names = fs
            .read_dir(dir)
            .with_context(|| format!("read module dir {}", dir.display()))?;
        names.sort_unstable();

        let mut has_file = false;
        for name in names {
            let path = dir.join(&name);
            if let Some(mut node) = Self::from_entry(fs, &name, &path)? {
                if node.file_type == NodeFileType::Directory {
                    has_file |= node.collect_module_files(fs, &path)? || node.replace;
                } else {
                    has_file = true;
                }
                self.children.entry(name).or_insert(node);
            }
        }
        Ok(has_file)
    }

    pub fn merge_from(&mut self, other: Self) {
        for (name, node) in other.children {
            match self.children.entry(name) {
                std::collections::btree_map::Entry::Vacant(slot) => {
                    slot.insert(node);
                }
                std::collections::btree_map::Entry::Occupied(mut slot) => {
                    let existing = slot.get_mut();
                    if existing.file_type == NodeFileType::Directory
                        && node.file_type == NodeFileType::Directory
                        && !existing.replace
                    {
                        existing.merge_from(node);
                    }
                }
            }
        }
    }

    #[allow(dead_code)]
    pub fn visit_targets(&self, base: &Path, f: &mut impl FnMut(&Self, &Path)) {
        let target = base.join(&self.name);
        f(self, &target);
        for child in self.children.values() {
            child.visit_targets(&target, f);
        }
    }
}

pub trait PlanFs {
    fn read_dir(&self, dir: &Path) -> Result<Vec<String>>;

    fn symlink_metadata(&self, path: &Path) -> Result<EntryMeta>;

    fn is_dir(&self, path: &Path) -> bool;

    fn is_symlink(&self, path: &Path) -> bool;

    fn exists(&self, path: &Path) -> bool;

    fn read_opaque(&self, path: &Path) -> Result<bool>;
}

pub struct StdFs<F> {
    opaque: F,
}

impl<F> StdFs<F> {
    #[must_use]
    pub const fn new(opaque: F) -> Self {
        Self { opaque }
    }
}

impl<F> PlanFs for StdFs<F>
where
    F: Fn(&Path) -> Result<bool>,
{
    fn read_dir(&self, dir: &Path) -> Result<Vec<String>> {
        let mut names = Vec::new();
        for entry in
            std::fs::read_dir(dir).with_context(|| format!("read dir {}", dir.display()))?
        {
            let entry = entry.with_context(|| format!("read entry in {}", dir.display()))?;
            names.push(entry.file_name().to_string_lossy().into_owned());
        }
        Ok(names)
    }

    fn symlink_metadata(&self, path: &Path) -> Result<EntryMeta> {
        let meta =
            std::fs::symlink_metadata(path).with_context(|| format!("stat {}", path.display()))?;
        Ok(EntryMeta::new(entry_kind(&meta), device_number(&meta)))
    }

    fn is_dir(&self, path: &Path) -> bool {
        path.is_dir()
    }

    fn is_symlink(&self, path: &Path) -> bool {
        path.is_symlink()
    }

    fn exists(&self, path: &Path) -> bool {
        path.exists()
    }

    fn read_opaque(&self, path: &Path) -> Result<bool> {
        (self.opaque)(path)
    }
}

#[must_use]
pub fn entry_meta(meta: &std::fs::Metadata) -> EntryMeta {
    EntryMeta::new(entry_kind(meta), device_number(meta))
}

fn entry_kind(meta: &std::fs::Metadata) -> EntryKind {
    let file_type = meta.file_type();
    if file_type.is_file() {
        EntryKind::File
    } else if file_type.is_dir() {
        EntryKind::Directory
    } else if file_type.is_symlink() {
        EntryKind::Symlink
    } else if is_char_device(meta) {
        EntryKind::CharDevice
    } else {
        EntryKind::Other
    }
}

#[cfg(unix)]
fn is_char_device(meta: &std::fs::Metadata) -> bool {
    use std::os::unix::fs::FileTypeExt;
    meta.file_type().is_char_device()
}

#[cfg(not(unix))]
fn is_char_device(_meta: &std::fs::Metadata) -> bool {
    false
}

#[cfg(unix)]
fn device_number(meta: &std::fs::Metadata) -> u64 {
    use std::os::unix::fs::MetadataExt;
    meta.rdev()
}

#[cfg(not(unix))]
fn device_number(_meta: &std::fs::Metadata) -> u64 {
    0
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PartitionSpec {
    pub name: &'static str,
    pub require_system_symlink: bool,
}

pub const BUILTIN_PARTITIONS: [PartitionSpec; 5] = [
    PartitionSpec {
        name: "vendor",
        require_system_symlink: true,
    },
    PartitionSpec {
        name: "system_ext",
        require_system_symlink: true,
    },
    PartitionSpec {
        name: "product",
        require_system_symlink: true,
    },
    PartitionSpec {
        name: "odm",
        require_system_symlink: false,
    },
    PartitionSpec {
        name: "oem",
        require_system_symlink: false,
    },
];

#[derive(Debug, Clone, Copy)]
pub struct PlanConfig<'a> {
    pub modules_dir: &'a Path,
    pub root_dir: &'a Path,
    pub system_dir: &'a Path,
    pub partitions: &'a [PartitionSpec],
}

impl PlanConfig<'_> {
    #[must_use]
    pub const fn new<'a>(
        modules_dir: &'a Path,
        root_dir: &'a Path,
        system_dir: &'a Path,
    ) -> PlanConfig<'a> {
        PlanConfig {
            modules_dir,
            root_dir,
            system_dir,
            partitions: &BUILTIN_PARTITIONS,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(clippy::struct_excessive_bools)]
pub struct ModuleCandidate {
    pub id: String,
    pub path: PathBuf,
    pub is_metamodule: bool,
    pub disabled: bool,
    pub removed: bool,
    pub skip_mount: bool,
    pub has_system: bool,
    pub system_is_symlink: bool,
}

impl ModuleCandidate {
    #[must_use]
    pub const fn is_mountable(&self) -> bool {
        !self.disabled
            && !self.removed
            && !self.skip_mount
            && !self.is_metamodule
            && self.has_system
            && !self.system_is_symlink
    }
}

pub fn enumerate_modules<F>(
    fs: &impl PlanFs,
    modules_dir: &Path,
    is_metamodule: F,
) -> Result<Vec<ModuleCandidate>>
where
    F: Fn(&Path) -> bool,
{
    let mut names = fs
        .read_dir(modules_dir)
        .with_context(|| format!("read modules dir {}", modules_dir.display()))?;
    names.sort_unstable();

    let mut modules = Vec::new();
    for id in names {
        let path = modules_dir.join(&id);
        if fs.is_symlink(&path) || !fs.is_dir(&path) {
            continue;
        }
        let system = path.join("system");
        modules.push(ModuleCandidate {
            id,
            is_metamodule: is_metamodule(&path),
            disabled: fs.exists(&path.join(DISABLE_FILE_NAME)),
            removed: fs.exists(&path.join(REMOVE_FILE_NAME)),
            skip_mount: fs.exists(&path.join(SKIP_MOUNT_FILE_NAME)),
            has_system: fs.is_dir(&system),
            system_is_symlink: fs.is_symlink(&system),
            path,
        });
    }
    Ok(modules)
}

pub fn collect_module_files<F>(
    fs: &impl PlanFs,
    cfg: &PlanConfig<'_>,
    is_metamodule: F,
) -> Result<Option<Node>>
where
    F: Fn(&Path) -> bool,
{
    let modules = enumerate_modules(fs, cfg.modules_dir, is_metamodule)?;
    let mut system = Node::new_root("system");
    let mut has_file = false;

    for module in modules {
        if !module.is_mountable() {
            continue;
        }
        let mod_system = module.path.join("system");
        let mut tree = Node::new_root("system");
        match tree.collect_module_files(fs, &mod_system) {
            Ok(true) => {
                has_file = true;
                system.merge_from(tree);
            }
            Ok(false) => {}
            Err(e) => warn!("folk mount: skipping module {}: {e:#}", module.id),
        }
    }

    if !has_file {
        return Ok(None);
    }

    let mut root = Node::new_root("");
    for spec in cfg.partitions {
        let root_partition = cfg.root_dir.join(spec.name);
        let system_partition = cfg.system_dir.join(spec.name);
        if fs.is_dir(&root_partition)
            && (!spec.require_system_symlink || fs.is_symlink(&system_partition))
            && let Some(node) = system.children.remove(spec.name)
        {
            root.children.insert(spec.name.to_string(), node);
        }
    }
    root.children.insert("system".to_string(), system);
    Ok(Some(root))
}

#[must_use]
pub fn should_create_tmpfs(
    path: &Path,
    current: &mut Node,
    has_tmpfs: bool,
    mut real_entry: impl FnMut(&Path) -> RealEntry,
) -> bool {
    if has_tmpfs {
        return false;
    }
    if current.replace && current.module_path.is_some() {
        return true;
    }
    for (name, node) in &mut current.children {
        if node
            .file_type
            .needs_tmpfs_vs_real(real_entry(&path.join(name)))
        {
            if current.module_path.is_none() {
                error!("cannot create tmpfs on {}, ignore: {name}", path.display());
                node.skip = true;
                continue;
            }
            return true;
        }
    }
    false
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FileMeta {
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RealDirEntry {
    pub name: String,
    pub file_type: NodeFileType,
}

pub trait MountOps {
    fn read_dir(&self, path: &Path) -> Result<Vec<RealDirEntry>>;

    fn symlink_metadata(&self, path: &Path) -> Result<EntryMeta>;

    fn metadata(&self, path: &Path) -> Result<FileMeta>;

    fn read_link(&self, path: &Path) -> Result<PathBuf>;

    fn exists(&self, path: &Path) -> bool;

    fn is_dir(&self, path: &Path) -> bool;

    fn create_dir_all(&self, path: &Path) -> Result<()>;

    fn create_file(&self, path: &Path) -> Result<()>;

    fn symlink(&self, target: &Path, link: &Path) -> Result<()>;

    fn chmod(&self, path: &Path, mode: u32) -> Result<()>;

    fn chown(&self, path: &Path, uid: u32, gid: u32) -> Result<()>;

    fn lgetfilecon(&self, path: &Path) -> Result<String>;

    fn lsetfilecon(&self, path: &Path, con: &str) -> Result<()>;

    fn mount_tmpfs(&self, name: &str, target: &Path) -> Result<()>;

    fn mount_bind(&self, source: &Path, target: &Path) -> Result<()>;

    fn mount_move(&self, source: &Path, target: &Path) -> Result<()>;

    fn make_private(&self, path: &Path) -> Result<()>;

    fn unmount_detach(&self, path: &Path) -> Result<()>;

    fn register_umount(&self, path: &Path) -> Result<()>;

    fn unregister_umount(&self, path: &Path) -> Result<()>;

    fn report_mounted(&self) -> Result<()>;
}

#[derive(Debug, Default, Clone)]
pub struct ExecutionOutcome {
    pub target_count: usize,
    pub skipped_count: usize,
    pub partial: bool,
    pub residue: Vec<PathBuf>,
}

#[derive(Debug)]
pub struct ExecutionReport {
    pub outcome: ExecutionOutcome,
    pub error: Option<anyhow::Error>,
}

struct MountTransaction<'a, O: MountOps> {
    ops: &'a O,
    mounts: Vec<PathBuf>,
    registrations: Vec<PathBuf>,
    skipped: usize,
}

struct RollbackOutcome {
    remaining_mounts: usize,
    residue: Vec<PathBuf>,
}

impl<'a, O: MountOps> MountTransaction<'a, O> {
    const fn new(ops: &'a O) -> Self {
        Self {
            ops,
            mounts: Vec::new(),
            registrations: Vec::new(),
            skipped: 0,
        }
    }

    fn publish(&mut self, target: &Path) {
        if !self.mounts.iter().any(|path| path == target) {
            self.mounts.push(target.to_path_buf());
        }
    }

    fn mount_node(
        &mut self,
        parent: &Path,
        work_parent: &Path,
        mut current: Node,
        has_tmpfs: bool,
    ) -> Result<()> {
        let ops = self.ops;
        let path = parent.join(&current.name);
        let work = work_parent.join(&current.name);
        if current.skip {
            self.skipped += 1;
            return Ok(());
        }
        match current.file_type {
            NodeFileType::RegularFile => {
                let target = if has_tmpfs {
                    ops.create_file(&work)?;
                    work.clone()
                } else {
                    path.clone()
                };
                let Some(module_path) = current.module_path.clone() else {
                    bail!("cannot mount root file {}", path.display());
                };
                ops.mount_bind(&module_path, &target)?;
                if !has_tmpfs {
                    self.publish(&target);
                }
                ops.make_private(&target)?;
            }
            NodeFileType::Symlink => {
                let Some(module_path) = current.module_path.clone() else {
                    bail!("cannot mount root symlink {}", path.display());
                };
                self.clone_symlink(&module_path, &work)?;
            }
            NodeFileType::Directory => {
                let create = should_create_tmpfs(&path, &mut current, has_tmpfs, |probe| {
                    real_entry(ops, probe)
                });
                let has_tmpfs = has_tmpfs || create;
                if has_tmpfs {
                    self.prepare_tmpfs_skeleton(&path, &work, current.module_path.as_deref())?;
                }
                if create {
                    ops.mount_bind(&work, &work)?;
                }
                if current.replace && current.module_path.is_none() {
                    bail!(
                        "dir {} is marked opaque but has no module source",
                        path.display()
                    );
                }
                if ops.is_dir(&path) && !current.replace {
                    self.process_existing_entries(&path, &work, &mut current.children, has_tmpfs)?;
                }
                self.process_remaining_children(&path, &work, current.children, has_tmpfs)?;
                if create {
                    self.move_tmpfs_to_target(&work, &path)?;
                }
            }
            NodeFileType::Whiteout => {
                log::debug!("folk mount: {} is whited out", path.display());
            }
        }
        Ok(())
    }

    fn mount_mirror(&self, parent: &Path, work_parent: &Path, entry: &RealDirEntry) -> Result<()> {
        let path = parent.join(&entry.name);
        let work = work_parent.join(&entry.name);
        match entry.file_type {
            NodeFileType::RegularFile => {
                self.ops.create_file(&work)?;
                self.ops.mount_bind(&path, &work)?;
            }
            NodeFileType::Directory => {
                self.ops.create_dir_all(&work)?;
                let meta = self.ops.metadata(&path)?;
                self.ops.chown(&work, meta.uid, meta.gid)?;
                self.ops.chmod(&work, meta.mode)?;
                self.ops.lsetfilecon(&work, &self.ops.lgetfilecon(&path)?)?;
                let mut entries = self.ops.read_dir(&path)?;
                entries.sort_by(|a, b| a.name.cmp(&b.name));
                for child in entries {
                    self.mount_mirror(&path, &work, &child)?;
                }
            }
            NodeFileType::Symlink => self.clone_symlink(&path, &work)?,
            NodeFileType::Whiteout => {}
        }
        Ok(())
    }

    fn clone_symlink(&self, source: &Path, link: &Path) -> Result<()> {
        let target = self.ops.read_link(source)?;
        self.ops.symlink(&target, link)?;
        self.ops.lsetfilecon(link, &self.ops.lgetfilecon(source)?)?;
        Ok(())
    }

    fn prepare_tmpfs_skeleton(
        &self,
        path: &Path,
        work: &Path,
        module_path: Option<&Path>,
    ) -> Result<()> {
        self.ops.create_dir_all(work)?;
        let source = if self.ops.is_dir(path) {
            path
        } else if let Some(module_path) = module_path {
            module_path
        } else {
            bail!(
                "cannot prepare mount skeleton for {} without a module source",
                path.display()
            );
        };
        let meta = self.ops.metadata(source)?;
        self.ops.chown(work, meta.uid, meta.gid)?;
        self.ops.chmod(work, meta.mode)?;
        self.ops.lsetfilecon(work, &self.ops.lgetfilecon(source)?)?;
        Ok(())
    }

    fn process_existing_entries(
        &mut self,
        path: &Path,
        work: &Path,
        children: &mut BTreeMap<String, Node>,
        has_tmpfs: bool,
    ) -> Result<()> {
        let mut entries = self.ops.read_dir(path)?;
        entries.sort_by(|a, b| a.name.cmp(&b.name));
        for entry in entries {
            if let Some(node) = children.remove(&entry.name) {
                if node.skip {
                    self.skipped += 1;
                    continue;
                }
                self.mount_node(path, work, node, has_tmpfs)
                    .with_context(|| format!("folk mount {}/{}", path.display(), entry.name))?;
            } else if has_tmpfs {
                self.mount_mirror(path, work, &entry)
                    .with_context(|| format!("folk mirror {}/{}", path.display(), entry.name))?;
            }
        }
        Ok(())
    }

    fn process_remaining_children(
        &mut self,
        path: &Path,
        work: &Path,
        children: BTreeMap<String, Node>,
        has_tmpfs: bool,
    ) -> Result<()> {
        for (name, node) in children {
            if node.skip {
                self.skipped += 1;
                continue;
            }
            self.mount_node(path, work, node, has_tmpfs)
                .with_context(|| format!("folk mount {}/{}", path.display(), name))?;
        }
        Ok(())
    }

    fn move_tmpfs_to_target(&mut self, work: &Path, target: &Path) -> Result<()> {
        self.ops.mount_move(work, target)?;
        self.publish(target);
        self.ops.make_private(target)?;
        Ok(())
    }

    fn register_targets(&mut self) -> Result<()> {
        let mut targets = self.mounts.clone();
        targets.sort_by(|a, b| {
            component_count(a)
                .cmp(&component_count(b))
                .then_with(|| a.cmp(b))
        });
        targets.dedup();
        for target in targets {
            self.ops
                .register_umount(&target)
                .with_context(|| format!("register umount target {}", target.display()))?;
            self.registrations.push(target);
        }
        Ok(())
    }

    fn rollback(&mut self, work_dir: &Path) -> RollbackOutcome {
        let mut residue = Vec::new();
        let mut remaining_mounts = 0;
        for path in std::mem::take(&mut self.registrations).into_iter().rev() {
            if self.ops.unregister_umount(&path).is_err() {
                residue.push(path);
            }
        }
        for path in std::mem::take(&mut self.mounts).into_iter().rev() {
            if self.ops.unmount_detach(&path).is_err() {
                residue.push(path);
                remaining_mounts += 1;
            }
        }
        if self.ops.unmount_detach(work_dir).is_err() {
            residue.push(work_dir.to_path_buf());
        }
        RollbackOutcome {
            remaining_mounts,
            residue,
        }
    }

    fn cleanup_staging(&self, work_dir: &Path) -> Vec<PathBuf> {
        if self.ops.unmount_detach(work_dir).is_err() {
            vec![work_dir.to_path_buf()]
        } else {
            Vec::new()
        }
    }
}

fn real_entry<O: MountOps>(ops: &O, path: &Path) -> RealEntry {
    let kind = ops
        .symlink_metadata(path)
        .ok()
        .and_then(NodeFileType::from_entry_meta);
    RealEntry {
        kind,
        exists: ops.exists(path),
    }
}

fn component_count(path: &Path) -> usize {
    path.components().count()
}

pub fn run_mount<O: MountOps>(
    plan: Option<Node>,
    target_root: &Path,
    work_dir: &Path,
    fs_name: &str,
    ops: &O,
) -> ExecutionReport {
    let Some(root) = plan else {
        return ExecutionReport {
            outcome: ExecutionOutcome::default(),
            error: None,
        };
    };

    if let Err(e) = ops
        .create_dir_all(work_dir)
        .with_context(|| format!("create mount work dir {}", work_dir.display()))
    {
        return ExecutionReport {
            outcome: ExecutionOutcome::default(),
            error: Some(e),
        };
    }
    if let Err(e) = ops
        .mount_tmpfs(fs_name, work_dir)
        .with_context(|| format!("mount tmpfs on {}", work_dir.display()))
    {
        return ExecutionReport {
            outcome: ExecutionOutcome::default(),
            error: Some(e),
        };
    }
    if let Err(e) = ops
        .make_private(work_dir)
        .with_context(|| format!("make {} private", work_dir.display()))
    {
        let residue = if ops.unmount_detach(work_dir).is_err() {
            vec![work_dir.to_path_buf()]
        } else {
            Vec::new()
        };
        return ExecutionReport {
            outcome: ExecutionOutcome {
                partial: !residue.is_empty(),
                residue,
                ..ExecutionOutcome::default()
            },
            error: Some(e),
        };
    }

    let mut tx = MountTransaction::new(ops);
    if let Err(e) = tx.mount_node(target_root, work_dir, root, false) {
        let rollback = tx.rollback(work_dir);
        return ExecutionReport {
            outcome: ExecutionOutcome {
                target_count: rollback.remaining_mounts,
                skipped_count: tx.skipped,
                partial: !rollback.residue.is_empty(),
                residue: rollback.residue,
            },
            error: Some(e),
        };
    }

    if let Err(e) = tx.register_targets() {
        let rollback = tx.rollback(work_dir);
        return ExecutionReport {
            outcome: ExecutionOutcome {
                target_count: rollback.remaining_mounts,
                skipped_count: tx.skipped,
                partial: !rollback.residue.is_empty(),
                residue: rollback.residue,
            },
            error: Some(e),
        };
    }

    let target_count = tx.mounts.len();
    let skipped_count = tx.skipped;
    let residue = tx.cleanup_staging(work_dir);
    let partial = !residue.is_empty();
    if target_count > 0
        && let Err(e) = ops.report_mounted()
    {
        warn!("folk mount: failed to notify module mount: {e:#}");
    }
    ExecutionReport {
        outcome: ExecutionOutcome {
            target_count,
            skipped_count,
            partial,
            residue,
        },
        error: None,
    }
}

#[cfg(test)]
#[path = "magic_mount_tests.rs"]
mod tests;
