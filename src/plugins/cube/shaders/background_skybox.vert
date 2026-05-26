#version 450

// CubeEffectV2 SkyBox-mode background vertex shader.
//
// Emits a full-screen triangle (3 verts covering the swapchain).
// The fragment shader reconstructs the view direction from the
// inverse projection-view matrix supplied via push constants; an
// interpolated `viewRayWorld` varying gives the fragment a
// world-space ray direction free of per-fragment matrix math.
//
// Build: glslc -O --target-env=vulkan1.2 background_skybox.vert
//              -o background_skybox.vert.spv

layout(push_constant) uniform PC {
    // Inverse(P * V) — used to compute the corner direction in
    // world space at the far plane (NDC z = 1).
    mat4 invViewProj;
    float opacity;
} pc;

layout(location = 0) out vec3 viewRayWorld;
layout(location = 1) out float fragOpacity;

void main()
{
    // Full-screen triangle (the standard 3-vert recipe). Index 0 →
    // (-1, -1), 1 → (3, -1), 2 → (-1, 3) — the triangle is bigger
    // than the screen and the off-screen verts get clipped away.
    // Cheaper than a 4-vert quad because there's no shared edge to
    // rasterise twice.
    vec2 pos = vec2((gl_VertexIndex & 1) << 2, (gl_VertexIndex & 2) << 1) - vec2(1.0);
    gl_Position = vec4(pos, 1.0, 1.0);

    // World-space view-ray at the far plane for this vertex.
    // Interpolation across the triangle gives a smoothly-varying
    // direction the fragment can normalize.
    vec4 farH = pc.invViewProj * vec4(pos, 1.0, 1.0);
    viewRayWorld = farH.xyz / farH.w;
    fragOpacity = pc.opacity;
}
