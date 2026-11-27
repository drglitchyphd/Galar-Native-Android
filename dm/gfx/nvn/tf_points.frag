#version 450
layout(location = 28) flat in uvec4 i0; layout(location = 0) out uvec4 o0;
layout(location = 29) flat in uvec4 i1; layout(location = 1) out uvec4 o1;
layout(location = 30) flat in uvec4 i2; layout(location = 2) out uvec4 o2;
layout(location = 31) flat in uvec4 i3; layout(location = 3) out uvec4 o3;
void main() { o0 = i0; o1 = i1; o2 = i2; o3 = i3; }
