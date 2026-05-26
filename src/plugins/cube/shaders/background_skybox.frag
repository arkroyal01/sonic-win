#version 450

// CubeEffectV2 SkyBox-mode background fragment shader.
//
// Samples a 2:1 equirectangular skybox texture via the view-ray
// direction received from the vertex stage. Equirect mapping:
//   u = atan2(dx, dz) / (2π) + 0.5
//   v = asin(dy)      / π    + 0.5
// (atan2's arg order picks +X to the right of -Z so the panorama
// faces the camera at yaw=0.)
//
// Output is premultiplied alpha — the post-FX pass alpha-blends
// against the cleared background colour beneath, so the skybox can
// have transparency too if the user wires one in (V1 doesn't support
// that; we don't try to either).
//
// Build: glslc -O --target-env=vulkan1.2 background_skybox.frag
//              -o background_skybox.frag.spv

layout(push_constant) uniform PC {
    mat4 invViewProj;
    vec4 cameraPosW;
    float opacity;
} pc;

layout(set = 0, binding = 0) uniform sampler2D skybox;

layout(location = 0) in vec3 viewRayWorld;
layout(location = 1) in float fragOpacity;

layout(location = 0) out vec4 outColor;

const float kInvTwoPi = 0.15915494309189535;  // 1 / (2 * pi)
const float kInvPi = 0.3183098861837907;       // 1 / pi

void main()
{
    vec3 dir = normalize(viewRayWorld - pc.cameraPosW.xyz);
    float u = atan(dir.x, dir.z) * kInvTwoPi + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) * kInvPi + 0.5;
    vec4 sampled = texture(skybox, vec2(u, v));
    outColor = vec4(sampled.rgb * fragOpacity, fragOpacity);
}
