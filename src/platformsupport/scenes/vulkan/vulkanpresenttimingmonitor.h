/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "utils/vsyncmonitor.h"

#include <QThread>
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace KWin
{

class VulkanBackend;
class VulkanSwapchain;

/**
 * @brief Worker living on the monitor's QThread. Each arm(presentId) is invoked
 * as a queued slot — the helper blocks on vkWaitForPresent2KHR for that id,
 * then drains vkGetPastPresentationTimingEXT to pull the matching real
 * timestamp, and emits vblankOccurred() back to the main thread.
 *
 * Why a worker thread at all: vkWaitForPresent2KHR blocks until the present
 * lands on the screen. Blocking on the main thread would freeze the
 * compositor. Mirroring the SGIVideoSyncVsyncMonitor pattern keeps the wait
 * off the main thread and lets the vblank signal fire independently of
 * compositor activity — same shape as the GLX vblank path, which is what
 * RenderLoop expects.
 */
class KWIN_EXPORT VulkanPresentTimingMonitorHelper : public QObject
{
    Q_OBJECT

public:
    VulkanPresentTimingMonitorHelper(VkDevice device,
                                     VkSwapchainKHR swapchain,
                                     PFN_vkWaitForPresent2KHR waitFn,
                                     PFN_vkGetPastPresentationTimingEXT pastTimingFn);

public Q_SLOTS:
    /**
     * @brief Wait for present @p presentId to land, then emit its real timestamp.
     *
     * On wait failure or timeout, falls back to emitting steady_clock::now()
     * so the in-flight OutputFrame is never stranded. On swapchain-out-of-date
     * errors emits errorOccurred() so the backend can tear down + recreate
     * the monitor on its next swapchain recreation.
     */
    void waitForPresent(uint64_t presentId);

Q_SIGNALS:
    void errorOccurred();
    void vblankOccurred(std::chrono::nanoseconds timestamp);
    /**
     * @brief Fired (when the drain returns useful entries) before
     * vblankOccurred() with the per-stage on-screen timestamps for the
     * just-completed presentId.
     *
     * Each timestamp is std::chrono::nanoseconds::zero() if that stage was
     * not reported. Slots are connected on the backend's QObject and Qt
     * queued-connection FIFO ordering on the same target guarantees this
     * signal is processed *before* vblankOccurred() — so the backend can
     * stash the stages on the in-flight OutputFrame before
     * OutputFrame::presented() runs.
     */
    void presentTimingsReady(uint64_t presentId,
                             std::chrono::nanoseconds queueOperationsEnd,
                             std::chrono::nanoseconds firstPixelOut,
                             std::chrono::nanoseconds firstPixelVisible);

private:
    VkDevice m_device;
    VkSwapchainKHR m_swapchain;
    PFN_vkWaitForPresent2KHR m_vkWaitForPresent2KHR;
    PFN_vkGetPastPresentationTimingEXT m_vkGetPastPresentationTimingEXT;
};

/**
 * @brief VsyncMonitor backed by VK_KHR_present_wait2 + VK_EXT_present_timing.
 *
 * Replaces the GLX-style hardware vsync monitor on a Vulkan compositor where
 * the extensions are available. Per-frame flow:
 *  - present() submits a frame tagged with presentId N and calls armWithPresentId(N).
 *  - armWithPresentId() posts a queued waitForPresent(N) invocation on the helper.
 *  - The helper thread blocks on vkWaitForPresent2KHR(N), then drains the timing
 *    queue for N's FIRST_PIXEL_VISIBLE timestamp.
 *  - vblankOccurred(timestamp) is emitted via Qt::QueuedConnection back to the
 *    main thread, which calls OutputFrame::presented() with a real past time.
 *
 * The arm() override required by the VsyncMonitor base is a no-op — this
 * monitor is presentId-driven; the X11 backend calls armWithPresentId() instead.
 */
class KWIN_EXPORT VulkanPresentTimingMonitor : public VsyncMonitor
{
    Q_OBJECT

public:
    /**
     * @brief Construct a monitor for @p swapchain. Returns nullptr if the
     * backend does not advertise both VK_EXT_present_timing and
     * VK_KHR_present_wait2.
     */
    static std::unique_ptr<VulkanPresentTimingMonitor> create(VulkanBackend *backend,
                                                              VulkanSwapchain *swapchain);

    ~VulkanPresentTimingMonitor() override;

    /** @brief VsyncMonitor contract: no-op. Use armWithPresentId() instead. */
    void arm() override
    {
    }

    /** @brief Queue an async wait on @p presentId on the helper thread. */
    void armWithPresentId(uint64_t presentId);

Q_SIGNALS:
    /** @brief Forwarded from VulkanPresentTimingMonitorHelper. */
    void presentTimingsReady(uint64_t presentId,
                             std::chrono::nanoseconds queueOperationsEnd,
                             std::chrono::nanoseconds firstPixelOut,
                             std::chrono::nanoseconds firstPixelVisible);

private:
    explicit VulkanPresentTimingMonitor(VkDevice device,
                                        VkSwapchainKHR swapchain,
                                        PFN_vkWaitForPresent2KHR waitFn,
                                        PFN_vkGetPastPresentationTimingEXT pastTimingFn);

    QThread m_thread;
    VulkanPresentTimingMonitorHelper m_helper;
};

} // namespace KWin
