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
    // Per-axis rounded-corner radius in UV space
    // (radius_px / half_quad_px on X and Y). Both zero disables the
    // SDF discard path so full-screen passes stay rectangular. The
    // per-axis split lets non-square quads draw a circular pixel-
    // space corner via an elliptical UV-space SDF.
    vec2 cornerRadiusUv;
} pc;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragOpacity;

layout(location = 0) out vec4 outColor;

// 9-tap symmetric Gaussian-ish blur at a single LOD with UV offsets.
// Earlier iterations blended across mip levels (1-5) for "free"
// coverage but the high mips introduced visible blockiness where
// mip aliasing showed through the wash. A single mid-mip plus
// spatial spread costs the same 9 fetch budget and produces a
// smoother result. Centre + 4 axial + 4 diagonal taps; weights sum
// to 1.0. Effective radius is set by the 4-texel step at the chosen
// mip level (cpp passes pc.lod = 1.0 so each step ≈ 8 source
// pixels in the original wallpaper).
vec3 wallpaperBlur(vec2 uv, float lod)
{
    vec2 t = 4.0 / vec2(textureSize(atlas, int(lod)));
    vec3 c  = textureLod(atlas, uv,                          lod).rgb * 0.20;
    c      += textureLod(atlas, uv + vec2( t.x,  0.0),       lod).rgb * 0.12;
    c      += textureLod(atlas, uv + vec2(-t.x,  0.0),       lod).rgb * 0.12;
    c      += textureLod(atlas, uv + vec2( 0.0,  t.y),       lod).rgb * 0.12;
    c      += textureLod(atlas, uv + vec2( 0.0, -t.y),       lod).rgb * 0.12;
    c      += textureLod(atlas, uv + vec2( t.x,  t.y),       lod).rgb * 0.08;
    c      += textureLod(atlas, uv + vec2(-t.x,  t.y),       lod).rgb * 0.08;
    c      += textureLod(atlas, uv + vec2( t.x, -t.y),       lod).rgb * 0.08;
    c      += textureLod(atlas, uv + vec2(-t.x, -t.y),       lod).rgb * 0.08;
    return c;
}

void main()
{
    // Rounded-rect discard: anything outside the inscribed rounded
    // rectangle drops out before sampling. The SDF works in the
    // quad's local UV space (centred at 0, half-extents 1). When
    // both components of cornerRadiusUv are zero the if-test short-
    // circuits — the path costs nothing for full-screen passes.
    if (pc.cornerRadiusUv.x > 0.0 || pc.cornerRadiusUv.y > 0.0) {
        vec2 p = abs(fragUv * 2.0 - 1.0);
        vec2 d = p - (vec2(1.0) - pc.cornerRadiusUv);
        if (d.x > 0.0 && d.y > 0.0) {
            // Elliptical-corner SDF: distance in radius-normalised
            // space. The cpp side picks per-axis r so the resulting
            // ellipse traces a circular pixel-space corner.
            vec2 q = d / pc.cornerRadiusUv;
            if (dot(q, q) > 1.0) discard;
        }
    }

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
