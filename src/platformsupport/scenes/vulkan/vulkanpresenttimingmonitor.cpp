/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vulkanpresenttimingmonitor.h"

#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanswapchain.h"

#include <array>

namespace KWin
{

// Helper-thread drain only ever looks at the most recently completed presents,
// so a small scratch buffer is enough. Independent of the swapchain's own
// timing queue size (set in setupPresentTimingQueue()).
static constexpr uint32_t kDrainBufferEntries = 8;
static constexpr uint32_t kDrainBufferMaxStages = 4;

// Maximum wait. Long enough that real presents always complete first; short
// enough that a stuck/cancelled present unblocks the helper and the
// in-flight OutputFrame can be resolved via the fallback timestamp before
// RenderLoop notices anything stalled.
static constexpr uint64_t kWaitTimeoutNs = 1'000'000'000ull; // 1 s

static std::chrono::nanoseconds monotonicNow()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

VulkanPresentTimingMonitorHelper::VulkanPresentTimingMonitorHelper(VkDevice device,
                                                                   VkSwapchainKHR swapchain,
                                                                   PFN_vkWaitForPresent2KHR waitFn,
                                                                   PFN_vkGetPastPresentationTimingEXT pastTimingFn)
    : m_device(device)
    , m_swapchain(swapchain)
    , m_vkWaitForPresent2KHR(waitFn)
    , m_vkGetPastPresentationTimingEXT(pastTimingFn)
{
}

void VulkanPresentTimingMonitorHelper::waitForPresent(uint64_t presentId)
{
    if (!m_vkWaitForPresent2KHR || m_swapchain == VK_NULL_HANDLE) {
        Q_EMIT vblankOccurred(monotonicNow());
        return;
    }

    VkPresentWait2InfoKHR waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR;
    waitInfo.presentId = presentId;
    waitInfo.timeout = kWaitTimeoutNs;

    const VkResult r = m_vkWaitForPresent2KHR(m_device, m_swapchain, &waitInfo);

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // The swapchain is gone — no real timestamp is coming for this id. Signal
        // the backend to recreate the monitor, and still resolve the OutputFrame
        // with a fallback so RenderLoop is not left waiting forever.
        Q_EMIT vblankOccurred(monotonicNow());
        Q_EMIT errorOccurred();
        return;
    }
    if (r != VK_SUCCESS) {
        // VK_TIMEOUT or any other error: the present may have been dropped, or
        // the GPU may be stuck. Fall back to "now" so the in-flight OutputFrame
        // is never stranded — this is the explicit safety valve against the
        // freeze that motivated the original synchronous-extrapolation design.
        Q_EMIT vblankOccurred(monotonicNow());
        return;
    }

    // The wait unblocked at presentation — "now" is already a tight upper bound,
    // and absent a drained entry it is the timestamp we report. If the timing
    // queue carries a precise FIRST_PIXEL_VISIBLE entry for this presentId,
    // prefer that.
    std::chrono::nanoseconds timestamp = monotonicNow();

    // Stages drained for this presentId. Zero == not reported by the driver.
    // Forwarded as a separate signal (presentTimingsReady) so the backend can
    // stash per-stage data on the in-flight OutputFrame before vblankOccurred()
    // triggers OutputFrame::presented().
    std::chrono::nanoseconds queueOperationsEnd = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds firstPixelOut = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds firstPixelVisible = std::chrono::nanoseconds::zero();

    if (m_vkGetPastPresentationTimingEXT) {
        std::array<std::array<VkPresentStageTimeEXT, kDrainBufferMaxStages>, kDrainBufferEntries> stageStorage{};
        std::array<VkPastPresentationTimingEXT, kDrainBufferEntries> timings{};
        for (uint32_t i = 0; i < kDrainBufferEntries; ++i) {
            timings[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
            timings[i].presentStageCount = kDrainBufferMaxStages;
            timings[i].pPresentStages = stageStorage[i].data();
        }

        VkPastPresentationTimingPropertiesEXT props{};
        props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
        props.presentationTimingCount = kDrainBufferEntries;
        props.pPresentationTimings = timings.data();

        VkPastPresentationTimingInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
        info.swapchain = m_swapchain;

        const VkResult dr = m_vkGetPastPresentationTimingEXT(m_device, &info, &props);
        if (dr == VK_SUCCESS || dr == VK_INCOMPLETE) {
            // The drain returns every completed entry — older entries for earlier
            // presents are simply ignored here; their wait tasks already ran (or
            // will run) and the queue is consumed regardless. Only look for the
            // entry matching this wait's presentId.
            for (uint32_t i = 0; i < props.presentationTimingCount; ++i) {
                const VkPastPresentationTimingEXT &t = timings[i];
                if (t.presentId != presentId) {
                    continue;
                }
                // Mesa MR !39551 emits stages in VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT
                // (1000208000) — the extension's own domain — not raw CLOCK_MONOTONIC.
                // Per the present-stage clocks gotcha: stage 0x4 (FIRST_PIXEL_OUT)
                // happens to be in CLOCK_MONOTONIC nanoseconds, while stage 0x1
                // (QUEUE_OPERATIONS_END) is in a separate GPU-side clock. Same
                // labeled domain, different underlying clocks. Consumers must
                // therefore treat per-stage timestamps as comparable only against
                // values from the same stage — not across stages, and not against
                // arbitrary CPU steady_clock values without prior calibration.
                if (t.timeDomain != VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR
                    && t.timeDomain != VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR
                    && t.timeDomain != VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) {
                    continue;
                }
                for (uint32_t s = 0; s < t.presentStageCount; ++s) {
                    const VkPresentStageTimeEXT &stage = t.pPresentStages[s];
                    const auto stageTime = std::chrono::nanoseconds(static_cast<int64_t>(stage.time));
                    if (stage.stage & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT) {
                        firstPixelVisible = stageTime;
                    }
                    if (stage.stage & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) {
                        firstPixelOut = stageTime;
                    }
                    if (stage.stage & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT) {
                        queueOperationsEnd = stageTime;
                    }
                }
                break;
            }
            // Prefer the latest CLOCK_MONOTONIC-correlated stage for the
            // vblankOccurred timestamp (which feeds RenderLoop's
            // lastPresentationTimestamp). FIRST_PIXEL_VISIBLE is ideal but Mesa
            // doesn't yet emit it (its supportedStages mask is 0x5 =
            // QUEUE_OPS_END | FIRST_PIXEL_OUT); FIRST_PIXEL_OUT is in
            // CLOCK_MONOTONIC ns on Mesa, so falling back to it is much tighter
            // than monotonicNow(). QUEUE_OPS_END is in a different clock and
            // would corrupt RenderLoop's scheduling, so skip it as a fallback.
            if (firstPixelVisible != std::chrono::nanoseconds::zero()) {
                timestamp = firstPixelVisible;
            } else if (firstPixelOut != std::chrono::nanoseconds::zero()) {
                timestamp = firstPixelOut;
            }
        }
    }

    // Emit stage data first so backends connected on the same QObject see
    // it before vblankOccurred() triggers OutputFrame::presented(). Both are
    // delivered via Qt::QueuedConnection and processed in FIFO order on the
    // target object — same-thread same-target ordering is guaranteed.
    Q_EMIT presentTimingsReady(presentId, queueOperationsEnd, firstPixelOut, firstPixelVisible);
    Q_EMIT vblankOccurred(timestamp);
}

std::unique_ptr<VulkanPresentTimingMonitor> VulkanPresentTimingMonitor::create(VulkanBackend *backend,
                                                                               VulkanSwapchain *swapchain)
{
    if (!backend || !swapchain) {
        return nullptr;
    }
    if (!backend->supportsPresentTiming() || !backend->supportsPresentWait2()) {
        return nullptr;
    }
    auto waitFn = backend->waitForPresent2KHR();
    auto pastTimingFn = backend->getPastPresentationTimingEXT();
    if (!waitFn || !pastTimingFn) {
        return nullptr;
    }
    return std::unique_ptr<VulkanPresentTimingMonitor>(
        new VulkanPresentTimingMonitor(backend->device(),
                                       swapchain->swapchain(),
                                       waitFn,
                                       pastTimingFn));
}

VulkanPresentTimingMonitor::VulkanPresentTimingMonitor(VkDevice device,
                                                       VkSwapchainKHR swapchain,
                                                       PFN_vkWaitForPresent2KHR waitFn,
                                                       PFN_vkGetPastPresentationTimingEXT pastTimingFn)
    : m_helper(device, swapchain, waitFn, pastTimingFn)
{
    m_helper.moveToThread(&m_thread);

    // Forward the helper's signals to the VsyncMonitor base signals — the
    // backend connects to vblankOccurred() the same way it does for the GLX
    // monitor.
    connect(&m_helper, &VulkanPresentTimingMonitorHelper::errorOccurred,
            this, &VulkanPresentTimingMonitor::errorOccurred);
    connect(&m_helper, &VulkanPresentTimingMonitorHelper::vblankOccurred,
            this, &VulkanPresentTimingMonitor::vblankOccurred);
    // Per-stage timing for backends that want to stash data on the in-flight
    // OutputFrame before OutputFrame::presented() runs. Forwarded in the same
    // emit order as the helper so FIFO ordering on the backend target holds.
    connect(&m_helper, &VulkanPresentTimingMonitorHelper::presentTimingsReady,
            this, &VulkanPresentTimingMonitor::presentTimingsReady);

    m_thread.setObjectName(QStringLiteral("vulkan present-timing monitor"));
    m_thread.start();
}

VulkanPresentTimingMonitor::~VulkanPresentTimingMonitor()
{
    // Stop the helper thread cleanly. Any in-flight wait will be aborted by
    // the device/swapchain teardown happening outside this destructor; if
    // we are torn down without that having happened, vkWaitForPresent2KHR
    // will eventually unblock (timeout) and the dangling signal is harmless
    // because the connections to the now-deleted main-thread object will
    // be dropped by Qt.
    m_thread.quit();
    m_thread.wait();
}

void VulkanPresentTimingMonitor::armWithPresentId(uint64_t presentId)
{
    QMetaObject::invokeMethod(&m_helper,
                              "waitForPresent",
                              Qt::QueuedConnection,
                              Q_ARG(uint64_t, presentId));
}

} // namespace KWin

#include "moc_vulkanpresenttimingmonitor.cpp"
