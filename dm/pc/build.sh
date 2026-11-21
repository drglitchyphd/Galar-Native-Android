#!/bin/bash
# Builds the PC replay layer and the golden-frame tools (Makefile) on Linux / WSL: finds the Vulkan headers (the system's, else the NDK's, as gfx/tests/run_fgen_vk.sh)
# and glslc (PATH, else the NDK's shader-tools).   bash build.sh [GAME=<engine title ID>] [make targets...]
# Engines: 01008DB008C2C000 (Pokémon Shield's build, which also runs Pokémon Sword 0100ABF008968000), the default; others as their engines join this branch.
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$(ls -d "$HOME"/psa-build/android-ndk-r* "$HOME"/android-ndk/android-ndk-r* "$HOME"/nxnative-work/android-sdk/ndk/* 2>/dev/null | sort | tail -1)}
INC=${VULKAN_INC:-}
if [ -z "$INC" ] && [ ! -f /usr/include/vulkan/vulkan.h ]; then
    S=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
    [ -f "$S/vulkan/vulkan.h" ] || { echo "no Vulkan headers: install libvulkan-dev or set VULKAN_INC / NDK" >&2; exit 1; }
    INC=$PWD/out/vkinc; mkdir -p "$INC"; ln -sfn "$S/vulkan" "$INC/vulkan"; ln -sfn "$S/vk_video" "$INC/vk_video"
fi
GLSLC=${GLSLC:-$(command -v glslc || echo "$NDK/shader-tools/linux-x86_64/glslc")}
exec make -j"$(nproc)" VULKAN_INC="$INC" GLSLC="$GLSLC" "$@"
