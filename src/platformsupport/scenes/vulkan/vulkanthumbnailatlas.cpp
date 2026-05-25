/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vulkanthumbnailatlas.h"

#include "utils/common.h"
#include "vma_usage.h"
#include "vulkanbackend.h"
#include "vulkanbuffer.h"
#include "vulkancontext.h"

#include <QDebug>

#include <algorithm>

namespace KWin
{

namespace
{
constexpr VkFormat kAtlasFormat = VK_FORMAT_R8G8B8A8_SRGB;

// Number of mip levels for a fallback image of the given size. Caps at
// kMipLevels to keep the per-slot mip view count consistent (and the
// sampler's max-lod selection matches what an atlas-resident slot
// would expose).
uint32_t mipLevelsForSize(QSize size)
{
    const int maxDim = std::max(size.width(), size.height());
    uint32_t levels = 1;
    int dim = maxDim;
    while (dim > 1 && levels < VulkanThumbnailAtlas::kMipLevels) {
        dim /= 2;
        ++levels;
    }
    return levels;
}
} // namespace

VulkanThumbnailAtlas *VulkanThumbnailAtlas::get(VulkanContext *ctx)
{
    if (!ctx) {
        return nullptr;
    }
    // Lifetime tied to the VulkanContext: stored as a per-context
    // singleton. Lookup is keyed by the pointer so multiple contexts
    // (e.g., post-reset) each get their own atlas. Cleanup happens via
    // the destructor when VulkanContext shuts down and pumps deferred
    // destructions.
    static std::unordered_map<VulkanContext *, std::unique_ptr<VulkanThumbnailAtlas>> atlases;
    auto it = atlases.find(ctx);
    if (it == atlases.end()) {
        auto atlas = std::unique_ptr<VulkanThumbnailAtlas>(new VulkanThumbnailAtlas(ctx));
        it = atlases.emplace(ctx, std::move(atlas)).first;
    }
    return it->second.get();
}

VulkanThumbnailAtlas::VulkanThumbnailAtlas(VulkanContext *ctx)
    : m_context(ctx)
{
}

VulkanThumbnailAtlas::~VulkanThumbnailAtlas()
{
    if (m_atlasSrgbView != VK_NULL_HANDLE) {
        m_context->queueImageViewForDestruction(m_atlasSrgbView);
    }
    if (m_atlasMipZeroView != VK_NULL_HANDLE) {
        m_context->queueImageViewForDestruction(m_atlasMipZeroView);
    }
    if (m_atlasImage != VK_NULL_HANDLE) {
        m_context->queueImageForDestruction(m_atlasImage, m_atlasAlloc);
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->backend()->device(), m_sampler, nullptr);
    }
    for (auto &[handle, rec] : m_fallbackSlots) {
        if (rec.srgbView != VK_NULL_HANDLE) {
            m_context->queueImageViewForDestruction(rec.srgbView);
        }
        if (rec.mipZeroView != VK_NULL_HANDLE) {
            m_context->queueImageViewForDestruction(rec.mipZeroView);
        }
        if (rec.image != VK_NULL_HANDLE) {
            m_context->queueImageForDestruction(rec.image, rec.allocation);
        }
    }
}

bool VulkanThumbnailAtlas::ensureAtlasInitialized()
{
    if (m_atlasReady) {
        return true;
    }
    if (m_atlasFailed) {
        return false;
    }
    if (!createAtlasImage() || !createSampler()) {
        m_atlasFailed = true;
        return false;
    }
    m_atlasReady = true;
    return true;
}

bool VulkanThumbnailAtlas::createAtlasImage()
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent.width = static_cast<uint32_t>(kAtlasSize);
    info.extent.height = static_cast<uint32_t>(kAtlasSize);
    info.extent.depth = 1;
    info.mipLevels = kMipLevels;
    info.arrayLayers = 1;
    info.format = kAtlasFormat;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // SAMPLED for the QSGRenderNode reads, COLOR_ATTACHMENT for the
    // source-window renders, TRANSFER_SRC/DST for the mip blit cascade.
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(VulkanAllocator::allocator(), &info, &allocInfo,
                       &m_atlasImage, &m_atlasAlloc, nullptr)
        != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "VulkanThumbnailAtlas: vmaCreateImage failed";
        return false;
    }

    if (!createSrgbView(m_atlasImage, kMipLevels, &m_atlasSrgbView)
        || !createMipZeroView(m_atlasImage, &m_atlasMipZeroView)) {
        return false;
    }
    return true;
}

bool VulkanThumbnailAtlas::createSampler()
{
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.minLod = 0.0f;
    info.maxLod = static_cast<float>(kMipLevels);
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(m_context->backend()->device(), &info, nullptr, &m_sampler) != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "VulkanThumbnailAtlas: vkCreateSampler failed";
        return false;
    }
    return true;
}

bool VulkanThumbnailAtlas::createSrgbView(VkImage image, uint32_t mipLevels, VkImageView *out)
{
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = kAtlasFormat;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = mipLevels;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_context->backend()->device(), &info, nullptr, out) != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "VulkanThumbnailAtlas: vkCreateImageView (SRGB) failed";
        return false;
    }
    return true;
}

bool VulkanThumbnailAtlas::createMipZeroView(VkImage image, VkImageView *out)
{
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = kAtlasFormat;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_context->backend()->device(), &info, nullptr, out) != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "VulkanThumbnailAtlas: vkCreateImageView (mip 0) failed";
        return false;
    }
    return true;
}

QRect VulkanThumbnailAtlas::packIntoAtlas(QSize paddedSize)
{
    const int w = paddedSize.width();
    const int h = paddedSize.height();
    if (w > kAtlasSize || h > kAtlasSize) {
        return QRect();
    }
    // Best-fit by shelf height: shelves that already match the request's
    // height waste no vertical space. If none match, open a new shelf.
    Shelf *best = nullptr;
    for (auto &shelf : m_shelves) {
        if (shelf.height < h) {
            continue;
        }
        if (shelf.cursorX + w > kAtlasSize) {
            continue;
        }
        if (!best || shelf.height < best->height) {
            best = &shelf;
        }
    }
    if (!best) {
        if (m_atlasUsedY + h > kAtlasSize) {
            return QRect();
        }
        m_shelves.push_back(Shelf{m_atlasUsedY, h, 0});
        best = &m_shelves.back();
        m_atlasUsedY += h;
    }
    const QRect padded(best->cursorX, best->y, w, h);
    best->cursorX += w;
    // Inner rect: strip the half-padding on each side.
    return padded.adjusted(kSlotPadding / 2, kSlotPadding / 2,
                           -kSlotPadding / 2, -kSlotPadding / 2);
}

QRect VulkanThumbnailAtlas::findFreeRect(QSize requestedSize)
{
    if (m_freeRects.empty()) {
        return QRect();
    }
    const int reqMax = std::max(requestedSize.width(), requestedSize.height());
    // Best-fit: smallest entry whose max-dim is at least reqMax.
    auto it = m_freeRects.lower_bound(reqMax);
    while (it != m_freeRects.end()) {
        if (it->second.width() >= requestedSize.width()
            && it->second.height() >= requestedSize.height()) {
            const QRect found = it->second;
            m_freeRects.erase(it);
            // Inner rect inside the freed (padded) area.
            return found;
        }
        ++it;
    }
    return QRect();
}

bool VulkanThumbnailAtlas::allocateFallback(QSize size, Slot *outSlot)
{
    const uint32_t mipLevels = mipLevelsForSize(size);

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent.width = static_cast<uint32_t>(size.width());
    info.extent.height = static_cast<uint32_t>(size.height());
    info.extent.depth = 1;
    info.mipLevels = mipLevels;
    info.arrayLayers = 1;
    info.format = kAtlasFormat;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    FallbackRecord rec{};
    if (vmaCreateImage(VulkanAllocator::allocator(), &info, &allocInfo,
                       &rec.image, &rec.allocation, nullptr)
        != VK_SUCCESS) {
        qCWarning(KWIN_VULKAN) << "VulkanThumbnailAtlas: fallback vmaCreateImage failed";
        return false;
    }
    if (!createSrgbView(rec.image, mipLevels, &rec.srgbView)
        || !createMipZeroView(rec.image, &rec.mipZeroView)) {
        if (rec.image != VK_NULL_HANDLE) {
            m_context->queueImageForDestruction(rec.image, rec.allocation);
        }
        return false;
    }

    const uint64_t handle = m_nextHandle++;
    m_fallbackSlots.emplace(handle, rec);
    outSlot->rect = QRect(QPoint(0, 0), size);
    outSlot->mipLevels = mipLevels;
    outSlot->image = rec.image;
    outSlot->srgbView = rec.srgbView;
    outSlot->mipZeroView = rec.mipZeroView;
    outSlot->handle = handle;
    outSlot->isFallback = true;
    outSlot->hasContent = false;
    return true;
}

VulkanThumbnailAtlas::Slot VulkanThumbnailAtlas::reserve(QSize requestedSize)
{
    Slot slot;
    if (requestedSize.isEmpty()) {
        return slot;
    }
    if (!ensureAtlasInitialized()) {
        // Try fallback even if atlas init failed — some basic Vulkan
        // setups may still succeed at per-image allocations.
        allocateFallback(requestedSize, &slot);
        return slot;
    }

    // Try the free list first.
    const QRect freed = findFreeRect(requestedSize);
    if (!freed.isNull()) {
        const uint64_t handle = m_nextHandle++;
        // The freed rect was stored at *padded* dimensions; the consumer
        // gets the inner rect.
        const QRect inner = freed.adjusted(kSlotPadding / 2, kSlotPadding / 2,
                                           -kSlotPadding / 2, -kSlotPadding / 2);
        // Resize inner to the actual requested size (anchored top-left).
        const QRect actualInner(inner.topLeft(), requestedSize);
        m_atlasSlotRects.emplace(handle, freed);
        slot.rect = actualInner;
        slot.mipLevels = kMipLevels;
        slot.image = m_atlasImage;
        slot.srgbView = m_atlasSrgbView;
        slot.mipZeroView = m_atlasMipZeroView;
        slot.handle = handle;
        slot.isFallback = false;
        slot.hasContent = false;
        return slot;
    }

    const QSize padded(requestedSize.width() + kSlotPadding,
                       requestedSize.height() + kSlotPadding);
    const QRect inner = packIntoAtlas(padded);
    if (inner.isNull()) {
        // Atlas overflow — dedicate a per-source image.
        allocateFallback(requestedSize, &slot);
        return slot;
    }
    const QRect actualInner(inner.topLeft(), requestedSize);
    const uint64_t handle = m_nextHandle++;
    // Store the padded rect for release(); we want the whole padded
    // block back into the free list when the slot is released.
    const QRect paddedStored(inner.topLeft() - QPoint(kSlotPadding / 2, kSlotPadding / 2),
                             padded);
    m_atlasSlotRects.emplace(handle, paddedStored);
    slot.rect = actualInner;
    slot.mipLevels = kMipLevels;
    slot.image = m_atlasImage;
    slot.srgbView = m_atlasSrgbView;
    slot.mipZeroView = m_atlasMipZeroView;
    slot.handle = handle;
    slot.isFallback = false;
    slot.hasContent = false;
    return slot;
}

void VulkanThumbnailAtlas::release(Slot &slot)
{
    if (!slot.isValid()) {
        return;
    }
    if (slot.isFallback) {
        auto it = m_fallbackSlots.find(slot.handle);
        if (it != m_fallbackSlots.end()) {
            if (it->second.srgbView != VK_NULL_HANDLE) {
                m_context->queueImageViewForDestruction(it->second.srgbView);
            }
            if (it->second.mipZeroView != VK_NULL_HANDLE) {
                m_context->queueImageViewForDestruction(it->second.mipZeroView);
            }
            if (it->second.image != VK_NULL_HANDLE) {
                m_context->queueImageForDestruction(it->second.image, it->second.allocation);
            }
            m_fallbackSlots.erase(it);
        }
    } else {
        auto it = m_atlasSlotRects.find(slot.handle);
        if (it != m_atlasSlotRects.end()) {
            // Return the padded block to the free list, keyed by max-dim.
            const QRect padded = it->second;
            m_freeRects.emplace(std::max(padded.width(), padded.height()), padded);
            m_atlasSlotRects.erase(it);
        }
    }
    slot = Slot();
}

void VulkanThumbnailAtlas::prepareForRenderTo(VkCommandBuffer cmd, const Slot &slot)
{
    if (!slot.isValid()) {
        return;
    }
    // Per-subresource layout transitions on a shared atlas would clobber
    // the layout of every other slot at the same mip level (Vulkan
    // applies barriers to subresource ranges, not 2D regions). Keep the
    // entire atlas image in GENERAL throughout its lifetime — slightly
    // less optimal on paper, but the only correct way to share an image
    // between many active slots that all read/write concurrently across
    // submits.
    const VkImageLayout oldLayout = m_atlasFirstUse && !slot.isFallback
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_GENERAL;
    const VkImageLayout fallbackOldLayout = slot.hasContent
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = slot.isFallback
        ? (slot.hasContent ? VK_ACCESS_SHADER_READ_BIT : 0)
        : (m_atlasFirstUse ? 0 : VK_ACCESS_SHADER_READ_BIT);
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    barrier.oldLayout = slot.isFallback ? fallbackOldLayout : oldLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = slot.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = slot.mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    const VkPipelineStageFlags srcStage = (barrier.srcAccessMask == 0)
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
        : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    if (!slot.isFallback) {
        m_atlasFirstUse = false;
    }
}

void VulkanThumbnailAtlas::generateMipsAndPublish(VkCommandBuffer cmd, Slot &slot)
{
    if (!slot.isValid()) {
        return;
    }

    // Transition mip 0 from COLOR_ATTACHMENT_WRITE to TRANSFER_READ; the
    // other mips are still UNDEFINED (or stale from prior render) — we'll
    // overwrite them with the blit cascade so TRANSFER_WRITE access is
    // enough.
    {
        VkImageMemoryBarrier writeToBlit{};
        writeToBlit.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        writeToBlit.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        writeToBlit.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        writeToBlit.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        writeToBlit.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        writeToBlit.image = slot.image;
        writeToBlit.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        writeToBlit.subresourceRange.baseMipLevel = 0;
        writeToBlit.subresourceRange.levelCount = 1;
        writeToBlit.subresourceRange.baseArrayLayer = 0;
        writeToBlit.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &writeToBlit);
    }

    // Cascade: mip N-1 → mip N. Each blit reads the slot's sub-rect at
    // the previous mip and writes the halved sub-rect at the current
    // mip. After each level, transition the just-written level from
    // TRANSFER_WRITE to TRANSFER_READ so the next iteration can sample
    // it.
    //
    // Crucially: oldLayout must be GENERAL (not UNDEFINED) on this
    // barrier. The atlas is shared across many slots; specifying
    // UNDEFINED here would let the driver discard the *entire* mip
    // level — wiping the downscales of every other slot already
    // resident in the atlas. The atlas image is in GENERAL throughout
    // its lifetime (prepareForRenderTo's all-mips transition handles
    // the initial UNDEFINED→GENERAL on first use), so a same-layout
    // access-mask-only barrier is correct here.
    QRect srcRect = slot.rect;
    for (uint32_t level = 1; level < slot.mipLevels; ++level) {
        // Make any prior shader reads of this level (from the published
        // state at the end of the previous frame) and any prior
        // transfer writes (this frame, if multiple slots share the
        // cascade) visible to the upcoming TRANSFER_WRITE.
        VkImageMemoryBarrier toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
            | VK_ACCESS_TRANSFER_WRITE_BIT
            | VK_ACCESS_TRANSFER_READ_BIT;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toDst.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toDst.image = slot.image;
        toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toDst.subresourceRange.baseMipLevel = level;
        toDst.subresourceRange.levelCount = 1;
        toDst.subresourceRange.baseArrayLayer = 0;
        toDst.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                 | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);

        const QRect dstRect(srcRect.x() / 2, srcRect.y() / 2,
                            std::max(1, srcRect.width() / 2),
                            std::max(1, srcRect.height() / 2));
        VkImageBlit region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.mipLevel = level - 1;
        region.srcSubresource.baseArrayLayer = 0;
        region.srcSubresource.layerCount = 1;
        region.srcOffsets[0] = {srcRect.x(), srcRect.y(), 0};
        region.srcOffsets[1] = {srcRect.x() + srcRect.width(),
                                srcRect.y() + srcRect.height(), 1};
        region.dstSubresource = region.srcSubresource;
        region.dstSubresource.mipLevel = level;
        region.dstOffsets[0] = {dstRect.x(), dstRect.y(), 0};
        region.dstOffsets[1] = {dstRect.x() + dstRect.width(),
                                dstRect.y() + dstRect.height(), 1};
        vkCmdBlitImage(cmd,
                       slot.image, VK_IMAGE_LAYOUT_GENERAL,
                       slot.image, VK_IMAGE_LAYOUT_GENERAL,
                       1, &region, VK_FILTER_LINEAR);

        // Prepare the level we just wrote as TRANSFER_READ for the next
        // iteration (if any).
        if (level + 1 < slot.mipLevels) {
            VkImageMemoryBarrier toSrc{};
            toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toSrc.image = slot.image;
            toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toSrc.subresourceRange.baseMipLevel = level;
            toSrc.subresourceRange.levelCount = 1;
            toSrc.subresourceRange.baseArrayLayer = 0;
            toSrc.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toSrc);
        }
        srcRect = dstRect;
    }

    // Final publishing barrier: all mips visible to Qt's subsequent
    // FRAGMENT_SHADER reads on the same queue. Mip 0 was last read by
    // TRANSFER (or written by COLOR_ATTACHMENT_OUTPUT if mipLevels==1);
    // mips 1..N-1 were last written by TRANSFER. Cover both with the
    // src-access mask.
    VkImageMemoryBarrier publish{};
    publish.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    publish.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
        | VK_ACCESS_TRANSFER_READ_BIT
        | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    publish.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    publish.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    publish.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    publish.image = slot.image;
    publish.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    publish.subresourceRange.baseMipLevel = 0;
    publish.subresourceRange.levelCount = slot.mipLevels;
    publish.subresourceRange.baseArrayLayer = 0;
    publish.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT
                             | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &publish);

    slot.hasContent = true;
}

} // namespace KWin
