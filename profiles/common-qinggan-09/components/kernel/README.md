# Free/Dreamer Android 9 development kernel

Linux 4.4.302 goldfish kernel for the API 28 emulator base of the A9 profiles. Machine preparation replaces the stock `kernel-ranchu` with this Image so the OEM CAN service finds the `cis_can_dev` character device and netlink protocol 21, and Android sees the USB drive as a USB mass-storage disk. It is a development kernel for a simulated ECU, not an OEM kernel and not proof of hardware compatibility.

## Inputs

`component.json` pins the external inputs and lists the recipe patches and driver files in order; the recipe files themselves carry no pins, `prepare` records their hashes and later steps reject a recipe edited since, and the published Image pin covers them. The source is goldfish commit `cc24b0a8f68af21e7fc2a6480646866d3ccdf83e`, matching the stock API 28 4.4.302 kernel. Gitiles archives embed the request time, so the extracted tree is verified by its Git tree id and a SHA-256 over sorted mode, content hash and path lines instead of the archive bytes. The compiler is NDK 29.0.14206865 clang 21 for the host, not the stock clang 9; clang, lld and GNU make are pinned per host. `clang21.patch` keeps the 4.4 tree building with clang 21, `netlink-reset.patch` adds the netlink reset, `oid-registry-path.patch` keeps the absolute script path out of a generated source whose DWARF checksum would change the build ID, and `cis_can_dev.c` with `can_boundary.h` is the private driver and its boundary ABI; `can_datagram.h` lets the model device write both 10-byte CAN frames and longer service datagrams (up to 65536 bytes) to the protocol-21 multicast group, as the car does.

Host tools come from `src/app/toolchain.json` (`optionalTools.kernelBuild`): GNU sed, GNU gzip and OpenSSL libcrypto built from pinned sources with the pinned Zig, and Android build-tools for the host: toybox with its libraries (`date`, `find`, `tar`, `echo` applets), plus xz, bison, flex and m4 used by the API 30 recipe. Zig is also the host C compiler. No Homebrew, system compiler or system OpenSSL is used.

## Build

Supported build hosts are macOS and Linux x86_64 with JDK 25 or newer, the pinned NDK in `.local/toolchains/native-sdk/ndk/29.0.14206865` and the pinned Zig. The workspace must be on a case-sensitive file system and its path must not contain spaces. Kbuild needs `bash`, `perl`, `bc` and POSIX file utilities from the host.

```bash
recipe=profiles/common-qinggan-09/components/kernel
src/prepare/kernel/kernel-tool.sh "$recipe" prepare "$PWD" "$WORKSPACE"
src/prepare/kernel/kernel-tool.sh "$recipe" build "$PWD" "$WORKSPACE" 2
src/prepare/kernel/kernel-tool.sh "$recipe" publish "$PWD" "$WORKSPACE"
```

`prepare` downloads the source if missing, verifies the tree, applies the patches and installs the driver. `build` installs missing host tools, runs Kbuild with 1 to 4 jobs, checks the configuration and audits relocations. `publish` writes the content-addressed Image, `build.json` and the profile descriptor `development-kernel.json` under `.local/profiles/common/development-kernel/<sha256>/`.

The `kernels` workflow builds this recipe on Linux, verifies the Image against the profile pin and uploads it as the `Image` asset of release `kernel-qinggan-09-<sha8>`. Profiles pin that URL and SHA-256; `fetch` downloads it. Every host downloads the published Image; a local build only reproduces or updates it.

## Reproducibility

The build runs with a minimal environment: `LC_ALL=C`, `TZ=UTC`, fixed `KBUILD_BUILD_TIMESTAMP`, `KBUILD_BUILD_USER`, `KBUILD_BUILD_HOST` and `KBUILD_BUILD_VERSION`, and `-ffile-prefix-map` for the workspace and the compiler, so neither the Image nor `vmlinux` depends on the build path, user, host or time. `build` refuses to continue if the pinned toybox cannot run, because Kbuild would silently stamp the initramfs with the current time. The embedded configuration is compressed by the pinned gzip.

The normalized configuration equals the stock embedded configuration except `CONFIG_CRYPTO_USER=n`, which frees netlink protocol 21, and built-in `CONFIG_SCSI`, `CONFIG_BLK_DEV_SD` and `CONFIG_USB_STORAGE`, which with the stock PCI EHCI driver attach the USB drive of the emulator as a SCSI disk; `olddefconfig` adds `CONFIG_SCSI_DMA`, `CONFIG_SCSI_LOWLEVEL` and `CONFIG_SCSI_PROC_FS`, and every other option these make visible stays unset. `publish` checks this exact set. SELinux and module versioning stay enabled. Linker `-z notext` is retained; the relocation audit permits only `R_AARCH64_RELATIVE` and does not prove runtime page protections.

`build-complete.json` binds the component, Image and vmlinux hashes after the source, configuration and relocation checks; `publish` rejects a stale receipt and records `runtime_accepted=false`. A source build is not runtime acceptance: each new Image needs a machine boot, native receiver checks and cold-OFF boundary acceptance.

## Driver boundary ABI

The private request 3 ABI is 176 bytes; argument 0 keeps the first-open query. The kernel, the `can-simulator` daemon and the `can-model-observer` helper of a profile must use the same `can_boundary.h`.

## Development model

The model sources are in the profile `components/can-simulator/src/` and describe a development simulator. The transport queues ten-byte commands independently of model feedback.

The model fixture is one exact line: `voyemu-sim-v1 <forced> <warning> <drive> <eps> <retention> <propulsion> <daily-repeat> <boot-acc>`. `boot-acc` is 0 or 2. The driver admits it only on the first model open per kernel boot; a later model restart starts with unknown ACC. Values are explicit simulated inputs, not vehicle facts.

## Model observer protocol

The observer protocol is separate from the OEM CAN ABI. The endpoint is the init socket `voyemu_can_observe`, Unix `SOCK_SEQPACKET`, intended to be root-only and restricted by enforcing SELinux.

A request is exactly 24 bytes: six big-endian 32-bit words `[0x4f425331, op, session_hi, session_lo, cursor_hi, cursor_lo]`. Op 1 is a snapshot and requires zero session and cursor; op 2 reads one event by nonzero session and sequence. Other forms are invalid.

A reply is exactly 148 bytes, 37 big-endian 32-bit words:

| Words | Content |
| --- | --- |
| 0-1 | Magic; signed status: 0 success, -1 invalid, -2 GAP, -3 UNKNOWN |
| 2-5 | Raw 16-byte kernel boot UUID |
| 6 | Driver first-open witness |
| 7-16 | Pairs: model session, power epoch, snapshot `CLOCK_BOOTTIME` ns, earliest retained sequence, latest sequence |
| 17-22 | Pairs: event sequence, event epoch, event `CLOCK_BOOTTIME` |
| 23-25 | Event kind, signed state, signed value |
| 26-29 | Snapshot simulated ACC, warning, forced EV, drive; -1 is unknown |
| 30-31 | `CLOCK_BOOTTIME` of the last model boot, state change or loss observation |
| 32-35 | Up to ten raw frame bytes with padding; frame length |
| 36 | Flags; bit 0: kernel ioctl admissions before model read are not in this journal |

Event kinds: 1 boot fixture, 2 semantic admission, 3 raw frame read by model, 4 state change, 5 RX emission attempt, 6 loss or reset, 7 dropped request, 8 no-op. Admission does not mean applied, a model read does not prove when the ioctl was accepted, and an emission attempt does not prove delivery. Loss means continuity is uncertain, including queued TX discarded by a reset or a lost reply. State changes and emissions can repeat; consumers use sequence and epoch, not values, to infer intent.

The model keeps 256 events. A cursor older than the retained range or from another session returns GAP; a cursor newer than the latest returns UNKNOWN. A restart changes the session and drops the journal. Consumers treat GAP, UNKNOWN, loss, endpoint failure and restart as missing evidence. Periodic feedback consumes ring capacity, so callers must drain promptly.
