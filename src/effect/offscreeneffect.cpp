/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "effect/offscreeneffect.h"
#include "compositor.h"
#include "core/colorspace.h"
#include "core/output.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "opengl/openglcontext.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/windowitem.h"
#include "scene/workspacescene_vulkan.h"
#include "utils/common.h"

namespace KWin
{

// Base class for offscreen data
struct OffscreenData
{
    virtual ~OffscreenData() = default;
    virtual void setDirty()
    {
    }
    virtual void setVertexSnappingMode(RenderGeometry::VertexSnappingMode)
    {
    }

    virtual void paint(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, const QRegion &region,
                       const WindowPaintData &data, const WindowQuadList &quads)
    {
        Q_UNUSED(renderTarget);
        Q_UNUSED(viewport);
        Q_UNUSED(window);
        Q_UNUSED(region);
        Q_UNUSED(data);
        Q_UNUSED(quads);
    }

    virtual void maybeRender(EffectWindow *window)
    {
        Q_UNUSED(window);
    }

    RenderGeometry::VertexSnappingMode m_vertexSnappingMode = RenderGeometry::VertexSnappingMode::Round;
    bool m_isDirty = true;
    QMetaObject::Connection m_windowDamagedConnection;
    ItemEffect m_windowEffect;
};

// OpenGL-specific offscreen data
struct GLOffscreenData : public OffscreenData
{
public:
    ~GLOffscreenData() override;

    void setDirty() override;
    void setShader(GLShader *newShader);
    void setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode) override;

    void paint(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, const QRegion &region,
               const WindowPaintData &data, const WindowQuadList &quads) override;

    void maybeRender(EffectWindow *window) override;

    std::unique_ptr<GLTexture> m_texture;
    std::unique_ptr<GLFramebuffer> m_fbo;
    GLShader *m_shader = nullptr;
};

// Vulkan-specific offscreen data
struct VulkanOffscreenData : public OffscreenData
{
    ~VulkanOffscreenData() override;

    void setDirty() override;
    void setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode) override;

    void paint(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, const QRegion &region,
               const WindowPaintData &data, const WindowQuadList &quads) override;

    void maybeRender(EffectWindow *window) override;

    std::unique_ptr<VulkanFramebuffer> m_vulkanFbo;
    std::unique_ptr<VulkanRenderPass> m_vulkanRenderPass;
    std::unique_ptr<VulkanBuffer> m_vertexBuffer;
    std::unique_ptr<VulkanBuffer> m_uniformBuffer;
    VulkanContext *m_vulkanContext = nullptr;
    VulkanPipeline *m_pipeline = nullptr;

    // Binary semaphore signaled by the offscreen submit and waited on by
    // the next main-scene submit. Reused across frames: vkQueueSubmit on
    // the main scene resets it back to unsignaled on wait, so the next
    // maybeRender() can signal it again. Created lazily and destroyed in
    // the dtor. m_isDirty gates re-signaling to at most once per main-
    // scene frame for a given window, matching the binary-semaphore
    // contract of one-signal-per-one-wait.
    VkSemaphore m_renderSemaphore = VK_NULL_HANDLE;

    // Custom uniform values for effects
    float m_brightness = 1.0f;
    float m_saturation = 1.0f;

    // Color blindness correction parameters (zero cbIntensity = disabled)
    float m_cbDefectMatrix[12] = {};
    float m_cbIntensity = 0.0f;

    // Custom uniform color override (alpha < 0 means disabled).
    float m_uniformColorOverride[4] = {0.0f, 0.0f, 0.0f, -1.0f};

    void setShader(VulkanPipeline *pipeline);
    void setShader(VulkanPipeline *pipeline, float brightness, float saturation);
    void setColorBlindnessParams(const float cbMatrix[12], float cbIntensity);
    void setUniformColor(const QColor &color);
};

class OffscreenEffectPrivate
{
public:
    std::map<EffectWindow *, std::unique_ptr<OffscreenData>> windows;
    QMetaObject::Connection windowDeletedConnection;
    QMetaObject::Connection aboutToDestroyConnection;
    RenderGeometry::VertexSnappingMode vertexSnappingMode = RenderGeometry::VertexSnappingMode::Round;
};

OffscreenEffect::OffscreenEffect(QObject *parent)
    : Effect(parent)
    , d(std::make_unique<OffscreenEffectPrivate>())
{
}

OffscreenEffect::~OffscreenEffect() = default;

bool OffscreenEffect::supported()
{
    // Support both OpenGL and Vulkan for offscreen rendering
    return effects->isOpenGLCompositing() || effects->isVulkanCompositing();
}

void OffscreenEffect::redirect(EffectWindow *window)
{
    std::unique_ptr<OffscreenData> &offscreenData = d->windows[window];

    // Mark the offscreen data as dirty so it gets rendered
    if (offscreenData) {
        offscreenData->setDirty();
        return;
    }

    // Create the appropriate offscreen data based on compositing type
    if (effects->isOpenGLCompositing()) {
        offscreenData = std::make_unique<GLOffscreenData>();
    } else if (effects->isVulkanCompositing()) {
        // Create Vulkan offscreen data
        auto vulkanData = std::make_unique<VulkanOffscreenData>();

        // Get the Vulkan context from the scene
        auto *scene = static_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
        if (scene) {
            vulkanData->m_vulkanContext = scene->backend()->vulkanContext();
        }

        offscreenData = std::move(vulkanData);
    } else {
        return;
    }

    offscreenData->setVertexSnappingMode(d->vertexSnappingMode);
    offscreenData->m_windowEffect = ItemEffect(window->windowItem());
    offscreenData->m_windowDamagedConnection =
        connect(window, &EffectWindow::windowDamaged, this, &OffscreenEffect::handleWindowDamaged);

    if (d->windows.size() == 1) {
        setupConnections();
    }
}

void OffscreenEffect::unredirect(EffectWindow *window)
{
    auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        return;
    }

    // Only make OpenGL context current if we're using OpenGL compositing
    if (effects->isOpenGLCompositing()) {
        if (!OpenGlContext::currentContext()) {
            effects->openglContext()->makeCurrent();
        }
    }

    d->windows.erase(it);
    if (d->windows.empty()) {
        destroyConnections();
    }
}

void OffscreenEffect::setShader(EffectWindow *window, GLShader *shader)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        static_cast<GLOffscreenData *>(it->second.get())->setShader(shader);
    }
}

void OffscreenEffect::setPipeline(EffectWindow *window, VulkanPipeline *pipeline)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        auto *vulkanData = static_cast<VulkanOffscreenData *>(it->second.get());
        vulkanData->setShader(pipeline);
    }
}

void OffscreenEffect::setPipeline(EffectWindow *window, VulkanPipeline *pipeline, float brightness, float saturation)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        auto *vulkanData = static_cast<VulkanOffscreenData *>(it->second.get());
        vulkanData->setShader(pipeline, brightness, saturation);
    }
}

void OffscreenEffect::setColorBlindnessParams(EffectWindow *window, const float cbMatrix[12], float cbIntensity)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        auto *vulkanData = static_cast<VulkanOffscreenData *>(it->second.get());
        vulkanData->setColorBlindnessParams(cbMatrix, cbIntensity);
    }
}

void OffscreenEffect::setUniformColor(EffectWindow *window, const QColor &color)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        auto *vulkanData = static_cast<VulkanOffscreenData *>(it->second.get());
        vulkanData->setUniformColor(color);
    }
}

void OffscreenEffect::apply(EffectWindow *window, int mask, WindowPaintData &data, WindowQuadList &quads)
{
}

void GLOffscreenData::maybeRender(EffectWindow *window)
{
    const QRectF logicalGeometry = window->expandedGeometry();
    const qreal scale = window->screen()->scale();
    const QSize textureSize = (logicalGeometry.size() * scale).toSize();

    if (!m_texture || m_texture->size() != textureSize) {
        m_texture = GLTexture::allocate(GL_RGBA8, textureSize);
        if (!m_texture) {
            return;
        }
        m_texture->setFilter(GL_LINEAR);
        m_texture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_fbo = std::make_unique<GLFramebuffer>(m_texture.get());
        m_isDirty = true;
    }

    if (m_isDirty) {
        RenderTarget renderTarget(m_fbo.get());
        RenderViewport viewport(logicalGeometry, scale, renderTarget);
        GLFramebuffer::pushFramebuffer(m_fbo.get());
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);

        WindowPaintData data;
        data.setOpacity(1.0);

        const int mask = Effect::PAINT_WINDOW_TRANSFORMED | Effect::PAINT_WINDOW_TRANSLUCENT;
        effects->drawWindow(renderTarget, viewport, window, mask, infiniteRegion(), data);

        GLFramebuffer::popFramebuffer();
        m_isDirty = false;
    }
}

// Vulkan implementation
void VulkanOffscreenData::maybeRender(EffectWindow *window)
{
    if (!m_vulkanContext) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::maybeRender: No context";
        return;
    }

    const QRectF logicalGeometry = window->expandedGeometry();
    const qreal scale = window->screen()->scale();
    const QSize textureSize = (logicalGeometry.size() * scale).toSize();

    // Create or resize offscreen framebuffer
    if (!m_vulkanFbo || m_vulkanFbo->size() != textureSize) {
        // Match the swapchain's actual format so pipelines (compiled against the swapchain
        // render pass) remain Vulkan-compatible with this offscreen render pass.
        // Hardcoding SRGB breaks on drivers that select UNORM (NVIDIA, lavapipe).
        const VkFormat offscreenFormat = m_vulkanContext->backend()->colorFormat();

        // Create render pass for offscreen rendering
        m_vulkanRenderPass = VulkanRenderPass::createForOffscreen(m_vulkanContext, offscreenFormat, false);
        if (!m_vulkanRenderPass || !m_vulkanRenderPass->isValid()) {
            return;
        }

        // Create framebuffer with its own texture - we'll use the framebuffer's texture for sampling
        m_vulkanFbo = VulkanFramebuffer::createWithTexture(m_vulkanContext, m_vulkanRenderPass.get(), textureSize, offscreenFormat);
        if (!m_vulkanFbo || !m_vulkanFbo->isValid()) {
            return;
        }

        m_isDirty = true;
    }

    if (m_isDirty) {
        // Use single-time commands for offscreen rendering
        VkCommandBuffer cmd = m_vulkanContext->beginSingleTimeCommands();
        if (cmd == VK_NULL_HANDLE) {
            qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::maybeRender: Failed to begin single-time commands";
            return;
        }

        // Create Vulkan render target from our framebuffer
        std::unique_ptr<VulkanRenderTarget> vulkanRenderTarget = std::make_unique<VulkanRenderTarget>(m_vulkanFbo.get());
        // Set the command buffer so ItemRendererVulkan uses the correct one
        vulkanRenderTarget->setCommandBuffer(cmd);
        RenderTarget renderTarget(vulkanRenderTarget.get());
        RenderViewport viewport(logicalGeometry, scale, renderTarget);

        // Begin render pass
        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 0.0f;
        clearValue.color.float32[1] = 0.0f;
        clearValue.color.float32[2] = 0.0f;
        clearValue.color.float32[3] = 0.0f;

        VkRect2D renderArea = {};
        renderArea.offset = {0, 0};
        renderArea.extent = {(uint32_t)textureSize.width(), (uint32_t)textureSize.height()};
        m_vulkanRenderPass->begin(cmd, m_vulkanFbo->framebuffer(), renderArea, &clearValue, 1);

        // Set viewport and scissor for the offscreen framebuffer.
        // Use the same negative-height Y-flip convention as ItemRendererVulkan::beginFrame()
        // so the projection matrices (which assume Y=0 at top) produce correctly-oriented
        // content in the texture.  VK_KHR_maintenance1 / Vulkan 1.1 is already required.
        VkViewport vkViewport{};
        vkViewport.x = 0.0f;
        vkViewport.y = static_cast<float>(textureSize.height());
        vkViewport.width = static_cast<float>(textureSize.width());
        vkViewport.height = -static_cast<float>(textureSize.height());
        vkViewport.minDepth = 0.0f;
        vkViewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vkViewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {(uint32_t)textureSize.width(), (uint32_t)textureSize.height()};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        WindowPaintData data;
        data.setOpacity(1.0);

        const int mask = Effect::PAINT_WINDOW_TRANSFORMED | Effect::PAINT_WINDOW_TRANSLUCENT;

        // Switch the main renderer to its dedicated offscreen streaming-buffer
        // slot for the duration of the draw. The offscreen slot doesn't alias
        // any swapchain frame's in-flight buffers, so the submission below can
        // be fire-and-forget — paint() inserts a cross-submit barrier into the
        // consumer command buffer before sampling our texture.
        auto *mainScene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
        auto *mainRenderer = mainScene
            ? static_cast<ItemRendererVulkan *>(mainScene->renderer())
            : nullptr;
        if (mainRenderer) {
            mainRenderer->pushOffscreenSlot();
        }

        effects->drawWindow(renderTarget, viewport, window, mask, infiniteRegion(), data);

        if (mainRenderer) {
            mainRenderer->popOffscreenSlot();
        }

        // End render pass
        m_vulkanRenderPass->end(cmd);

        // Update the texture's tracked layout - the render pass's finalLayout
        // transitions it to SHADER_READ_ONLY_OPTIMAL on the GPU side, but we need
        // to update the CPU-side tracking
        VulkanTexture *texture = m_vulkanFbo->colorTexture();
        if (texture) {
            texture->setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // Submit asynchronously and signal m_renderSemaphore so the main
        // scene's next submit waits on it via addExternalWaitSemaphore.
        // No CPU stall, no in-render-pass barrier — pure GPU-GPU sync.
        if (m_renderSemaphore == VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(m_vulkanContext->backend()->device(), &semInfo, nullptr,
                                  &m_renderSemaphore)
                != VK_SUCCESS) {
                m_renderSemaphore = VK_NULL_HANDLE;
                // Fall back to a synchronous submit if we couldn't allocate
                // a semaphore: correct, just blocks the render thread for
                // one fence wait.
                m_vulkanContext->endSingleTimeCommands(cmd);
                m_isDirty = false;
                return;
            }
        }
        m_vulkanContext->submitSingleTimeCommandsAsync(cmd, m_renderSemaphore);
        if (mainRenderer) {
            // Wait at FRAGMENT_SHADER, the earliest stage the main scene
            // could sample the offscreen texture. The renderer clears its
            // external-wait list after every endFrame() submit, so the
            // semaphore handle is implicitly de-registered each frame and
            // re-added here when the window damages again.
            mainRenderer->addExternalWaitSemaphore(m_renderSemaphore,
                                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        m_isDirty = false;
    }
}

VulkanOffscreenData::~VulkanOffscreenData()
{
    QObject::disconnect(m_windowDamagedConnection);

    // Destroy the wait semaphore first. The main renderer's external-wait
    // list is cleared on every endFrame; if we're being destroyed mid-
    // frame, the still-registered handle would have to be revoked too —
    // but in practice OffscreenData destruction is driven by unredirect()
    // / window deletion, which happens outside the render pass.
    if (m_renderSemaphore != VK_NULL_HANDLE && m_vulkanContext) {
        vkDestroySemaphore(m_vulkanContext->backend()->device(), m_renderSemaphore, nullptr);
        m_renderSemaphore = VK_NULL_HANDLE;
    }

    // Release Vulkan resources before the context might be destroyed
    // The unique_ptrs will be destroyed automatically, but we need to
    // ensure they're cleaned up in the right order
    m_vulkanFbo.reset();
    m_vulkanRenderPass.reset();
    m_vertexBuffer.reset();
    m_uniformBuffer.reset();
}

void VulkanOffscreenData::setDirty()
{
    m_isDirty = true;
}

void VulkanOffscreenData::setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode)
{
    m_vertexSnappingMode = mode;
}

void VulkanOffscreenData::setShader(VulkanPipeline *pipeline)
{
    m_pipeline = pipeline;
}

void VulkanOffscreenData::setShader(VulkanPipeline *pipeline, float brightness, float saturation)
{
    m_pipeline = pipeline;
    m_brightness = brightness;
    m_saturation = saturation;
}

void VulkanOffscreenData::setColorBlindnessParams(const float cbMatrix[12], float cbIntensity)
{
    memcpy(m_cbDefectMatrix, cbMatrix, sizeof(m_cbDefectMatrix));
    m_cbIntensity = cbIntensity;
}

void VulkanOffscreenData::setUniformColor(const QColor &color)
{
    const float a = static_cast<float>(color.alphaF());
    // Premultiply so the standard ONE / ONE_MINUS_SRC_ALPHA blend produces solid color.
    m_uniformColorOverride[0] = static_cast<float>(color.redF()) * a;
    m_uniformColorOverride[1] = static_cast<float>(color.greenF()) * a;
    m_uniformColorOverride[2] = static_cast<float>(color.blueF()) * a;
    m_uniformColorOverride[3] = a;
}

void VulkanOffscreenData::paint(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, const QRegion &region,
                                const WindowPaintData &data, const WindowQuadList &quads)
{
    // Get the command buffer from the scene's renderer
    auto *scene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
    if (!scene) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: No scene";
        return;
    }

    auto *renderer = static_cast<ItemRendererVulkan *>(scene->renderer());

    if (!m_vulkanFbo || !m_vulkanContext) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: No framebuffer or context";
        return;
    }

    // Get the texture from the framebuffer - this is the texture we rendered to in maybeRender()
    VulkanTexture *renderTexture = m_vulkanFbo->colorTexture();
    if (!renderTexture || !renderTexture->isValid()) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: No valid render texture";
        return;
    }

    // Get the Vulkan render target from the passed render target
    auto *vulkanRenderTarget = renderTarget.vulkanTarget();
    if (!vulkanRenderTarget) {
        return;
    }

    // Use activeCommandBuffer so recursive paint flows (e.g. ZoomEffect's
    // fullscreen offscreen capture) record into the caller's render pass and
    // not the swapchain one — otherwise the recursive target ends up empty.
    VkCommandBuffer cmd = renderer->activeCommandBuffer(renderTarget);
    if (cmd == VK_NULL_HANDLE) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: No active command buffer";
        return;
    }

    // Cross-submit visibility for the offscreen texture is handled by the
    // semaphore registered with the main renderer in maybeRender() — no
    // barrier here. A barrier inside an active render pass would need a
    // subpass self-dependency to be valid Vulkan, which we don't declare.

    // Get the pipeline manager
    auto *pipelineManager = m_vulkanContext->pipelineManager();
    if (!pipelineManager) {
        return;
    }

    // Get a basic textured pipeline
    // Use traits similar to the GL version's default shader
    VulkanPipeline *pipeline = m_pipeline ? m_pipeline : pipelineManager->pipeline(VulkanShaderTrait::MapTexture | VulkanShaderTrait::Modulate | VulkanShaderTrait::AdjustSaturation | VulkanShaderTrait::TransformColorspace);
    if (!pipeline || !pipeline->isValid()) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: Failed to get valid pipeline";
        return;
    }

    // Get the framebuffer we're rendering to
    auto *targetFbo = vulkanRenderTarget->framebuffer();
    if (!targetFbo) {
        return;
    }
    // Bind the pipeline
    pipeline->bind(cmd);

    // Set up vertex buffer with quad geometry
    const double scale = viewport.scale();

    // Use RenderGeometry to generate vertices from quads
    RenderGeometry geometry;
    geometry.setVertexSnappingMode(m_vertexSnappingMode);
    for (auto &quad : quads) {
        geometry.appendWindowQuad(quad, scale);
    }
    // Post-process texture coordinates using the framebuffer's texture
    geometry.postProcessTextureCoordinates(renderTexture->matrix(VulkanCoordinateType::Normalized));

    const size_t vertexCount = geometry.size();
    const size_t bufferSize = vertexCount * sizeof(VulkanVertex2D);

    // Create or resize vertex buffer if needed
    if (!m_vertexBuffer || m_vertexBuffer->size() < bufferSize) {
        m_vertexBuffer = VulkanBuffer::createStreamingBuffer(m_vulkanContext, bufferSize);
        if (!m_vertexBuffer || !m_vertexBuffer->isValid()) {
            qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: Failed to create vertex buffer";
            return;
        }
    }

    // Map and fill vertex data from RenderGeometry
    auto mapped = m_vertexBuffer->map<VulkanVertex2D>(vertexCount);
    if (!mapped) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: Failed to map vertex buffer";
        return;
    }

    // Copy vertices from RenderGeometry to Vulkan format
    // Vertices are in window-local coordinates; the MVP matrix handles the window transform
    std::vector<GLVertex2D> glVertices(vertexCount);
    geometry.copy(glVertices);
    for (size_t i = 0; i < vertexCount; i++) {
        (*mapped)[i] = {QVector2D(glVertices[i].position.x(), glVertices[i].position.y()),
                        QVector2D(glVertices[i].texcoord.x(), glVertices[i].texcoord.y())};
    }
    m_vertexBuffer->unmap();

    // Bind vertex buffer
    VkBuffer vertexBuffers[] = {m_vertexBuffer->buffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // Set up texture image info
    std::array<VkDescriptorImageInfo, 4> imageInfos{};
    // Use the framebuffer's texture for slot 0 (this is the texture we rendered to)
    imageInfos[0].sampler = renderTexture->sampler();
    imageInfos[0].imageView = renderTexture->imageView();
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Fill remaining slots with the same texture
    for (int i = 1; i < 4; i++) {
        imageInfos[i] = imageInfos[0]; // Reuse the same texture for all slots
    }

    // Create or resize uniform buffer if needed
    const size_t uniformSize = sizeof(VulkanUniforms);
    if (!m_uniformBuffer || m_uniformBuffer->size() < uniformSize) {
        m_uniformBuffer = VulkanBuffer::createUniformBuffer(m_vulkanContext, uniformSize);
        if (!m_uniformBuffer || !m_uniformBuffer->isValid()) {
            qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: Failed to create uniform buffer";
            return;
        }
    }

    // Set up uniforms - use data from paint data directly (matching OpenGL implementation)
    VulkanUniforms uniforms{};
    const qreal rgb = data.brightness() * data.opacity();
    const qreal a = data.opacity();
    if (m_uniformColorOverride[3] >= 0.0f) {
        // Caller (e.g. systembell color flash) wants a solid color via UniformColor trait.
        memcpy(uniforms.uniformColor, m_uniformColorOverride, sizeof(uniforms.uniformColor));
    } else {
        uniforms.uniformColor[0] = rgb;
        uniforms.uniformColor[1] = rgb;
        uniforms.uniformColor[2] = rgb;
        uniforms.uniformColor[3] = a;
    }
    uniforms.opacity = data.opacity();
    uniforms.brightness = (m_brightness != 1.0f) ? m_brightness : data.brightness();
    uniforms.saturation = (m_saturation != 1.0f) ? m_saturation : data.saturation();
    uniforms.geometryBox[0] = 0;
    uniforms.geometryBox[1] = 0;
    uniforms.geometryBox[2] = window->width();
    uniforms.geometryBox[3] = window->height();

    // Initialize colorspace uniforms (matching OpenGL's setColorspaceUniforms)
    // Source is sRGB (the offscreen texture is in sRGB format)
    // Destination is the render target's color description
    const ColorDescription srcColor = ColorDescription::sRGB;
    const ColorDescription dstColor = renderTarget.colorDescription();

    // Set colorimetry transform matrix
    QMatrix4x4 colorimetryTransform = srcColor.toOther(dstColor, RenderingIntent::Perceptual);
    memcpy(uniforms.colorimetryTransform, colorimetryTransform.data(), sizeof(uniforms.colorimetryTransform));

    // Source transfer function (sRGB = gamma22, type 3)
    uniforms.sourceTransferFunction = srcColor.transferFunction().type;
    uniforms.sourceTransferParams[0] = srcColor.transferFunction().minLuminance;
    uniforms.sourceTransferParams[1] = srcColor.transferFunction().maxLuminance - srcColor.transferFunction().minLuminance;
    uniforms.sourceReferenceLuminance = srcColor.referenceLuminance();

    // Destination transfer function
    uniforms.destTransferFunction = dstColor.transferFunction().type;
    uniforms.destTransferParams[0] = dstColor.transferFunction().minLuminance;
    uniforms.destTransferParams[1] = dstColor.transferFunction().maxLuminance - dstColor.transferFunction().minLuminance;
    uniforms.destReferenceLuminance = dstColor.referenceLuminance();
    uniforms.maxDestLuminance = dstColor.maxHdrLuminance().value_or(10000.0f);

    // Tonemapping luminance
    uniforms.maxTonemappingLuminance = dstColor.referenceLuminance();

    // LMS transformation matrices for tonemapping
    QMatrix4x4 destToLMS = dstColor.containerColorimetry().toLMS();
    QMatrix4x4 lmsToDest = dstColor.containerColorimetry().fromLMS();
    memcpy(uniforms.destToLMS, destToLMS.data(), sizeof(uniforms.destToLMS));
    memcpy(uniforms.lmsToDest, lmsToDest.data(), sizeof(uniforms.lmsToDest));

    // Primary brightness for saturation adjustment
    const auto toXYZ = dstColor.containerColorimetry().toXYZ();
    uniforms.primaryBrightness[0] = toXYZ(1, 0);
    uniforms.primaryBrightness[1] = toXYZ(1, 1);
    uniforms.primaryBrightness[2] = toXYZ(1, 2);

    // Color blindness correction parameters
    if (m_cbIntensity > 0.0f) {
        memcpy(uniforms.cbDefectMatrix, m_cbDefectMatrix, sizeof(uniforms.cbDefectMatrix));
        uniforms.cbIntensity = m_cbIntensity;
    }

    m_uniformBuffer->upload(&uniforms, sizeof(VulkanUniforms), 0);

    // Set up uniform buffer info
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_uniformBuffer->buffer();
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(VulkanUniforms);

    // Bind 4 samplers + 1 UBO. dstSet is filled in by the helper.
    std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 4;
    descriptorWrites[0].pImageInfo = imageInfos.data();

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pBufferInfo = &bufferInfo;

    if (!m_vulkanContext->bindDescriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          pipeline->layout(), pipeline->descriptorSetLayout(),
                                          0, descriptorWrites.size(), descriptorWrites.data())) {
        qCWarning(KWIN_VULKAN) << "VulkanOffscreenData::paint: Failed to bind descriptors";
        return;
    }

    // Handle clipping region - same as OpenGL implementation
    // OpenGL enables GL_SCISSOR_TEST only when clipping, then disables it after
    // In Vulkan, scissor is always active, so we save the current scissor, set the clip region,
    // draw, then restore to full framebuffer (equivalent to disabling scissor test)
    const QSize targetSize = targetFbo->size();
    const bool clipping = region != infiniteRegion();

    // Save the current scissor to restore after drawing
    VkRect2D savedScissor{};
    if (clipping) {
        // Get current scissor from the renderer's state (full framebuffer)
        savedScissor.offset = {0, 0};
        savedScissor.extent = {static_cast<uint32_t>(targetSize.width()), static_cast<uint32_t>(targetSize.height())};

        // Set clipping scissor
        const QRect clipBounds = viewport.mapToRenderTarget(region).boundingRect();
        VkRect2D clipScissor{};
        clipScissor.offset = {clipBounds.x(), clipBounds.y()};
        clipScissor.extent = {static_cast<uint32_t>(clipBounds.width()), static_cast<uint32_t>(clipBounds.height())};
        vkCmdSetScissor(cmd, 0, 1, &clipScissor);
    }

    // Set push constants for MVP matrix
    // Use the viewport's projection matrix to match the renderer's coordinate system
    // This matches the OpenGL implementation in GLOffscreenData::paint()
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(std::round(window->x() * viewport.scale()), std::round(window->y() * viewport.scale()));
    mvp = mvp * data.toMatrix(viewport.scale());

    VulkanPushConstants pc;
    memcpy(pc.mvp, mvp.data(), sizeof(pc.mvp));
    // Texture matrix is identity - we already applied it via postProcessTextureCoordinates()
    memset(pc.textureMatrix, 0, sizeof(pc.textureMatrix));
    pc.textureMatrix[0] = 1.0f;
    pc.textureMatrix[5] = 1.0f;
    pc.textureMatrix[10] = 1.0f;
    pc.textureMatrix[15] = 1.0f;

    vkCmdPushConstants(cmd, pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(VulkanPushConstants), &pc);

    // Draw the geometry
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);

    // Restore scissor to full framebuffer (equivalent to glDisable(GL_SCISSOR_TEST) in OpenGL)
    if (clipping) {
        vkCmdSetScissor(cmd, 0, 1, &savedScissor);
    }
}

GLOffscreenData::~GLOffscreenData()
{
    QObject::disconnect(m_windowDamagedConnection);
}

void GLOffscreenData::setDirty()
{
    m_isDirty = true;
}

void GLOffscreenData::setShader(GLShader *newShader)
{
    m_shader = newShader;
}

void GLOffscreenData::setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode)
{
    m_vertexSnappingMode = mode;
}

void GLOffscreenData::paint(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, const QRegion &region,
                            const WindowPaintData &data, const WindowQuadList &quads)
{
    GLShader *shader = m_shader ? m_shader : GLShaderManager::instance()->shader(GLShaderTrait::MapTexture | GLShaderTrait::Modulate | GLShaderTrait::AdjustSaturation | GLShaderTrait::TransformColorspace);
    ShaderBinder binder(shader);

    const double scale = viewport.scale();

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));

    RenderGeometry geometry;
    geometry.setVertexSnappingMode(m_vertexSnappingMode);
    for (auto &quad : quads) {
        geometry.appendWindowQuad(quad, scale);
    }
    geometry.postProcessTextureCoordinates(m_texture->matrix(NormalizedCoordinates));

    const auto map = vbo->map<GLVertex2D>(geometry.size());
    if (!map) {
        return;
    }
    geometry.copy(*map);
    vbo->unmap();

    vbo->bindArrays();

    const qreal rgb = data.brightness() * data.opacity();
    const qreal a = data.opacity();

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(std::round(window->x() * scale), std::round(window->y() * scale));

    const auto toXYZ = renderTarget.colorDescription().containerColorimetry().toXYZ();
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp * data.toMatrix(scale));
    shader->setUniform(GLShader::Vec4Uniform::ModulationConstant, QVector4D(rgb, rgb, rgb, a));
    shader->setUniform(GLShader::FloatUniform::Saturation, data.saturation());
    shader->setUniform(GLShader::Vec3Uniform::PrimaryBrightness, QVector3D(toXYZ(1, 0), toXYZ(1, 1), toXYZ(1, 2)));
    shader->setUniform(GLShader::IntUniform::TextureWidth, m_texture->width());
    shader->setUniform(GLShader::IntUniform::TextureHeight, m_texture->height());
    shader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

    const bool clipping = region != infiniteRegion();
    const QRegion clipRegion = clipping ? viewport.mapToRenderTarget(region) : infiniteRegion();

    if (clipping) {
        glEnable(GL_SCISSOR_TEST);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_texture->bind();
    vbo->draw(clipRegion, GL_TRIANGLES, 0, geometry.count(), clipping);
    m_texture->unbind();

    glDisable(GL_BLEND);
    if (clipping) {
        glDisable(GL_SCISSOR_TEST);
    }
    vbo->unbindArrays();
}

void OffscreenEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, int mask, const QRegion &region, WindowPaintData &data)
{
    const auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        effects->drawWindow(renderTarget, viewport, window, mask, region, data);
        return;
    }
    OffscreenData *offscreenData = it->second.get();

    const QRectF expandedGeometry = window->expandedGeometry();
    const QRectF frameGeometry = window->frameGeometry();

    QRectF visibleRect = expandedGeometry;
    visibleRect.moveTopLeft(expandedGeometry.topLeft() - frameGeometry.topLeft());
    WindowQuad quad;
    quad[0] = WindowVertex(visibleRect.topLeft(), QPointF(0, 0));
    quad[1] = WindowVertex(visibleRect.topRight(), QPointF(1, 0));
    quad[2] = WindowVertex(visibleRect.bottomRight(), QPointF(1, 1));
    quad[3] = WindowVertex(visibleRect.bottomLeft(), QPointF(0, 1));

    WindowQuadList quads;
    quads.append(quad);
    apply(window, mask, data, quads);

    offscreenData->maybeRender(window);
    offscreenData->paint(renderTarget, viewport, window, region, data, quads);
}

void OffscreenEffect::handleWindowDamaged(EffectWindow *window)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        it->second->setDirty();
    }
}

void OffscreenEffect::handleWindowDeleted(EffectWindow *window)
{
    unredirect(window);
}

void OffscreenEffect::setupConnections()
{
    d->windowDeletedConnection =
        connect(effects, &EffectsHandler::windowDeleted, this, &OffscreenEffect::handleWindowDeleted);

    // Connect to aboutToDestroy to clean up Vulkan resources before the context is destroyed
    d->aboutToDestroyConnection =
        connect(Compositor::self(), &Compositor::aboutToDestroy, this, [this]() {
        // Clear all windows to release Vulkan resources before the context is destroyed
        d->windows.clear();
    });
}

void OffscreenEffect::destroyConnections()
{
    disconnect(d->windowDeletedConnection);
    disconnect(d->aboutToDestroyConnection);

    d->windowDeletedConnection = {};
    d->aboutToDestroyConnection = {};
}

void OffscreenEffect::setVertexSnappingMode(RenderGeometry::VertexSnappingMode mode)
{
    d->vertexSnappingMode = mode;
    for (auto &window : std::as_const(d->windows)) {
        window.second->setVertexSnappingMode(mode);
    }
}

bool OffscreenEffect::blocksDirectScanout() const
{
    return false;
}

class CrossFadeWindowData : public OffscreenData
{
public:
    virtual ~CrossFadeWindowData() = default;

    QRectF frameGeometryAtCapture;
};

class GLCrossFadeWindowData : public CrossFadeWindowData
{
public:
    void setShader(GLShader *newShader)
    {
        m_shader = newShader;
    }

    GLShader *m_shader = nullptr;
};

class VulkanCrossFadeWindowData : public CrossFadeWindowData
{
public:
    // Add Vulkan-specific functionality
    VulkanPipeline *m_pipeline = nullptr;

    void setShader(VulkanPipeline *pipeline)
    {
        m_pipeline = pipeline;
    }
};

class CrossFadeEffectPrivate
{
public:
    std::map<EffectWindow *, std::unique_ptr<CrossFadeWindowData>> windows;
    qreal progress;
};

CrossFadeEffect::CrossFadeEffect(QObject *parent)
    : Effect(parent)
    , d(std::make_unique<CrossFadeEffectPrivate>())
{
}

CrossFadeEffect::~CrossFadeEffect() = default;

void CrossFadeEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, int mask, const QRegion &region, WindowPaintData &data)
{
    const auto it = d->windows.find(window);

    // paint the new window (if applicable) underneath
    if (data.crossFadeProgress() > 0 || it == d->windows.end()) {
        Effect::drawWindow(renderTarget, viewport, window, mask, region, data);
    }

    if (it == d->windows.end()) {
        return;
    }
    CrossFadeWindowData *offscreenData = it->second.get();

    // paint old snapshot on top
    WindowPaintData previousWindowData = data;
    previousWindowData.setOpacity((1.0 - data.crossFadeProgress()) * data.opacity());

    const QRectF expandedGeometry = window->expandedGeometry();
    const QRectF frameGeometry = window->frameGeometry();

    // This is for the case of *non* live effect, when the window buffer we saved has a different size
    // compared to the size the window has now. The "old" window will be rendered scaled to the current
    // window geometry, but everything will be scaled, also the shadow if there is any, making the window
    // frame not line up anymore with window->frameGeometry()
    // to fix that, we consider how much the shadow will have scaled, and use that as margins to the
    // current frame geometry. this causes the scaled window to visually line up perfectly with frameGeometry,
    // having the scaled shadow all outside of it.
    const qreal widthRatio = offscreenData->frameGeometryAtCapture.width() / frameGeometry.width();
    const qreal heightRatio = offscreenData->frameGeometryAtCapture.height() / frameGeometry.height();

    const QMarginsF margins(
        (expandedGeometry.x() - frameGeometry.x()) / widthRatio,
        (expandedGeometry.y() - frameGeometry.y()) / heightRatio,
        (frameGeometry.right() - expandedGeometry.right()) / widthRatio,
        (frameGeometry.bottom() - expandedGeometry.bottom()) / heightRatio);

    QRectF visibleRect = QRectF(QPointF(0, 0), frameGeometry.size()) - margins;

    WindowQuad quad;
    quad[0] = WindowVertex(visibleRect.topLeft(), QPointF(0, 0));
    quad[1] = WindowVertex(visibleRect.topRight(), QPointF(1, 0));
    quad[2] = WindowVertex(visibleRect.bottomRight(), QPointF(1, 1));
    quad[3] = WindowVertex(visibleRect.bottomLeft(), QPointF(0, 1));

    WindowQuadList quads;
    quads.append(quad);
    offscreenData->paint(renderTarget, viewport, window, region, previousWindowData, quads);
}

void CrossFadeEffect::redirect(EffectWindow *window)
{
    if (d->windows.empty()) {
        connect(effects, &EffectsHandler::windowDeleted, this, &CrossFadeEffect::handleWindowDeleted);
    }

    std::unique_ptr<CrossFadeWindowData> &offscreenData = d->windows[window];
    if (offscreenData) {
        return;
    }

    // Create the appropriate offscreen data based on compositing type
    if (effects->isOpenGLCompositing()) {
        offscreenData = std::make_unique<GLCrossFadeWindowData>();
    } else if (effects->isVulkanCompositing()) {
        // Get the Vulkan context from the scene
        auto *scene = static_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
        if (!scene) {
            return;
        }
        offscreenData = std::make_unique<VulkanCrossFadeWindowData>();
    } else {
        return;
    }

    offscreenData->m_windowEffect = ItemEffect(window->windowItem());

    // Avoid including blur and contrast effects. During a normal painting cycle they
    // won't be included, but since we call effects->drawWindow() outside usual compositing
    // cycle, we have to prevent backdrop effects kicking in.
    const QVariant blurRole = window->data(WindowForceBlurRole);
    window->setData(WindowForceBlurRole, QVariant());
    const QVariant contrastRole = window->data(WindowForceBackgroundContrastRole);
    window->setData(WindowForceBackgroundContrastRole, QVariant());

    if (effects->isOpenGLCompositing()) {
        effects->makeOpenGLContextCurrent();
    }
    offscreenData->maybeRender(window);
    offscreenData->frameGeometryAtCapture = window->frameGeometry();

    window->setData(WindowForceBlurRole, blurRole);
    window->setData(WindowForceBackgroundContrastRole, contrastRole);
}

void CrossFadeEffect::unredirect(EffectWindow *window)
{
    auto it = d->windows.find(window);
    if (it == d->windows.end()) {
        return;
    }

    if (effects->isOpenGLCompositing()) {
        if (!OpenGlContext::currentContext()) {
            effects->openglContext()->makeCurrent();
        }
    }

    d->windows.erase(it);
    if (d->windows.empty()) {
        disconnect(effects, &EffectsHandler::windowDeleted, this, &CrossFadeEffect::handleWindowDeleted);
    }
}

void CrossFadeEffect::handleWindowDeleted(EffectWindow *window)
{
    unredirect(window);
}

void CrossFadeEffect::setShader(EffectWindow *window, GLShader *shader)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        // Only call setShader for GL-specific implementation
        auto *glData = dynamic_cast<GLCrossFadeWindowData *>(it->second.get());
        if (glData) {
            glData->setShader(shader);
        }
    }
}

void CrossFadeEffect::setPipeline(EffectWindow *window, VulkanPipeline *pipeline)
{
    if (const auto it = d->windows.find(window); it != d->windows.end()) {
        // Only call setShader for Vulkan-specific implementation
        auto *vulkanData = dynamic_cast<VulkanCrossFadeWindowData *>(it->second.get());
        if (vulkanData) {
            vulkanData->setShader(pipeline);
        }
    }
}

bool CrossFadeEffect::blocksDirectScanout() const
{
    return false;
}

} // namespace KWin

#include "moc_offscreeneffect.cpp"
