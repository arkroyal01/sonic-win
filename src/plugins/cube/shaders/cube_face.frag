#version 450

// CubeEffectV2 face fragment shader.
//
// Samples the per-face VulkanThumbnailAtlas slot at the UV computed
// in the vertex shader, then multiplies by the slide-in opacity.
// Output is premultiplied alpha to match the compositor's blend mode
// ([[project_vulkan_premultiplied_alpha]]).
//
// Build: glslc -O --target-env=vulkan1.2 cube_face.frag
//              -o cube_face.frag.spv

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 atlasSlotUv;
    float opacity;
} pc;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragOpacity;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 sampled = texture(atlas, fragUv);
    // Atlas is sampled through the SRGB view; hardware decodes to
    // linear on read, so `sampled` is linear premultiplied. Modulate
    // by the slide-in opacity and re-output as premultiplied.
    outColor = vec4(sampled.rgb * fragOpacity,
                    sampled.a * fragOpacity);
}
