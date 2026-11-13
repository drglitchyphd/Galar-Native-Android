#!/bin/bash
# FORK: builds the fork's APK. It installs next to the main Galar Native app (its own package, name and icon) and keeps its game files, saves and
# settings in a folder on the phone's shared storage instead of Android/data. Everything else is scripts/build_apk_pc.sh (same needs, same ~/psa-build).
#
#   bash scripts/build_fork.sh                         → ~/psa-build/GalarFork.apk
#   FORK_DATA_DIR=GalarFork-test bash scripts/build_fork.sh   another data folder (a second test install, say)
#
# Settings (environment variables):
#   FORK_PACKAGE   Android package name             default com.nxnative.galarfork  (keep it the same between builds, or Android treats it as a new app)
#   FORK_NAME      app name under the icon          default "Galar Fork"
#   FORK_DATA_DIR  folder in /storage/emulated/0    default GalarFork  (setup writes the ~15 GB game here; it survives uninstalling)
#   FORK_ICON      icon colours "top ground" (hex)  default "2a6fdb 182040"
#   FORK_ONLY_GAME the one game the icon opens      default 0100ABF008968000 (Pokémon Sword; empty = the game picker as upstream)
#   FORK_VERSION   versionCode                      default 37 (raise it for each build you install over an older one)
#   FORK_LSFG      1 (default) builds libforklsfg.so, frame generation with the player's own Lossless.dll (dm/fork/lsfg); 0 leaves it out
#
# Frame generation's sources are fetched once into $WORK/fork-lsfg at pinned commits (MIT / zlib; never copied into this repository):
#   FrankBarretta/lsfg-vk-android 3e89e54 (with its pe-parse and volk submodules), exyorha/dxbc fe8aefd, KhronosGroup/SPIRV-Headers vulkan-sdk-1.4.304.0
set -euo pipefail
FORK_PACKAGE=${FORK_PACKAGE:-com.nxnative.galarfork}
FORK_NAME=${FORK_NAME:-Galar Fork}
FORK_DATA_DIR=${FORK_DATA_DIR:-GalarFork}
FORK_ICON=${FORK_ICON:-2a6fdb 182040}
FORK_ONLY_GAME=${FORK_ONLY_GAME-0100ABF008968000}
FORK_VERSION=${FORK_VERSION:-44}
FORK_LSFG=${FORK_LSFG:-1}
fork_lsfg_build() {   # (build_apk_pc.sh calls this before packing the APK: PRE_APK_HOOK)
    rm -f "$LIBS/libforklsfg.so"
    [ "$FORK_LSFG" = 1 ] || { echo "[FORK] frame generation left out (FORK_LSFG=0)"; return 0; }
    step "[FORK] frame generation (libforklsfg.so)"
    local L="$WORK/fork-lsfg"; mkdir -p "$L"
    ( cd "$L"
      [ -d lsfg-vk-android/.git ] || { git clone -q https://github.com/FrankBarretta/lsfg-vk-android.git && git -C lsfg-vk-android -c advice.detachedHead=false checkout -q 3e89e54 &&
                                       git -C lsfg-vk-android submodule update --init -q thirdparty/pe-parse thirdparty/volk; }
      [ -d dxbc/.git ] || { git clone -q https://github.com/exyorha/dxbc && git -C dxbc -c advice.detachedHead=false checkout -q fe8aefd; }
      [ -d SPIRV-Headers/.git ] || git clone -q --depth 1 -b vulkan-sdk-1.4.304.0 https://github.com/KhronosGroup/SPIRV-Headers.git )
    cmake -G Ninja -S "$REPO/dm/fork/lsfg" -B "$L/build" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=$API \
          -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DLSFG_SRC="$L/lsfg-vk-android" -DDXBC_SRC="$L/dxbc" -DSPIRV_HEADERS="$L/SPIRV-Headers" >/dev/null
    ninja -C "$L/build" forklsfg >/dev/null
    "$TC/llvm-strip" -o "$LIBS/libforklsfg.so" "$L/build/libforklsfg.so"
    echo "libforklsfg.so: $(stat -c %s "$LIBS/libforklsfg.so") bytes"
}
PRE_APK_HOOK=fork_lsfg_build
APK_ARGS=(--package "$FORK_PACKAGE" --name "$FORK_NAME" --data-dir "$FORK_DATA_DIR" --icon "$FORK_ICON" --version "$FORK_VERSION" --only-game "$FORK_ONLY_GAME")
APK_OUT="${WORK:-$HOME/psa-build}/$(echo "$FORK_NAME" | tr -d ' ').apk"
echo "[FORK] package $FORK_PACKAGE, name \"$FORK_NAME\", data folder /storage/emulated/0/$FORK_DATA_DIR"
source "$(dirname "$0")/build_apk_pc.sh"
