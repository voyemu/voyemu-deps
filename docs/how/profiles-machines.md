# Profiles and machines

A profile describes one firmware; a machine is what preparation makes from it. Preparation reads the profile, and launch reads only the machine. The reasons are in [profiles as sources, machines as results](../why/profiles-machines.md).

## Profile

A profile is the authoring directory `profiles/<profile>/`. Its name encodes the vehicle line, the Android generation, the firmware language, the firmware date and its version, for example `free-09-chinese-20260106-6.0.3`. `voyemu list` prints the known profile identifiers; the [published profiles](../profiles/index.md) have their own pages.

| File or directory | Content |
| --- | --- |
| `profile.json` | Android system image pin, emulator settings, displays, OEM inputs with SHA-256, recipes, installed files, guest configuration and checks |
| `capture.json` | Which OEM files an import takes from a physical vehicle |
| `profile-qnx.json` | OEM QNX images, the hardware adapter, storage settings and the OEM inputs taken from another profile; only in profiles with a cluster |
| `components/` | [Components](components.md) of this profile |
| `patches/` | Recipe steps that `profile.json` refers to |
| `files/` | Small own files that preparation installs into the guest |

The `status` field of `profile.json` is `ready` when the multimedia is supported and `incomplete` while the OEM inputs of the profile are still being collected: then `fetch PROFILE --source=adb --serial SERIAL` reads them as `capture.json` lists. The `status` field of `profile-qnx.json` is `incomplete` while the cluster is experimental. The support of each published profile is listed in [profiles](../profiles/index.md).

The `title` field of `profile.json` names the profile in text for people, for example `Free 7.3.1`. `platform` is `qinggan09` or `qinggan11` for the Android 9 or Android 11 generation of the [Free/Dreamer platform](../GLOSSARY.md), or `passion` for Passion. `board` names the multimedia board: `8155` for Qualcomm SA8155 or `nxp` for NXP i.MX 8QuadMax. Voyemu rejects a published profile without these fields or with another value.

Shared parts live outside the profiles: `profiles/common/` holds common components, fonts, icons and steering-wheel artwork, and `profiles/common-qinggan-09/` and `profiles/common-qinggan-11/` hold the development kernel recipes of each Android generation, `profiles/common-passion/` the Passion variant of the `qinggan-11` recipe.

OEM inputs never enter the repository. The user imports them into `.local/profiles/<profile>/`, and the profile records only their names, sizes and SHA-256.

## Machine

A machine is a prepared directory under `machines/<name>/`. `voyemu build PROFILE` creates `machines/PROFILE` by default; `build PROFILE machines/NAME` picks another name. The destination must be a new directory directly inside the installation's `machines/`.

| Path | Content |
| --- | --- |
| `runtime.json` | Machine manifest: profile identity, guest images, boot files and resources with sizes and SHA-256 |
| `profile.json` | The embedded copy of the authoring profile |
| `android/` | Android system and vendor images, the kernel, writable user and cache disks, installed packages |
| `qnx/` | QNX boot images and the writable QNX disk; only when the cluster is prepared |
| `ui/` | Voyemu logo, OEM boot animations in `ui/startup/`, steering-wheel artwork and button routes |

`runtime.json` appears last, after the guests have booted for verification, so a directory without it is not a machine. Lifecycle commands select a machine by its path, relative or absolute, also outside the installation root, and infer the profile from `runtime.json`.

## Copying and moving a machine

A machine carries everything a launch needs from the profile, so it runs without `profiles/` and moves as one directory. Writable disks inside the machine, the Android QCOW2 layers among them, keep installed applications, files and settings: copy or archive the whole machine directory after a clean stop. Session state lives separately in `.local/machines/<name>/` and is recreated when absent; see [local roots](local-roots.md).

A machine needs the installed host tools to run. A cluster launch also needs the QEMU and ANGLE binding in `.local/toolchains/installed.json`, which machine preparation writes on its host; `deps` installs the engine and `start` does not create the binding, so on a host without it start the machine with `--cluster=off`. `voyemu check MACHINE_PATH` verifies the stored files and the launch configuration without starting guests.

Give independent copies distinct directory names: session state is keyed by the machine directory name, and each machine has at most one session.
