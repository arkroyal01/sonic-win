/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "overvieweffectv2.h"

#include "effect/effecthandler.h"

#if HAVE_VULKAN
#include "compositor.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanthumbnailatlas.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene.h"
#include "utils/common.h"
#endif

#include <KGlobalAccel>
#include <KLocalizedString>

#include <QAction>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QKeySequence>

#if HAVE_VULKAN

namespace
{
// OverviewEffectV2 textured-quad shaders, pre-compiled from
// src/plugins/overview/shaders/overview_quad.{vert,frag} via
// `glslc -O`. One pipeline draws every window tile by varying the
// push-constant rect + atlas-UV per draw call. Same embed-the-bytes
// pattern as src/plugins/backgroundcontrast/contrast.cpp:74.

static const uint32_t s_overviewQuadVertSpv[] = {
    0x07230203,
    0x00010000,
    0x000d000b,
    0x00000042,
    0x00000000,
    0x00020011,
    0x00000001,
    0x0006000b,
    0x00000001,
    0x4c534c47,
    0x6474732e,
    0x3035342e,
    0x00000000,
    0x0003000e,
    0x00000000,
    0x00000001,
    0x0009000f,
    0x00000000,
    0x00000004,
    0x6e69616d,
    0x00000000,
    0x0000000c,
    0x0000001b,
    0x00000032,
    0x0000003d,
    0x00040047,
    0x0000000c,
    0x0000000b,
    0x0000002a,
    0x00030047,
    0x00000019,
    0x00000002,
    0x00050048,
    0x00000019,
    0x00000000,
    0x0000000b,
    0x00000000,
    0x00050048,
    0x00000019,
    0x00000001,
    0x0000000b,
    0x00000001,
    0x00050048,
    0x00000019,
    0x00000002,
    0x0000000b,
    0x00000003,
    0x00050048,
    0x00000019,
    0x00000003,
    0x0000000b,
    0x00000004,
    0x00030047,
    0x0000001d,
    0x00000002,
    0x00050048,
    0x0000001d,
    0x00000000,
    0x00000023,
    0x00000000,
    0x00050048,
    0x0000001d,
    0x00000001,
    0x00000023,
    0x00000010,
    0x00050048,
    0x0000001d,
    0x00000002,
    0x00000023,
    0x00000020,
    0x00040047,
    0x00000032,
    0x0000001e,
    0x00000000,
    0x00040047,
    0x0000003d,
    0x0000001e,
    0x00000001,
    0x00020013,
    0x00000002,
    0x00030021,
    0x00000003,
    0x00000002,
    0x00030016,
    0x00000006,
    0x00000020,
    0x00040017,
    0x00000007,
    0x00000006,
    0x00000002,
    0x00040015,
    0x0000000a,
    0x00000020,
    0x00000001,
    0x00040020,
    0x0000000b,
    0x00000001,
    0x0000000a,
    0x0004003b,
    0x0000000b,
    0x0000000c,
    0x00000001,
    0x0004002b,
    0x0000000a,
    0x0000000e,
    0x00000001,
    0x00040017,
    0x00000015,
    0x00000006,
    0x00000004,
    0x00040015,
    0x00000016,
    0x00000020,
    0x00000000,
    0x0004002b,
    0x00000016,
    0x00000017,
    0x00000001,
    0x0004001c,
    0x00000018,
    0x00000006,
    0x00000017,
    0x0006001e,
    0x00000019,
    0x00000015,
    0x00000006,
    0x00000018,
    0x00000018,
    0x00040020,
    0x0000001a,
    0x00000003,
    0x00000019,
    0x0004003b,
    0x0000001a,
    0x0000001b,
    0x00000003,
    0x0004002b,
    0x0000000a,
    0x0000001c,
    0x00000000,
    0x0005001e,
    0x0000001d,
    0x00000015,
    0x00000015,
    0x00000006,
    0x00040020,
    0x0000001e,
    0x00000009,
    0x0000001d,
    0x0004003b,
    0x0000001e,
    0x0000001f,
    0x00000009,
    0x00040020,
    0x00000020,
    0x00000009,
    0x00000015,
    0x0004002b,
    0x00000006,
    0x0000002a,
    0x00000000,
    0x0004002b,
    0x00000006,
    0x0000002b,
    0x3f800000,
    0x00040020,
    0x0000002f,
    0x00000003,
    0x00000015,
    0x00040020,
    0x00000031,
    0x00000003,
    0x00000007,
    0x0004003b,
    0x00000031,
    0x00000032,
    0x00000003,
    0x00040020,
    0x0000003c,
    0x00000003,
    0x00000006,
    0x0004003b,
    0x0000003c,
    0x0000003d,
    0x00000003,
    0x0004002b,
    0x0000000a,
    0x0000003e,
    0x00000002,
    0x00040020,
    0x0000003f,
    0x00000009,
    0x00000006,
    0x00050036,
    0x00000002,
    0x00000004,
    0x00000000,
    0x00000003,
    0x000200f8,
    0x00000005,
    0x0004003d,
    0x0000000a,
    0x0000000d,
    0x0000000c,
    0x000500c7,
    0x0000000a,
    0x0000000f,
    0x0000000d,
    0x0000000e,
    0x0004006f,
    0x00000006,
    0x00000010,
    0x0000000f,
    0x000500c3,
    0x0000000a,
    0x00000012,
    0x0000000d,
    0x0000000e,
    0x0004006f,
    0x00000006,
    0x00000013,
    0x00000012,
    0x00050050,
    0x00000007,
    0x00000014,
    0x00000010,
    0x00000013,
    0x00050041,
    0x00000020,
    0x00000021,
    0x0000001f,
    0x0000001c,
    0x0004003d,
    0x00000015,
    0x00000022,
    0x00000021,
    0x0007004f,
    0x00000007,
    0x00000023,
    0x00000022,
    0x00000022,
    0x00000000,
    0x00000001,
    0x0007004f,
    0x00000007,
    0x00000027,
    0x00000022,
    0x00000022,
    0x00000002,
    0x00000003,
    0x00050085,
    0x00000007,
    0x00000028,
    0x00000014,
    0x00000027,
    0x00050081,
    0x00000007,
    0x00000029,
    0x00000023,
    0x00000028,
    0x00050051,
    0x00000006,
    0x0000002c,
    0x00000029,
    0x00000000,
    0x00050051,
    0x00000006,
    0x0000002d,
    0x00000029,
    0x00000001,
    0x00070050,
    0x00000015,
    0x0000002e,
    0x0000002c,
    0x0000002d,
    0x0000002a,
    0x0000002b,
    0x00050041,
    0x0000002f,
    0x00000030,
    0x0000001b,
    0x0000001c,
    0x0003003e,
    0x00000030,
    0x0000002e,
    0x00050041,
    0x00000020,
    0x00000033,
    0x0000001f,
    0x0000000e,
    0x0004003d,
    0x00000015,
    0x00000034,
    0x00000033,
    0x0007004f,
    0x00000007,
    0x00000035,
    0x00000034,
    0x00000034,
    0x00000000,
    0x00000001,
    0x0007004f,
    0x00000007,
    0x00000039,
    0x00000034,
    0x00000034,
    0x00000002,
    0x00000003,
    0x00050085,
    0x00000007,
    0x0000003a,
    0x00000014,
    0x00000039,
    0x00050081,
    0x00000007,
    0x0000003b,
    0x00000035,
    0x0000003a,
    0x0003003e,
    0x00000032,
    0x0000003b,
    0x00050041,
    0x0000003f,
    0x00000040,
    0x0000001f,
    0x0000003e,
    0x0004003d,
    0x00000006,
    0x00000041,
    0x00000040,
    0x0003003e,
    0x0000003d,
    0x00000041,
    0x000100fd,
    0x00010038,
};

static const uint32_t s_overviewQuadFragSpv[] = {
    0x07230203,
    0x00010000,
    0x000d000b,
    0x00000028,
    0x00000000,
    0x00020011,
    0x00000001,
    0x0006000b,
    0x00000001,
    0x4c534c47,
    0x6474732e,
    0x3035342e,
    0x00000000,
    0x0003000e,
    0x00000000,
    0x00000001,
    0x0008000f,
    0x00000004,
    0x00000004,
    0x6e69616d,
    0x00000000,
    0x00000011,
    0x00000015,
    0x0000001a,
    0x00030010,
    0x00000004,
    0x00000007,
    0x00040047,
    0x0000000d,
    0x00000021,
    0x00000000,
    0x00040047,
    0x0000000d,
    0x00000022,
    0x00000000,
    0x00040047,
    0x00000011,
    0x0000001e,
    0x00000000,
    0x00040047,
    0x00000015,
    0x0000001e,
    0x00000000,
    0x00040047,
    0x0000001a,
    0x0000001e,
    0x00000001,
    0x00020013,
    0x00000002,
    0x00030021,
    0x00000003,
    0x00000002,
    0x00030016,
    0x00000006,
    0x00000020,
    0x00040017,
    0x00000007,
    0x00000006,
    0x00000004,
    0x00090019,
    0x0000000a,
    0x00000006,
    0x00000001,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000001,
    0x00000000,
    0x0003001b,
    0x0000000b,
    0x0000000a,
    0x00040020,
    0x0000000c,
    0x00000000,
    0x0000000b,
    0x0004003b,
    0x0000000c,
    0x0000000d,
    0x00000000,
    0x00040017,
    0x0000000f,
    0x00000006,
    0x00000002,
    0x00040020,
    0x00000010,
    0x00000001,
    0x0000000f,
    0x0004003b,
    0x00000010,
    0x00000011,
    0x00000001,
    0x00040020,
    0x00000014,
    0x00000003,
    0x00000007,
    0x0004003b,
    0x00000014,
    0x00000015,
    0x00000003,
    0x00040017,
    0x00000016,
    0x00000006,
    0x00000003,
    0x00040020,
    0x00000019,
    0x00000001,
    0x00000006,
    0x0004003b,
    0x00000019,
    0x0000001a,
    0x00000001,
    0x00050036,
    0x00000002,
    0x00000004,
    0x00000000,
    0x00000003,
    0x000200f8,
    0x00000005,
    0x0004003d,
    0x0000000b,
    0x0000000e,
    0x0000000d,
    0x0004003d,
    0x0000000f,
    0x00000012,
    0x00000011,
    0x00050057,
    0x00000007,
    0x00000013,
    0x0000000e,
    0x00000012,
    0x0008004f,
    0x00000016,
    0x00000018,
    0x00000013,
    0x00000013,
    0x00000000,
    0x00000001,
    0x00000002,
    0x0004003d,
    0x00000006,
    0x0000001b,
    0x0000001a,
    0x0005008e,
    0x00000016,
    0x0000001c,
    0x00000018,
    0x0000001b,
    0x00050051,
    0x00000006,
    0x00000021,
    0x00000013,
    0x00000003,
    0x00050085,
    0x00000006,
    0x00000023,
    0x00000021,
    0x0000001b,
    0x00050051,
    0x00000006,
    0x00000024,
    0x0000001c,
    0x00000000,
    0x00050051,
    0x00000006,
    0x00000025,
    0x0000001c,
    0x00000001,
    0x00050051,
    0x00000006,
    0x00000026,
    0x0000001c,
    0x00000002,
    0x00070050,
    0x00000007,
    0x00000027,
    0x00000024,
    0x00000025,
    0x00000026,
    0x00000023,
    0x0003003e,
    0x00000015,
    0x00000027,
    0x000100fd,
    0x00010038,
};

// Push-constant block layout must mirror the GLSL `PC` struct in
// shaders/overview_quad.vert. Stored in shader-readable order; offsets
// 0..32 and 32..36 must match the GLSL `layout(offset = …)` rules (no
// explicit offsets used → tightly packed, vec4 alignment honoured).
struct OverviewQuadPushConstants
{
    float quadRectNdc[4];
    float atlasSlotUv[4];
    float opacity;
};
static_assert(sizeof(OverviewQuadPushConstants) == 36,
              "Push-constant layout must match shaders/overview_quad.vert");
} // namespace

#endif // HAVE_VULKAN

namespace KWin
{

bool OverviewEffectV2::supported()
{
    // Gate strictly on the env var so the existing OverviewEffect stays
    // the default. When the user opts in, this V2 effect takes over the
    // same shortcut and OverviewEffect::supported() refuses to load.
    if (qEnvironmentVariableIntValue("KWIN_OVERVIEW_V2") == 0) {
        return false;
    }
    // Compositor check matches the existing plugin's predicate; V2 will
    // later require Vulkan specifically once the renderer integration
    // lands, but the skeleton works either way.
    return effects && (effects->isOpenGLCompositing() || effects->isVulkanCompositing());
}

OverviewEffectV2::OverviewEffectV2()
{
    m_animation.setDuration(m_animationDuration);
    m_animation.setEasingCurve(QEasingCurve::OutCubic);
    m_animation.setStartValue(qreal(0.0));
    m_animation.setEndValue(qreal(1.0));

    connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_activationFactor = v.toReal();
        effects->addRepaintFull();
    });
    connect(&m_animation, &QVariantAnimation::finished, this, [this]() {
        if (qFuzzyCompare(m_activationFactor, 0.0)) {
            m_visible = false;
            effects->addRepaintFull();
        }
    });

    // Register the same `Overview` action name as the existing
    // OverviewEffect — the user's saved keyboard binding carries over.
    // mutual exclusion at supported() level guarantees only one plugin
    // is loaded at a time, so there's no double-registration race.
    m_toggleAction = new QAction(this);
    m_toggleAction->setObjectName(QStringLiteral("Overview"));
    m_toggleAction->setText(i18nc("@action Overview is the name of a Kwin effect", "Toggle Overview"));
    m_toggleAction->setAutoRepeat(false);
    const QKeySequence defaultShortcut = Qt::META | Qt::Key_W;
    KGlobalAccel::self()->setDefaultShortcut(m_toggleAction, {defaultShortcut});
    KGlobalAccel::self()->setShortcut(m_toggleAction, {defaultShortcut});
    connect(m_toggleAction, &QAction::triggered, this, [this]() {
        if (m_visible) {
            deactivate();
        } else {
            activate();
        }
    });
}

OverviewEffectV2::~OverviewEffectV2()
{
    if (m_toggleAction) {
        KGlobalAccel::self()->removeAllShortcuts(m_toggleAction);
    }
#if HAVE_VULKAN
    destroyVulkanPipeline();
#endif
}

#if HAVE_VULKAN
bool OverviewEffectV2::ensureVulkanPipeline(VulkanContext *ctx, VkFormat colorFormat)
{
    // Lazy + idempotent: rebuild only when the swapchain format changes
    // (rare; typically once per display reconfigure). Idle calls when
    // the cached pipeline is still valid are a single integer compare.
    if (m_vkPipeline != VK_NULL_HANDLE && m_pipelineColorFormat == colorFormat && m_vulkanCtx == ctx) {
        return true;
    }
    destroyVulkanPipeline();
    if (!ctx) {
        return false;
    }
    m_vulkanCtx = ctx;
    m_pipelineColorFormat = colorFormat;

    VkDevice device = ctx->backend()->device();

    // Shader modules from the embedded SPIR-V. Destroyed at the bottom
    // of this function — the spec lets us release modules as soon as
    // the pipeline references them via vkCreateGraphicsPipelines.
    auto makeModule = [&](const uint32_t *code, size_t byteSize) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = byteSize;
        info.pCode = code;
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &info, nullptr, &mod);
        return mod;
    };
    VkShaderModule vertMod = makeModule(s_overviewQuadVertSpv, sizeof(s_overviewQuadVertSpv));
    VkShaderModule fragMod = makeModule(s_overviewQuadFragSpv, sizeof(s_overviewQuadFragSpv));
    if (!vertMod || !fragMod) {
        if (vertMod) {
            vkDestroyShaderModule(device, vertMod, nullptr);
        }
        if (fragMod) {
            vkDestroyShaderModule(device, fragMod, nullptr);
        }
        qCWarning(KWIN_VULKAN) << "OverviewEffectV2: shader-module creation failed";
        return false;
    }

    // Descriptor set: binding 0 = combined image sampler (atlas + its
    // built-in linear-mipmap sampler). Push-descriptor flag matches the
    // codebase convention so the renderer's KHR_push_descriptor
    // shortcut is used at draw time (no descriptor pool churn).
    VkDescriptorSetLayoutBinding dsBinding{};
    dsBinding.binding = 0;
    dsBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsBinding.descriptorCount = 1;
    dsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsLayoutInfo{};
    dsLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutInfo.bindingCount = 1;
    dsLayoutInfo.pBindings = &dsBinding;
    if (ctx->backend()->supportsPushDescriptor()) {
        dsLayoutInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    }
    if (vkCreateDescriptorSetLayout(device, &dsLayoutInfo, nullptr, &m_vkDescriptorSetLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertMod, nullptr);
        vkDestroyShaderModule(device, fragMod, nullptr);
        qCWarning(KWIN_VULKAN) << "OverviewEffectV2: vkCreateDescriptorSetLayout failed";
        return false;
    }

    // Push constants visible to vertex (quad rect + atlas UV) and
    // fragment (opacity) stages; declare a single 36-byte range that
    // spans both ends.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(OverviewQuadPushConstants);
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_vkDescriptorSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_vkPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertMod, nullptr);
        vkDestroyShaderModule(device, fragMod, nullptr);
        vkDestroyDescriptorSetLayout(device, m_vkDescriptorSetLayout, nullptr);
        m_vkDescriptorSetLayout = VK_NULL_HANDLE;
        qCWarning(KWIN_VULKAN) << "OverviewEffectV2: vkCreatePipelineLayout failed";
        return false;
    }

    // Build the pipeline against a transient render pass with the
    // same attachment description as the renderer's post-FX pass. The
    // pipeline is render-pass-*compatible* — Vulkan only requires the
    // attachment formats + sample counts to match for compatibility,
    // not identity of the VkRenderPass object. We discard our copy
    // immediately after vkCreateGraphicsPipelines returns.
    auto compatRenderPass = VulkanRenderPass::createForSwapchainPostFx(ctx, colorFormat);
    if (!compatRenderPass) {
        vkDestroyShaderModule(device, vertMod, nullptr);
        vkDestroyShaderModule(device, fragMod, nullptr);
        destroyVulkanPipeline();
        qCWarning(KWIN_VULKAN) << "OverviewEffectV2: failed to construct compat render pass";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Premultiplied alpha blending — the fragment shader emits
    // premultiplied colour (rgb*opacity, a*opacity), so the standard
    // "SRC_ALPHA, ONE_MINUS_SRC_ALPHA" pair is replaced with the
    // premultiplied equivalent "ONE, ONE_MINUS_SRC_ALPHA".
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gpInfo{};
    gpInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpInfo.stageCount = 2;
    gpInfo.pStages = stages;
    gpInfo.pVertexInputState = &vi;
    gpInfo.pInputAssemblyState = &ia;
    gpInfo.pViewportState = &vp;
    gpInfo.pRasterizationState = &rs;
    gpInfo.pMultisampleState = &ms;
    gpInfo.pColorBlendState = &cb;
    gpInfo.pDynamicState = &dyn;
    gpInfo.layout = m_vkPipelineLayout;
    gpInfo.renderPass = compatRenderPass->renderPass();
    gpInfo.subpass = 0;

    const bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpInfo,
                                              nullptr, &m_vkPipeline)
        == VK_SUCCESS;

    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);

    if (!ok) {
        qCWarning(KWIN_VULKAN) << "OverviewEffectV2: vkCreateGraphicsPipelines failed";
        destroyVulkanPipeline();
        return false;
    }
    return true;
}

void OverviewEffectV2::destroyVulkanPipeline()
{
    if (!m_vulkanCtx) {
        return;
    }
    VkDevice device = m_vulkanCtx->backend()->device();
    if (m_vkPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_vkPipeline, nullptr);
        m_vkPipeline = VK_NULL_HANDLE;
    }
    if (m_vkPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_vkPipelineLayout, nullptr);
        m_vkPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_vkDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_vkDescriptorSetLayout, nullptr);
        m_vkDescriptorSetLayout = VK_NULL_HANDLE;
    }
    m_pipelineColorFormat = VK_FORMAT_UNDEFINED;
    m_vulkanCtx = nullptr;
}
#endif // HAVE_VULKAN

void OverviewEffectV2::activate()
{
    if (m_visible && m_animation.direction() == QVariantAnimation::Forward) {
        return;
    }
    m_visible = true;
    m_animation.stop();
    m_animation.setDirection(QVariantAnimation::Forward);
    m_animation.start();
}

void OverviewEffectV2::deactivate()
{
    if (!m_visible) {
        return;
    }
    m_animation.stop();
    m_animation.setDirection(QVariantAnimation::Backward);
    m_animation.start();
}

bool OverviewEffectV2::isActive() const
{
    return m_visible;
}

int OverviewEffectV2::requestedEffectChainPosition() const
{
    // Late in the chain: we want to paint over windows. Matches the
    // existing OverviewEffect's position (kept in sync intentionally so
    // toggling between V1 and V2 doesn't perturb other effects).
    return 70;
}

void OverviewEffectV2::prePaintScreen(ScreenPrePaintData &data,
                                      std::chrono::milliseconds presentTime)
{
    if (m_visible) {
        // Mask the screen so windows below us are still painted (we
        // composite on top). PAINT_SCREEN_TRANSFORMED would be needed
        // if we transformed window geometry, but phase 1 just overlays
        // a translucent rect.
        data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;
    }
    Effect::prePaintScreen(data, presentTime);
}

void OverviewEffectV2::paintScreen(const RenderTarget &renderTarget,
                                   const RenderViewport &viewport, int mask,
                                   const QRegion &region, Output *screen)
{
    // Let the windows below us paint first.
    Effect::paintScreen(renderTarget, viewport, mask, region, screen);

    if (!m_visible) {
        return;
    }

    // Phase 1 placeholder: no overlay drawn yet. Subsequent phases will
    // call ItemRendererVulkan here to paint the C++-managed scene tree
    // (background, desktop bar, window grid, search field) at the
    // current m_activationFactor.
    //
    // Intentionally a no-op for now to keep this commit purely the
    // skeleton + lifecycle; we don't want to bind a renderer-specific
    // path before the scene-tree design is in.
    (void)renderTarget;
    (void)viewport;
    (void)mask;
    (void)region;
    (void)screen;
}

void OverviewEffectV2::postPaintScreen()
{
    Effect::postPaintScreen();
}

void OverviewEffectV2::grabbedKeyboardEvent(QKeyEvent *event)
{
    if (event->type() == QEvent::KeyPress && event->key() == Qt::Key_Escape) {
        deactivate();
    }
}

} // namespace KWin

#include "moc_overvieweffectv2.cpp"
