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
    // Explicit mip LOD for textureLod sampling. 0 disables the LOD
    // path and uses the default texture() (implicit derivatives, so
    // LOD 0 for a fullscreen quad). The wallpaper backdrop uses a
    // higher LOD as a cheap mipmap-based blur — the atlas already
    // generates a mip cascade for every slot.
    float lod;
} pc;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragOpacity;

layout(location = 0) out vec4 outColor;

// Multi-scale blur: sample several mip levels at the same UV and
// blend their pre-filtered contributions. Each mip is already a
// box-filter average over progressively larger source regions
// (generateMipsAndPublish writes the full cascade), so the weighted
// sum behaves like a wide Gaussian — but without the per-LOD texel
// structure a single-LOD tap shows. Single-pass, 5 texture fetches.
// pc.lod selects the heaviest mip (centre of mass for the blur).
vec3 wallpaperBlur(vec2 uv, float lod)
{
    // σ-like spacing: closer mip levels carry more weight, far mips
    // soften the result. Weights sum to 1.
    vec3 c  = textureLod(atlas, uv, max(lod - 2.0, 0.0)).rgb * 0.08;
    c      += textureLod(atlas, uv, max(lod - 1.0, 0.0)).rgb * 0.22;
    c      += textureLod(atlas, uv,        lod        ).rgb * 0.40;
    c      += textureLod(atlas, uv,        lod + 1.0  ).rgb * 0.22;
    c      += textureLod(atlas, uv,        lod + 2.0  ).rgb * 0.08;
    return c;
}

void main()
{
    vec4 sampled;
    if (pc.lod > 0.0) {
        sampled = vec4(wallpaperBlur(fragUv, pc.lod), 1.0);
    } else {
        sampled = texture(atlas, fragUv);
    }
    // Atlas is SRGB-sampled (hardware decodes to linear on read).
    vec3 rgb = mix(sampled.rgb, pc.tintRgba.rgb, pc.tintRgba.a);
    float a = mix(sampled.a, 1.0, pc.tintRgba.a);
    // Premultiplied output, modulated by opacity.
    outColor = vec4(rgb * fragOpacity, a * fragOpacity);
}
