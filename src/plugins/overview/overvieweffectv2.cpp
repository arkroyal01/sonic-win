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
#include "platformsupport/scenes/vulkan/vulkankawaseblur.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "platformsupport/scenes/vulkan/vulkanthumbnailatlas.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene.h"
#include "utils/common.h"
#include "virtualdesktops.h"
#include "window.h"
#endif

#include <cmath>

#include <KConfigGroup>
#include <KGlobalAccel>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QAction>
#include <QEasingCurve>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

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
    // Explicit mip LOD for textureLod sampling. 0 = use implicit
    // texture() (LOD 0 for a fullscreen quad). Higher values produce
    // a mipmap-based blur — see overview_quad.frag.
    float lod;
    // Per-quad rounded-corner radius in UV space, separately on each
    // axis (radius_px / half_quad_px on X and Y). The shader runs a
    // rounded-rect SDF discard against this; both components zero
    // disables the path entirely so full-screen passes (wash,
    // wallpaper backdrop) stay rectangular. Per-axis values mean
    // non-square quads draw an elliptical corner that traces the
    // intended pixel-radius even when the quad's NDC aspect differs
    // from its pixel aspect. Helper setCornerRadiusUv() below.
    float cornerRadiusUv[2];
    // Soft-edge fade distance in the SDF's normalised radius units.
    // Zero = hard discard at the rounded boundary (the default for
    // tiles). Positive = smoothstep alpha falloff from the boundary
    // outward, used for soft drop shadows. Paired with a quad that
    // extends beyond the surface being shadowed so the falloff has
    // room to taper.
    float edgeSoftnessUv;
    // Pad to 16-byte alignment so the struct size matches the shader
    // std430 push-constant block size deterministically.
    float _pad;
    // Soft-shadow inset (proper rounded-box SDF mode). When both
    // components are non-zero, the shader switches from the simple
    // elliptical-corner-discard path to a real signed-distance
    // rounded-box. shadowInnerHalfUv is the inner solid rect's
    // half-extent in the quad's local UV (so values < 1.0 leave a
    // ring of soft alpha between the inner edge and the quad edge).
    // shadowCornerRadiusUv is the corner radius of that inner rect
    // in the same UV. edgeSoftnessUv controls the fade distance
    // outside the inner boundary. Disables the elliptical-corner
    // path when active so a draw can't accidentally do both.
    float shadowInnerHalfUv[2];
    float shadowCornerRadiusUv;
    float _pad2;
};
static_assert(sizeof(OverviewQuadPushConstants) == 88,
              "Push-constant layout must match shaders/overview_quad.{vert,frag}");

// Convert a desired pixel-space corner radius into the per-axis UV
// values OverviewQuadPushConstants::cornerRadiusUv expects. The shader
// evaluates an elliptical SDF whose semi-axes are these two values,
// chosen so a non-square pixel quad still draws a circular pixel-space
// corner of size radiusPx. Clamped to [0, 1] because anything larger
// would degenerate the SDF (the rect becomes all-corner).
inline void setCornerRadiusUv(OverviewQuadPushConstants &pc,
                              const QSize &fbSize, float radiusPx)
{
    const float halfWpx = std::max(1.0f,
                                   std::abs(pc.quadRectNdc[2]) * 0.5f * float(fbSize.width()));
    const float halfHpx = std::max(1.0f,
                                   std::abs(pc.quadRectNdc[3]) * 0.5f * float(fbSize.height()));
    pc.cornerRadiusUv[0] = std::clamp(radiusPx / halfWpx, 0.0f, 1.0f);
    pc.cornerRadiusUv[1] = std::clamp(radiusPx / halfHpx, 0.0f, 1.0f);
}

// Corner radii expressed as gridUnit fractions so they scale with the
// user's font / DPI setting (V1's Kirigami.Units.cornerRadius derives
// from smallSpacing which is itself ~gridUnit/4). Multiplied by
// QFontMetrics(qApp->font()).height() at each call site via the
// roundedRadiusPx helper to get the pixel value setCornerRadiusUv
// expects. Conservative defaults approximate V1's modest rounding.
constexpr float kRoundedRadiusCardFrac = 0.5f; // wallpaper card -- ~gridUnit/2
constexpr float kRoundedRadiusTileFrac = 0.3f; // grid tiles, bar tiles
constexpr float kRoundedRadiusMiniFrac = 0.15f; // bar mini-thumbnails

inline float roundedRadiusPx(float gridUnitFrac)
{
    return gridUnitFrac * float(QFontMetrics(qApp->font()).height());
}

// Fill a push-constants struct with a Breeze-style soft drop shadow
// for an inner rounded rect at (innerLeftNdc, innerTopNdc) of size
// (innerWNdc, innerHNdc). Uses the shader's rounded-box SDF mode --
// shadowInnerHalfUv + shadowCornerRadiusUv + edgeSoftnessUv path --
// so the alpha smoothsteps to zero outside the inner boundary
// (Gaussian-like falloff in one fragment per pixel).
//   spreadPx   = how far the shadow extends past the inner rect
//                on each side (= softness distance)
//   yOffsetPx  = downward offset of the shadow's centre vs the inner
//                rect's centre (makes the shadow sit below the
//                inner rect, like a real drop shadow)
//   radiusPx   = inner rect's corner radius
//   alpha      = peak alpha of the shadow (typically 0.25-0.55)
inline void setupShadowPc(OverviewQuadPushConstants &pc, const QSize &fbSize,
                          float innerLeftNdc, float innerTopNdc,
                          float innerWNdc, float innerHNdc,
                          float radiusPx, float spreadPx, float yOffsetPx,
                          float alpha)
{
    const float screenW = std::max(1.0f, float(fbSize.width()));
    const float screenH = std::max(1.0f, float(fbSize.height()));
    const float innerHalfWPx = innerWNdc * screenW * 0.25f;
    const float innerHalfHPx = innerHNdc * screenH * 0.25f;
    const float quadHalfPxX = innerHalfWPx + spreadPx;
    const float quadHalfPxY = innerHalfHPx + spreadPx;
    const float spreadNdcX = 2.0f * spreadPx / screenW;
    const float spreadNdcY = 2.0f * spreadPx / screenH;
    const float yOffsetNdc = 2.0f * yOffsetPx / screenH;

    pc.quadRectNdc[0] = innerLeftNdc - spreadNdcX;
    pc.quadRectNdc[1] = innerTopNdc + yOffsetNdc - spreadNdcY;
    pc.quadRectNdc[2] = innerWNdc + 2.0f * spreadNdcX;
    pc.quadRectNdc[3] = innerHNdc + 2.0f * spreadNdcY;
    pc.atlasSlotUv[0] = 0.0f;
    pc.atlasSlotUv[1] = 0.0f;
    pc.atlasSlotUv[2] = 1.0f;
    pc.atlasSlotUv[3] = 1.0f;
    pc.tintRgba[3] = 1.0f; // solid tint (rgb stays 0 → black)
    pc.opacity = alpha;
    pc.shadowInnerHalfUv[0] = innerHalfWPx / quadHalfPxX;
    pc.shadowInnerHalfUv[1] = innerHalfHPx / quadHalfPxY;
    pc.shadowCornerRadiusUv = radiusPx / std::max(1.0f, quadHalfPxX);
    pc.edgeSoftnessUv = spreadPx / std::max(1.0f, quadHalfPxX);
}
} // namespace

#endif // HAVE_VULKAN

namespace KWin
{

bool OverviewEffectV2::supported()
{
    // V2 is the default for dogfooding; the QML OverviewEffect only
    // takes the shortcut back when the user sets KWIN_OVERVIEW_V2=0
    // (mirrored gate in ../main.cpp).
    if (qEnvironmentVariable("KWIN_OVERVIEW_V2", QStringLiteral("1")).toInt() == 0) {
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
        // Only the slide-out finish should release resources. Use the
        // animation direction rather than the activation-factor value;
        // qFuzzyCompare against 0.0 is unreliable — Qt's epsilon is
        // relative, so a value of 5e-17 from float rounding fails the
        // compare and leaves m_visible = true forever, which makes the
        // next corner / shortcut trigger no-op as if V2 were still up.
        if (m_animation.direction() == QVariantAnimation::Backward) {
            m_visible = false;
#if HAVE_VULKAN
            // Stop scheduling atlas writes. Deactivate() used to do
            // this at the START of the slide-out, but that froze the
            // window thumbnails for the duration of the animation —
            // the atlas slots stopped re-rendering, so any window
            // showing live content (video, animation, terminal cursor)
            // turned static the moment the user triggered exit. Keep
            // preFrameRender connected through the lerp; disconnect it
            // here, the instant the animation completes, alongside the
            // post-pass unregister + slot release.
            QObject::disconnect(m_preFrameConnection);
            m_preFrameConnection = {};
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
        // From GridView, Meta+W switches back to Overview mode rather
        // than dismissing -- matches V1's "Cycle Overview" semantics
        // for the canonical overview toggle.
        if (m_visible && m_mode == Mode::GridView) {
            activate(Mode::Overview);
        } else if (m_visible) {
            deactivate();
        } else {
            activate(Mode::Overview);
        }
    });

    // Grid View action. V1 binds Super+G to the "Grid View" action
    // (object name picked up by Plasma's shortcut UI). Same toggle
    // semantics: pressed while showing GridView -> dismiss; pressed
    // in Overview -> switch to GridView; pressed when hidden ->
    // activate in GridView.
    m_gridViewAction = new QAction(this);
    m_gridViewAction->setObjectName(QStringLiteral("Grid View"));
    m_gridViewAction->setText(i18nc("@action Grid View is the name of a Kwin overview mode",
                                    "Toggle Grid View"));
    m_gridViewAction->setAutoRepeat(false);
    const QKeySequence gridShortcut = Qt::META | Qt::Key_G;
    KGlobalAccel::self()->setDefaultShortcut(m_gridViewAction, {gridShortcut});
    KGlobalAccel::self()->setShortcut(m_gridViewAction, {gridShortcut});
    connect(m_gridViewAction, &QAction::triggered, this, [this]() {
        if (m_visible && m_mode == Mode::GridView) {
            deactivate();
        } else {
            activate(Mode::GridView);
        }
    });

    // WindowClass mode (default Ctrl+F7) — present every window sharing the
    // active window's class across all desktops. The stock windowview effect
    // owns "ExposeClass"/Ctrl+F7; this fork unbinds it (see
    // windowvieweffect.cpp) so V2 takes over with this Vulkan grid + search
    // reimplementation. Distinct object name to avoid a KGlobalAccel
    // action-name collision with windowview's still-registered action.
    m_exposeClassAction = new QAction(this);
    m_exposeClassAction->setObjectName(QStringLiteral("OverviewWindowClass"));
    m_exposeClassAction->setText(i18nc("@action", "Toggle Present Windows (Current Application)"));
    m_exposeClassAction->setAutoRepeat(false);
    const QKeySequence classShortcut = Qt::CTRL | Qt::Key_F7;
    KGlobalAccel::self()->setDefaultShortcut(m_exposeClassAction, {classShortcut});
    // NoAutoloading: force Ctrl+F7 onto this action regardless of the saved
    // config. Effects load alphabetically, so this ("overviewv2") registers
    // before windowview clears its ExposeClass binding — with plain
    // autoloading KGlobalAccel sees ExposeClass still holding Ctrl+F7,
    // assigns us an empty shortcut and *persists* the empty, so the binding
    // silently dies on every start. Forcing it steals the key from the
    // (about-to-be-cleared) ExposeClass and overwrites the poisoned entry.
    KGlobalAccel::self()->setShortcut(m_exposeClassAction, {classShortcut},
                                      KGlobalAccel::NoAutoloading);
    connect(m_exposeClassAction, &QAction::triggered, this, [this]() {
        if (m_visible && m_mode == Mode::WindowClass) {
            deactivate();
        } else {
            activate(Mode::WindowClass);
        }
    });

    // Touchpad + touchscreen gesture activation. V1 binds 4-finger
    // touchpad swipe up and 3-finger touchscreen swipe up to
    // activate the overview, with the opposite direction to
    // deactivate. We mirror those bindings as binary triggers (no
    // progress-driven slide-in yet — see m_swipeActivateAction
    // doc). QActions live for the effect's lifetime; the gesture
    // API holds them by pointer.
    m_swipeActivateAction = new QAction(this);
    connect(m_swipeActivateAction, &QAction::triggered, this, [this]() {
        if (!m_visible) {
            activate();
        }
    });
    m_swipeDeactivateAction = new QAction(this);
    connect(m_swipeDeactivateAction, &QAction::triggered, this, [this]() {
        if (m_visible) {
            deactivate();
        }
    });
    effects->registerTouchpadSwipeShortcut(SwipeDirection::Up, 4, m_swipeActivateAction, {});
    effects->registerTouchpadSwipeShortcut(SwipeDirection::Down, 4, m_swipeDeactivateAction, {});
    effects->registerTouchscreenSwipeShortcut(SwipeDirection::Up, 3, m_swipeActivateAction, {});
    effects->registerTouchscreenSwipeShortcut(SwipeDirection::Down, 3, m_swipeDeactivateAction, {});

    // Pick up the same config the V1 overview uses (group
    // "Effect-overview" in kwinrc) so users get their saved
    // electric-border / config-knob preferences when V2 takes over.
    reconfigure(ReconfigureAll);
}

OverviewEffectV2::~OverviewEffectV2()
{
    if (m_toggleAction) {
        KGlobalAccel::self()->removeAllShortcuts(m_toggleAction);
    }
    if (m_gridViewAction) {
        KGlobalAccel::self()->removeAllShortcuts(m_gridViewAction);
    }
    if (m_exposeClassAction) {
        KGlobalAccel::self()->removeAllShortcuts(m_exposeClassAction);
    }
    for (const ElectricBorder &border : std::as_const(m_borderActivate)) {
        effects->unreserveElectricBorder(border, this);
    }
    m_borderActivate.clear();
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
    m_kawaseBlur.reset();
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

bool OverviewEffectV2::ensureBlurPipelines()
{
    // Lazy-built once per V2 plugin lifetime via the shared dual-kawase
    // helper. The level textures are RGBA8 — the resulting blurred
    // wallpaper is sampled by the main overview pipeline afterwards.
    if (m_kawaseBlur) {
        return true;
    }
    if (!m_vulkanCtx) {
        return false;
    }
    m_kawaseBlur = VulkanKawaseBlur::create(m_vulkanCtx, VK_FORMAT_R8G8B8A8_UNORM);
    if (!m_kawaseBlur) {
        qCWarning(KWIN_OVERVIEW_V2_LOG) << "OverviewEffectV2: kawase blur helper create failed";
        return false;
    }
    return true;
}

bool OverviewEffectV2::ensureBlurScratch(const QSize &screenSize)
{
    if (screenSize.isEmpty() || !m_vulkanCtx || !m_kawaseBlur) {
        return false;
    }
    if (m_blurScratchScreenSize == screenSize && m_blurScratchLevels.size() == 2) {
        return true;
    }
    m_blurScratchLevels.clear();
    static constexpr VkFormat kBlurFmt = VK_FORMAT_R8G8B8A8_UNORM;
    // Two shared scratch levels at screen/8 and /16. The per-VD result
    // texture serves as level /4 (input to first downsample + final
    // destination of the last upsample), so we don't need a shared /4.
    for (int divisor : {8, 16}) {
        BlurLevel lvl;
        lvl.size = QSize(std::max(1, screenSize.width() / divisor),
                         std::max(1, screenSize.height() / divisor));
        lvl.texture = VulkanTexture::createRenderTarget(m_vulkanCtx, lvl.size, kBlurFmt);
        if (!lvl.texture || !lvl.texture->isValid()) {
            qCWarning(KWIN_OVERVIEW_V2_LOG)
                << "OverviewEffectV2: blur scratch level texture failed at" << lvl.size;
            m_blurScratchLevels.clear();
            return false;
        }
        lvl.view = lvl.texture->imageView();
        lvl.framebuffer = VulkanFramebuffer::create(m_vulkanCtx, m_kawaseBlur->renderPass(),
                                                    lvl.view, lvl.size);
        if (!lvl.framebuffer) {
            qCWarning(KWIN_OVERVIEW_V2_LOG)
                << "OverviewEffectV2: blur scratch level framebuffer failed at" << lvl.size;
            m_blurScratchLevels.clear();
            return false;
        }
        m_blurScratchLevels.push_back(std::move(lvl));
    }
    m_blurScratchScreenSize = screenSize;
    return true;
}

bool OverviewEffectV2::blurWallpaperSlot(VkCommandBuffer cmd, Window *handle,
                                         const VulkanThumbnailAtlas::Slot &slot)
{
    // One-shot per overview activation per wallpaper handle. The atlas
    // slot stores the sharp source; we (1) blit-copy + downscale it to
    // a per-VD /4 texture, (2) run kawase downsample /4→/8 and /8→/16
    // through shared scratch, then (3) run kawase upsample /16→/8 and
    // /8→/4 back into the per-VD texture. Final state of the per-VD
    // texture is the blurred wallpaper, in GENERAL, ready to be
    // sampled by drawWallpaperBackground in subsequent frames.
    if (!handle || !slot.isValid() || slot.isFallback) {
        return false;
    }
    if (!ensureBlurPipelines() || !m_vulkanCtx) {
        return false;
    }
    if (!effects) {
        return false;
    }
    const QRect virtualScreen = effects->virtualScreenGeometry();
    if (virtualScreen.isEmpty()) {
        return false;
    }
    if (!ensureBlurScratch(virtualScreen.size())) {
        return false;
    }
    static constexpr VkFormat kBlurFmt = VK_FORMAT_R8G8B8A8_UNORM;

    auto &entry = m_wallpaperBlurByHandle[handle];
    if (entry.generated && entry.texture && entry.texture->isValid()) {
        return true; // already blurred this activation
    }

    // Allocate the per-VD result texture on first use. Sized to /4 of
    // the screen — wallpaper backdrop is heavy blur, finer levels add
    // nothing visible.
    if (!entry.texture) {
        entry.size = QSize(std::max(1, virtualScreen.width() / 4),
                           std::max(1, virtualScreen.height() / 4));
        entry.texture = VulkanTexture::createRenderTarget(m_vulkanCtx, entry.size, kBlurFmt);
        if (!entry.texture || !entry.texture->isValid()) {
            qCWarning(KWIN_OVERVIEW_V2_LOG)
                << "OverviewEffectV2: wallpaper blur result texture failed at" << entry.size;
            return false;
        }
        entry.view = entry.texture->imageView();
        entry.framebuffer = VulkanFramebuffer::create(m_vulkanCtx, m_kawaseBlur->renderPass(),
                                                      entry.view, entry.size);
        if (!entry.framebuffer) {
            qCWarning(KWIN_OVERVIEW_V2_LOG)
                << "OverviewEffectV2: wallpaper blur framebuffer failed";
            entry.texture.reset();
            return false;
        }
    }

    auto undefinedToGeneral = [&](VkImage img) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT
                                 | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                 | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    };
    // First use: bring the per-VD result image and scratch images from
    // UNDEFINED to GENERAL. The scratch transition only needs to happen
    // the first time the scratch chain exists in this activation;
    // ensureBlurScratch leaves them undefined on fresh alloc.
    undefinedToGeneral(entry.texture->image());
    if (!m_blurScratchInitialised) {
        for (auto &lvl : m_blurScratchLevels) {
            undefinedToGeneral(lvl.texture->image());
        }
        m_blurScratchInitialised = true;
    }

    // Barrier so the atlas-pass writes to the slot are visible to the
    // transfer that blits out of the atlas image.
    {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // Step 1: vkCmdBlitImage(atlas slot rect) -> entry.texture (/4).
    {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {slot.rect.x(), slot.rect.y(), 0};
        blit.srcOffsets[1] = {slot.rect.x() + slot.rect.width(),
                              slot.rect.y() + slot.rect.height(), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {entry.size.width(), entry.size.height(), 1};
        vkCmdBlitImage(cmd, slot.image, VK_IMAGE_LAYOUT_GENERAL,
                       entry.texture->image(), VK_IMAGE_LAYOUT_GENERAL,
                       1, &blit, VK_FILTER_LINEAR);
    }

    // Make the blit's write to the per-VD /4 texture (levels[0]) visible to
    // the first downsample's shader read — recordPyramid's contract is that
    // the caller has published levels[0]'s initial contents.
    {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // Run the shared dual-kawase pyramid: /4 (per-VD result, levels[0]) ->
    // /8 -> /16 down, then back up into /4. The /4 texture is both the
    // source and the final destination; the two shared scratch levels are
    // /8 and /16. Positive-height viewport (the wallpaper source and result
    // are authored bottom-up) and the canonical offset of 1.0. The final
    // barrier recordPyramid inserts leaves levels[0] visible to the main
    // overview pipeline that samples it next frame.
    BlurLevel &l8 = m_blurScratchLevels[0]; // /8
    BlurLevel &l16 = m_blurScratchLevels[1]; // /16
    const std::vector<VulkanKawaseBlur::Level> levels{
        {entry.view, entry.framebuffer.get(), entry.size},
        {l8.view, l8.framebuffer.get(), l8.size},
        {l16.view, l16.framebuffer.get(), l16.size},
    };
    m_kawaseBlur->recordPyramid(cmd, levels, 1.0f, /*flipViewportY=*/false);

    entry.generated = true;
    return true;
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

    // Freeze the current global stacking order so the overlapping bar
    // mini-thumbnails keep a stable z-order for the whole activation,
    // independent of any live raise/lower during the animation.
    m_stackingIndex.clear();
    {
        int z = 0;
        for (EffectWindow *ew : effects->stackingOrder()) {
            if (ew && ew->window()) {
                m_stackingIndex.emplace(ew->window(), z++);
            }
        }
    }

    // Grid View doesn't draw the overview heap, so it needs no large
    // per-window slots. Overview and Grid View are mutually exclusive
    // (you're only ever in one), so skipping these here leaves the
    // whole atlas free for the high-resolution grid mini-thumbnails
    // that reserveBarThumbs sizes to the card — no separate atlas
    // image required, and nothing is held that the active mode won't
    // draw.
    if (m_mode == Mode::GridView) {
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
        if (!ew) {
            continue;
        }
        Window *handle = ew->window();
        if (!handle) {
            continue;
        }
        // Which windows belong in the heap depends on the mode:
        //  - WindowClass (Ctrl+F7): every VD's windows whose resourceClass
        //    matches the active window captured at activation. An empty
        //    filter (no active window / no class) matches nothing.
        //  - Overview: current-desktop windows only.
        if (m_mode == Mode::WindowClass) {
            if (m_windowClassFilter.isEmpty()
                || handle->resourceClass() != m_windowClassFilter) {
                continue;
            }
        } else if (!currentDesktop || !ew->isOnDesktop(currentDesktop)) {
            continue;
        }
        // Match V1's effective overview filter (overview/qml/main.qml
        // + WindowFilterModel) and add skipSwitcher() as the OSD
        // backstop:
        //
        // - isClient() — exclude X11 override-redirect / unmanaged
        //   windows. Plasma's VD-switch OSD is an unmanaged popup, so
        //   this is the one filter that catches it even when its
        //   windowType hint hasn't propagated yet (the race that
        //   isNormalWindow() loses).
        // - isNormalWindow() — the windowType bucket V1 accepts (V1
        //   also accepts Dialog; V2 stays stricter for now).
        // - skipSwitcher() — the semantic "don't show me in any
        //   window switcher / overview" flag. Plasma sets it on its
        //   transient popups; catches anything our type checks miss.
        // - dedicated transient-type predicates + readyForPainting:
        //   defence in depth against the race where windowType
        //   momentarily reports Normal before _NET_WM_WINDOW_TYPE is
        //   parsed.
        if (!handle->isClient() || !handle->isNormalWindow()
            || handle->skipSwitcher() || handle->isOnScreenDisplay()
            || handle->isNotification() || handle->isCriticalNotification()
            || handle->isTooltip() || handle->isComboBox()
            || handle->isDNDIcon() || handle->isPopupWindow()
            || !handle->readyForPainting()) {
            continue;
        }
        if (m_ignoreMinimized && handle->isMinimized()) {
            continue;
        }
        // Activity filter: only show windows on the current activity.
        // No-op when activities aren't enabled (isOnCurrentActivity
        // returns true for every window in that case). Mirrors V1's
        // implicit WindowFilterModel.activity binding.
        if (!ew->isOnCurrentActivity()) {
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
            // Clear the drag candidate if it was this window —
            // m_dragCandidate is a bare Window* and would dangle
            // through the next mouse release if the dragged window
            // dies mid-drag.
            if (m_dragCandidate == handle) {
                m_dragCandidate = nullptr;
                m_dragActive = false;
            }
            // No unrefOffscreenRendering here — the window is being
            // destroyed; KWin tears down its own ref tracking.
            m_windowSlots.erase(it);
        });
    }
}

void OverviewEffectV2::reserveWallpaperSlot()
{
    // Initialise m_vulkanCtx / m_atlas if they aren't yet — we may be
    // called before reserveSlotsForCurrentDesktop now that wallpaper
    // reservation runs first to keep the atlas defragmented.
    if (!effects || !effects->isVulkanCompositing()) {
        return;
    }
    if (!m_vulkanCtx || !m_atlas) {
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
    }
    // Already reserved (activity refresh re-entered before
    // releaseAllSlots ran). Release all entries first so we re-pick
    // the wallpapers for the new activity / desktops.
    for (auto &[handle, slot] : m_wallpaperSlotByHandle) {
        if (handle) {
            handle->unrefOffscreenRendering();
            m_atlas->release(slot);
        }
    }
    m_wallpaperSlotByHandle.clear();
    m_wallpaperHandleByVd.clear();

    const QRect screen = effects->virtualScreenGeometry();
    if (screen.isEmpty()) {
        return;
    }
    // Reserve a per-VD wallpaper slot so bar tiles can each sample
    // their own desktop's wallpaper (matches V1's per-tile DesktopView).
    // Plasma renders the wallpaper as a _NET_WM_WINDOW_TYPE_DESKTOP
    // X11 window per VD; isDesktop() + isOnDesktop(vd) is the canonical
    // pair.
    for (VirtualDesktop *vd : VirtualDesktopManager::self()->desktops()) {
        if (!vd) {
            continue;
        }
        for (EffectWindow *ew : effects->stackingOrder()) {
            if (!ew || !ew->isDesktop()) {
                continue;
            }
            Window *handle = ew->window();
            if (!handle || !handle->isClient() || !handle->readyForPainting()) {
                continue;
            }
            if (!ew->isOnDesktop(vd)) {
                continue;
            }
            if (!ew->isOnCurrentActivity()) {
                continue;
            }
            // Map VD → handle first; reserve a slot only if this is a
            // new handle (Plasma's wallpaper window is sticky-on-all-
            // desktops for users who keep one wallpaper, so the same
            // Window* serves every VD — no need to allocate N copies).
            m_wallpaperHandleByVd[vd] = handle;
            if (m_wallpaperSlotByHandle.count(handle)) {
                break;
            }
            auto slot = m_atlas->reserve(screen.size());
            if (!slot.isValid()) {
                qCWarning(KWIN_OVERVIEW_V2_LOG)
                    << "OverviewEffectV2: wallpaper slot reserve failed" << screen.size();
                m_wallpaperHandleByVd.erase(vd);
                continue;
            }
            handle->refOffscreenRendering();
            m_wallpaperSlotByHandle.emplace(handle, std::move(slot));
            connect(handle, &QObject::destroyed, this, [this, handle]() {
                auto it = m_wallpaperSlotByHandle.find(handle);
                if (it == m_wallpaperSlotByHandle.end()) {
                    return;
                }
                if (m_vulkanCtx && m_lastAtlasSubmit.isValid()) {
                    m_vulkanCtx->waitForSubmit(m_lastAtlasSubmit);
                    m_lastAtlasSubmit = VulkanSubmitHandle{};
                }
                if (m_atlas) {
                    m_atlas->release(it->second);
                }
                m_wallpaperSlotByHandle.erase(it);
                for (auto vdIt = m_wallpaperHandleByVd.begin();
                     vdIt != m_wallpaperHandleByVd.end();) {
                    vdIt = (vdIt->second == handle)
                        ? m_wallpaperHandleByVd.erase(vdIt)
                        : std::next(vdIt);
                }
            });
            break;
        }
    }
}

void OverviewEffectV2::reserveBarThumbs()
{
    if (!effects || !m_atlas || !m_vulkanCtx) {
        return;
    }
    // WindowClass mode draws no desktop bar and no per-VD grid cards, so it
    // needs none of the bar mini-thumbnails this reserves.
    if (m_mode == Mode::WindowClass) {
        return;
    }
    auto *currentDesktop = effects->currentDesktop();
    if (!currentDesktop) {
        return;
    }
    // Fixed-size slot per non-current-desktop window. 256×192 ≈ 200 KB
    // of mip-0 storage plus the mip cascade — a few MB total for tens
    // of windows, vs the ~750 MB the per-frame full-size approach
    // bloated kwin to. Each window's bar mini-thumbnail samples this
    // small slot; sampling at low LOD is still cheap (mip cascade is
    // built once when the snapshot is rendered).
    //
    // In Grid View each card is ~screen/gridSize and the window thumbs
    // are drawn large, so 256x192 reads blocky. Size the slot to the
    // card resolution instead (screen / gridSize). This is self-
    // balancing — more virtual desktops means a larger gridSize, hence
    // smaller cards and smaller thumbs — so atlas use stays bounded,
    // and Grid View has the whole atlas to itself anyway (the overview
    // heap's large slots are skipped in reserveSlotsForCurrentDesktop).
    QSize kBarThumbSize(256, 192);
    if (m_mode == Mode::GridView) {
        const int gridSize = std::max({1, effects->desktopGridWidth(),
                                       effects->desktopGridHeight()});
        const QRect vs = effects->virtualScreenGeometry();
        kBarThumbSize = QSize(std::max(256, vs.width() / gridSize),
                              std::max(192, vs.height() / gridSize));
    }

    for (EffectWindow *ew : effects->stackingOrder()) {
        if (!ew) {
            continue;
        }
        // A small bar thumb is reserved for EVERY window, including
        // current-desktop ones. The bar previously reused the large
        // m_windowSlots entry for current-desktop windows, but under
        // atlas pressure those large reservations spill to fallback
        // slots (dedicated images, not atlas-resident), which the bar
        // pass cannot sample with the shared atlas descriptor — so the
        // window silently vanished from the bar whenever its desktop
        // was current. A dedicated 256x192 atlas thumb always fits
        // (fallback grid slots don't consume atlas space), keeping the
        // bar robust and uniform across current and non-current tiles.
        const bool onCurrent = ew->isOnDesktop(currentDesktop);
        Window *handle = ew->window();
        if (!handle) {
            continue;
        }
        // Same filter as the main-desktop reservation.
        if (!handle->isClient() || !handle->isNormalWindow()
            || handle->skipSwitcher() || handle->isOnScreenDisplay()
            || handle->isNotification() || handle->isCriticalNotification()
            || handle->isTooltip() || handle->isComboBox()
            || handle->isDNDIcon() || handle->isPopupWindow()
            || !handle->readyForPainting()) {
            continue;
        }
        if (m_ignoreMinimized && handle->isMinimized()) {
            continue;
        }
        if (!ew->isOnCurrentActivity()) {
            continue;
        }
        auto slot = m_atlas->reserve(kBarThumbSize);
        if (!slot.isValid()) {
            qCWarning(KWIN_OVERVIEW_V2_LOG)
                << "OverviewEffectV2: snapshot reserve failed for"
                << handle->caption().left(40);
            continue;
        }
        handle->refOffscreenRendering();
        // Force the WindowItem visible for the whole overview
        // lifetime so renderItem produces content for off-desktop
        // windows every frame (live mini-thumbnails). Intentionally
        // exclude PAINT_DISABLED_BY_DESKTOP from the override -- if
        // we override that too the window paints on the actual
        // screen during the first frames of the activation animation
        // before the wash dims things, producing a visible flash of
        // other VDs' windows. Offscreen rendering still works for
        // the mini-thumb atlas because the explicit
        // refOffscreenRendering() call above is independent of the
        // paint-disabled flag. Current-desktop windows are already
        // painted normally, so they need no visibility override.
        if (!onCurrent) {
            m_barThumbVisRefs.emplace_back(
                ew,
                EffectWindow::PAINT_DISABLED
                    | EffectWindow::PAINT_DISABLED_BY_MINIMIZE
                    | EffectWindow::PAINT_DISABLED_BY_ACTIVITY);
        }
        m_barThumbSlots.emplace(handle, std::move(slot));
        // Drop the snapshot slot if the window dies during overview.
        connect(handle, &QObject::destroyed, this, [this, handle]() {
            auto it = m_barThumbSlots.find(handle);
            if (it == m_barThumbSlots.end()) {
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
            // Same dangling-pointer fix as in reserveSlotsForCurrentDesktop
            // — clear the drag candidate if the dragged window dies.
            if (m_dragCandidate == handle) {
                m_dragCandidate = nullptr;
                m_dragActive = false;
            }
            m_barThumbSlots.erase(it);
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
        for (auto &[handle, slot] : m_barThumbSlots) {
            if (handle) {
                handle->unrefOffscreenRendering();
            }
            m_atlas->release(slot);
        }
        for (auto &[handle, slot] : m_wallpaperSlotByHandle) {
            if (handle) {
                handle->unrefOffscreenRendering();
            }
            m_atlas->release(slot);
        }
    }
    m_windowSlots.clear();
    m_barThumbSlots.clear();
    m_stackingIndex.clear();
    m_gridCardRects.clear();
    m_wallpaperSlotByHandle.clear();
    m_wallpaperHandleByVd.clear();
    // Drop the pre-blurred wallpaper textures + scratch chain (per the
    // V2 active-memory rule). The pipeline objects (sampler, layouts,
    // render pass, pipelines) stay — they're kwin-lifetime and tiny.
    m_wallpaperBlurByHandle.clear();
    m_wallpaperBlurSrcByHandle.clear();
    m_blurScratchLevels.clear();
    m_blurScratchScreenSize = QSize();
    m_blurScratchInitialised = false;
    m_barScrollX = 0.0f;
    m_barStripWidthNdc = 0.0f;
    m_barThumbVisRefs.clear();
    m_tileLayout.clear();
    m_barTiles.clear();
    m_dragCandidate = nullptr;
    m_dragActive = false;
    m_dragLastDamage = QRect();
    m_searchText.clear();
    m_searchRenderedText.clear();
    m_searchTexture.reset();
    m_searchTextureSize = QSize();
    m_noMatchesTexture.reset();
    m_noMatchesTextureSize = QSize();
    m_addTileNdc = QRectF();
    m_addTileHover = false;
    // Drop the "+" icon texture so the V2 effect releases all GPU
    // resources on deactivate (per the V2 active-memory rule). The
    // texture is tiny (~16 KB) so re-uploading on the next activate
    // is essentially free.
    m_addIconTexture.reset();
    m_deleteIconTexture.reset();
    // Per-tile label cache (~40 KiB per window) -- drop on deactivate
    // so the next activation starts at zero (V2 active-memory rule).
    m_tileLabels.clear();
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
        // Search filter: drop windows whose caption doesn't contain
        // the typed string. Case-insensitive substring match, same
        // semantics V1's WindowFilterModel uses for its caption
        // check. Empty m_searchText short-circuits to "match all".
        if (!m_searchText.isEmpty()
            && !handle->caption().contains(m_searchText, Qt::CaseInsensitive)) {
            continue;
        }
        // Dynamic IgnoreMinimized: a window minimised after the
        // overview opened drops out without waiting for the next
        // activation. Reservation site already filters at activate
        // time; this catches the live state change.
        if (m_ignoreMinimized && handle->isMinimized()) {
            continue;
        }
        // Dynamic activity filter: same reasoning — a window moved
        // off the current activity mid-overview drops out without
        // needing a re-activate.
        if (ew && !ew->isOnCurrentActivity()) {
            continue;
        }
        drawable.push_back({handle, it->second, 0, 0, 0, 0, 0, 0, 0, 0});
    }
    const int n = drawable.size();
    // Always publish the filtered set so callers see the true count
    // (the search "No matching windows" placeholder relies on
    // m_tileLayout.empty() to mean "filter matched nothing", not
    // "we left an old layout untouched because n was zero").
    m_tileLayout.clear();
    if (n == 0) {
        return;
    }
    const int cols = std::max(1, int(std::ceil(std::sqrt(double(n)))));
    const int rows = (n + cols - 1) / cols;
    // Grid top sits below the desktop bar AND the search-bar strip
    // (kBarTop + kBarHeight + small gap + search-bar height + small
    // gap); see rebuildBarLayout for the bar's NDC bounds and the
    // search-bar draw in renderTilesPostPass for where it lands. V1's
    // QML reserves a whole row for PlasmaExtras.SearchField above the
    // window heap, so the grid heap starts noticeably lower than the
    // gap-below-bar position the early V2 used (which made the search
    // bar overlap the top row of grid tiles).
    // V1's QML lays the per-VD card at ~87% of screen height /
    // ~87% width on a 16:9 monitor (heap loader fills the space
    // below topBar, extends to screen bottom). V2's previous
    // 75% × 75% card looked visibly smaller. New range 1.70 NDC
    // gives ~85% × 85% which closes most of the gap. kGridTop stays
    // below the search bar's quad bottom (-0.72) so the search
    // strip fits between the desktop bar and the grid top.
    constexpr float kGridTop = -0.72f;
    constexpr float kGridBottom = 0.98f;
    const float screenW = std::max(1.0f, float(fbSize.width()));
    const float screenH = std::max(1.0f, float(fbSize.height()));
    // Match the wallpaper card's settled rect (drawWallpaperBackground
    // computes the same formula). V1's QML positions the window heap
    // *inside* the per-VD card; without this match the cells were
    // laid against [-0.8, +0.8] and tiles spilled past the card's
    // left/right edges on 16:9 screens (card is ~1.5 NDC wide).
    const float cardPxW = screenW;
    const float cardPxH = (kGridBottom - kGridTop) * 0.5f * screenH;
    const float screenAspect = screenW / std::max(1.0f, screenH);
    const float cardSettledPxH = std::min(cardPxH, cardPxW / screenAspect);
    const float cardSettledPxW = cardSettledPxH * screenAspect;
    const float cardNdcW = 2.0f * cardSettledPxW / screenW;
    const float cardNdcH = 2.0f * cardSettledPxH / screenH;
    const float cardCentreY = (kGridTop + kGridBottom) * 0.5f;
    const float cardLeft = -cardNdcW * 0.5f;
    const float cardTop = cardCentreY - cardNdcH * 0.5f;
    const float cellNdcW = cardNdcW / cols;
    const float cellNdcH = cardNdcH / rows;
    constexpr float kTilePad = 0.9f;
    const float maxNdcW = cellNdcW * kTilePad;
    const float maxNdcH = cellNdcH * kTilePad;
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
        // V1 parity: clamp at 1.0 so a small window (e.g. KCalc) on a
        // sparse grid doesn't blow up to fill the cell. Tiles only
        // ever shrink to fit, never enlarge past their natural size.
        const float scale = std::min({scaleX, scaleY, 1.0f});
        const float tileNdcW = t.realNdcW * scale;
        const float tileNdcH = t.realNdcH * scale;
        const int col = i % cols;
        const int row = i / cols;
        const float cellOriginX = cardLeft + col * cellNdcW;
        const float cellOriginY = cardTop + row * cellNdcH;
        t.gridNdcX = cellOriginX + (cellNdcW - tileNdcW) * 0.5f;
        t.gridNdcY = cellOriginY + (cellNdcH - tileNdcH) * 0.5f;
        t.gridNdcW = tileNdcW;
        t.gridNdcH = tileNdcH;
    }
    m_tileLayout = std::move(drawable);

    // Clamp grid focus to the new tile count so layout changes (window
    // closed during overview, window appeared) don't leave a stale
    // focus index pointing at nothing.
    if (m_focusZone == FocusZone::Grid) {
        const int newN = int(m_tileLayout.size());
        if (newN == 0) {
            m_focusZone = FocusZone::None;
            m_focusedIndex = -1;
        } else if (m_focusedIndex >= newN) {
            m_focusedIndex = newN - 1;
        }
    }
}

void OverviewEffectV2::rebuildBarLayout(const QSize &fbSize)
{
    m_barTiles.clear();
    m_addTileNdc = QRectF();
    if (!effects) {
        return;
    }
    // No desktop bar in WindowClass mode — leave the tile list empty so the
    // post-pass draws only the window grid and the search field.
    if (m_mode == Mode::WindowClass) {
        return;
    }
    const auto desktops = effects->desktops();
    const int n = desktops.size();
    VirtualDesktop *current = effects->currentDesktop();

    // Bar bounds in NDC, derived from Plasma's gridUnit so the bar
    // scales with the user's font setting (V1 parity: V1's
    // DesktopBar.qml uses Kirigami.Units.gridUnit * 5 for the tile
    // height and gridUnit-derived spacing). gridUnit corresponds to
    // QFontMetrics(qApp->font()).height(); on a default 10pt Plasma
    // setup that's ~14-18 px.
    //
    // Tile width = tile height in NDC because NDC X and Y are scaled
    // by different pixel half-extents (screenW/2 vs screenH/2), so
    // equal NDC values give screen-aspect-correct pixel tiles.
    //
    // Bar label strip (gridUnit tall) sits below each tile; reserved
    // here so other layout sites (kGridTop) can stay below the full
    // bar zone (bar tile + label strip).
    //
    // The Add-VD tile lives at the trailing end of the strip and is
    // always rendered, even with a single desktop, so users can
    // create more VDs from inside the overview.
    const float screenW = std::max(1.0f, float(fbSize.width()));
    const float screenH = std::max(1.0f, float(fbSize.height()));
    const float gridUnitPx = float(QFontMetrics(qApp->font()).height());
    const float gridUnitNdcX = 2.0f * gridUnitPx / screenW;
    const float gridUnitNdcY = 2.0f * gridUnitPx / screenH;
    const float kBarTop = -1.0f + gridUnitNdcY; // 1 gridUnit top margin
    const float kBarHeight = 5.0f * gridUnitNdcY; // V1's desktopHeight
    // Reserve no space for labels yet -- the label draw is the next
    // step in stage 3b. When labels land, set this to gridUnitNdcY
    // to match V1's column = desktopHeight + gridUnit layout.
    const float kBarLabelStrip = 0.0f;
    // V1's Row spacing in DesktopBar.qml defaults to smallSpacing
    // (~gridUnit/4.5 in Plasma's Kirigami.Units). Approximate.
    const float kGutter = gridUnitNdcX * 0.22f;
    const float tileW = kBarHeight; // → screen aspect in pixel space
    m_barTopNdc = kBarTop;
    m_barHeightNdc = kBarHeight;
    m_barTileWidthNdc = tileW;
    m_barLabelStripNdc = kBarLabelStrip;
    const int totalSlots = n + 1; // desktop tiles + Add tile
    const float stripW = totalSlots * tileW + (totalSlots - 1) * kGutter;
    m_barStripWidthNdc = stripW;
    // When the strip fits the viewport, centre it (V1 Flickable centres
    // its content via leftMargin); when it overflows, shift by the user's
    // scroll offset clamped so the leading / trailing edge can't
    // overshoot the viewport. Matches V1's HorizontalFlick behaviour.
    constexpr float kViewportW = 2.0f;
    float stripLeft;
    if (stripW <= kViewportW) {
        m_barScrollX = 0.0f;
        stripLeft = -stripW * 0.5f;
    } else {
        const float overflow = (stripW - kViewportW) * 0.5f;
        m_barScrollX = std::clamp(m_barScrollX, -overflow, overflow);
        stripLeft = -stripW * 0.5f + m_barScrollX;
    }

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
    m_addTileNdc = QRectF(stripLeft + n * (tileW + kGutter), kBarTop, tileW, kBarHeight);
}

void OverviewEffectV2::renderDesktopBar(VkCommandBuffer cmd, const QSize &fbSize)
{
    if ((m_barTiles.empty() && m_addTileNdc.isEmpty()) || m_vkPipelineLayout == VK_NULL_HANDLE) {
        return;
    }
    // The pipeline is already bound, descriptor set 0 already has an
    // atlas-or-fallback image bound from the window-tile passes (the
    // shader doesn't sample it when tintRgba.a == 1, but Vulkan still
    // requires a valid binding). Viewport+scissor are also set to the
    // full framebuffer; bar tiles draw at their own NDC rects.
    const float factor = float(std::clamp(m_activationFactor, 0.0, 1.0));
    const float atlasSize = float(VulkanThumbnailAtlas::kAtlasSize);
    for (const BarTile &b : m_barTiles) {
        OverviewQuadPushConstants pc{};
        pc.quadRectNdc[0] = b.ndcX;
        pc.quadRectNdc[1] = b.ndcY;
        pc.quadRectNdc[2] = b.ndcW;
        pc.quadRectNdc[3] = b.ndcH;
        setCornerRadiusUv(pc, fbSize, roundedRadiusPx(kRoundedRadiusTileFrac));
        // Sample this VD's wallpaper if its slot is atlas-resident.
        // All atlas slots share the bound srgbView; only UV differs.
        // Fallback slots have their own dedicated view, so skip the
        // wallpaper path and fall back to the solid colour. Mirrors
        // V1's DesktopView per-tile wallpaper rendering.
        const VulkanThumbnailAtlas::Slot *wpSlot = nullptr;
        auto hIt = m_wallpaperHandleByVd.find(b.desktop);
        if (hIt != m_wallpaperHandleByVd.end()) {
            auto sIt = m_wallpaperSlotByHandle.find(hIt->second);
            if (sIt != m_wallpaperSlotByHandle.end()
                && sIt->second.isValid() && sIt->second.hasContent
                && !sIt->second.isFallback) {
                wpSlot = &sIt->second;
            }
        }
        if (wpSlot) {
            pc.atlasSlotUv[0] = float(wpSlot->rect.x()) / atlasSize;
            pc.atlasSlotUv[1] = float(wpSlot->rect.y()) / atlasSize;
            pc.atlasSlotUv[2] = float(wpSlot->rect.width()) / atlasSize;
            pc.atlasSlotUv[3] = float(wpSlot->rect.height()) / atlasSize;
            // tintRgba.a = 0 → pure texture sample, no tint mix.
            pc.opacity = factor;
        } else {
            // Solid-colour fallback (no wallpaper slot available).
            pc.atlasSlotUv[0] = 0.0f;
            pc.atlasSlotUv[1] = 0.0f;
            pc.atlasSlotUv[2] = 1.0f;
            pc.atlasSlotUv[3] = 1.0f;
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
            pc.opacity = (b.isCurrent ? 0.7f : 0.55f) * factor;
        }

        vkCmdPushConstants(cmd, m_vkPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }

    // Add-VD affordance background. Same solid-tile draw as a bar
    // tile but tinted neutrally and brightened on hover. The "+"
    // icon overlay is drawn in a separate pass by the caller so it
    // can swap the descriptor view to the persistent icon texture.
    if (!m_addTileNdc.isEmpty()) {
        OverviewQuadPushConstants pc{};
        pc.quadRectNdc[0] = float(m_addTileNdc.x());
        pc.quadRectNdc[1] = float(m_addTileNdc.y());
        pc.quadRectNdc[2] = float(m_addTileNdc.width());
        pc.quadRectNdc[3] = float(m_addTileNdc.height());
        setCornerRadiusUv(pc, fbSize, roundedRadiusPx(kRoundedRadiusTileFrac));
        pc.atlasSlotUv[0] = 0.0f;
        pc.atlasSlotUv[1] = 0.0f;
        pc.atlasSlotUv[2] = 1.0f;
        pc.atlasSlotUv[3] = 1.0f;
        pc.tintRgba[0] = 0.5f;
        pc.tintRgba[1] = 0.5f;
        pc.tintRgba[2] = 0.55f;
        pc.tintRgba[3] = 1.0f;
        pc.opacity = (m_addTileHover ? 0.7f : 0.5f) * factor;
        vkCmdPushConstants(cmd, m_vkPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }

    // Mini-thumbnails inside each bar tile. Real-position layout
    // (V1's DesktopView parity): each window maps to a scaled-down
    // copy of its actual on-screen rect within the bar tile, so the
    // user sees the desktop's real arrangement at a glance rather
    // than a generic grid of caption-less rectangles. Position is
    // computed from visibleGeometry() relative to the virtual screen
    // and scaled into the bar tile's inner-padded rect.
    //
    // Current-desktop windows use the full-size slot from
    // m_windowSlots (sampled at low LOD via the mip cascade); non-
    // current-desktop windows use the small static snapshot in
    // m_barThumbSlots. Both are atlas-resident and share the
    // descriptor the caller already bound. Fallback slots are skipped
    // here to avoid per-thumb descriptor switching -- the bar tile's
    // solid colour still shows for that window.
    if (!effects) {
        return;
    }
    constexpr float kBarInnerPad = 0.08f;
    const QRect virtualScreen = effects->virtualScreenGeometry();
    const float vsWf = std::max(1.0f, float(virtualScreen.width()));
    const float vsHf = std::max(1.0f, float(virtualScreen.height()));
    for (const BarTile &b : m_barTiles) {
        if (!b.desktop) {
            continue;
        }
        struct MiniSrc
        {
            VulkanThumbnailAtlas::Slot slot;
            QRectF frame; // window frame rect, virtual-screen px
            QRectF vis; // visible rect (frame + shadow halo), same space
            int z; // frozen stacking index (bottom..top)
        };
        std::vector<MiniSrc> srcs;
        for (EffectWindow *ew : effects->stackingOrder()) {
            if (!ew || !ew->isOnDesktop(b.desktop)) {
                continue;
            }
            Window *handle = ew->window();
            // Always sample the small bar-thumb slot, for every tile.
            // Current-desktop windows have one too now (see
            // reserveBarThumbs) so the bar never depends on the large,
            // atlas-pressure-prone m_windowSlots entries.
            const auto &lookupMap = m_barThumbSlots;
            auto it = lookupMap.find(handle);
            if (it == lookupMap.end()) {
                continue;
            }
            const auto &slot = it->second;
            if (slot.isFallback || !slot.hasContent) {
                continue;
            }
            // Place by frameGeometry(): it is the authoritative, always-
            // current window rect, whereas visibleGeometry() (the
            // WindowItem's scene-mapped bounding rect) can lag behind a
            // move performed while the overview was closed, leaving the
            // mini-thumbnail at a stale position. visibleGeometry is
            // still needed for the slot UV crop, since the slot content
            // spans it (frame + decoration-shadow halo).
            const QRectF frame = handle->frameGeometry();
            const QRectF vis = handle->visibleGeometry();
            if (frame.isEmpty() || vis.isEmpty()) {
                continue;
            }
            auto zit = m_stackingIndex.find(handle);
            const int z = (zit != m_stackingIndex.end()) ? zit->second : 0;
            srcs.push_back({slot, frame, vis, z});
        }
        if (srcs.empty()) {
            continue;
        }
        // Draw bottom-to-top using the frozen stacking snapshot so the
        // overlapping mini-thumbnails keep a steady z-order through the
        // activation animation (stable_sort keeps reservation order for
        // any ties).
        std::stable_sort(srcs.begin(), srcs.end(),
                         [](const MiniSrc &a, const MiniSrc &b) {
            return a.z < b.z;
        });
        const float padX = b.ndcW * kBarInnerPad;
        const float padY = b.ndcH * kBarInnerPad;
        const float innerW = b.ndcW - 2 * padX;
        const float innerH = b.ndcH - 2 * padY;
        const float innerX = b.ndcX + padX;
        const float innerY = b.ndcY + padY;
        for (const MiniSrc &s : srcs) {
            const float relX = float(s.frame.x() - virtualScreen.x()) / vsWf;
            const float relY = float(s.frame.y() - virtualScreen.y()) / vsHf;
            const float relW = float(s.frame.width()) / vsWf;
            const float relH = float(s.frame.height()) / vsHf;
            const float x = innerX + std::clamp(relX, 0.0f, 1.0f) * innerW;
            const float y = innerY + std::clamp(relY, 0.0f, 1.0f) * innerH;
            const float w = std::clamp(relW, 0.0f, 1.0f) * innerW;
            const float h = std::clamp(relH, 0.0f, 1.0f) * innerH;
            if (w <= 0.0f || h <= 0.0f) {
                continue;
            }
            // Slot content spans the visible rect (frame + shadow halo);
            // the quad is the frame only, so sample the frame sub-region
            // of the slot to crop the halo — same as the grid tiles.
            const float ixL = std::clamp(float((s.frame.x() - s.vis.x()) / s.vis.width()), 0.0f, 1.0f);
            const float iyT = std::clamp(float((s.frame.y() - s.vis.y()) / s.vis.height()), 0.0f, 1.0f);
            const float ixR = std::clamp(float((s.vis.right() - s.frame.right()) / s.vis.width()), 0.0f, 1.0f);
            const float iyB = std::clamp(float((s.vis.bottom() - s.frame.bottom()) / s.vis.height()), 0.0f, 1.0f);

            OverviewQuadPushConstants pc{};
            pc.quadRectNdc[0] = x;
            pc.quadRectNdc[1] = y;
            pc.quadRectNdc[2] = w;
            pc.quadRectNdc[3] = h;
            setCornerRadiusUv(pc, fbSize, roundedRadiusPx(kRoundedRadiusMiniFrac));
            pc.atlasSlotUv[0] = (float(s.slot.rect.x()) + float(s.slot.rect.width()) * ixL) / atlasSize;
            pc.atlasSlotUv[1] = (float(s.slot.rect.y()) + float(s.slot.rect.height()) * iyT) / atlasSize;
            pc.atlasSlotUv[2] = float(s.slot.rect.width()) * (1.0f - ixL - ixR) / atlasSize;
            pc.atlasSlotUv[3] = float(s.slot.rect.height()) * (1.0f - iyT - iyB) / atlasSize;
            // tintRgba.a = 0 → pure atlas sample.
            pc.opacity = factor;
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
    }
}

void OverviewEffectV2::ensureBarIconTextures()
{
    // Persistent "+" icon. The "×" delete icon is allocated by the
    // same path in the next commit (item 3b). Both are built once
    // per V2 lifetime — they're small (~16 KB each at 64×64 RGBA)
    // and the QPainter rendering is the only non-trivial cost.
    if (!m_vulkanCtx) {
        return;
    }
    if (!m_addIconTexture) {
        // Use the same "list-add" theme icon as V1's QML
        // DesktopBar.qml so the affordance matches across versions.
        const int sidePx = 64;
        const QIcon icon = QIcon::fromTheme(QStringLiteral("list-add"));
        QImage img(sidePx, sidePx, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        bool haveThemeIcon = false;
        if (!icon.isNull()) {
            const QPixmap pm = icon.pixmap(sidePx, sidePx);
            if (!pm.isNull()) {
                QPainter p(&img);
                p.drawPixmap(0, 0, pm);
                p.end();
                haveThemeIcon = true;
            }
        }
        if (!haveThemeIcon) {
            // Theme icon missing — fall back to a hand-drawn "+".
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            QPen pen(QColor(255, 255, 255, 230));
            pen.setWidthF(sidePx * 0.10f);
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            const float half = sidePx * 0.5f;
            const float arm = sidePx * 0.28f;
            p.drawLine(QPointF(half - arm, half), QPointF(half + arm, half));
            p.drawLine(QPointF(half, half - arm), QPointF(half, half + arm));
            p.end();
        }
        m_addIconTexture = VulkanTexture::upload(m_vulkanCtx, img);
        if (!m_addIconTexture) {
            qCWarning(KWIN_OVERVIEW_V2_LOG)
                << "OverviewEffectV2: add-icon upload failed";
        }
    }
    if (!m_deleteIconTexture) {
        const int sidePx = 64;
        QImage img(sidePx, sidePx, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        // Translucent dark disc with a white "×". Disc draws first,
        // strokes on top — gives the affordance an obvious target on
        // any bar-tile colour underneath.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 180));
        p.drawEllipse(QRectF(0, 0, sidePx, sidePx));
        QPen pen(QColor(255, 255, 255, 240));
        pen.setWidthF(sidePx * 0.10f);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        const float half = sidePx * 0.5f;
        const float arm = sidePx * 0.22f;
        p.drawLine(QPointF(half - arm, half - arm), QPointF(half + arm, half + arm));
        p.drawLine(QPointF(half - arm, half + arm), QPointF(half + arm, half - arm));
        p.end();
        m_deleteIconTexture = VulkanTexture::upload(m_vulkanCtx, img);
        if (!m_deleteIconTexture) {
            qCWarning(KWIN_OVERVIEW_V2_LOG)
                << "OverviewEffectV2: delete-icon upload failed";
        }
    }
}

VulkanTexture *OverviewEffectV2::ensureTileLabelTexture(Window *handle)
{
    if (!handle || !m_vulkanCtx) {
        return nullptr;
    }
    // Label texture mirrors V1's WindowHeapDelegate: a large window
    // icon stacked ABOVE a one-line caption, both horizontally
    // centred. Only the caption carries a rounded background pill
    // (sized to the text content, like V1's PC3.Label background);
    // the icon floats on transparent texture. All dimensions are
    // gridUnit-derived so the layout scales with the user's font /
    // DPI. The icon side approximates Kirigami.Units.iconSizes.large
    // (≈ 8/3 gridUnit at default metrics) so it reads clearly bigger
    // than the caption, matching V1.
    const int gridUnitPx = QFontMetrics(qApp->font()).height();
    const int kSmallSpacing = std::max(2, gridUnitPx / 4);
    const int kCornerRadius = std::max(2, gridUnitPx / 4);
    const int kIconSidePx = 3 * gridUnitPx;
    const QFontMetrics fm(qApp->font());
    const int kTextH = fm.height();
    const int kTextPillH = kTextH + kSmallSpacing;
    const int kLabelW = 16 * gridUnitPx;
    const int kLabelH = kIconSidePx + kSmallSpacing + kTextPillH;

    const QString caption = handle->caption();
    auto it = m_tileLabels.find(handle);
    if (it != m_tileLabels.end()
        && it->second.texture
        && it->second.texture->isValid()
        && it->second.renderedCaption == caption) {
        return it->second.texture.get(); // cache hit
    }

    QImage img(kLabelW, kLabelH, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    // Icon centred at the top -- Window::icon() returns a QIcon;
    // renders blank if the window has none (caption-only fallback).
    const QIcon icon = handle->icon();
    const int iconX = (kLabelW - kIconSidePx) / 2;
    const int iconY = 0;
    if (!icon.isNull()) {
        const QPixmap pm = icon.pixmap(QSize(kIconSidePx, kIconSidePx));
        if (!pm.isNull()) {
            p.drawPixmap(iconX, iconY, pm);
        }
    }

    // Caption below the icon, centred. qApp->font() (system default)
    // matches V1's Kirigami theme font; palette.window() pill +
    // palette.windowText() text give the canonical theme contrast on
    // any colour scheme. The pill is sized to the text content
    // (+ smallSpacing) and centred, exactly like V1's PC3.Label
    // background Rectangle.
    p.setFont(qApp->font());
    const int textMaxW = kLabelW - 2 * kSmallSpacing;
    const QString elided = fm.elidedText(caption, Qt::ElideRight, textMaxW);
    const int textW = std::min(textMaxW, fm.horizontalAdvance(elided));
    const int pillW = textW + kSmallSpacing;
    const int pillX = (kLabelW - pillW) / 2;
    const int pillY = kIconSidePx + kSmallSpacing;
    p.setBrush(qApp->palette().window().color());
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRect(pillX, pillY, pillW, kTextPillH),
                      kCornerRadius, kCornerRadius);

    p.setPen(qApp->palette().windowText().color());
    const int textBaselineY = pillY + (kTextPillH + fm.ascent() - fm.descent()) / 2;
    p.drawText((kLabelW - textW) / 2, textBaselineY, elided);
    p.end();

    auto next = VulkanTexture::upload(m_vulkanCtx, img);
    if (!next) {
        qCWarning(KWIN_OVERVIEW_V2_LOG)
            << "OverviewEffectV2: tile-label upload failed for"
            << handle;
        return nullptr;
    }
    VulkanTexture *raw = next.get();
    auto &entry = m_tileLabels[handle];
    entry.texture = std::move(next);
    entry.renderedCaption = caption;
    return raw;
}

void OverviewEffectV2::updateSearchTexture()
{
    // Always-visible search bar matches V1's QML, which renders the
    // PlasmaExtras.SearchField for the entire overview lifetime --
    // the empty bar with a blinking caret is the visual hint that
    // typing will filter the grid. First-keystroke buffering still
    // works (Effect::grabKeyboard catches the press regardless).
    //
    // ~128 KiB texture pinned for the duration of an overview
    // activation; deactivate() releases it via m_searchTexture.reset()
    // alongside every other GPU resource (V2 active-memory rule).
    if (m_searchText == m_searchRenderedText && m_searchTexture) {
        return; // cache hit — no re-render needed
    }
    if (!m_vulkanCtx) {
        return;
    }

    // Bar dimensions derived from gridUnit (= QFontMetrics(font).height
    // = ~Kirigami.Units.gridUnit on the C++ side). V1's QML uses
    // Math.min(parent.width, 20 * gridUnit) for the SearchField width
    // and the system font height (plus theme padding ~= half gridUnit
    // on each side) for the height. Result scales with the user's
    // font / DPI setting so HiDPI users get a proportionally larger
    // bar instead of a tiny stripe.
    const int gridUnitPx = QFontMetrics(qApp->font()).height();
    const int kWidth = 20 * gridUnitPx;
    const int kHeight = 2 * gridUnitPx;
    // Spacing tracks the font (≈ Kirigami.Units.smallSpacing), same
    // formula the tile labels use, so the field's internal proportions
    // hold at any DPI / font size. Corner radius + border weight stay
    // fixed small values, matching Kirigami's cornerRadius / border
    // (those are device-pixel units, not font-scaled).
    const int kSmallSpacing = std::max(2, gridUnitPx / 4);
    const QSize size(kWidth, kHeight);

    QImage img(size, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    // Rounded translucent background, similar contrast to the bar
    // tiles' solid colour scheme. Premultiplied output so the
    // existing post-pass blend math (premul ONE / ONE_MINUS_SRC_ALPHA)
    // composites it correctly on top of the dimmed scene capture.
    QColor bg(0, 0, 0, 180);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(img.rect().adjusted(2, 2, -2, -2), 8, 8);

    // V1's PlasmaExtras.SearchField has a thin themed outline that
    // tracks the active focus colour. We don't have keyboard focus
    // semantics for the bar (it's always focused while overview is
    // open), so just paint the highlight-coloured border statically
    // at the standard Kirigami border weight.
    QPen borderPen(qApp->palette().highlight().color());
    borderPen.setWidthF(1.5);
    p.setPen(borderPen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(img.rect()).adjusted(2.0, 2.0, -2.0, -2.0), 8.0, 8.0);

    // Search icon on the left, matching V1's PlasmaExtras.SearchField.
    // The icon stays visible whether or not the field has text. Theme
    // icons are typically dark glyphs; recolour to the text colour
    // so the icon stays visible against the dark bar background and
    // reads as continuous with the typed text.
    const int kIconSize = gridUnitPx;
    const int kIconX = gridUnitPx / 2;
    const QColor textColor(255, 255, 255, 240);
    const int iconY = (kHeight - kIconSize) / 2;
    const QIcon searchIcon = QIcon::fromTheme(QStringLiteral("search-symbolic"),
                                              QIcon::fromTheme(QStringLiteral("edit-find")));
    if (!searchIcon.isNull()) {
        QPixmap pm = searchIcon.pixmap(QSize(kIconSize, kIconSize));
        if (!pm.isNull()) {
            QPainter recolour(&pm);
            recolour.setCompositionMode(QPainter::CompositionMode_SourceIn);
            recolour.fillRect(pm.rect(), textColor);
            recolour.end();
            p.drawPixmap(kIconX, iconY, pm);
        }
    }

    // System font at its default size matches V1's SearchField, which
    // doesn't override font.pointSize -- the inherited theme size.
    QFont font = qApp->font();
    p.setFont(font);

    const QFontMetrics fm(font);
    const int textBaseline = (kHeight + fm.ascent() - fm.descent()) / 2;
    const int textX = kIconX + kIconSize + 2 * kSmallSpacing;
    if (m_searchText.isEmpty()) {
        // Empty state: icon only, no caret. V1 shows a "Search…"
        // placeholder string from Plasma's i18n catalog; reusing it
        // cleanly from kwin_x11 would need a catalog-loading dance,
        // and the search icon already signals "this is a search
        // field" on its own. Skip the text to avoid creating a new
        // string for translators.
    } else {
        p.setPen(QColor(255, 255, 255, 240));
        // Elide-left so the cursor (right edge) stays visible.
        const QString display = fm.elidedText(m_searchText,
                                              Qt::ElideLeft, kWidth - textX - 2 * kSmallSpacing);
        p.drawText(textX, textBaseline, display);
        // Caret: thin vertical line at the right edge of rendered text.
        const int caretX = textX + fm.horizontalAdvance(display) + kSmallSpacing;
        if (caretX < kWidth - 2 * kSmallSpacing) {
            p.fillRect(QRect(caretX, 2 * kSmallSpacing, std::max(2, kSmallSpacing - 1),
                             kHeight - 4 * kSmallSpacing),
                       QColor(255, 255, 255, 220));
        }
    }
    p.end();

    // QPainter writes BGRA on little-endian QImage_Format_RGBA8888; the
    // upload path uploads as RGBA8 SRGB regardless, so call this only
    // when the user changes the search text — Qt's glyph cache is
    // shared across the process and the per-keystroke cost is just
    // QPainter's draw + a memcpy.
    auto next = VulkanTexture::upload(m_vulkanCtx, img);
    if (!next) {
        qCWarning(KWIN_OVERVIEW_V2_LOG)
            << "OverviewEffectV2: search-bar texture upload failed";
        return;
    }
    m_searchTexture = std::move(next);
    m_searchRenderedText = m_searchText;
    m_searchTextureSize = size;
}

void OverviewEffectV2::ensureNoMatchesTexture()
{
    if (m_noMatchesTexture || !m_vulkanCtx) {
        return;
    }
    // Mirrors V1's PlasmaExtras.PlaceholderMessage shown when
    // currentHeap.count == 0 && filterWindows. Text is fixed so we
    // build the texture once and reuse for the whole activation.
    // V1's PlasmaExtras.PlaceholderMessage renders at Kirigami Heading
    // 2 level (~1.6x the system font, bold). Mirror that here.
    QFont font = qApp->font();
    font.setPointSizeF(font.pointSizeF() * 1.6);
    font.setBold(true);
    const QFontMetrics fm(font);
    const QString text = i18ndc("kwin_x11",
                                "@info:placeholder Shown in the overview when search has no results",
                                "No matching windows");
    // gridUnit-derived side padding + minimum width so the placeholder
    // scales with the user's font setting alongside V1's Kirigami
    // PlaceholderMessage sizing.
    const int gridUnitPx = QFontMetrics(qApp->font()).height();
    const int kSidePad = gridUnitPx;
    const int kHeight = fm.height() + gridUnitPx; // half gridUnit padding each side
    const int width = std::max(8 * gridUnitPx,
                               fm.horizontalAdvance(text) + 2 * kSidePad);
    const QSize size(width, kHeight);

    QImage img(size, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    // No background pill: V1's PlaceholderMessage draws plain text on
    // top of the blur+wash backdrop, no chrome. We hide the sharp
    // wallpaper card when no matches (see drawWallpaperBackground)
    // so the text sits on the dimmed/blurred backdrop with consistent
    // contrast across wallpapers.
    p.setFont(font);
    // Same colour as the search-bar text so the empty-state message
    // reads as continuous with the bar that introduced it. The wash
    // beneath provides enough contrast for white-on-dim on both light
    // and dark themes.
    p.setPen(QColor(255, 255, 255, 240));
    const int baseline = (kHeight + fm.ascent() - fm.descent()) / 2;
    p.drawText(kSidePad, baseline, text);
    p.end();

    m_noMatchesTexture = VulkanTexture::upload(m_vulkanCtx, img);
    if (m_noMatchesTexture) {
        m_noMatchesTextureSize = size;
    } else {
        qCWarning(KWIN_OVERVIEW_V2_LOG)
            << "OverviewEffectV2: no-matches placeholder upload failed";
    }
}

void OverviewEffectV2::renderWindowsToAtlas()
{
    if (!m_atlas || !m_vulkanCtx) {
        return;
    }
    // Off-desktop slots (m_barThumbSlots) re-render every frame
    // alongside the current-desktop slots so bar mini-thumbnails are
    // live, not stale snapshots. m_barThumbVisRefs keeps the
    // off-desktop WindowItems visible for the whole overview
    // lifetime; releaseAllSlots drops them, the X11 suspend hook
    // (a50a7e6d1c + 1d51bf4e61) frees the per-window cached
    // resources, and the dedicated-allocation work (39cf11b882 +
    // b8d35e7a0b) returns that VRAM to the OS instead of pinning
    // VMA blocks.
    if (m_windowSlots.empty() && m_barThumbSlots.empty() && m_wallpaperSlotByHandle.empty()) {
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
    // such slot points at the same atlas resources, including the
    // snapshot slots). If every slot is a fallback we skip the atlas
    // render pass entirely.
    VkImage atlasImage = VK_NULL_HANDLE;
    VkImageView atlasMipZero = VK_NULL_HANDLE;
    auto findAtlas = [&](const auto &slots) {
        if (atlasImage != VK_NULL_HANDLE) {
            return;
        }
        for (const auto &[_handle, slot] : slots) {
            if (!slot.isFallback) {
                atlasImage = slot.image;
                atlasMipZero = slot.mipZeroView;
                return;
            }
        }
    };
    findAtlas(m_windowSlots);
    findAtlas(m_barThumbSlots);
    if (atlasImage == VK_NULL_HANDLE) {
        for (const auto &[handle, slot] : m_wallpaperSlotByHandle) {
            if (handle && !slot.isFallback) {
                atlasImage = slot.image;
                atlasMipZero = slot.mipZeroView;
                break;
            }
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
        for (auto &[_handle, slot] : m_barThumbSlots) {
            if (slot.isFallback) {
                continue;
            }
            m_atlas->prepareForRenderTo(cmd, slot);
        }
        for (auto &[handle, slot] : m_wallpaperSlotByHandle) {
            if (handle && !slot.isFallback) {
                m_atlas->prepareForRenderTo(cmd, slot);
            }
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

        auto renderSlot = [&](Window *handle, VulkanThumbnailAtlas::Slot &slot) {
            const QRectF windowGeom = handle->visibleGeometry();
            if (windowGeom.isEmpty()) {
                return;
            }
            // Y-flipped viewport matches the main render path (and
            // WindowThumbnailSource's existing GL path) so the
            // resulting mip-0 layout reads top-down. Viewport size =
            // slot size: a small snapshot slot scales the source
            // window down inline.
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
        };

        for (auto &[handle, slot] : m_windowSlots) {
            if (slot.isFallback || !handle) {
                continue;
            }
            renderSlot(handle, slot);
        }
        for (auto &[handle, slot] : m_barThumbSlots) {
            if (slot.isFallback || !handle) {
                continue;
            }
            renderSlot(handle, slot);
        }
        for (auto &[handle, slot] : m_wallpaperSlotByHandle) {
            if (handle && !slot.isFallback) {
                renderSlot(handle, slot);
            }
        }

        m_atlasRenderPass->end(cmd);
    }

    // Fallback slots — one dedicated image per source. Each gets its
    // own framebuffer (cached on m_fallbackFramebuffers) and its own
    // begin/end on the shared render pass. Same Y-flipped viewport
    // convention as the atlas pass.
    auto renderFallbackSlot = [&](Window *handle, VulkanThumbnailAtlas::Slot &slot) {
        if (!slot.isFallback || !handle) {
            return;
        }
        const QRectF windowGeom = handle->visibleGeometry();
        if (windowGeom.isEmpty()) {
            return;
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
                return;
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
    };
    for (auto &[handle, slot] : m_windowSlots) {
        renderFallbackSlot(handle, slot);
    }
    for (auto &[handle, slot] : m_wallpaperSlotByHandle) {
        if (handle && slot.isFallback) {
            renderFallbackSlot(handle, slot);
        }
    }

    // Mip cascade + publishing barrier for every slot — atlas and
    // fallback share the same prepare/publish API. Without this the
    // post-pass would sample undefined memory for tiles drawn at less
    // than 1:1 (mip > 0).
    for (auto &[_handle, slot] : m_windowSlots) {
        m_atlas->generateMipsAndPublish(cmd, slot);
    }
    for (auto &[_handle, slot] : m_barThumbSlots) {
        m_atlas->generateMipsAndPublish(cmd, slot);
    }
    for (auto &[handle, slot] : m_wallpaperSlotByHandle) {
        if (handle && slot.isValid()) {
            m_atlas->generateMipsAndPublish(cmd, slot);
        }
    }

    // Pre-blur the wallpapers into per-VD textures so the overview
    // backdrop samples a true dual-kawase result instead of running an
    // in-shader Gaussian per fragment. One-shot per slot per
    // activation (gated by entry.generated); subsequent frames find the
    // cached result and skip.
    for (auto &[handle, slot] : m_wallpaperSlotByHandle) {
        if (handle && slot.isValid() && slot.hasContent && !slot.isFallback) {
            blurWallpaperSlot(cmd, handle, slot);
        }
    }

    vkRenderer->popOffscreenSlot();

    // Async submit; the wait at the top of the next frame's call
    // synchronises on the offscreen-slot buffer reuse. The main
    // compositor submit on the same queue picks up the atlas writes
    // through the publishing barrier's same-queue visibility.
    m_lastAtlasSubmit = m_vulkanCtx->submitSingleTimeCommandsAsync(cmd);
}

bool OverviewEffectV2::drawWallpaperBackground(VkCommandBuffer cmd, const QSize &fbSize,
                                               VkFormat colorFormat)
{
    if (!m_atlas || !m_vulkanCtx || fbSize.isEmpty() || !effects) {
        return false;
    }
    // Fullscreen blur backdrop uses the current desktop's wallpaper.
    auto hIt = m_wallpaperHandleByVd.find(effects->currentDesktop());
    if (hIt == m_wallpaperHandleByVd.end()) {
        return false;
    }
    auto sIt = m_wallpaperSlotByHandle.find(hIt->second);
    if (sIt == m_wallpaperSlotByHandle.end()) {
        return false;
    }
    struct
    {
        Window *handle;
        VulkanThumbnailAtlas::Slot slot;
    } wp{hIt->second, sIt->second};
    if (!wp.handle || !wp.slot.isValid() || !wp.slot.hasContent
        || wp.slot.srgbView == VK_NULL_HANDLE) {
        return false;
    }
    if (!ensureVulkanPipeline(m_vulkanCtx, colorFormat)) {
        return false;
    }
    auto *backend = m_vulkanCtx->backend();
    auto pushDescriptor = backend->cmdPushDescriptorSetKHR();
    if (!pushDescriptor) {
        return false;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipeline);

    // Sample the pre-blurred per-VD result when it has been generated;
    // otherwise fall back to the sharp atlas slot + in-shader two-LOD
    // kawase (the bridge path for the first frame after activate before
    // renderWindowsToAtlas has run, and for fallback wallpaper slots).
    VkImageView srcView = wp.slot.srgbView;
    bool useBlurred = false;
    auto blurIt = m_wallpaperBlurByHandle.find(wp.handle);
    if (blurIt != m_wallpaperBlurByHandle.end() && blurIt->second.generated
        && blurIt->second.view != VK_NULL_HANDLE) {
        srcView = blurIt->second.view;
        useBlurred = true;
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = m_atlas->sampler();
    imgInfo.imageView = srcView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (imgInfo.sampler == VK_NULL_HANDLE) {
        return false;
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

    // Pass 1: wallpaper texture, fullscreen NDC quad. Pre-blurred path
    // samples the whole per-VD texture ([0,1] UV) and uses the plain
    // texture() shader path. Sharp-fallback samples the atlas slot's
    // sub-rect and triggers the in-shader two-LOD kawase via pc.lod.
    const float atlasSize = float(VulkanThumbnailAtlas::kAtlasSize);
    const float factor = float(std::clamp(m_activationFactor, 0.0, 1.0));
    OverviewQuadPushConstants pc{};
    pc.quadRectNdc[0] = -1.0f;
    pc.quadRectNdc[1] = -1.0f;
    pc.quadRectNdc[2] = 2.0f;
    pc.quadRectNdc[3] = 2.0f;
    if (useBlurred) {
        pc.atlasSlotUv[0] = 0.0f;
        pc.atlasSlotUv[1] = 0.0f;
        pc.atlasSlotUv[2] = 1.0f;
        pc.atlasSlotUv[3] = 1.0f;
        pc.lod = 0.0f; // pre-blurred — take the plain-sample path
    } else if (wp.slot.isFallback) {
        pc.atlasSlotUv[0] = 0.0f;
        pc.atlasSlotUv[1] = 0.0f;
        pc.atlasSlotUv[2] = 1.0f;
        pc.atlasSlotUv[3] = 1.0f;
        pc.lod = 1.0f;
    } else {
        pc.atlasSlotUv[0] = float(wp.slot.rect.x()) / atlasSize;
        pc.atlasSlotUv[1] = float(wp.slot.rect.y()) / atlasSize;
        pc.atlasSlotUv[2] = float(wp.slot.rect.width()) / atlasSize;
        pc.atlasSlotUv[3] = float(wp.slot.rect.height()) / atlasSize;
        pc.lod = 1.0f;
    }
    // Backdrop is always fully opaque while m_visible. The cards (Pass 3
    // in Overview, the grid in Grid View) keep their own opacity = factor
    // and animate-on-top, but the wallpaper fully covers the scene from
    // activation through deactivation. Without this clamp, the backdrop
    // fades with `factor` and the underlying real desktop bleeds through
    // during the zoom-in/-out animation — visible as flicker, especially
    // at slow animation speeds. The reveal of the real desktop happens
    // only at m_visible = false (instant cutoff at the very end of the
    // deactivation animation).
    pc.opacity = 1.0f;
    vkCmdPushConstants(cmd, m_vkPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);

    // Backdrop done. Rebind the descriptor back to the sharp atlas
    // view before any further passes (sharp wallpaper card in Overview
    // mode, the per-VD wallpaper cards in Grid View, and the grid-card
    // window thumbnails). All of those push atlas-space UVs at the
    // bound descriptor — without this rebind they'd sample those UVs
    // out of the blurred per-VD texture and read garbage (a tiny corner
    // of the blurred wallpaper rendered through every card). Only the
    // bottom backdrop should be blurred.
    if (useBlurred) {
        imgInfo.imageView = wp.slot.srgbView;
        pushDescriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipelineLayout, 0, 1, &write);
    }

    // Pass 2 (theme wash) intentionally dropped. The earlier wash was
    // there to compensate for the mip-based blur's blockiness and
    // brightness mismatch against V1's FastBlur, but it also masked
    // the wallpaper detail to a degree V1 doesn't. The plan is to
    // replace the mip-based blur with a proper dual-kawase chain
    // (matching the blur effect's Vulkan path -- see plugins/blur/
    // blur.cpp's m_vulkanBlurPipelineLayout etc.) and let the wallpaper
    // backdrop stand on its own merit. Wash hidden until that lands.

    // V1 hides the per-desktop background card when the active
    // search filter has zero matches (currentHeap.count === 0 in
    // the QML guard), so the "No matching windows" placeholder reads
    // against the dimmed wallpaper backdrop instead of the sharp
    // card. Mirror that: skip Pass 3 when m_tileLayout is empty AND
    // the user has typed a search term. m_tileLayout was rebuilt by
    // renderTilesPostPass on the previous frame so this is one-frame
    // lagged; at 60 Hz the lag is imperceptible.
    if (!m_searchText.isEmpty() && m_tileLayout.empty()) {
        return true;
    }

    // WindowClass mode (Ctrl+F7) presents the app's windows over the plain
    // blurred backdrop — no Overview wallpaper card, no GridView per-VD
    // cards. The window grid + search field draw in renderTilesPostPass.
    if (m_mode == Mode::WindowClass) {
        return true;
    }

    // Pass 3: sharp wallpaper card at the scaled-down grid area.
    // V1's per-desktop backgroundArea Item renders a second copy of
    // the same wallpaper inside Kirigami.ShadowedTexture (rounded
    // corners + drop shadow). The card animates from full-screen
    // (overviewVal=0) to a screen-aspect rect inside the grid bounds
    // (overviewVal=1). For V2 the rounded corners + shadow want a
    // dedicated shader and are deferred; this pass paints the sharp
    // wallpaper at the animated rect so the user sees the desktop
    // preview beneath the tile grid.
    constexpr float kGridTopNdc = -0.72f; // matches kGridTop in rebuildTileLayout
    constexpr float kGridBottomNdc = 0.98f; // matches kGridBottom
    const float screenWf = std::max(1.0f, float(fbSize.width()));
    const float screenHf = std::max(1.0f, float(fbSize.height()));
    // Settled (factor = 1) card: largest screen-aspect rect that
    // fits inside the grid bounds, centred. Computed in pixels then
    // converted back to NDC so the wallpaper aspect is preserved
    // independent of NDC X / Y scale.
    const float gridPxW = (1.0f - (-1.0f)) * 0.5f * screenWf;
    const float gridPxH = (kGridBottomNdc - kGridTopNdc) * 0.5f * screenHf;
    const float aspect = screenWf / screenHf;
    const float settledPxH = std::min(gridPxH, gridPxW / aspect);
    const float settledPxW = settledPxH * aspect;
    const float settledNdcW = 2.0f * settledPxW / screenWf;
    const float settledNdcH = 2.0f * settledPxH / screenHf;
    const float gridCentreX = 0.0f;
    const float gridCentreY = (kGridTopNdc + kGridBottomNdc) * 0.5f;
    const float settledLeft = gridCentreX - settledNdcW * 0.5f;
    const float settledTop = gridCentreY - settledNdcH * 0.5f;
    const float settledRight = settledLeft + settledNdcW;
    const float settledBottom = settledTop + settledNdcH;
    const float cardLeft = -1.0f + (settledLeft - -1.0f) * factor;
    const float cardTop = -1.0f + (settledTop - -1.0f) * factor;
    const float cardRight = 1.0f + (settledRight - 1.0f) * factor;
    const float cardBottom = 1.0f + (settledBottom - 1.0f) * factor;

    // GridView mode replaces the single wallpaper card with a 2-D
    // grid of per-VD wallpaper cards. The full-screen wallpaper +
    // theme wash still draw (above) so the activation slide-in
    // visually mirrors Overview's transition; only the card layout
    // differs. Bar + window-grid + outlines are skipped in
    // renderTilesPostPass when m_mode == GridView.
    if (m_mode == Mode::GridView && effects) {
        const int cols = std::max(1, effects->desktopGridWidth());
        const int rows = std::max(1, effects->desktopGridHeight());
        const auto desktopList = effects->desktops();
        // V1's DesktopGrid scales each desktop to 1/gridSize of the
        // FULL screen, where gridSize = max(rows, cols), applies a 2%
        // gap (xScale 1 - 0.02), arranges the cards in a cols x rows
        // grid and centres the whole grid. In NDC the full screen is
        // 2x2 and is itself screen-aspect, so a card occupying
        // 1/gridSize of the screen is a square-NDC rect of side
        // 2/gridSize (screen-aspect, undistorted); the gap shrinks it
        // slightly. Cell pitch is 2/gridSize on both axes; the grid is
        // centred on (0, 0).
        const int gridSize = std::max(rows, cols);
        constexpr float kCardGapFrac = 0.02f;
        const float pitch = 2.0f / float(gridSize);
        const float side = pitch * (1.0f - kCardGapFrac);
        m_gridCardRects.clear();
        // Draw the current desktop last so its wallpaper wins the
        // stack at factor = 0 — the moment of activation start /
        // deactivation end, when all cards have lerped to the full
        // screen and pile on top of each other. Matching the
        // current desktop's wallpaper there means the cross-fade
        // into / out of the real desktop is seamless (no pop from a
        // different VD's wallpaper showing on the last frame of the
        // overlay).
        VirtualDesktop *currentDesktop = effects->currentDesktop();
        std::vector<int> drawOrder;
        drawOrder.reserve(desktopList.size());
        int currentIdx = -1;
        for (int i = 0; i < int(desktopList.size()) && i < rows * cols; ++i) {
            if (desktopList[i] == currentDesktop) {
                currentIdx = i;
            } else {
                drawOrder.push_back(i);
            }
        }
        if (currentIdx >= 0) {
            drawOrder.push_back(currentIdx);
        }
        for (int i : drawOrder) {
            VirtualDesktop *vd = desktopList[i];
            const int row = i / cols;
            const int col = i % cols;
            const float cardCentreX = (float(col) - float(cols - 1) * 0.5f) * pitch;
            const float cardCentreY = (float(row) - float(rows - 1) * 0.5f) * pitch;
            const float cellSettledLeft = cardCentreX - side * 0.5f;
            const float cellSettledTop = cardCentreY - side * 0.5f;
            const float cellSettledRight = cardCentreX + side * 0.5f;
            const float cellSettledBottom = cardCentreY + side * 0.5f;
            // Record the settled card rect (NDC) for click hit-testing.
            m_gridCardRects.emplace_back(
                vd, QRectF(cellSettledLeft, cellSettledTop, cellSettledRight - cellSettledLeft, cellSettledBottom - cellSettledTop));
            const float cellLeft = -1.0f + (cellSettledLeft - -1.0f) * factor;
            const float cellTop = -1.0f + (cellSettledTop - -1.0f) * factor;
            const float cellRight = 1.0f + (cellSettledRight - 1.0f) * factor;
            const float cellBottom = 1.0f + (cellSettledBottom - 1.0f) * factor;

            // Per-VD wallpaper slot (same map renderDesktopBar uses).
            const VulkanThumbnailAtlas::Slot *wpSlot = nullptr;
            auto hIt = m_wallpaperHandleByVd.find(vd);
            if (hIt != m_wallpaperHandleByVd.end()) {
                auto sIt = m_wallpaperSlotByHandle.find(hIt->second);
                if (sIt != m_wallpaperSlotByHandle.end()
                    && sIt->second.isValid() && sIt->second.hasContent
                    && !sIt->second.isFallback) {
                    wpSlot = &sIt->second;
                }
            }
            OverviewQuadPushConstants cell{};
            cell.quadRectNdc[0] = cellLeft;
            cell.quadRectNdc[1] = cellTop;
            cell.quadRectNdc[2] = cellRight - cellLeft;
            cell.quadRectNdc[3] = cellBottom - cellTop;
            if (wpSlot) {
                cell.atlasSlotUv[0] = float(wpSlot->rect.x()) / atlasSize;
                cell.atlasSlotUv[1] = float(wpSlot->rect.y()) / atlasSize;
                cell.atlasSlotUv[2] = float(wpSlot->rect.width()) / atlasSize;
                cell.atlasSlotUv[3] = float(wpSlot->rect.height()) / atlasSize;
            } else {
                // Fallback: solid neutral tint when the wallpaper slot
                // hasn't been reserved (rare; reserveWallpaperSlot
                // handles all known cases at activate-time).
                cell.atlasSlotUv[0] = 0.0f;
                cell.atlasSlotUv[1] = 0.0f;
                cell.atlasSlotUv[2] = 1.0f;
                cell.atlasSlotUv[3] = 1.0f;
                cell.tintRgba[0] = 0.30f;
                cell.tintRgba[1] = 0.30f;
                cell.tintRgba[2] = 0.35f;
                cell.tintRgba[3] = 1.0f;
            }
            // Cards stay fully opaque while they lerp from full-screen
            // (factor = 0) to their settled cells (factor = 1). The
            // motion is the dominant animation, not a fade. At factor = 0
            // the current VD's card (drawn last) covers the screen with
            // the current wallpaper, so the cross-fade into the real
            // desktop on the very next frame is a no-op visually.
            cell.opacity = 1.0f;
            setCornerRadiusUv(cell, fbSize, roundedRadiusPx(kRoundedRadiusCardFrac));
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(cell), &cell);
            vkCmdDraw(cmd, 4, 1, 0, 0);

            // Draw the VD's windows on top of its card at their real
            // positions, mirroring V1's Grid View (each card is a live
            // mini-desktop). Same mapping as the overview bar mini-
            // thumbnails: frameGeometry into the card rect, slot UV
            // cropped to the frame (drops the decoration-shadow halo),
            // drawn bottom-to-top by the frozen stacking snapshot.
            const QRect vScreen = effects->virtualScreenGeometry();
            const float vsW = std::max(1.0f, float(vScreen.width()));
            const float vsH = std::max(1.0f, float(vScreen.height()));
            struct GridWin
            {
                VulkanThumbnailAtlas::Slot slot;
                QRectF frame;
                QRectF vis;
                int z;
            };
            std::vector<GridWin> gwins;
            for (EffectWindow *ew : effects->stackingOrder()) {
                if (!ew || !ew->isOnDesktop(vd)) {
                    continue;
                }
                Window *h = ew->window();
                if (!h) {
                    continue;
                }
                auto sit = m_barThumbSlots.find(h);
                if (sit == m_barThumbSlots.end()) {
                    continue;
                }
                const auto &slot = sit->second;
                if (slot.isFallback || !slot.hasContent) {
                    continue;
                }
                const QRectF frame = h->frameGeometry();
                const QRectF vis = h->visibleGeometry();
                if (frame.isEmpty() || vis.isEmpty()) {
                    continue;
                }
                auto zit = m_stackingIndex.find(h);
                gwins.push_back({slot, frame, vis, zit != m_stackingIndex.end() ? zit->second : 0});
            }
            std::stable_sort(gwins.begin(), gwins.end(),
                             [](const GridWin &a, const GridWin &b) {
                return a.z < b.z;
            });
            const float innerX = cellLeft;
            const float innerY = cellTop;
            const float innerW = cellRight - cellLeft;
            const float innerH = cellBottom - cellTop;
            for (const GridWin &g : gwins) {
                const float relX = float(g.frame.x() - vScreen.x()) / vsW;
                const float relY = float(g.frame.y() - vScreen.y()) / vsH;
                const float relW = float(g.frame.width()) / vsW;
                const float relH = float(g.frame.height()) / vsH;
                const float wx = innerX + std::clamp(relX, 0.0f, 1.0f) * innerW;
                const float wy = innerY + std::clamp(relY, 0.0f, 1.0f) * innerH;
                const float ww = std::clamp(relW, 0.0f, 1.0f) * innerW;
                const float wh = std::clamp(relH, 0.0f, 1.0f) * innerH;
                if (ww <= 0.0f || wh <= 0.0f) {
                    continue;
                }
                const float ixL = std::clamp(float((g.frame.x() - g.vis.x()) / g.vis.width()), 0.0f, 1.0f);
                const float iyT = std::clamp(float((g.frame.y() - g.vis.y()) / g.vis.height()), 0.0f, 1.0f);
                const float ixR = std::clamp(float((g.vis.right() - g.frame.right()) / g.vis.width()), 0.0f, 1.0f);
                const float iyB = std::clamp(float((g.vis.bottom() - g.frame.bottom()) / g.vis.height()), 0.0f, 1.0f);
                OverviewQuadPushConstants w{};
                w.quadRectNdc[0] = wx;
                w.quadRectNdc[1] = wy;
                w.quadRectNdc[2] = ww;
                w.quadRectNdc[3] = wh;
                w.atlasSlotUv[0] = (float(g.slot.rect.x()) + float(g.slot.rect.width()) * ixL) / atlasSize;
                w.atlasSlotUv[1] = (float(g.slot.rect.y()) + float(g.slot.rect.height()) * iyT) / atlasSize;
                w.atlasSlotUv[2] = float(g.slot.rect.width()) * (1.0f - ixL - ixR) / atlasSize;
                w.atlasSlotUv[3] = float(g.slot.rect.height()) * (1.0f - iyT - iyB) / atlasSize;
                // The current desktop's windows stay fully opaque
                // throughout the lerp so the very last post-pass frame
                // (factor → 0, card filling the screen, windows at their
                // real positions) matches what kwin reveals on
                // m_visible = false — no pop-in of windows when the
                // overlay disappears. Non-current desktops' windows
                // fade with factor as their cards shrink / grow.
                w.opacity = (vd == currentDesktop) ? 1.0f : factor;
                setCornerRadiusUv(w, fbSize, roundedRadiusPx(kRoundedRadiusMiniFrac));
                vkCmdPushConstants(cmd, m_vkPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(w), &w);
                vkCmdDraw(cmd, 4, 1, 0, 0);
            }
        }
        return true;
    }

    // Pass 2.7: wallpaper-card drop shadow. V1's
    // Kirigami.ShadowedTexture wraps the per-VD card with a soft
    // 2-grid-unit drop at alpha 0.3. The shader's edgeSoftnessUv path
    // is plumbed for a proper inset-SDF soft fade but not yet used
    // here -- this draws a slightly-larger rounded rect behind the
    // card at low opacity instead. The larger pixel radius
    // (kRoundedRadiusCardPx + kShadowSpreadPx) gives a visible halo
    // beyond the card's own rounded edge, which reads as depth at
    // 0.25 alpha even without true Gaussian falloff. Only drawn when
    // the activation factor is meaningfully forward so it doesn't
    // flash during fast cycle transitions.
    // Proper rounded-box SDF drop shadow: one draw, soft Gaussian-
    // like alpha falloff outside the card's rounded edge via the
    // shader's shadowInnerHalfUv + edgeSoftnessUv path. Quad expands
    // past the card by kShadowSpreadPx on each side, plus a small
    // downward Y offset so the brightest point of the shadow falls
    // below the card (Breeze decoration shadow shape).
    if (factor > 0.05f) {
        constexpr float kShadowSpreadPx = 32.0f;
        constexpr float kShadowYOffsetPx = 8.0f;
        constexpr float kShadowAlpha = 0.55f;
        OverviewQuadPushConstants shadow{};
        setupShadowPc(shadow, fbSize, cardLeft, cardTop,
                      cardRight - cardLeft, cardBottom - cardTop,
                      roundedRadiusPx(kRoundedRadiusCardFrac),
                      kShadowSpreadPx, kShadowYOffsetPx,
                      kShadowAlpha * factor);
        vkCmdPushConstants(cmd, m_vkPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(shadow), &shadow);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }

    OverviewQuadPushConstants card{};
    card.quadRectNdc[0] = cardLeft;
    card.quadRectNdc[1] = cardTop;
    card.quadRectNdc[2] = cardRight - cardLeft;
    card.quadRectNdc[3] = cardBottom - cardTop;
    if (wp.slot.isFallback) {
        card.atlasSlotUv[0] = 0.0f;
        card.atlasSlotUv[1] = 0.0f;
        card.atlasSlotUv[2] = 1.0f;
        card.atlasSlotUv[3] = 1.0f;
    } else {
        card.atlasSlotUv[0] = float(wp.slot.rect.x()) / atlasSize;
        card.atlasSlotUv[1] = float(wp.slot.rect.y()) / atlasSize;
        card.atlasSlotUv[2] = float(wp.slot.rect.width()) / atlasSize;
        card.atlasSlotUv[3] = float(wp.slot.rect.height()) / atlasSize;
    }
    // Sharp wallpaper card is opaque throughout. It grows from full-
    // screen (factor = 0) to the grid area (factor = 1); at factor = 0
    // it fully covers the opaque blurred backdrop with the sharp
    // wallpaper, so the overlay's first/last frame matches the live
    // desktop exactly. Fading it with factor instead would double-
    // expose it against the backdrop and reintroduce the entry/exit
    // flicker this stage removes.
    card.opacity = 1.0f;
    // lod stays 0 — the card uses the sharp wallpaper, not the
    // blurred mip cascade.
    setCornerRadiusUv(card, fbSize, roundedRadiusPx(kRoundedRadiusCardFrac));
    vkCmdPushConstants(cmd, m_vkPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(card), &card);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    return true;
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
    // GridView draws its content entirely in drawWallpaperBackground:
    // the per-VD cards take the place of the window-grid heap and the
    // desktop bar. Skip the rest of this function to avoid rendering
    // the Overview-mode bar / heap / outlines / labels / search bar
    // on top of the GridView layout. Search + drag-tile are
    // intentional follow-ups (stage 10b in the parity execution plan).
    if (m_mode == Mode::GridView) {
        return;
    }
    if (!m_atlas || !m_vulkanCtx) {
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
    // Keep rendering while a search filter is active even if it currently
    // matches nothing: the search field is always-visible, and bailing here
    // (WindowClass mode has no bar, so m_barTiles is always empty) would make
    // the search bar + "no matching windows" placeholder vanish until the
    // user backspaced the query back to a matching prefix.
    if (m_tileLayout.empty() && m_barTiles.empty() && m_searchText.isEmpty()) {
        return;
    }
    // When activating on an empty desktop, seed focus on the bar so
    // the user can immediately see what arrow keys will navigate.
    // With grid tiles available, leave focus unset and let the first
    // Tab/arrow pick — avoids a visible highlight the moment overview
    // opens. Auto-pick the current desktop's bar tile so Enter is a
    // no-op on first activation (rather than switching elsewhere).
    if (m_focusZone == FocusZone::None && m_tileLayout.empty()
        && !m_barTiles.empty()) {
        m_focusZone = FocusZone::Bar;
        m_focusedIndex = 0;
        for (size_t i = 0; i < m_barTiles.size(); ++i) {
            if (m_barTiles[i].isCurrent) {
                m_focusedIndex = int(i);
                break;
            }
        }
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

    const float screenW = std::max(1.0f, float(fbSize.width()));
    const float screenH = std::max(1.0f, float(fbSize.height()));
    // Drag offset in NDC for the tile being dragged. Recomputed here
    // so pushAndDraw stays focused on push-constant assembly.
    const QPointF dragOffsetNdc = (m_dragActive)
        ? QPointF(2.0 * (m_dragCurrentGlobal.x() - m_dragPressGlobal.x()) / screenW,
                  2.0 * (m_dragCurrentGlobal.y() - m_dragPressGlobal.y()) / screenH)
        : QPointF(0, 0);
    auto pushAndDraw = [&](const TileLayout &t, float uvX, float uvY, float uvW, float uvH) {
        const bool dragging = m_dragActive && t.handle == m_dragCandidate;
        const float baseNdcX = t.realNdcX + (t.gridNdcX - t.realNdcX) * factor
            + (dragging ? float(dragOffsetNdc.x()) : 0.0f);
        const float baseNdcY = t.realNdcY + (t.gridNdcY - t.realNdcY) * factor
            + (dragging ? float(dragOffsetNdc.y()) : 0.0f);
        const float baseNdcW = t.realNdcW + (t.gridNdcW - t.realNdcW) * factor;
        const float baseNdcH = t.realNdcH + (t.gridNdcH - t.realNdcH) * factor;

        // The atlas slot was reserved at visibleGeometry size, so it
        // contains the window's decoration drop-shadow halo as part
        // of the rendered content. That halo reads as a chunky black
        // outline on every tile when it shouldn't be visible. Inset
        // both the drawn tile rect AND the UV sample by the frame-vs-
        // visible ratios so we crop the shadow halo away cleanly --
        // tile = frame portion only, no halo.
        float ixL = 0.0f, iyT = 0.0f, ixR = 0.0f, iyB = 0.0f;
        if (t.handle) {
            const QRectF vis = t.handle->visibleGeometry();
            const QRectF frame = t.handle->frameGeometry();
            if (vis.width() > 0.0 && vis.height() > 0.0) {
                ixL = float((frame.x() - vis.x()) / vis.width());
                iyT = float((frame.y() - vis.y()) / vis.height());
                ixR = float((vis.right() - frame.right()) / vis.width());
                iyB = float((vis.bottom() - frame.bottom()) / vis.height());
                ixL = std::max(0.0f, ixL);
                iyT = std::max(0.0f, iyT);
                ixR = std::max(0.0f, ixR);
                iyB = std::max(0.0f, iyB);
            }
        }
        const float tileNdcX = baseNdcX + baseNdcW * ixL;
        const float tileNdcY = baseNdcY + baseNdcH * iyT;
        const float tileNdcW = baseNdcW * (1.0f - ixL - ixR);
        const float tileNdcH = baseNdcH * (1.0f - iyT - iyB);
        const float tileRadiusPx = roundedRadiusPx(kRoundedRadiusTileFrac);

        // Subtle SDF drop shadow under each grid tile so the tiles
        // read as floating against the wallpaper card. Now that the
        // baked decoration-shadow halo is cropped away (UV inset
        // above), this is purely additive instead of doubling with
        // the existing halo.
        if (factor > 0.05f) {
            constexpr float kTileShadowSpreadPx = 10.0f;
            constexpr float kTileShadowYOffsetPx = 4.0f;
            constexpr float kTileShadowAlpha = 0.30f;
            OverviewQuadPushConstants shadow{};
            setupShadowPc(shadow, fbSize, tileNdcX, tileNdcY, tileNdcW, tileNdcH,
                          tileRadiusPx, kTileShadowSpreadPx, kTileShadowYOffsetPx,
                          kTileShadowAlpha * factor);
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(shadow), &shadow);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }

        OverviewQuadPushConstants pc{};
        pc.quadRectNdc[0] = tileNdcX;
        pc.quadRectNdc[1] = tileNdcY;
        pc.quadRectNdc[2] = tileNdcW;
        pc.quadRectNdc[3] = tileNdcH;
        setCornerRadiusUv(pc, fbSize, tileRadiusPx);
        pc.atlasSlotUv[0] = uvX + uvW * ixL;
        pc.atlasSlotUv[1] = uvY + uvH * iyT;
        pc.atlasSlotUv[2] = uvW * (1.0f - ixL - ixR);
        pc.atlasSlotUv[3] = uvH * (1.0f - iyT - iyB);
        // Heap tiles stay fully opaque and only lerp their position
        // (realNdc -> gridNdc), so each window visibly travels from
        // its real on-screen rect into its grid cell on entry and back
        // out on exit instead of cross-fading. At factor = 0 the tiles
        // sit at their real positions over the full-screen sharp
        // wallpaper card, matching the live desktop the overlay
        // covers/reveals — the desktop is only ever swapped at the
        // animation endpoints (m_visible flip), so there's no fade or
        // bleed-through during the zoom. Mirrors the Grid View
        // treatment (cards/windows clamp opacity, lerp position only).
        pc.opacity = 1.0f;
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
    // The dragged tile is skipped in both passes and re-drawn last so
    // it lands on top of every other grid tile regardless of natural
    // iteration order.
    auto isDragged = [&](const TileLayout &t) {
        return m_dragActive && t.handle == m_dragCandidate;
    };
    if (atlasSrgbView != VK_NULL_HANDLE) {
        pushView(atlasSrgbView);
        for (const TileLayout &t : m_tileLayout) {
            if (t.slot.isFallback || isDragged(t)) {
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
        if (!t.slot.isFallback || isDragged(t)) {
            continue;
        }
        pushView(t.slot.srgbView);
        pushAndDraw(t, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    // Pass 2.5: hover / keyboard-focus outline on grid tiles.
    // V1's WindowHeapDelegate draws a Rectangle border with
    // Kirigami.Theme.highlightColor when the tile is hovered or
    // selected — same predicate here. Four thin solid-colour rects
    // around the tile rect (top, bottom, left, right) form the
    // border without needing a rounded-rect shader. Bar focus wash
    // still draws after the bar pass below.
    // Theme-driven border colours: highlightColor for active state
    // (focused / hovered / current / drop-target), text color (with
    // reduced opacity) for the always-on resting border. Read from
    // the application palette so the user's KDE colour scheme drives
    // the outline appearance — no hardcoded RGB.
    const QColor highlight = qApp->palette().highlight().color();
    const QColor textColor = qApp->palette().windowText().color();
    auto pushOutline = [&](float ndcX, float ndcY, float ndcW, float ndcH,
                           float borderPx, const QColor &c, float alpha) {
        const float bx = 2.0f * borderPx / screenW;
        const float by = 2.0f * borderPx / screenH;
        OverviewQuadPushConstants hl{};
        hl.atlasSlotUv[0] = 0.0f;
        hl.atlasSlotUv[1] = 0.0f;
        hl.atlasSlotUv[2] = 1.0f;
        hl.atlasSlotUv[3] = 1.0f;
        hl.tintRgba[0] = float(c.redF());
        hl.tintRgba[1] = float(c.greenF());
        hl.tintRgba[2] = float(c.blueF());
        hl.tintRgba[3] = 1.0f;
        hl.opacity = alpha * factor;
        auto drawRect = [&](float x, float y, float w, float h) {
            hl.quadRectNdc[0] = x;
            hl.quadRectNdc[1] = y;
            hl.quadRectNdc[2] = w;
            hl.quadRectNdc[3] = h;
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(hl), &hl);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        };
        drawRect(ndcX - bx, ndcY - by, ndcW + 2.0f * bx, by); // top
        drawRect(ndcX - bx, ndcY + ndcH, ndcW + 2.0f * bx, by); // bottom
        drawRect(ndcX - bx, ndcY, bx, ndcH); // left
        drawRect(ndcX + ndcW, ndcY, bx, ndcH); // right
    };
    auto pushHighlight = [&](float ndcX, float ndcY, float ndcW, float ndcH) {
        // Kirigami.Units.largeSpacing — same width V1's
        // WindowHeapDelegate uses for the hover/focus border.
        pushOutline(ndcX, ndcY, ndcW, ndcH, 12.0f, highlight, 1.0f);
    };
    for (size_t i = 0; i < m_tileLayout.size(); ++i) {
        const bool hovered = (int(i) == m_hoveredTileIndex);
        const bool focused = (m_focusZone == FocusZone::Grid && int(i) == m_focusedIndex);
        if (!hovered && !focused) {
            continue;
        }
        const TileLayout &t = m_tileLayout[i];
        if (isDragged(t) || !t.handle) {
            continue;
        }
        // Outline hugs the frame (window + decorations) rather than
        // the visibleGeometry (which includes the drop-shadow halo)
        // — without this, the border sits ~shadow-margin pixels off
        // the visible window edge. Compute the inset ratios from the
        // window's frame vs visible rect and apply them in tile NDC
        // space.
        const float tileNdcX = t.realNdcX + (t.gridNdcX - t.realNdcX) * factor;
        const float tileNdcY = t.realNdcY + (t.gridNdcY - t.realNdcY) * factor;
        const float tileNdcW = t.realNdcW + (t.gridNdcW - t.realNdcW) * factor;
        const float tileNdcH = t.realNdcH + (t.gridNdcH - t.realNdcH) * factor;
        const QRectF vis = t.handle->visibleGeometry();
        const QRectF frame = t.handle->frameGeometry();
        float insetX = 0.0f, insetY = 0.0f, insetW = 0.0f, insetH = 0.0f;
        if (vis.width() > 0.0 && vis.height() > 0.0) {
            const float rx = float((frame.x() - vis.x()) / vis.width());
            const float ry = float((frame.y() - vis.y()) / vis.height());
            const float rw = float((vis.right() - frame.right()) / vis.width());
            const float rh = float((vis.bottom() - frame.bottom()) / vis.height());
            insetX = rx * tileNdcW;
            insetY = ry * tileNdcH;
            insetW = rw * tileNdcW;
            insetH = rh * tileNdcH;
        }
        pushHighlight(tileNdcX + insetX,
                      tileNdcY + insetY,
                      tileNdcW - insetX - insetW,
                      tileNdcH - insetY - insetH);
    }

    // Pass 2.6: per-tile labels (icon + caption) beneath each tile.
    // Mirrors V1's WindowHeapDelegate: a large window icon stacked
    // above a one-line caption pill. Each label texture is cached
    // per-window (m_tileLabels) and re-rendered only when the caption
    // changes. Drawn AFTER the highlight outline (pass 2.5) so the
    // icon + title stay on top of the hover/selection border, matching
    // V1's stacking. Skipped for the dragged tile so it doesn't ghost
    // on top of the drop-target landing area.
    if (factor > 0.05f) {
        const int gridUnitPx = QFontMetrics(qApp->font()).height();
        // V1 anchors the icon's vertical centre to the thumbnail bottom
        // with verticalCenterOffset -iconHeight/4, so the icon overlaps
        // the thumbnail by 3/4 of its height and pokes 1/4 below. Our
        // label texture stacks the icon at its top, so to reproduce
        // that the texture top sits 3/4 of an icon height above the
        // visible window bottom edge.
        const int kIconSidePx = 3 * gridUnitPx;
        const float iconOverlapNdc = 2.0f * (0.75f * float(kIconSidePx)) / screenH;
        for (const TileLayout &t : m_tileLayout) {
            if (!t.handle || isDragged(t)) {
                continue;
            }
            VulkanTexture *labelTex = ensureTileLabelTexture(t.handle);
            if (!labelTex || !labelTex->isValid()) {
                continue;
            }
            const float texW = float(labelTex->width());
            const float texH = float(labelTex->height());
            const float labelHeightNdc = 2.0f * texH / screenH;
            const float labelWidthNdc = 2.0f * texW / screenW;
            const float tileNdcX = t.realNdcX + (t.gridNdcX - t.realNdcX) * factor;
            const float tileNdcY = t.realNdcY + (t.gridNdcY - t.realNdcY) * factor;
            const float tileNdcW = t.realNdcW + (t.gridNdcW - t.realNdcW) * factor;
            const float tileNdcH = t.realNdcH + (t.gridNdcH - t.realNdcH) * factor;
            // The tile rect spans visibleGeometry (which includes the
            // decoration-shadow halo); the visible window occupies the
            // inset frame rect within it. Position the label relative
            // to that inset rect — its centre and bottom edge — so the
            // gap to the visible window border is uniform regardless of
            // each window's shadow halo size (matches the tile-draw
            // crop in pushAndDraw).
            float ixL = 0.0f, ixR = 0.0f, iyB = 0.0f;
            {
                const QRectF vis = t.handle->visibleGeometry();
                const QRectF frame = t.handle->frameGeometry();
                if (vis.width() > 0.0 && vis.height() > 0.0) {
                    ixL = std::max(0.0f, float((frame.x() - vis.x()) / vis.width()));
                    ixR = std::max(0.0f, float((vis.right() - frame.right()) / vis.width()));
                    iyB = std::max(0.0f, float((vis.bottom() - frame.bottom()) / vis.height()));
                }
            }
            const float winNdcX = tileNdcX + tileNdcW * ixL;
            const float winNdcW = tileNdcW * (1.0f - ixL - ixR);
            const float winBottomNdc = tileNdcY + tileNdcH * (1.0f - iyB);
            // Draw at native aspect, centred under the visible window.
            // If the label is wider than the window, clip the centre
            // band by shrinking the drawn width and sampling the centre
            // UV slice — keeps icon+text in proportion when constrained.
            const float lblH = labelHeightNdc;
            const float lblW = std::min(labelWidthNdc, winNdcW);
            const float lblX = winNdcX + (winNdcW - lblW) * 0.5f;
            const float lblY = winBottomNdc - iconOverlapNdc;
            const float uvScale = lblW / labelWidthNdc;
            const float uvX = (1.0f - uvScale) * 0.5f;
            pushView(labelTex->imageView());
            OverviewQuadPushConstants pc{};
            pc.quadRectNdc[0] = lblX;
            pc.quadRectNdc[1] = lblY;
            pc.quadRectNdc[2] = lblW;
            pc.quadRectNdc[3] = lblH;
            pc.atlasSlotUv[0] = uvX;
            pc.atlasSlotUv[1] = 0.0f;
            pc.atlasSlotUv[2] = uvScale;
            pc.atlasSlotUv[3] = 1.0f;
            // tintRgba.a = 0 → pure texture sample.
            pc.opacity = factor;
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
        // Rebind the atlas view for the bar pass below (pushOutline /
        // bar tiles don't sample the atlas — tintRgba.a is 1.0 — but
        // the pipeline still needs a valid descriptor bound).
        if (atlasSrgbView != VK_NULL_HANDLE) {
            pushView(atlasSrgbView);
        }
    }

    // Bar-tile focus wash drawn after the bar pass below so it lands
    // on top of the bar tile's own colour.

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
        if (anyView == VK_NULL_HANDLE) {
            // Empty current desktop: no slot views to borrow. Force-
            // initialise the atlas to get its own view so the bar can
            // still draw — costs the atlas image's VRAM (~85 MB) on
            // the first frame, but the user is actively staring at
            // the overview so the trade is reasonable.
            anyView = m_atlas->atlasView();
        }
        if (anyView != VK_NULL_HANDLE) {
            pushView(anyView);
            renderDesktopBar(cmd, fbSize);
            // Per-bar-tile outline. V1's DesktopBar.qml draws a 1px
            // text-color border at rest and a 2px highlight-color
            // border in the "active" state (current desktop / focused
            // / drop-target / pressed). Same predicate here, themed
            // colours from the system palette.
            VirtualDesktop *dragOver = m_dragActive ? hitTestBar(m_dragCurrentGlobal) : nullptr;
            for (size_t i = 0; i < m_barTiles.size(); ++i) {
                const BarTile &b = m_barTiles[i];
                const bool keyboardFocused = (m_focusZone == FocusZone::Bar
                                              && int(i) == m_focusedIndex);
                const bool hovered = (!m_dragActive && int(i) == m_hoveredBarIndex);
                const bool dropTarget = (dragOver && b.desktop == dragOver
                                         && dragOver != effects->currentDesktop());
                const bool active = b.isCurrent || keyboardFocused || hovered || dropTarget;
                if (active) {
                    pushOutline(b.ndcX, b.ndcY, b.ndcW, b.ndcH, 3.0f, highlight, 1.0f);
                } else {
                    pushOutline(b.ndcX, b.ndcY, b.ndcW, b.ndcH, 1.0f, textColor, 0.5f);
                }
            }
            // Drop-target visual feedback is now part of the per-tile
            // outline above (active = current / focused / drop-target).
        }
    }

    // Per-tile "×" delete affordance overlay. Only drawn for the
    // bar tile (or affordance) currently hovered, and only when
    // there's more than one desktop (the last one can't be removed).
    // Uses its own image view; the icon texture is shared with the
    // Add-VD pass via ensureBarIconTextures (called below).
    if (!m_dragActive
        && m_deleteAffordanceHover >= 0 && m_deleteAffordanceHover < int(m_barTiles.size())
        && m_barTiles.size() > 1) {
        ensureBarIconTextures();
        if (m_deleteIconTexture && m_deleteIconTexture->isValid()) {
            const BarTile &b = m_barTiles[m_deleteAffordanceHover];
            const QRectF aff = deleteAffordanceNdc(b.ndcX, b.ndcY, b.ndcW, b.ndcH);
            pushView(m_deleteIconTexture->imageView());
            OverviewQuadPushConstants pc{};
            pc.quadRectNdc[0] = float(aff.x());
            pc.quadRectNdc[1] = float(aff.y());
            pc.quadRectNdc[2] = float(aff.width());
            pc.quadRectNdc[3] = float(aff.height());
            pc.atlasSlotUv[0] = 0.0f;
            pc.atlasSlotUv[1] = 0.0f;
            pc.atlasSlotUv[2] = 1.0f;
            pc.atlasSlotUv[3] = 1.0f;
            pc.opacity = factor;
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
    }

    // Add-VD "+" icon overlay. Drawn after the bar pass so it stacks
    // on top of the add-tile background. Uses its own image view
    // (pushView swaps the binding); UV is the full texture.
    ensureBarIconTextures();
    if (!m_addTileNdc.isEmpty() && m_addIconTexture && m_addIconTexture->isValid()) {
        pushView(m_addIconTexture->imageView());
        // Square icon (theme "list-add" rendered at 64×64). The tile
        // rect is square in NDC but rectangular in pixels because NDC
        // X and Y scale by different framebuffer half-extents — fit
        // the icon to the smaller pixel dimension so it stays square
        // on screen instead of stretching horizontally.
        const float iconFrac = 0.55f;
        const float screenWf = std::max(1.0f, float(fbSize.width()));
        const float screenHf = std::max(1.0f, float(fbSize.height()));
        const float tilePxW = float(m_addTileNdc.width()) * 0.5f * screenWf;
        const float tilePxH = float(m_addTileNdc.height()) * 0.5f * screenHf;
        const float iconPx = iconFrac * std::min(tilePxW, tilePxH);
        const float iconNdcW = 2.0f * iconPx / screenWf;
        const float iconNdcH = 2.0f * iconPx / screenHf;
        const float iconNdcX = float(m_addTileNdc.x())
            + (float(m_addTileNdc.width()) - iconNdcW) * 0.5f;
        const float iconNdcY = float(m_addTileNdc.y())
            + (float(m_addTileNdc.height()) - iconNdcH) * 0.5f;
        OverviewQuadPushConstants pc{};
        pc.quadRectNdc[0] = iconNdcX;
        pc.quadRectNdc[1] = iconNdcY;
        pc.quadRectNdc[2] = iconNdcW;
        pc.quadRectNdc[3] = iconNdcH;
        pc.atlasSlotUv[0] = 0.0f;
        pc.atlasSlotUv[1] = 0.0f;
        pc.atlasSlotUv[2] = 1.0f;
        pc.atlasSlotUv[3] = 1.0f;
        // tintRgba.a = 0 → pure texture sample (premultiplied alpha
        // from the QPainter render).
        pc.opacity = factor;
        vkCmdPushConstants(cmd, m_vkPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }

    // Search bar — only drawn while a filter is active. Lazily build /
    // update the texture, then bind its dedicated SRGB view and draw a
    // single quad centred between the desktop bar and the grid.
    updateSearchTexture();
    if (m_searchTexture && m_searchTexture->isValid()) {
        pushView(m_searchTexture->imageView());
        // NDC search-bar geometry. V1's topBar anchors to
        // desktopBar.bottom + largeSpacing (= 1 gridUnit), so the
        // search bar sits 1 gridUnit below the bar zone (bar tile +
        // label strip). We mirror that with m_barTopNdc /
        // m_barHeightNdc / m_barLabelStripNdc populated by
        // rebuildBarLayout. Width samples the texture 1:1 so text
        // stays crisp; the texture itself is gridUnit-sized in
        // updateSearchTexture.
        const float texPxW = float(m_searchTextureSize.width());
        const float texPxH = float(m_searchTextureSize.height());
        const float screenW = std::max(1.0f, float(fbSize.width()));
        const float screenH = std::max(1.0f, float(fbSize.height()));
        const float ndcW = 2.0f * texPxW / screenW;
        const float ndcH = 2.0f * texPxH / screenH;
        const float ndcX = -ndcW * 0.5f;
        const float gridUnitNdcY = 2.0f * float(QFontMetrics(qApp->font()).height())
            / screenH;
        float ndcY;
        if (m_mode == Mode::WindowClass) {
            // No desktop bar in this mode, so anchor the search field one
            // gridUnit below the screen top — matching windowview's
            // ColumnLayout, where the SearchField uses
            // Layout.topMargin = Kirigami.Units.gridUnit.
            ndcY = -1.0f + gridUnitNdcY;
        } else {
            const float barZoneBottom = m_barTopNdc + m_barHeightNdc + m_barLabelStripNdc;
            ndcY = barZoneBottom + gridUnitNdcY * 0.5f;
        }
        OverviewQuadPushConstants pc{};
        pc.quadRectNdc[0] = ndcX;
        pc.quadRectNdc[1] = ndcY;
        pc.quadRectNdc[2] = ndcW;
        pc.quadRectNdc[3] = ndcH;
        pc.atlasSlotUv[0] = 0.0f;
        pc.atlasSlotUv[1] = 0.0f;
        pc.atlasSlotUv[2] = 1.0f;
        pc.atlasSlotUv[3] = 1.0f;
        // tintRgba.a = 0 → pure texture sample, no tint mix.
        pc.opacity = factor;
        vkCmdPushConstants(cmd, m_vkPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }

    // "No matching windows" placeholder. V1 shows it whenever the
    // active filter has zero results (PlaceholderMessage at main.qml
    // line 738); we draw it just below where the search bar sits so
    // it reads as the empty-state for the search rather than a
    // generic empty-grid message. Skipped when the search field is
    // empty -- the grid is just sparse / empty, not "no matches".
    if (m_tileLayout.empty() && !m_searchText.isEmpty()) {
        ensureNoMatchesTexture();
        if (m_noMatchesTexture && m_noMatchesTexture->isValid()) {
            pushView(m_noMatchesTexture->imageView());
            const float texPxW = float(m_noMatchesTextureSize.width());
            const float texPxH = float(m_noMatchesTextureSize.height());
            const float ndcW = 2.0f * texPxW / std::max(1.0f, float(fbSize.width()));
            const float ndcH = 2.0f * texPxH / std::max(1.0f, float(fbSize.height()));
            const float ndcX = -ndcW * 0.5f;
            // Mirror V1: PlaceholderMessage anchored to the heap
            // loader's parent.top, i.e. immediately below the topBar
            // with no gap. Search-bar quad bottom in V2 ≈ -0.72, so
            // top of placeholder texture starts there. Texture
            // extends down into what would be the grid area (which
            // is empty in the no-matches state).
            const float ndcY = -0.72f;
            OverviewQuadPushConstants pc{};
            pc.quadRectNdc[0] = ndcX;
            pc.quadRectNdc[1] = ndcY;
            pc.quadRectNdc[2] = ndcW;
            pc.quadRectNdc[3] = ndcH;
            pc.atlasSlotUv[0] = 0.0f;
            pc.atlasSlotUv[1] = 0.0f;
            pc.atlasSlotUv[2] = 1.0f;
            pc.atlasSlotUv[3] = 1.0f;
            pc.opacity = factor;
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
    }

    // The dragged grid tile, drawn last so it floats above every
    // other grid tile, the desktop bar, and the search bar — the user
    // always sees what they're holding, even when the cursor crosses
    // into the bar zone to drop on a different desktop's tile. Shrinks
    // toward the bar as the cursor approaches it so drop-target wash
    // on bar tiles stays visible — matches V1's targetScale (the QML
    // overview's main.qml:662-675).
    if (m_dragActive) {
        // Shrink is the ratio of the cursor's remaining distance to
        // the bar over the original press distance. At press position
        // scale = 1; at the bar bottom scale clamps to 0.1 so the
        // tile stays visible. The press point on the tile follows the
        // cursor so what the user grabbed stays under their finger.
        const QRect screen = effects ? effects->virtualScreenGeometry() : QRect();
        float dragScale = 1.0f;
        float cursorNdcX = 0.0f;
        float cursorNdcY = 0.0f;
        float pressXNdc = 0.0f;
        float pressYNdc = 0.0f;
        if (screen.height() > 0 && screen.width() > 0) {
            const float barBottomPx = float(screen.y()) + 0.10f * float(screen.height());
            const float localPress = std::max(1.0f, float(m_dragPressGlobal.y()) - barBottomPx);
            const float localPos = float(m_dragCurrentGlobal.y()) - barBottomPx;
            dragScale = std::clamp(localPos / localPress, 0.1f, 1.0f);
            cursorNdcX = -1.0f + 2.0f * float(m_dragCurrentGlobal.x() - screen.x()) / screenW;
            cursorNdcY = -1.0f + 2.0f * float(m_dragCurrentGlobal.y() - screen.y()) / screenH;
            pressXNdc = -1.0f + 2.0f * float(m_dragPressGlobal.x() - screen.x()) / screenW;
            pressYNdc = -1.0f + 2.0f * float(m_dragPressGlobal.y() - screen.y()) / screenH;
        }
        for (const TileLayout &t : m_tileLayout) {
            if (t.handle != m_dragCandidate) {
                continue;
            }
            const float naturalX = t.realNdcX + (t.gridNdcX - t.realNdcX) * factor;
            const float naturalY = t.realNdcY + (t.gridNdcY - t.realNdcY) * factor;
            const float naturalW = t.realNdcW + (t.gridNdcW - t.realNdcW) * factor;
            const float naturalH = t.realNdcH + (t.gridNdcH - t.realNdcH) * factor;
            const float pressOnTileX = pressXNdc - naturalX;
            const float pressOnTileY = pressYNdc - naturalY;
            const float w = naturalW * dragScale;
            const float h = naturalH * dragScale;
            const float x = cursorNdcX - pressOnTileX * dragScale;
            const float y = cursorNdcY - pressOnTileY * dragScale;
            float uvX = 0.0f, uvY = 0.0f, uvW = 1.0f, uvH = 1.0f;
            if (t.slot.isFallback) {
                pushView(t.slot.srgbView);
            } else if (atlasSrgbView != VK_NULL_HANDLE) {
                pushView(atlasSrgbView);
                uvX = float(t.slot.rect.x()) / atlasSize;
                uvY = float(t.slot.rect.y()) / atlasSize;
                uvW = float(t.slot.rect.width()) / atlasSize;
                uvH = float(t.slot.rect.height()) / atlasSize;
            } else {
                break;
            }
            // Same shadow-halo crop as the resting grid tile: inset
            // the drawn rect AND UV by the source window's frame-vs-
            // visible ratios so the dragged tile shows only the
            // frame portion of the atlas slot (no Breeze decoration
            // shadow baked into the thumbnail).
            float ixL = 0.0f, iyT = 0.0f, ixR = 0.0f, iyB = 0.0f;
            if (t.handle) {
                const QRectF vis = t.handle->visibleGeometry();
                const QRectF frame = t.handle->frameGeometry();
                if (vis.width() > 0.0 && vis.height() > 0.0) {
                    ixL = float((frame.x() - vis.x()) / vis.width());
                    iyT = float((frame.y() - vis.y()) / vis.height());
                    ixR = float((vis.right() - frame.right()) / vis.width());
                    iyB = float((vis.bottom() - frame.bottom()) / vis.height());
                    ixL = std::max(0.0f, ixL);
                    iyT = std::max(0.0f, iyT);
                    ixR = std::max(0.0f, ixR);
                    iyB = std::max(0.0f, iyB);
                }
            }
            const float dragX = x + w * ixL;
            const float dragY = y + h * iyT;
            const float dragW = w * (1.0f - ixL - ixR);
            const float dragH = h * (1.0f - iyT - iyB);
            const float dragRadiusPx = roundedRadiusPx(kRoundedRadiusTileFrac);

            // SDF drop shadow under the dragged tile (larger spread
            // and higher alpha than the resting grid-tile shadow so
            // the dragged tile reads as lifted off the grid).
            {
                constexpr float kDragShadowSpreadPx = 18.0f;
                constexpr float kDragShadowYOffsetPx = 6.0f;
                constexpr float kDragShadowAlpha = 0.45f;
                OverviewQuadPushConstants ds{};
                setupShadowPc(ds, fbSize, dragX, dragY, dragW, dragH,
                              dragRadiusPx, kDragShadowSpreadPx, kDragShadowYOffsetPx,
                              kDragShadowAlpha * factor);
                vkCmdPushConstants(cmd, m_vkPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(ds), &ds);
                vkCmdDraw(cmd, 4, 1, 0, 0);
                // Re-bind the tile's image view since the shadow draw
                // doesn't read the texture but the next pc still
                // needs the slot view bound.
                if (t.slot.isFallback) {
                    pushView(t.slot.srgbView);
                } else {
                    pushView(atlasSrgbView);
                }
            }
            OverviewQuadPushConstants pc{};
            pc.quadRectNdc[0] = dragX;
            pc.quadRectNdc[1] = dragY;
            pc.quadRectNdc[2] = dragW;
            pc.quadRectNdc[3] = dragH;
            setCornerRadiusUv(pc, fbSize, dragRadiusPx);
            pc.atlasSlotUv[0] = uvX + uvW * ixL;
            pc.atlasSlotUv[1] = uvY + uvH * iyT;
            pc.atlasSlotUv[2] = uvW * (1.0f - ixL - ixR);
            pc.atlasSlotUv[3] = uvH * (1.0f - iyT - iyB);
            pc.opacity = factor;
            vkCmdPushConstants(cmd, m_vkPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
            break;
        }
    }
}
#endif // HAVE_VULKAN

void OverviewEffectV2::activate()
{
    activate(Mode::Overview);
}

void OverviewEffectV2::activate(Mode mode)
{
    // Capture the target window class up-front, before the keyboard grab
    // below steals focus from the active window. WindowClass mode presents
    // every window sharing this resourceClass across all virtual desktops.
    if (mode == Mode::WindowClass) {
        EffectWindow *active = effects ? effects->activeWindow() : nullptr;
        m_windowClassFilter = (active && active->window())
            ? active->window()->resourceClass()
            : QString();
    }

    // Mode change while already showing: just flip m_mode and request
    // a repaint. The activation animation has already played; the
    // follow-up FSM stage will add a real crossfade between modes.
    if (m_visible && m_animation.direction() == QVariantAnimation::Forward) {
        if (m_mode != mode) {
            m_mode = mode;
            effects->addRepaintFull();
        }
        return;
    }
    m_mode = mode;
    m_visible = true;
    m_focusZone = FocusZone::None; // first arrow/Tab picks tile 0 in grid
    m_focusedIndex = -1;
    m_searchText.clear();
    // Mouse interception (bar clicks, tile clicks, click-away
    // dismiss) and keyboard grab (Esc, Tab, arrows, Enter). The
    // earlier conditional-on-empty grab was a workaround for the
    // bar-click shortcut break — that root cause is now handled by
    // teardownImmediate(), and skipping the grab here broke the
    // arrow-leak nav into the bar on empty desktops. Grab always.
    effects->startMouseInterception(this, Qt::ArrowCursor);
    m_grabbedKeyboard = effects->grabKeyboard(this);
#if HAVE_VULKAN
    // Reserve wallpaper FIRST — it's the largest slot (full screen)
    // and needs contiguous atlas space. Reserving after the window /
    // bar-thumb slots fragments the atlas enough that the wallpaper
    // ends up in a fallback dedicated image, which the bar pass
    // can't sample because we share one descriptor binding across
    // all bar tiles.
    reserveWallpaperSlot();
    reserveSlotsForCurrentDesktop();
    reserveBarThumbs();
    // Live activity refresh: window slots are reserved at activate-
    // time, so an external activity switch (Plasma keybinding, KCM,
    // DBus) would otherwise leave V2 showing an empty grid (old slots
    // filtered out by isOnCurrentActivity, new activity's windows
    // never reserved). Release everything and re-reserve on the new
    // activity so the next frame paints correctly.
    QObject::disconnect(m_activityConnection);
    m_activityConnection = connect(effects, &EffectsHandler::currentActivityChanged,
                                   this, [this](const QString &) {
        if (!m_visible) {
            return;
        }
        releaseAllSlots();
        reserveWallpaperSlot();
        reserveSlotsForCurrentDesktop();
        reserveBarThumbs();
        effects->addRepaintFull();
    });
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
                // would be undefined garbage. Prefer the wallpaper
                // slot — that's V1's behaviour (the underlying scene
                // with windows isn't shown through V2's backdrop). If
                // the wallpaper slot isn't populated yet (first frame
                // after activate, or no Desktop-class window matched)
                // fall back to dimming the scene capture so we still
                // paint a valid background.
                if (!drawWallpaperBackground(cmd, fbSize, fmt)) {
                    drawSceneCaptureBackground(cmd, sceneCapture, fbSize, fmt);
                }
                renderTilesPostPass(cmd, fbSize, fmt);
            });
        }
    }
#endif
    // Smooth-reverse from any in-flight slide-out: setDirection on a
    // running animation lets Qt continue from currentTime back the
    // other way without restarting from 0. stop()+start() snaps to
    // the endpoint first, which feels like a jump at slow durations.
    if (m_animation.state() == QAbstractAnimation::Running) {
        if (m_animation.direction() == QVariantAnimation::Backward) {
            m_animation.setDirection(QVariantAnimation::Forward);
        }
        // Already going Forward — let it finish; spam-pressing the
        // toggle must not stop+restart and snap to the start frame.
    } else {
        m_animation.stop();
        m_animation.setDirection(QVariantAnimation::Forward);
        m_animation.start();
    }
}

void OverviewEffectV2::deactivate()
{
    if (!m_visible) {
        return;
    }
#if HAVE_VULKAN
    // Activity-change tracking is no longer needed while we slide out;
    // the slot release at animation-finished handles the cleanup. The
    // preFrameRender connection deliberately stays live for the
    // duration of the slide-out so window thumbnails keep re-rendering
    // (live video, animated cursors, terminal output) instead of
    // freezing to a stale snapshot the instant exit is triggered.
    // m_preFrameConnection is disconnected in the m_animation.finished
    // handler alongside the post-pass unregister + slot release.
    QObject::disconnect(m_activityConnection);
    m_activityConnection = {};
#endif
    // Release input grabs at the start of deactivate; the user can
    // immediately interact with normal windows again while the slide-
    // out animation finishes drawing.
    effects->stopMouseInterception(this);
    if (m_grabbedKeyboard) {
        effects->ungrabKeyboard();
        m_grabbedKeyboard = false;
    }
    // Smooth-reverse from any in-flight slide-in: see activate() for
    // why we avoid stop()+start() when already running.
    if (m_animation.state() == QAbstractAnimation::Running) {
        if (m_animation.direction() == QVariantAnimation::Forward) {
            m_animation.setDirection(QVariantAnimation::Backward);
        }
        // Already going Backward — let it finish; spam-pressing
        // must not stop+restart and snap to fully-open before sliding.
    } else {
        m_animation.stop();
        m_animation.setDirection(QVariantAnimation::Backward);
        m_animation.start();
    }
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
    QObject::disconnect(m_activityConnection);
    m_activityConnection = {};
#endif
    effects->stopMouseInterception(this);
    if (m_grabbedKeyboard) {
        effects->ungrabKeyboard();
        m_grabbedKeyboard = false;
    }
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

void OverviewEffectV2::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags);
    // Read the V1 overview config group (Effect-overview in kwinrc)
    // so user-saved settings carry across without needing to
    // re-bind anything. New V2-specific knobs can live here too.
    const auto cfg = KSharedConfig::openConfig();
    const KConfigGroup group = cfg->group(QStringLiteral("Effect-overview"));

    // Electric borders: same key as V1 (BorderActivate). Re-reserve
    // on every reconfigure so a live settings change picks up.
    for (const ElectricBorder &border : std::as_const(m_borderActivate)) {
        effects->unreserveElectricBorder(border, this);
    }
    m_borderActivate.clear();
    const QList<int> activateBorders = group.readEntry(
        QStringLiteral("BorderActivate"),
        QList<int>{int(ElectricTopLeft)});
    for (const int border : activateBorders) {
        const ElectricBorder eb = ElectricBorder(border);
        m_borderActivate.append(eb);
        effects->reserveElectricBorder(eb, this);
    }

    // V1 parity knob: hide minimised windows from the grid + bar
    // when set. Per-tile filter applied in reserveSlotsForCurrentDesktop,
    // reserveBarThumbs and rebuildTileLayout. Default false matches V1.
    m_ignoreMinimized = group.readEntry(QStringLiteral("IgnoreMinimized"), false);

    // V1 parity knob: animation duration in ms. Effect::animationTime
    // reads the kwinrc key directly and only applies the user's global
    // animation-speed factor when the key is unset (matches V1 behavior
    // exactly — explicit user override wins, otherwise the global
    // slider scales the default). Clamp to 0 so a negative value reads
    // as instant. Setting on the running QVariantAnimation lets a
    // reconfigure take effect on the next activate without a restart.
    using namespace std::chrono_literals;
    const int duration = int(animationTime(group, QStringLiteral("AnimationDuration"), 400ms));
    m_animationDuration = std::max(0, duration);
    m_animation.setDuration(m_animationDuration);
}

bool OverviewEffectV2::borderActivated(ElectricBorder border)
{
    if (!m_borderActivate.contains(border)) {
        return false;
    }
    if (m_visible) {
        deactivate();
    } else {
        activate();
    }
    return true;
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

bool OverviewEffectV2::event(QEvent *e)
{
    if (e->type() == QEvent::ApplicationPaletteChange
        || e->type() == QEvent::ApplicationFontChange) {
        // Qt 6 dispatches ApplicationPaletteChange / ApplicationFontChange
        // to every QObject when QGuiApplication's palette or font updates
        // (KDE colour-scheme switch, system theme / font change).
        //
        // Paint-time samples of qApp->palette() (the grid/bar outlines)
        // pick up the new colours on the next repaint for free. But the
        // themed colours — and the font-derived gridUnit sizing — are
        // also baked into cached QPainter textures (tile labels, search
        // field, bar labels, the no-matches message and the bar icons),
        // whose cache keys are the rendered text only. A repaint alone
        // re-shows those stale textures, so drop the caches here to force
        // a rebuild with the current palette/font on the next frame.
        // event() runs on the main thread outside frame recording, and
        // the same texture-replacement happens routinely on text changes,
        // so clearing here is safe. Only matters when the theme/font
        // changes while the overview is open; a fresh activation already
        // rebuilds against the current values.
        if (m_visible) {
            m_tileLabels.clear();
            m_barLabels.clear();
            m_searchTexture.reset();
            m_searchRenderedText.clear();
            m_noMatchesTexture.reset();
            m_addIconTexture.reset();
            m_deleteIconTexture.reset();
            effects->addRepaintFull();
        }
    }
    return Effect::event(e);
}

void OverviewEffectV2::grabbedKeyboardEvent(QKeyEvent *event)
{
    if (event->type() != QEvent::KeyPress) {
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        // Escape with a non-empty search clears the filter rather
        // than dismissing — matches the convention from most search-
        // box UIs. A second Escape (now with empty m_searchText)
        // falls through to deactivate as usual.
        if (!m_searchText.isEmpty()) {
            m_searchText.clear();
            effects->addRepaintFull();
            return;
        }
        deactivate();
        return;
    }
    // Grid View toggle (Meta+G). KGlobalAccel can't fire
    // m_gridViewAction while we hold the keyboard grab (see the
    // desktop-switch note below), so recognise the default binding
    // locally and mirror the action's toggle semantics: in GridView
    // it dismisses (same as Escape), in Overview it flips to GridView.
    if (event->key() == Qt::Key_G && (event->modifiers() & Qt::MetaModifier)) {
        if (m_mode == Mode::GridView) {
            deactivate();
        } else {
            activate(Mode::GridView);
        }
        return;
    }
    // WindowClass toggle (Ctrl+F7). Same rationale as Meta+G: KGlobalAccel
    // can't fire m_exposeClassAction under the grab. Critically this must run
    // BEFORE the Ctrl+F1..F12 desktop-switch handling below, which would
    // otherwise swallow Ctrl+F7 as "switch to desktop 7". In WindowClass it
    // dismisses; from another mode it switches in.
    if (event->key() == Qt::Key_F7 && event->modifiers() == Qt::ControlModifier) {
        if (m_mode == Mode::WindowClass) {
            deactivate();
        } else {
            activate(Mode::WindowClass);
        }
        return;
    }
    // Backspace edits the search filter when one is active. Sits
    // before the nav handling so the same key doesn't double as
    // anything else.
    if (event->key() == Qt::Key_Backspace && !m_searchText.isEmpty()) {
        m_searchText.chop(1);
        effects->addRepaintFull();
        return;
    }
    // Printable text (including space) goes into the search filter.
    // QString::at(0).isPrint() returns false for Tab/Return/control
    // chars, so navigation keys still fall through. Ctrl-prefixed
    // input is excluded so e.g. Ctrl+W doesn't get typed.
    {
        const QString text = event->text();
        if (!text.isEmpty() && text.at(0).isPrint()
            && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            m_searchText.append(text);
            effects->addRepaintFull();
            return;
        }
    }
    // Default Plasma desktop-switch shortcuts. KGlobalAccel can't fire
    // its registered actions while we hold the keyboard grab, so we
    // recognise the well-known bindings locally and call
    // setCurrentDesktop through the bar-click teardown path (it
    // synchronises with the slide-out + VD OSD the same way). Only
    // the defaults are matched — Ctrl+F1..F12 for absolute desktop
    // selection, Ctrl+Alt+Left/Right for relative. Custom user
    // bindings aren't covered; that would need a per-frame walk of
    // KGlobalAccel's registry, which the public API doesn't expose.
    if (effects) {
        const Qt::KeyboardModifiers mods = event->modifiers();
        const auto desktops = effects->desktops();
        VirtualDesktop *target = nullptr;
        if (mods == Qt::ControlModifier
            && event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12) {
            const int idx = event->key() - Qt::Key_F1;
            if (idx >= 0 && idx < desktops.size()) {
                target = desktops.at(idx);
            }
        } else if (mods == (Qt::ControlModifier | Qt::AltModifier)
                   && desktops.size() > 1) {
            VirtualDesktop *current = effects->currentDesktop();
            const int currentIdx = current ? desktops.indexOf(current) : -1;
            if (currentIdx >= 0) {
                const int n = desktops.size();
                if (event->key() == Qt::Key_Left) {
                    target = desktops.at((currentIdx - 1 + n) % n);
                } else if (event->key() == Qt::Key_Right) {
                    target = desktops.at((currentIdx + 1) % n);
                }
            }
        }
        if (target) {
            VirtualDesktop *current = effects->currentDesktop();
            teardownImmediate();
            if (target != current) {
                effects->setCurrentDesktop(target);
            }
            return;
        }
    }
#if HAVE_VULKAN
    // Navigation across the grid + bar (V1's "leak" pattern from
    // qml/main.qml:201-240). Up at the top grid row jumps focus into
    // the desktop bar; Down from the bar drops back into the grid's
    // top row at the matching column. Enter activates the focused
    // window tile or switches to the focused desktop, mirroring a
    // click on the same element.
    const int gridN = int(m_tileLayout.size());
    const int barN = int(m_barTiles.size());
    const int gridCols = gridN > 0 ? std::max(1, int(std::ceil(std::sqrt(double(gridN))))) : 1;
    const int key = event->key();
    if (gridN > 0 || barN > 0) {
        auto repaint = [&]() {
            effects->addRepaintFull();
        };
        // Pick a sensible default zone on the first navigation press —
        // grid if there's anything there, otherwise the bar. Returns
        // true if seedFocus actually moved focus from None to a real
        // zone, so callers can short-circuit: the first nav press
        // should land on the seeded tile (visible highlight) and stop
        // — a *second* press is what actually navigates. Without this,
        // a single-tile grid would absorb both presses silently before
        // the user saw any feedback.
        auto seedFocus = [&]() -> bool {
            if (m_focusZone != FocusZone::None) {
                return false;
            }
            if (gridN > 0) {
                m_focusZone = FocusZone::Grid;
                m_focusedIndex = 0;
                repaint();
                return true;
            }
            if (barN > 0) {
                m_focusZone = FocusZone::Bar;
                m_focusedIndex = 0;
                repaint();
                return true;
            }
            return false;
        };

        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            if (m_focusZone == FocusZone::Grid && m_focusedIndex >= 0
                && m_focusedIndex < gridN) {
                Window *handle = m_tileLayout[m_focusedIndex].handle;
                if (handle && handle->effectWindow()) {
                    effects->activateWindow(handle->effectWindow());
                }
                deactivate();
                return;
            }
            if (m_focusZone == FocusZone::Bar && m_focusedIndex >= 0
                && m_focusedIndex < barN) {
                // Same teardown path as a bar-tile click — snap V2
                // down synchronously, then switch. Animated slide-out
                // races with setCurrentDesktop's OSD / fadedesktop /
                // KGlobalAccel state.
                const BarTile &b = m_barTiles[m_focusedIndex];
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

        const bool shiftTab = (key == Qt::Key_Backtab)
            || (key == Qt::Key_Tab && (event->modifiers() & Qt::ShiftModifier));
        if (key == Qt::Key_Tab && !shiftTab) {
            if (seedFocus()) {
                return;
            }
            const int n = (m_focusZone == FocusZone::Bar) ? barN : gridN;
            if (n > 0) {
                m_focusedIndex = (m_focusedIndex + 1) % n;
                repaint();
            }
            return;
        }
        if (shiftTab) {
            if (seedFocus()) {
                return;
            }
            const int n = (m_focusZone == FocusZone::Bar) ? barN : gridN;
            if (n > 0) {
                m_focusedIndex = (m_focusedIndex <= 0) ? (n - 1) : (m_focusedIndex - 1);
                repaint();
            }
            return;
        }

        if (key == Qt::Key_Right) {
            if (seedFocus()) {
                return;
            }
            const int n = (m_focusZone == FocusZone::Bar) ? barN : gridN;
            if (n > 0 && m_focusedIndex + 1 < n) {
                m_focusedIndex++;
                repaint();
            }
            return;
        }
        if (key == Qt::Key_Left) {
            if (seedFocus()) {
                return;
            }
            if (m_focusedIndex > 0) {
                m_focusedIndex--;
                repaint();
            }
            return;
        }
        if (key == Qt::Key_Down) {
            if (m_focusZone == FocusZone::Bar) {
                if (gridN > 0) {
                    // Drop into the top grid row at the matching
                    // column. Bar tiles don't map 1:1 to grid columns,
                    // so just take the column ratio.
                    const int col = (barN > 1)
                        ? int(double(m_focusedIndex) * gridCols / barN)
                        : 0;
                    m_focusZone = FocusZone::Grid;
                    m_focusedIndex = std::clamp(col, 0, gridN - 1);
                    repaint();
                }
                return;
            }
            if (seedFocus()) {
                return;
            }
            if (m_focusZone == FocusZone::Grid && gridN > 0) {
                const int candidate = m_focusedIndex + gridCols;
                if (candidate < gridN) {
                    m_focusedIndex = candidate;
                    repaint();
                }
            }
            return;
        }
        if (key == Qt::Key_Up) {
            if (m_focusZone == FocusZone::Grid && m_focusedIndex < gridCols) {
                // Top grid row → bar. Map grid column to bar tile.
                if (barN > 0) {
                    const int barIdx = (gridCols > 1)
                        ? int(double(m_focusedIndex) * barN / gridCols)
                        : 0;
                    m_focusZone = FocusZone::Bar;
                    m_focusedIndex = std::clamp(barIdx, 0, barN - 1);
                    repaint();
                }
                return;
            }
            if (seedFocus()) {
                return;
            }
            if (m_focusZone == FocusZone::Grid && m_focusedIndex >= gridCols) {
                m_focusedIndex -= gridCols;
                repaint();
            }
            return;
        }
    }
#endif
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
    if (!m_visible) {
        return;
    }
    // Wheel over the desktop-bar zone scrolls the strip horizontally
    // when it overflows the viewport (many VDs). Mirrors V1's
    // Flickable.HorizontalFlick. We respond to both vertical and
    // horizontal wheel input — a normal mouse wheel maps to angleDelta.y,
    // touchpad horizontal-swipe to angleDelta.x.
    if (auto *wheel = dynamic_cast<QWheelEvent *>(event)) {
        if (m_barStripWidthNdc <= 2.0f) {
            return;
        }
        if (!effects) {
            return;
        }
        const QRect screen = effects->virtualScreenGeometry();
        if (screen.isEmpty()) {
            return;
        }
        const QPoint pos = wheel->globalPosition().toPoint();
        // Bar zone covers the bar tile + label strip below it.
        // Computed from the gridUnit-derived member variables that
        // rebuildBarLayout populates so the hit-test stays in sync
        // with rendering regardless of the user's font / DPI.
        const int barTopPx = screen.y()
            + int((m_barTopNdc + 1.0f) * 0.5f * screen.height());
        const int barBottomPx = barTopPx
            + int((m_barHeightNdc + m_barLabelStripNdc) * 0.5f * screen.height());
        if (pos.y() < barTopPx || pos.y() > barBottomPx) {
            return;
        }
        const QPoint angle = wheel->angleDelta();
        const float delta = float(angle.y() + angle.x());
        if (qFuzzyIsNull(delta)) {
            return;
        }
        // Translate wheel notches (120 per detent) into a comfortable
        // step in NDC X. ~0.2 NDC per detent ≈ a tile-and-a-bit per
        // scroll click.
        m_barScrollX += delta / 120.0f * 0.2f;
        const float overflow = (m_barStripWidthNdc - 2.0f) * 0.5f;
        m_barScrollX = std::clamp(m_barScrollX, -overflow, overflow);
        effects->addRepaintFull();
        return;
    }
    auto *mouseEvent = dynamic_cast<QMouseEvent *>(event);
    if (!mouseEvent) {
        return;
    }
    // Hold off press/move until the slide-in animation has
    // substantially settled — otherwise hit-tests miss against tiles
    // still animating from their real positions. 0.95 is a comfortable
    // threshold; the user can't perceive the last 5% of the OutCubic.
    //
    // Release is always processed: if V2 starts deactivating mid-drag
    // (some external trigger forces it), we still need to clean up
    // m_dragCandidate / m_dragActive so the next activation doesn't
    // inherit stale state.
    if (event->type() != QEvent::MouseButtonRelease && m_activationFactor < 0.95) {
        return;
    }

    const QPoint pos = mouseEvent->globalPosition().toPoint();

#if HAVE_VULKAN
    // Grid View handles input on its own: a left-click on a desktop
    // card activates that desktop and dismisses the effect (V1's
    // TapHandler-on-card behaviour). The overview heap/bar/drag paths
    // below don't apply in this mode, so consume everything here.
    if (m_mode == Mode::GridView) {
        if (event->type() == QEvent::MouseButtonRelease
            && mouseEvent->button() == Qt::LeftButton && effects) {
            if (VirtualDesktop *vd = hitTestGridCard(pos)) {
                VirtualDesktop *current = effects->currentDesktop();
                // Synchronous teardown before the switch, same as the
                // bar-click path — avoids racing the slide-out with
                // setCurrentDesktop's OSD / desktop-change effects.
                teardownImmediate();
                if (vd != current) {
                    effects->setCurrentDesktop(vd);
                }
            }
        }
        return;
    }
#endif

    if (event->type() == QEvent::MouseMove) {
        const QPoint prev = m_dragCurrentGlobal;
        m_dragCurrentGlobal = pos;
        m_mouseGlobal = pos;
#if HAVE_VULKAN
        // Hover state for the bar's hover-revealed affordances:
        // (a) Add-VD "+" tile brightens on hover.
        // (b) Each bar tile's "×" delete affordance is revealed
        //     when the cursor is anywhere inside that tile (so the
        //     small target doesn't require pixel-precise targeting
        //     to discover) or directly on the affordance.
        // Only repaint when at least one bit actually changes — most
        // pointer movement doesn't cross any of these boundaries.
        const bool addHoverNow = hitTestAddTile(pos);
        VirtualDesktop *barUnder = hitTestBar(pos);
        int deleteHoverNow = -1;
        if (m_barTiles.size() > 1) {
            // Prefer direct affordance hit first; otherwise reveal
            // on whichever bar tile the cursor sits in.
            deleteHoverNow = hitTestDeleteAffordance(pos);
            if (deleteHoverNow < 0 && barUnder) {
                for (size_t i = 0; i < m_barTiles.size(); ++i) {
                    if (m_barTiles[i].desktop == barUnder) {
                        deleteHoverNow = int(i);
                        break;
                    }
                }
            }
        }
        if (addHoverNow != m_addTileHover || deleteHoverNow != m_deleteAffordanceHover) {
            m_addTileHover = addHoverNow;
            m_deleteAffordanceHover = deleteHoverNow;
            if (effects) {
                const QRect screen = effects->virtualScreenGeometry();
                if (!screen.isEmpty()) {
                    const int barTopPx = screen.y() + int(0.02 * screen.height());
                    const int barHeightPx = int(0.16 * screen.height()) + kDragDamagePadPx * 2;
                    effects->addRepaint(QRect(screen.x(), barTopPx, screen.width(), barHeightPx));
                }
            }
        }
        // Grid-tile hover: walk m_tileLayout for the cell under the
        // cursor. Repaint only on transitions to keep stationary
        // mouse drift cheap.
        int hoveredNow = -1;
        if (effects && !m_dragActive) {
            const QRect screen = effects->virtualScreenGeometry();
            if (!screen.isEmpty() && screen.contains(pos)) {
                const float sw = std::max(1.0f, float(screen.width()));
                const float sh = std::max(1.0f, float(screen.height()));
                const float mx = (float(pos.x() - screen.x()) / sw) * 2.0f - 1.0f;
                const float my = (float(pos.y() - screen.y()) / sh) * 2.0f - 1.0f;
                for (size_t i = 0; i < m_tileLayout.size(); ++i) {
                    const TileLayout &t = m_tileLayout[i];
                    if (mx >= t.gridNdcX && mx <= t.gridNdcX + t.gridNdcW
                        && my >= t.gridNdcY && my <= t.gridNdcY + t.gridNdcH) {
                        hoveredNow = int(i);
                        break;
                    }
                }
            }
        }
        if (hoveredNow != m_hoveredTileIndex) {
            m_hoveredTileIndex = hoveredNow;
            effects->addRepaintFull();
        }
        // Bar-tile hover for outline rendering.
        int barHoveredNow = -1;
        if (barUnder) {
            for (size_t i = 0; i < m_barTiles.size(); ++i) {
                if (m_barTiles[i].desktop == barUnder) {
                    barHoveredNow = int(i);
                    break;
                }
            }
        }
        if (barHoveredNow != m_hoveredBarIndex) {
            m_hoveredBarIndex = barHoveredNow;
            effects->addRepaintFull();
        }
#endif
        if (m_dragCandidate && !m_dragActive) {
            const QPoint delta = pos - m_dragPressGlobal;
            if (delta.manhattanLength() >= kDragThresholdPx) {
                m_dragActive = true;
                // First active frame: the settled grid rect becomes
                // the "last damage" we union with so the tile's home
                // cell gets repainted (no longer occupied by the
                // tile) as the tile moves off it.
                m_dragLastDamage = draggedTileScreenRect(m_dragPressGlobal);
            }
        }
        if (m_dragActive) {
            // Damage = where the tile *was* last frame ∪ where it is
            // *this* frame, plus the desktop-bar strip so the drop-
            // target wash can appear/disappear on bar tiles as the
            // cursor crosses them. Outside that region the partial-
            // repaint backend preserves the previous frame, so we
            // don't waste a fullscreen recomposite on a cursor
            // wiggle. kwin's compositor coalesces multiple addRepaint
            // calls within a frame and caps presents at vblank, so no
            // extra throttling needed here.
            const QRect newRect = draggedTileScreenRect(pos);
            QRegion damage = QRegion(m_dragLastDamage).united(QRegion(newRect));
            // Bar strip damage region: derived from gridUnit-aware
            // member variables so the repaint covers exactly the bar
            // tile + label strip area, with a small pad so a tile
            // leaving the cursor doesn't leave a smear.
            if (effects) {
                const QRect screen = effects->virtualScreenGeometry();
                if (!screen.isEmpty()) {
                    const int barTopPx = screen.y()
                        + int((m_barTopNdc + 1.0f) * 0.5f * screen.height());
                    const int barHeightPx = int((m_barHeightNdc + m_barLabelStripNdc)
                                                * 0.5f * screen.height())
                        + kDragDamagePadPx * 2;
                    damage += QRect(screen.x(), barTopPx, screen.width(), barHeightPx);
                }
            }
            if (!damage.isEmpty()) {
                effects->addRepaint(damage);
                m_dragLastDamage = newRect;
            }
            Q_UNUSED(prev);
        }
        return;
    }

    if (event->type() == QEvent::MouseButtonPress) {
        if (mouseEvent->button() != Qt::LeftButton) {
            return;
        }
#if HAVE_VULKAN
        // Bar press → no candidate, no drag. Bar tiles are click-only.
        // Match the bar hit-test that the existing release path uses
        // and treat the bar press as a no-op so the release switches
        // the desktop. Same for the Add-VD tile — release creates the
        // new desktop; press is a no-op. Same again for the per-tile
        // "×" affordance — release removes the desktop.
        if (hitTestBar(pos) || hitTestAddTile(pos) || hitTestDeleteAffordance(pos) >= 0) {
            return;
        }
#endif
        // Grid press → record drag candidate. Don't commit any action
        // yet; release decides between click (activate) and drag-drop
        // (move-to-desktop).
        if (Window *target = hitTestTile(pos)) {
            m_dragCandidate = target;
            m_dragPressGlobal = pos;
            m_dragCurrentGlobal = pos;
            m_dragActive = false;
        } else {
            m_dragCandidate = nullptr;
        }
        return;
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        // Middle-click on a grid tile closes the window without
        // dismissing V2 — matches V1's TapHandler-on-middle behaviour.
        // Lets the user reap several windows in one overview session.
        // Skip if a drag was somehow in progress (LeftButton-only,
        // but defensive in case kwin synthesises a Middle release).
        if (mouseEvent->button() == Qt::MiddleButton) {
            if (Window *target = hitTestTile(pos)) {
                if (target->effectWindow()) {
                    target->effectWindow()->closeWindow();
                }
            }
            return;
        }
        if (mouseEvent->button() != Qt::LeftButton) {
            return;
        }
        m_dragCurrentGlobal = pos;
        const bool wasDrag = m_dragActive;
        Window *candidate = m_dragCandidate;
        m_dragCandidate = nullptr;
        m_dragActive = false;

        if (wasDrag && candidate) {
            // Drop hit-test: bar tile under the cursor is the target
            // desktop. Anything else cancels (just clear state and
            // let the next frame snap the tile back to its grid cell).
#if HAVE_VULKAN
            if (VirtualDesktop *target = hitTestBar(pos)) {
                if (target != effects->currentDesktop() && candidate->effectWindow()) {
                    // Same teardown pattern as a bar-tile click —
                    // synchronous so the desktop switch doesn't race
                    // with the still-running slide-out and the
                    // shortcut state stays coherent.
                    effects->windowToDesktops(candidate->effectWindow(), {target});
                }
                teardownImmediate();
                if (target != effects->currentDesktop()) {
                    effects->setCurrentDesktop(target);
                }
                return;
            }
#endif
            // Drop on empty space / source tile / grid: cancel.
            // Damage = the strip the tile last covered ∪ its settled
            // grid position so the snap-back is visible in one frame
            // without a fullscreen recomposite.
            const QRect settledRect = draggedTileScreenRect(m_dragPressGlobal);
            if (m_dragLastDamage.isValid() || settledRect.isValid()) {
                effects->addRepaint(QRegion(m_dragLastDamage).united(QRegion(settledRect)));
            }
            m_dragLastDamage = QRect();
            return;
        }

        // Not a drag — fall through to the click logic. Bar hit-test
        // first (smaller targets sit over the top of the screen).
#if HAVE_VULKAN
        if (const int deleteIdx = hitTestDeleteAffordance(pos); deleteIdx >= 0) {
            // Remove the desktop, keep V2 open so the user can see the
            // bar shrink. VirtualDesktopManager handles re-homing
            // windows that were on the removed desktop. Repaint the
            // bar strip so the layout change is visible immediately.
            auto *vdm = VirtualDesktopManager::self();
            if (vdm && deleteIdx < int(m_barTiles.size())) {
                VirtualDesktop *target = m_barTiles[deleteIdx].desktop;
                if (target) {
                    vdm->removeVirtualDesktop(target);
                    // Layout depends on desktop count; force a rebuild
                    // before the next post-pass so the bar reflows.
                    m_deleteAffordanceHover = -1;
                    if (effects) {
                        const QRect screen = effects->virtualScreenGeometry();
                        if (!screen.isEmpty()) {
                            rebuildBarLayout(screen.size());
                            effects->addRepaint(screen);
                        }
                    }
                }
            }
            return;
        }
        if (hitTestAddTile(pos)) {
            // Create a new desktop and switch to it. Position past the
            // last existing desktop so it appears at the end of the
            // bar (where the "+" tile sat). createVirtualDesktop()
            // takes a 1-based position; passing count()+1 appends.
            auto *vdm = VirtualDesktopManager::self();
            if (vdm) {
                VirtualDesktop *created = vdm->createVirtualDesktop(vdm->count() + 1);
                if (created && effects) {
                    teardownImmediate();
                    effects->setCurrentDesktop(created);
                    return;
                }
            }
            // Couldn't create (config-locked count, etc.) — just keep
            // V2 open so the user notices nothing changed.
            return;
        }
        if (VirtualDesktop *barTarget = hitTestBar(pos)) {
            VirtualDesktop *target = (barTarget != effects->currentDesktop())
                ? barTarget
                : nullptr;
            teardownImmediate();
            if (target) {
                effects->setCurrentDesktop(target);
            }
            return;
        }
#endif
        if (Window *target = hitTestTile(pos)) {
            if (target->effectWindow()) {
                effects->activateWindow(target->effectWindow());
            }
            deactivate();
            return;
        }
        // Click outside any tile or bar: dismiss.
        deactivate();
    }
}

#if HAVE_VULKAN
QRectF OverviewEffectV2::deleteAffordanceNdc(float ndcX, float ndcY, float ndcW, float ndcH) const
{
    // Top-right corner of the bar tile, sized to keep the square "×"
    // texture square on screen. The tile's NDC height drives the
    // pixel size we want; convert back to NDC for both axes through
    // the per-axis framebuffer scale so we don't stretch on widescreen.
    if (!effects) {
        const float side = ndcH * kDeleteAffordanceFrac;
        const float pad = ndcH * 0.06f;
        return QRectF(ndcX + ndcW - side - pad, ndcY + pad, side, side);
    }
    const QRect screen = effects->virtualScreenGeometry();
    const float screenWf = std::max(1.0f, float(screen.width()));
    const float screenHf = std::max(1.0f, float(screen.height()));
    const float tilePxH = ndcH * 0.5f * screenHf;
    const float sidePx = tilePxH * kDeleteAffordanceFrac;
    const float padPx = tilePxH * 0.06f;
    const float ndcSideW = 2.0f * sidePx / screenWf;
    const float ndcSideH = 2.0f * sidePx / screenHf;
    const float ndcPadW = 2.0f * padPx / screenWf;
    const float ndcPadH = 2.0f * padPx / screenHf;
    return QRectF(ndcX + ndcW - ndcSideW - ndcPadW, ndcY + ndcPadH, ndcSideW, ndcSideH);
}

int OverviewEffectV2::hitTestDeleteAffordance(const QPoint &globalPos) const
{
    if (m_barTiles.empty() || !effects) {
        return -1;
    }
    // No delete affordance when there's only one desktop — can't
    // remove the last one. Matches V1's behaviour.
    if (m_barTiles.size() <= 1) {
        return -1;
    }
    const QRect screen = effects->virtualScreenGeometry();
    if (screen.isEmpty() || !screen.contains(globalPos)) {
        return -1;
    }
    const float screenW = std::max(1.0f, float(screen.width()));
    const float screenH = std::max(1.0f, float(screen.height()));
    const float mxNdc = (float(globalPos.x() - screen.x()) / screenW) * 2.0f - 1.0f;
    const float myNdc = (float(globalPos.y() - screen.y()) / screenH) * 2.0f - 1.0f;
    for (size_t i = 0; i < m_barTiles.size(); ++i) {
        const BarTile &b = m_barTiles[i];
        const QRectF aff = deleteAffordanceNdc(b.ndcX, b.ndcY, b.ndcW, b.ndcH);
        if (mxNdc >= aff.x() && mxNdc <= aff.x() + aff.width()
            && myNdc >= aff.y() && myNdc <= aff.y() + aff.height()) {
            return int(i);
        }
    }
    return -1;
}

bool OverviewEffectV2::hitTestAddTile(const QPoint &globalPos) const
{
    if (m_addTileNdc.isEmpty() || !effects) {
        return false;
    }
    const QRect screen = effects->virtualScreenGeometry();
    if (screen.isEmpty() || !screen.contains(globalPos)) {
        return false;
    }
    const float screenW = std::max(1.0f, float(screen.width()));
    const float screenH = std::max(1.0f, float(screen.height()));
    const float mxNdc = (float(globalPos.x() - screen.x()) / screenW) * 2.0f - 1.0f;
    const float myNdc = (float(globalPos.y() - screen.y()) / screenH) * 2.0f - 1.0f;
    return mxNdc >= m_addTileNdc.x() && mxNdc <= m_addTileNdc.x() + m_addTileNdc.width()
        && myNdc >= m_addTileNdc.y() && myNdc <= m_addTileNdc.y() + m_addTileNdc.height();
}

VirtualDesktop *OverviewEffectV2::hitTestBar(const QPoint &globalPos) const
{
    if (m_barTiles.empty() || !effects) {
        return nullptr;
    }
    const QRect screen = effects->virtualScreenGeometry();
    if (screen.isEmpty() || !screen.contains(globalPos)) {
        return nullptr;
    }
    const float screenW = std::max(1.0f, float(screen.width()));
    const float screenH = std::max(1.0f, float(screen.height()));
    const float mxNdc = (float(globalPos.x() - screen.x()) / screenW) * 2.0f - 1.0f;
    const float myNdc = (float(globalPos.y() - screen.y()) / screenH) * 2.0f - 1.0f;
    for (const BarTile &b : m_barTiles) {
        if (mxNdc >= b.ndcX && mxNdc <= b.ndcX + b.ndcW
            && myNdc >= b.ndcY && myNdc <= b.ndcY + b.ndcH) {
            return b.desktop;
        }
    }
    return nullptr;
}
#endif

QRect OverviewEffectV2::draggedTileScreenRect(const QPoint &cursor) const
{
#if HAVE_VULKAN
    if (!m_dragCandidate || !effects) {
        return {};
    }
    const TileLayout *src = nullptr;
    for (const TileLayout &t : m_tileLayout) {
        if (t.handle == m_dragCandidate) {
            src = &t;
            break;
        }
    }
    if (!src) {
        return {};
    }
    const QRect screen = effects->virtualScreenGeometry();
    if (screen.isEmpty()) {
        return {};
    }
    const float screenW = float(screen.width());
    const float screenH = float(screen.height());
    // Settled grid screen rect, computed from the tile's stored NDC
    // dims. (gridNdc.x + 1) * screenW / 2 maps NDC X back into the
    // post-pass pixel space.
    const float gridXpx = (src->gridNdcX + 1.0f) * 0.5f * screenW + screen.x();
    const float gridYpx = (src->gridNdcY + 1.0f) * 0.5f * screenH + screen.y();
    const float gridWpx = src->gridNdcW * 0.5f * screenW;
    const float gridHpx = src->gridNdcH * 0.5f * screenH;
    const QPoint delta = cursor - m_dragPressGlobal;
    QRect rect(int(std::floor(gridXpx + delta.x())) - kDragDamagePadPx,
               int(std::floor(gridYpx + delta.y())) - kDragDamagePadPx,
               int(std::ceil(gridWpx)) + 2 * kDragDamagePadPx,
               int(std::ceil(gridHpx)) + 2 * kDragDamagePadPx);
    return rect.intersected(screen);
#else
    Q_UNUSED(cursor)
    return {};
#endif
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

VirtualDesktop *OverviewEffectV2::hitTestGridCard(const QPoint &globalPos) const
{
    if (m_gridCardRects.empty() || !effects) {
        return nullptr;
    }
    const QRect screen = effects->virtualScreenGeometry();
    if (screen.isEmpty() || !screen.contains(globalPos)) {
        return nullptr;
    }
    const float screenW = std::max(1.0f, float(screen.width()));
    const float screenH = std::max(1.0f, float(screen.height()));
    const float mxNdc = (float(globalPos.x() - screen.x()) / screenW) * 2.0f - 1.0f;
    const float myNdc = (float(globalPos.y() - screen.y()) / screenH) * 2.0f - 1.0f;
    for (const auto &[vd, rect] : m_gridCardRects) {
        if (rect.contains(QPointF(mxNdc, myNdc))) {
            return vd;
        }
    }
    return nullptr;
}

} // namespace KWin

#include "moc_overvieweffectv2.cpp"
