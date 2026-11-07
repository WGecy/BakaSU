# Supported configurations

The SDK targets ARM64 and ARM32 Android processes using NDK `30.0.16248370` and
API31. Each build handles its native ELF class and machine. Runtime page size
comes from the host. Libraries use 16 KiB maximum LOAD alignment.

## Backend selection

PRIVATE manages ELF mappings, private dependency scopes, constructors and
supported dynamic TLS. SYSTEM delegates loading and symbol resolution to
HostServices and Bionic. Use SYSTEM for a component closure sharing Bionic TLS,
libc++, cross-DSO exceptions or CFI.

The host coordinates code-pointer lifetime, thread exit, callbacks and Core
retirement. PRIVATE call barriers contain escaping exceptions where required by
the ABI. C API callbacks must obey their declared signatures.

## TLS and executable mappings

Traditional AArch64 dynamic TLS relocation numbers differ between the pinned NDK
and some Bionic releases. PRIVATE rejects the ambiguous pair before resolvers or
constructors run. AArch64 TLSDESC and supported ARM32 dynamic TLS are available.
SYSTEM follows its actual host's relocation conventions.

PRIVATE validates load-segment permissions and rejects writable executable
mappings. Writable file-backed segments can clear their last file page through
the writable mapping. Executable segments needing a writable zero-tail view are
subject to host mapping permissions and policy. SELinux and executable-memory
policy can cause refusal with the corresponding error detail.

## Instrumentation

CFI runtime belongs to its containing system-loaded Core. Compile Core and system
modules with compatible cross-DSO CFI settings. A CFI runtime refuses PRIVATE
loading before execution. Use the ordinary private bootstrap route with its
matching Core configuration.

The sanitizer option selects NDK instrumentation. The containing host and module
closure must supply compatible runtime services. Instrumented execution depends
on the target Android environment.

## Validation boundaries

Native hosts cover C/C++ consumption, retained cleanup, callback and fork lifetime,
and actual two-stage unloading. Local QEMU uses separately provisioned Bionic
runtime bytes. Record their identity when reproducing failures. Device policy,
Java VM lifecycle and application integration require verification in the
consuming application's environment.
