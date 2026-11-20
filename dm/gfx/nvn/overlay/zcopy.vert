#version 450
// A texture copy into a depth / stencil image whose format cannot be a blit's destination (nvn_vk.c zcopy_draw): one triangle over the viewport, which is the copy's box
void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
