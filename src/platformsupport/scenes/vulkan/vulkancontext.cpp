/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vulkancontext.h"
#include "core/graphicsbuffer.h"
#include "utils/common.h"
#include "vma_usage.h"
#include "vulkanbackend.h"
#include "vulkanbuffer.h"
#include "vulkanframebuffer.h"
#include "vulkanpipelinemanager.h"
#include "vulkantexture.h"
#include "workspace.h"

#include <QDebug>
#include <drm_fourcc.h>
#include <unistd.h>

namespace KWin
{

VkFormat VulkanContext::drmFormatToVkFormat(uint32_t drmFormat)
{
    switch (drmFormat) {
    case DRM_FORMAT_ARGB8888:
        // X11 pixmaps are sRGB encoded, use SRGB format for proper color handling
        return VK_FORMAT_B8G8R8A8_SRGB;
    case DRM_FORMAT_XRGB8888:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case DRM_FORMAT_ABGR8888:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case DRM_FORMAT_XBGR8888:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case DRM_FORMAT_RGB888:
        // Use 4-component format for better driver compatibility
        // The alpha channel will be ignored
        return VK_FORMAT_R8G8B8A8_SRGB;
    case DRM_FORMAT_BGR888:
        // Use 4-component format for better driver compatibility
        // The alpha channel will be ignored
        return VK_FORMAT_B8G8R8A8_SRGB;
    case DRM_FORMAT_RGB565:
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case DRM_FORMAT_BGR565:
        return VK_FORMAT_B5G6R5_UNORM_PACK16;
    case DRM_FORMAT_ARGB2101010:
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case DRM_FORMAT_XRGB2101010:
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case DRM_FORMAT_ABGR2101010:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DRM_FORMAT_XBGR2101010:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    // YUV plane formats (for multi-plane textures) - use UNORM for YUV
    case DRM_FORMAT_R8:
        return VK_FORMAT_R8_UNORM;
    case DRM_FORMAT_GR88:
        return VK_FORMAT_R8G8_UNORM;
    case DRM_FORMAT_RG88:
        return VK_FORMAT_R8G8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

VulkanContext *VulkanContext::s_currentContext = nullptr;

VulkanContext::VulkanContext(VulkanBackend *backend)
    : m_backend(backend)
{
    // Initialize VMA allocator
    if (!VulkanAllocator::initialize(backend)) {
        qCWarning(KWIN_VULKAN) << "Failed to initialize VMA allocator";
        return;
    }

    if (!createCommandPool()) {
        qCWarning(KWIN_VULKAN) << "Failed to create Vulkan command pool";
        return;
    }

    if (!createDescriptorPool()) {
        qCWarning(KWIN_VULKAN) << "Failed to create Vulkan descriptor pool";
        cleanup();
        return;
    }

    // Create pipeline manager
    m_pipelineManager = std::make_unique<VulkanPipelineManager>(this);

    // Check for DMA-BUF import support
    // This requires VK_EXT_external_memory_dma_buf extension
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties2(m_backend->physicalDevice(), &props2);

    // Check if external memory extensions are available
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(m_backend->physicalDevice(), nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(m_backend->physicalDevice(), nullptr, &extensionCount, extensions.data());

    for (const auto &ext : extensions) {
        if (strcmp(ext.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) {
            m_supportsDmaBufImport = true;
            break;
        }
    }

    qCDebug(KWIN_VULKAN) << "VulkanContext created, DMA-BUF import:" << m_supportsDmaBufImport;
}

VulkanContext::~VulkanContext()
{
    if (s_currentContext == this) {
        doneCurrent();
    }
    cleanup();
}

bool VulkanContext::createCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_backend->graphicsQueueFamily();

    VkResult result = vkCreateCommandPool(m_backend->device(), &poolInfo, nullptr, &m_commandPool);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create command pool:" << result;
        return false;
    }

    return true;
}

bool VulkanContext::createDescriptorPool()
{
    // Pool is only used by legacy / out-of-tree callers of allocateDescriptorSet().
    // In-tree consumers all use bindDescriptors() (push descriptors). A small budget
    // per output is enough — the pool is reset each frame in doBeginFrame().
    uint32_t outputCount = 1; // Default to 1 if workspace not available yet
    if (workspace()) {
        outputCount = static_cast<uint32_t>(workspace()->outputs().count());
    }
    m_descriptorPoolMaxSets = outputCount * DESCRIPTOR_POOL_SETS_PER_OUTPUT;

    // Per-binding budgets: up to 4 sampler descriptors per set (matches the shared
    // item-renderer layout's binding 0 size), 1 UBO per set, modest storage-buffer
    // headroom for the same hypothetical external user.
    std::array<VkDescriptorPoolSize, 3> poolSizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, m_descriptorPoolMaxSets},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_descriptorPoolMaxSets * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m_descriptorPoolMaxSets},
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = m_descriptorPoolMaxSets;

    VkResult result = vkCreateDescriptorPool(m_backend->device(), &poolInfo, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create descriptor pool:" << result;
        return false;
    }

    qCDebug(KWIN_VULKAN) << "Created descriptor pool with maxSets=" << poolInfo.maxSets << "for" << outputCount << "outputs";
    return true;
}

void VulkanContext::cleanup()
{
    VkDevice device = m_backend->device();
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // Set cleanup flag to destroy resources immediately instead of queueing
    m_isCleaningUp = true;

    vkDeviceWaitIdle(device);

    // First, destroy the pipeline manager
    // This will queue its resources for destruction
    m_pipelineManager.reset();

    // Clean up all pending resources since GPU is now idle
    // We can destroy everything without checking fences since vkDeviceWaitIdle returned

    // Clean up pending samplers
    for (auto &item : m_pendingSamplerDestructions) {
        vkDestroySampler(device, item.first, nullptr);
    }
    m_pendingSamplerDestructions.clear();

    // Clean up pending image views
    for (auto &item : m_pendingImageViewDestructions) {
        vkDestroyImageView(device, item.imageView, nullptr);
    }
    m_pendingImageViewDestructions.clear();

    // Clean up pending images (and their memory if any)
    for (auto &item : m_pendingImageDestructions) {
        if (item.allocation != nullptr && VulkanAllocator::isInitialized()) {
            // VMA-managed image
            vmaDestroyImage(VulkanAllocator::allocator(), item.image, item.allocation);
        } else {
            // Raw Vulkan image
            vkDestroyImage(device, item.image, nullptr);
            if (item.deviceMemory != VK_NULL_HANDLE) {
                vkFreeMemory(device, item.deviceMemory, nullptr);
            }
        }
    }
    m_pendingImageDestructions.clear();

    // Clean up pending buffers
    for (auto &item : m_pendingBufferDestructions) {
        vmaDestroyBuffer(VulkanAllocator::allocator(), item.buffer, item.allocation);
    }
    m_pendingBufferDestructions.clear();

    if (m_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
    }

    // Drain the single-time submission pool. vkDeviceWaitIdle above already
    // guarantees nothing is in flight, so the fences are safe to destroy and
    // the command buffers safe to free without further synchronization.
    for (auto &slot : m_singleTimeSlots) {
        if (slot.fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, slot.fence, nullptr);
        }
        if (slot.cmd != VK_NULL_HANDLE && m_commandPool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device, m_commandPool, 1, &slot.cmd);
        }
    }
    m_singleTimeSlots.clear();

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }

    // Shutdown VMA allocator - must be done before device is destroyed
    VulkanAllocator::shutdown();
}

bool VulkanContext::makeCurrent()
{
    s_currentContext = this;
    return true;
}

void VulkanContext::doneCurrent()
{
    if (s_currentContext == this) {
        s_currentContext = nullptr;
    }
}

bool VulkanContext::isValid() const
{
    return m_commandPool != VK_NULL_HANDLE && m_descriptorPool != VK_NULL_HANDLE;
}

VulkanBackend *VulkanContext::backend() const
{
    return m_backend;
}

VkCommandPool VulkanContext::commandPool() const
{
    return m_commandPool;
}

VkDescriptorPool VulkanContext::descriptorPool() const
{
    return m_descriptorPool;
}

VulkanPipelineManager *VulkanContext::pipelineManager() const
{
    return m_pipelineManager.get();
}

VkCommandBuffer VulkanContext::allocateCommandBuffer()
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    VkResult result = vkAllocateCommandBuffers(m_backend->device(), &allocInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to allocate command buffer:" << result;
        return VK_NULL_HANDLE;
    }

    return commandBuffer;
}

void VulkanContext::freeCommandBuffer(VkCommandBuffer commandBuffer)
{
    if (commandBuffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(m_backend->device(), m_commandPool, 1, &commandBuffer);
    }
}

int VulkanContext::findSingleTimeSlotById(uint64_t id) const
{
    if (id == 0) {
        return -1;
    }
    for (size_t i = 0; i < m_singleTimeSlots.size(); ++i) {
        if (m_singleTimeSlots[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int VulkanContext::findSingleTimeSlotByCmd(VkCommandBuffer cmd) const
{
    if (cmd == VK_NULL_HANDLE) {
        return -1;
    }
    for (size_t i = 0; i < m_singleTimeSlots.size(); ++i) {
        if (m_singleTimeSlots[i].cmd == cmd && m_singleTimeSlots[i].id != 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void VulkanContext::reclaimSignaledSingleTimeSlots()
{
    VkDevice device = m_backend->device();
    for (auto &slot : m_singleTimeSlots) {
        if (slot.id == 0 || slot.fence == VK_NULL_HANDLE) {
            continue;
        }
        // VK_NOT_READY = still in flight; VK_SUCCESS = signaled and reusable.
        // An error result (e.g. device lost) leaves the slot in place; the
        // next device-wide cleanup path will tear everything down.
        if (vkGetFenceStatus(device, slot.fence) == VK_SUCCESS) {
            vkResetFences(device, 1, &slot.fence);
            // Reset is cheap and the pool was created with
            // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT.
            vkResetCommandBuffer(slot.cmd, 0);
            slot.id = 0;
        }
    }
}

int VulkanContext::acquireFreeSingleTimeSlot()
{
    // Opportunistic reclaim so a finished slot from an earlier submission
    // becomes reusable on the next begin() without waiting for the next
    // frame-boundary cleanup pass.
    reclaimSignaledSingleTimeSlots();

    for (size_t i = 0; i < m_singleTimeSlots.size(); ++i) {
        if (m_singleTimeSlots[i].id == 0) {
            return static_cast<int>(i);
        }
    }

    // No free slot — grow the pool. Steady state is 1–2 slots; growth here
    // happens when an async submission is still in flight and a second one
    // is requested.
    SingleTimeSlot slot;
    slot.cmd = allocateCommandBuffer();
    if (slot.cmd == VK_NULL_HANDLE) {
        return -1;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = 0; // Unsignaled — fence is signaled by the first submit.
    if (vkCreateFence(m_backend->device(), &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS) {
        freeCommandBuffer(slot.cmd);
        return -1;
    }

    m_singleTimeSlots.push_back(slot);
    return static_cast<int>(m_singleTimeSlots.size() - 1);
}

VkCommandBuffer VulkanContext::beginSingleTimeCommands()
{
    const int idx = acquireFreeSingleTimeSlot();
    if (idx < 0) {
        return VK_NULL_HANDLE;
    }
    SingleTimeSlot &slot = m_singleTimeSlots[idx];

    // Assign the id eagerly so the slot is reserved between begin() and the
    // matching submit / end. The slot only becomes free again when the GPU
    // signals its fence post-submit.
    slot.id = m_nextSubmitId++;
    if (m_nextSubmitId == 0) {
        m_nextSubmitId = 1; // wrap past the sentinel; practically unreachable
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(slot.cmd, &beginInfo) != VK_SUCCESS) {
        // Roll the slot back to free without consuming the fence.
        slot.id = 0;
        return VK_NULL_HANDLE;
    }
    return slot.cmd;
}

VulkanSubmitHandle VulkanContext::submitSingleTimeCommandsAsync(VkCommandBuffer commandBuffer)
{
    return submitSingleTimeCommandsAsync(commandBuffer, VK_NULL_HANDLE);
}

VulkanSubmitHandle VulkanContext::submitSingleTimeCommandsAsync(VkCommandBuffer commandBuffer, VkSemaphore signalSemaphore)
{
    const int idx = findSingleTimeSlotByCmd(commandBuffer);
    if (idx < 0) {
        qCWarning(KWIN_VULKAN) << "submitSingleTimeCommandsAsync: unknown command buffer";
        return VulkanSubmitHandle{};
    }
    SingleTimeSlot &slot = m_singleTimeSlots[idx];

    if (vkEndCommandBuffer(slot.cmd) != VK_SUCCESS) {
        // The command buffer is now in an invalid state; mark the slot free
        // so it gets reset on next acquire (the fence is still unsignaled,
        // so opportunistic reclaim won't touch it — force a reset here).
        vkResetCommandBuffer(slot.cmd, 0);
        slot.id = 0;
        return VulkanSubmitHandle{};
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.cmd;
    if (signalSemaphore != VK_NULL_HANDLE) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;
    }

    const VkResult result = vkQueueSubmit(m_backend->graphicsQueue(), 1, &submitInfo, slot.fence);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "submitSingleTimeCommandsAsync: vkQueueSubmit failed:" << result;
        // Fence was never signaled — force-reset the slot.
        vkResetCommandBuffer(slot.cmd, 0);
        slot.id = 0;
        return VulkanSubmitHandle{};
    }

    return VulkanSubmitHandle{slot.id};
}

void VulkanContext::waitForSubmit(VulkanSubmitHandle handle)
{
    const int idx = findSingleTimeSlotById(handle.id);
    if (idx < 0) {
        // Either the submit failed (no slot) or already completed and was
        // reclaimed by a prior cleanup pass — either way, nothing to wait on.
        return;
    }
    VkFence fence = m_singleTimeSlots[idx].fence;
    if (fence == VK_NULL_HANDLE) {
        return;
    }
    vkWaitForFences(m_backend->device(), 1, &fence, VK_TRUE, UINT64_MAX);
    // Opportunistically reclaim now that we know it's signaled; saves a
    // round-trip through cleanupPendingResources for synchronous callers.
    reclaimSignaledSingleTimeSlots();
}

VkFence VulkanContext::fenceFor(VulkanSubmitHandle handle) const
{
    const int idx = findSingleTimeSlotById(handle.id);
    if (idx < 0) {
        return VK_NULL_HANDLE;
    }
    return m_singleTimeSlots[idx].fence;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer commandBuffer)
{
    // Compatibility shim: submit, then wait scoped to just this submission's
    // fence — no more queue-wide vkQueueWaitIdle. Unmigrated callers retain
    // the synchronous contract but stop blocking unrelated GPU work.
    const VulkanSubmitHandle handle = submitSingleTimeCommandsAsync(commandBuffer);
    if (handle.isValid()) {
        waitForSubmit(handle);
    }
}

VkDescriptorSet VulkanContext::allocateDescriptorSet(VkDescriptorSetLayout layout)
{
    // Do NOT reset pool here - would violate Vulkan spec if called during command buffer recording
    // Pool will be reset between frames when safe

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet descriptorSet;
    VkResult result = vkAllocateDescriptorSets(m_backend->device(), &allocInfo, &descriptorSet);

    // If pool is exhausted, we're out of options - cannot reset during command buffer recording
    // VK_ERROR_OUT_OF_POOL_MEMORY = -1000069000, VK_ERROR_FRAGMENTED_POOL = -1000069001
    const bool poolExhausted = (result == VK_ERROR_OUT_OF_POOL_MEMORY) || (result == VK_ERROR_FRAGMENTED_POOL) || (static_cast<int>(result) == -1000069000) || (static_cast<int>(result) == -1000069001);
    if (poolExhausted) {
        qCCritical(KWIN_VULKAN) << "Descriptor pool exhausted at" << m_descriptorAllocCount << "allocations (max:" << m_descriptorPoolMaxSets << ") - pool reset should prevent this";
        return VK_NULL_HANDLE;
    }

    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to allocate descriptor set:" << result;
        return VK_NULL_HANDLE;
    }

    m_descriptorAllocCount++;
    return descriptorSet;
}

void VulkanContext::resetDescriptorPool()
{
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkResetDescriptorPool(m_backend->device(), m_descriptorPool, 0);
        m_descriptorAllocCount = 0;
    }
}

bool VulkanContext::bindDescriptors(VkCommandBuffer cmd,
                                    VkPipelineBindPoint bindPoint,
                                    VkPipelineLayout pipelineLayout,
                                    VkDescriptorSetLayout setLayout,
                                    uint32_t setIndex,
                                    uint32_t writeCount,
                                    VkWriteDescriptorSet *writes)
{
    // Push path. Layouts created by VulkanPipeline carry
    // VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR whenever the
    // extension is enabled (see vulkanpipeline.cpp), so the descriptor writes
    // are encoded inline in the command buffer — no pool allocation, no
    // vkUpdateDescriptorSets, no per-draw set binding. dstSet in each write
    // is ignored by the push entry point so we leave it as the caller left
    // it (unset).
    if (m_backend->supportsPushDescriptor()) {
        if (auto fn = m_backend->cmdPushDescriptorSetKHR()) {
            fn(cmd, bindPoint, pipelineLayout, setIndex, writeCount, writes);
            return true;
        }
    }

    // Pool fallback (extension unavailable). The layout in this branch must
    // have been created *without* the push-descriptor flag — which it is
    // when supportsPushDescriptor() is false, so the dispatch is consistent.
    VkDescriptorSet ds = allocateDescriptorSet(setLayout);
    if (ds == VK_NULL_HANDLE) {
        return false;
    }
    for (uint32_t i = 0; i < writeCount; ++i) {
        writes[i].dstSet = ds;
    }
    vkUpdateDescriptorSets(m_backend->device(), writeCount, writes, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, bindPoint, pipelineLayout, setIndex, 1, &ds, 0, nullptr);
    return true;
}

void VulkanContext::queueDmaBufBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    m_pendingDmaBufBarriers.append({image, oldLayout, newLayout});
}

void VulkanContext::flushPendingDmaBufBarriers(VkCommandBuffer commandBuffer)
{
    if (m_pendingDmaBufBarriers.isEmpty()) {
        return;
    }

    QVector<VkImageMemoryBarrier> barriers;
    barriers.reserve(m_pendingDmaBufBarriers.size());

    for (const auto &pending : m_pendingDmaBufBarriers) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = pending.oldLayout;
        barrier.newLayout = pending.newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pending.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        // External memory acquire: no prior Vulkan access to flush
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        barriers.append(barrier);
    }

    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,
                         0, nullptr,
                         0, nullptr,
                         barriers.size(), barriers.data());

    m_pendingDmaBufBarriers.clear();
}

void VulkanContext::queueSamplerForDestruction(VkSampler sampler)
{
    if (sampler == VK_NULL_HANDLE) {
        return;
    }

    // If we're in cleanup mode, destroy immediately
    if (m_isCleaningUp) {
        vkDestroySampler(m_backend->device(), sampler, nullptr);
        return;
    }

    // Queue the sampler for destruction along with the current in-flight fence
    // The sampler will be destroyed when the fence is signaled
    VkFence fence = m_fence != VK_NULL_HANDLE ? m_fence : VK_NULL_HANDLE;
    m_pendingSamplerDestructions.append({sampler, fence});
}

void VulkanContext::cleanupPendingResources()
{
    VkDevice device = m_backend->device();

    // Reclaim async single-time submission slots whose fence has signaled.
    // Done first so command buffers belonging to drained submissions become
    // available again before any new work is recorded this frame.
    reclaimSignaledSingleTimeSlots();

    // Clean up pending samplers
    if (!m_pendingSamplerDestructions.isEmpty()) {
        for (auto it = m_pendingSamplerDestructions.begin(); it != m_pendingSamplerDestructions.end();) {
            VkSampler sampler = it->first;
            VkFence fence = it->second;

            bool canDestroy = true;
            if (fence != VK_NULL_HANDLE) {
                VkResult result = vkGetFenceStatus(device, fence);
                if (result == VK_NOT_READY) {
                    canDestroy = false;
                }
            }

            if (canDestroy) {
                vkDestroySampler(device, sampler, nullptr);
                it = m_pendingSamplerDestructions.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Clean up pending image views (must be done before images)
    if (!m_pendingImageViewDestructions.isEmpty()) {
        for (auto it = m_pendingImageViewDestructions.begin(); it != m_pendingImageViewDestructions.end();) {
            VkImageView imageView = it->imageView;
            VkFence fence = it->fence;

            bool canDestroy = true;
            if (fence != VK_NULL_HANDLE) {
                VkResult result = vkGetFenceStatus(device, fence);
                if (result == VK_NOT_READY) {
                    canDestroy = false;
                }
            }

            if (canDestroy) {
                vkDestroyImageView(device, imageView, nullptr);
                it = m_pendingImageViewDestructions.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Clean up pending images (only after their views are destroyed)
    if (!m_pendingImageDestructions.isEmpty()) {
        for (auto it = m_pendingImageDestructions.begin(); it != m_pendingImageDestructions.end();) {
            VkImage image = it->image;
            VkFence fence = it->fence;
            VmaAllocation allocation = it->allocation;
            VkDeviceMemory deviceMemory = it->deviceMemory;

            bool canDestroy = true;
            if (fence != VK_NULL_HANDLE) {
                VkResult result = vkGetFenceStatus(device, fence);
                if (result == VK_NOT_READY) {
                    canDestroy = false;
                }
            }

            if (canDestroy) {
                if (allocation != nullptr && VulkanAllocator::isInitialized()) {
                    // VMA-managed image
                    vmaDestroyImage(VulkanAllocator::allocator(), image, allocation);
                } else {
                    // Raw Vulkan image
                    vkDestroyImage(device, image, nullptr);
                    // Free raw Vulkan memory after image is destroyed
                    if (deviceMemory != VK_NULL_HANDLE) {
                        vkFreeMemory(device, deviceMemory, nullptr);
                    }
                }
                it = m_pendingImageDestructions.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Clean up pending buffers
    if (!m_pendingBufferDestructions.isEmpty()) {
        for (auto it = m_pendingBufferDestructions.begin(); it != m_pendingBufferDestructions.end();) {
            VkBuffer buffer = it->buffer;
            VmaAllocation allocation = it->allocation;
            VkFence fence = it->fence;

            bool canDestroy = true;
            if (fence != VK_NULL_HANDLE) {
                VkResult result = vkGetFenceStatus(device, fence);
                if (result == VK_NOT_READY) {
                    canDestroy = false;
                }
            }

            if (canDestroy) {
                vmaDestroyBuffer(VulkanAllocator::allocator(), buffer, allocation);
                it = m_pendingBufferDestructions.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void VulkanContext::queueImageViewForDestruction(VkImageView imageView)
{
    if (imageView == VK_NULL_HANDLE) {
        return;
    }

    // If we're in cleanup mode, destroy immediately
    if (m_isCleaningUp) {
        vkDestroyImageView(m_backend->device(), imageView, nullptr);
        return;
    }

    VkFence fence = m_fence != VK_NULL_HANDLE ? m_fence : VK_NULL_HANDLE;
    m_pendingImageViewDestructions.append({imageView, fence, VK_NULL_HANDLE});
}

void VulkanContext::queueImageForDestruction(VkImage image, VmaAllocation allocation, VkDeviceMemory deviceMemory)
{
    if (image == VK_NULL_HANDLE) {
        return;
    }

    // If we're in cleanup mode, destroy immediately
    if (m_isCleaningUp) {
        if (allocation != nullptr && VulkanAllocator::isInitialized()) {
            // VMA-managed image
            vmaDestroyImage(VulkanAllocator::allocator(), image, allocation);
        } else {
            // Raw Vulkan image
            vkDestroyImage(m_backend->device(), image, nullptr);
            if (deviceMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_backend->device(), deviceMemory, nullptr);
            }
        }
        return;
    }

    VkFence fence = m_fence != VK_NULL_HANDLE ? m_fence : VK_NULL_HANDLE;
    m_pendingImageDestructions.append({image, fence, allocation, deviceMemory});
}

void VulkanContext::queueImageAndViewForDestruction(VkImageView imageView, VkImage image)
{
    if (imageView == VK_NULL_HANDLE && image == VK_NULL_HANDLE) {
        return;
    }

    // If we're in cleanup mode, destroy immediately
    if (m_isCleaningUp) {
        VkDevice device = m_backend->device();
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        return;
    }

    VkFence fence = m_fence != VK_NULL_HANDLE ? m_fence : VK_NULL_HANDLE;

    // Queue image view first (must be destroyed before image)
    if (imageView != VK_NULL_HANDLE) {
        m_pendingImageViewDestructions.append({imageView, fence, image});
    }

    // Queue image second (will be destroyed after view)
    if (image != VK_NULL_HANDLE) {
        m_pendingImageDestructions.append({image, fence, nullptr, VK_NULL_HANDLE});
    }
}

void VulkanContext::queueBufferForDestruction(VkBuffer buffer, VmaAllocation allocation)
{
    if (buffer == VK_NULL_HANDLE) {
        return;
    }

    // If we're in cleanup mode, destroy immediately
    if (m_isCleaningUp) {
        if (VulkanAllocator::isInitialized()) {
            vmaDestroyBuffer(VulkanAllocator::allocator(), buffer, allocation);
        }
        return;
    }

    VkFence fence = m_fence != VK_NULL_HANDLE ? m_fence : VK_NULL_HANDLE;
    m_pendingBufferDestructions.append({buffer, allocation, fence});
}

std::unique_ptr<VulkanTexture> VulkanContext::importDmaBufAsTexture(const DmaBufAttributes &attributes)
{
    if (!m_supportsDmaBufImport) {
        qCWarning(KWIN_VULKAN) << "DMA-BUF import not supported";
        return nullptr;
    }

    // Convert DRM format to Vulkan format
    VkFormat vkFormat = drmFormatToVkFormat(attributes.format);
    if (vkFormat == VK_FORMAT_UNDEFINED) {
        qCWarning(KWIN_VULKAN) << "DMA-BUF import: unsupported DRM format:" << Qt::hex << attributes.format << Qt::dec;
        return nullptr;
    }

    // Create VkImage with external memory
    VkExternalMemoryImageCreateInfo externalMemoryInfo{};
    externalMemoryInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemoryInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalMemoryInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = vkFormat;
    imageInfo.extent.width = attributes.width;
    imageInfo.extent.height = attributes.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image;
    VkResult result = vkCreateImage(m_backend->device(), &imageInfo, nullptr, &image);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create image for DMA-BUF import:" << result;
        return nullptr;
    }

    // Get memory requirements using vkGetImageMemoryRequirements2 to check for dedicated allocation
    VkMemoryDedicatedRequirements dedicatedReqs{};
    dedicatedReqs.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    dedicatedReqs.pNext = nullptr;

    VkMemoryRequirements2 memReqs2{};
    memReqs2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memReqs2.pNext = &dedicatedReqs;

    VkImageMemoryRequirementsInfo2 imageMemReqInfo{};
    imageMemReqInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    imageMemReqInfo.image = image;
    imageMemReqInfo.pNext = nullptr;

    vkGetImageMemoryRequirements2(m_backend->device(), &imageMemReqInfo, &memReqs2);

    VkMemoryRequirements &memReqs = memReqs2.memoryRequirements;

    // Find appropriate memory type for DMA-BUF import
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_backend->physicalDevice(), &memProperties);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }

    // Fallback to any compatible memory type if device-local not found
    if (memoryTypeIndex == UINT32_MAX) {
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if (memReqs.memoryTypeBits & (1 << i)) {
                memoryTypeIndex = i;
                break;
            }
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        qCWarning(KWIN_VULKAN) << "Failed to find suitable memory type for DMA-BUF import";
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Import memory with VkImportMemoryFdInfoKHR
    VkImportMemoryFdInfoKHR importFdInfo{};
    importFdInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importFdInfo.fd = attributes.fd[0].get(); // Use the first plane's fd

    // Add VkMemoryDedicatedAllocateInfo if required by the driver
    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &importFdInfo;
    dedicatedInfo.image = dedicatedReqs.requiresDedicatedAllocation ? image : VK_NULL_HANDLE;
    dedicatedInfo.buffer = VK_NULL_HANDLE;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &dedicatedInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory memory;
    result = vkAllocateMemory(m_backend->device(), &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to allocate memory for DMA-BUF import:" << result;
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Bind memory to image
    result = vkBindImageMemory(m_backend->device(), image, memory, 0);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to bind memory to image:" << result;
        vkFreeMemory(m_backend->device(), memory, nullptr);
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    // XRGB DRM formats have an undefined/zero X byte in the alpha position of the DMA-BUF.
    // Swizzle alpha to ONE so the texture samples as fully opaque, matching CPU upload behaviour.
    switch (attributes.format) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_XBGR2101010:
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_ONE;
        break;
    default:
        break;
    }

    VkImageView imageView;
    result = vkCreateImageView(m_backend->device(), &viewInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create image view:" << result;
        vkFreeMemory(m_backend->device(), memory, nullptr);
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    VkSampler sampler;
    result = vkCreateSampler(m_backend->device(), &samplerInfo, nullptr, &sampler);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create sampler:" << result;
        vkDestroyImageView(m_backend->device(), imageView, nullptr);
        vkFreeMemory(m_backend->device(), memory, nullptr);
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Create VulkanTexture wrapper
    auto texture = std::unique_ptr<VulkanTexture>(new VulkanTexture(this));
    texture->m_image = image;
    texture->m_imageView = imageView;
    texture->m_sampler = sampler;
    texture->m_deviceMemory = memory; // Store device memory for cleanup
    texture->m_format = imageInfo.format;
    texture->m_size = QSize(attributes.width, attributes.height);
    texture->m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    texture->m_ownsImage = true;

    // Transition image layout from UNDEFINED to SHADER_READ_ONLY_OPTIMAL
    VkCommandBuffer cmd = allocateCommandBuffer();
    if (cmd != VK_NULL_HANDLE) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            // External memory acquire: no prior Vulkan access to flush
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0,
                                 0, nullptr,
                                 0, nullptr,
                                 1, &barrier);

            vkEndCommandBuffer(cmd);

            // Submit and wait for completion
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &cmd;

            VkFence fence = getOrCreateFence();
            vkResetFences(m_backend->device(), 1, &fence);
            vkQueueSubmit(m_backend->graphicsQueue(), 1, &submitInfo, fence);
            vkWaitForFences(m_backend->device(), 1, &fence, VK_TRUE, UINT64_MAX);
        }
        vkFreeCommandBuffers(m_backend->device(), m_commandPool, 1, &cmd);
    }

    texture->m_currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    return texture;
}

std::unique_ptr<VulkanTexture> VulkanContext::importDmaBufPlaneAsTexture(const DmaBufAttributes &attributes,
                                                                         int planeIndex,
                                                                         VkFormat format,
                                                                         const QSize &size)
{
    if (!m_supportsDmaBufImport) {
        qCWarning(KWIN_VULKAN) << "DMA-BUF import not supported";
        return nullptr;
    }

    if (planeIndex < 0 || planeIndex >= attributes.planeCount || planeIndex >= 4) {
        qCWarning(KWIN_VULKAN) << "Invalid plane index:" << planeIndex << "planeCount:" << attributes.planeCount;
        return nullptr;
    }

    if (!attributes.fd[planeIndex].isValid()) {
        qCWarning(KWIN_VULKAN) << "Invalid file descriptor for plane" << planeIndex;
        return nullptr;
    }

    // Create VkImage with external memory
    VkExternalMemoryImageCreateInfo externalMemoryInfo{};
    externalMemoryInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemoryInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalMemoryInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = static_cast<uint32_t>(size.width());
    imageInfo.extent.height = static_cast<uint32_t>(size.height());
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image;
    VkResult result = vkCreateImage(m_backend->device(), &imageInfo, nullptr, &image);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create image for DMA-BUF plane" << planeIndex << "import:" << result;
        return nullptr;
    }

    // Get memory requirements using vkGetImageMemoryRequirements2 to check for dedicated allocation
    VkMemoryDedicatedRequirements dedicatedReqs{};
    dedicatedReqs.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    dedicatedReqs.pNext = nullptr;

    VkMemoryRequirements2 memReqs2{};
    memReqs2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memReqs2.pNext = &dedicatedReqs;

    VkImageMemoryRequirementsInfo2 imageMemReqInfo{};
    imageMemReqInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    imageMemReqInfo.image = image;
    imageMemReqInfo.pNext = nullptr;

    vkGetImageMemoryRequirements2(m_backend->device(), &imageMemReqInfo, &memReqs2);

    VkMemoryRequirements &memReqs = memReqs2.memoryRequirements;

    // Find appropriate memory type for DMA-BUF import
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_backend->physicalDevice(), &memProperties);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }

    // Fallback to any compatible memory type if device-local not found
    if (memoryTypeIndex == UINT32_MAX) {
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if (memReqs.memoryTypeBits & (1 << i)) {
                memoryTypeIndex = i;
                break;
            }
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        qCWarning(KWIN_VULKAN) << "Failed to find suitable memory type for DMA-BUF plane import";
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Duplicate the fd since Vulkan takes ownership
    int fd = dup(attributes.fd[planeIndex].get());
    if (fd < 0) {
        qCWarning(KWIN_VULKAN) << "Failed to duplicate fd for plane" << planeIndex;
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Import memory with VkImportMemoryFdInfoKHR
    VkImportMemoryFdInfoKHR importFdInfo{};
    importFdInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importFdInfo.fd = fd;

    // Add VkMemoryDedicatedAllocateInfo if required by the driver
    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &importFdInfo;
    dedicatedInfo.image = dedicatedReqs.requiresDedicatedAllocation ? image : VK_NULL_HANDLE;
    dedicatedInfo.buffer = VK_NULL_HANDLE;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &dedicatedInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory memory;
    result = vkAllocateMemory(m_backend->device(), &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to allocate memory for DMA-BUF plane" << planeIndex << "import:" << result;
        // fd is consumed by vkAllocateMemory on failure, no need to close
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Bind memory to image
    result = vkBindImageMemory(m_backend->device(), image, memory, 0);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to bind memory to image for plane" << planeIndex << ":" << result;
        vkFreeMemory(m_backend->device(), memory, nullptr);
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    result = vkCreateImageView(m_backend->device(), &viewInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create image view for plane" << planeIndex << ":" << result;
        vkFreeMemory(m_backend->device(), memory, nullptr);
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    VkSampler sampler;
    result = vkCreateSampler(m_backend->device(), &samplerInfo, nullptr, &sampler);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create sampler for plane" << planeIndex << ":" << result;
        vkDestroyImageView(m_backend->device(), imageView, nullptr);
        vkFreeMemory(m_backend->device(), memory, nullptr);
        vkDestroyImage(m_backend->device(), image, nullptr);
        return nullptr;
    }

    // Create VulkanTexture wrapper
    auto texture = std::unique_ptr<VulkanTexture>(new VulkanTexture(this));
    texture->m_image = image;
    texture->m_imageView = imageView;
    texture->m_sampler = sampler;
    texture->m_deviceMemory = memory;
    texture->m_format = imageInfo.format;
    texture->m_size = size;
    texture->m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    texture->m_ownsImage = true;

    // Transition image layout from UNDEFINED to SHADER_READ_ONLY_OPTIMAL
    VkCommandBuffer cmd = allocateCommandBuffer();
    if (cmd != VK_NULL_HANDLE) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0,
                                 0, nullptr,
                                 0, nullptr,
                                 1, &barrier);

            vkEndCommandBuffer(cmd);

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &cmd;

            VkFence fence = getOrCreateFence();
            vkResetFences(m_backend->device(), 1, &fence);
            vkQueueSubmit(m_backend->graphicsQueue(), 1, &submitInfo, fence);
            vkWaitForFences(m_backend->device(), 1, &fence, VK_TRUE, UINT64_MAX);
        }
        vkFreeCommandBuffers(m_backend->device(), m_commandPool, 1, &cmd);
    }

    texture->m_currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    qCDebug(KWIN_VULKAN) << "Successfully imported DMA-BUF plane" << planeIndex << "as Vulkan texture size" << size;

    return texture;
}

bool VulkanContext::supportsDmaBufImport() const
{
    return m_supportsDmaBufImport;
}

void VulkanContext::pushFramebuffer(VulkanFramebuffer *fbo)
{
    m_framebufferStack.push(fbo);
}

VulkanFramebuffer *VulkanContext::popFramebuffer()
{
    if (m_framebufferStack.isEmpty()) {
        return nullptr;
    }
    return m_framebufferStack.pop();
}

VulkanFramebuffer *VulkanContext::currentFramebuffer()
{
    if (m_framebufferStack.isEmpty()) {
        return nullptr;
    }
    return m_framebufferStack.top();
}

VulkanContext *VulkanContext::currentContext()
{
    return s_currentContext;
}

VkFence VulkanContext::getOrCreateFence()
{
    if (m_fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = 0;

        VkResult result = vkCreateFence(m_backend->device(), &fenceInfo, nullptr, &m_fence);
        if (result != VK_SUCCESS) {
            qCWarning(KWIN_VULKAN) << "Failed to create fence:" << result;
            return VK_NULL_HANDLE;
        }
    }
    return m_fence;
}

VkFence VulkanContext::createExportableFence()
{
    if (!m_backend->supportsExternalFenceFd()) {
        return VK_NULL_HANDLE;
    }

    // Set up export info for sync fd
    VkExportFenceCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = &exportInfo;
    fenceInfo.flags = 0;

    VkFence fence = VK_NULL_HANDLE;
    VkResult result = vkCreateFence(m_backend->device(), &fenceInfo, nullptr, &fence);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to create exportable fence:" << result;
        return VK_NULL_HANDLE;
    }

    return fence;
}

FileDescriptor VulkanContext::exportFenceToSyncFd(VkFence fence)
{
    if (!m_backend->supportsExternalFenceFd() || fence == VK_NULL_HANDLE) {
        return FileDescriptor();
    }

    VkFenceGetFdInfoKHR getFdInfo{};
    getFdInfo.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
    getFdInfo.fence = fence;
    getFdInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

    int fd = -1;
    VkResult result = m_backend->vkGetFenceFdKHR()(m_backend->device(), &getFdInfo, &fd);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "Failed to export fence to sync fd:" << result;
        return FileDescriptor();
    }

    return FileDescriptor(fd);
}

bool VulkanContext::supportsExternalFenceFd() const
{
    return m_backend->supportsExternalFenceFd();
}

QImage VulkanContext::readTextureToImage(VulkanTexture *texture, const QRect &rect)
{
    if (!texture || !texture->isValid()) {
        return {};
    }

    const QRect region = rect.isEmpty() ? QRect(QPoint(0, 0), texture->size()) : rect;
    const int w = region.width();
    const int h = region.height();
    if (w <= 0 || h <= 0) {
        return {};
    }

    const VkDeviceSize bufferSize = VkDeviceSize(w) * h * 4; // RGBA8 = 4 bytes/pixel
    auto staging = VulkanBuffer::createStagingBuffer(this, bufferSize);
    if (!staging) {
        qCWarning(KWIN_VULKAN) << "readTextureToImage: failed to create staging buffer";
        return {};
    }

    VkCommandBuffer cmd = beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return {};
    }

    // Transition from SHADER_READ_ONLY_OPTIMAL to TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture->image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy image region to staging buffer
    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0; // tightly packed
    copyRegion.bufferImageHeight = 0; // tightly packed
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageOffset = {region.x(), region.y(), 0};
    copyRegion.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyImageToBuffer(cmd, texture->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging->buffer(), 1, &copyRegion);

    // Transition back to SHADER_READ_ONLY_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    endSingleTimeCommands(cmd); // blocks until GPU idle

    // Determine QImage format from texture format
    const bool isBGRA = (texture->format() == VK_FORMAT_B8G8R8A8_SRGB
                         || texture->format() == VK_FORMAT_B8G8R8A8_UNORM);
    const QImage::Format imgFormat = isBGRA
        ? QImage::Format_ARGB32_Premultiplied // BGRA in memory = ARGB32 in Qt
        : QImage::Format_RGBA8888_Premultiplied;

    QImage img(w, h, imgFormat);
    const void *mapped = staging->map();
    if (!mapped) {
        qCWarning(KWIN_VULKAN) << "readTextureToImage: failed to map staging buffer";
        return {};
    }
    memcpy(img.bits(), mapped, bufferSize);
    staging->unmap();

    return img;
}

} // namespace KWin
