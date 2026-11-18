# Voyemu dependencies

Pinned build and runtime assets for [Voyemu](https://github.com/voyemu/voyemu). Each release records the source and build provenance of its assets.

## Licensing

The MIT license in this repository covers Voyemu's own documentation and workflow code. Published artifacts retain their respective component licenses. See the notices, license files and source packages attached to or referenced by each release. QEMU builds include their corresponding source archive alongside the binary. This repository does not redistribute vehicle firmware.

## CI caches

The cache workflow runs on the default branch so resource tags can reuse those entries. For a new recipe, dispatch `caches.yml` with the full Voyemu source commit SHA and update the scheduled SHA when the resource tags change.
