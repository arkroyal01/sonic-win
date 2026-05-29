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
    // Soft-edge falloff distance in normalised SDF radius units.
    // Zero = hard discard outside the rounded boundary. Positive =
    // smoothstep alpha falloff from the boundary out to
    // (1.0 + edgeSoftnessUv) in q-space. Used by drop-shadow passes
    // to fade the dark rect out gradually past the card edge.
    float edgeSoftnessUv;
    float _pad;
    // Soft-shadow inset for the proper rounded-box-SDF path. When
    // both components are non-zero, the shader uses a signed-
    // distance rounded box instead of the elliptical-corner discard:
    // inner half-extent = shadowInnerHalfUv, scalar corner radius =
    // shadowCornerRadiusUv (both in the quad's local UV space), and
    // outside-the-boundary alpha smoothsteps to zero over
    // edgeSoftnessUv distance. Lets a single draw emit a proper
    // soft drop shadow with Gaussian-like falloff outside the card.
    vec2 shadowInnerHalfUv;
    float shadowCornerRadiusUv;
    float _pad2;
} pc;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragOpacity;

layout(location = 0) out vec4 outColor;

// Two-LOD kawase-style blur. Each tap is the dual-kawase downsample
// pattern (centre weight 4 + 4 diagonals weight 1, sum 8) at one mip
// level — bilinear filtering at the mip already blurs in-pixel, so
// even a 5-tap diamond reads as a smooth Gaussian. We sample twice:
// a coarser mip with a wider spread gives the broad backdrop blur,
// and a finer mip with a tight spread fills in low-frequency detail
// the coarser sample loses. The 60/40 blend keeps the wide spread
// dominant without the heavy mip-step aliasing that the previous
// single-LOD 9-tap path showed at low wash multipliers.
//
// The `lod` push-constant arrives as a base offset (cpp passes 1.0);
// we add to it so cpp can dial overall strength without rewriting
// the shader's mip selection.
vec3 kawaseTap(vec2 uv, float lod, float offsetTexels)
{
    vec2 t = offsetTexels / vec2(textureSize(atlas, int(lod)));
    vec4 sum = textureLod(atlas, uv, lod) * 4.0;
    sum += textureLod(atlas, uv + vec2( t.x,  t.y), lod);
    sum += textureLod(atlas, uv + vec2(-t.x,  t.y), lod);
    sum += textureLod(atlas, uv + vec2( t.x, -t.y), lod);
    sum += textureLod(atlas, uv + vec2(-t.x, -t.y), lod);
    return sum.rgb / 8.0;
}

vec3 wallpaperBlur(vec2 uv, float lod)
{
    return 0.60 * kawaseTap(uv, lod + 2.0, 2.5)
         + 0.40 * kawaseTap(uv, lod + 4.0, 1.0);
}

void main()
{
    // Two alpha paths, chosen by which push-constant fields are set:
    //   shadowInnerHalfUv non-zero → proper rounded-box SDF (drop
    //   shadow with smooth Gaussian-like falloff outside the inner
    //   card).
    //   cornerRadiusUv non-zero (and shadow path inactive) → simple
    //   elliptical-corner discard / hard rounded boundary (tiles,
    //   wallpaper card).
    //   Otherwise → rectangular fullscreen pass.
    float edgeAlpha = 1.0;
    if (pc.shadowInnerHalfUv.x > 0.0 && pc.shadowInnerHalfUv.y > 0.0) {
        // Rounded-box signed distance function. Returns negative
        // inside the inner card, zero on its boundary, positive
        // outside (magnitude = distance to nearest boundary point
        // in the quad's UV space). The smoothstep then fades alpha
        // from 1.0 at the boundary to 0 at edgeSoftnessUv beyond it.
        vec2 p = fragUv * 2.0 - 1.0; // -1..1 quad UV
        float r = pc.shadowCornerRadiusUv;
        vec2 d = abs(p) - pc.shadowInnerHalfUv + vec2(r);
        float sdf = length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - r;
        if (sdf <= 0.0) {
            edgeAlpha = 1.0;
        } else if (pc.edgeSoftnessUv > 0.0) {
            edgeAlpha = 1.0 - smoothstep(0.0, pc.edgeSoftnessUv, sdf);
            if (edgeAlpha <= 0.0) discard;
        } else {
            discard;
        }
    } else if (pc.cornerRadiusUv.x > 0.0 || pc.cornerRadiusUv.y > 0.0) {
        vec2 p = abs(fragUv * 2.0 - 1.0);
        vec2 d = p - (vec2(1.0) - pc.cornerRadiusUv);
        if (d.x > 0.0 && d.y > 0.0) {
            // Elliptical-corner SDF: distance in radius-normalised
            // space. The cpp side picks per-axis r so the resulting
            // ellipse traces a circular pixel-space corner.
            vec2 q = d / pc.cornerRadiusUv;
            float distQ = length(q);
            if (pc.edgeSoftnessUv > 0.0) {
                edgeAlpha = 1.0 - smoothstep(1.0, 1.0 + pc.edgeSoftnessUv, distQ);
                if (edgeAlpha <= 0.0) discard;
            } else if (distQ > 1.0) {
                discard;
            }
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
    // Premultiplied output, modulated by opacity and the soft-edge
    // alpha falloff (edgeAlpha is 1.0 for the hard-discard path).
    outColor = vec4(rgb * fragOpacity, a * fragOpacity) * edgeAlpha;
}
