#version 450

// CubeEffectV2 face vertex shader.
//
// Each face of the desktop cube is a unit quad in model space —
// corners at (-0.5, -0.5, 0) to (+0.5, +0.5, 0). The C++ side
// supplies a per-face MVP matrix that:
//   1. Scales by (faceW, faceH, 1) so the quad is fb-sized in world
//      space.
//   2. Translates by (0, 0, faceDistance) so the face sits on the
//      +Z axis.
//   3. Rotates around Y by `angleTick * faceIndex` so each face
//      sits at its azimuth around the cube.
//   4. View matrix (camera lookAt).
//   5. Projection matrix (perspective, aspect-corrected).
//
// Atlas UV for the face's desktop comes through push constants too.
// The atlas is SRGB-sampled (hardware decodes to linear); output
// blending is premultiplied alpha, same as the overview pipeline.
//
// Build: glslc -O --target-env=vulkan1.2 cube_face.vert
//              -o cube_face.vert.spv

layout(push_constant) uniform PC {
    // World→clip per-face MVP. mat4 occupies 64 bytes; the rest of
    // the block fits inside Vulkan's minimum 128-byte push-constant
    // budget so we don't hit the limit at runtime.
    mat4 mvp;
    // Atlas UV sub-rect: xy = top-left, zw = size. Same packing the
    // overview shader uses.
    vec4 atlasSlotUv;
    // Global slide-in / slide-out alpha.
    float opacity;
} pc;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out float fragOpacity;

void main()
{
    // gl_VertexIndex picks one of four quad corners. Triangle strip
    // emit order matches overview_quad.vert:
    //   0 -> (0, 0) top-left
    //   1 -> (1, 0) top-right
    //   2 -> (0, 1) bottom-left
    //   3 -> (1, 1) bottom-right
    vec2 corner = vec2(float(gl_VertexIndex & 1),
                       float(gl_VertexIndex >> 1));

    // Centred unit quad in model space. Y+ points up; after the
    // model-view-projection chain the face ends up the right way
    // up on screen regardless of the kwin Vulkan viewport's Y-flip
    // ([[project_vulkan_y_flip_viewport]]) because we mirror UV.y
    // below to compensate.
    vec3 modelPos = vec3(corner.x - 0.5, 0.5 - corner.y, 0.0);
    gl_Position = pc.mvp * vec4(modelPos, 1.0);

    // The atlas slot stores its desktop content top-down (we wrote
    // it through a Y-flipped viewport in renderDesktopsToAtlas).
    // corner.y = 0 → top of quad → sample UV.y = 0 → top of slot.
    fragUv = pc.atlasSlotUv.xy + vec2(corner.x, corner.y) * pc.atlasSlotUv.zw;
    fragOpacity = pc.opacity;
}
