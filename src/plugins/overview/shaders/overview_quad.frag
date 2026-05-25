#version 450

// OverviewEffectV2 textured-quad fragment shader.
//
// Samples the shared VulkanThumbnailAtlas at the UV computed in the
// vertex shader, applies a per-tile opacity (driven by the slide-in
// animation). Output is premultiplied alpha to match the compositor
// blending mode (see project_vulkan_premultiplied_alpha memory).
//
// Build: glslc -O overview_quad.frag -o overview_quad.frag.spv

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragOpacity;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 c = texture(atlas, fragUv);
    // Atlas is SRGB-sampled (hardware decodes to linear on read). The
    // compositor expects premultiplied colour; modulate by opacity here.
    outColor = vec4(c.rgb * fragOpacity, c.a * fragOpacity);
}
