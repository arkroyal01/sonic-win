/*
    SPDX-FileCopyrightText: 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config-kwin.h"

#include <QImage>
#include <QQuickItem>
#include <QUuid>

#include <epoxy/gl.h>

namespace KWin
{
class Window;
class GLFramebuffer;
class GLTexture;
class ThumbnailTextureProvider;
class WindowThumbnailSource;
#if HAVE_VULKAN
class VulkanFramebuffer;
class VulkanRenderPass;
#endif

class WindowThumbnailSource : public QObject
{
    Q_OBJECT

public:
    WindowThumbnailSource(QQuickWindow *view, Window *handle);
    ~WindowThumbnailSource() override;

    static std::shared_ptr<WindowThumbnailSource> getOrCreate(QQuickWindow *window, Window *handle);

    struct Frame
    {
        std::shared_ptr<GLTexture> texture;
        GLsync fence;
    };

    Frame acquire();
    QImage acquireImage() const;

Q_SIGNALS:
    void changed();

private:
    void update();
#if HAVE_VULKAN
    void updateVulkan();
#endif

    QPointer<QQuickWindow> m_view;
    QPointer<Window> m_handle;

    std::shared_ptr<GLTexture> m_offscreenTexture;
    std::unique_ptr<GLFramebuffer> m_offscreenTarget;
    GLsync m_acquireFence = 0;
    bool m_dirty = true;

#if HAVE_VULKAN
    std::unique_ptr<VulkanFramebuffer> m_vulkanFbo;
    std::unique_ptr<VulkanRenderPass> m_vulkanRenderPass;
    QImage m_cachedImage;
#endif
};

class WindowThumbnailItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QUuid wId READ wId WRITE setWId NOTIFY wIdChanged)
    Q_PROPERTY(KWin::Window *client READ client WRITE setClient NOTIFY clientChanged)

public:
    explicit WindowThumbnailItem(QQuickItem *parent = nullptr);
    ~WindowThumbnailItem() override;

    QUuid wId() const;
    void setWId(const QUuid &wId);

    Window *client() const;
    void setClient(Window *client);

    QSGTextureProvider *textureProvider() const override;
    bool isTextureProvider() const override;
    QSGNode *updatePaintNode(QSGNode *oldNode, QQuickItem::UpdatePaintNodeData *) override;

protected:
    void releaseResources() override;
    void itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &value) override;

Q_SIGNALS:
    void wIdChanged();
    void clientChanged();

private:
    QImage fallbackImage() const;
    QRectF paintedRect() const;
    void updateImplicitSize();
    void updateSource();
    void resetSource();

    QUuid m_wId;
    QPointer<Window> m_client;

    mutable ThumbnailTextureProvider *m_provider = nullptr;
    std::shared_ptr<WindowThumbnailSource> m_source;
};

} // namespace KWin
