/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vulkangpurendertimequery.h"

#include <array>

namespace KWin
{

VulkanGpuRenderTimeQuery::VulkanGpuRenderTimeQuery(VkDevice device,
                                                   VkQueryPool pool,
                                                   uint32_t beginIdx,
                                                   uint32_t endIdx,
                                                   float timestampPeriod)
    : m_device(device)
    , m_pool(pool)
    , m_beginIdx(beginIdx)
    , m_endIdx(endIdx)
    , m_timestampPeriod(timestampPeriod)
{
}

void VulkanGpuRenderTimeQuery::recordBegin(VkCommandBuffer cmd)
{
    // Reset our two pool slots so this frame's writes start from "unavailable".
    // The host-side vkResetQueryPool entry point requires VK_EXT_host_query_reset
    // (or Vulkan 1.2); the cmd-buffer reset is universally available, so we use
    // that. Resetting both slots in one call is cheap and avoids ordering risk
    // between the reset and the subsequent vkCmdWriteTimestamp.
    vkCmdResetQueryPool(cmd, m_pool, m_beginIdx, 2);

    // VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT writes the timestamp as soon as the
    // command stream reaches this point on the GPU — before any work scheduled
    // after this command starts. Pairs with BOTTOM_OF_PIPE in recordEnd() to
    // bracket the frame's GPU work.
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool, m_beginIdx);

    m_cpuStart = std::chrono::steady_clock::now();
}

void VulkanGpuRenderTimeQuery::recordEnd(VkCommandBuffer cmd)
{
    // BOTTOM_OF_PIPE writes after every previous command finishes. The delta
    // (end - begin) is GPU time spent inside this command buffer.
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, m_endIdx);
    m_endRecorded = true;
}

std::optional<RenderTimeSpan> VulkanGpuRenderTimeQuery::query()
{
    if (!m_endRecorded) {
        return std::nullopt;
    }

    // Read both timestamps with WITH_AVAILABILITY_BIT so vkGetQueryPoolResults
    // does not block — if the GPU has not finished the frame yet, we return
    // std::nullopt and the caller skips the renderJournal update for this
    // frame (existing OutputFrame::queryRenderTime() contract). By the time
    // this is called from RenderLoopPrivate::notifyFrameCompleted() the
    // frame's fence has been signaled, so availability is essentially
    // guaranteed — the bit is a safety net against an aborted submit.
    struct Slot
    {
        uint64_t timestamp;
        uint64_t available;
    };
    std::array<Slot, 2> results{};

    const VkResult r = vkGetQueryPoolResults(
        m_device, m_pool, m_beginIdx, 2,
        sizeof(results), results.data(), sizeof(Slot),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS) {
        return std::nullopt;
    }
    if (!results[0].available || !results[1].available) {
        return std::nullopt;
    }

    // GPU ticks → nanoseconds via the device's timestampPeriod
    // (VkPhysicalDeviceLimits). Use double for the multiplication so we don't
    // lose precision for low-period devices (some report sub-ns periods).
    const uint64_t deltaTicks = results[1].timestamp - results[0].timestamp;
    const auto elapsed = std::chrono::nanoseconds(
        static_cast<int64_t>(double(deltaTicks) * double(m_timestampPeriod)));

    return RenderTimeSpan{
        .start = m_cpuStart,
        .end = m_cpuStart + elapsed,
    };
}

} // namespace KWin
