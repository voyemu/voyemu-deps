# Voyemu

English | [Русский](README.ru.md) | [Changelog](CHANGELOG.md)

Voyemu emulates Android-based Voyah vehicles (Free, Dreamer, Passion) on macOS, Windows and Linux. It runs the original multimedia (Android, two screens) and instrument cluster (QNX) firmware, downloaded from the user's vehicle or taken from OTA update files; no firmware is included. Voyemu emulates steering-wheel button presses and vehicle parameter input. Through the Voyemu API, scripts take screenshots, read the UI element tree, send input and observe CAN messages.

A firmware profile describes the original inputs and their adaptations; a prepared machine contains the guest images, resources and writable disks.

Voyemu runs on macOS 15+ with Apple Silicon, on Windows x64 and on Linux x64 with glibc 2.34+, the system libcurl and OpenSSL 3. The current release is the prerelease 1.0.0-beta1; the table below gives the status of each profile.

[Install](#install) · [Quick start](#quick-start) · [Documentation](#documentation)

## Profiles and features

| Profile | Status and scope |
| --- | --- |
| `free-09-chinese-20260106-6.0.3` | Free 6.0.3, supported. Both multimedia screens and the QNX cluster, Android checkpoint restore, USB Music, DVR, the synthetic phone scenario and the OEM light/dark theme |
| `free-11-chinese-20260731-7.3.1` | Free 7.3.1, supported. Multimedia and cluster, Classic, Minimalist and Speedy Dash themes, AEB states, USB Music, DVR, Settings and the synthetic phone scenario. OEM navigation requires activation |
| `free-11-russian-20260318-rc1` | Free Sport+ 2026-03-18 RC1, experimental. Multimedia only from the Russian partition snapshot, which has no QNX images. No OEM theme or navigation in the snapshot |
| `passion-11-russian-20260403-2.1.4.80` | Passion, supported for multimedia: both OEM home screens, the synthetic phone scenario, steering-wheel controls, USB Music and Camera. The QNX cluster is experimental. The Russian firmware has no OEM navigation application |
| `free-11-chinese-20250710-6.11.1` | Free 6.11.1, supported. Multimedia and cluster |
| `free-09-chinese-20221230-nxp-rc45` | Free NXP RC45, supported. Multimedia only; the profile has no cluster. OEM navigation requires activation |

Each [profile page](https://github.com/voyemu/voyemu/blob/main/docs/profiles/index.md) describes the guests, the OEM inputs and the notes of one profile.

## Install

The 1.0.0-beta1 archive for macOS arm64 (`.tar.xz`) is on [GitHub Releases](https://github.com/voyemu/voyemu/releases); `brew install voyemu/tap/voyemu` installs it with Homebrew. Release 1.0.0 adds the archives for Linux x64 (`.tar.xz`) and Windows x64 (`.zip`) and the Homebrew formula of the stable release; see [release](https://github.com/voyemu/voyemu/blob/main/docs/dev/release.md).

## Quick start

Run the commands from the installation root, see [release](https://github.com/voyemu/voyemu/blob/main/docs/dev/release.md#installation-root); on Windows run `.\voyemu.exe` in PowerShell with the same arguments. `deps` installs the pinned tools, `fetch` imports the OEM firmware archive, `build` prepares the machine and `start` opens its window.

```sh
./voyemu deps free-09-chinese-20260106-6.0.3
./voyemu fetch free-09-chinese-20260106-6.0.3 --source=path/to/update.zip
./voyemu build free-09-chinese-20260106-6.0.3
./voyemu start machines/free-09-chinese-20260106-6.0.3
```

`status`, `stop` and `check` take the same machine path, and `./voyemu help` prints the complete syntax.

## Documentation

- [Launch and sessions](https://github.com/voyemu/voyemu/blob/main/docs/how/launch.md): lifecycle commands, subsystems, launch options and QNX frames over gRPC.
- [Preparation](https://github.com/voyemu/voyemu/blob/main/docs/how/preparation.md): `deps`, `fetch` and `build`, with the input route of each [profile](https://github.com/voyemu/voyemu/blob/main/docs/profiles/index.md).
- [Profiles and machines](https://github.com/voyemu/voyemu/blob/main/docs/how/profiles-machines.md) and [local roots](https://github.com/voyemu/voyemu/blob/main/docs/how/local-roots.md): where everything is stored, persistent guest data and copying a machine.
- [Voyemu API](https://github.com/voyemu/voyemu/blob/main/docs/how/api.md): requests, ownership, receipts and the reference of every operation.
- [Build](https://github.com/voyemu/voyemu/blob/main/docs/dev/build.md), [tests](https://github.com/voyemu/voyemu/blob/main/docs/dev/tests.md) and [CI](https://github.com/voyemu/voyemu/blob/main/docs/dev/ci.md): building and checking Voyemu from source.
- [All documentation](https://github.com/voyemu/voyemu/blob/main/docs/index.md).

Voyemu code is covered by [MIT](LICENSE). Third-party code and tools retain their own licenses; see [third-party notices](https://github.com/voyemu/voyemu/blob/main/src/licenses/README.md).

OEM firmware files and derived vehicle images are excluded from public source code. Recipes with a `guard` and checked binary offsets describe adaptations; OEM artwork is extracted into the prepared machine. The project license does not grant permission to redistribute OEM materials. Voyemu is not affiliated with the vehicle manufacturer.
