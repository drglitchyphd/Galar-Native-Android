#!/bin/bash
# Builds the Khronos Vulkan Profiles layer (VK_LAYER_KHRONOS_profiles) and the Khronos validation layer (VK_LAYER_KHRONOS_validation) from source, pinned to one SDK
# tag, into $PREFIX (default ~/vkprofiles): sources in $PREFIX/src, the layers' manifests in $PREFIX/share/vulkan/explicit_layer.d. Nothing goes into this repository.
# Needs git, a C++ compiler, python3, cmake (CMAKE=<path>, default ~/cmake-venv/bin/cmake or the one on PATH) and network access (the layers fetch their own
# dependencies through scripts/update_deps.py, pinned by each repository's known_good.json). Run again: up-to-date builds are skipped.
#   bash setup_layers.sh [Vulkan-Profiles | Vulkan-ValidationLayers]   (one of the two, or both; about 10-20 minutes on 20+ cores, most of it the validation layer)
set -euo pipefail
TAG=${VK_LAYERS_TAG:-vulkan-sdk-1.4.363.0}
PREFIX=${PREFIX:-$HOME/vkprofiles}
CMAKE=${CMAKE:-$( [ -x "$HOME/cmake-venv/bin/cmake" ] && echo "$HOME/cmake-venv/bin/cmake" || command -v cmake )}
[ -n "$CMAKE" ] || { echo "no cmake (CMAKE=<path>)"; exit 1; }
export PATH="$(dirname "$CMAKE"):$PATH"
GEN=; command -v ninja >/dev/null && GEN="-G Ninja"
JOBS=${JOBS:-$(nproc)}
mkdir -p "$PREFIX/src"
fetch() {   # repo dir: a shallow clone of exactly $TAG
    local repo=$1 dir="$PREFIX/src/$1"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" describe --tags --exact-match 2>/dev/null)" = "$TAG" ]; then return; fi
    rm -rf "$dir"; git clone -q --depth 1 --branch "$TAG" "https://github.com/KhronosGroup/$repo.git" "$dir"
}
build() {   # repo, the target to build (ALL: everything), extra cmake options...
    local repo=$1 target=$2; shift 2; local dir="$PREFIX/src/$repo" stamp="$PREFIX/src/$repo.installed"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$TAG" ]; then echo "$repo $TAG: installed"; return; fi
    fetch "$repo"
    echo "$repo $TAG: building"
    "$CMAKE" -S "$dir" -B "$dir/build" $GEN -D CMAKE_BUILD_TYPE=Release -D UPDATE_DEPS=ON -D CMAKE_INSTALL_PREFIX="$PREFIX" "$@" >"$dir/configure.log" 2>&1 \
        || { tail -40 "$dir/configure.log"; exit 1; }
    "$CMAKE" --build "$dir/build" -j "$JOBS" $( [ "$target" = ALL ] || echo --target "$target" ) >"$dir/build.log" 2>&1 || { tail -60 "$dir/build.log"; exit 1; }
    "$CMAKE" --install "$dir/build" >"$dir/install.log" 2>&1 || { tail -20 "$dir/install.log"; exit 1; }
    echo "$TAG" >"$stamp"
}
# (the profiles repository builds its own profile graphs and generated tests by default: slow, and not needed for the layer, so only the layer is built)
NOWSI="-D BUILD_WSI_XCB_SUPPORT=OFF -D BUILD_WSI_XLIB_SUPPORT=OFF -D BUILD_WSI_WAYLAND_SUPPORT=OFF"
[ -z "${1:-}" ] || [ "$1" = Vulkan-Profiles ] && build Vulkan-Profiles ProfilesLayer -D BUILD_TESTS=OFF -D PROFILES_BUILD_TESTS=OFF $NOWSI
[ -z "${1:-}" ] || [ "$1" = Vulkan-ValidationLayers ] && build Vulkan-ValidationLayers ALL -D BUILD_TESTS=OFF -D BUILD_WERROR=OFF $NOWSI
ls "$PREFIX/share/vulkan/explicit_layer.d" 2>/dev/null || true
echo "layers in $PREFIX (tag $TAG): export VK_ADD_LAYER_PATH=$PREFIX/share/vulkan/explicit_layer.d"
