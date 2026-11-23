#!/bin/bash
# The compute shaders of frame generation → SPIR-V headers (glslc --target-env=vulkan1.0 -O; the NDK's shader-tools/<host>/glslc, or GLSLC=...):
#   ls_hud.comp → ls_hud_spv.h (HUD-free Lossless Scaling, nvn_lsfg.h), fg_cut.comp + fg_cutsel.comp + fg_cutsel_rt.vert / .frag → fgcut_spv.h (camera cuts and
#   still frames, nvn_fgcut.h); and mali_white.comp → mali_white_spv.h (the Mali white-screen detector, nvn_vk.c mali_white_vk.inc)
set -e
O=$(cd "$(dirname "$0")" && pwd)
G=${GLSLC:-$(ls "${NDK:-$LOCALAPPDATA/Android/Sdk/ndk/27.2.12479018}"/shader-tools/*/glslc* 2>/dev/null | head -1)}
PY=${PYTHON:-$(command -v python3 || command -v py)}
W=$(mktemp -d)
for f in ls_hud fg_cut fg_cutsel mali_white; do "$G" --target-env=vulkan1.0 -O "$O/$f.comp" -o "$W/$f.spv"; done
for f in rgba8 rgb10_a2 rgba16f; do "$G" --target-env=vulkan1.0 -O -fshader-stage=compute -DOFMT=$f "$O/ls_hud.comp" -o "$W/ls_hud_$f.spv"; done   # (one module per output format: nvn_lsfg.h ls_hud_pipe)
for f in fg_cutsel_rt.vert fg_cutsel_rt.frag; do "$G" --target-env=vulkan1.0 -O "$O/$f" -o "$W/$f.spv"; done
"$PY" - "$W" "$O" <<'EOF'
import struct, sys
w, o = sys.argv[1], sys.argv[2]
NL = chr(10)
def arr(name, f):
    b = open(w + '/' + f, 'rb').read(); v = struct.unpack('<%dI' % (len(b) // 4), b)
    return 'static const uint32_t %s[] = {%s};' % (name, ','.join(hex(x) for x in v)) + NL
open(o + '/ls_hud_spv.h', 'w', newline=NL).write('// Generated from ls_hud.comp (glslc --target-env=vulkan1.0 -O; build_comp_spv.sh): HUD-free Lossless Scaling (nvn_lsfg.h). One module without a format, one per output format (-DOFMT)' + NL
    + arr('ls_hud_comp_spv', 'ls_hud.spv') + ''.join(arr('ls_hud_%s_comp_spv' % f, 'ls_hud_%s.spv' % f) for f in ('rgba8', 'rgb10_a2', 'rgba16f')))
open(o + '/fgcut_spv.h', 'w', newline=NL).write('// Generated from fg_cut.comp, fg_cutsel.comp and fg_cutsel_rt.vert / .frag (glslc --target-env=vulkan1.0 -O): camera cuts and still frames (nvn_fgcut.h)' + NL
    + arr('fg_cut_comp_spv', 'fg_cut.spv') + arr('fg_cutsel_comp_spv', 'fg_cutsel.spv') + arr('fg_cutsel_rt_vert_spv', 'fg_cutsel_rt.vert.spv') + arr('fg_cutsel_rt_frag_spv', 'fg_cutsel_rt.frag.spv'))
open(o + '/mali_white_spv.h', 'w', newline=NL).write('// Generated from mali_white.comp (glslc --target-env=vulkan1.0 -O; build_comp_spv.sh): the Mali white-screen detector (nvn_vk.c mali_white_vk.inc)' + NL
    + arr('mali_white_comp_spv', 'mali_white.spv'))
print('ls_hud_spv.h fgcut_spv.h mali_white_spv.h')
EOF
rm -rf "$W"
