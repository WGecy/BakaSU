# Ownership and bootstrap handoff

Every Core contains its own runtime, linked from the archive or compiled from
sources. Core owns HostServices callbacks, module handles and runtime state.
An external caller loads `libyloaderXX.so` through Bionic and transfers its one
loader reference through the bootstrap protocol.

## Module retirement

Stop new calls and drain users before releasing module code or callback storage.
A returned function pointer can outlive the lookup that produced it. Execution,
unwind, CFI and bridge leases protect the corresponding image lifetime. TLS
blocks and callbacks can also retain an image.

SYSTEM close releases a Bionic reference. PRIVATE close also reports mapping
removal. Inspect `handle_valid`, reference state, retention reasons and cleanup
diagnostics. A released PRIVATE wrapper can remain valid while the image is
retained, and close can retry incomplete cleanup. `forget` drops an eligible
released wrapper while other owners protect the image.

Shutdown can refuse before it begins or return a resumable state after crossing
its admission boundary. Finish outstanding modules and observations, retry
cleanup, destroy runtime, then retire Core from an external continuation.

## Bootstrap protocol

The normal entry is `yuki_bootstrap(int core_fd, int early_packet_fd_plus1)`.
ARM32 also exposes the five-argument `yuki_bootstrap_tango` declared in the C
header. Tango restores the injected GOT slot and page protection and transfers
the stub through the Core protocol.

1. Bootstrap validates inputs and claims one operation. The reduced loader maps
   Core and validates its handoff exports before initialization.
2. Core adopts its anonymous image and the original loader reference, receives
   `zygisk_core_entry`, and initializes its embedded runtime.
3. `zygisk_bootstrap_handoff_ready` confirms that long-lived callbacks and runtime
   state belong to Core. `zygisk_bootstrap_take_dependencies` accepts the exact
   system dependency references acquired while loading it.
4. Bootstrap releases loading metadata and completes
   `zygisk_bootstrap_finish_handoff`.
5. A forced tail call enters the matching Core finalizer. Core releases the
   original Bionic reference with `dlclose`. Execution returns through the Core
   continuation after loader removal.

The caller transfers its sole application loader reference. Additional references
would keep bootstrap mapped. Core must own all code pointers and storage needed
after its finalizer releases loader.

| Resource | Owner after successful handoff | Final release |
| --- | --- | --- |
| Core mapping | Core | External continuation after Core shutdown |
| Original loader reference | Core | Core finalizer calls Bionic `dlclose` |
| Accepted system dependencies | Core | After runtime and callback teardown |
| Loading metadata | Bootstrap until release | Before handoff completion |
| Runtime and HostServices | Core | While Core code is callable |

Early-packet wire version is 3. Its layout and constants are in `yukilinker.h`.
The transport transfers descriptors and cleans up failed loads. Endpoint
connections and application configuration belong to Core.

## Failure and fork

A failure before Core execution can admit retry after complete rollback.
Execution followed by rejected handoff or incomplete cleanup retains required
mappings and references. Core must accept dependencies before bootstrap clears
its ownership. Reentrant and concurrent calls refuse duplicate transfer.
Inputs rejected at admission stay with their caller.

The loader claim includes process identity. A fork child rejects inherited
incomplete handoff and checks descriptor identity during cleanup so a reused
fd number retains its current resource.

Coordinate host activity, call `prepare_fork`, fork, then call `after_fork` in
both processes with the corresponding child flag. Destroy runtime before
unloading Core so TLS keys and libc callback registrations can retire while
their code is still callable.

## Native same-process extension v1

Native injectors can supply a verified host origin and the exact initial loader
reference through `yuki_bootstrap_bind_native_v1`. The binding carries explicit
size/version, PID and generation. It copies origin storage; successful binding
consumes one loader reference and refusal consumes none. Native entry requires
this binding and refuses the old ordinary entry on the same instance.
Existing C ABI1, HostServices1 and packet-v3 layouts remain unchanged.

Binding and ordinary/Tango entry compete on one atomic PID/phase claim. Binding
reserves the instance before publishing origin or completion storage, then only
native entry can advance the bound claim to execution. A losing binding consumes
no reference or completion storage; a losing entry consumes no input descriptors.

`yuki_bootstrap_native_v1(int core_fd, int packet_fd_plus1)` validates the Core's
`zygisk_bootstrap_bind_native_v1` export before Core execution. Core receives and
copies a synchronous temporary open service before adoption. Its HostServices
may call this service for explicit SYSTEM opens during Core entry. The service
borrows fd/request storage and copies errors into Core-owned buffers. It returns
one Bionic reference directly into the existing SYSTEM slot. The real
`android_dlopen_ext` call must retain its return address in the Bionic-registered
loader, never the anonymous Core.

Core must stop new opens, drain service calls and erase the callback/context
before reporting ready. No callback, service buffer or resolver pointer may
survive into the finalizer. The unchanged adoption/dependencies/finish protocol
then transfers the loader reference and forces a tail call to Core's finalizer.
An incomplete handoff in a fork child cannot reuse the inherited window.

Binding also borrows a `yukilinker_c_native_completion` record until entry
returns or tail-calls. Its storage must live outside both the resolver and
loader. RETAINED preserves unresolved ownership; ADOPTED assigns ownership to
Core; RETURNED gives the exact reference back only after a complete
pre-execution rollback. Only RETURNED authorizes one external `dlclose` after
entry returns. Early reentry/fork admission rejection does not overwrite the
original operation's completion. Only the invocation that acquired the claim in
the current process may consume the record. Native admission remains closed
through rollback publication and until the returned loader reference is released;
retry starts with a fresh loader binding. Clear all loader-owned pointers to the
record before the finalizer can unload the loader.
