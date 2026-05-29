#version 450

// Full-screen triangle for the wallpaper dual-kawase blur passes.
// Generates 3 vertices from gl_VertexIndex without a vertex buffer.
// The blur passes use a standard positive-height viewport (unlike the
// main overview pipeline which uses NDC-Y-up), so the UV does not need
// a Y-flip here — both the downsample and upsample read and write in
// top-down orientation, and the final blurred texture is sampled by
// the main overview pipeline with regular [0..1] UVs.

layout(location = 0) out vec2 vUv;

void main()
{
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
