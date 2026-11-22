#!/bin/bash
# Builds t_vkdev (the engine's nvn_vk.c compiled as it is, with its own SPIR-V / texture code) for this PC: Linux x86-64 (WSL is fine), gcc, zlib, python3.
#   bash build_vkdev.sh [out dir, default /tmp/simdev-build]     ->  <out>/t_vkdev
# The Vulkan headers are the repository's own (dm/third_party/vulkan-headers, the version the engine is built with); a vulkan.h is made beside them (the
# repository keeps only vulkan_core.h: Android's comes from the NDK). GAME picks the game profile header (default Sword, 01008DB008C2C000).
set -euo pipefail
cd "$(dirname "$0")"
OUT=${1:-/tmp/simdev-build}; GAME=${GAME:-01008DB008C2C000}
DM=$(cd ../../.. && pwd)
mkdir -p "$OUT/gen" "$OUT/inc/vulkan" "$OUT/obj"
V="$DM/third_party/vulkan-headers/include/vulkan"
printf '#include "%s/vk_platform.h"\n#include "%s/vulkan_core.h"\n' "$V" "$V" > "$OUT/inc/vulkan/vulkan.h"
python3 "$DM/games/game.py" header "$GAME" "$OUT/gen/game_profile.h"
INC="-I$OUT/inc -I$DM/host -I$DM/gfx/nvn -I$DM/gfx/nvn/include -I$DM/third_party/vulkan-headers/include -I$OUT/gen"
for d in $(python3 "$DM/games/game.py" dirs "$GAME"); do INC="$INC -I$d"; done
CF="-D_GNU_SOURCE -include $PWD/vk_linux_shim.h -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format -Wno-misleading-indentation -Wno-unused-label -Wno-pointer-sign -Wno-sign-compare -Wno-parentheses -Wno-unused-value -Wno-address -Wno-stringop-truncation -Wno-format-truncation -Wno-array-bounds -Wno-maybe-uninitialized -Wno-unused-result -Wno-dangling-pointer -Wno-int-in-bool-context -Wno-stringop-overflow"
SRC="$DM/gfx/nvn/nvn_spv.c $DM/gfx/nvn/golden_rec.c $DM/gfx/nvn/nvn_tex.c $DM/gfx/nvn/framecap_zip.c $DM/gfx/tex/bc_decode.c $DM/gfx/tex/bc_gpu.c $DM/gfx/tex/etc_encode.c $DM/gfx/tex/nvn_deswizzle.c $DM/gfx/tex/nvn_fmt_vk.c $DM/gfx/tex/tex_cache.c"
OBJ=
for s in $SRC; do o="$OUT/obj/$(basename "$s" .c).o"; gcc $CF $INC -c "$s" -o "$o" 2>"$o.log" || { cat "$o.log"; exit 1; }; OBJ="$OBJ $o"; done
gcc $CF $INC -no-pie -rdynamic -o "$OUT/t_vkdev" t_vkdev.c $OBJ -lz -lm -ldl -lpthread   # (no PIE: a crash's addresses go straight to addr2line) 2>"$OUT/t_vkdev.log" || { cat "$OUT/t_vkdev.log"; exit 1; }
grep -E "warning|error" "$OUT/t_vkdev.log" | grep "t_vkdev.c" || true
echo "$OUT/t_vkdev"
