# Pins

Every external input has an exact version, a URL and a SHA-256, recorded in one of the files below. Tools check the SHA-256 before using a file, and a mismatch stops the step. The rule and its reasons are in [pins for external inputs](../why/pins.md).

## Where pins live

| File | What it pins |
| --- | --- |
| `src/app/toolchain.json` | Android SDK parts (command-line tools, Platform Tools, Emulator, Build Tools) and the files of their archives that Voyemu checks or runs directly, such as the Emulator, its e2fsck and its ANGLE and SwiftShader libraries and `android.jar` of the platform archive, NDK with the file list of the archive of each host, SDK CMake and Ninja, the JDK, e2fsprogs (its release tarball and the image helpers of each host built in CI), the QNX guest binaries of each hardware adapter built in CI with the initial native state they are built with, the native Android components of each profile built in CI, the guest Java code of the profiles built in CI, the Java runtime of preparation (its modules, the Temurin jmods of each host and the runtime of each host built in CI), the Java helper of preparation built in CI, the build tools of each host (Zig, CMake, Ninja, JDK), clang-format, clang-tidy, Uncrustify (its release tarball and the executable of each host built in CI), google-java-format, Checkstyle, Error Prone, apktool and smali, the sources of the development kernel build and the release base of kernel images |
| `src/app/qnx-host.json` | The QEMU engine for QNX per host with its patches and extractor, and under `prebuilt` the engine of each host built in CI |
| `src/app/dependencies.lock.json` | Native third-party sources compiled into Voyemu, with the SHA-256 of each file |
| `src/prepare/toolchain.json` and `src/prepare/libraries.json` | Java libraries and Android build tool JARs used by preparation |
| `component.json` | Third-party sources of one component: SDL3 and the X11 and Wayland headers of its Linux drivers in `src/app/platform/`, HTTP/2 libraries in `src/app/api/`, guest components in profiles |
| `profile.json` | The Android system image with the pins of its contents in `android.clean_base` (the file list of the extracted archive, the boot files, the system and vendor images, the logical partitions of an API 30 image and single guest files), the development kernel image, OEM inputs and their acquisition from firmware archives |
| `profile-qnx.json` | OEM QNX images of a profile |

OEM inputs are pinned by name, size and SHA-256 only; their bytes never enter the repository. Files of the repository itself are never pinned. Code reads every pin from its file and never repeats it.

The Passion field catalog `src/app/core/passion_catalog.inc` is generated from an OEM file: `src/app/core/generator/GeneratePassionCatalog.java` reads the SOME/IP service table `H53_SOMEIP_ServiceTable_V6.0.0_OIB_8155_QNX_20240103-142433.json` extracted from `system_la.img` of the Passion profile, checks its SHA-256, which is also the `map_sha256` of a Passion state, and writes the catalog; its Javadoc gives the commands.

System libraries that a host loads at run time are not downloads and have no pin: on Linux these are libcurl and OpenSSL 3 libcrypto, loaded by their stable names `libcurl.so.4` and `libcrypto.so.3`.

## Updating a dependency

1. Take the latest release of the dependency unless a recorded reason requires another version.
2. Change its version, URL and SHA-256 together in the one file that pins it.
3. Update the files that consume the pin, such as a toolchain file or a recipe, in the same change.
4. Build and run the affected [tests](tests.md) and [checks](checks.md).

The CI workflows install their tools from the same pins, so a pin change takes effect in [CI](ci.md) without editing the workflow.

The Linux executables built in CI, the e2fsprogs image helpers and Uncrustify, take the glibc baseline of the launcher from `src/dev/toolchains/zig-linux-x64.cmake`. A change of that baseline, like a change of the Zig pin, changes their bytes: rebuild them and update their pins and release tags as [CI](ci.md) describes.
