#!/bin/bash
# The SPIR-V side of DM_VK_B10_AS_RGBA16's stand-in (nvn_spv.c): spv_b10_outputs (fragment outputs into the stand-in made what R11G11B10F stores) and
#   spv_b10_image_writes + spv_image_format_swap (storage images declared R11fG11fB10f) on modules from glslc and by hand: what is patched and what is left alone,
#   every result valid SPIR-V (spirv-val, the module's own target); then t_b10_spv_vk runs the store rule on a Vulkan driver (lavapipe) over a million texels
#   against b10_conv.h. Needs the NDK's shader tools and headers:  NDK=<ndk> bash t_b10_spv.sh
set -u
NDK=${NDK:?set NDK to the Android NDK}; T=$NDK/shader-tools/linux-x86_64; D=$(mktemp -d -p "${TMPBASE:-/tmp}"); cd "$(dirname "$0")"
cc -O1 -Wall -Wextra -Wno-unused-parameter -I../nvn -o "$D/t" t_b10_spv.c ../nvn/nvn_spv.c || exit 1
checks=0; fails=0
check() { checks=$((checks + 1)); if eval "$1"; then echo "✓ $2"; else echo "✗ $2"; fails=$((fails + 1)); fi; }
glsl() { printf '%s\n' "$3" > "$D/$1.$2"; "$T/glslc" --target-env="${ENV:-vulkan1.0}" -o "$D/$1.spv" "$D/$1.$2" || { echo "✗ $1: does not compile"; fails=$((fails + 1)); }; }
asm() { printf '%s\n' "$2" > "$D/$1.spvasm"; "$T/spirv-as" --target-env vulkan1.0 "$D/$1.spvasm" -o "$D/$1.spv" || { echo "✗ $1: does not assemble"; fails=$((fails + 1)); }; }
pat() { "$D/t" "$1" "$D/$2.spv" "$D/$2.out.spv" ${3:-} > "$D/$2.txt"; echo $? > "$D/$2.rc"; }   # mode, name[, mask]
val() { "$T/spirv-val" --target-env "${2:-vulkan1.0}" "$D/$1.out.spv"; }
dis() { "$T/spirv-dis" --raw-id "$D/$1.out.spv" 2>/dev/null; }
rc() { cat "$D/$1.rc"; }

# ---- fragment outputs ----
glsl f_vec4 frag '#version 450
layout(location = 0) in vec3 c;
layout(location = 0) out vec4 o;
void main() { o = vec4(c * 70000.0 - 1.0, 0.5); }'
pat out f_vec4 1
check '[ "$(rc f_vec4)" = 0 ] && val f_vec4' "a vec4 output: patched, valid"
check '[ "$(dis f_vec4 | grep -c OpVectorShuffle)" = 1 ] && dis f_vec4 | grep -q "OpTypeBool" && dis f_vec4 | grep -q "OpTypeInt 32 0"' "its alpha kept (a shuffle), the uint and bool types it lacked added"
check '[ "$(dis f_vec4 | grep -c "OpConvertUToF")" = 1 ] && [ "$(dis f_vec4 | grep -c "OpShiftLeftLogical")" -ge 3 ]' "the rule written in integer operations, one conversion back"

glsl f_early frag '#version 450
layout(location = 0) in vec3 c;
layout(location = 0) out vec4 o;
void main() { if (c.x > 0.5) { o = vec4(-c, 1.0); return; } o = vec4(c, 1.0); }'
pat out f_early 1
check '[ "$(rc f_early)" = 0 ] && val f_early' "a shader with an early return: patched, valid"
check '[ "$(dis f_early | grep -c OpVectorShuffle)" = "$(dis f_early | grep -c OpReturn$)" ] && [ "$(dis f_early | grep -c OpReturn$)" -ge 2 ]' "every return of the entry point gets it ($(dis f_early | grep -c OpReturn$) returns)"

glsl f_two frag '#version 450
layout(location = 0) in vec3 c;
layout(location = 0) out vec4 a;
layout(location = 1) out vec4 b;
void main() { a = vec4(c, 1.0); b = vec4(-c, 1.0); }'
pat out f_two 2
check '[ "$(rc f_two)" = 0 ] && val f_two && [ "$(dis f_two | grep -c OpVectorShuffle)" = 1 ]' "two outputs, mask 2: only location 1's"
pat out f_two 4
check '[ "$(rc f_two)" = 2 ]' "a mask with no output there: nothing to do"

glsl f_back frag '#version 450
layout(location = 0) in vec3 c;
layout(location = 0) out vec4 o;
void main() { o = vec4(c, 1.0); o.x += o.y * 3.0; }'
pat out f_back 1
check '[ "$(rc f_back)" = 0 ] && val f_back' "an output the shader reads back: patched (at the return), valid"

glsl f_v3 frag '#version 450
layout(location = 0) in vec3 c;
layout(location = 0) out vec3 o;
layout(location = 1) out float s;
void main() { o = c - 0.5; s = c.x - 1.0; }'
pat out f_v3 3
check '[ "$(rc f_v3)" = 0 ] && val f_v3 && [ "$(dis f_v3 | grep -c OpVectorShuffle)" = 0 ] && [ "$(dis f_v3 | grep -c OpConvertUToF)" = 2 ]' "a vec3 and a float output: both patched (no alpha to keep), valid"

glsl f_dual frag '#version 450
layout(location = 0) in vec3 c;
layout(location = 0, index = 0) out vec4 a;
layout(location = 0, index = 1) out vec4 b;
void main() { a = vec4(c, 1.0); b = vec4(c, 0.5); }'
pat out f_dual 1
check '[ "$(rc f_dual)" = 0 ] && val f_dual && [ "$(dis f_dual | grep -c OpVectorShuffle)" = 1 ]' "dual-source outputs: the colour (index 0) patched, the blend factor (index 1) left alone"

ENV=vulkan1.2 glsl f_v12 frag '#version 450
layout(location = 0) in vec3 c;
layout(location = 0) out vec4 o;
void main() { o = vec4(c, 1.0); }'
pat out f_v12 1
check '[ "$(rc f_v12)" = 0 ] && val f_v12 vulkan1.2' "a SPIR-V 1.5 module (Vulkan 1.2 target): patched, valid"

asm f_comp '; SPIR-V
; Version: 1.0
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %in %out
OpExecutionMode %main OriginUpperLeft
OpDecorate %in Location 0
OpDecorate %out Location 0
%void = OpTypeVoid
%fnty = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%int = OpTypeInt 32 1
%bool = OpTypeBool
%pin = OpTypePointer Input %v4
%pout = OpTypePointer Output %v4
%pof = OpTypePointer Output %float
%in = OpVariable %pin Input
%out = OpVariable %pout Output
%i0 = OpConstant %int 0
%i1 = OpConstant %int 1
%i2 = OpConstant %int 2
%i3 = OpConstant %int 3
%main = OpFunction %void None %fnty
%l = OpLabel
%v = OpLoad %v4 %in
%x = OpCompositeExtract %float %v 0
%y = OpCompositeExtract %float %v 1
%z = OpCompositeExtract %float %v 2
%w = OpCompositeExtract %float %v 3
%p0 = OpAccessChain %pof %out %i0
OpStore %p0 %x
%p1 = OpAccessChain %pof %out %i1
OpStore %p1 %y
%p2 = OpAccessChain %pof %out %i2
OpStore %p2 %z
%p3 = OpAccessChain %pof %out %i3
OpStore %p3 %w
OpReturn
OpFunctionEnd'
pat out f_comp 1
check '[ "$(rc f_comp)" = 0 ] && val f_comp && [ "$(dis f_comp | grep -c "OpTypeBool")" = 1 ]' "component stores through access chains (the translator's way): patched, valid, its own bool type reused"

glsl v_vert vert '#version 450
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0); gl_Position = vec4(0.0); }'
pat out v_vert 1
check '[ "$(rc v_vert)" = 2 ]' "a vertex shader: left alone"

# ---- storage image writes ----
glsl c_img comp '#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, r11f_g11f_b10f) uniform writeonly image2D img;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D other;
void main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy); imageStore(img, p, vec4(-1.0, 70000.0, 0.3, 0.25)); imageStore(other, p, vec4(0.5)); }'
pat img c_img
check '[ "$(rc c_img)" = 0 ] && val c_img && grep -q "left 0" "$D/c_img.txt" && grep -q "swapped 1" "$D/c_img.txt"' "an R11G11B10F image store: patched, the declaration Rgba16f, valid"
check '[ "$(dis c_img | grep -c OpCompositeInsert)" = 1 ] && [ "$(dis c_img | grep -c OpConvertUToF)" = 1 ] && dis c_img | grep -q "Rgba16f" && dis c_img | grep -q "Rgba8"' "its alpha written 1, the RGBA8 image's store and declaration left alone"

glsl c_v3 comp '#version 450
layout(local_size_x = 8) in;
layout(set = 0, binding = 0, r11f_g11f_b10f) uniform image2D img;
void main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy); vec4 t = imageLoad(img, p); imageStore(img, p, t * 2.0 - 1.0); }'
pat img c_v3
check '[ "$(rc c_v3)" = 0 ] && val c_v3' "a read-modify-write of the image: patched, valid"

glsl c_buf comp '#version 450
layout(local_size_x = 8) in;
layout(set = 0, binding = 0, r11f_g11f_b10f) uniform writeonly imageBuffer tb;
void main() { imageStore(tb, int(gl_GlobalInvocationID.x), vec4(-1.0)); }'
pat img c_buf
check '[ "$(rc c_buf)" = 2 ] && grep -q "swapped 0" "$D/c_buf.txt"' "a texel buffer declared R11fG11fB10f: neither patched nor re-declared (it stays the packed format)"

glsl c_unk comp '#version 450
layout(local_size_x = 8) in;
layout(set = 0, binding = 0) uniform writeonly image2D img;
void main() { imageStore(img, ivec2(gl_GlobalInvocationID.xy), vec4(-1.0)); }'
pat img c_unk
check '[ "$(rc c_unk)" = 2 ]' "an image declared without a format: left alone (nvn_vk.c quantizes such a stand-in after the dispatch)"

# ---- the rule on a driver ----
if [ -z "${SKIP_VK:-}" ]; then
  glsl c_store comp '#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, r11f_g11f_b10f) uniform writeonly image2D img;
layout(set = 0, binding = 1, std430) readonly buffer In { vec4 v[]; } inb;
layout(push_constant) uniform P { uint w; } p;
void main() { uvec2 g = gl_GlobalInvocationID.xy; imageStore(img, ivec2(g), inb.v[g.y * p.w + g.x]); }'
  mkdir -p "$D/inc"; ln -sfn "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/vulkan" "$D/inc/vulkan"; ln -sfn "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/vk_video" "$D/inc/vk_video"
  cc -O2 -Wall -I"$D/inc" -I../nvn -o "$D/tvk" t_b10_spv_vk.c ../nvn/nvn_spv.c -l:libvulkan.so.1 -lm || exit 1
  ICD=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}
  VK_ICD_FILENAMES=$ICD "$D/tvk" "$D/c_store.spv" | tee "$D/vk.txt"
  check 'grep -q "^PASS t_b10_spv_vk" "$D/vk.txt"' "the store rule on the driver: every texel as R11G11B10F stores it"
fi
echo "$checks checks, $fails failed"; rm -rf "$D"; [ $fails = 0 ]
