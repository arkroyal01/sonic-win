/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/renderbackend.h"

#include <QRegion>
#include <memory>
#include <vulkan/vulkan.h>

QT_BEGIN_NAMESPACE
class QVulkanInstance;
QT_END_NAMESPACE

namespace KWin
{
class Output;
class VulkanBackend;
class VulkanContext;
class VulkanSwapchain;

/**
 * @brief The VulkanBackend creates and holds the Vulkan context and is responsible for rendering.
 *
 * The VulkanBackend is an abstract base class used by the scene system to abstract away the
 * differences between various Vulkan implementations.
 *
 * A concrete implementation has to create and release the Vulkan context in a way so that the
 * scene renderer does not have to care about it.
 *
 * This class handles Vulkan instance, device, and swapchain management.
 */
class KWIN_EXPORT VulkanBackend : public RenderBackend
{
    Q_OBJECT

public:
    VulkanBackend();
    virtual ~VulkanBackend();

    virtual void init() = 0;
    CompositingType compositingType() const override final;
    bool checkGraphicsReset() override final;

    virtual bool makeCurrent() = 0;
    virtual void doneCurrent() = 0;
    virtual VulkanContext *vulkanContext() const = 0;

    /**
     * @brief Whether the creation of the Backend failed.
     *
     * The scene should test whether the Backend got constructed correctly. If this method
     * returns @c true, the scene should not try to start the rendering.
     *
     * @return bool @c true if the creation of the Backend failed, @c false otherwise.
     */
    bool isFailed() const
    {
        return m_failed;
    }

    /**
     * @brief Get the Vulkan instance handle
     */
    VkInstance instance() const
    {
        return m_instance;
    }

    /**
     * @brief Get the physical device handle
     */
    VkPhysicalDevice physicalDevice() const
    {
        return m_physicalDevice;
    }

    /**
     * @brief Get the logical device handle
     */
    VkDevice device() const
    {
        return m_device;
    }

    /**
     * @brief Get the graphics queue family index
     */
    uint32_t graphicsQueueFamily() const
    {
        return m_graphicsQueueFamily;
    }

    /**
     * @brief Get the graphics queue handle
     */
    VkQueue graphicsQueue() const
    {
        return m_graphicsQueue;
    }

    /**
     * @brief Copy a region of pixels
     */
    void copyPixels(const QRegion &region, const QSize &screenSize);

    /**
     * @brief Get the colour-attachment format chosen at swapchain init time.
     *
     * All offscreen render passes that composite onto the swapchain must use this
     * format to remain pipeline-compatible with the swapchain render pass.
     */
    VkFormat colorFormat() const
    {
        return m_colorFormat;
    }

    /**
     * @brief Check if external fence fd export is supported (VK_KHR_external_fence_fd)
     */
    bool supportsExternalFenceFd() const
    {
        return m_supportsExternalFenceFd;
    }

    /**
     * @brief Check if VK_KHR_incremental_present is supported.
     *
     * When true, present() may attach a VkPresentRegionsKHR listing the rectangles
     * that changed this frame, letting the presentation engine copy only those.
     */
    bool supportsIncrementalPresent() const
    {
        return m_supportsIncrementalPresent;
    }

    /**
     * @brief Check if VK_EXT_present_timing (with VK_KHR_present_id) is enabled.
     *
     * When true, presents can be tagged with a presentId and their real on-screen
     * timestamps retrieved via vkGetPastPresentationTimingEXT() — used to feed
     * RenderLoop accurate per-frame presentation feedback.
     */
    bool supportsPresentTiming() const
    {
        return m_supportsPresentTiming;
    }

    /** @brief vkSetSwapchainPresentTimingQueueSizeEXT (valid iff supportsPresentTiming()). */
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT setSwapchainPresentTimingQueueSizeEXT() const
    {
        return m_vkSetSwapchainPresentTimingQueueSizeEXT;
    }

    /** @brief vkGetPastPresentationTimingEXT (valid iff supportsPresentTiming()). */
    PFN_vkGetPastPresentationTimingEXT getPastPresentationTimingEXT() const
    {
        return m_vkGetPastPresentationTimingEXT;
    }

    /** @brief vkGetSwapchainTimeDomainPropertiesEXT (valid iff supportsPresentTiming()). */
    PFN_vkGetSwapchainTimeDomainPropertiesEXT getSwapchainTimeDomainPropertiesEXT() const
    {
        return m_vkGetSwapchainTimeDomainPropertiesEXT;
    }

    /**
     * @brief Whether VK_KHR_present_wait2 is enabled on the device.
     *
     * Required to wait synchronously on a specific presentId completing — the
     * primitive the async presentation-timing monitor uses to deliver real
     * past timestamps to RenderLoop without blocking the main thread.
     */
    bool supportsPresentWait2() const
    {
        return m_supportsPresentWait2;
    }

    /** @brief vkWaitForPresent2KHR (valid iff supportsPresentWait2()). */
    PFN_vkWaitForPresent2KHR waitForPresent2KHR() const
    {
        return m_vkWaitForPresent2KHR;
    }

    /**
     * @brief Whether VK_KHR_push_descriptor is enabled on the device.
     *
     * Lets per-draw descriptor state be encoded inline in the command buffer
     * via vkCmdPushDescriptorSetKHR, removing the per-draw allocate→update→
     * bind chain through the descriptor pool. Descriptor set layouts that
     * want to use this path must be created with
     * VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR.
     */
    bool supportsPushDescriptor() const
    {
        return m_supportsPushDescriptor;
    }

    /** @brief vkCmdPushDescriptorSetKHR (valid iff supportsPushDescriptor()). */
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSetKHR() const
    {
        return m_vkCmdPushDescriptorSetKHR;
    }

    /**
     * @brief The time-domain id put in VkPresentTimingInfoEXT — chosen by the
     * concrete backend from the swapchain's reported time domains.
     */
    uint64_t presentTimeDomainId() const
    {
        return m_presentTimeDomainId;
    }
    void setPresentTimeDomainId(uint64_t id)
    {
        m_presentTimeDomainId = id;
    }

    /**
     * @brief Whether present timing is enabled for use — device support plus the
     * surface actually supporting it, and not disabled via the env override. Set
     * by the concrete backend once it has verified surface support.
     */
    bool presentTimingEnabled() const
    {
        return m_presentTimingEnabled;
    }
    void setPresentTimingEnabled(bool enabled)
    {
        m_presentTimingEnabled = enabled;
    }

    /** @brief Present stages (VkPresentStageFlagsEXT) to request timing for. */
    VkPresentStageFlagsEXT presentTimingStages() const
    {
        return m_presentTimingStages;
    }
    void setPresentTimingStages(VkPresentStageFlagsEXT stages)
    {
        m_presentTimingStages = stages;
    }

    /**
     * @brief Whether the surface advertises support for absolute target
     * presentation times (VkPresentTimingSurfaceCapabilitiesEXT::
     * presentAtAbsoluteTimeSupported). Used together with the env gate to
     * decide whether to fill VkPresentTimingInfoEXT.targetTime in
     * VulkanSwapchain::present().
     */
    bool surfaceSupportsAbsolutePresentTime() const
    {
        return m_surfaceSupportsAbsolutePresentTime;
    }
    void setSurfaceSupportsAbsolutePresentTime(bool supported)
    {
        m_surfaceSupportsAbsolutePresentTime = supported;
    }

    /**
     * @brief The stage the targetTime hint refers to
     * (VkPresentTimingInfoEXT::targetTimeDomainPresentStage). Picked once
     * at surface-caps detection as the latest stage the surface supports
     * (visible > out > queue end). Zero when no target stage is available.
     */
    VkPresentStageFlagsEXT targetTimeDomainPresentStage() const
    {
        return m_targetTimeDomainPresentStage;
    }
    void setTargetTimeDomainPresentStage(VkPresentStageFlagsEXT stage)
    {
        m_targetTimeDomainPresentStage = stage;
    }

    /**
     * @brief Whether the Phase 5 targetTime hint should be filled this run.
     *
     * True iff KWIN_VULKAN_PRESENT_TARGET=1 is set AND the surface and
     * timing path support it. Read by VulkanSwapchain::present() to gate
     * the VkPresentTimingInfoEXT.targetTime field.
     */
    bool presentAtAbsoluteTimeRequested() const;

    /**
     * @brief Nanoseconds per GPU timestamp tick (from VkPhysicalDeviceLimits).
     *
     * Multiply a `vkCmdWriteTimestamp` delta by this to get nanoseconds. Zero
     * if the device does not advertise timestamps.
     */
    float timestampPeriod() const
    {
        return m_timestampPeriod;
    }

    /**
     * @brief Whether the graphics queue supports GPU timestamp queries.
     *
     * True iff timestampPeriod() > 0 and the graphics queue family's
     * timestampValidBits > 0. Used to gate the GPU render-time path; the CPU
     * RenderTimeQuery fallback runs when this is false.
     */
    bool supportsGpuTimestamps() const
    {
        return m_supportsGpuTimestamps;
    }

    /**
     * @brief Whether the GPU render-time path should be used this run.
     *
     * True iff KWIN_VULKAN_GPU_RENDER_TIME=1 is set in the environment AND
     * supportsGpuTimestamps() is true. The renderer checks this in beginFrame
     * to decide whether to write vkCmdWriteTimestamp brackets and register a
     * VulkanGpuRenderTimeQuery with the backend. When false, the backend's
     * concrete subclass installs a CpuRenderTimeQuery as the Phase 0 baseline.
     */
    bool gpuRenderTimeRequested() const;

    /**
     * @brief Lazily-created VkQueryPool with 2 * MAX_FRAMES_IN_FLIGHT timestamp
     * slots. Returns VK_NULL_HANDLE on creation failure or when the GPU
     * timing path is disabled.
     */
    VkQueryPool gpuRenderTimePool();

    /**
     * @brief Stash the in-flight VulkanGpuRenderTimeQuery the renderer
     * constructed in beginFrame, to be picked up by the backend's
     * endAndAttachRenderTimeQuery() after the renderer's endFrame finishes
     * recording the END timestamp.
     */
    void setPendingGpuRenderTimeQuery(std::unique_ptr<class VulkanGpuRenderTimeQuery> q);

    /** @brief Non-owning peek (used by the renderer in endFrame). */
    class VulkanGpuRenderTimeQuery *pendingGpuRenderTimeQuery() const;

    /** @brief Move-out the in-flight query (used by the backend at end-of-frame). */
    std::unique_ptr<class VulkanGpuRenderTimeQuery> takePendingGpuRenderTimeQuery();

    /**
     * @brief Get the vkGetFenceFdKHR function pointer (only valid if supportsExternalFenceFd() returns true)
     */
    PFN_vkGetFenceFdKHR vkGetFenceFdKHR() const
    {
        return m_vkGetFenceFdKHR;
    }

    /**
     * @brief Return a QVulkanInstance wrapping the backend's VkInstance.
     *
     * Lazily constructed on first call so apps that never instantiate Qt Quick offscreen
     * views do not pay for it. Ownership stays with the backend; the wrapper does not
     * destroy the underlying VkInstance. Returns nullptr if creation fails.
     */
    QVulkanInstance *qVulkanInstance();

protected:
    /**
     * @brief Sets the backend initialization to failed.
     *
     * This method should be called by the concrete subclass in case the initialization failed.
     * The given @p reason is logged as a warning.
     *
     * @param reason The reason why the initialization failed.
     */
    void setFailed(const QString &reason);

    /**
     * @brief Initialize the Vulkan instance
     */
    bool createInstance(const QList<const char *> &requiredExtensions);

    /**
     * @brief Select and initialize the physical device
     */
    bool selectPhysicalDevice();

    /**
     * @brief Create the logical device
     */
    bool createDevice(const QList<const char *> &requiredDeviceExtensions);

    /**
     * @brief Cleanup Vulkan resources
     */
    void cleanup();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VkFormat m_colorFormat = VK_FORMAT_B8G8R8A8_SRGB; // set by concrete backend after swapchain init

private:
    /**
     * @brief Whether the initialization failed, of course default to @c false.
     */
    bool m_failed = false;

    // Debug messenger for validation layers
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

    // External fence fd support
    bool m_supportsExternalFenceFd = false;
    PFN_vkGetFenceFdKHR m_vkGetFenceFdKHR = nullptr;

    // GPU timestamp query support (VkCmdWriteTimestamp).
    // Cached at physical-device selection so VulkanGpuRenderTimeQuery can
    // convert ticks to nanoseconds without re-querying limits per frame.
    float m_timestampPeriod = 0.0f;
    bool m_supportsGpuTimestamps = false;

    // Phase 5 absolute-target-time hint support
    // (VkPresentTimingSurfaceCapabilitiesEXT::presentAtAbsoluteTimeSupported).
    // Set by the concrete backend's surface-caps detection. The target stage
    // is the latest stage we'll ask the driver to land at targetTime.
    bool m_surfaceSupportsAbsolutePresentTime = false;
    VkPresentStageFlagsEXT m_targetTimeDomainPresentStage = 0;

    // Lazy-allocated query pool with 2 * MAX_FRAMES_IN_FLIGHT slots
    // (KWIN_VULKAN_GPU_RENDER_TIME=1). Slots 2*i and 2*i+1 are used for the
    // begin/end timestamps of the in-flight frame at slot i.
    VkQueryPool m_gpuRenderTimePool = VK_NULL_HANDLE;

    // In-flight query handed off between the renderer (which records the
    // GPU timestamps into the scene command buffer) and the concrete backend
    // (which attaches it to the OutputFrame at end-of-frame). Serialized:
    // doBeginFrame → renderer.beginFrame (set) → renderer.endFrame (record
    // end) → doEndFrame (take) → next frame.
    std::unique_ptr<class VulkanGpuRenderTimeQuery> m_pendingGpuRenderTimeQuery;

    // VK_KHR_incremental_present support (changed-region hints in vkQueuePresentKHR)
    bool m_supportsIncrementalPresent = false;

    // VK_EXT_present_timing support (real per-frame presentation timestamps)
    bool m_supportsPresentTiming = false;
    bool m_presentTimingEnabled = false;
    VkPresentStageFlagsEXT m_presentTimingStages = 0;
    uint64_t m_presentTimeDomainId = 0;
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT m_vkSetSwapchainPresentTimingQueueSizeEXT = nullptr;
    PFN_vkGetPastPresentationTimingEXT m_vkGetPastPresentationTimingEXT = nullptr;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT m_vkGetSwapchainTimeDomainPropertiesEXT = nullptr;

    // VK_KHR_present_wait2 — block on a specific presentId completing. Used by
    // the async present-timing monitor running on its own worker thread.
    bool m_supportsPresentWait2 = false;
    PFN_vkWaitForPresent2KHR m_vkWaitForPresent2KHR = nullptr;

    // VK_KHR_push_descriptor — encode descriptor writes inline in the command
    // buffer, skipping per-draw descriptor pool allocation + vkUpdateDescriptor
    // Sets. Used by the item renderer and effects on the hot path.
    bool m_supportsPushDescriptor = false;
    PFN_vkCmdPushDescriptorSetKHR m_vkCmdPushDescriptorSetKHR = nullptr;

    // QVulkanInstance wrapping m_instance; created lazily for native Qt Quick.
    std::unique_ptr<QVulkanInstance> m_qVulkanInstance;
};

} // namespace KWin
