# NoMount upstream provenance

NoMount is vendored so the kernel module is built by this repository's pipeline
instead of being downloaded from upstream releases. The layout, build script and
Rust control path follow FinaSU (`userspace/nomount`), which vendors the same
upstream revision. Only `kernel/src/` is imported; the userspace control path is
reimplemented in Rust inside `userspace/ksud`, so no upstream shell scripts or
WebUI are imported.

## Pinned sources

- NoMount: https://github.com/maxsteeel/nomount
  commit `dcd77d03e9912f397a586d7da81694d1628f170e` (tag `v2.3.1`).

  | upstream path | vendored path | sha256 |
  | --- | --- | --- |
  | `kernel/src/nomount.c` | `kernel/src/nomount.c` | `041c09ba94ab76ab67c25241bda563dd8cdbb6567e476c557909108088cf2813` |
  | `kernel/src/nomount.h` | `kernel/src/nomount.h` | `a396b6469fc4912e9d93179ace982468dc466f8b2a83ce6e4b606857b015941c` |
  | `kernel/src/Kconfig` | `kernel/src/Kconfig` | `68e1d5a393eed6026b071149d0998d1652a598dfbcd4e4cd98f23e178337dbca` |
  | `kernel/src/Makefile` | `kernel/src/Makefile` | `1191fe9ef8e2f3549419fd90a118fbe3d11fc02a04b6353bae0e81be27e03e62` |
  | `kernel/LICENSE` | `kernel/LICENSE` | `8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643` |
  | `LICENSE` | `LICENSE` | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |

Every imported file is byte-identical to upstream and carries no BakaSU
attribution.

## Integration boundary

The imported driver speaks protocol v23: rule-add and list headers are 24 bytes
(`flags`, `uid`, `uid_end`, `deny_start`, `deny_end`, `v_len`, `r_len`), the
delete header is 18 bytes, and list pagination uses an opaque cursor with an
all-ones completion marker. The Rust control path accepts v23 and rejects older
drivers. The built-in planner does not generate UID ranges or opaque rules, so
opaque and `.replace` directories keep file-merge and directory-allowlist
handling; `.remove` is a whiteout only for unprotected directories. Upstream's
bootloop detection and KernelSU/APatch injection ordering are not imported:
ksud owns driver loading, rule injection, UI readiness and boot guards.

Not imported: `module/*.sh`, `module/webroot/**`, `userspace/src/nm.c(.h)` and
`kernel/setup.sh` (it patches an in-tree GKI source; this module builds
out-of-tree).

## License mapping

`kernel/**` is GPL-2.0 (full text in `kernel/LICENSE`; the module declares
`MODULE_LICENSE("GPL")`). Upstream's non-kernel files are GPL-3.0, kept verbatim
as `LICENSE` for provenance even though none of them are vendored. Files authored
in this directory (`UPSTREAM.md`, `NOTICE`, `.gitignore`, `build-lkm.sh`) are not
upstream files; `build-lkm.sh` carries `SPDX-License-Identifier: GPL-2.0-only`.
BakaSU releases must keep both license texts.

## Build contract

`kernel/src/` builds out-of-tree with the same DDK image matrix as
`kernelsu.ko`, and the output is staged as
`userspace/ksud/bin/aarch64/<kmi>_nomount.ko`:

```
CONFIG_NOMOUNT=m make -C "$KDIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
    M=<repo>/userspace/nomount/kernel/src modules
```

Upstream's `Makefile` only appends a commit count when the enclosing repository
has a `nomount` origin, which is false here, so the module reports base version
`23` and the Rust driver check compares against that.
