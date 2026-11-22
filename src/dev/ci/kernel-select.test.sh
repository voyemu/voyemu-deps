#!/bin/bash
set -euo pipefail
# Run the real CI selector against isolated JSON manifests; no kernel inputs are fetched.
project=$(cd "$(dirname "$0")/../../.." && pwd -P)
selector="$project/src/dev/ci/kernel-select.sh"
mkdir -p "$project/.local/build/jobs"
work=$(mktemp -d "$project/.local/build/jobs/kernel-select.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"
export GITHUB_REPOSITORY=voyemu/voyemu GITHUB_REF_TYPE=branch GITHUB_REF_NAME=main INPUT_KERNEL=qinggan-11
export GITHUB_OUTPUT="$work/output"
base=https://github.com/voyemu/voyemu-deps/releases/download/
# Synthetic profiles: the selector reads the platform and the pins, never the names.
p09=synthetic-09
p11a=synthetic-11-first
p11b=synthetic-11-second
nxp=synthetic-09-nxp
passion=synthetic-passion
sha09=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
sha11=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
write_profile() {
    mkdir -p "profiles/$1"
    jq -n --arg platform "$2" --arg sha "$3" --arg url "$4" '{platform:$platform,android:{development_kernel:{sha256:$sha,url:$url}}}' > "profiles/$1/profile.json"
}
# A profile of the platform without a development kernel is not selected.
write_plain_profile() {
    mkdir -p "profiles/$1"
    jq -n --arg platform "$2" '{platform:$platform,android:{}}' > "profiles/$1/profile.json"
}
reset() {
    GITHUB_REF_TYPE=branch GITHUB_REF_NAME=main INPUT_KERNEL=qinggan-11
    mkdir -p src/app
    jq -n --arg base "$base" '{optionalTools:{kernelBuild:{releaseBase:$base}}}' > src/app/toolchain.json
    write_profile "$p09" qinggan09 "$sha09" "${base}kernel-qinggan-09-aaaaaaaa/Image"
    write_profile "$p11a" qinggan11 "$sha11" "${base}kernel-qinggan-11-bbbbbbbb/Image"
    write_profile "$p11b" qinggan11 "$sha11" "${base}kernel-qinggan-11-bbbbbbbb/Image"
    write_plain_profile "$nxp" qinggan09
    write_plain_profile "$passion" passion
    : > "$GITHUB_OUTPUT"
}
accept() {
    bash "$selector"
    printf 'kernel=%s\nsha=%s\ntag=kernel-%s-%.8s\n' "$1" "$2" "$1" "$2" > expected
    cmp expected "$GITHUB_OUTPUT"
    echo "PASS $3"
}
reject() {
    if bash "$selector" > rejected.log 2>&1; then
        echo "FAIL accepted $1" >&2
        exit 1
    fi
    test ! -s "$GITHUB_OUTPUT"
    echo "PASS reject $1"
}
reset
accept qinggan-11 "$sha11" manual-11
reset
write_profile "$p11b" qinggan11 bbbbbbbbbaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "${base}kernel-qinggan-11-bbbbbbbb/Image"
reject second-pin-drift
reset
write_profile "$p11b" qinggan11 "$sha11" 'https://example.invalid/Image'
reject second-url-drift
reset
INPUT_KERNEL=qinggan-09
accept qinggan-09 "$sha09" manual-09
reset
GITHUB_REF_TYPE=tag GITHUB_REF_NAME=kernel-qinggan-11-bbbbbbbb INPUT_KERNEL=qinggan-09
accept qinggan-11 "$sha11" tag-overrides-input
reset
GITHUB_REF_TYPE=tag GITHUB_REF_NAME=kernel-qinggan-09-aaaaaaaa
accept qinggan-09 "$sha09" tag-09
reset
GITHUB_REF_TYPE=tag GITHUB_REF_NAME=kernel-qinggan-11-aaaaaaaa
reject tag-pin-mismatch
reset
GITHUB_REF_TYPE=tag GITHUB_REF_NAME=kernel-qinggan-11-invalid
reject malformed-tag
reset
INPUT_KERNEL=unknown
reject unknown-kernel
reset
printf '{' > "profiles/$p11b/profile.json"
reject malformed-manifest
reset
write_profile "$p11b" qinggan09 "$sha11" "${base}kernel-qinggan-11-bbbbbbbb/Image"
INPUT_KERNEL=qinggan-09
reject other-platform-pin
reset
write_plain_profile "$p11a" qinggan11
write_plain_profile "$p11b" qinggan11
reject no-platform-kernel
for field in sha256 url; do
    reset
    jq "del(.android.development_kernel.$field)" "profiles/$p11b/profile.json" > changed
    mv changed "profiles/$p11b/profile.json"
    reject "missing-$field"
done
reset
write_profile "$p11a" qinggan11 bbbbbbbb "${base}kernel-qinggan-11-bbbbbbbb/Image"
write_profile "$p11b" qinggan11 bbbbbbbb "${base}kernel-qinggan-11-bbbbbbbb/Image"
reject malformed-sha256
reset
write_profile "$p09" qinggan09 "$sha09" 'https://example.invalid/Image'
INPUT_KERNEL=qinggan-09
reject 09-url-drift
reset
printf '{"optionalTools":{"kernelBuild":{"releaseBase":"https://example.invalid/"}}}\n' > src/app/toolchain.json
reject foreign-release-base
reset
printf '{}\n' > src/app/toolchain.json
reject missing-release-base
# The Passion variant kernel is selected by its own platform, independently of the qinggan11 pin.
shapassion=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
reset
INPUT_KERNEL=passion
reject passion-without-pin
reset
write_profile "$passion" passion "$shapassion" "${base}kernel-passion-cccccccc/Image"
INPUT_KERNEL=passion
accept passion "$shapassion" manual-passion
reset
write_profile "$passion" passion "$shapassion" "${base}kernel-passion-cccccccc/Image"
GITHUB_REF_TYPE=tag GITHUB_REF_NAME=kernel-passion-cccccccc
accept passion "$shapassion" tag-passion
reset
write_profile "$passion" passion "$shapassion" "${base}kernel-qinggan-11-cccccccc/Image"
INPUT_KERNEL=passion
reject passion-url-names-other-kernel
