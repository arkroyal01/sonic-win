/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2011 Philipp Knechtges <philipp-dev@knechtges.com>
    SPDX-FileCopyrightText: 2018 Alex Nemeth <alex.nemeth329@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "blur.h"
// KConfigSkeleton
#include "blurconfig.h"

#include "config-kwin.h"

#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glplatform.h"
#include "scene/decorationitem.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "window.h"

#include "utils/xcbutils.h"

#if HAVE_VULKAN
#include "compositor.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene.h"
#endif

#include <QGuiApplication>
#include <QMatrix4x4>
#include <QScreen>
#include <QTime>
#include <QTimer>
#include <QWindow>
#include <cmath> // for ceil()
#include <cstdlib>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KDecoration3/Decoration>

Q_LOGGING_CATEGORY(KWIN_BLUR, "kwin_effect_blur", QtWarningMsg)

#if HAVE_VULKAN
// clang-format off
static const uint32_t s_blurVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000030, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00000021,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000c,
    0x0000000b, 0x0000002a, 0x00030047, 0x0000001f, 0x00000002, 0x00050048,
    0x0000001f, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001f,
    0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000001f, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x0000001f, 0x00000003, 0x0000000b,
    0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000002, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b,
    0x00000008, 0x00000009, 0x00000003, 0x00040015, 0x0000000a, 0x00000020,
    0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b,
    0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e,
    0x00000001, 0x00020014, 0x0000000f, 0x0004002b, 0x00000006, 0x00000011,
    0x40000000, 0x0004002b, 0x00000006, 0x00000012, 0x00000000, 0x0004002b,
    0x0000000a, 0x00000015, 0x00000002, 0x0004002b, 0x00000006, 0x00000017,
    0xbf800000, 0x0004002b, 0x00000006, 0x00000018, 0x3f800000, 0x00040017,
    0x0000001b, 0x00000006, 0x00000004, 0x00040015, 0x0000001c, 0x00000020,
    0x00000000, 0x0004002b, 0x0000001c, 0x0000001d, 0x00000001, 0x0004001c,
    0x0000001e, 0x00000006, 0x0000001d, 0x0006001e, 0x0000001f, 0x0000001b,
    0x00000006, 0x0000001e, 0x0000001e, 0x00040020, 0x00000020, 0x00000003,
    0x0000001f, 0x0004003b, 0x00000020, 0x00000021, 0x00000003, 0x0004002b,
    0x0000000a, 0x00000022, 0x00000000, 0x0004002b, 0x0000001c, 0x00000023,
    0x00000000, 0x00040020, 0x00000024, 0x00000003, 0x00000006, 0x00040020,
    0x0000002e, 0x00000003, 0x0000001b, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a,
    0x0000000d, 0x0000000c, 0x000500aa, 0x0000000f, 0x00000010, 0x0000000d,
    0x0000000e, 0x000600a9, 0x00000006, 0x00000013, 0x00000010, 0x00000011,
    0x00000012, 0x000500aa, 0x0000000f, 0x00000016, 0x0000000d, 0x00000015,
    0x000600a9, 0x00000006, 0x00000019, 0x00000016, 0x00000017, 0x00000018,
    0x00050050, 0x00000007, 0x0000001a, 0x00000013, 0x00000019, 0x0003003e,
    0x00000009, 0x0000001a, 0x00050041, 0x00000024, 0x00000025, 0x00000009,
    0x00000023, 0x0004003d, 0x00000006, 0x00000026, 0x00000025, 0x00050085,
    0x00000006, 0x00000027, 0x00000026, 0x00000011, 0x00050083, 0x00000006,
    0x00000028, 0x00000027, 0x00000018, 0x00050041, 0x00000024, 0x00000029,
    0x00000009, 0x0000001d, 0x0004003d, 0x00000006, 0x0000002a, 0x00000029,
    0x00050085, 0x00000006, 0x0000002b, 0x0000002a, 0x00000011, 0x00050083,
    0x00000006, 0x0000002c, 0x00000018, 0x0000002b, 0x00070050, 0x0000001b,
    0x0000002d, 0x00000028, 0x0000002c, 0x00000012, 0x00000018, 0x00050041,
    0x0000002e, 0x0000002f, 0x00000021, 0x00000022, 0x0003003e, 0x0000002f,
    0x0000002d, 0x000100fd, 0x00010038,
};

static const uint32_t s_blurDownsampleSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000005e, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000011, 0x00000056, 0x00030010,
    0x00000004, 0x00000007, 0x00040047, 0x0000000d, 0x00000021, 0x00000000,
    0x00040047, 0x0000000d, 0x00000022, 0x00000000, 0x00040047, 0x00000011,
    0x0000001e, 0x00000000, 0x00030047, 0x00000018, 0x00000002, 0x00050048,
    0x00000018, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000018,
    0x00000001, 0x00000023, 0x00000008, 0x00040047, 0x00000056, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00090019, 0x0000000a, 0x00000006, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000b,
    0x0000000a, 0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b,
    0x0000000c, 0x0000000d, 0x00000000, 0x00040017, 0x0000000f, 0x00000006,
    0x00000002, 0x00040020, 0x00000010, 0x00000001, 0x0000000f, 0x0004003b,
    0x00000010, 0x00000011, 0x00000001, 0x0004002b, 0x00000006, 0x00000014,
    0x40800000, 0x0004001e, 0x00000018, 0x0000000f, 0x00000006, 0x00040020,
    0x00000019, 0x00000009, 0x00000018, 0x0004003b, 0x00000019, 0x0000001a,
    0x00000009, 0x00040015, 0x0000001b, 0x00000020, 0x00000001, 0x0004002b,
    0x0000001b, 0x0000001c, 0x00000000, 0x00040020, 0x0000001d, 0x00000009,
    0x0000000f, 0x0004002b, 0x0000001b, 0x00000020, 0x00000001, 0x00040020,
    0x00000021, 0x00000009, 0x00000006, 0x00040015, 0x00000036, 0x00000020,
    0x00000000, 0x0004002b, 0x00000036, 0x00000037, 0x00000000, 0x0004002b,
    0x00000036, 0x0000003a, 0x00000001, 0x00040020, 0x00000055, 0x00000003,
    0x00000007, 0x0004003b, 0x00000055, 0x00000056, 0x00000003, 0x0004002b,
    0x00000006, 0x0000005c, 0x3e000000, 0x0007002c, 0x00000007, 0x0000005d,
    0x0000005c, 0x0000005c, 0x0000005c, 0x0000005c, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d,
    0x0000000b, 0x0000000e, 0x0000000d, 0x0004003d, 0x0000000f, 0x00000012,
    0x00000011, 0x00050057, 0x00000007, 0x00000013, 0x0000000e, 0x00000012,
    0x0005008e, 0x00000007, 0x00000015, 0x00000013, 0x00000014, 0x0004003d,
    0x0000000b, 0x00000016, 0x0000000d, 0x00050041, 0x0000001d, 0x0000001e,
    0x0000001a, 0x0000001c, 0x0004003d, 0x0000000f, 0x0000001f, 0x0000001e,
    0x00050041, 0x00000021, 0x00000022, 0x0000001a, 0x00000020, 0x0004003d,
    0x00000006, 0x00000023, 0x00000022, 0x0005008e, 0x0000000f, 0x00000024,
    0x0000001f, 0x00000023, 0x00050083, 0x0000000f, 0x00000025, 0x00000012,
    0x00000024, 0x00050057, 0x00000007, 0x00000026, 0x00000016, 0x00000025,
    0x00050081, 0x00000007, 0x00000028, 0x00000015, 0x00000026, 0x0004003d,
    0x0000000b, 0x00000029, 0x0000000d, 0x00050081, 0x0000000f, 0x00000030,
    0x00000012, 0x00000024, 0x00050057, 0x00000007, 0x00000031, 0x00000029,
    0x00000030, 0x00050081, 0x00000007, 0x00000033, 0x00000028, 0x00000031,
    0x0004003d, 0x0000000b, 0x00000034, 0x0000000d, 0x00060041, 0x00000021,
    0x00000038, 0x0000001a, 0x0000001c, 0x00000037, 0x0004003d, 0x00000006,
    0x00000039, 0x00000038, 0x00060041, 0x00000021, 0x0000003b, 0x0000001a,
    0x0000001c, 0x0000003a, 0x0004003d, 0x00000006, 0x0000003c, 0x0000003b,
    0x0004007f, 0x00000006, 0x0000003d, 0x0000003c, 0x00050050, 0x0000000f,
    0x0000003e, 0x00000039, 0x0000003d, 0x0005008e, 0x0000000f, 0x00000041,
    0x0000003e, 0x00000023, 0x00050081, 0x0000000f, 0x00000042, 0x00000012,
    0x00000041, 0x00050057, 0x00000007, 0x00000043, 0x00000034, 0x00000042,
    0x00050081, 0x00000007, 0x00000045, 0x00000033, 0x00000043, 0x0004003d,
    0x0000000b, 0x00000046, 0x0000000d, 0x0004007f, 0x00000006, 0x0000004a,
    0x00000039, 0x00050050, 0x0000000f, 0x0000004d, 0x0000004a, 0x0000003c,
    0x0005008e, 0x0000000f, 0x00000050, 0x0000004d, 0x00000023, 0x00050081,
    0x0000000f, 0x00000051, 0x00000012, 0x00000050, 0x00050057, 0x00000007,
    0x00000052, 0x00000046, 0x00000051, 0x00050081, 0x00000007, 0x00000054,
    0x00000045, 0x00000052, 0x00050085, 0x00000007, 0x0000005a, 0x00000054,
    0x0000005d, 0x0003003e, 0x00000056, 0x0000005a, 0x000100fd, 0x00010038,
};

static const uint32_t s_blurUpsampleSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000009b, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000011, 0x00000092, 0x00030010,
    0x00000004, 0x00000007, 0x00040047, 0x0000000d, 0x00000021, 0x00000000,
    0x00040047, 0x0000000d, 0x00000022, 0x00000000, 0x00040047, 0x00000011,
    0x0000001e, 0x00000000, 0x00030047, 0x00000013, 0x00000002, 0x00050048,
    0x00000013, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000013,
    0x00000001, 0x00000023, 0x00000008, 0x00040047, 0x00000092, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00090019, 0x0000000a, 0x00000006, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000b,
    0x0000000a, 0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b,
    0x0000000c, 0x0000000d, 0x00000000, 0x00040017, 0x0000000f, 0x00000006,
    0x00000002, 0x00040020, 0x00000010, 0x00000001, 0x0000000f, 0x0004003b,
    0x00000010, 0x00000011, 0x00000001, 0x0004001e, 0x00000013, 0x0000000f,
    0x00000006, 0x00040020, 0x00000014, 0x00000009, 0x00000013, 0x0004003b,
    0x00000014, 0x00000015, 0x00000009, 0x00040015, 0x00000016, 0x00000020,
    0x00000001, 0x0004002b, 0x00000016, 0x00000017, 0x00000000, 0x00040015,
    0x00000018, 0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019,
    0x00000000, 0x00040020, 0x0000001a, 0x00000009, 0x00000006, 0x0004002b,
    0x00000006, 0x0000001e, 0x40000000, 0x0004002b, 0x00000006, 0x00000020,
    0x00000000, 0x0004002b, 0x00000016, 0x00000022, 0x00000001, 0x0004002b,
    0x00000018, 0x0000002d, 0x00000001, 0x00040020, 0x00000091, 0x00000003,
    0x00000007, 0x0004003b, 0x00000091, 0x00000092, 0x00000003, 0x0004002b,
    0x00000006, 0x00000098, 0xc0000000, 0x0004002b, 0x00000006, 0x00000099,
    0x3daaaaab, 0x0007002c, 0x00000007, 0x0000009a, 0x00000099, 0x00000099,
    0x00000099, 0x00000099, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000b, 0x0000000e,
    0x0000000d, 0x0004003d, 0x0000000f, 0x00000012, 0x00000011, 0x00060041,
    0x0000001a, 0x0000001b, 0x00000015, 0x00000017, 0x00000019, 0x0004003d,
    0x00000006, 0x0000001c, 0x0000001b, 0x0004007f, 0x00000006, 0x0000001d,
    0x0000001c, 0x00050085, 0x00000006, 0x0000001f, 0x0000001c, 0x00000098,
    0x00050050, 0x0000000f, 0x00000021, 0x0000001f, 0x00000020, 0x00050041,
    0x0000001a, 0x00000023, 0x00000015, 0x00000022, 0x0004003d, 0x00000006,
    0x00000024, 0x00000023, 0x0005008e, 0x0000000f, 0x00000025, 0x00000021,
    0x00000024, 0x00050081, 0x0000000f, 0x00000026, 0x00000012, 0x00000025,
    0x00050057, 0x00000007, 0x00000027, 0x0000000e, 0x00000026, 0x0004003d,
    0x0000000b, 0x00000028, 0x0000000d, 0x00060041, 0x0000001a, 0x0000002e,
    0x00000015, 0x00000017, 0x0000002d, 0x0004003d, 0x00000006, 0x0000002f,
    0x0000002e, 0x00050050, 0x0000000f, 0x00000030, 0x0000001d, 0x0000002f,
    0x0005008e, 0x0000000f, 0x00000033, 0x00000030, 0x00000024, 0x00050081,
    0x0000000f, 0x00000034, 0x00000012, 0x00000033, 0x00050057, 0x00000007,
    0x00000035, 0x00000028, 0x00000034, 0x0005008e, 0x00000007, 0x00000036,
    0x00000035, 0x0000001e, 0x00050081, 0x00000007, 0x00000038, 0x00000027,
    0x00000036, 0x0004003d, 0x0000000b, 0x00000039, 0x0000000d, 0x00050085,
    0x00000006, 0x0000003d, 0x0000002f, 0x0000001e, 0x00050050, 0x0000000f,
    0x0000003e, 0x00000020, 0x0000003d, 0x0005008e, 0x0000000f, 0x00000041,
    0x0000003e, 0x00000024, 0x00050081, 0x0000000f, 0x00000042, 0x00000012,
    0x00000041, 0x00050057, 0x00000007, 0x00000043, 0x00000039, 0x00000042,
    0x00050081, 0x00000007, 0x00000045, 0x00000038, 0x00000043, 0x0004003d,
    0x0000000b, 0x00000046, 0x0000000d, 0x00050050, 0x0000000f, 0x0000004c,
    0x0000001c, 0x0000002f, 0x0005008e, 0x0000000f, 0x0000004f, 0x0000004c,
    0x00000024, 0x00050081, 0x0000000f, 0x00000050, 0x00000012, 0x0000004f,
    0x00050057, 0x00000007, 0x00000051, 0x00000046, 0x00000050, 0x0005008e,
    0x00000007, 0x00000052, 0x00000051, 0x0000001e, 0x00050081, 0x00000007,
    0x00000054, 0x00000045, 0x00000052, 0x0004003d, 0x0000000b, 0x00000055,
    0x0000000d, 0x00050085, 0x00000006, 0x00000059, 0x0000002f, 0x00000098,
    0x00050050, 0x0000000f, 0x0000005a, 0x00000059, 0x00000020, 0x0005008e,
    0x0000000f, 0x0000005d, 0x0000005a, 0x00000024, 0x00050081, 0x0000000f,
    0x0000005e, 0x00000012, 0x0000005d, 0x00050057, 0x00000007, 0x0000005f,
    0x00000055, 0x0000005e, 0x00050081, 0x00000007, 0x00000061, 0x00000054,
    0x0000005f, 0x0004003d, 0x0000000b, 0x00000062, 0x0000000d, 0x0004007f,
    0x00000006, 0x00000068, 0x0000002f, 0x00050050, 0x0000000f, 0x00000069,
    0x0000001c, 0x00000068, 0x0005008e, 0x0000000f, 0x0000006c, 0x00000069,
    0x00000024, 0x00050081, 0x0000000f, 0x0000006d, 0x00000012, 0x0000006c,
    0x00050057, 0x00000007, 0x0000006e, 0x00000062, 0x0000006d, 0x0005008e,
    0x00000007, 0x0000006f, 0x0000006e, 0x0000001e, 0x00050081, 0x00000007,
    0x00000071, 0x00000061, 0x0000006f, 0x0004003d, 0x0000000b, 0x00000072,
    0x0000000d, 0x00050085, 0x00000006, 0x00000077, 0x0000002f, 0x00000098,
    0x00050050, 0x0000000f, 0x00000078, 0x00000020, 0x00000077, 0x0005008e,
    0x0000000f, 0x0000007b, 0x00000078, 0x00000024, 0x00050081, 0x0000000f,
    0x0000007c, 0x00000012, 0x0000007b, 0x00050057, 0x00000007, 0x0000007d,
    0x00000072, 0x0000007c, 0x00050081, 0x00000007, 0x0000007f, 0x00000071,
    0x0000007d, 0x0004003d, 0x0000000b, 0x00000080, 0x0000000d, 0x00050050,
    0x0000000f, 0x00000088, 0x0000001d, 0x00000068, 0x0005008e, 0x0000000f,
    0x0000008b, 0x00000088, 0x00000024, 0x00050081, 0x0000000f, 0x0000008c,
    0x00000012, 0x0000008b, 0x00050057, 0x00000007, 0x0000008d, 0x00000080,
    0x0000008c, 0x0005008e, 0x00000007, 0x0000008e, 0x0000008d, 0x0000001e,
    0x00050081, 0x00000007, 0x00000090, 0x0000007f, 0x0000008e, 0x00050085,
    0x00000007, 0x00000096, 0x00000090, 0x0000009a, 0x0003003e, 0x00000092,
    0x00000096, 0x000100fd, 0x00010038,
};
// clang-format on
#endif // HAVE_VULKAN

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(blur);
}

namespace KWin
{

static const QByteArray s_blurAtomName = QByteArrayLiteral("_KDE_NET_WM_BLUR_BEHIND_REGION");

BlurManagerInterface *BlurEffect::s_blurManager = nullptr;
QTimer *BlurEffect::s_blurManagerRemoveTimer = nullptr;

BlurEffect::BlurEffect()
{
    BlurConfig::instance(effects->config());
    ensureResources();

#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        initBlurStrengthValues();
        reconfigure(ReconfigureAll);
        if (!initVulkanResources()) {
            qCWarning(KWIN_BLUR) << "Failed to initialise Vulkan blur resources";
            return;
        }
        // Skip GL shader loading below
        goto skip_gl_init;
    }
#endif

    m_contrastPass.shader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture,
                                                                                QStringLiteral(":/effects/blur/shaders/vertex.vert"),
                                                                                QStringLiteral(":/effects/blur/shaders/contrast.frag"));
    if (!m_contrastPass.shader) {
        qCWarning(KWIN_BLUR) << "Failed to load contrast pass shader";
        return;
    } else {
        m_contrastPass.mvpMatrixLocation = m_contrastPass.shader->uniformLocation("modelViewProjectionMatrix");
        m_contrastPass.offsetLocation = m_contrastPass.shader->uniformLocation("offset");
        m_contrastPass.halfpixelLocation = m_contrastPass.shader->uniformLocation("halfpixel");
    }

    m_roundedContrastPass.shader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture,
                                                                                       QStringLiteral(":/effects/blur/shaders/contrast_rounded.vert"),
                                                                                       QStringLiteral(":/effects/blur/shaders/contrast_rounded.frag"));
    if (!m_roundedContrastPass.shader) {
        qCWarning(KWIN_BLUR) << "Failed to load contrast pass shader";
        return;
    } else {
        m_roundedContrastPass.mvpMatrixLocation = m_roundedContrastPass.shader->uniformLocation("modelViewProjectionMatrix");
        m_roundedContrastPass.offsetLocation = m_roundedContrastPass.shader->uniformLocation("offset");
        m_roundedContrastPass.halfpixelLocation = m_roundedContrastPass.shader->uniformLocation("halfpixel");
        m_roundedContrastPass.boxLocation = m_roundedContrastPass.shader->uniformLocation("box");
        m_roundedContrastPass.cornerRadiusLocation = m_roundedContrastPass.shader->uniformLocation("cornerRadius");
        m_roundedContrastPass.opacityLocation = m_roundedContrastPass.shader->uniformLocation("opacity");
    }

    m_downsamplePass.shader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture,
                                                                                  QStringLiteral(":/effects/blur/shaders/vertex.vert"),
                                                                                  QStringLiteral(":/effects/blur/shaders/downsample.frag"));
    if (!m_downsamplePass.shader) {
        qCWarning(KWIN_BLUR) << "Failed to load downsampling pass shader";
        return;
    } else {
        m_downsamplePass.mvpMatrixLocation = m_downsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_downsamplePass.offsetLocation = m_downsamplePass.shader->uniformLocation("offset");
        m_downsamplePass.halfpixelLocation = m_downsamplePass.shader->uniformLocation("halfpixel");
    }

    m_upsamplePass.shader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture,
                                                                                QStringLiteral(":/effects/blur/shaders/vertex.vert"),
                                                                                QStringLiteral(":/effects/blur/shaders/upsample.frag"));
    if (!m_upsamplePass.shader) {
        qCWarning(KWIN_BLUR) << "Failed to load upsampling pass shader";
        return;
    } else {
        m_upsamplePass.mvpMatrixLocation = m_upsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_upsamplePass.offsetLocation = m_upsamplePass.shader->uniformLocation("offset");
        m_upsamplePass.halfpixelLocation = m_upsamplePass.shader->uniformLocation("halfpixel");
    }

    m_noisePass.shader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture,
                                                                             QStringLiteral(":/effects/blur/shaders/vertex.vert"),
                                                                             QStringLiteral(":/effects/blur/shaders/noise.frag"));
    if (!m_noisePass.shader) {
        qCWarning(KWIN_BLUR) << "Failed to load noise pass shader";
        return;
    } else {
        m_noisePass.mvpMatrixLocation = m_noisePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_noisePass.noiseTextureSizeLocation = m_noisePass.shader->uniformLocation("noiseTextureSize");
        m_noisePass.texStartPosLocation = m_noisePass.shader->uniformLocation("texStartPos");
    }

    initBlurStrengthValues();
    reconfigure(ReconfigureAll);

#if HAVE_VULKAN
skip_gl_init:
#endif

    if (effects->xcbConnection()) {
        net_wm_blur_region = effects->announceSupportProperty(s_blurAtomName, this);
    }

    connect(effects, &EffectsHandler::windowAdded, this, &BlurEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &BlurEffect::slotWindowDeleted);
    connect(effects, &EffectsHandler::propertyNotify, this, &BlurEffect::slotPropertyNotify);
    connect(effects, &EffectsHandler::xcbConnectionChanged, this, [this]() {
        net_wm_blur_region = effects->announceSupportProperty(s_blurAtomName, this);
    });

    // Fetch the blur regions for all windows
    const auto stackingOrder = effects->stackingOrder();
    for (EffectWindow *window : stackingOrder) {
        slotWindowAdded(window);
    }

    m_valid = true;
}

BlurEffect::~BlurEffect()
{
#if HAVE_VULKAN
    if (m_vulkanCtx) {
        VkDevice dev = m_vulkanCtx->backend()->device();
        if (m_vulkanDownsamplePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, m_vulkanDownsamplePipeline, nullptr);
        if (m_vulkanUpsamplePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, m_vulkanUpsamplePipeline, nullptr);
        if (m_vulkanBlurPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(dev, m_vulkanBlurPipelineLayout, nullptr);
        if (m_vulkanBlurDsLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(dev, m_vulkanBlurDsLayout, nullptr);
        if (m_vulkanBlurSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, m_vulkanBlurSampler, nullptr);
        m_vulkanBlurPass.reset();
        if (m_vulkanResumePass != VK_NULL_HANDLE)
            vkDestroyRenderPass(dev, m_vulkanResumePass, nullptr);
    }
#endif
}

void BlurEffect::initBlurStrengthValues()
{
    // This function creates an array of blur strength values that are evenly distributed

    // The range of the slider on the blur settings UI
    int numOfBlurSteps = 15;
    int remainingSteps = numOfBlurSteps;

    /*
     * Explanation for these numbers:
     *
     * The texture blur amount depends on the downsampling iterations and the offset value.
     * By changing the offset we can alter the blur amount without relying on further downsampling.
     * But there is a minimum and maximum value of offset per downsample iteration before we
     * get artifacts.
     *
     * The minOffset variable is the minimum offset value for an iteration before we
     * get blocky artifacts because of the downsampling.
     *
     * The maxOffset value is the maximum offset value for an iteration before we
     * get diagonal line artifacts because of the nature of the dual kawase blur algorithm.
     *
     * The expandSize value is the minimum value for an iteration before we reach the end
     * of a texture in the shader and sample outside of the area that was copied into the
     * texture from the screen.
     */

    // {minOffset, maxOffset, expandSize}
    blurOffsets.append({1.0, 2.0, 10}); // Down sample size / 2
    blurOffsets.append({2.0, 3.0, 20}); // Down sample size / 4
    blurOffsets.append({2.0, 5.0, 50}); // Down sample size / 8
    blurOffsets.append({3.0, 8.0, 150}); // Down sample size / 16
    // blurOffsets.append({5.0, 10.0, 400}); // Down sample size / 32
    // blurOffsets.append({7.0, ?.0});       // Down sample size / 64

    float offsetSum = 0;

    for (int i = 0; i < blurOffsets.size(); i++) {
        offsetSum += blurOffsets[i].maxOffset - blurOffsets[i].minOffset;
    }

    for (int i = 0; i < blurOffsets.size(); i++) {
        int iterationNumber = std::ceil((blurOffsets[i].maxOffset - blurOffsets[i].minOffset) / offsetSum * numOfBlurSteps);
        remainingSteps -= iterationNumber;

        if (remainingSteps < 0) {
            iterationNumber += remainingSteps;
        }

        float offsetDifference = blurOffsets[i].maxOffset - blurOffsets[i].minOffset;

        for (int j = 1; j <= iterationNumber; j++) {
            // {iteration, offset}
            blurStrengthValues.append({i + 1, blurOffsets[i].minOffset + (offsetDifference / iterationNumber) * j});
        }
    }
}

void BlurEffect::reconfigure(ReconfigureFlags flags)
{
    BlurConfig::self()->read();

    int blurStrength = BlurConfig::blurStrength() - 1;
    m_iterationCount = blurStrengthValues[blurStrength].iteration;
    m_offset = blurStrengthValues[blurStrength].offset;
    m_expandSize = blurOffsets[m_iterationCount - 1].expandSize;
    m_noiseStrength = BlurConfig::noiseStrength();

    // Update all windows for the blur to take effect
    effects->addRepaintFull();
}

void BlurEffect::updateBlurRegion(EffectWindow *w)
{
    std::optional<QRegion> content;
    std::optional<QRegion> frame;

    if (net_wm_blur_region != XCB_ATOM_NONE) {
        const QByteArray value = w->readProperty(net_wm_blur_region, XCB_ATOM_CARDINAL, 32);
        QRegion region;
        if (value.size() > 0 && !(value.size() % (4 * sizeof(uint32_t)))) {
            const uint32_t *cardinals = reinterpret_cast<const uint32_t *>(value.constData());
            for (unsigned int i = 0; i < value.size() / sizeof(uint32_t);) {
                int x = cardinals[i++];
                int y = cardinals[i++];
                int w = cardinals[i++];
                int h = cardinals[i++];
                region += Xcb::fromXNative(QRect(x, y, w, h)).toRect();
            }
        }
        if (!value.isNull()) {
            content = region;
        }
    }

    if (auto internal = w->internalWindow()) {
        const auto property = internal->property("kwin_blur");
        if (property.isValid()) {
            content = property.value<QRegion>();
        }
    }

    if (w->decorationHasAlpha() && decorationSupportsBlurBehind(w)) {
        frame = decorationBlurRegion(w);
    }

    if (content.has_value() || frame.has_value()) {
        BlurEffectData &data = m_windows[w];
        data.content = content;
        data.frame = frame;
        data.windowEffect = ItemEffect(w->windowItem());
    } else {
        if (auto it = m_windows.find(w); it != m_windows.end()) {
            effects->makeOpenGLContextCurrent();
            m_windows.erase(it);
        }
    }
}

void BlurEffect::slotWindowAdded(EffectWindow *w)
{

    if (auto internal = w->internalWindow()) {
        internal->installEventFilter(this);
    }

    setupDecorationConnections(w);
    connect(w, &EffectWindow::windowDecorationChanged, this, [this, w]() {
        setupDecorationConnections(w);
        updateBlurRegion(w);
    });

    updateBlurRegion(w);
}

void BlurEffect::slotWindowDeleted(EffectWindow *w)
{
    if (auto it = m_windows.find(w); it != m_windows.end()) {
        effects->makeOpenGLContextCurrent();
        m_windows.erase(it);
    }
}

void BlurEffect::slotPropertyNotify(EffectWindow *w, long atom)
{
    if (w && atom == net_wm_blur_region && net_wm_blur_region != XCB_ATOM_NONE) {
        updateBlurRegion(w);
    }
}

void BlurEffect::setupDecorationConnections(EffectWindow *w)
{
    if (!w->decoration()) {
        return;
    }

    connect(w->decoration(), &KDecoration3::Decoration::blurRegionChanged, this, [this, w]() {
        updateBlurRegion(w);
    });
}

bool BlurEffect::eventFilter(QObject *watched, QEvent *event)
{
    auto internal = qobject_cast<QWindow *>(watched);
    if (internal && event->type() == QEvent::DynamicPropertyChange) {
        QDynamicPropertyChangeEvent *pe = static_cast<QDynamicPropertyChangeEvent *>(event);
        if (pe->propertyName() == "kwin_blur") {
            if (auto w = effects->findWindow(internal)) {
                updateBlurRegion(w);
            }
        }
    }
    return false;
}

bool BlurEffect::enabledByDefault()
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
    GLPlatform *gl = context->glPlatform();

    if (gl->isIntel() && gl->chipClass() < SandyBridge) {
        return false;
    }
    if (gl->isPanfrost() && gl->chipClass() <= MaliT8XX) {
        return false;
    }
    // The blur effect works, but is painfully slow (FPS < 5) on Mali and VideoCore
    if (gl->isLima() || gl->isVideoCore4() || gl->isVideoCore3D()) {
        return false;
    }
    return true;
}

bool BlurEffect::supported()
{
#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        return true;
    }
#endif
    return effects->openglContext() && effects->openglContext()->supportsBlits();
}

bool BlurEffect::decorationSupportsBlurBehind(const EffectWindow *w) const
{
    return w->decoration() && !w->decoration()->blurRegion().isNull();
}

QRegion BlurEffect::decorationBlurRegion(const EffectWindow *w) const
{
    if (!decorationSupportsBlurBehind(w)) {
        return QRegion();
    }

    QRegion decorationRegion = QRegion(w->decoration()->rect().toAlignedRect()) - w->contentsRect().toRect();
    //! we return only blurred regions that belong to decoration region
    return decorationRegion.intersected(w->decoration()->blurRegion());
}

QRegion BlurEffect::blurRegion(EffectWindow *w) const
{
    QRegion region;

    if (auto it = m_windows.find(w); it != m_windows.end()) {
        const std::optional<QRegion> &content = it->second.content;
        const std::optional<QRegion> &frame = it->second.frame;
        if (content.has_value()) {
            if (content->isEmpty()) {
                // An empty region means that the blur effect should be enabled
                // for the whole window.
                region = w->contentsRect().toRect();
            } else {
                region = content->translated(w->contentsRect().topLeft().toPoint()) & w->contentsRect().toRect();
            }
            if (frame.has_value()) {
                region += frame.value();
            }
        } else if (frame.has_value()) {
            region = frame.value();
        }
    }

    return region;
}

void BlurEffect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    m_paintedArea = QRegion();
    m_currentBlur = QRegion();

    effects->prePaintScreen(data, presentTime);
}

void BlurEffect::prePaintWindow(EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime)
{
    // this effect relies on prePaintWindow being called in the bottom to top order

    effects->prePaintWindow(w, data, presentTime);

    const QRegion oldOpaque = data.opaque;
    if (data.opaque.intersects(m_currentBlur)) {
        // to blur an area partially we have to shrink the opaque area of a window
        QRegion newOpaque;
        for (const QRect &rect : data.opaque) {
            newOpaque += rect.adjusted(m_expandSize, m_expandSize, -m_expandSize, -m_expandSize);
        }
        data.opaque = newOpaque;

        // we don't have to blur a region we don't see
        m_currentBlur -= newOpaque;
    }

    // if we have to paint a non-opaque part of this window that intersects with the
    // currently blurred region we have to redraw the whole region
    if ((data.paint - oldOpaque).intersects(m_currentBlur)) {
        data.paint += m_currentBlur;
    }

    // in case this window has regions to be blurred
    const QRegion blurArea = blurRegion(w).boundingRect().translated(w->pos().toPoint());

    // if this window or a window underneath the blurred area is painted again we have to
    // blur everything
    if (m_paintedArea.intersects(blurArea) || data.paint.intersects(blurArea)) {
        data.paint += blurArea;
        // we have to check again whether we do not damage a blurred area
        // of a window
        if (blurArea.intersects(m_currentBlur)) {
            data.paint += m_currentBlur;
        }
    }

    m_currentBlur += blurArea;

    m_paintedArea -= data.opaque;
    m_paintedArea += data.paint;
}

bool BlurEffect::shouldBlur(const EffectWindow *w, int mask, const WindowPaintData &data) const
{
    if (effects->activeFullScreenEffect() && !w->data(WindowForceBlurRole).toBool()) {
        return false;
    }

    if (w->isDesktop()) {
        return false;
    }

    bool scaled = !qFuzzyCompare(data.xScale(), 1.0) && !qFuzzyCompare(data.yScale(), 1.0);
    bool translated = data.xTranslation() || data.yTranslation();

    if ((scaled || (translated || (mask & PAINT_WINDOW_TRANSFORMED))) && !w->data(WindowForceBlurRole).toBool()) {
        return false;
    }

    return true;
}

void BlurEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const QRegion &region, WindowPaintData &data)
{
    blur(renderTarget, viewport, w, mask, region, data);

    // Draw the window over the blurred area
    effects->drawWindow(renderTarget, viewport, w, mask, region, data);
}

GLTexture *BlurEffect::ensureNoiseTexture()
{
    if (m_noiseStrength == 0) {
        return nullptr;
    }

    const qreal scale = std::max(1.0, QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96.0);
    if (!m_noisePass.noiseTexture || m_noisePass.noiseTextureScale != scale || m_noisePass.noiseTextureStength != m_noiseStrength) {
        // Init randomness based on time
        std::srand((uint)QTime::currentTime().msec());

        QImage noiseImage(QSize(256, 256), QImage::Format_Grayscale8);

        for (int y = 0; y < noiseImage.height(); y++) {
            uint8_t *noiseImageLine = (uint8_t *)noiseImage.scanLine(y);

            for (int x = 0; x < noiseImage.width(); x++) {
                noiseImageLine[x] = std::rand() % m_noiseStrength;
            }
        }

        noiseImage = noiseImage.scaled(noiseImage.size() * scale);

        m_noisePass.noiseTexture = GLTexture::upload(noiseImage);
        if (!m_noisePass.noiseTexture) {
            return nullptr;
        }
        m_noisePass.noiseTexture->setFilter(GL_NEAREST);
        m_noisePass.noiseTexture->setWrapMode(GL_REPEAT);
        m_noisePass.noiseTextureScale = scale;
        m_noisePass.noiseTextureStength = m_noiseStrength;
    }

    return m_noisePass.noiseTexture.get();
}

void BlurEffect::blur(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const QRegion &region, WindowPaintData &data)
{
#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        blurVulkan(renderTarget, viewport, w, mask, region, data);
        return;
    }
#endif

    auto it = m_windows.find(w);
    if (it == m_windows.end()) {
        return;
    }

    BlurEffectData &blurInfo = it->second;
    BlurRenderData &renderInfo = blurInfo.render;
    if (!shouldBlur(w, mask, data)) {
        return;
    }

    // Compute the effective blur shape. Note that if the window is transformed, so will be the blur shape.
    QRegion blurShape = blurRegion(w).translated(w->pos().toPoint());
    if (data.xScale() != 1 || data.yScale() != 1) {
        QPoint pt = blurShape.boundingRect().topLeft();
        QRegion scaledShape;
        for (const QRect &r : blurShape) {
            const QPointF topLeft(pt.x() + (r.x() - pt.x()) * data.xScale() + data.xTranslation(),
                                  pt.y() + (r.y() - pt.y()) * data.yScale() + data.yTranslation());
            const QPoint bottomRight(std::floor(topLeft.x() + r.width() * data.xScale()) - 1,
                                     std::floor(topLeft.y() + r.height() * data.yScale()) - 1);
            scaledShape += QRect(QPoint(std::floor(topLeft.x()), std::floor(topLeft.y())), bottomRight);
        }
        blurShape = scaledShape;
    } else if (data.xTranslation() || data.yTranslation()) {
        blurShape.translate(std::round(data.xTranslation()), std::round(data.yTranslation()));
    }

    const QRect backgroundRect = blurShape.boundingRect();
    const QRect deviceBackgroundRect = snapToPixelGrid(scaledRect(backgroundRect, viewport.scale()));
    const auto opacity = w->opacity() * data.opacity();

    // Get the effective shape that will be actually blurred. It's possible that all of it will be clipped.
    QList<QRectF> effectiveShape;
    effectiveShape.reserve(blurShape.rectCount());
    if (region != infiniteRegion()) {
        for (const QRect &clipRect : region) {
            const QRectF deviceClipRect = snapToPixelGridF(scaledRect(clipRect, viewport.scale()))
                                              .translated(-deviceBackgroundRect.topLeft());
            for (const QRect &shapeRect : blurShape) {
                const QRectF deviceShapeRect = snapToPixelGridF(scaledRect(shapeRect.translated(-backgroundRect.topLeft()), viewport.scale()));
                if (const QRectF intersected = deviceClipRect.intersected(deviceShapeRect); !intersected.isEmpty()) {
                    effectiveShape.append(intersected);
                }
            }
        }
    } else {
        for (const QRect &rect : blurShape) {
            effectiveShape.append(snapToPixelGridF(scaledRect(rect.translated(-backgroundRect.topLeft()), viewport.scale())));
        }
    }
    if (effectiveShape.isEmpty()) {
        return;
    }

    // Maybe reallocate offscreen render targets. Keep in mind that the first one contains
    // original background behind the window, it's not blurred.
    GLenum textureFormat = GL_RGBA8;
    if (renderTarget.texture()) {
        textureFormat = renderTarget.texture()->internalFormat();
    }

    if (renderInfo.framebuffers.size() != (m_iterationCount + 1) || renderInfo.textures[0]->size() != backgroundRect.size() || renderInfo.textures[0]->internalFormat() != textureFormat) {
        renderInfo.framebuffers.clear();
        renderInfo.textures.clear();

        for (size_t i = 0; i <= m_iterationCount; ++i) {
            auto texture = GLTexture::allocate(textureFormat, backgroundRect.size() / (1 << i));
            if (!texture) {
                qCWarning(KWIN_BLUR) << "Failed to allocate an offscreen texture";
                return;
            }
            texture->setFilter(GL_LINEAR);
            texture->setWrapMode(GL_CLAMP_TO_EDGE);

            auto framebuffer = std::make_unique<GLFramebuffer>(texture.get());
            if (!framebuffer->valid()) {
                qCWarning(KWIN_BLUR) << "Failed to create an offscreen framebuffer";
                return;
            }
            renderInfo.textures.push_back(std::move(texture));
            renderInfo.framebuffers.push_back(std::move(framebuffer));
        }
    }

    // Fetch the pixels behind the shape that is going to be blurred.
    const QRegion dirtyRegion = region & backgroundRect;
    for (const QRect &dirtyRect : dirtyRegion) {
        renderInfo.framebuffers[0]->blitFromRenderTarget(renderTarget, viewport, dirtyRect, dirtyRect.translated(-backgroundRect.topLeft()));
    }

    // Upload the geometry: the first 6 vertices are used when downsampling and upsampling offscreen,
    // the remaining vertices are used when rendering on the screen.
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));

    const int vertexCount = effectiveShape.size() * 6;
    if (auto result = vbo->map<GLVertex2D>(6 + vertexCount)) {
        auto map = *result;

        size_t vboIndex = 0;

        // The geometry that will be blurred offscreen, in logical pixels.
        {
            const QRectF localRect = QRectF(0, 0, backgroundRect.width(), backgroundRect.height());

            const float x0 = localRect.left();
            const float y0 = localRect.top();
            const float x1 = localRect.right();
            const float y1 = localRect.bottom();

            const float u0 = x0 / backgroundRect.width();
            const float v0 = 1.0f - y0 / backgroundRect.height();
            const float u1 = x1 / backgroundRect.width();
            const float v1 = 1.0f - y1 / backgroundRect.height();

            // first triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y1),
                .texcoord = QVector2D(u0, v1),
            };

            // second triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y0),
                .texcoord = QVector2D(u1, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
        }

        // The geometry that will be painted on screen, in device pixels.
        for (const QRectF &rect : effectiveShape) {
            const float x0 = rect.left();
            const float y0 = rect.top();
            const float x1 = rect.right();
            const float y1 = rect.bottom();

            const float u0 = x0 / deviceBackgroundRect.width();
            const float v0 = 1.0f - y0 / deviceBackgroundRect.height();
            const float u1 = x1 / deviceBackgroundRect.width();
            const float v1 = 1.0f - y1 / deviceBackgroundRect.height();

            // first triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y1),
                .texcoord = QVector2D(u0, v1),
            };

            // second triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y0),
                .texcoord = QVector2D(u1, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
        }

        vbo->unmap();
    } else {
        qCWarning(KWIN_BLUR) << "Failed to map vertex buffer";
        return;
    }

    vbo->bindArrays();

    // The downsample pass of the dual Kawase algorithm: the background will be scaled down 50% every iteration.
    {
        GLShaderManager::instance()->pushShader(m_downsamplePass.shader.get());

        QMatrix4x4 projectionMatrix;
        projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

        m_downsamplePass.shader->setUniform(m_downsamplePass.mvpMatrixLocation, projectionMatrix);
        m_downsamplePass.shader->setUniform(m_downsamplePass.offsetLocation, float(m_offset));

        for (size_t i = 1; i < renderInfo.framebuffers.size(); ++i) {
            const auto &read = renderInfo.framebuffers[i - 1];
            const auto &draw = renderInfo.framebuffers[i];

            const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                      0.5 / read->colorAttachment()->height());
            m_downsamplePass.shader->setUniform(m_downsamplePass.halfpixelLocation, halfpixel);

            read->colorAttachment()->bind();

            GLFramebuffer::pushFramebuffer(draw.get());
            vbo->draw(GL_TRIANGLES, 0, 6);
        }

        GLShaderManager::instance()->popShader();
    }

    // The upsample pass of the dual Kawase algorithm: the background will be scaled up 200% every iteration.
    {
        GLShaderManager::instance()->pushShader(m_upsamplePass.shader.get());

        QMatrix4x4 projectionMatrix;
        projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

        m_upsamplePass.shader->setUniform(m_upsamplePass.mvpMatrixLocation, projectionMatrix);
        m_upsamplePass.shader->setUniform(m_upsamplePass.offsetLocation, float(m_offset));

        for (size_t i = renderInfo.framebuffers.size() - 1; i > 1; --i) {
            GLFramebuffer::popFramebuffer();
            const auto &read = renderInfo.framebuffers[i];

            const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                      0.5 / read->colorAttachment()->height());
            m_upsamplePass.shader->setUniform(m_upsamplePass.halfpixelLocation, halfpixel);

            read->colorAttachment()->bind();

            vbo->draw(GL_TRIANGLES, 0, 6);
        }

        GLShaderManager::instance()->popShader();
    }

    if (const BorderRadius cornerRadius = w->window()->borderRadius(); !cornerRadius.isNull()) {
        GLShaderManager::instance()->pushShader(m_roundedContrastPass.shader.get());

        QMatrix4x4 projectionMatrix = viewport.projectionMatrix();
        projectionMatrix.translate(deviceBackgroundRect.x(), deviceBackgroundRect.y());

        GLFramebuffer::popFramebuffer();
        const auto &read = renderInfo.framebuffers[1];

        const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                  0.5 / read->colorAttachment()->height());

        const QRectF transformedRect = QRectF{
            w->frameGeometry().x() + data.xTranslation(),
            w->frameGeometry().y() + data.yTranslation(),
            w->frameGeometry().width() * data.xScale(),
            w->frameGeometry().height() * data.yScale(),
        };
        const QRectF nativeBox = snapToPixelGridF(scaledRect(transformedRect, viewport.scale()))
                                     .translated(-deviceBackgroundRect.topLeft());
        const BorderRadius nativeCornerRadius = cornerRadius.scaled(viewport.scale()).rounded();

        m_roundedContrastPass.shader->setUniform(m_roundedContrastPass.mvpMatrixLocation, projectionMatrix);
        m_roundedContrastPass.shader->setUniform(m_roundedContrastPass.halfpixelLocation, halfpixel);
        m_roundedContrastPass.shader->setUniform(m_roundedContrastPass.offsetLocation, float(m_offset));
        m_roundedContrastPass.shader->setUniform(m_roundedContrastPass.boxLocation, QVector4D(nativeBox.x() + nativeBox.width() * 0.5, nativeBox.y() + nativeBox.height() * 0.5, nativeBox.width() * 0.5, nativeBox.height() * 0.5));
        m_roundedContrastPass.shader->setUniform(m_roundedContrastPass.cornerRadiusLocation, nativeCornerRadius.toVector());
        m_roundedContrastPass.shader->setUniform(m_roundedContrastPass.opacityLocation, opacity);

        read->colorAttachment()->bind();

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        vbo->draw(GL_TRIANGLES, 6, vertexCount);

        glDisable(GL_BLEND);

        GLShaderManager::instance()->popShader();
    } else {
        GLShaderManager::instance()->pushShader(m_contrastPass.shader.get());

        QMatrix4x4 projectionMatrix = viewport.projectionMatrix();
        projectionMatrix.translate(deviceBackgroundRect.x(), deviceBackgroundRect.y());

        GLFramebuffer::popFramebuffer();
        const auto &read = renderInfo.framebuffers[1];

        const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                  0.5 / read->colorAttachment()->height());

        m_contrastPass.shader->setUniform(m_contrastPass.mvpMatrixLocation, projectionMatrix);
        m_contrastPass.shader->setUniform(m_contrastPass.halfpixelLocation, halfpixel);
        m_contrastPass.shader->setUniform(m_contrastPass.offsetLocation, float(m_offset));

        read->colorAttachment()->bind();

        // Modulate the blurred texture with the window opacity if the window isn't opaque
        if (opacity < 1.0) {
            glEnable(GL_BLEND);
            glBlendColor(0, 0, 0, opacity * opacity);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        }

        vbo->draw(GL_TRIANGLES, 6, vertexCount);

        if (opacity < 1.0) {
            glDisable(GL_BLEND);
        }

        GLShaderManager::instance()->popShader();
    }

    if (m_noiseStrength > 0) {
        // Apply an additive noise onto the blurred image. The noise is useful to mask banding
        // artifacts, which often happens due to the smooth color transitions in the blurred image.

        glEnable(GL_BLEND);
        if (opacity < 1.0) {
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
        } else {
            glBlendFunc(GL_ONE, GL_ONE);
        }

        if (GLTexture *noiseTexture = ensureNoiseTexture()) {
            GLShaderManager::instance()->pushShader(m_noisePass.shader.get());

            QMatrix4x4 projectionMatrix = viewport.projectionMatrix();
            projectionMatrix.translate(deviceBackgroundRect.x(), deviceBackgroundRect.y());

            m_noisePass.shader->setUniform(m_noisePass.mvpMatrixLocation, projectionMatrix);
            m_noisePass.shader->setUniform(m_noisePass.noiseTextureSizeLocation, QVector2D(noiseTexture->width(), noiseTexture->height()));
            m_noisePass.shader->setUniform(m_noisePass.texStartPosLocation, QVector2D(deviceBackgroundRect.topLeft()));

            noiseTexture->bind();

            vbo->draw(GL_TRIANGLES, 6, vertexCount);

            GLShaderManager::instance()->popShader();
        }

        glDisable(GL_BLEND);
    }

    vbo->unbindArrays();
}

#if HAVE_VULKAN

bool BlurEffect::initVulkanResources()
{
    auto *scene = Compositor::self()->scene();
    if (!scene) {
        return false;
    }
    auto *renderer = static_cast<ItemRendererVulkan *>(scene->renderer());
    if (!renderer) {
        return false;
    }
    m_vulkanCtx = renderer->context();
    if (!m_vulkanCtx) {
        return false;
    }

    VkDevice device = m_vulkanCtx->backend()->device();
    VkFormat swapchainFormat = m_vulkanCtx->backend()->colorFormat();
    static constexpr VkFormat kBlurFmt = VK_FORMAT_B8G8R8A8_UNORM;

    // Linear sampler for the Kawase passes
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_vulkanBlurSampler) != VK_SUCCESS)
        return false;

    // Descriptor set layout: binding 0 = combined image sampler (immutable sampler)
    VkDescriptorSetLayoutBinding dsBinding{};
    dsBinding.binding = 0;
    dsBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsBinding.descriptorCount = 1;
    dsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dsBinding.pImmutableSamplers = &m_vulkanBlurSampler;

    VkDescriptorSetLayoutCreateInfo dsLayoutInfo{};
    dsLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutInfo.bindingCount = 1;
    dsLayoutInfo.pBindings = &dsBinding;
    if (vkCreateDescriptorSetLayout(device, &dsLayoutInfo, nullptr, &m_vulkanBlurDsLayout) != VK_SUCCESS)
        return false;

    // Pipeline layout: push constants = {vec2 halfpixel, float offset} = 12 bytes
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = 12;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_vulkanBlurDsLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_vulkanBlurPipelineLayout) != VK_SUCCESS)
        return false;

    // Intermediate blur render pass: GENERAL layout, DONT_CARE/STORE
    {
        VulkanRenderPass::Config cfg;
        cfg.colorFormat = kBlurFmt;
        cfg.colorLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        cfg.colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        cfg.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        cfg.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        m_vulkanBlurPass = VulkanRenderPass::create(m_vulkanCtx, cfg);
        if (!m_vulkanBlurPass)
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

    VkShaderModule vertMod = makeModule(s_blurVertSpv, sizeof(s_blurVertSpv));
    VkShaderModule dsMod = makeModule(s_blurDownsampleSpv, sizeof(s_blurDownsampleSpv));
    VkShaderModule usMod = makeModule(s_blurUpsampleSpv, sizeof(s_blurUpsampleSpv));

    if (!vertMod || !dsMod || !usMod) {
        if (vertMod)
            vkDestroyShaderModule(device, vertMod, nullptr);
        if (dsMod)
            vkDestroyShaderModule(device, dsMod, nullptr);
        if (usMod)
            vkDestroyShaderModule(device, usMod, nullptr);
        return false;
    }

    // Graphics pipelines (downsample and upsample share the same render pass)
    auto makePipeline = [&](VkShaderModule fragMod) -> VkPipeline {
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
        pipeInfo.layout = m_vulkanBlurPipelineLayout;
        pipeInfo.renderPass = m_vulkanBlurPass->renderPass();

        VkPipeline p = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &p);
        return p;
    };

    m_vulkanDownsamplePipeline = makePipeline(dsMod);
    m_vulkanUpsamplePipeline = makePipeline(usMod);

    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, dsMod, nullptr);
    vkDestroyShaderModule(device, usMod, nullptr);

    if (!m_vulkanDownsamplePipeline || !m_vulkanUpsamplePipeline)
        return false;

    // Swapchain resume render pass: LOAD_OP_LOAD so subsequent draw calls land on top of blur
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

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAttachment;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &m_vulkanResumePass) != VK_SUCCESS)
            return false;
    }

    return true;
}

void BlurEffect::blurVulkan(const RenderTarget &renderTarget, const RenderViewport &viewport,
                            EffectWindow *w, int mask, const QRegion &region, WindowPaintData &data)
{
    auto it = m_windows.find(w);
    if (it == m_windows.end()) {
        return;
    }
    BlurEffectData &blurInfo = it->second;
    if (!shouldBlur(w, mask, data)) {
        return;
    }

    auto *scene = Compositor::self()->scene();
    if (!scene) {
        return;
    }
    auto *renderer = static_cast<ItemRendererVulkan *>(scene->renderer());

    VkCommandBuffer cmd = renderer->currentCommandBuffer();
    VulkanFramebuffer *fb = renderer->currentFramebuffer();
    if (!cmd || !fb) {
        return;
    }

    VkImage swapchainImage = fb->colorImage();
    if (swapchainImage == VK_NULL_HANDLE) {
        return;
    }

    VkDevice device = m_vulkanCtx->backend()->device();

    // Compute blur region in device pixels
    QRegion blurShape = blurRegion(w).translated(w->pos().toPoint());
    const QRect backgroundRect = blurShape.boundingRect();
    if (backgroundRect.isEmpty()) {
        return;
    }
    const QRect deviceRect = snapToPixelGrid(scaledRect(backgroundRect, viewport.scale()));
    if (deviceRect.isEmpty()) {
        return;
    }

    // Allocate/resize intermediate texture chain
    BlurVulkanRenderData &vkData = blurInfo.vulkanRender;
    const size_t neededTextures = m_iterationCount + 1;
    const QSize tex0Size = deviceRect.size();

    if (vkData.textures.size() != neededTextures
        || vkData.capturedSize != tex0Size
        || vkData.iterationCount != m_iterationCount) {
        vkData.textures.clear();
        vkData.framebuffers.clear();
        static constexpr VkFormat kBlurFmt = VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t i = 0; i <= m_iterationCount; ++i) {
            QSize sz(std::max(1, tex0Size.width() >> i),
                     std::max(1, tex0Size.height() >> i));
            auto tex = VulkanTexture::createRenderTarget(m_vulkanCtx, sz, kBlurFmt);
            if (!tex) {
                qCWarning(KWIN_BLUR) << "Failed to allocate Vulkan blur texture";
                vkData.textures.clear();
                vkData.framebuffers.clear();
                return;
            }
            auto fbTex = VulkanFramebuffer::create(m_vulkanCtx, m_vulkanBlurPass.get(),
                                                   tex->imageView(), sz);
            if (!fbTex) {
                qCWarning(KWIN_BLUR) << "Failed to allocate Vulkan blur framebuffer";
                vkData.textures.clear();
                vkData.framebuffers.clear();
                return;
            }
            vkData.textures.push_back(std::move(tex));
            vkData.framebuffers.push_back(std::move(fbTex));
        }
        vkData.capturedSize = tex0Size;
        vkData.iterationCount = m_iterationCount;
        vkData.needsInit = true;
    }

    // Helper: image layout barrier
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

    // Helper: global memory barrier (no layout transition, used between render passes)
    auto memBarrier = [&](VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &b, 0, nullptr, 0, nullptr);
    };

    // On first use after allocation, transition textures from UNDEFINED → GENERAL
    if (vkData.needsInit) {
        for (auto &tex : vkData.textures) {
            imgBarrier(tex->image(),
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                       0, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        vkData.needsInit = false;
    }

    // End the active render pass so we can use the swapchain as a transfer source
    vkCmdEndRenderPass(cmd);

    // Swapchain: PRESENT_SRC → TRANSFER_SRC (capture)
    imgBarrier(swapchainImage,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Blit swapchain blur region → tex[0] (GENERAL is a valid blit destination)
    {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {deviceRect.x(), deviceRect.y(), 0};
        blit.srcOffsets[1] = {deviceRect.x() + deviceRect.width(), deviceRect.y() + deviceRect.height(), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {vkData.textures[0]->width(), vkData.textures[0]->height(), 1};
        vkCmdBlitImage(cmd,
                       swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       vkData.textures[0]->image(), VK_IMAGE_LAYOUT_GENERAL,
                       1, &blit, VK_FILTER_LINEAR);
    }

    // Swapchain: TRANSFER_SRC → TRANSFER_DST (for final writeback)
    imgBarrier(swapchainImage,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Make blit to tex[0] visible to fragment shaders
    memBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // Helper: bind source texture as descriptor set
    auto bindSrc = [&](VulkanTexture *src) {
        VkDescriptorSet ds = m_vulkanCtx->allocateDescriptorSet(m_vulkanBlurDsLayout);
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageView = src->imageView();
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = ds;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_vulkanBlurPipelineLayout, 0, 1, &ds, 0, nullptr);
    };

    // Helper: set full-texture viewport/scissor (with Y-flip)
    auto setViewportScissor = [&](VulkanTexture *dst) {
        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = static_cast<float>(dst->height());
        vp.width = static_cast<float>(dst->width());
        vp.height = -static_cast<float>(dst->height());
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        VkRect2D sc{};
        sc.extent = {static_cast<uint32_t>(dst->width()), static_cast<uint32_t>(dst->height())};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    };

    // Push constants: halfpixel of the source texture, plus offset
    struct KawasePc
    {
        float halfpixelX, halfpixelY, offset;
    };

    // Downsample: tex[0] → tex[1] → ... → tex[N]
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vulkanDownsamplePipeline);
    for (size_t i = 1; i <= m_iterationCount; ++i) {
        VulkanTexture *src = vkData.textures[i - 1].get();
        VulkanTexture *dst = vkData.textures[i].get();

        bindSrc(src);

        KawasePc pc{0.5f / src->width(), 0.5f / src->height(), static_cast<float>(m_offset)};
        vkCmdPushConstants(cmd, m_vulkanBlurPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_vulkanBlurPass->renderPass();
        rpBegin.framebuffer = vkData.framebuffers[i]->framebuffer();
        rpBegin.renderArea.extent = {static_cast<uint32_t>(dst->width()), static_cast<uint32_t>(dst->height())};
        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        setViewportScissor(dst);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);

        // Make this pass's output visible to the next fragment shader read
        memBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    // Upsample: tex[N] → tex[N-1] → ... → tex[0]
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vulkanUpsamplePipeline);
    for (size_t i = m_iterationCount; i > 0; --i) {
        VulkanTexture *src = vkData.textures[i].get();
        VulkanTexture *dst = vkData.textures[i - 1].get();

        bindSrc(src);

        KawasePc pc{0.5f / src->width(), 0.5f / src->height(), static_cast<float>(m_offset)};
        vkCmdPushConstants(cmd, m_vulkanBlurPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_vulkanBlurPass->renderPass();
        rpBegin.framebuffer = vkData.framebuffers[i - 1]->framebuffer();
        rpBegin.renderArea.extent = {static_cast<uint32_t>(dst->width()), static_cast<uint32_t>(dst->height())};
        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        setViewportScissor(dst);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);

        // Make this pass's output visible for the next fragment read or final blit
        memBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    // Blit blurred tex[0] → swapchain blur region (swapchain is in TRANSFER_DST)
    {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {vkData.textures[0]->width(), vkData.textures[0]->height(), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {deviceRect.x(), deviceRect.y(), 0};
        blit.dstOffsets[1] = {deviceRect.x() + deviceRect.width(), deviceRect.y() + deviceRect.height(), 1};
        vkCmdBlitImage(cmd,
                       vkData.textures[0]->image(), VK_IMAGE_LAYOUT_GENERAL,
                       swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
    }

    // Swapchain: TRANSFER_DST → COLOR_ATTACHMENT_OPTIMAL (for resumed rendering)
    imgBarrier(swapchainImage,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // Resume rendering with LOAD_OP_LOAD so subsequent drawWindow() renders on top of the blur
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_vulkanResumePass;
    rpBegin.framebuffer = fb->framebuffer();
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {static_cast<uint32_t>(fb->width()), static_cast<uint32_t>(fb->height())};
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
}

#endif // HAVE_VULKAN

bool BlurEffect::isActive() const
{
    return m_valid && !effects->isScreenLocked();
}

bool BlurEffect::blocksDirectScanout() const
{
    return false;
}

} // namespace KWin

#include "moc_blur.cpp"
