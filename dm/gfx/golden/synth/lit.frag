#version 450
layout(set = 0, binding = 0, std140) uniform FB { vec4 light_dir; vec4 light_col; vec4 ambient; } fp_c1;
layout(set = 0, binding = 1) uniform sampler2D fp_t0;
layout(location = 0) in vec3 vn;
layout(location = 1) in vec2 vuv;
layout(location = 2) in vec4 vtint;
layout(location = 0) out vec4 o;
void main() {
    float d = max(dot(normalize(vn), -normalize(fp_c1.light_dir.xyz)), 0.0);
    o = vec4(texture(fp_t0, vuv).rgb * (fp_c1.ambient.rgb + d * fp_c1.light_col.rgb) * vtint.rgb, 1.0);
}
