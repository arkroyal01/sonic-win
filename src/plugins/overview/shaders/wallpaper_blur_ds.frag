#version 450

// Dual-kawase downsample. Centre weight 4 + four diagonal taps weight 1
// each (sum 8). The sampler's bilinear interpolation smooths each
// diagonal so even one tap per quadrant reads as a smooth Gaussian.
// Chained across progressively smaller render targets the effective
// radius grows without raising per-pixel shader cost — see
// wallpaper_blur_us.frag for the matching upsample.
//
// halfpixel = 0.5 / source-texture-size; offset scales the canonical
// tap distance (offset = 1 is the standard kawase pattern). The blur
// plugin uses the same shader on the GL side.

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform PC {
    vec2 halfpixel;
    float offset;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 sum = texture(srcTex, vUv) * 4.0;
    sum += texture(srcTex, vUv + vec2( pc.halfpixel.x,  pc.halfpixel.y) * pc.offset);
    sum += texture(srcTex, vUv + vec2(-pc.halfpixel.x,  pc.halfpixel.y) * pc.offset);
    sum += texture(srcTex, vUv + vec2( pc.halfpixel.x, -pc.halfpixel.y) * pc.offset);
    sum += texture(srcTex, vUv + vec2(-pc.halfpixel.x, -pc.halfpixel.y) * pc.offset);
    outColor = sum / 8.0;
}
