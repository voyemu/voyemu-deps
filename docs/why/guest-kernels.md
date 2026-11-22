# Development kernels built in CI

## Decision

The Free 6.0.3, 6.11.1 and 7.3.1 profiles boot the Android guest with a development kernel instead of the `kernel-ranchu` of the emulator system image. Each kernel is a recipe in `profiles/common-qinggan-09/components/kernel/` or `profiles/common-qinggan-11/components/kernel/`: the pinned emulator kernel source of its Android generation, a few patches and the CAN driver `cis_can_dev`. The Passion 2.1.4.80 profile boots with `profiles/common-passion/components/kernel/`, a variant of the `qinggan-11` recipe with 802.1Q VLAN support. The `kernels` workflow builds the recipe, checks the image against the profile pin and publishes it as a release asset; `fetch` downloads it like any other pinned input. [Components and recipes](../how/components.md#development-kernels) describes how a profile consumes the kernel.

## Context

The OEM CAN service of these firmware versions opens the character device `cis_can_dev` and listens on netlink protocol 21, where the vehicle's CAN traffic arrives. The standard emulator kernel has neither, and on Android 9 protocol 21 is taken by `CONFIG_CRYPTO_USER`. Without them the OEM CAN service and the applications above it see no vehicle. On Passion the OEM vendor scripts create 802.1Q VLAN interfaces `eth0.N` with `vconfig`, and the OEM `someipd` exchanges the vehicle SOME/IP services on them; the standard kernel has no VLAN support. Enabling it alone would change the layout of `struct net_device` and the symbol CRCs that the unchanged emulator vendor modules import, so the variant keeps the VLAN state in a reserved slot of the frozen kernel ABI. A kernel build needs Kbuild with bash, perl and bc, a case-sensitive file system and the pinned kernel compiler; `kernel-tool.sh` runs on macOS and Linux only. OEM inputs never take part in the build.

## Alternatives

**The standard kernel with the CAN service adapted in userspace.** A recipe would rewrite the OEM CAN service or its library to talk to a socket instead of the driver. The adapted code path would no longer be the one the vehicle runs, and every firmware version would need its own rewrite.

**A loadable module for the standard kernel.** A module cannot free netlink protocol 21 from `CONFIG_CRYPTO_USER` built into the Android 9 kernel, and it would still need a build per kernel generation.

**One host NIC per VLAN for Passion.** The host would give Android a separate untagged NIC for every VLAN and tag the frames itself, and the guest would rename the NICs to `eth0.N`. The OEM `config_vlan.sh` would then fail or have to be replaced, and egress priorities would no longer come from its maps.

**A kernel build on every host during preparation.** Every developer and user would install the kernel toolchain and wait for the build; Windows hosts cannot run it at all, and builds on different hosts would have to prove that they give the same bytes.

## Argument

The pin, not the builder, decides which image a machine gets. CI builds the recipe from pinned sources on Linux and publishes only an image whose SHA-256 equals the pin, so every host downloads the same bytes and needs no kernel toolchain. `kernel-tool.sh` runs the same recipe outside CI, so anyone can reproduce an image and check it against the pin. The OEM CAN service stays unchanged and meets the interface it expects.

## Consequences

Changing a kernel means changing its recipe, reproducing the image, updating the pin in every profile that uses it and pushing a release tag; [CI](../dev/ci.md#the-kernels-workflow) describes the procedure. Profiles that do not need the driver keep the standard kernel. A built image proves only the build: a new image still needs a boot of a machine and acceptance of its CAN scenarios. The model process and the driver share one boundary ABI, so the kernel and the `can-simulator` component of a profile change together; see [the vehicle model and CAN](../how/vehicle.md#can-model).
