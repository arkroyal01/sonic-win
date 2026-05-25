#version 450

// OverviewEffectV2 textured-quad fragment shader.
//
// Samples the shared VulkanThumbnailAtlas at the UV computed in the
// vertex shader, applies a per-tile opacity (driven by the slide-in
// animation), and optionally mixes in a solid tint colour so the
// post-pass can draw both atlas-backed window tiles and solid-colour
// bar tiles from one pipeline. Output is premultiplied alpha to match
// the compositor blending mode (see project_vulkan_premultiplied_alpha
// memory).
//
// Build: glslc -O overview_quad.frag -o overview_quad.frag.spv

layout(push_constant) uniform PC {
    vec4 quadRectNdc;
    vec4 atlasSlotUv;
    // .a == 0 → pure atlas sample; .a == 1 → solid tint (RGB used).
    // Intermediate values blend the two so future hover/highlight
    // states can tint a thumbnail without a second pipeline.
    vec4 tintRgba;
    float opacity;
} pc;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragOpacity;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 sampled = texture(atlas, fragUv);
    // Atlas is SRGB-sampled (hardware decodes to linear on read).
    vec3 rgb = mix(sampled.rgb, pc.tintRgba.rgb, pc.tintRgba.a);
    float a = mix(sampled.a, 1.0, pc.tintRgba.a);
    // Premultiplied output, modulated by opacity.
    outColor = vec4(rgb * fragOpacity, a * fragOpacity);
}
