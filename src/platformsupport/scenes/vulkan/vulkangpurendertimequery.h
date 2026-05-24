/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/renderbackend.h"
#include "kwin_export.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace KWin
{

/**
 * @brief RenderTimeQuery backed by Vulkan timestamp queries.
 *
 * Mirrors the CpuRenderTimeQuery contract (returns a synthetic RenderTimeSpan
 * with start in steady_clock and end = start + GPU-measured duration). Two
 * VkQueryPool slots — one written at TOP_OF_PIPE in the scene command buffer's
 * begin, one at BOTTOM_OF_PIPE before the buffer ends — are queried back after
 * the frame's fence signals, multiplied by the device's timestampPeriod to
 * convert ticks to nanoseconds.
 *
 * The slot indices are owned by the caller (typically derived from the
 * frame-in-flight index so adjacent in-flight frames don't collide). The pool
 * itself is owned by VulkanBackend.
 *
 * Lifecycle: construct in the renderer's beginFrame, call recordBegin() into
 * the same command buffer, recordEnd() before vkEndCommandBuffer. Once the
 * GPU has consumed the work (fence signaled) query() is non-blocking and
 * returns the span; before then it returns std::nullopt.
 */
class KWIN_EXPORT VulkanGpuRenderTimeQuery : public RenderTimeQuery
{
public:
    VulkanGpuRenderTimeQuery(VkDevice device,
                             VkQueryPool pool,
                             uint32_t beginIdx,
                             uint32_t endIdx,
                             float timestampPeriod);
    ~VulkanGpuRenderTimeQuery() override = default;

    /**
     * @brief Reset our two pool slots and write the TOP_OF_PIPE timestamp.
     *
     * Captures the CPU steady_clock "start" used to build the synthetic
     * RenderTimeSpan at query() time. Must be called from inside a command
     * buffer recording (between vkBeginCommandBuffer and vkEndCommandBuffer).
     */
    void recordBegin(VkCommandBuffer cmd);

    /**
     * @brief Write the BOTTOM_OF_PIPE timestamp into @p cmd.
     *
     * Must be called from inside the same command buffer recording, after
     * all the work whose duration we want to measure.
     */
    void recordEnd(VkCommandBuffer cmd);

    std::optional<RenderTimeSpan> query() override;

private:
    const VkDevice m_device;
    const VkQueryPool m_pool;
    const uint32_t m_beginIdx;
    const uint32_t m_endIdx;
    const float m_timestampPeriod;
    std::chrono::steady_clock::time_point m_cpuStart;
    bool m_endRecorded = false;
};

} // namespace KWin
