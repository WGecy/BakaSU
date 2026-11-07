# Build and integrate

Use CMake 3.22 or newer, Ninja and Android NDK `30.0.16248370`. Select
`arm64-v8a` or `armeabi-v7a` and an Android API31 toolchain.

```sh
cmake -S . -B out/arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31
cmake --build out/arm64
cmake --install out/arm64 --prefix "$PWD/out/install"
```

Each ABI installs two production libraries under `lib/<abi>/`. The public
headers are directly under `include/`. Both ABIs can share an installation
prefix because exported CMake targets use separate ABI directories.

## Archive consumption

Point `YukiLinker_DIR` at `<prefix>/lib/cmake/YukiLinker` and configure with the
matching Android toolchain.

```cmake
cmake_minimum_required(VERSION 3.22)
project(MyCore LANGUAGES C)
find_package(YukiLinker 1 CONFIG REQUIRED)
add_library(core SHARED core.c)
target_link_libraries(core PRIVATE YukiLinker::runtime)
```

C++ consumers use `LANGUAGES CXX` and C++17. Runtime functions and state bind
locally in each Core. Expose application services through explicitly exported
Core functions.

## Source consumption

```cmake
cmake_minimum_required(VERSION 3.22)
project(MyCore LANGUAGES C)
add_subdirectory(path/to/YukiLinker sdk)
add_library(core SHARED core.c)
yukilinker_add_runtime_sources(core)
```

Call the helper at file scope in the Core target's directory. It enables C++
and ASM and adds the runtime sources and their compilation settings. The archive
and source forms share those settings. Core translation units select their own
exception and RTTI flags. Choose one integration form per Core. Duplicate source
attachment or mixed archive/source integration is rejected.

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `YUKILINKER_BUILD_BOOTSTRAP` | `ON` | Build the shared Core loader |
| `YUKILINKER_CFI` | `OFF` | Instrument runtime for a system-loaded CFI Core |
| `YUKILINKER_SANITIZER` | empty | Select NDK address, hwaddress, undefined or thread instrumentation |

Apply compatible instrumentation to the containing Core and its system-loaded
modules. Use fresh output and install directories for different configurations.
Debug paths use `/yukilinker`, `/yukilinker/build` and `/ndk`. Configure GDB
substitutions from the build root before the source root.

## Native hosts

Configure `tests/` as an independent CMake project with the same toolchain.
Run `c_host <fixtures>`, `cpp_host <fixtures>` and `bootstrap_host <fixtures>`
under the matching Android linker. Include the fixture directory and its `sdk/`
subdirectory in `LD_LIBRARY_PATH`.

The `tests/consumer` project builds a shared Core from either language host.
Set `YUKILINKER_RUNTIME_FORM=archive|sources` and
`YUKILINKER_CONSUMER_LANGUAGE=C|CXX`. The archive form uses the installed package.
`YUKILINKER_CONSUMER_NOEXCEPT=ON` checks disabled exceptions and RTTI. Run a
consumer with `bootstrap_host --system <absolute-core.so> <fixtures>` to check
Core unloading followed by fork and process-exit cleanup.

CI builds both ABIs, installs the SDK and compiles the Native hosts and consumers.
Local QEMU execution needs a provisioned Bionic runtime. The NDK supplies link
stubs for compilation, so provision executable Bionic libraries separately.
