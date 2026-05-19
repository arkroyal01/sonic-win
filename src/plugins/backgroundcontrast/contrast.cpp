/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2011 Philipp Knechtges <philipp-dev@knechtges.com>
    SPDX-FileCopyrightText: 2014 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "contrast.h"
#include "contrastshader.h"
// KConfigSkeleton

#include "config-kwin.h"

#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"

#include "utils/xcbutils.h"

#if HAVE_VULKAN
#include "compositor.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene.h"
#endif

#include <QCoreApplication>
#include <QMatrix4x4>
#include <QTimer>
#include <QWindow>
#include <cmath> // for ceil()

#if HAVE_VULKAN
// clang-format off
// Full-screen triangle vertex shader: outputs UV [0,1]x[0,1] from gl_VertexIndex
static const uint32_t s_contrastBgVertSpv[] = {
    0x07230203, 0x00010500, 0x000d000b, 0x0000002e, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00000018, 0x00000020,
    0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f,
    0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365,
    0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63,
    0x69645f65, 0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00030005, 0x00000009, 0x00007675, 0x00060005, 0x0000000c,
    0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00060005, 0x00000018,
    0x67617266, 0x43786554, 0x64726f6f, 0x00000000, 0x00060005, 0x0000001e,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000001e,
    0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001e,
    0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006,
    0x0000001e, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e,
    0x00070006, 0x0000001e, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369,
    0x0065636e, 0x00030005, 0x00000020, 0x00000000, 0x00040047, 0x0000000c,
    0x0000000b, 0x0000002a, 0x00040047, 0x00000018, 0x0000001e, 0x00000000,
    0x00030047, 0x0000001e, 0x00000002, 0x00050048, 0x0000001e, 0x00000000,
    0x0000000b, 0x00000000, 0x00050048, 0x0000001e, 0x00000001, 0x0000000b,
    0x00000001, 0x00050048, 0x0000001e, 0x00000002, 0x0000000b, 0x00000003,
    0x00050048, 0x0000001e, 0x00000003, 0x0000000b, 0x00000004, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020,
    0x00000008, 0x00000007, 0x00000007, 0x00040015, 0x0000000a, 0x00000020,
    0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b,
    0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e,
    0x00000001, 0x0004002b, 0x0000000a, 0x00000010, 0x00000002, 0x00040020,
    0x00000017, 0x00000003, 0x00000007, 0x0004003b, 0x00000017, 0x00000018,
    0x00000003, 0x00040017, 0x0000001a, 0x00000006, 0x00000004, 0x00040015,
    0x0000001b, 0x00000020, 0x00000000, 0x0004002b, 0x0000001b, 0x0000001c,
    0x00000001, 0x0004001c, 0x0000001d, 0x00000006, 0x0000001c, 0x0006001e,
    0x0000001e, 0x0000001a, 0x00000006, 0x0000001d, 0x0000001d, 0x00040020,
    0x0000001f, 0x00000003, 0x0000001e, 0x0004003b, 0x0000001f, 0x00000020,
    0x00000003, 0x0004002b, 0x0000000a, 0x00000021, 0x00000000, 0x0004002b,
    0x00000006, 0x00000023, 0x40000000, 0x0004002b, 0x00000006, 0x00000025,
    0x3f800000, 0x0004002b, 0x00000006, 0x00000028, 0x00000000, 0x00040020,
    0x0000002c, 0x00000003, 0x0000001a, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008,
    0x00000009, 0x00000007, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c,
    0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7,
    0x0000000a, 0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006,
    0x00000012, 0x00000011, 0x0004003d, 0x0000000a, 0x00000013, 0x0000000c,
    0x000500c7, 0x0000000a, 0x00000014, 0x00000013, 0x00000010, 0x0004006f,
    0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016,
    0x00000012, 0x00000015, 0x0003003e, 0x00000009, 0x00000016, 0x0004003d,
    0x00000007, 0x00000019, 0x00000009, 0x0003003e, 0x00000018, 0x00000019,
    0x0004003d, 0x00000007, 0x00000022, 0x00000009, 0x0005008e, 0x00000007,
    0x00000024, 0x00000022, 0x00000023, 0x00050050, 0x00000007, 0x00000026,
    0x00000025, 0x00000025, 0x00050083, 0x00000007, 0x00000027, 0x00000024,
    0x00000026, 0x00050051, 0x00000006, 0x00000029, 0x00000027, 0x00000000,
    0x00050051, 0x00000006, 0x0000002a, 0x00000027, 0x00000001, 0x00070050,
    0x0000001a, 0x0000002b, 0x00000029, 0x0000002a, 0x00000028, 0x00000025,
    0x00050041, 0x0000002c, 0x0000002d, 0x00000020, 0x00000021, 0x0003003e,
    0x0000002d, 0x0000002b, 0x000100fd, 0x00010038,
};
// Fragment shader: sample capture texture and apply 4x4 color matrix + opacity blend
static const uint32_t s_contrastBgFragSpv[] = {
    0x07230203, 0x00010500, 0x000d000b, 0x00000049, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0009000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000011, 0x00000017,
    0x00000023, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002,
    0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45, 0x74735f70,
    0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004,
    0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572,
    0x00657669, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00040005,
    0x00000009, 0x6f6c6f63, 0x00000072, 0x00030005, 0x0000000d, 0x00637273,
    0x00060005, 0x00000011, 0x67617266, 0x43786554, 0x64726f6f, 0x00000000,
    0x00030005, 0x00000015, 0x00004350, 0x00060006, 0x00000015, 0x00000000,
    0x6f6c6f63, 0x74614d72, 0x00786972, 0x00050006, 0x00000015, 0x00000001,
    0x6361706f, 0x00797469, 0x00030005, 0x00000017, 0x00006370, 0x00050005,
    0x00000023, 0x4374756f, 0x726f6c6f, 0x00000000, 0x00040047, 0x0000000d,
    0x00000021, 0x00000000, 0x00040047, 0x0000000d, 0x00000022, 0x00000000,
    0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00030047, 0x00000015,
    0x00000002, 0x00040048, 0x00000015, 0x00000000, 0x00000005, 0x00050048,
    0x00000015, 0x00000000, 0x00000007, 0x00000010, 0x00050048, 0x00000015,
    0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000015, 0x00000001,
    0x00000023, 0x00000040, 0x00040047, 0x00000023, 0x0000001e, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
    0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00090019, 0x0000000a,
    0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000000, 0x0003001b, 0x0000000b, 0x0000000a, 0x00040020, 0x0000000c,
    0x00000000, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000000,
    0x00040017, 0x0000000f, 0x00000006, 0x00000002, 0x00040020, 0x00000010,
    0x00000001, 0x0000000f, 0x0004003b, 0x00000010, 0x00000011, 0x00000001,
    0x00040018, 0x00000014, 0x00000007, 0x00000004, 0x0004001e, 0x00000015,
    0x00000014, 0x00000006, 0x00040020, 0x00000016, 0x00000009, 0x00000015,
    0x0004003b, 0x00000016, 0x00000017, 0x00000009, 0x00040015, 0x00000018,
    0x00000020, 0x00000001, 0x0004002b, 0x00000018, 0x00000019, 0x00000001,
    0x00040020, 0x0000001a, 0x00000009, 0x00000006, 0x0004002b, 0x00000006,
    0x0000001d, 0x3f800000, 0x00020014, 0x0000001e, 0x00040020, 0x00000022,
    0x00000003, 0x00000007, 0x0004003b, 0x00000022, 0x00000023, 0x00000003,
    0x0004002b, 0x00000018, 0x00000025, 0x00000000, 0x00040020, 0x00000026,
    0x00000009, 0x00000014, 0x0004002b, 0x00000006, 0x00000034, 0x00000000,
    0x0007002c, 0x00000007, 0x00000035, 0x0000001d, 0x00000034, 0x00000034,
    0x00000034, 0x0007002c, 0x00000007, 0x00000036, 0x00000034, 0x0000001d,
    0x00000034, 0x00000034, 0x0007002c, 0x00000007, 0x00000037, 0x00000034,
    0x00000034, 0x0000001d, 0x00000034, 0x0007002c, 0x00000007, 0x00000038,
    0x00000034, 0x00000034, 0x00000034, 0x0000001d, 0x0007002c, 0x00000014,
    0x00000039, 0x00000035, 0x00000036, 0x00000037, 0x00000038, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003d, 0x0000000b,
    0x0000000e, 0x0000000d, 0x0004003d, 0x0000000f, 0x00000012, 0x00000011,
    0x00050057, 0x00000007, 0x00000013, 0x0000000e, 0x00000012, 0x0003003e,
    0x00000009, 0x00000013, 0x00050041, 0x0000001a, 0x0000001b, 0x00000017,
    0x00000019, 0x0004003d, 0x00000006, 0x0000001c, 0x0000001b, 0x000500be,
    0x0000001e, 0x0000001f, 0x0000001c, 0x0000001d, 0x000300f7, 0x00000021,
    0x00000000, 0x000400fa, 0x0000001f, 0x00000020, 0x0000002a, 0x000200f8,
    0x00000020, 0x0004003d, 0x00000007, 0x00000024, 0x00000009, 0x00050041,
    0x00000026, 0x00000027, 0x00000017, 0x00000025, 0x0004003d, 0x00000014,
    0x00000028, 0x00000027, 0x00050090, 0x00000007, 0x00000029, 0x00000024,
    0x00000028, 0x0003003e, 0x00000023, 0x00000029, 0x000200f9, 0x00000021,
    0x000200f8, 0x0000002a, 0x0004003d, 0x00000007, 0x0000002b, 0x00000009,
    0x00050041, 0x0000001a, 0x0000002c, 0x00000017, 0x00000019, 0x0004003d,
    0x00000006, 0x0000002d, 0x0000002c, 0x00050041, 0x00000026, 0x0000002e,
    0x00000017, 0x00000025, 0x0004003d, 0x00000014, 0x0000002f, 0x0000002e,
    0x0005008f, 0x00000014, 0x00000030, 0x0000002f, 0x0000002d, 0x00050041,
    0x0000001a, 0x00000031, 0x00000017, 0x00000019, 0x0004003d, 0x00000006,
    0x00000032, 0x00000031, 0x00050083, 0x00000006, 0x00000033, 0x0000001d,
    0x00000032, 0x0005008f, 0x00000014, 0x0000003a, 0x00000039, 0x00000033,
    0x00050051, 0x00000007, 0x0000003b, 0x00000030, 0x00000000, 0x00050051,
    0x00000007, 0x0000003c, 0x0000003a, 0x00000000, 0x00050081, 0x00000007,
    0x0000003d, 0x0000003b, 0x0000003c, 0x00050051, 0x00000007, 0x0000003e,
    0x00000030, 0x00000001, 0x00050051, 0x00000007, 0x0000003f, 0x0000003a,
    0x00000001, 0x00050081, 0x00000007, 0x00000040, 0x0000003e, 0x0000003f,
    0x00050051, 0x00000007, 0x00000041, 0x00000030, 0x00000002, 0x00050051,
    0x00000007, 0x00000042, 0x0000003a, 0x00000002, 0x00050081, 0x00000007,
    0x00000043, 0x00000041, 0x00000042, 0x00050051, 0x00000007, 0x00000044,
    0x00000030, 0x00000003, 0x00050051, 0x00000007, 0x00000045, 0x0000003a,
    0x00000003, 0x00050081, 0x00000007, 0x00000046, 0x00000044, 0x00000045,
    0x00070050, 0x00000014, 0x00000047, 0x0000003d, 0x00000040, 0x00000043,
    0x00000046, 0x00050090, 0x00000007, 0x00000048, 0x0000002b, 0x00000047,
    0x0003003e, 0x00000023, 0x00000048, 0x000200f9, 0x00000021, 0x000200f8,
    0x00000021, 0x000100fd, 0x00010038,
};
// clang-format on
#endif // HAVE_VULKAN

namespace KWin
{

static const QByteArray s_contrastAtomName = QByteArrayLiteral("_KDE_NET_WM_BACKGROUND_CONTRAST_REGION");

ContrastManagerInterface *ContrastEffect::s_contrastManager = nullptr;
QTimer *ContrastEffect::s_contrastManagerRemoveTimer = nullptr;

ContrastEffect::ContrastEffect()
{
    if (effects->isOpenGLCompositing()) {
        m_shader = std::make_unique<ContrastShader>();
        m_shader->init();
    }
#if HAVE_VULKAN
    else if (effects->isVulkanCompositing()) {
        initVulkanResources();
    }
#endif

    const bool effectValid = (m_shader && m_shader->isValid())
#if HAVE_VULKAN
        || m_vulkanValid
#endif
        ;

    // ### Hackish way to announce support.
    //     Should be included in _NET_SUPPORTED instead.
    if (effectValid) {
        if (effects->xcbConnection()) {
            m_net_wm_contrast_region = effects->announceSupportProperty(s_contrastAtomName, this);
        }
    }

    connect(effects, &EffectsHandler::windowAdded, this, &ContrastEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &ContrastEffect::slotWindowDeleted);
    connect(effects, &EffectsHandler::virtualScreenGeometryChanged, this, &ContrastEffect::slotScreenGeometryChanged);

    connect(effects, &EffectsHandler::propertyNotify, this, &ContrastEffect::slotPropertyNotify);
    connect(effects, &EffectsHandler::xcbConnectionChanged, this, [this]() {
        const bool valid = (m_shader && m_shader->isValid())
#if HAVE_VULKAN
            || m_vulkanValid
#endif
            ;
        if (valid) {
            m_net_wm_contrast_region = effects->announceSupportProperty(s_contrastAtomName, this);
        }
    });

    // Fetch the contrast regions for all windows
    const QList<EffectWindow *> windowList = effects->stackingOrder();
    for (EffectWindow *window : windowList) {
        slotWindowAdded(window);
    }
}

ContrastEffect::~ContrastEffect()
{
#if HAVE_VULKAN
    if (m_vulkanCtx) {
        VkDevice device = m_vulkanCtx->backend()->device();
        if (m_vulkanPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, m_vulkanPipeline, nullptr);
        if (m_vulkanPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, m_vulkanPipelineLayout, nullptr);
        if (m_vulkanDsLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, m_vulkanDsLayout, nullptr);
        if (m_vulkanSampler != VK_NULL_HANDLE)
            vkDestroySampler(device, m_vulkanSampler, nullptr);
        if (m_vulkanRenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, m_vulkanRenderPass, nullptr);
    }
#endif
}

void ContrastEffect::slotScreenGeometryChanged()
{
    if (effects->isOpenGLCompositing()) {
        effects->makeOpenGLContextCurrent();
    }
    if (!supported()) {
        effects->reloadEffect(this);
        return;
    }

    const QList<EffectWindow *> windowList = effects->stackingOrder();
    for (EffectWindow *window : windowList) {
        updateContrastRegion(window);
    }
}

void ContrastEffect::updateContrastRegion(EffectWindow *w)
{
    QRegion region;
    QMatrix4x4 matrix;
    bool valid = false;

    if (m_net_wm_contrast_region != XCB_ATOM_NONE) {
        float colorTransform[16];
        const QByteArray value = w->readProperty(m_net_wm_contrast_region, m_net_wm_contrast_region, 32);

        if (value.size() > 0 && !((value.size() - (16 * sizeof(uint32_t))) % ((4 * sizeof(uint32_t))))) {
            const uint32_t *cardinals = reinterpret_cast<const uint32_t *>(value.constData());
            const float *floatCardinals = reinterpret_cast<const float *>(value.constData());
            unsigned int i = 0;
            for (; i < ((value.size() - (16 * sizeof(uint32_t)))) / sizeof(uint32_t);) {
                int x = cardinals[i++];
                int y = cardinals[i++];
                int w = cardinals[i++];
                int h = cardinals[i++];
                region += Xcb::fromXNative(QRect(x, y, w, h)).toRect();
            }

            for (unsigned int j = 0; j < 16; ++j) {
                colorTransform[j] = floatCardinals[i + j];
            }

            matrix = QMatrix4x4(colorTransform);
        }

        valid = !value.isNull();
    }

    if (auto internal = w->internalWindow()) {
        const auto property = internal->property("kwin_background_region");
        if (property.isValid()) {
            region = property.value<QRegion>();
            bool ok = false;
            qreal contrast = internal->property("kwin_background_contrast").toReal(&ok);
            if (!ok) {
                contrast = 1.0;
            }
            qreal intensity = internal->property("kwin_background_intensity").toReal(&ok);
            if (!ok) {
                intensity = 1.0;
            }
            qreal saturation = internal->property("kwin_background_saturation").toReal(&ok);
            if (!ok) {
                saturation = 1.0;
            }
            matrix = colorMatrix(contrast, intensity, saturation);
            valid = true;
        }
    }

    if (valid) {
        Data &data = m_windowData[w];
        data.colorMatrix = matrix;
        data.contrastRegion = region;
        data.windowEffect = ItemEffect(w->windowItem());
    } else {
        if (auto it = m_windowData.find(w); it != m_windowData.end()) {
            if (effects->isOpenGLCompositing()) {
                effects->makeOpenGLContextCurrent();
            }
            m_windowData.erase(it);
        }
    }
}

void ContrastEffect::slotWindowAdded(EffectWindow *w)
{

    if (auto internal = w->internalWindow()) {
        internal->installEventFilter(this);
    }

    updateContrastRegion(w);
}

bool ContrastEffect::eventFilter(QObject *watched, QEvent *event)
{
    auto internal = qobject_cast<QWindow *>(watched);
    if (internal && event->type() == QEvent::DynamicPropertyChange) {
        QDynamicPropertyChangeEvent *pe = static_cast<QDynamicPropertyChangeEvent *>(event);
        if (pe->propertyName() == "kwin_background_region" || pe->propertyName() == "kwin_background_contrast" || pe->propertyName() == "kwin_background_intensity" || pe->propertyName() == "kwin_background_saturation") {
            if (auto w = effects->findWindow(internal)) {
                updateContrastRegion(w);
            }
        }
    }
    return false;
}

void ContrastEffect::slotWindowDeleted(EffectWindow *w)
{
    if (auto it = m_windowData.find(w); it != m_windowData.end()) {
        if (effects->isOpenGLCompositing()) {
            effects->makeOpenGLContextCurrent();
        }
        m_windowData.erase(it);
    }
}

void ContrastEffect::slotPropertyNotify(EffectWindow *w, long atom)
{
    if (w && atom == m_net_wm_contrast_region && m_net_wm_contrast_region != XCB_ATOM_NONE) {
        updateContrastRegion(w);
    }
}

QMatrix4x4 ContrastEffect::colorMatrix(qreal contrast, qreal intensity, qreal saturation)
{
    QMatrix4x4 satMatrix; // saturation
    QMatrix4x4 intMatrix; // intensity
    QMatrix4x4 contMatrix; // contrast

    // Saturation matrix
    if (!qFuzzyCompare(saturation, 1.0)) {
        const qreal rval = (1.0 - saturation) * .2126;
        const qreal gval = (1.0 - saturation) * .7152;
        const qreal bval = (1.0 - saturation) * .0722;

        satMatrix = QMatrix4x4(rval + saturation, rval, rval, 0.0,
                               gval, gval + saturation, gval, 0.0,
                               bval, bval, bval + saturation, 0.0,
                               0, 0, 0, 1.0);
    }

    // IntensityMatrix
    if (!qFuzzyCompare(intensity, 1.0)) {
        intMatrix.scale(intensity, intensity, intensity);
    }

    // Contrast Matrix
    if (!qFuzzyCompare(contrast, 1.0)) {
        const float transl = (1.0 - contrast) / 2.0;

        contMatrix = QMatrix4x4(contrast, 0, 0, 0.0,
                                0, contrast, 0, 0.0,
                                0, 0, contrast, 0.0,
                                transl, transl, transl, 1.0);
    }

    QMatrix4x4 colorMatrix = contMatrix * satMatrix * intMatrix;
    // colorMatrix = colorMatrix.transposed();

    return colorMatrix;
}

bool ContrastEffect::enabledByDefault()
{
#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        return true;
    }
#endif
    const auto context = effects->openglContext();
    if (!context || context->isSoftwareRenderer()) {
        return false;
    }
    const auto gl = context->glPlatform();
    if (gl->isIntel() && gl->chipClass() < SandyBridge) {
        return false;
    }
    if (gl->isPanfrost() && gl->chipClass() <= MaliT8XX) {
        return false;
    }
    if (gl->isLima() || gl->isVideoCore4() || gl->isVideoCore3D()) {
        return false;
    }
    return true;
}

bool ContrastEffect::supported()
{
#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        return true;
    }
#endif
    if (!effects->isOpenGLCompositing()) {
        return false;
    }
    return effects->openglContext() && effects->openglContext()->supportsBlits();
}

QRegion ContrastEffect::contrastRegion(const EffectWindow *w) const
{
    QRegion region;
    if (const auto it = m_windowData.find(w); it != m_windowData.end()) {
        const QRegion &appRegion = it->second.contrastRegion;
        if (!appRegion.isEmpty()) {
            region += appRegion.translated(w->contentsRect().topLeft().toPoint()) & w->contentsRect().toRect();
        } else {
            // An empty region means that the contrast effect should be enabled
            // for the whole window.
            region = w->contentsRect().toRect();
        }
    }

    return region;
}

void ContrastEffect::uploadRegion(std::span<QVector2D> map, const QRegion &region, qreal scale)
{
    size_t index = 0;
    for (const QRect &r : region) {
        const auto deviceRect = scaledRect(r, scale);
        const QVector2D topLeft = roundVector(QVector2D(deviceRect.x(), deviceRect.y()));
        const QVector2D topRight = roundVector(QVector2D(deviceRect.x() + deviceRect.width(), deviceRect.y()));
        const QVector2D bottomLeft = roundVector(QVector2D(deviceRect.x(), deviceRect.y() + deviceRect.height()));
        const QVector2D bottomRight = roundVector(QVector2D(deviceRect.x() + deviceRect.width(), deviceRect.y() + deviceRect.height()));

        // First triangle
        map[index++] = topRight;
        map[index++] = topLeft;
        map[index++] = bottomLeft;

        // Second triangle
        map[index++] = bottomLeft;
        map[index++] = bottomRight;
        map[index++] = topRight;
    }
}

bool ContrastEffect::uploadGeometry(GLVertexBuffer *vbo, const QRegion &region, qreal scale)
{
    const int vertexCount = region.rectCount() * 6;
    if (!vertexCount) {
        return false;
    }

    const auto map = vbo->map<QVector2D>(vertexCount);
    if (!map) {
        return false;
    }
    uploadRegion(*map, region, scale);
    vbo->unmap();

    constexpr std::array layout{
        GLVertexAttrib{
            .attributeIndex = VA_Position,
            .componentCount = 2,
            .type = GL_FLOAT,
            .relativeOffset = 0,
        },
        GLVertexAttrib{
            .attributeIndex = VA_TexCoord,
            .componentCount = 2,
            .type = GL_FLOAT,
            .relativeOffset = 0,
        },
    };
    vbo->setAttribLayout(std::span(layout), sizeof(QVector2D));
    return true;
}

bool ContrastEffect::shouldContrast(const EffectWindow *w, int mask, const WindowPaintData &data) const
{
    bool backendValid = (m_shader && m_shader->isValid());
#if HAVE_VULKAN
    backendValid = backendValid || m_vulkanValid;
#endif
    if (!backendValid) {
        return false;
    }

    if (effects->activeFullScreenEffect() && !w->data(WindowForceBackgroundContrastRole).toBool()) {
        return false;
    }

    if (w->isDesktop()) {
        return false;
    }

    bool scaled = !qFuzzyCompare(data.xScale(), 1.0) && !qFuzzyCompare(data.yScale(), 1.0);
    bool translated = data.xTranslation() || data.yTranslation();

    if ((scaled || (translated || (mask & PAINT_WINDOW_TRANSFORMED))) && !w->data(WindowForceBackgroundContrastRole).toBool()) {
        return false;
    }

    return true;
}

void ContrastEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const QRegion &region, WindowPaintData &data)
{
    if (shouldContrast(w, mask, data)) {
        const QRect screen = viewport.renderRect().toRect();
        QRegion shape = contrastRegion(w).translated(w->pos().toPoint());

        // let's do the evil parts - someone wants to contrast behind a transformed window
        const bool translated = data.xTranslation() || data.yTranslation();
        const bool scaled = data.xScale() != 1 || data.yScale() != 1;
        if (scaled) {
            QPoint pt = shape.boundingRect().topLeft();
            QRegion scaledShape;
            for (QRect r : shape) {
                const QPointF topLeft(pt.x() + (r.x() - pt.x()) * data.xScale() + data.xTranslation(),
                                      pt.y() + (r.y() - pt.y()) * data.yScale() + data.yTranslation());
                const QPoint bottomRight(std::floor(topLeft.x() + r.width() * data.xScale()) - 1,
                                         std::floor(topLeft.y() + r.height() * data.yScale()) - 1);
                scaledShape += QRect(QPoint(std::floor(topLeft.x()), std::floor(topLeft.y())), bottomRight);
            }
            shape = scaledShape;

            // Only translated, not scaled
        } else if (translated) {
            shape.translate(std::round(data.xTranslation()), std::round(data.yTranslation()));
        }

        const QRegion effectiveShape = shape & region & screen;
        if (!effectiveShape.isEmpty()) {
#if HAVE_VULKAN
            if (effects->isVulkanCompositing()) {
                doContrastVulkan(renderTarget, viewport, w, effectiveShape, w->opacity() * data.opacity());
            } else
#endif
            {
                doContrast(renderTarget, viewport, w, effectiveShape, w->opacity() * data.opacity(), viewport.projectionMatrix());
            }
        }
    }

    // Draw the window over the contrast area
    effects->drawWindow(renderTarget, viewport, w, mask, region, data);
}

void ContrastEffect::doContrast(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, const QRegion &shape, const float opacity, const QMatrix4x4 &screenProjection)
{
    const qreal scale = viewport.scale();
    const QRectF r = viewport.mapToRenderTarget(shape.boundingRect());

    // Upload geometry for the horizontal and vertical passes
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    if (!uploadGeometry(vbo, shape, scale)) {
        return;
    }
    vbo->bindArrays();

    Q_ASSERT(m_windowData.contains(w));
    auto &windowData = m_windowData[w];
    if (!windowData.texture || (renderTarget.texture() && windowData.texture->internalFormat() != renderTarget.texture()->internalFormat()) || windowData.texture->size() != r.size()) {
        windowData.texture = GLTexture::allocate(renderTarget.texture() ? renderTarget.texture()->internalFormat() : GL_RGBA8, r.size().toSize());
        if (!windowData.texture) {
            return;
        }
        windowData.fbo = std::make_unique<GLFramebuffer>(windowData.texture.get());
        windowData.texture->setFilter(GL_LINEAR);
        windowData.texture->setWrapMode(GL_CLAMP_TO_EDGE);
    }
    GLTexture *contrastTexture = windowData.texture.get();
    contrastTexture->bind();

    windowData.fbo->blitFromFramebuffer(r.toRect(), QRect(QPoint(), contrastTexture->size()));

    m_shader->setColorMatrix(m_windowData[w].colorMatrix);
    m_shader->bind();

    m_shader->setOpacity(opacity);
    // Set up the texture matrix to transform from screen coordinates
    // to texture coordinates.
    const QRectF boundingRect = shape.boundingRect();
    QMatrix4x4 textureMatrix;
    textureMatrix.scale(1, -1);
    textureMatrix.translate(0, -1);
    // apply texture->buffer transformation
    textureMatrix.translate(0.5, 0.5);
    textureMatrix *= renderTarget.transform().toMatrix();
    textureMatrix.translate(-0.5, -0.5);
    // scaled logical to texture coordinates
    textureMatrix.scale(1.0 / boundingRect.width(), 1.0 / boundingRect.height(), 1);
    textureMatrix.translate(-boundingRect.x(), -boundingRect.y(), 0);
    textureMatrix.scale(1.0 / viewport.scale(), 1.0 / viewport.scale());

    m_shader->setTextureMatrix(textureMatrix);
    m_shader->setModelViewProjectionMatrix(screenProjection);

    vbo->draw(GL_TRIANGLES, 0, shape.rectCount() * 6);

    contrastTexture->unbind();

    vbo->unbindArrays();

    if (opacity < 1.0) {
        glDisable(GL_BLEND);
    }

    m_shader->unbind();
}

bool ContrastEffect::isActive() const
{
    return !effects->isScreenLocked();
}

bool ContrastEffect::blocksDirectScanout() const
{
    return false;
}

#if HAVE_VULKAN

bool ContrastEffect::initVulkanResources()
{
    auto *scene = Compositor::self()->scene();
    if (!scene)
        return false;
    auto *renderer = static_cast<ItemRendererVulkan *>(scene->renderer());
    if (!renderer)
        return false;
    m_vulkanCtx = renderer->context();
    if (!m_vulkanCtx)
        return false;

    VkDevice device = m_vulkanCtx->backend()->device();
    VkFormat swapchainFormat = m_vulkanCtx->backend()->colorFormat();

    // Nearest sampler — 1:1 pixel capture, no filtering needed
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_vulkanSampler) != VK_SUCCESS)
        return false;

    // Descriptor set layout: binding 0 = combined image sampler (immutable)
    VkDescriptorSetLayoutBinding dsBinding{};
    dsBinding.binding = 0;
    dsBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsBinding.descriptorCount = 1;
    dsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dsBinding.pImmutableSamplers = &m_vulkanSampler;

    VkDescriptorSetLayoutCreateInfo dsLayoutInfo{};
    dsLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutInfo.bindingCount = 1;
    dsLayoutInfo.pBindings = &dsBinding;
    if (vkCreateDescriptorSetLayout(device, &dsLayoutInfo, nullptr, &m_vulkanDsLayout) != VK_SUCCESS)
        return false;

    // Pipeline layout: descriptor set + push constants {mat4 colorMatrix, float opacity} = 68 bytes
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = 68;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_vulkanDsLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_vulkanPipelineLayout) != VK_SUCCESS)
        return false;

    // Render pass: LOAD_OP_LOAD to preserve pixels outside the contrast region.
    // Begins in COLOR_ATTACHMENT_OPTIMAL, ends in PRESENT_SRC_KHR (same invariant as blur's resume pass).
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapchainFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAttachment;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dep;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &m_vulkanRenderPass) != VK_SUCCESS)
            return false;
    }

    // Shader modules
    auto makeModule = [&](const uint32_t *code, size_t byteSize) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = byteSize;
        info.pCode = code;
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &info, nullptr, &mod);
        return mod;
    };

    VkShaderModule vertMod = makeModule(s_contrastBgVertSpv, sizeof(s_contrastBgVertSpv));
    VkShaderModule fragMod = makeModule(s_contrastBgFragSpv, sizeof(s_contrastBgFragSpv));

    if (!vertMod || !fragMod) {
        if (vertMod)
            vkDestroyShaderModule(device, vertMod, nullptr);
        if (fragMod)
            vkDestroyShaderModule(device, fragMod, nullptr);
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
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttach;

    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vi;
    pipeInfo.pInputAssemblyState = &ia;
    pipeInfo.pViewportState = &vpState;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pColorBlendState = &blend;
    pipeInfo.pDynamicState = &dyn;
    pipeInfo.layout = m_vulkanPipelineLayout;
    pipeInfo.renderPass = m_vulkanRenderPass;

    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &m_vulkanPipeline);

    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);

    if (result != VK_SUCCESS)
        return false;

    m_vulkanValid = true;
    return true;
}

void ContrastEffect::doContrastVulkan(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, const QRegion &shape, float opacity)
{
    if (!m_windowData.contains(w))
        return;

    auto *scene = Compositor::self()->scene();
    if (!scene)
        return;
    auto *renderer = static_cast<ItemRendererVulkan *>(scene->renderer());
    // FIXME: also samples currentFramebuffer() — under a recursive paint flow
    // we'd want the caller's framebuffer instead.
    VkCommandBuffer cmd = renderer->activeCommandBuffer(renderTarget);
    VulkanFramebuffer *fb = renderer->currentFramebuffer();
    if (!cmd || !fb)
        return;

    VkImage swapchainImage = fb->colorImage();
    if (swapchainImage == VK_NULL_HANDLE)
        return;

    VkDevice device = m_vulkanCtx->backend()->device();

    const QRect backgroundRect = shape.boundingRect();
    if (backgroundRect.isEmpty())
        return;
    const QRect deviceRect = snapToPixelGrid(scaledRect(backgroundRect, viewport.scale()));
    if (deviceRect.isEmpty())
        return;

    // Allocate or resize the per-window capture texture
    auto &windowData = m_windowData[w];
    static constexpr VkFormat kCaptureFmt = VK_FORMAT_B8G8R8A8_UNORM;
    if (!windowData.vkTexture || windowData.vkCaptureSize != deviceRect.size()) {
        windowData.vkTexture = VulkanTexture::createRenderTarget(m_vulkanCtx, deviceRect.size(), kCaptureFmt);
        if (!windowData.vkTexture)
            return;
        windowData.vkCaptureSize = deviceRect.size();
        windowData.vkNeedsInit = true;
    }
    VulkanTexture *tex = windowData.vkTexture.get();

    auto imgBarrier = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    auto memBarrier = [&](VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &b, 0, nullptr, 0, nullptr);
    };

    // Transition capture texture from UNDEFINED → GENERAL on first use
    if (windowData.vkNeedsInit) {
        imgBarrier(tex->image(),
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        windowData.vkNeedsInit = false;
    }

    // End the active render pass so we can use the swapchain as a transfer source
    vkCmdEndRenderPass(cmd);

    // Swapchain: PRESENT_SRC → TRANSFER_SRC (capture)
    imgBarrier(swapchainImage,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Blit contrast region from swapchain → capture texture
    {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {deviceRect.x(), deviceRect.y(), 0};
        blit.srcOffsets[1] = {deviceRect.x() + deviceRect.width(), deviceRect.y() + deviceRect.height(), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {tex->width(), tex->height(), 1};
        vkCmdBlitImage(cmd,
                       swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tex->image(), VK_IMAGE_LAYOUT_GENERAL,
                       1, &blit, VK_FILTER_NEAREST);
    }

    // Make blit writes to capture texture visible to the fragment shader
    memBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // Swapchain: TRANSFER_SRC → COLOR_ATTACHMENT_OPTIMAL (for rendering the contrast result)
    imgBarrier(swapchainImage,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // Allocate and write descriptor set for the capture texture
    VkDescriptorSet ds = m_vulkanCtx->allocateDescriptorSet(m_vulkanDsLayout);
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView = tex->imageView();
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = ds;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);

    // Push constants: 4x4 color matrix (column-major, matches GL convention) + opacity
    struct ContrastPc
    {
        float colorMatrix[16];
        float opacity;
    };
    ContrastPc pc{};
    const float *matData = windowData.colorMatrix.data();
    std::copy(matData, matData + 16, pc.colorMatrix);
    pc.opacity = opacity;

    // Begin contrast render pass on the swapchain framebuffer (LOAD_OP_LOAD)
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_vulkanRenderPass;
    rpBegin.framebuffer = fb->framebuffer();
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {static_cast<uint32_t>(fb->width()), static_cast<uint32_t>(fb->height())};
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vulkanPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vulkanPipelineLayout,
                            0, 1, &ds, 0, nullptr);
    vkCmdPushConstants(cmd, m_vulkanPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);

    // Viewport mapped to contrast region (Y-flipped for Vulkan)
    VkViewport vp{};
    vp.x = static_cast<float>(deviceRect.x());
    vp.y = static_cast<float>(deviceRect.y() + deviceRect.height());
    vp.width = static_cast<float>(deviceRect.width());
    vp.height = -static_cast<float>(deviceRect.height());
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {deviceRect.x(), deviceRect.y()};
    sc.extent = {static_cast<uint32_t>(deviceRect.width()), static_cast<uint32_t>(deviceRect.height())};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    // Restore full-screen viewport/scissor so subsequent window rendering is correct
    VkViewport fullVp{};
    fullVp.x = 0.0f;
    fullVp.y = static_cast<float>(fb->height());
    fullVp.width = static_cast<float>(fb->width());
    fullVp.height = -static_cast<float>(fb->height());
    fullVp.minDepth = 0.0f;
    fullVp.maxDepth = 1.0f;
    VkRect2D fullSc{};
    fullSc.extent = {static_cast<uint32_t>(fb->width()), static_cast<uint32_t>(fb->height())};
    vkCmdSetViewport(cmd, 0, 1, &fullVp);
    vkCmdSetScissor(cmd, 0, 1, &fullSc);

    // Leave the render pass active — the window itself will be drawn into it by effects->drawWindow()
}

#endif // HAVE_VULKAN

} // namespace KWin

#include "moc_contrast.cpp"
