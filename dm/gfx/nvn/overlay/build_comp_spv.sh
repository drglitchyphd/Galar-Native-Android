#!/bin/bash
# The compute shaders of frame generation → SPIR-V headers (glslc --target-env=vulkan1.0 -O; the NDK's shader-tools/<host>/glslc, or GLSLC=...):
#   ls_hud.comp → ls_hud_spv.h (HUD-free Lossless Scaling, nvn_lsfg.h), fg_cut.comp + fg_cutsel.comp → fgcut_spv.h (camera cuts and still frames, nvn_fgcut.h)
set -e
O=$(cd "$(dirname "$0")" && pwd)
G=${GLSLC:-$(ls "${NDK:-$LOCALAPPDATA/Android/Sdk/ndk/27.2.12479018}"/shader-tools/*/glslc* 2>/dev/null | head -1)}
PY=${PYTHON:-$(command -v python3 || command -v py)}
W=$(mktemp -d)
for f in ls_hud fg_cut fg_cutsel; do "$G" --target-env=vulkan1.0 -O "$O/$f.comp" -o "$W/$f.spv"; done
"$PY" - "$W" "$O" <<'EOF'
import struct, sys
w, o = sys.argv[1], sys.argv[2]
def arr(name, f):
    b = open(w + '/' + f, 'rb').read(); v = struct.unpack('<%dI' % (len(b) // 4), b)
    return 'static const uint32_t %s[] = {%s};\n' % (name, ','.join(hex(x) for x in v))
open(o + '/ls_hud_spv.h', 'w', newline='\n').write('// Generated from ls_hud.comp (glslc --target-env=vulkan1.0 -O): HUD-free Lossless Scaling (nvn_lsfg.h)\n' + arr('ls_hud_comp_spv', 'ls_hud.spv'))
open(o + '/fgcut_spv.h', 'w', newline='\n').write('// Generated from fg_cut.comp and fg_cutsel.comp (glslc --target-env=vulkan1.0 -O): camera cuts and still frames (nvn_fgcut.h)\n'
    + arr('fg_cut_comp_spv', 'fg_cut.spv') + arr('fg_cutsel_comp_spv', 'fg_cutsel.spv'))
print('ls_hud_spv.h fgcut_spv.h')
EOF
rm -rf "$W"
