# C and C++ API

`<yukilinker.h>` provides the complete C interface. `<yukilinker.hpp>` adds
header-only C++17 ownership objects and typed results. Both headers support
independent, repeated and mixed inclusion.

Functions and C types use the `yukilinker_c_` prefix. Constants use
`YUKILINKER_C_`. `yukilinker_c_abi_version()` returns 1. The side-effect-free
`yukilinker_c_has_feature()` query reports supported feature identifiers.
Capabilities are subject to their backend and platform restrictions.

## Runtime and HostServices

Create one runtime owner per linked copy with `runtime_create`. Supply a complete
HostServices table with its `size`, version 1 and required system linker callbacks.
The runtime copies the table. Callback code and `user` storage must stay alive
until owner retirement. `host_services_bind` changes services only when active
operations and modules permit it.

System linker callbacks need the containing Core's return address for Bionic
namespace selection. Mark wrappers `noinline` and `disable_tail_calls`. For symbol
lookup, use `dlerror` to distinguish a valid null address and save consumed error
text for the error callback. The C host demonstrates these wrappers.

Initialize input record sizes and required fields. `runtime_destroy`,
`context_destroy` and `try_shutdown` report success as a boolean. Most other
status-returning operations use `YUKILINKER_C_OK`, whose value is zero. Consult
the declarations for each operation's result and buffer-sizing contract.

## Modules and scopes

Select PRIVATE or SYSTEM in open options. A handle contains opaque `slot` and
`generation` words bound to its issuing runtime. Pass both words back intact.
`open_fd` borrows the caller's descriptor at offset zero. The caller closes its
descriptor after the operation.

PRIVATE can use a context or namespace. Contexts copy search paths and retain
dependency-provider callbacks and user storage. A provider transfers a successful
returned fd to the loader. Namespaces copy resolution settings and link declared
namespace or SYSTEM scopes. Host imports bind explicitly named system symbols.
Images, links and observations can prevent scope destruction.

Select at most one context or namespace in open options. A null scope selects
the default context. SYSTEM rejects private scope and mapping options. Backend
selection determines which loader owns the module.

`lookup` accepts a symbol name and optional ELF symbol version. Inspect `found`
separately from the address. `query` and `handles` report execution state,
reference ownership and retained resources. `close` returns the complete outcome
and handle validity. A retained handle remains usable according to that outcome.

## Observations and diagnostics

| Capability | C operations after the common prefix |
| --- | --- |
| Owner and limits | `runtime_create`, `host_services_bind`, `try_shutdown`, `runtime_destroy`, `set_limits`, `query_stats` |
| Modules | `open_path`, `open_fd`, `lookup`, `query`, `handles`, `close`, `forget` |
| Trusted mapping transfer | `release_host`, `handle_image` |
| Contexts and namespaces | `context_create`, `context_destroy`, `namespace_create`, `namespace_destroy`, `namespace_id` |
| Links and imports | `namespace_link`, `namespace_links`, `namespace_host_import`, `namespace_host_imports`, `query_host_import_refusal` |
| Copied views | `image_query`, `query_image_detail`, `image_dependencies`, `image_program_headers`, `query_address`, `iterate_images` |
| Snapshots | `snapshot_acquire`, `snapshot_image`, `snapshot_mapping`, `snapshot_program_headers`, `snapshot_release` |
| Leases | `image_acquire`, `image_release` |
| Events and waits | `read_events`, `wait_timeout`, `load_waits` |
| Controlled fork | `prepare_fork`, `after_fork` |
| Omitted-release diagnostic | `report_unreleased` |

Runtime, context and namespace are separate opaque types. Image identities,
snapshots and leases each have their own representation. Keep the issuing owner
alongside every token. Unscoped snapshots and iteration observe all visible
images. Program-header capacities and required sizes are expressed in bytes.

Snapshot and lease release take in/out tokens. Consumed tokens become empty,
including when later unmapping reports `IO_ERROR`. Retry retained cleanup through
the module or owner. A refused or stale release preserves its token. Errors retain
the failure stage, errno, execution status and incomplete-cleanup details.

## C++ ownership

`Runtime`, `Module`, `Context`, `Namespace`, `Snapshot` and `Lease` support move
construction and explicit `swap`. Call `close`, `release` or `destroy` explicitly.
Destructors attempt a diagnostic for omitted release and leave the underlying
resource available for recovery.

`Result<T>` carries a value and error. `Module::symbol<FunctionPointer>` provides
typed lookup. `CloseResult` exposes reference state, handle validity, retention
reasons and private unmap status. `Module::last_close()` preserves its last result.

`adopt` transfers an existing owned C token into a wrapper. Core and the runtime
owner must outlive every child, callback and borrowed code pointer. All wrappers
use the same C interface and ownership state. They support callers compiled with
exceptions and RTTI disabled.

The C and C++ hosts provide executable examples. See the
[lifecycle guide](lifecycle.md) for release and Core handoff requirements.
