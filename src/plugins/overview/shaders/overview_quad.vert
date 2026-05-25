#version 450

// OverviewEffectV2 textured-quad vertex shader.
//
// Emits a quad (4 verts, triangle strip) covering a rect in NDC. The
// quad's screen-space rect and the atlas UV sub-rect both come from
// push constants — one draw call per window tile.
//
// Build: glslc -O overview_quad.vert -o overview_quad.vert.spv
// Embedded into the C++ effect as a static uint32_t[] (see
// contrast.cpp:74 for the same pattern).

layout(push_constant) uniform PC {
    // NDC-space rect [-1, 1] for the quad: xy = top-left, zw = size.
    vec4 quadRectNdc;
    // Normalised UV sub-rect within the atlas: xy = top-left, zw = size.
    vec4 atlasSlotUv;
    // Tint colour and weight. tintRgba.a controls the mix between the
    // sampled atlas pixel (.a=0) and the solid tint (.a=1) — lets the
    // post-pass draw both atlas-backed window tiles AND solid-colour
    // tiles (desktop bar) from one pipeline. Read in the fragment
    // stage; declared here so the push-constant range covers both
    // stages.
    vec4 tintRgba;
    // Per-frame uniform alpha for the slide-in animation.
    float opacity;
} pc;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out float fragOpacity;

void main()
{
    // gl_VertexIndex picks one of four quad corners. The mapping below
    // matches a triangle-strip emit:
    //   index 0 → (0, 0)  top-left
    //   index 1 → (1, 0)  top-right
    //   index 2 → (0, 1)  bottom-left
    //   index 3 → (1, 1)  bottom-right
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = vec4(pc.quadRectNdc.xy + corner * pc.quadRectNdc.zw, 0.0, 1.0);
    fragUv = pc.atlasSlotUv.xy + corner * pc.atlasSlotUv.zw;
    fragOpacity = pc.opacity;
}
