/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "kwin_export.h"
#include "utils/filedescriptor.h"

#include <QImage>
#include <QRect>
#include <QSize>
#include <QStack>
#include <QVector>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

struct VmaAllocator_T;
typedef VmaAllocator_T *VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T *VmaAllocation;

namespace KWin
{

class VulkanBackend;
class VulkanBuffer;
class VulkanFramebuffer;
class VulkanPipelineManager;
class VulkanTexture;
struct DmaBufAttributes;

/**
 * @brief Opaque handle for a non-blocking single-time submission.
 *
 * Returned by VulkanContext::submitSingleTimeCommandsAsync(). A handle stays
 * usable across frames: waitForSubmit() blocks until the GPU finishes the
 * referenced submission; fenceFor() exposes the underlying fence while the
 * submission is still in flight. Once the fence signals and the slot is
 * recycled, both calls become no-ops — this is the expected steady-state
 * for fire-and-forget submissions chained via semaphore to a later draw.
 */
struct VulkanSubmitHandle
{
    uint64_t id = 0;
    bool isValid() const
    {
        return id != 0;
    }
};

/**
 * @brief Manages Vulkan rendering context including command pools, descriptor pools, and pipelines.
 *
 * VulkanContext is analogous to EglContext in the OpenGL backend. It manages per-thread
 * Vulkan resources and provides the interface for command buffer allocation, descriptor
 * set management, and pipeline access.
 */
class KWIN_EXPORT VulkanContext
{
public:
    explicit VulkanContext(VulkanBackend *backend);
    ~VulkanContext();

    /**
     * @brief Make this context current for the calling thread.
     */
    bool makeCurrent();

    /**
     * @brief Release this context from the calling thread.
     */
    void doneCurrent();

    /**
     * @brief Check if context is valid and ready for use.
     */
    bool isValid() const;

    /**
     * @brief Get the backend this context belongs to.
     */
    VulkanBackend *backend() const;

    /**
     * @brief Get the command pool for allocating command buffers.
     */
    VkCommandPool commandPool() const;

    /**
     * @brief Get the descriptor pool for allocating descriptor sets.
     */
    VkDescriptorPool descriptorPool() const;

    /**
     * @brief Get the pipeline manager for shader/pipeline access.
     */
    VulkanPipelineManager *pipelineManager() const;

    /**
     * @brief Queue a DMA-BUF image barrier to be issued at frame start.
     *
     * Instead of immediately submitting barriers (which causes vkQueueWaitIdle per texture),
     * this batches all barriers and issues them together in flushPendingDmaBufBarriers().
     */
    void queueDmaBufBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);

    /**
     * @brief Issue all pending DMA-BUF barriers into the given command buffer.
     *
     * This should be called once per frame, after beginFrame() but before beginRenderPass().
     * Collapses all per-texture barriers into a single vkCmdPipelineBarrier call.
     */
    void flushPendingDmaBufBarriers(VkCommandBuffer commandBuffer);

    /**
     * @brief Allocate a primary command buffer from the command pool.
     */
    VkCommandBuffer allocateCommandBuffer();

    /**
     * @brief Free a command buffer back to the pool.
     */
    void freeCommandBuffer(VkCommandBuffer commandBuffer);

    /**
     * @brief Begin a single-time command buffer for immediate submission.
     *
     * Command buffers and fences are pooled per context — repeated
     * begin/submit pairs recycle prior slots once their fence signals, so the
     * hot path no longer allocates a fresh VkCommandBuffer per call.
     */
    VkCommandBuffer beginSingleTimeCommands();

    /**
     * @brief End, submit, and block until completion (compatibility shim).
     *
     * Equivalent to submitSingleTimeCommandsAsync() + waitForSubmit() and
     * preserves the historical synchronous contract callers relied on. The
     * wait is fence-scoped to just this submission rather than draining the
     * whole graphics queue (vkQueueWaitIdle), so even unmigrated callers see
     * a strict improvement when other work is in flight.
     */
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);

    /**
     * @brief Submit a single-time command buffer without waiting.
     *
     * The caller is responsible for ordering GPU consumers of any resources
     * the submission writes — typically via a semaphore attached to the next
     * submit, or by parking a waitForSubmit() at the actual point of need.
     * The pool reclaims the command buffer + fence at frame-boundary cleanup
     * once the fence signals; until then the handle stays referenceable.
     *
     * @return A handle for waitForSubmit() / fenceFor(); .isValid() is false
     *         if the submit itself failed.
     */
    VulkanSubmitHandle submitSingleTimeCommandsAsync(VkCommandBuffer commandBuffer);

    /**
     * @brief Variant of submitSingleTimeCommandsAsync that also signals
     *        @p signalSemaphore when the GPU finishes this submission.
     *
     * The semaphore must be a binary semaphore in the unsignaled state. The
     * caller is responsible for its lifetime and for ensuring exactly one
     * consumer waits on it before it's reused. Typical use: an offscreen
     * effect's render submit signals its texture-ready semaphore, then the
     * main scene's submit waits on it via @c addExternalWaitSemaphore — no
     * CPU stall, no barrier inside the render pass.
     */
    VulkanSubmitHandle submitSingleTimeCommandsAsync(VkCommandBuffer commandBuffer, VkSemaphore signalSemaphore);

    /**
     * @brief Block until the submission identified by @p handle completes.
     *
     * No-op if the submission has already completed and been reclaimed —
     * which is the normal steady state for fire-and-forget submits.
     */
    void waitForSubmit(VulkanSubmitHandle handle);

    /**
     * @brief Returns the VkFence backing an in-flight submission, or
     *        VK_NULL_HANDLE if already reclaimed. Useful for chaining via
     *        VkSubmitInfo::pWaitSemaphores's host-side equivalents or
     *        vkWaitForFences with a custom timeout.
     */
    VkFence fenceFor(VulkanSubmitHandle handle) const;

    /**
     * @brief Read a region of a texture into a QImage (blocking).
     *
     * The texture must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
     * Uses a staging buffer and vkQueueWaitIdle — intended for one-shot
     * screenshot readback, not per-frame use.
     *
     * @param texture  The texture to read from.
     * @param rect     The region to read (in texel coordinates). Empty = full texture.
     * @return         RGBA8 QImage, or a null QImage on failure.
     */
    QImage readTextureToImage(VulkanTexture *texture, const QRect &rect = QRect());

    /**
     * @brief Allocate a descriptor set from the descriptor pool.
     */
    VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout);

    /**
     * @brief Reset the descriptor pool, freeing all allocated descriptor sets.
     * Call at the start of each frame before allocating new descriptor sets.
     */
    void resetDescriptorPool();

    /**
     * @brief Import a DMA-BUF as a Vulkan texture (if supported).
     */
    std::unique_ptr<VulkanTexture> importDmaBufAsTexture(const DmaBufAttributes &attributes);

    /**
     * @brief Import a single DMA-BUF plane as a Vulkan texture.
     * @param attributes The DMA-BUF attributes
     * @param planeIndex The plane index to import (0-3)
     * @param format The Vulkan format for the plane
     * @param size The size of this plane (may differ from overall buffer size for YUV)
     */
    std::unique_ptr<VulkanTexture> importDmaBufPlaneAsTexture(const DmaBufAttributes &attributes,
                                                              int planeIndex,
                                                              VkFormat format,
                                                              const QSize &size);

    /**
     * @brief Check if DMA-BUF import is supported.
     */
    bool supportsDmaBufImport() const;

    /**
     * @brief Push a framebuffer onto the FBO stack.
     */
    void pushFramebuffer(VulkanFramebuffer *fbo);

    /**
     * @brief Pop a framebuffer from the FBO stack.
     */
    VulkanFramebuffer *popFramebuffer();

    /**
     * @brief Get the current framebuffer.
     */
    VulkanFramebuffer *currentFramebuffer();

    /**
     * @brief Get the currently active context for this thread.
     */
    static VulkanContext *currentContext();

    /**
     * @brief Convert a DRM fourcc format to the corresponding Vulkan format.
     * Returns VK_FORMAT_UNDEFINED for unknown formats.
     */
    static VkFormat drmFormatToVkFormat(uint32_t drmFormat);

    /**
     * @brief Get or create a fence for synchronization.
     */
    VkFence getOrCreateFence();

    /**
     * @brief Create an exportable fence (for sync fd export).
     * @return The fence handle, or VK_NULL_HANDLE if creation failed or not supported.
     */
    VkFence createExportableFence();

    /**
     * @brief Export a fence to a sync file descriptor.
     * @param fence The fence to export (must be created with createExportableFence).
     * @return The sync file descriptor, or invalid if export failed.
     */
    FileDescriptor exportFenceToSyncFd(VkFence fence);

    /**
     * @brief Check if external fence fd export is supported.
     */
    bool supportsExternalFenceFd() const;

    /**
     * @brief Queue a sampler for deferred destruction.
     * Samplers in use by in-flight command buffers cannot be destroyed immediately.
     * This method queues them for destruction when no longer in use.
     */
    void queueSamplerForDestruction(VkSampler sampler);

    /**
     * @brief Queue a buffer for deferred destruction.
     * Buffers in use by in-flight command buffers cannot be destroyed immediately.
     * This method queues them for destruction when no longer in use.
     */
    void queueBufferForDestruction(VkBuffer buffer, VmaAllocation allocation);

    /**
     * @brief Clean up any pending resources that are no longer in use.
     * Called at the start of each frame to safely destroy resources from previous frames.
     */
    void cleanupPendingResources();

    /**
     * @brief Queue an image view for deferred destruction.
     * Image views must be destroyed before their parent images.
     */
    void queueImageViewForDestruction(VkImageView imageView);

    /**
     * @brief Queue an image for deferred destruction.
     * Images can only be destroyed after all their views are destroyed.
     * For raw Vulkan memory (non-VMA), pass deviceMemory to have it freed after image destruction.
     */
    void queueImageForDestruction(VkImage image, VmaAllocation allocation = nullptr, VkDeviceMemory deviceMemory = VK_NULL_HANDLE);

    /**
     * @brief Queue both image view and image for deferred destruction in correct order.
     * This is the recommended way to destroy image+view pairs.
     */
    void queueImageAndViewForDestruction(VkImageView imageView, VkImage image);

private:
    bool createCommandPool();
    bool createDescriptorPool();
    void cleanup();

    // Pool entry for the single-time submission API. A slot is "free" when
    // id == 0; assigned a unique id at begin(); cleared back to 0 once the
    // GPU signals its fence and reclaimSignaledSingleTimeSlots() picks it up.
    struct SingleTimeSlot
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t id = 0;
    };
    int findSingleTimeSlotById(uint64_t id) const;
    int findSingleTimeSlotByCmd(VkCommandBuffer cmd) const;
    int acquireFreeSingleTimeSlot();
    void reclaimSignaledSingleTimeSlots();

    VulkanBackend *m_backend;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    bool m_isCleaningUp = false; // Set to true during cleanup to destroy immediately

    std::unique_ptr<VulkanPipelineManager> m_pipelineManager;

    QStack<VulkanFramebuffer *> m_framebufferStack;

    bool m_supportsDmaBufImport = false;

    // Track descriptor allocations for debugging
    // Pool size is calculated dynamically based on output count: outputs * 15000
    // With pool resetting each frame, this provides headroom for multi-monitor setups
    uint32_t m_descriptorAllocCount = 0;
    uint32_t m_descriptorPoolMaxSets = 0;
    static constexpr uint32_t DESCRIPTOR_POOL_SETS_PER_OUTPUT = 15000;

    // Deferred sampler destruction queue (samplers in use by in-flight command buffers)
    QVector<std::pair<VkSampler, VkFence>> m_pendingSamplerDestructions;

    // Deferred image destruction queue (images waiting for their views to be destroyed)
    struct PendingImageDestruction
    {
        VkImage image;
        VkFence fence;
        VmaAllocation allocation; // For VMA-managed memory - used with vmaDestroyImage
        VkDeviceMemory deviceMemory; // For raw Vulkan memory (non-VMA) - freed after image
    };
    QVector<PendingImageDestruction> m_pendingImageDestructions;

    // Deferred image view destruction queue (views must be destroyed before images)
    struct PendingImageViewDestruction
    {
        VkImageView imageView;
        VkFence fence;
        VkImage parentImage; // Image this view belongs to (for tracking)
    };
    QVector<PendingImageViewDestruction> m_pendingImageViewDestructions;

    // Deferred buffer destruction queue (buffers in use by in-flight command buffers)
    struct PendingBufferDestruction
    {
        VkBuffer buffer;
        VmaAllocation allocation;
        VkFence fence;
    };
    QVector<PendingBufferDestruction> m_pendingBufferDestructions;

    // Pending DMA-BUF image barriers (batched for single vkCmdPipelineBarrier per frame)
    struct PendingDmaBufBarrier
    {
        VkImage image;
        VkImageLayout oldLayout;
        VkImageLayout newLayout;
    };
    QVector<PendingDmaBufBarrier> m_pendingDmaBufBarriers;

    // Fence-gated command buffer pool used by the single-time submission API.
    // Slots are reused once their fence signals; size grows on contention and
    // settles at a small steady-state (typically 1–2 in flight).
    std::vector<SingleTimeSlot> m_singleTimeSlots;
    uint64_t m_nextSubmitId = 1;

    static VulkanContext *s_currentContext;
};

} // namespace KWin
