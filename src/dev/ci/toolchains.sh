#!/bin/bash
# Installs the pinned tools and source archives of one CI toolchain set into .local/toolchains and
# checks every SHA-256; with --env it only adds the installed tools of the set to GITHUB_PATH and
# GITHUB_ENV. The CI workflows and the cache refresh workflow share the sets through
# .github/actions/toolchains, so each set has one cache content.
# Usage: src/dev/ci/toolchains.sh [--env] lint|linux|tidy-windows|windows-cross|uncrustify|macos|kernel-qinggan-09|kernel-qinggan-11
set -euo pipefail
cd "$(dirname "$0")/../../.."
pins=src/app/toolchain.json
env_only=false
if [[ "${1:-}" == --env ]]; then
    env_only=true
    shift
fi
set_name=${1:?toolchain set required}
fixture_inputs=${2:-false}
downloads=.local/toolchains/downloads
mkdir -p "$downloads/sources"

check() {
    if command -v sha256sum > /dev/null; then
        echo "$1  $2" | sha256sum -c -
    else
        echo "$1  $2" | shasum -a 256 -c -
    fi
}

fetch() {
    curl -fsSL --retry 3 -o "$3" "$1"
    check "$2" "$3"
}

# Archive tool of optionalTools.<host>: Zig, CMake, Ninja or the JDK.
host_tool() {
    local host=$1 tool=$2 url sha dir archive
    url=$(jq -er ".optionalTools.$host.$tool.url" "$pins")
    sha=$(jq -er ".optionalTools.$host.$tool.sha256" "$pins")
    dir=$(jq -er ".optionalTools.$host.$tool.directory" "$pins")
    archive="$downloads/${url##*/}"
    mkdir -p "$dir"
    fetch "$url" "$sha" "$archive"
    case "$archive" in
        *.zip) unzip -q "$archive" -d "$dir" ;;
        *) tar -xf "$archive" -C "$dir" --strip-components=1 ;;
    esac
}

# Python wheel of clang-format or clang-tidy for Linux x64.
wheel_tool() {
    local tool=$1 url sha dir archive
    url=$(jq -er ".optionalTools.$tool.archives.\"linux-x64\".url" "$pins")
    sha=$(jq -er ".optionalTools.$tool.archives.\"linux-x64\".sha256" "$pins")
    dir=$(jq -er ".optionalTools.$tool.directory" "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$sha" "$archive"
    unzip -q "$archive" -d "$dir"
    chmod +x "$dir/$(jq -er ".optionalTools.$tool.binary" "$pins")"
    if [[ "$tool" == clangTidy ]]; then
        chmod +x "$dir/clang_tidy/data/bin/clang-apply-replacements"
    fi
}

# Source archives that the CMake configuration otherwise downloads, named as its offline cache expects.
native_sources() {
    local url sha suffix
    while IFS=$'\t' read -r url sha; do
        [[ "$url" =~ \.(tar\.gz|tar\.xz|zip)$ ]]
        suffix=${BASH_REMATCH[0]}
        fetch "$url" "$sha" "$downloads/sources/$sha$suffix"
    done < <(jq -er '.dependencies[] | [.url, .archive_sha256] | @tsv' src/app/dependencies.lock.json
        jq -er '.window_sdl3 | [.url, .sha256] | @tsv' src/app/platform/component.json
        jq -er '.windows_media_http2.curl, .windows_media_http2.nghttp2 | [.url, .sha256] | @tsv' src/app/api/component.json)
    while IFS=$'\t' read -r url sha; do
        fetch "$url" "$sha" "$downloads/sources/$sha.blob"
    done < <(jq -er '.headers[] | [.url, .sha256] | @tsv' src/guest/qnx/component.json)
}

# Formatters, Checkstyle, Error Prone, Java libraries, D8, apksigner and android.jar of the lint job.
lint_tools() {
    local url sha dir file archive library
    wheel_tool clangFormat
    wheel_tool clangTidy
    url=$(jq -er '.optionalTools.googleJavaFormat.binaries."linux-x64".url' "$pins")
    sha=$(jq -er '.optionalTools.googleJavaFormat.binaries."linux-x64".sha256' "$pins")
    dir=$(jq -er .optionalTools.googleJavaFormat.directory "$pins")
    file="$dir/$(jq -er .optionalTools.googleJavaFormat.binary "$pins")"
    mkdir -p "$dir"
    fetch "$url" "$sha" "$file"
    chmod +x "$file"
    dir=$(jq -er .optionalTools.checkstyle.directory "$pins")
    mkdir -p "$dir"
    fetch "$(jq -er .optionalTools.checkstyle.url "$pins")" "$(jq -er .optionalTools.checkstyle.sha256 "$pins")" \
        "$dir/$(jq -er .optionalTools.checkstyle.jar "$pins")"
    dir=$(jq -er .optionalTools.errorProne.directory "$pins")
    mkdir -p "$dir"
    for jar in core dataflow; do
        fetch "$(jq -er ".optionalTools.errorProne.jars.$jar.url" "$pins")" \
            "$(jq -er ".optionalTools.errorProne.jars.$jar.sha256" "$pins")" \
            "$dir/$(jq -er ".optionalTools.errorProne.jars.$jar.jar" "$pins")"
    done
    mkdir -p .local/toolchains/java/libraries
    jq -c '.[]' src/prepare/libraries.json | while read -r library; do
        fetch "$(jq -er .url <<< "$library")" "$(jq -er .sha256 <<< "$library")" \
            ".local/toolchains/java/libraries/$(jq -er .filename <<< "$library")"
    done
    url=$(jq -er .sdk.archives.linux.buildTools.url "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$(jq -er .sdk.archives.linux.buildTools.sha256 "$pins")" "$archive"
    # Error Prone verifies both JARs against src/prepare/toolchain.json.
    dir=".local/toolchains/java-sdk/build-tools/$(jq -er .sdk.buildTools "$pins")/lib"
    mkdir -p "$dir"
    unzip -j -q "$archive" '*/lib/d8.jar' '*/lib/apksigner.jar' -d "$dir"
    # Guest units compile against the android.jar of the pinned platform archive.
    url=$(jq -er .sdk.archives.linux.platform.url "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$(jq -er .sdk.archives.linux.platform.sha256 "$pins")" "$archive"
    dir=".local/toolchains/java-sdk/platforms/android-$(jq -er .sdk.compileSdk "$pins")"
    mkdir -p "$dir"
    unzip -j -q "$archive" '*/android.jar' -d "$dir"
}

# Kernel inputs: the host clang and GNU make of the NDK, the kernel source archive and, for API 30,
# the stock kernel and ramdisk; kernel-tool.sh checks the source tree and the stock images by their pins.
kernel_inputs() {
    local kernel=$1 url archive ndk recipe definition candidate
    url=$(jq -er .sdk.archives.linux.ndk.url "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$(jq -er .sdk.archives.linux.ndk.sha256 "$pins")" "$archive"
    ndk=".local/toolchains/native-sdk/ndk/$(jq -er .native.ndk "$pins")"
    mkdir -p "$(dirname "$ndk")" .local/build/tmp/ndk
    unzip -q "$archive" '*/source.properties' '*/toolchains/llvm/prebuilt/linux-x86_64/*' '*/prebuilt/linux-x86_64/*' -d .local/build/tmp/ndk
    mv .local/build/tmp/ndk/* "$ndk"
    rm "$archive"
    recipe="profiles/common-$kernel/components/kernel/component.json"
    curl -fsSL --retry 3 -o "$(jq -er "(.source // .inputs).archive_file" "$recipe")" "$(jq -er "(.source // .inputs).url" "$recipe")"
    if [[ "$kernel" == qinggan-11 ]]; then
        # Any qinggan11 profile with this kernel names the API 30 image of its stock kernel and ramdisk.
        definition=
        for candidate in profiles/*/profile.json; do
            if [[ "$(jq -r '.platform' "$candidate")" == qinggan11 ]] && jq -e '.android.development_kernel' "$candidate" > /dev/null; then
                definition=$candidate
                break
            fi
        done
        [[ -n "$definition" ]] || { echo "No qinggan11 profile pins a development kernel" >&2; exit 1; }
        url=$(jq -er .android.url "$definition")
        archive="$downloads/${url##*/}"
        fetch "$url" "$(jq -er .android.sha256 "$definition")" "$archive"
        mkdir -p .local/build/kernel/base
        unzip -j -q "$archive" '*/kernel-ranchu' '*/ramdisk.img' -d .local/build/kernel/base
        rm "$archive"
    fi
}

add_path() {
    echo "$PWD/$1" >> "$GITHUB_PATH"
}

# Public source-fixture prerequisites only: never OEM fetch, emulator or QEMU installation.
# Existing lint/kernel installers own the same archive pins and canonical destinations.
fixture_tools() {
    local host=$1 url archive dir library
    # SigningIntermediateTest creates an own APK and uses the public AOSP test key/certificate.
    # The repository pins decoded bytes, matching DownloadEncoding::base64 in production.
    mkdir -p "$downloads/native"
    local role sha
    for role in key cert; do
        url=$(jq -er ".signing.platform.$role.url" "$pins")
        sha=$(jq -er ".signing.platform.$role.sha256" "$pins")
        curl -fsSL --retry 3 "$url" | base64 -d > "$downloads/native/$sha.blob"
        check "$sha" "$downloads/native/$sha.blob"
    done
    mkdir -p .local/toolchains/java/libraries
    jq -c '.[]' src/prepare/libraries.json | while read -r library; do
        fetch "$(jq -er .url <<< "$library")" "$(jq -er .sha256 <<< "$library")" \
            ".local/toolchains/java/libraries/$(jq -er .filename <<< "$library")"
    done
    url=$(jq -er ".sdk.archives.$host.buildTools.url" "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$(jq -er ".sdk.archives.$host.buildTools.sha256" "$pins")" "$archive"
    dir=".local/toolchains/java-sdk/build-tools/$(jq -er .sdk.buildTools "$pins")"
    mkdir -p "$dir"
    local stage=.local/build/tmp/ci-fixture-build-tools
    mkdir -p "$stage"
    unzip -o -q "$archive" -d "$stage"
    mv "$stage"/*/* "$dir/"
    rmdir "$stage"/* "$stage"
    url=$(jq -er ".sdk.archives.$host.platform.url" "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$(jq -er ".sdk.archives.$host.platform.sha256" "$pins")" "$archive"
    dir=".local/toolchains/java-sdk/platforms/android-$(jq -er .sdk.compileSdk "$pins")"
    mkdir -p "$dir"
    unzip -j -o -q "$archive" '*/android.jar' -d "$dir"
    url=$(jq -er ".sdk.archives.$host.ndk.url" "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$(jq -er ".sdk.archives.$host.ndk.sha256" "$pins")" "$archive"
    dir=".local/toolchains/native-sdk/ndk/$(jq -er .native.ndk "$pins")"
    mkdir -p "$dir"
    # Preserve NDK layout, sysroot and executable permissions from the pinned archive.
    stage=.local/build/tmp/ci-fixture-ndk
    mkdir -p "$stage"
    unzip -o -q "$archive" -d "$stage"
    mv "$stage"/android-ndk-*/* "$dir/"
    rmdir "$stage"/android-ndk-* "$stage"
    url=$(jq -er ".sdk.archives.$host.cmake.url" "$pins")
    archive="$downloads/${url##*/}"
    fetch "$url" "$(jq -er ".sdk.archives.$host.cmake.sha256" "$pins")" "$archive"
    dir=".local/toolchains/native-sdk/cmake/$(jq -er .native.cmake "$pins")"
    mkdir -p "$dir"
    unzip -o -q "$archive" -d "$dir"
}

case "$set_name" in
    lint | linux | tidy-windows | windows-cross | uncrustify)
        if $env_only; then
            add_path "$(jq -er .optionalTools.linuxBuild.cmake.directory "$pins")/bin"
            add_path "$(jq -er .optionalTools.linuxBuild.ninja.directory "$pins")"
            if [[ "$set_name" != uncrustify ]]; then
                jdk="$(jq -er .optionalTools.linuxBuild.jdk.directory "$pins")"
                echo "JAVA_HOME=$PWD/$jdk" >> "$GITHUB_ENV"
                add_path "$jdk/bin"
            fi
            exit 0
        fi
        for tool in zig cmake ninja; do
            host_tool linuxBuild "$tool"
        done
        case "$set_name" in
            lint)
                host_tool linuxBuild jdk
                lint_tools
                # The prebuilt Uncrustify of the uncrustify workflow; while its release is missing, the
                # source build with the pinned Zig, CMake and Ninja, checked against the same pin.
                PATH="$PWD/$(jq -er .optionalTools.linuxBuild.cmake.directory "$pins")/bin:$PWD/$(jq -er .optionalTools.linuxBuild.ninja.directory "$pins"):$PATH" \
                    bash -c 'cmake -P src/dev/format/uncrustify.cmake || cmake -DVOYEMU_UNCRUSTIFY_FROM_SOURCE=ON -P src/dev/format/uncrustify.cmake'
                ;;
            linux | windows-cross) host_tool linuxBuild jdk ;;
            tidy-windows)
                host_tool linuxBuild jdk
                wheel_tool clangTidy
                ;;
            # The Uncrustify source tarball, named as src/dev/format/uncrustify.cmake expects; the build needs Python 3.
            uncrustify)
                fetch "$(jq -er .optionalTools.uncrustify.url "$pins")" "$(jq -er .optionalTools.uncrustify.sha256 "$pins")" \
                    "$downloads/sources/uncrustify-$(jq -er .optionalTools.uncrustify.version "$pins").tar.gz"
                ;;
        esac
        if [[ "$set_name" != uncrustify ]]; then
            native_sources
        fi
        ;;
    macos)
        jdk="$(jq -er .optionalTools.macosBuild.jdk.directory "$pins")/Contents/Home"
        if $env_only; then
            add_path "$(jq -er .optionalTools.macosBuild.cmake.directory "$pins")/CMake.app/Contents/bin"
            add_path "$(jq -er .optionalTools.macosBuild.ninja.directory "$pins")"
            # Java preparation tests read JAVA_HOME and PATH; the macOS archive wraps the JDK in Contents/Home.
            echo "JAVA_HOME=$PWD/$jdk" >> "$GITHUB_ENV"
            add_path "$jdk/bin"
            exit 0
        fi
        for tool in zig cmake ninja jdk; do
            host_tool macosBuild "$tool"
        done
        native_sources
        ;;
    kernel-qinggan-09 | kernel-qinggan-11)
        if $env_only; then
            echo "JAVA_HOME=$PWD/$(jq -er .optionalTools.linuxBuild.jdk.directory "$pins")" >> "$GITHUB_ENV"
            exit 0
        fi
        host_tool linuxBuild jdk
        host_tool linuxBuild zig
        kernel_inputs "${set_name#kernel-}"
        ;;
    *)
        echo "Unknown toolchain set: $set_name" >&2
        exit 2
        ;;
esac
if [[ "$fixture_inputs" == true ]] && ! $env_only; then
    case "$set_name" in
        linux) fixture_tools linux ;;
        macos) fixture_tools darwin ;;
        *) echo "No source-fixture inputs for set: $set_name" >&2; exit 2 ;;
    esac
fi
