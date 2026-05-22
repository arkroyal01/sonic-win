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

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkancontext.h" // VulkanSubmitHandle
#endif

namespace KWin
{
class Window;
class GLFramebuffer;
class GLTexture;
class ThumbnailTextureProvider;
class WindowThumbnailSource;
#if HAVE_VULKAN
class VulkanRenderPass;
class VulkanFramebuffer;
class VulkanTexture;
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

#if HAVE_VULKAN
    /**
     * @brief A zero-copy reference to the most recently rendered Vulkan
     *        thumbnail.
     *
     * The texture is owned by this source and lives until the next size
     * change (or destruction). @c handle identifies the GPU submission that
     * produced the current contents — consumers must wait on it before
     * sampling, either via @c VulkanContext::waitForSubmit() or by chaining
     * a semaphore. Replaces the old QImage readback+reupload round-trip.
     */
    struct VulkanFrame
    {
        VulkanTexture *texture = nullptr;
        VulkanSubmitHandle handle;
        QSize size;
    };
    VulkanFrame acquireVulkan() const;
#endif

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
    // Persistent offscreen target: render pass + framebuffer (which owns the
    // color texture). Recreated only when the thumbnail size changes; sampled
    // zero-copy by QtQuick via QSGVulkanTexture::fromNative.
    std::unique_ptr<VulkanRenderPass> m_vkRenderPass;
    std::unique_ptr<VulkanFramebuffer> m_vkOffscreenFbo;
    // Identifies the most recent updateVulkan() submission. updatePaintNode()
    // waits on this before letting QtQuick sample the texture, so the GPU's
    // writes are visible. Stays usable across frames — a completed handle is
    // a no-op wait.
    VulkanSubmitHandle m_vkSubmitHandle;
    qreal m_vkLastDpr = 1.0;
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
