# Components and recipes

A profile changes the OEM firmware only through declared recipes and components. Preparation applies them to exact OEM bytes and stops on any mismatch, so a machine is the same wherever it is prepared. APK and DEX work runs in Java without a user JDK, see [Java preparation](../why/java-preparation.md).

## Components

A component is a directory `components/<name>/` with `component.json`, inside a profile or in `profiles/common/`. `profile.json` lists the components it uses under `components` as a name and a path. The `type` field of `component.json` says what preparation builds:

| Type | Result |
| --- | --- |
| `apk` | An Android application built from sources, signed with the declared key |
| `jar` | A Java library for the guest framework |
| `native` | A native guest executable or library, built in CI with the pinned NDK and downloaded by its pin |
| `native-library-archive` | Native libraries taken from a pinned archive |

Third-party sources and archives inside a component are pinned by URL and SHA-256 in its `component.json`, see [pins](../why/pins.md).

Several profiles can build one component from shared sources. Their `component.json` lists under `sourceFiles` which files of a component in `profiles/common/` the build takes, including the variant files of their firmware generation, and `sourceEdits` names exact text edits that adapt the shared sources to their firmware, for example by removing an API that their firmware does not declare. An edit whose original text is not found stops preparation. A `native` component takes shared C sources through `sourceFiles` too, without edits.

A `native` component whose output another profile already publishes names that pin under `pin` as `<profile>/<component>`. Preparation downloads the published file, and the profile publishes no file of its own for this component.

## Recipes and guards

A recipe is an entry under `patches` in `profile.json`. It names its input, such as `original:Launcher` for an OEM APK, the components it uses, the list of steps and the signing key. A step is a JSON file in `patches/` with a list of operations: line edits in decompiled sources, added DEX files, removed entries, library declarations and class merges.

The guard is the exact original bytes: each OEM input is pinned by SHA-256 in `profile.json`, and a line edit also pins the SHA-256 of its result. Any difference stops preparation before a changed file reaches a machine. OEM package identities stay unchanged; Voyemu's own Android components and the Java classes of preparation use `com.voyemu.*`.

## Installation and guest configuration

After recipes, three sections of `profile.json` finish the guest:

- `install` places artifacts into the guest image: OEM originals, patched files, components and pinned `files/` entries, each with a target path and mode.
- `configure` sets boot properties, Android settings, the home activity and the services that start with the guest.
- `verify` lists the services, activities and home priorities that a verification boot must observe before `runtime.json` is written.

## Development kernels

Some profiles replace the emulator kernel with a development kernel for their Android generation. The recipe lives in `profiles/common-qinggan-09/components/kernel/` or `profiles/common-qinggan-11/components/kernel/`, and Passion uses the variant in `profiles/common-passion/components/kernel/`, the `qinggan-11` recipe with 802.1Q VLAN support; the `kernels` workflow builds it from pinned sources and publishes the image as a release asset `kernel-<name>-<sha8>`. A profile pins that asset under `android.development_kernel` by URL and SHA-256, and `fetch` downloads it into `.local/toolchains/`. See [CI](../dev/ci.md); the reasons are in [development kernels built in CI](../why/guest-kernels.md).
