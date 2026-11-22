# Passion Android 11 development kernel

The `qinggan-11` development kernel (`profiles/common-qinggan-11/components/kernel`) with 802.1Q VLAN support for the Passion profile. `component.json` is a variant recipe: it names the base recipe, whose source, compiler, patches, driver and stock configuration it builds unchanged, and adds its own patches and Kconfig values after them. The release is `kernel-passion-<sha8>`.

## Why VLAN

In the car the Android vendor init service `mega_vlan_config` runs `/vendor/bin/config_vlan.sh`, which creates `eth0.3`, `eth0.4`, `eth0.6`, `eth0.7` and `eth0.10` to `eth0.14` with `vconfig` and their egress priority maps; `/vendor/bin/someipd.sh` waits for `eth0.10` to `eth0.14` and starts one `someipd` per VLAN, and EthernetService gives each VLAN its static `.40` address from the product overlay. The stock API 30 kernel has `# CONFIG_VLAN_8021Q is not set`, so these OEM scripts cannot run unchanged.

## Configuration and ABI

`CONFIG_VLAN_8021Q=y` is the only enabled Kconfig option over the base; GVRP, MVRP and bridge VLAN filtering, which it makes visible, stay off. On its own the option would add `vlan_info` in the middle of `struct net_device` and change the layout and CRC of every symbol that uses it, so the 37 stock vendor modules would no longer load. `vlan-kabi.patch` moves `vlan_info` into the reserved slot `ANDROID_KABI_USE(1, ...)` at the end of `struct net_device`: the layout stays that of the stock kernel, genksyms sees the reserved field, and the module ABI guard of the base recipe checks all 2,594 imported CRCs as before.

## Build

```bash
recipe=profiles/common-passion/components/kernel
src/prepare/kernel/kernel-tool.sh "$recipe" prepare "$PWD" "$WORKSPACE" "$STOCK_API30_BASE"
src/prepare/kernel/kernel-tool.sh "$recipe" build "$PWD" "$WORKSPACE" 2
src/prepare/kernel/kernel-tool.sh "$recipe" publish "$PWD" "$WORKSPACE"
```

Requirements and stages are those of the base recipe. `publish` accepts only the configuration changes of the base plus the variant values and records the variant deviation in the descriptor.
