# Build

Voyemu builds with CMake and Ninja from `src/app/`, always in Release and always with the pinned Zig compiler. One source tree builds for macOS, Windows and Linux; the reasons are in [the Zig toolchain](../why/zig-toolchain.md), [Ninja only](../why/ninja-only.md) and [Release only](../why/release-only.md). The host is written in C++20 and the QNX guest code in C11; [C++ for the host](../why/cpp.md) explains the choice. The sources live in `src/app/` for the C++ host, `src/prepare/` for the Java preparation and its tests, `src/guest/` for the Android and QNX guest code, `src/dev/` for the developer tools, the QNX diagnostics of `src/dev/qnx/` among them, and `src/licenses/` for the third-party notices.

## Tools

The build uses Zig 0.15.2, CMake 3.31.10, Ninja and a JDK 25 for the Java tests, checks and recipes, pinned per build host under `optionalTools.macosBuild`, `optionalTools.windowsBuild` and `optionalTools.linuxBuild` in `src/app/toolchain.json`. Each pin names its URL, SHA-256 and the directory in `.local/toolchains/` where the archive is unpacked. The compiler wrappers in `src/dev/toolchains/` run Zig from that directory, so no system compiler is used. Their Zig cache stays in `.local/build/zig-cache/` unless the caller sets `ZIG_GLOBAL_CACHE_DIR`. Install the tools of the host from these pins into their listed directories and check each SHA-256. Unpack the Zig and CMake archives without their top-level directory, so that `zig` or `zig.exe` lies directly in `.local/toolchains/zig-0.15.2`. Put the Ninja directory and the CMake `bin` directory (`CMake.app/Contents/bin` on macOS) on `PATH`. On macOS the build also needs the Command Line Tools SDK, which `xcode-select --install` installs; Xcode is not required. The macOS toolchain file takes the SDK from `xcrun --sdk macosx --show-sdk-path`, and `-DVOYEMU_MACOS_SDK=<path>` selects another `MacOSX.sdk`. Machine preparation does not use this JDK: it runs on the Java runtime that `deps` installs, see [CI](ci.md).

## Configure and build

```sh
cmake -S src/app -B .local/build/native -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/src/dev/toolchains/zig-macos-arm64.cmake" -DBUILD_TESTING=ON
cmake --build .local/build/native
```

The commands run from the project root; on Windows they run in PowerShell with `zig-windows-x64.cmake`, and on Linux with `zig-linux-x64.cmake`. `-DBUILD_TESTING=OFF` builds the launcher alone. Every configuration passes `-G Ninja` and a Zig toolchain file; CMake rejects another generator, so a tree configured with another generator needs a fresh build directory, and the build type is forced to Release. With the build tree `.local/build/native/` the launcher lands in the project root; any other tree keeps it inside the tree.

| Toolchain file | Target |
| --- | --- |
| `zig-macos-arm64.cmake` | macOS on Apple Silicon with the Command Line Tools SDK |
| `zig-windows-x64.cmake` | Windows x64 with MinGW-w64, natively on Windows or cross-built on Linux |
| `zig-linux-x64.cmake` | Linux x86-64 with a glibc 2.34 baseline |

The glibc 2.34 baseline is the oldest glibc of the supported Linux distributions: RHEL 9 has 2.34, Ubuntu 22.04 has 2.35 and Debian 12 has 2.36. Each of them ships libcurl and OpenSSL 3, which the launcher loads at run time instead of linking a TLS library of its own; see [deps](../how/preparation.md#deps).

The toolchain files turn off debug information and PDB files. Diagnose with [logs](../how/logs.md), not a debugger.

## Optimization

Release compiles every target with `-Os`, the pinned third-party sources included; `src/app/CMakeLists.txt` sets the flags. Every function and data object gets its own section (`-ffunction-sections -fdata-sections`), so the linkers drop what nothing references: ld.lld through the `--gc-sections` that Zig always passes, lld-link through its default `/OPT:REF`, and the Zig Mach-O linker per symbol. lld-link also folds identical functions by default (`/OPT:ICF`). On Linux and macOS the C sources, which are only the pinned SDL3, curl, nghttp2 and miniz, compile without unwind tables (`-fno-asynchronous-unwind-tables -fno-unwind-tables`): no exception may unwind through them, so every C++ callback that they call is `noexcept` or catches its exceptions.

Sources that touch every pixel or byte of a frame compile with `-O2`. `voyemu_frame_path_sources` lists the QNX frame conversion, the graphics worker, the Android frame stream, the PNG decoder of the window, the boot animation and the startup videos; the SDL3 sources of stb_image, surfaces, pixel formats and blitters follow them. Every source on the per-frame path of the window belongs to this list. A build configured with `-DBUILD_TESTING=OFF`, as the release builds the launcher, compiles and links with full link-time optimization (`-flto=full`) on Linux and Windows; test builds and macOS link without it, see [size-optimized Release](../why/size-optimized-release.md).

The data that the launcher embeds, the profile catalog, the project files of `src/app/project_files.cmake` and the images of the window, is stored as raw deflate streams that `src/app/embedded_deflate.cmake` writes with the deflate of CMake; the launcher inflates each with miniz on first use.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Builds the tests and registers them with CTest |
| `VOYEMU_BUILD_CLUSTER` | `ON` | Builds the SDL3 window |
| `VOYEMU_BUILD_GRAPHICS` | `ON` | Builds the QNX graphics worker with ANGLE |
| `VOYEMU_EXECUTABLE_OUTPUT_DIRECTORY` | See above | Where the launcher is written |
| `VOYEMU_JDK` | Pinned JDK of the host | JDK home for Java tests and checks |

## Source dependencies

Third-party sources are downloaded at configure time into the build cache, never into the source tree. Dear ImGui, nlohmann/json, miniz, lzokay and the libarchive headers are pinned in `src/app/dependencies.lock.json`, SDL3 in `src/app/platform/component.json`. On Linux the same file pins under `window_linux` the X11, Wayland, xkbcommon and libdecor sources whose headers the X11 and Wayland drivers of SDL3 compile against, and expat for the `wayland-scanner` that the configuration builds from the Wayland sources; SDL3 loads the libraries of the system when the window opens, so the build takes nothing from the build host. CMake verifies every file against its pin before building; see [pins](pins.md).

The build of Voyemu does not compile guest kernels. Development kernels are CI-built release assets that `fetch` downloads by their pins on every host; see [the kernels workflow](ci.md#the-kernels-workflow).

The QNX engine, the e2fsprogs image helpers, the QNX guest binaries and the native Android components are CI-built release assets as well. `deps` downloads the engine by the `prebuilt` pin of `src/app/qnx-host.json` and on macOS and Linux builds it from its recipe only while that pin is pending; preparation downloads the helpers by the `e2fsprogs.helpers` pins, the guest binaries by the `qnxGuestModules` pins and the native components by the `androidNativeComponents` pins in `src/app/toolchain.json`. See [the qnx-host workflow](ci.md#the-qnx-host-workflow), [the e2fs-helpers workflow](ci.md#the-e2fs-helpers-workflow), [the qnx-guest workflow](ci.md#the-qnx-guest-workflow) and [the android-native workflow](ci.md#the-android-native-workflow).

## Windows cross-build

`zig-windows-x64.cmake` cross-builds `voyemu.exe` on Linux with the same commands and the tools of `optionalTools.linuxBuild`, for example into `.local/build/windows`. CI runs selected tests of this build on Linux under Wine and also runs tests in a native Windows job; Wine does not replace native Windows acceptance, see [CI](ci.md).
