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
     * @brief A reference to the most recently rendered Vulkan thumbnail target,
     *        consumed zero-copy when Qt Quick's RHI is Vulkan.
     *
     * @c framebuffer is a shared_ptr so the provider can keep the underlying
     * VulkanTexture alive across resize-induced replacements — without that,
     * Qt's render node would briefly hold a QSGTexture wrapping a freed VkImage
     * and produce the wrong-sized / flickering thumbnails the previous attempt
     * regressed. @c handle identifies the GPU submission that produced the
     * current contents; consumers must wait on it before sampling.
     */
    struct VulkanFrame
    {
        std::shared_ptr<VulkanFramebuffer> framebuffer;
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
    // color texture). Recreated only when the thumbnail size changes — the
    // hot path no longer reallocates VkImage memory per frame.
    //
    // m_vkOffscreenFbo is shared_ptr so the texture provider can hold a
    // reference while Qt's render node still wraps it. On a thumbnail resize
    // we drop our reference and create a new framebuffer; the previous one
    // stays alive in the provider until updatePaintNode() swaps to the new
    // one. Without that, the QSGTexture from QSGVulkanTexture::fromNative
    // would dangle pointing at a freed VkImage and the scene graph would
    // sample garbage for a frame — the exact symptom (wrong-sized
    // wallpaper, half-containment thumbnails, flicker) that retired the
    // first zero-copy attempt.
    //
    // Two consumer paths, picked in updatePaintNode based on Qt Quick's
    // graphics API:
    //   * Vulkan RHI (Phase A): zero-copy import via QSGVulkanTexture
    //     ::fromNative — no readback, no upload, no QImage.
    //   * OpenGL RHI (fallback, e.g. KWIN_FORCE_QT_GL_RHI=1): readback
    //     into m_cachedImage and upload via createTextureFromImage.
    // The path is decided once and cached in m_qtRhiIsVulkan so the render
    // side knows whether to spend the readback or skip it.
    std::unique_ptr<VulkanRenderPass> m_vkRenderPass;
    std::shared_ptr<VulkanFramebuffer> m_vkOffscreenFbo;
    QImage m_cachedImage;
    VulkanSubmitHandle m_vkSubmitHandle;
    qreal m_vkLastDpr = 1.0;
    // Resolved once on first updateVulkan() call from
    // m_view->rendererInterface()->graphicsApi(); selects whether to spend
    // the GPU->CPU readback at render time. Stays valid for this source's
    // lifetime — Qt's graphicsApi is process-global and set once.
    enum class QtRhi : uint8_t { Unknown,
                                 Vulkan,
                                 OpenGL };
    mutable QtRhi m_qtRhi = QtRhi::Unknown;
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
