# Changelog

English | [Русский](CHANGELOG.ru.md) | [README](README.md)

## 1.0.0-beta2

Unreleased prerelease. It fixes Windows storage checks, Android Quick Boot and native CI. The known issues from beta1 still apply.

- Profiles: Free Sport+ 2026-03-18 RC1 (`free-11-russian-20260318-rc1`), experimental, with multimedia and cluster from a Russian partition snapshot.
- QNX cluster on Windows: `deps` installs the QNX engine of Windows, built in CI from the patched QEMU source, from its pinned release asset, as on macOS and Linux.
- Console of `deps`, `fetch` and `build`: phases, each download with its size, progress, rate and remaining time, each extraction and each step already installed, in the language of the command output; `build` ends with its time and the command that starts the machine. A failed download names its cause. A disk space error names the free and required space, how much to free and the installation root, and `deps` and `fetch` keep 2 GiB free instead of 20 GiB.
- Command log: every command except `check`, `list` and `status` writes its records to `.local/build/logs/<command>-<time>.log`, and the newest 20 logs stay; a failed or interrupted command names its log. `--verbose` on `deps`, `fetch`, `build`, `start` and `restart` also prints the records on `stderr`.
- Ctrl+C stops `deps`, `fetch`, `build` and `start`, removes partial downloads and temporary files and exits with code `130`; an interrupted `start` stops the session it started.
- Paths in CLI options may be relative, start with `~/` and contain symbolic links.
- `build` refuses an existing machine. For profiles that support it, `build --force` updates the Android system of an existing stopped machine and keeps its writable disks.
- Preparation and installation locks are OS file locks that the system releases when the process exits, so a killed `build` no longer blocks the next one.
- Window: Voyemu draws the toggles, the selected radio option and the light panel background itself instead of using OEM textures; the gear, scale and language radios are as high as the input fields.
- Errors: a failure without its own code reports `FILESYSTEM` with the operation, the path and the reason given by the system, or `SYSTEM` or `JSON` with its reason, instead of `INTERNAL_ERROR`.
- There is no default profile: `deps` without a profile installs only the tools, and `deps` and `build` reject an unknown profile.
- Vehicle model on Passion: its own catalog of the 1598 signals the cluster receives over SOME/IP, with raw wire values; `vehicle.fields` gives each Passion field its signal declaration and each bound field its vehicle concept and encoding. Saved Passion scenarios of the earlier catalog are not read.

## 1.0.0-beta1 — 2026-11-18

First public release. Voyemu runs a vehicle's Android multimedia applications and QNX instrument cluster on macOS, Windows and Linux from firmware profiles. On Windows the cluster needs the CI-built QNX engine, which is not published yet; until then machines run without the cluster.

- Profiles
  - Free 6.0.3 (`free-09-chinese-20260106-6.0.3`) with multimedia and cluster.
  - Free 6.11.1 (`free-11-chinese-20250710-6.11.1`) with multimedia and cluster.
  - Free 7.3.1 (`free-11-chinese-20260731-7.3.1`) with multimedia and cluster.
  - Free NXP RC45 (`free-09-chinese-20221230-nxp-rc45`) with multimedia.
  - Passion (`passion-11-russian-20260403-2.1.4.80`) with multimedia and an experimental cluster.

  The Free profiles run on the Free/Dreamer platform. The QNX hardware adapters `sa8155-qinggan09`, `sa8155-qinggan11-611` and `sa8155-qinggan11-731` and their error codes use its internal designations `qinggan09` and `qinggan11`.

- Preparation: pinned tools through `deps`, OEM input import through `fetch` from a local update archive or over ADB, and Android/QNX machine preparation through `build`; Free 7.3.1 and Passion support an Android-only rebuild that preserves user disks.
- Portable machines: a prepared machine embeds its profile and resources; persistent Android/QNX disks keep installed applications, files and settings across cold boots and copies of a stopped machine.
- Lifecycle: `start`, `restart`, `status`, `stop`, `check` and `patch` with positional machine paths, `--cluster=on|off` and `--multimedia=on|off`; command output in English, Russian or Chinese chosen by `--lang` or the host language.
- Unified window with the QNX cluster, two Android multimedia screens with the OEM boot animation of each screen until Android is ready, vehicle parameters and steering-wheel controls; on Free 6.11.1 and 7.3.1 the cluster plays its OEM startup sequence at power-on; 50/75/100% zoom and an English/Russian/Chinese interface.
- Screens window with the other screens of the vehicle, opened by a control next to the wheel: the rear-seat screen of Free 6.11.1 and 7.3.1 with the OEM rear-screen application and its boot animation, and the second QNX screen of Passion, 1280×720, with its OEM startup video; the Voyemu API addresses them as `rear` and `hud`.
- Synthetic phone scenario with Russian contacts, `+7` incoming and outgoing calls and OEM answer/hangup input; no real calls are made.
- Cluster data channel on the Free 6.0.3, 6.11.1 and 7.3.1 profiles: one persistent connection to the cluster with the OEM heartbeat carries the phone call, media and turn-by-turn data from the OEM APIs in OEM IVC frames; the OEM `SystemPolicy` and `QGBus` imported from the firmware produce this data as in the vehicle; `cluster.ipk.history` reads the sent and received frames without their text.
- OEM service `qg.carsignal` on the Free 6.0.3, 6.11.1 and 7.3.1 profiles: one implementation answers as a parked vehicle without the multimedia MCU, so the OEM applications stop waiting for it.
- Voyemu API (`schema: 1`) through `voyemu test`: session ownership, vehicle state, input, UI observations with the live drawable state of Android views and the Kanzi node tree of the Free 6.0.3, 6.11.1 and 7.3.1 clusters with typed node properties, CAN observations, waits, logs, operation receipts and joint screenshots of all three screens; QNX frames over local gRPC.
- Pinned dependencies: every download is fixed by URL and SHA-256; a mismatch stops preparation. The development kernels with the CAN driver of the Free 6.0.3, 6.11.1 and 7.3.1 profiles are built in CI and published as release assets.
- Native C++ host built with Zig; machine preparation needs no JDK: a minimal Java runtime, the Java helper and the guest Java code are built in CI and installed by `deps`; English/Russian documentation and embedded third-party license notices.

### Known limitations

- The Passion cluster has no UI tree.
- Android observations can be partial.
- Command or screenshot success does not prove a visible result.
- Real calls, host microphone input and OEM cloud services are outside the scope of Voyemu.
- The project excludes original firmware materials, which fall outside the project license.

### Known issues

- The QNX instrument cluster shows the wrong startup animation on the `free-09-chinese-20260106-6.0.3` profile.
- On Free 6.11.1 and 7.3.1, preparing a factory machine can exit with code `4` if ADB disconnects while applying the Android wallpaper. A copy of a Free 7.3.1 machine requires a successful base build.
- On Free 6.11.1 and 7.3.1, the rear-seat screen app (ThirdScreen) crashes when SELinux denies a write to the Android property `qinggan.thirdscreen.standby`.
