#!/bin/bash
# Run from the checkout root before installing or building kernel inputs.
set -euo pipefail
if [[ "$GITHUB_REF_TYPE" == tag ]]; then
    [[ "$GITHUB_REF_NAME" =~ ^kernel-(qinggan-(09|11)|passion)-([0-9a-f]{8})$ ]] || { echo "Unexpected tag $GITHUB_REF_NAME" >&2; exit 1; }
    kernel=${BASH_REMATCH[1]}
else
    kernel=$INPUT_KERNEL
fi
case "$kernel" in
    qinggan-09) platform=qinggan09 ;;
    qinggan-11) platform=qinggan11 ;;
    passion) platform=passion ;;
    *) echo "Unknown kernel $kernel" >&2; exit 1 ;;
esac
# Every profile of the platform that pins a development kernel must pin this one.
profiles=()
for definition in profiles/*/profile.json; do
    pins=$(jq -r --arg platform "$platform" '.platform == $platform and (.android.development_kernel | type) == "object"' "$definition")
    if [[ "$pins" == true ]]; then
        profiles+=("$(basename "$(dirname "$definition")")")
    fi
done
(( ${#profiles[@]} )) || { echo "No $platform profile pins a development kernel" >&2; exit 1; }
base=$(jq -er '.optionalTools.kernelBuild.releaseBase | strings' src/app/toolchain.json)
[[ "$base" == "https://github.com/voyemu/voyemu-deps/releases/download/" ]] || { echo "Invalid dependency release base: $base" >&2; exit 1; }
sha=
for profile in "${profiles[@]}"; do
    pin=$(jq -er '.android.development_kernel.sha256 | strings' "profiles/$profile/profile.json")
    [[ "$pin" =~ ^[0-9a-f]{64}$ ]] || { echo "Invalid SHA-256 in $profile" >&2; exit 1; }
    if [[ -z "$sha" ]]; then
        sha=$pin
    fi
    [[ "$pin" == "$sha" ]] || { echo "Profile $profile differs from the $kernel Image pin" >&2; exit 1; }
    tag="kernel-$kernel-${sha:0:8}"
    url=$(jq -er '.android.development_kernel.url | strings' "profiles/$profile/profile.json")
    [[ "$url" == "$base$tag/Image" ]] || { echo "Profile $profile URL $url does not name $tag" >&2; exit 1; }
done
if [[ "$GITHUB_REF_TYPE" == tag && "$GITHUB_REF_NAME" != "$tag" ]]; then
    echo "Tag $GITHUB_REF_NAME differs from the profile pin $tag" >&2
    exit 1
fi
{
    echo "kernel=$kernel"
    echo "sha=$sha"
    echo "tag=$tag"
} >> "$GITHUB_OUTPUT"
