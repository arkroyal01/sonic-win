#version 450

// Dual-kawase upsample. Four axial taps weight 1 + four diagonal taps
// weight 2 each (sum 12). Paired with the downsample chain this
// reconstructs a Gaussian-quality blur at the destination resolution
// while keeping the per-pixel shader cost bounded.

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform PC {
    vec2 halfpixel;
    float offset;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 sum = texture(srcTex, vUv + vec2(-pc.halfpixel.x * 2.0, 0.0) * pc.offset);
    sum += texture(srcTex, vUv + vec2(-pc.halfpixel.x,  pc.halfpixel.y) * pc.offset) * 2.0;
    sum += texture(srcTex, vUv + vec2( 0.0,  pc.halfpixel.y * 2.0) * pc.offset);
    sum += texture(srcTex, vUv + vec2( pc.halfpixel.x,  pc.halfpixel.y) * pc.offset) * 2.0;
    sum += texture(srcTex, vUv + vec2( pc.halfpixel.x * 2.0, 0.0) * pc.offset);
    sum += texture(srcTex, vUv + vec2( pc.halfpixel.x, -pc.halfpixel.y) * pc.offset) * 2.0;
    sum += texture(srcTex, vUv + vec2( 0.0, -pc.halfpixel.y * 2.0) * pc.offset);
    sum += texture(srcTex, vUv + vec2(-pc.halfpixel.x, -pc.halfpixel.y) * pc.offset) * 2.0;
    outColor = sum / 12.0;
}
