/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <kwin_export.h>

#include <QMatrix4x4>
#include <QRegion>
#include <memory>

class QPainter;

namespace KWin
{

class ImageItem;
class Item;
class RenderTarget;
class RenderViewport;
class Scene;
class WindowPaintData;

class KWIN_EXPORT ItemRenderer
{
public:
    ItemRenderer();
    virtual ~ItemRenderer();

    virtual QPainter *painter() const;

    /**
     * Starts a frame. @p damage is the region the scene is about to repaint, in
     * scene coordinates, or infiniteRegion() for a full repaint. Backends that
     * support partial repaints (Vulkan) use it to scissor the frame and to
     * preserve undamaged pixels; backends that clip per-draw (OpenGL) ignore it.
     */
    virtual void beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRegion &damage);
    virtual void endFrame();

    virtual void renderBackground(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRegion &region) = 0;
    virtual void renderItem(const RenderTarget &renderTarget, const RenderViewport &viewport, Item *item, int mask, const QRegion &region, const WindowPaintData &data) = 0;

    /**
     * Drains any registered "fullscreen post-pass" callbacks against the current
     * frame's render target. Invoked by WorkspaceScene::paint() after the effect
     * chain has finished and the overlay item (if any) has been rendered, but
     * before endFrame() submits. Default implementation is a no-op; the Vulkan
     * backend overrides this for effects that must wrap the whole screen including
     * QuickSceneEffect-derived overlays (overview, etc.) which terminate the
     * effect chain.
     */
    virtual void runFullscreenPostPasses(const RenderTarget &renderTarget, const RenderViewport &viewport);

    virtual std::unique_ptr<ImageItem> createImageItem(Item *parent = nullptr) = 0;
};

} // namespace KWin
