# YukiLinker

An Android ELF loader SDK with a standalone bootstrap and a runtime embedded in
the consuming Core. It supports ARM64 and ARM32, C callers and C++17 callers.
SDK version is 1.0.0. The C ABI and HostServices version are 1.

| Target | ARM64 artifact | ARM32 artifact | Purpose |
| --- | --- | --- | --- |
| `YukiLinker::bootstrap` | `libyloader64.so` | `libyloader32.so` | Load Core, transfer ownership and unload bootstrap |
| `YukiLinker::runtime` | `libylrt64.a` | `libylrt32.a` | Complete PIC runtime linked into Core |

Include `<yukilinker.h>` for C or `<yukilinker.hpp>` for header-only C++ ownership
wrappers. The runtime also supports direct source compilation into Core.

```cmake
find_package(YukiLinker 1 CONFIG REQUIRED)
add_library(core SHARED core.cpp)
target_link_libraries(core PRIVATE YukiLinker::runtime)
```

Use NDK `30.0.16248370` and API31. PRIVATE loads independently managed ELF images.
SYSTEM uses Bionic's component and runtime integration. Close outcomes describe
reference ownership, mapping retention and cleanup failures. C++ owners require
explicit release.

- [Build and integrate](docs/building.md)
- [C and C++ API](docs/api.md)
- [Ownership and bootstrap handoff](docs/lifecycle.md)
- [Supported configurations](docs/compatibility.md)

The three Native hosts demonstrate C consumption, C++ ownership and actual
bootstrap handoff. Their independent CMake project and small fixtures are under
`tests/`.

YukiLinker uses Apache-2.0. The AOSP-derived ARM64 TLS descriptor assembly carries
its own copyright and BSD notice in the source file.
