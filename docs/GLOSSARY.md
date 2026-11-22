# Glossary

The single terminology reference for Voyemu docs, README, CHANGELOG, SECURITY and CONTRIBUTING, paired with [GLOSSARY.ru.md](GLOSSARY.ru.md); edit both by meaning. The canonical form is in the left column; the variants on the right are not used. Code, CLI options and API names keep their identifiers in code spans; prose uses the canonical terms.

`java src/dev/checks/CheckDocs.java` rejects every variant in a "Do not use" column outside code spans and code blocks. A variant with a footnote mark is correct in the context the footnote names; review enforces it.

## Product

| Term | Meaning | Do not use |
| --- | --- | --- |
| Voyemu | The project and its executable `voyemu` (`voyemu.exe` on Windows) | — |
| launcher¹ | The installed `voyemu` executable in the installation root; it runs the CLI, the window and the graphics worker | — |
| host | The computer that runs Voyemu: macOS, Windows or Linux | server² |
| guest | An operating system that Voyemu runs in an emulator: Android or QNX | VM guest, guest VM |
| multimedia | The Android guest with the primary and secondary screens; CLI `--multimedia`, API `multimedia.*` | head unit, IVI, infotainment |
| cluster | The QNX guest that shows the instrument cluster; CLI `--cluster`, API `cluster.*` | dashboard, instrument panel |
| subsystem | Multimedia or cluster, selected at start with `--multimedia` and `--cluster` | — |
| window | The host window with the cluster, both multimedia screens and vehicle controls | main window, UI shell |
| graphics worker | The launcher process that renders QNX frames through ANGLE for the window and gRPC; `graphics-*.log` | ANGLE worker |

## Profiles and machines

| Term | Meaning | Do not use |
| --- | --- | --- |
| profile | The authoring directory `profiles/<profile>/`: `profile.json`, `capture.json`, components, recipes and pins for one firmware version | preset, firmware config |
| component | A directory `components/<name>/` with `component.json` inside a profile or `profiles/common/` | module³ |
| OEM | Belonging to the original vehicle firmware: OEM application, OEM theme, OEM package identity | stock |
| OEM input | An original firmware file the user imports into `.local/profiles/<profile>/`; never part of the repository | owner input, raw input |
| recipe | A declarative adaptation of an OEM input that a profile or component describes | — |
| `guard` | A check of the exact original bytes that a recipe requires before it applies | — |
| pin | An exact version with URL and SHA-256 for an external download, OEM input or third-party source | — |
| prepare, preparation | Turning a profile and its OEM inputs into a machine; CLI `build PROFILE` | build a machine⁴ |
| machine | A prepared directory `machines/<name>/`: `runtime.json`, embedded profile and resources, boot files and writable disks | VM, instance, virtual machine, ready machine |
| `.local` roots | The four local directories `profiles`, `toolchains`, `build` and `machines` under `.local/` | — |
| toolchain | Pinned build tools: Zig, CMake, Ninja, JDK | — |
| hardware adapter | A replacement for the vehicle hardware that the OEM QNX images expect, selected by `hardware_adapter` in `profile-qnx.json` | — |
| Free/Dreamer platform | The multimedia and cluster platform shared by the Voyah Free and Dreamer vehicles, in an Android 9 and an Android 11 generation; code identifiers, hardware adapters and development kernel recipes use the internal designation `qinggan09` and `qinggan11` for them | Free09, Free11, Free611, Free731, Qinggan platform, A9, A11, qinggan09, qinggan11 |
| development kernel | An Android emulator kernel built from a recipe in `profiles/common-*/components/kernel/` by the `kernels` workflow and pinned under `android.development_kernel` | custom kernel |
| workflow | A GitHub Actions workflow in `.github/workflows/`, such as `ci` or `kernels` | — |
| content provider | An Android `ContentProvider` of a guest application that a recipe adapts; not a [provider](#sessions-and-api) | — |

## Sessions and API

| Term | Meaning | Do not use |
| --- | --- | --- |
| session | One run of a machine's guests under a supervisor; at most one per machine | — |
| supervisor | The host process that owns a session and its guest processes | daemon |
| ownership | The right to change a session, proven by `ownership_id`; `session.attach` grants read access only | — |
| generation | The number that identifies one guest start; a reboot creates a new generation (`qnx_generation`, `android_generation`) | — |
| Voyemu API | The public interface for scripts: `voyemu test` reads one JSON request with `schema: 1` and writes one JSON response; `info` lists the operations | JSON API |
| operation | One Voyemu API request, named by its `op` field and executed by `voyemu test` | command⁵ |
| receipt | The saved result of a registered operation, returned again for a repeated request | — |
| outcome | The classification of an operation result: `completed`, `not_started` or `unknown` | — |
| screen role | `primary`, `secondary`, `cluster`, `rear` (the rear-seat screen of Free 6.11.1 and 7.3.1) or `hud` (the second QNX screen of Passion, 1280×720); neither an emulator display number nor a guest display ID | display slot, third screen |
| screens window | The second host window with the other screens of the vehicle, opened by the control next to the wheel | extra window |
| interguest link | The QNET and Ethernet relay between the Android and QNX guests | — |
| `flush` | The request that the host sends to QNX through the mailbox of the interguest link: QNX syncs and remounts its storage before the host pauses or stops it, then answers with an ACK and a status | — |
| vehicle model | The host-side simulated vehicle state that `patch` and `vehicle.set` change | — |
| field | One named value of the vehicle model | — |
| CAN model | The simulated CAN bus of the multimedia guest; API `vehicle.can.*` | — |
| MCU field | One value of the OEM MCU protocol that a cluster adapter supports | — |
| cluster data channel | The persistent connection of the multimedia to `ivcd` of the cluster that carries the media title, the phone call and turn-by-turn guidance in OEM IVC frames; API `cluster.ipk.history` | IPK sync link, IPK channel |
| observer | The Android component that reads the UI tree for `session.ui` | — |
| provider | An external process registered as an observation source; an Android content provider is a separate term | — |
| screenshot | An image of screens taken by `session.screenshot` | capture⁶ |
| frame | One image that a guest presents; QNX frames are also delivered over gRPC | — |
| discovery file | `qnx-frame-grpc.json` with the host, port and token of the QNX frame gRPC endpoint for the current cluster generation | — |
| cold boot | A guest start without restoring RAM | cold start |
| warm reboot | A guest reboot that keeps the session and its ownership; `mode: warm` of `multimedia.reboot` | warm restart |
| checkpoint | Saved guest RAM that a start can restore instead of a cold boot; the Russian docs say «снимок RAM». For Android it is the emulator RAM that the supervisor saves when a session stops. For QNX it is the RAM with its QEMU state; only a start without the interguest link and persistent storage restores it, so prepared machines cold-boot QNX, and the `checkpoint.selected` log event records the choice and its reason | RAM snapshot |
| acceptance | Checking a guest scenario on a running machine; host tests do not replace it | — |
| QNX startup script | The shell script `qnx-runtime.sh` that preparation installs into the QNX guest; it sends the animation-ready signal and asks the session to switch the ignition on | cluster runtime |

## Prose

1. State the facts the user needs; no enthusiasm, guesses about the audience or filler.
2. Name a feature by its effect for the user (what is emulated, what the user can do), not by implementation details such as the window count or internal platform names.
3. Plain language: a common word instead of a term when the term adds nothing.
4. No address to the reader: no "you". Facts and behavior are stated in the third person (Voyemu, the command, the developer, the profile does X); procedures are numbered steps or bare imperatives ("Change the recipe, reproduce the image, record the pin").
5. Numbers with five or more digits group their digits with commas in prose (65,536 bytes); values and bounds of fields and parameters from code stay as in code (`timeout_ms` from 1 to 10000, 4294967295).
6. Name a profile by its model and firmware version, as the user sees them: Free 6.0.3, Free 6.11.1 and 7.3.1, Free NXP RC45, Passion; never by internal shorthand such as A9 or A11. Where the platform generation matters, write Android 9 or Android 11; identifiers such as `qinggan09` stay in code spans.

---

## Footnotes

¹ Write "launcher"; "executable" is correct where the file itself matters. An Android "launcher" is a home-screen application of the guest.

² "Server" is correct for a network service: the ADB server, the gRPC endpoint of QNX frames.

³ "Module" is correct for kernel modules and Java modules.

⁴ "Build" names the CLI command `build` and compiling Voyemu from source; for what `build PROFILE` does, write "prepare".

⁵ "Command" is correct for a CLI command such as `start` or `test`.

⁶ "Capture" keeps its own meaning in `capture.json` and the reading of OEM inputs from a vehicle that it describes.
