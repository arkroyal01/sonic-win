/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QImage>
#include <QTest>

#include "config-kwin.h"

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include <libdrm/drm_fourcc.h>
#include <vulkan/vulkan.h>
#endif

using namespace KWin;

/**
 * @brief Tests for Vulkan format conversion functions.
 *
 * These tests call the real production functions directly.
 * A test failure here means the production mapping changed and the caller
 * (surface texture import, DMA-BUF import, or offscreen rendering) may be broken.
 */
class VulkanFormatTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testDrmFormatToVkFormat_data();
    void testDrmFormatToVkFormat();
    void testQImageFormatToVkFormat_data();
    void testQImageFormatToVkFormat();
};

#if HAVE_VULKAN

void VulkanFormatTest::testDrmFormatToVkFormat_data()
{
    QTest::addColumn<uint32_t>("drmFormat");
    QTest::addColumn<int>("expectedVkFormat");

    // Colour formats: X11 pixmaps are sRGB-encoded, so production uses SRGB formats.
    // If these change to UNORM, X11 window colours will be rendered with wrong gamma.
    QTest::newRow("ARGB8888 -> SRGB") << uint32_t(DRM_FORMAT_ARGB8888) << int(VK_FORMAT_B8G8R8A8_SRGB);
    QTest::newRow("XRGB8888 -> SRGB") << uint32_t(DRM_FORMAT_XRGB8888) << int(VK_FORMAT_B8G8R8A8_SRGB);
    QTest::newRow("ABGR8888 -> SRGB") << uint32_t(DRM_FORMAT_ABGR8888) << int(VK_FORMAT_R8G8B8A8_SRGB);
    QTest::newRow("XBGR8888 -> SRGB") << uint32_t(DRM_FORMAT_XBGR8888) << int(VK_FORMAT_R8G8B8A8_SRGB);
    // RGB888/BGR888 use 4-component SRGB for driver compatibility (alpha ignored)
    QTest::newRow("RGB888  -> B8G8R8A8_SRGB") << uint32_t(DRM_FORMAT_RGB888) << int(VK_FORMAT_R8G8B8A8_SRGB);
    QTest::newRow("BGR888  -> R8G8B8A8_SRGB") << uint32_t(DRM_FORMAT_BGR888) << int(VK_FORMAT_B8G8R8A8_SRGB);
    // Wide/HDR formats stay UNORM (no sRGB equivalent in standard Vulkan)
    QTest::newRow("RGB565  -> UNORM") << uint32_t(DRM_FORMAT_RGB565) << int(VK_FORMAT_R5G6B5_UNORM_PACK16);
    QTest::newRow("BGR565  -> UNORM") << uint32_t(DRM_FORMAT_BGR565) << int(VK_FORMAT_B5G6R5_UNORM_PACK16);
    QTest::newRow("ARGB2101010 -> UNORM") << uint32_t(DRM_FORMAT_ARGB2101010) << int(VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    QTest::newRow("XRGB2101010 -> UNORM") << uint32_t(DRM_FORMAT_XRGB2101010) << int(VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    QTest::newRow("ABGR2101010 -> UNORM") << uint32_t(DRM_FORMAT_ABGR2101010) << int(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    QTest::newRow("XBGR2101010 -> UNORM") << uint32_t(DRM_FORMAT_XBGR2101010) << int(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    // YUV plane formats
    QTest::newRow("R8   -> R8_UNORM") << uint32_t(DRM_FORMAT_R8) << int(VK_FORMAT_R8_UNORM);
    QTest::newRow("GR88 -> R8G8_UNORM") << uint32_t(DRM_FORMAT_GR88) << int(VK_FORMAT_R8G8_UNORM);
    QTest::newRow("RG88 -> R8G8_UNORM") << uint32_t(DRM_FORMAT_RG88) << int(VK_FORMAT_R8G8_UNORM);
    // Unknown format returns UNDEFINED
    QTest::newRow("unknown -> UNDEFINED") << uint32_t(0x12345678) << int(VK_FORMAT_UNDEFINED);
}

void VulkanFormatTest::testDrmFormatToVkFormat()
{
    QFETCH(uint32_t, drmFormat);
    QFETCH(int, expectedVkFormat);

    QCOMPARE(int(VulkanContext::drmFormatToVkFormat(drmFormat)), expectedVkFormat);
}

void VulkanFormatTest::testQImageFormatToVkFormat_data()
{
    QTest::addColumn<int>("qImageFormat");
    QTest::addColumn<int>("expectedVkFormat");

    // Qt paints in sRGB, so production uses SRGB Vulkan formats for proper gamma on sampling.
    // A change to UNORM here would silently double-apply gamma correction on every frame.
    QTest::newRow("RGBA8888            -> R8G8B8A8_SRGB") << int(QImage::Format_RGBA8888) << int(VK_FORMAT_R8G8B8A8_SRGB);
    QTest::newRow("RGBA8888_Pre        -> R8G8B8A8_SRGB") << int(QImage::Format_RGBA8888_Premultiplied) << int(VK_FORMAT_R8G8B8A8_SRGB);
    QTest::newRow("RGBX8888            -> R8G8B8A8_SRGB") << int(QImage::Format_RGBX8888) << int(VK_FORMAT_R8G8B8A8_SRGB);
    QTest::newRow("RGB888              -> R8G8B8_SRGB") << int(QImage::Format_RGB888) << int(VK_FORMAT_R8G8B8_SRGB);
    QTest::newRow("ARGB32              -> B8G8R8A8_SRGB") << int(QImage::Format_ARGB32) << int(VK_FORMAT_B8G8R8A8_SRGB);
    QTest::newRow("ARGB32_Pre          -> B8G8R8A8_SRGB") << int(QImage::Format_ARGB32_Premultiplied) << int(VK_FORMAT_B8G8R8A8_SRGB);
    QTest::newRow("RGB32               -> B8G8R8A8_SRGB") << int(QImage::Format_RGB32) << int(VK_FORMAT_B8G8R8A8_SRGB);
    // Grayscale and HDR formats have no sRGB Vulkan equivalent — stay UNORM
    QTest::newRow("Grayscale8          -> R8_UNORM") << int(QImage::Format_Grayscale8) << int(VK_FORMAT_R8_UNORM);
    QTest::newRow("Grayscale16         -> R16_UNORM") << int(QImage::Format_Grayscale16) << int(VK_FORMAT_R16_UNORM);
    QTest::newRow("RGBA64              -> R16G16B16A16_UNORM") << int(QImage::Format_RGBA64) << int(VK_FORMAT_R16G16B16A16_UNORM);
    QTest::newRow("RGBA64_Pre          -> R16G16B16A16_UNORM") << int(QImage::Format_RGBA64_Premultiplied) << int(VK_FORMAT_R16G16B16A16_UNORM);
    QTest::newRow("RGBX64              -> R16G16B16A16_UNORM") << int(QImage::Format_RGBX64) << int(VK_FORMAT_R16G16B16A16_UNORM);
    QTest::newRow("RGBA16FPx4          -> R16G16B16A16_SFLOAT") << int(QImage::Format_RGBA16FPx4) << int(VK_FORMAT_R16G16B16A16_SFLOAT);
    QTest::newRow("RGBA16FPx4_Pre      -> R16G16B16A16_SFLOAT") << int(QImage::Format_RGBA16FPx4_Premultiplied) << int(VK_FORMAT_R16G16B16A16_SFLOAT);
    QTest::newRow("RGBA32FPx4          -> R32G32B32A32_SFLOAT") << int(QImage::Format_RGBA32FPx4) << int(VK_FORMAT_R32G32B32A32_SFLOAT);
    QTest::newRow("RGBA32FPx4_Pre      -> R32G32B32A32_SFLOAT") << int(QImage::Format_RGBA32FPx4_Premultiplied) << int(VK_FORMAT_R32G32B32A32_SFLOAT);
    // Unsupported formats return UNDEFINED
    QTest::newRow("Mono    -> UNDEFINED") << int(QImage::Format_Mono) << int(VK_FORMAT_UNDEFINED);
    QTest::newRow("Invalid -> UNDEFINED") << int(QImage::Format_Invalid) << int(VK_FORMAT_UNDEFINED);
}

void VulkanFormatTest::testQImageFormatToVkFormat()
{
    QFETCH(int, qImageFormat);
    QFETCH(int, expectedVkFormat);

    QCOMPARE(int(VulkanTexture::qImageFormatToVkFormat(QImage::Format(qImageFormat))), expectedVkFormat);
}

#else // !HAVE_VULKAN

void VulkanFormatTest::testDrmFormatToVkFormat_data()
{
}
void VulkanFormatTest::testDrmFormatToVkFormat()
{
    QSKIP("Vulkan support not available");
}
void VulkanFormatTest::testQImageFormatToVkFormat_data()
{
}
void VulkanFormatTest::testQImageFormatToVkFormat()
{
    QSKIP("Vulkan support not available");
}

#endif // HAVE_VULKAN

QTEST_GUILESS_MAIN(VulkanFormatTest)
#include "vulkan_format_test.moc"
