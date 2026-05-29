/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "kwin_export.h"

#include <QSize>

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace KWin
{

class VulkanContext;
class VulkanRenderPass;
class VulkanFramebuffer;
class VulkanTexture;

/**
 * @brief Reusable dual-kawase blur for the Vulkan compositor.
 *
 * Encapsulates everything the kawase pyramid needs that does not depend on
 * a specific consumer: a linear clamp-to-edge sampler, a single-binding
 * descriptor-set layout (combined image sampler, immutable sampler, push
 * descriptor when available), a pipeline layout with 12 bytes of fragment
 * push constants (`vec2 halfpixel` + `float offset`), a `GENERAL → GENERAL`
 * colour render pass (`DONT_CARE`/`STORE`) for the intermediate targets, and
 * the downsample + upsample graphics pipelines that share all of the above.
 *
 * Both the standalone blur effect and OverviewEffectV2's wallpaper backdrop
 * used to carry their own near-identical copy of this; they now share one
 * instance each. Consumers still own their source and scratch render targets
 * (allocation policies differ) and drive the barriers around the capture —
 * the helper only builds the pipeline objects and records the down/up chain.
 *
 * The render pass and pipelines are baked for one colour format (the format
 * of the level textures), passed at @ref create time. Build the level
 * framebuffers against @ref renderPass so they are render-pass compatible.
 *
 * Lifetime: tied to the @ref VulkanContext it was created against; destroy it
 * before that context goes away.
 */
class KWIN_EXPORT VulkanKawaseBlur
{
public:
    ~VulkanKawaseBlur();

    VulkanKawaseBlur(const VulkanKawaseBlur &) = delete;
    VulkanKawaseBlur &operator=(const VulkanKawaseBlur &) = delete;

    /**
     * @brief Build the pipeline objects for level textures of @p colorFormat.
     * @return nullptr on any creation failure.
     */
    static std::unique_ptr<VulkanKawaseBlur> create(VulkanContext *ctx, VkFormat colorFormat);

    VkFormat colorFormat() const
    {
        return m_colorFormat;
    }

    /// Render pass the level framebuffers must be created against.
    VulkanRenderPass *renderPass() const
    {
        return m_renderPass.get();
    }

    /// Pipeline layout (shared 12-byte fragment push constants). Exposed so
    /// consumers can build companion pipelines (e.g. a composite pass) that
    /// bind the same descriptor set.
    VkPipelineLayout pipelineLayout() const
    {
        return m_pipelineLayout;
    }

    /// Descriptor-set layout (binding 0 = combined image sampler, immutable
    /// linear sampler). Pair with @ref pipelineLayout for
    /// VulkanContext::bindDescriptors.
    VkDescriptorSetLayout descriptorSetLayout() const
    {
        return m_dsLayout;
    }

    /**
     * @brief One pyramid level: a view to sample and the framebuffer that
     * backs the same texture, plus its pixel size. Build @c framebuffer
     * against @ref renderPass.
     */
    struct Level
    {
        VkImageView view = VK_NULL_HANDLE;
        VulkanFramebuffer *framebuffer = nullptr;
        QSize size;
    };

    /**
     * @brief Record the down + up kawase chain into @p cmd.
     *
     * `levels[0]` is the full-resolution source — already populated, with its
     * prior write (blit or render) made visible to a fragment-shader read by
     * the caller — and is also the final destination. `levels[1..N]` are
     * progressively smaller scratch targets. Downsamples `0 → 1 → … → N`,
     * then upsamples `N → … → 0`. A colour-write → (shader-read |
     * transfer-read) memory barrier is inserted after every pass, including
     * the last, so the result in `levels[0]` is ready to sample or blit.
     *
     * @param offset Kawase tap distance (1.0 = canonical pattern).
     * @param flipViewportY Use a negative-height viewport (for sources whose
     *        device-Y runs top-down, e.g. captured swapchain regions). Pass
     *        false for sources already authored bottom-up.
     *
     * Needs at least two levels; a shorter chain is a no-op.
     */
    void recordPyramid(VkCommandBuffer cmd, const std::vector<Level> &levels,
                       float offset, bool flipViewportY) const;

private:
    VulkanKawaseBlur(VulkanContext *ctx, VkFormat colorFormat);
    bool init();
    void recordPass(VkCommandBuffer cmd, VkPipeline pipeline, VkImageView srcView,
                    const QSize &srcSize, const Level &dst, float offset,
                    bool flipViewportY) const;

    VulkanContext *m_ctx = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    VkPipeline m_downsamplePipeline = VK_NULL_HANDLE;
    VkPipeline m_upsamplePipeline = VK_NULL_HANDLE;
};

} // namespace KWin
