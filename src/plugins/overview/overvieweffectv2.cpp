/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "overvieweffectv2.h"

#include "effect/effecthandler.h"

#if HAVE_VULKAN
#include "compositor.h"
#include "effect/effectwindow.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "platformsupport/scenes/vulkan/vulkanthumbnailatlas.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene.h"
#include "utils/common.h"
#include "virtualdesktops.h"
#include "window.h"
#endif

#include <KGlobalAccel>
#include <KLocalizedString>

#include <QAction>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLoggingCategory>

namespace KWin
{
// Local logging category — the kwin lib's KWIN_VULKAN doesn't resolve
// across the LTO boundary when this static plugin is linked into
// kwin_x11 (undefined-reference at link time). Define our own; it
// shows up in QT_LOGGING_RULES as `kwin_overview_v2`.
Q_LOGGING_CATEGORY(KWIN_OVERVIEW_V2_LOG, "kwin_overview_v2", QtWarningMsg)
} // namespace KWin

#if HAVE_VULKAN

// OverviewEffectV2 textured-quad shaders, pre-compiled from
// src/plugins/overview/shaders/overview_quad.{vert,frag} via
// `glslc -O`. The packed SPIR-V tables live in their own header so
// clang-format doesn't blow them up to one element per line every
// time this file is touched.
#include "shaders/overview_quad_spv.inc"

namespace
{
using KWin::OverviewEffectV2Shaders::kFragSpv;
using KWin::OverviewEffectV2Shaders::kVertSpv;

// Push-constant block layout must mirror the GLSL `PC` struct in
// shaders/overview_quad.{vert,frag}. Tightly packed std140-style: vec4
// fields are 16-byte aligned, the trailing float follows naturally.
// tintRgba lets the post-pass draw both atlas-backed window tiles
// (tintRgba.a == 0) and solid-colour bar tiles (tintRgba.a == 1) from
// one pipeline.
struct OverviewQuadPushConstants
{
    float quadRectNdc[4];
    float atlasSlotUv[4];
    float tintRgba[4];
    float opacity;
};
static_assert(sizeof(OverviewQuadPushConstants) == 52,
              "Push-constant layout must match shaders/overview_quad.{vert,frag}");
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
#if HAVE_VULKAN
            // Stop drawing tiles, then release atlas slots. Order
            // matters: as long as the post-pass is registered, the
            // renderer will try to sample from the atlas slots, so
            // unregister BEFORE the slots get released.
            if (m_postPassId != -1) {
                if (auto *scene = Compositor::self()->scene()) {
                    if (auto *vk = dynamic_cast<ItemRendererVulkan *>(scene->renderer())) {
                        vk->unregisterFullscreenPostPass(m_postPassId);
                    }
                }
                m_postPassId = -1;
            }
            releaseAllSlots();
#endif
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
    QObject::disconnect(m_preFrameConnection);
    if (m_postPassId != -1) {
        if (auto *scene = Compositor::self()->scene()) {
            if (auto *vk = dynamic_cast<ItemRendererVulkan *>(scene->renderer())) {
                vk->unregisterFullscreenPostPass(m_postPassId);
            }
        }
        m_postPassId = -1;
    }
    releaseAllSlots();
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
    VkShaderModule vertMod = makeModule(kVertSpv, sizeof(kVertSpv));
    VkShaderModule fragMod = makeModule(kFragSpv, sizeof(kFragSpv));
    if (!vertMod || !fragMod) {
        if (vertMod) {
            vkDestroyShaderModule(device, vertMod, nullptr);
        }
        if (fragMod) {
            vkDestroyShaderModule(device, fragMod, nullptr);
        }
        qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: shader-module creation failed";
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
        qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: vkCreateDescriptorSetLayout failed";
        return false;
    }

    // Push constants are accessed in both stages: vertex reads quad
    // geometry and atlas UVs, fragment reads tintRgba to mix solid
    // colours for bar tiles into the sampled atlas pixel.
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
        qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: vkCreatePipelineLayout failed";
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
        qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: failed to construct compat render pass";
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
        qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: vkCreateGraphicsPipelines failed";
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

void OverviewEffectV2::reserveSlotsForCurrentDesktop()
{
    if (!effects || !effects->isVulkanCompositing()) {
        return;
    }
    auto *scene = Compositor::self()->scene();
    if (!scene) {
        return;
    }
    auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(scene->renderer());
    if (!vkRenderer) {
        return;
    }
    m_vulkanCtx = vkRenderer->context();
    if (!m_vulkanCtx) {
        return;
    }
    m_atlas = VulkanThumbnailAtlas::get(m_vulkanCtx);
    if (!m_atlas) {
        return;
    }

    // Walk current-desktop windows in stacking order and reserve one
    // atlas slot per window, sized to the window's visible geometry
    // (matching the rendered region in WindowThumbnailSource's existing
    // GL path). Skip hidden/unmanaged windows, and also the desktop
    // pseudo-window (plasmashell's wallpaper layer) and docks (panels):
    // those serve as the captured-scene background and a separate panel
    // strip respectively, not as overview tiles. Mirrors the V1
    // overview's `isNormalWindow` filter in WindowHeapView.
    auto *currentDesktop = effects->currentDesktop();
    for (EffectWindow *ew : effects->stackingOrder()) {
        if (!ew || !currentDesktop || !ew->isOnDesktop(currentDesktop)) {
            continue;
        }
        Window *handle = ew->window();
        if (!handle || !handle->isNormalWindow()) {
            continue;
        }
        const QSize size = handle->visibleGeometry().toAlignedRect().size();
        if (size.isEmpty()) {
            continue;
        }
        auto slot = m_atlas->reserve(size);
        if (!slot.isValid()) {
            qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: atlas reserve failed for"
                                            << handle->caption().left(40) << size;
            continue;
        }
        qCDebug(KWIN_OVERVIEW_V2_LOG).nospace()
            << "reserve slot for '" << handle->caption().left(40) << "' size=" << size
            << " atlasRect=" << slot.rect << " isFallback=" << slot.isFallback;
        // Keep the offscreen pixmap rendered for this window for as
        // long as we hold the slot. Without this, when the X11
        // compositor decides the window is "not needed", its
        // SurfaceItem's source texture becomes stale or unavailable
        // and renderItem produces empty atlas slots. WindowThumbnail-
        // Source uses the same ref/unref pattern.
        handle->refOffscreenRendering();
        m_windowSlots.emplace(handle, std::move(slot));
        // Drop the slot the moment the window dies so renderWindowsToAtlas
        // doesn't dereference a stale pointer in handle->visibleGeometry()
        // / handle->windowItem(). The atlas->release call has to wait on
        // any in-flight submit; do that lazily on the next render or at
        // releaseAllSlots time rather than synchronously in the destroyed
        // signal handler (which can fire mid-frame).
        connect(handle, &QObject::destroyed, this, [this, handle]() {
            auto it = m_windowSlots.find(handle);
            if (it == m_windowSlots.end()) {
                return;
            }
            if (m_vulkanCtx && m_lastAtlasSubmit.isValid()) {
                m_vulkanCtx->waitForSubmit(m_lastAtlasSubmit);
                m_lastAtlasSubmit = VulkanSubmitHandle{};
            }
            if (m_atlas) {
                m_atlas->release(it->second);
            }
            m_fallbackFramebuffers.erase(handle);
            // No unrefOffscreenRendering here — the window is being
            // destroyed; KWin tears down its own ref tracking.
            m_windowSlots.erase(it);
        });
    }
}

void OverviewEffectV2::releaseAllSlots()
{
    // Wait on any in-flight atlas submit so the GPU is done reading
    // from the slot's vertex-buffer region and writing into its image
    // before we hand the slot back to the atlas's free list (which
    // might immediately give the rect to a fresh consumer).
    if (m_vulkanCtx && m_lastAtlasSubmit.isValid()) {
        m_vulkanCtx->waitForSubmit(m_lastAtlasSubmit);
        m_lastAtlasSubmit = VulkanSubmitHandle{};
    }
    if (m_atlas) {
        for (auto &[handle, slot] : m_windowSlots) {
            if (handle) {
                handle->unrefOffscreenRendering();
            }
            m_atlas->release(slot);
        }
    }
    m_windowSlots.clear();
    m_tileLayout.clear();
    m_barTiles.clear();
    m_fallbackFramebuffers.clear();
    m_atlasFramebuffer.reset();
    m_atlasRenderPass.reset();
    // Drop the per-context atlas singleton too. V2 is currently the
    // only consumer, so keeping it across activations would hold
    // ~85 MB of VRAM (4096² SRGB + 5 mip levels) idle until the
    // next overview gesture. Reallocating on the next activate costs
    // a few ms — acceptable for an explicit user action and the
    // whole point of the C++ rewrite is to hand memory back when
    // it isn't needed. If a future consumer (switchers, window-
    // view) starts sharing this atlas, this drop becomes a ref-
    // counting concern. The atlas destructor queues its image and
    // views for destruction; VulkanContext drains the queue on the
    // next frame.
    if (m_vulkanCtx) {
        VulkanThumbnailAtlas::dropForContext(m_vulkanCtx);
    }
    m_atlas = nullptr;
}

void OverviewEffectV2::rebuildTileLayout(const QSize &fbSize)
{
    m_tileLayout.clear();
    if (m_windowSlots.empty() || !effects) {
        return;
    }
    // Walk stacking order (oldest below, freshest on top) so the grid
    // mirrors what the user expects: most-recently-focused tile in a
    // consistent position across activations. unordered_map iteration
    // would be deterministic-within-a-session but is implementation-
    // defined; rebuilding from effects->stackingOrder gives a stable,
    // documented order that both the post-pass and the hit-test
    // consume.
    std::vector<TileLayout> drawable;
    drawable.reserve(m_windowSlots.size());
    for (EffectWindow *ew : effects->stackingOrder()) {
        if (!ew) {
            continue;
        }
        Window *handle = ew->window();
        auto it = m_windowSlots.find(handle);
        if (it == m_windowSlots.end()) {
            continue;
        }
        if (!it->second.hasContent) {
            continue;
        }
        drawable.push_back({handle, it->second, 0, 0, 0, 0, 0, 0, 0, 0});
    }
    const int n = drawable.size();
    if (n == 0) {
        return;
    }
    const int cols = std::max(1, int(std::ceil(std::sqrt(double(n)))));
    const int rows = (n + cols - 1) / cols;
    // Grid top sits just below the desktop bar (kBarTop + kBarHeight +
    // a small gap); see rebuildBarLayout for the bar's NDC bounds.
    constexpr float kGridTop = -0.78f;
    constexpr float kGridBottom = 0.8f;
    const float cellNdcW = 1.6f / cols;
    const float cellNdcH = (kGridBottom - kGridTop) / rows;
    constexpr float kTilePad = 0.9f;
    const float maxNdcW = cellNdcW * kTilePad;
    const float maxNdcH = cellNdcH * kTilePad;
    const float screenW = std::max(1.0f, float(fbSize.width()));
    const float screenH = std::max(1.0f, float(fbSize.height()));
    for (int i = 0; i < n; ++i) {
        TileLayout &t = drawable[i];
        const QRectF realGeom = t.handle->visibleGeometry();
        t.realNdcX = (float(realGeom.x()) / screenW) * 2.0f - 1.0f;
        t.realNdcY = (float(realGeom.y()) / screenH) * 2.0f - 1.0f;
        t.realNdcW = (float(realGeom.width()) / screenW) * 2.0f;
        t.realNdcH = (float(realGeom.height()) / screenH) * 2.0f;
        // Aspect-preserving fit: realNdcW/realNdcH already encodes the
        // window's pixel aspect (the X/Y NDC mapping has different
        // scales — /screenW vs /screenH — so the ratio carries it).
        // Pick the largest uniform scale that fits the cell, then
        // centre the tile inside its cell so smaller windows don't
        // stretch to fill.
        const float scaleX = maxNdcW / std::max(1e-6f, t.realNdcW);
        const float scaleY = maxNdcH / std::max(1e-6f, t.realNdcH);
        const float scale = std::min(scaleX, scaleY);
        const float tileNdcW = t.realNdcW * scale;
        const float tileNdcH = t.realNdcH * scale;
        const int col = i % cols;
        const int row = i / cols;
        const float cellOriginX = -0.8f + col * cellNdcW;
        const float cellOriginY = kGridTop + row * cellNdcH;
        t.gridNdcX = cellOriginX + (cellNdcW - tileNdcW) * 0.5f;
        t.gridNdcY = cellOriginY + (cellNdcH - tileNdcH) * 0.5f;
        t.gridNdcW = tileNdcW;
        t.gridNdcH = tileNdcH;
    }
    m_tileLayout = std::move(drawable);
}

void OverviewEffectV2::rebuildBarLayout(const QSize &fbSize)
{
    Q_UNUSED(fbSize);
    m_barTiles.clear();
    if (!effects) {
        return;
    }
    const auto desktops = effects->desktops();
    const int n = desktops.size();
    if (n <= 1) {
        // Only one desktop → no bar (nothing to switch to).
        return;
    }
    VirtualDesktop *current = effects->currentDesktop();

    // Bar bounds in NDC. Positive viewport Y in the post-pass → -1 is
    // the top of the screen. Bar gets a thin band at the top; tile
    // width is chosen so each tile matches the screen's pixel aspect
    // (NDC X and Y have different pixel scales, so a screen-aspect
    // rect has equal NDC width and height). The strip is centred
    // horizontally. Keep kBarTop+kBarHeight in sync with kGridTop in
    // rebuildTileLayout so grid tiles and bar tiles don't overlap.
    constexpr float kBarTop = -0.96f;
    constexpr float kBarHeight = 0.16f;
    constexpr float kGutter = 0.012f;
    const float tileW = kBarHeight; // → screen aspect in pixel space
    const float stripW = n * tileW + (n - 1) * kGutter;
    const float stripLeft = -stripW * 0.5f;

    m_barTiles.reserve(n);
    for (int i = 0; i < n; ++i) {
        m_barTiles.push_back({
            desktops[i],
            stripLeft + i * (tileW + kGutter),
            kBarTop,
            tileW,
            kBarHeight,
            desktops[i] == current,
        });
    }
}

void OverviewEffectV2::renderDesktopBar(VkCommandBuffer cmd)
{
    if (m_barTiles.empty() || m_vkPipelineLayout == VK_NULL_HANDLE) {
        return;
    }
    // The pipeline is already bound, descriptor set 0 already has an
    // atlas-or-fallback image bound from the window-tile passes (the
    // shader doesn't sample it when tintRgba.a == 1, but Vulkan still
    // requires a valid binding). Viewport+scissor are also set to the
    // full framebuffer; bar tiles draw at their own NDC rects.
    const float factor = float(std::clamp(m_activationFactor, 0.0, 1.0));
    for (const BarTile &b : m_barTiles) {
        OverviewQuadPushConstants pc{};
        pc.quadRectNdc[0] = b.ndcX;
        pc.quadRectNdc[1] = b.ndcY;
        pc.quadRectNdc[2] = b.ndcW;
        pc.quadRectNdc[3] = b.ndcH;
        // atlasSlotUv unused when tintRgba.a == 1, but keep it sane.
        pc.atlasSlotUv[0] = 0.0f;
        pc.atlasSlotUv[1] = 0.0f;
        pc.atlasSlotUv[2] = 1.0f;
        pc.atlasSlotUv[3] = 1.0f;
        // Bright translucent white for the current desktop, dimmer
        // grey for the rest. The .a field is the tint mix weight
        // (1.0 = pure solid), not the final alpha — that comes from
        // pc.opacity below.
        if (b.isCurrent) {
            pc.tintRgba[0] = 0.95f;
            pc.tintRgba[1] = 0.95f;
            pc.tintRgba[2] = 1.0f;
        } else {
            pc.tintRgba[0] = 0.35f;
            pc.tintRgba[1] = 0.35f;
            pc.tintRgba[2] = 0.4f;
        }
        pc.tintRgba[3] = 1.0f;
        // Translucent so the dimmed scene-capture background shows
        // through; ramps with activation so the bar fades in alongside
        // the tiles.
        pc.opacity = (b.isCurrent ? 0.7f : 0.55f) * factor;

        vkCmdPushConstants(cmd, m_vkPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
}

void OverviewEffectV2::renderWindowsToAtlas()
{
    if (m_windowSlots.empty() || !m_atlas || !m_vulkanCtx) {
        return;
    }

    auto *scene = Compositor::self()->scene();
    if (!scene) {
        return;
    }
    auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(scene->renderer());
    if (!vkRenderer) {
        return;
    }

    // Lazy-build the shared atlas render pass. Reused for fallback
    // framebuffers too — same color format and GENERAL layouts on both
    // sides, so render-pass compatibility holds.
    constexpr VkFormat kAtlasFormat = VK_FORMAT_R8G8B8A8_SRGB;
    if (!m_atlasRenderPass) {
        m_atlasRenderPass = VulkanRenderPass::createForAtlasWrite(m_vulkanCtx, kAtlasFormat);
        if (!m_atlasRenderPass) {
            qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: createForAtlasWrite failed";
            return;
        }
    }

    // Find one atlas-resident slot to learn the shared image+view (any
    // such slot points at the same atlas resources). If every slot is
    // a fallback we skip the atlas render pass entirely.
    VkImage atlasImage = VK_NULL_HANDLE;
    VkImageView atlasMipZero = VK_NULL_HANDLE;
    for (const auto &[_handle, slot] : m_windowSlots) {
        if (!slot.isFallback) {
            atlasImage = slot.image;
            atlasMipZero = slot.mipZeroView;
            break;
        }
    }
    const bool haveAtlasSlots = atlasImage != VK_NULL_HANDLE;
    if (haveAtlasSlots && !m_atlasFramebuffer) {
        m_atlasFramebuffer = VulkanFramebuffer::create(m_vulkanCtx, m_atlasRenderPass.get(),
                                                       atlasMipZero,
                                                       QSize(VulkanThumbnailAtlas::kAtlasSize,
                                                             VulkanThumbnailAtlas::kAtlasSize));
        if (!m_atlasFramebuffer) {
            qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: framebuffer wrap failed";
            return;
        }
        m_atlasFramebuffer->setColorImage(atlasImage);
    }

    // Wait on the previous overview-frame's atlas submit so the
    // offscreen streaming buffer region we're about to reuse is
    // GPU-finished. In steady state this is already signalled and the
    // call is a no-op.
    if (m_lastAtlasSubmit.isValid()) {
        m_vulkanCtx->waitForSubmit(m_lastAtlasSubmit);
        m_lastAtlasSubmit = VulkanSubmitHandle{};
    }

    VkCommandBuffer cmd = m_vulkanCtx->beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return;
    }

    // Route renderItem's vertex/uniform writes to the renderer's
    // offscreen-slot buffers so they don't collide with the in-flight
    // swapchain frame's streaming buffer (preFrameRender fires before
    // the swapchain fence is waited on, so the main-frame buffer may
    // still be in use by the GPU). pop restores the swapchain cursors
    // when we're done.
    vkRenderer->pushOffscreenSlot();

    VkClearValue clearVal{};

    if (haveAtlasSlots) {
        // Pre-pass barriers for atlas slots. The atlas image stays in
        // GENERAL throughout (see createForAtlasWrite); these are
        // memory barriers, not layout transitions.
        for (auto &[_handle, slot] : m_windowSlots) {
            if (slot.isFallback) {
                continue;
            }
            m_atlas->prepareForRenderTo(cmd, slot);
        }

        // Single render pass covering all atlas-slot writes. Each
        // slot's renderItem sets its own viewport + scissor so the GPU
        // only touches that slot's sub-rect.
        const VkRect2D fullArea{
            {0, 0},
            {uint32_t(VulkanThumbnailAtlas::kAtlasSize), uint32_t(VulkanThumbnailAtlas::kAtlasSize)},
        };
        m_atlasRenderPass->begin(cmd, m_atlasFramebuffer->framebuffer(), fullArea, &clearVal, 1);

        auto vkRT = std::make_unique<VulkanRenderTarget>(m_atlasFramebuffer.get());
        vkRT->setCommandBuffer(cmd);
        RenderTarget atlasTarget(vkRT.get());

        for (auto &[handle, slot] : m_windowSlots) {
            if (slot.isFallback || !handle) {
                continue;
            }
            const QRectF windowGeom = handle->visibleGeometry();
            if (windowGeom.isEmpty()) {
                continue;
            }

            // Y-flipped viewport matches the main render path (and
            // WindowThumbnailSource's existing GL path) so the
            // resulting mip-0 layout reads top-down.
            VkViewport vp{};
            vp.x = float(slot.rect.x());
            vp.y = float(slot.rect.y() + slot.rect.height());
            vp.width = float(slot.rect.width());
            vp.height = -float(slot.rect.height());
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D scissor{
                {int32_t(slot.rect.x()), int32_t(slot.rect.y())},
                {uint32_t(slot.rect.width()), uint32_t(slot.rect.height())},
            };
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            // No vertex-offset save/restore here: the offscreen slot's
            // cursor is meant to accumulate across these renderItem
            // calls so each slot's vertices land in a disjoint buffer
            // region. popOffscreenSlot restores the swapchain frame's
            // cursor at the end of the function.
            RenderViewport viewport(windowGeom, 1.0, atlasTarget);
            vkRenderer->renderItem(atlasTarget, viewport, handle->windowItem(),
                                   Scene::PAINT_WINDOW_TRANSFORMED, infiniteRegion(),
                                   WindowPaintData{});

            slot.hasContent = true;
        }

        m_atlasRenderPass->end(cmd);
    }

    // Fallback slots — one dedicated image per source. Each gets its
    // own framebuffer (cached on m_fallbackFramebuffers) and its own
    // begin/end on the shared render pass. Same Y-flipped viewport
    // convention as the atlas pass.
    for (auto &[handle, slot] : m_windowSlots) {
        if (!slot.isFallback || !handle) {
            continue;
        }
        const QRectF windowGeom = handle->visibleGeometry();
        if (windowGeom.isEmpty()) {
            continue;
        }

        auto &fb = m_fallbackFramebuffers[handle];
        if (!fb) {
            fb = VulkanFramebuffer::create(m_vulkanCtx, m_atlasRenderPass.get(),
                                           slot.mipZeroView, slot.rect.size());
            if (!fb) {
                qCWarning(KWIN_OVERVIEW_V2_LOG)
                    << "OverviewEffectV2: fallback framebuffer wrap failed for"
                    << handle->caption().left(40) << slot.rect.size();
                m_fallbackFramebuffers.erase(handle);
                continue;
            }
            fb->setColorImage(slot.image);
        }

        m_atlas->prepareForRenderTo(cmd, slot);

        const VkRect2D fullArea{
            {0, 0},
            {uint32_t(slot.rect.width()), uint32_t(slot.rect.height())},
        };
        m_atlasRenderPass->begin(cmd, fb->framebuffer(), fullArea, &clearVal, 1);

        auto vkRT = std::make_unique<VulkanRenderTarget>(fb.get());
        vkRT->setCommandBuffer(cmd);
        RenderTarget fallbackTarget(vkRT.get());

        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = float(slot.rect.height());
        vp.width = float(slot.rect.width());
        vp.height = -float(slot.rect.height());
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        const VkRect2D scissor{
            {0, 0},
            {uint32_t(slot.rect.width()), uint32_t(slot.rect.height())},
        };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        RenderViewport viewport(windowGeom, 1.0, fallbackTarget);
        vkRenderer->renderItem(fallbackTarget, viewport, handle->windowItem(),
                               Scene::PAINT_WINDOW_TRANSFORMED, infiniteRegion(),
                               WindowPaintData{});

        m_atlasRenderPass->end(cmd);

        slot.hasContent = true;
    }

    // Mip cascade + publishing barrier for every slot — atlas and
    // fallback share the same prepare/publish API. Without this the
    // post-pass would sample undefined memory for tiles drawn at less
    // than 1:1 (mip > 0).
    for (auto &[_handle, slot] : m_windowSlots) {
        m_atlas->generateMipsAndPublish(cmd, slot);
    }

    vkRenderer->popOffscreenSlot();

    // Async submit; the wait at the top of the next frame's call
    // synchronises on the offscreen-slot buffer reuse. The main
    // compositor submit on the same queue picks up the atlas writes
    // through the publishing barrier's same-queue visibility.
    m_lastAtlasSubmit = m_vulkanCtx->submitSingleTimeCommandsAsync(cmd);
}

void OverviewEffectV2::drawSceneCaptureBackground(VkCommandBuffer cmd, VulkanTexture *sceneCapture,
                                                  const QSize &fbSize, VkFormat colorFormat)
{
    if (!sceneCapture || !m_vulkanCtx || fbSize.isEmpty()) {
        return;
    }
    // We borrow the atlas's sampler for sceneCapture sampling; if the
    // atlas isn't initialised (reserveSlots bailed) skip the background
    // pass — the tile pass will skip too and the user will see whatever
    // garbage LOAD_OP_DONT_CARE left, but that's better than a crash.
    if (!m_atlas) {
        return;
    }
    if (!ensureVulkanPipeline(m_vulkanCtx, colorFormat)) {
        return;
    }

    auto *backend = m_vulkanCtx->backend();
    auto pushDescriptor = backend->cmdPushDescriptorSetKHR();
    if (!pushDescriptor) {
        return;
    }

    // Reuse the tile pipeline: same fragment shader does `texture(s, uv) *
    // opacity`; sampler doesn't care that the bound image has only one
    // mip level. The scene capture texture is in
    // SHADER_READ_ONLY_OPTIMAL coming into this pass (the renderer
    // transitions it before invoking the callback), so a GENERAL
    // descriptor layout assertion would be wrong here — use the
    // texture's currentLayout instead.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipeline);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = m_atlas ? m_atlas->sampler() : VK_NULL_HANDLE;
    imgInfo.imageView = sceneCapture->imageView();
    imgInfo.imageLayout = sceneCapture->currentLayout();
    if (imgInfo.sampler == VK_NULL_HANDLE || imgInfo.imageView == VK_NULL_HANDLE) {
        return;
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    pushDescriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipelineLayout, 0, 1, &write);

    VkViewport vp{0.0f, 0.0f, float(fbSize.width()), float(fbSize.height()), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {uint32_t(fbSize.width()), uint32_t(fbSize.height())}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Fullscreen NDC quad, full-texture UVs. The captured scene is
    // dimmed as the activation animation progresses so tiles stand
    // out against a darkened backdrop — full brightness at factor=0
    // (matches the moment of activation) and ~30% at factor=1 (the
    // settled overview state). The letterbox gap around aspect-
    // preserving tiles reads as a darker background rather than the
    // live scene bleeding through. Premultiplied output means
    // multiplying opacity scales rgb and a together.
    constexpr float kSettledDim = 0.3f;
    const float factor = float(std::clamp(m_activationFactor, 0.0, 1.0));
    const float bgOpacity = 1.0f - factor * (1.0f - kSettledDim);
    OverviewQuadPushConstants pc{};
    pc.quadRectNdc[0] = -1.0f;
    pc.quadRectNdc[1] = -1.0f;
    pc.quadRectNdc[2] = 2.0f;
    pc.quadRectNdc[3] = 2.0f;
    pc.atlasSlotUv[0] = 0.0f;
    pc.atlasSlotUv[1] = 0.0f;
    pc.atlasSlotUv[2] = 1.0f;
    pc.atlasSlotUv[3] = 1.0f;
    pc.opacity = bgOpacity;
    vkCmdPushConstants(cmd, m_vkPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
}

void OverviewEffectV2::renderTilesPostPass(VkCommandBuffer cmd, const QSize &fbSize, VkFormat colorFormat)
{
    if (!m_atlas || m_windowSlots.empty() || !m_vulkanCtx) {
        return;
    }
    if (!ensureVulkanPipeline(m_vulkanCtx, colorFormat)) {
        return;
    }

    auto *backend = m_vulkanCtx->backend();
    auto pushDescriptor = backend->cmdPushDescriptorSetKHR();
    if (!pushDescriptor) {
        // Pipeline was created with the push-descriptor flag in
        // ensureVulkanPipeline; on hardware without the extension that
        // flag would have failed earlier. Guard here for safety.
        return;
    }

    // Refresh both layout caches each frame: window geometries can
    // shift (e.g. on resize during overview), and the hit-test must
    // consume the same numbers we draw with — so they share
    // `m_tileLayout` and `m_barTiles`. Each tile lerps from its real
    // on-screen rect to a grid cell; the bar sits at the top.
    rebuildTileLayout(fbSize);
    rebuildBarLayout(fbSize);
    if (m_tileLayout.empty() && m_barTiles.empty()) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipeline);
    VkViewport vp{0.0f, 0.0f, float(fbSize.width()), float(fbSize.height()), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {uint32_t(fbSize.width()), uint32_t(fbSize.height())}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const float atlasSize = float(VulkanThumbnailAtlas::kAtlasSize);
    const float factor = float(std::clamp(m_activationFactor, 0.0, 1.0));

    auto pushView = [&](VkImageView view) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = m_atlas->sampler();
        imgInfo.imageView = view;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imgInfo;
        pushDescriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipelineLayout, 0, 1, &write);
    };

    auto pushAndDraw = [&](const TileLayout &t, float uvX, float uvY, float uvW, float uvH) {
        OverviewQuadPushConstants pc{};
        pc.quadRectNdc[0] = t.realNdcX + (t.gridNdcX - t.realNdcX) * factor;
        pc.quadRectNdc[1] = t.realNdcY + (t.gridNdcY - t.realNdcY) * factor;
        pc.quadRectNdc[2] = t.realNdcW + (t.gridNdcW - t.realNdcW) * factor;
        pc.quadRectNdc[3] = t.realNdcH + (t.gridNdcH - t.realNdcH) * factor;
        pc.atlasSlotUv[0] = uvX;
        pc.atlasSlotUv[1] = uvY;
        pc.atlasSlotUv[2] = uvW;
        pc.atlasSlotUv[3] = uvH;
        pc.opacity = factor;
        vkCmdPushConstants(cmd, m_vkPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    };

    // Pass 1: atlas-resident tiles share the singleton atlas view.
    // One descriptor push, then a batch of draws.
    VkImageView atlasSrgbView = VK_NULL_HANDLE;
    for (const TileLayout &t : m_tileLayout) {
        if (!t.slot.isFallback) {
            atlasSrgbView = t.slot.srgbView;
            break;
        }
    }
    if (atlasSrgbView != VK_NULL_HANDLE) {
        pushView(atlasSrgbView);
        for (const TileLayout &t : m_tileLayout) {
            if (t.slot.isFallback) {
                continue;
            }
            pushAndDraw(t,
                        float(t.slot.rect.x()) / atlasSize,
                        float(t.slot.rect.y()) / atlasSize,
                        float(t.slot.rect.width()) / atlasSize,
                        float(t.slot.rect.height()) / atlasSize);
        }
    }

    // Pass 2: fallback tiles each have their own dedicated image, so
    // re-push the descriptor per draw. UV is the full texture (the
    // slot owns the entire image). With typical fallback counts in the
    // single digits the per-tile descriptor switch is negligible.
    for (const TileLayout &t : m_tileLayout) {
        if (!t.slot.isFallback) {
            continue;
        }
        pushView(t.slot.srgbView);
        pushAndDraw(t, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    // Pass 3: desktop bar. Solid-colour tiles (tintRgba.a == 1 in the
    // shader). The pipeline still requires a valid descriptor binding
    // even though the shader doesn't sample it for this path — reuse
    // whatever view we already bound from a tile pass. With no atlas
    // slots and no fallbacks we have nothing valid to bind, so skip
    // the bar; the user can fall back to the global Super+arrow
    // shortcuts in that (unusual) case.
    if (!m_barTiles.empty()) {
        VkImageView anyView = atlasSrgbView;
        if (anyView == VK_NULL_HANDLE) {
            for (const TileLayout &t : m_tileLayout) {
                if (t.slot.srgbView != VK_NULL_HANDLE) {
                    anyView = t.slot.srgbView;
                    break;
                }
            }
        }
        if (anyView != VK_NULL_HANDLE) {
            pushView(anyView);
            renderDesktopBar(cmd);
        }
    }
}
#endif // HAVE_VULKAN

void OverviewEffectV2::activate()
{
    if (m_visible && m_animation.direction() == QVariantAnimation::Forward) {
        return;
    }
    m_visible = true;
    // Capture mouse + keyboard. The keyboard grab routes through
    // grabbedKeyboardEvent (Esc-to-dismiss); the mouse interception
    // sends clicks through windowInputMouseEvent (tile hit-test or
    // click-outside dismiss).
    effects->startMouseInterception(this, Qt::ArrowCursor);
    effects->grabKeyboard(this);
#if HAVE_VULKAN
    reserveSlotsForCurrentDesktop();
    // Subscribe to preFrameRender so the atlas re-renders per frame
    // (per-window dirty tracking is a later optimisation). Bail out
    // gracefully if scene access fails — Phase 2c will gate behaviour
    // on this anyway.
    if (auto *scene = Compositor::self()->scene()) {
        m_preFrameConnection = connect(scene, &WorkspaceScene::preFrameRender,
                                       this, &OverviewEffectV2::renderWindowsToAtlas);
        // Register the fullscreen post-pass that actually draws the
        // window tiles on top of the rendered scene. Lifetime extends
        // through the slide-out animation so fading tiles stay visible
        // until activationFactor reaches zero (unregistered in the
        // animation-finished handler).
        if (auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(scene->renderer())) {
            m_postPassId = vkRenderer->registerFullscreenPostPass(
                [this](VkCommandBuffer cmd, VulkanTexture *sceneCapture,
                       const RenderTarget &target, const RenderViewport & /*viewport*/) {
                auto *vkTarget = target.vulkanTarget();
                if (!vkTarget) {
                    return;
                }
                const QSize fbSize = target.size();
                // Pull the swapchain colour format from the bound
                // framebuffer so the pipeline cache key in
                // ensureVulkanPipeline matches the active render
                // pass — Vulkan only requires format + sample-count
                // for pipeline / render-pass compatibility.
                VkFormat fmt = m_vulkanCtx
                    ? m_vulkanCtx->backend()->colorFormat()
                    : VK_FORMAT_UNDEFINED;
                // Background pass first: the post-FX render pass uses
                // LOAD_OP_DONT_CARE so every pixel outside a tile rect
                // would be undefined garbage. Sample the scene capture
                // fullscreen via the same pipeline (texture changes,
                // shader doesn't).
                drawSceneCaptureBackground(cmd, sceneCapture, fbSize, fmt);
                renderTilesPostPass(cmd, fbSize, fmt);
            });
        }
    }
#endif
    m_animation.stop();
    m_animation.setDirection(QVariantAnimation::Forward);
    m_animation.start();
}

void OverviewEffectV2::deactivate()
{
    if (!m_visible) {
        return;
    }
#if HAVE_VULKAN
    // Stop scheduling atlas writes as soon as deactivation starts; the
    // slot resources are released only after the slide-out animation
    // completes (the existing m_animation.finished slot handles that).
    QObject::disconnect(m_preFrameConnection);
    m_preFrameConnection = {};
#endif
    // Release input grabs at the start of deactivate; the user can
    // immediately interact with normal windows again while the slide-
    // out animation finishes drawing.
    effects->stopMouseInterception(this);
    effects->ungrabKeyboard();
    m_animation.stop();
    m_animation.setDirection(QVariantAnimation::Backward);
    m_animation.start();
}

void OverviewEffectV2::teardownImmediate()
{
    if (!m_visible) {
        return;
    }
    m_animation.stop();
#if HAVE_VULKAN
    QObject::disconnect(m_preFrameConnection);
    m_preFrameConnection = {};
#endif
    effects->stopMouseInterception(this);
    effects->ungrabKeyboard();
#if HAVE_VULKAN
    if (m_postPassId != -1) {
        if (auto *scene = Compositor::self()->scene()) {
            if (auto *vk = dynamic_cast<ItemRendererVulkan *>(scene->renderer())) {
                vk->unregisterFullscreenPostPass(m_postPassId);
            }
        }
        m_postPassId = -1;
    }
    releaseAllSlots();
#endif
    m_activationFactor = 0.0;
    m_visible = false;
    effects->addRepaintFull();
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
    if (event->type() != QEvent::KeyPress) {
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        deactivate();
        return;
    }
    // The compositor's keyboard grab swallows key presses before
    // KGlobalAccel can match them, so the user's configured toggle
    // (default Super+W) would otherwise never fire while the overview
    // is up. Match it here and dismiss explicitly. Modifier-only
    // presses (Super alone, etc.) carry the modifier in key() too —
    // skip them so we only react on the full combination.
    if (!m_toggleAction || event->key() == Qt::Key_Meta
        || event->key() == Qt::Key_Control || event->key() == Qt::Key_Alt
        || event->key() == Qt::Key_Shift) {
        return;
    }
    const QKeySequence pressed(event->modifiers() | event->key());
    const auto shortcuts = KGlobalAccel::self()->shortcut(m_toggleAction);
    for (const QKeySequence &s : shortcuts) {
        if (s == pressed) {
            deactivate();
            return;
        }
    }
}

void OverviewEffectV2::windowInputMouseEvent(QEvent *event)
{
    if (!m_visible || event->type() != QEvent::MouseButtonPress) {
        return;
    }
    auto *mouseEvent = dynamic_cast<QMouseEvent *>(event);
    if (!mouseEvent || mouseEvent->button() != Qt::LeftButton) {
        return;
    }
    // Hold off clicks until the slide-in animation has substantially
    // settled — otherwise the user clicks a tile that's still
    // animating from its real position and the hit-test against the
    // grid position misses. 0.95 is a comfortable threshold; the user
    // can't perceive the last 5% of the OutCubic anyway.
    if (m_activationFactor < 0.95) {
        return;
    }
    const QPoint pos = mouseEvent->globalPosition().toPoint();
#if HAVE_VULKAN
    // Bar hit-test first — it sits over the top edge of the screen and
    // its rects are smaller targets than the grid tiles. Convert click
    // to NDC the same way the post-pass does so geometry matches.
    if (!m_barTiles.empty() && effects) {
        const QRect screen = effects->virtualScreenGeometry();
        if (!screen.isEmpty() && screen.contains(pos)) {
            const float screenW = std::max(1.0f, float(screen.width()));
            const float screenH = std::max(1.0f, float(screen.height()));
            const float mxNdc = (float(pos.x() - screen.x()) / screenW) * 2.0f - 1.0f;
            const float myNdc = (float(pos.y() - screen.y()) / screenH) * 2.0f - 1.0f;
            for (const BarTile &b : m_barTiles) {
                if (mxNdc >= b.ndcX && mxNdc <= b.ndcX + b.ndcW
                    && myNdc >= b.ndcY && myNdc <= b.ndcY + b.ndcH) {
                    // Tear down V2 *synchronously* before switching
                    // desktops. The animated slide-out raced with
                    // setCurrentDesktop's side effects (VD-switch OSD,
                    // fadedesktop, KGlobalAccel state) and the
                    // post-pass kept firing into the race — the next
                    // activate would draw the VD OSD zoomed in
                    // instead of windows, and Super+W silently broke
                    // until something else (e.g. the VD plasmoid)
                    // reset shortcut state. Snap everything to the
                    // dormant state, then switch.
                    VirtualDesktop *target = (b.desktop && b.desktop != effects->currentDesktop())
                        ? b.desktop
                        : nullptr;
                    teardownImmediate();
                    if (target) {
                        effects->setCurrentDesktop(target);
                    }
                    return;
                }
            }
        }
    }
#endif
    if (Window *target = hitTestTile(pos)) {
        if (target->effectWindow()) {
            effects->activateWindow(target->effectWindow());
        }
        deactivate();
        return;
    }
    // Click outside any tile or bar: dismiss the overview (matches the
    // QML overview's TapHandler on the underlay).
    deactivate();
}

Window *OverviewEffectV2::hitTestTile(const QPoint &globalPos) const
{
#if HAVE_VULKAN
    if (m_tileLayout.empty() || !effects) {
        return nullptr;
    }
    const QRect screen = effects->virtualScreenGeometry();
    if (screen.isEmpty() || !screen.contains(globalPos)) {
        return nullptr;
    }
    // Read the cached layout the post-pass built this frame — same grid
    // cells, same ordering, no recomputation. virtualScreenGeometry is
    // in scene coordinates; convert the click to NDC relative to its
    // top-left.
    const float screenW = std::max(1.0f, float(screen.width()));
    const float screenH = std::max(1.0f, float(screen.height()));
    const float mxNdc = (float(globalPos.x() - screen.x()) / screenW) * 2.0f - 1.0f;
    const float myNdc = (float(globalPos.y() - screen.y()) / screenH) * 2.0f - 1.0f;
    for (const TileLayout &t : m_tileLayout) {
        if (mxNdc >= t.gridNdcX && mxNdc <= t.gridNdcX + t.gridNdcW
            && myNdc >= t.gridNdcY && myNdc <= t.gridNdcY + t.gridNdcH) {
            return t.handle;
        }
    }
#endif
    Q_UNUSED(globalPos)
    return nullptr;
}

} // namespace KWin

#include "moc_overvieweffectv2.cpp"
