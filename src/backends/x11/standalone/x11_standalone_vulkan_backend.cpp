/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "x11_standalone_vulkan_backend.h"
#include "compositor.h"
#include "core/outputbackend.h"
#include "core/overlaywindow.h"
#include "core/renderloop.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkanpresenttimingmonitor.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "platformsupport/scenes/vulkan/vulkansurfacetexture_x11.h"
#include "scene/itemrenderer.h"
#include "scene/surfaceitem_x11.h"
#include "scene/workspacescene.h"
#include "utils/softwarevsyncmonitor.h"
#include "utils/vsyncmonitor.h"
#include "utils/xcbutils.h"
#include "workspace.h"
#include "x11_standalone_backend.h"
#include "x11_standalone_logging.h"
#include "x11_standalone_omlsynccontrolvsyncmonitor.h"
#include "x11_standalone_overlaywindow.h"
#include "x11_standalone_sgivideosyncvsyncmonitor.h"

#include <QOpenGLContext>
#include <private/qtx11extras_p.h>

#include <vulkan/vulkan_xcb.h>

#include <algorithm>
#include <chrono>
#include <unistd.h>

namespace KWin
{

VulkanLayer::VulkanLayer(KWin::VulkanBackend *backend)
    : OutputLayer(nullptr)
    , m_backend(backend)
{
}

std::optional<OutputLayerBeginFrameInfo> VulkanLayer::doBeginFrame()
{
    // Get the context
    if (!m_backend->vulkanContext()) {
        qCWarning(KWIN_X11STANDALONE) << "VulkanLayer::doBeginFrame() - no context";
        return std::nullopt;
    }

    // Cast backend to X11StandaloneVulkanBackend to access swapchain
    auto *x11Backend = static_cast<X11StandaloneVulkanBackend *>(m_backend);
    auto *swapchain = x11Backend->swapchain();
    if (!swapchain || !swapchain->isValid()) {
        qCWarning(KWIN_X11STANDALONE) << "VulkanLayer::doBeginFrame() - no swapchain";
        return std::nullopt;
    }

    // VK_SUBOPTIMAL_KHR from a previous acquire or present is technically a
    // success — the spec lets us proceed with the suboptimal swapchain — but
    // every subsequent frame will keep flagging suboptimal until the
    // swapchain is rebuilt against the new surface. Without acting on the
    // flag here that turns into a recreation-loop log spam after a display
    // reconfiguration. Recreate up front; if recreation fails just skip the
    // frame and try again next time.
    if (swapchain->needsRecreation()) {
        if (!x11Backend->recreateSwapchainIfNeeded()) {
            return std::nullopt;
        }
        swapchain = x11Backend->swapchain();
        if (!swapchain || !swapchain->isValid()) {
            return std::nullopt;
        }
    }

    // Wait for the previous frame's fence to complete (CPU wait is still needed
    // to ensure the command buffer from the previous frame is not in use).
    // Reset is deferred until after a successful acquire: if we reset before
    // acquiring and the acquire fails, the fence stays unsignaled and the next
    // waitForFence() will block forever (UINT64_MAX timeout → freeze).
    swapchain->waitForFence();

    // Acquire the next swapchain image
    // This signals imageAvailableSemaphore when the image is ready
    uint32_t imageIndex = swapchain->acquireNextImage();
    if (imageIndex == UINT32_MAX) {
        // Swapchain needs recreation (resize, etc.)
        if (swapchain->needsRecreation()) {
            qCDebug(KWIN_X11STANDALONE) << "Swapchain needs recreation";
            // Try to recreate - for now just fail this frame
            return std::nullopt;
        }
        qCWarning(KWIN_X11STANDALONE) << "VulkanLayer::doBeginFrame() - failed to acquire image";
        return std::nullopt;
    }

    // Reset only after a successful acquire, so a failed acquire leaves the
    // fence signaled and the next frame's wait passes immediately.
    swapchain->resetFence();

    // Get the framebuffer for this image
    auto *framebuffer = swapchain->currentFramebuffer();
    if (!framebuffer) {
        qCWarning(KWIN_X11STANDALONE) << "VulkanLayer::doBeginFrame() - no framebuffer";
        return std::nullopt;
    }

    // Create a VulkanRenderTarget with the framebuffer
    auto vulkanRenderTarget = std::make_unique<VulkanRenderTarget>(framebuffer);

    // Set up GPU-GPU synchronization info
    // The renderer will:
    // - Wait on imageAvailableSemaphore before writing to the framebuffer
    // - Signal renderFinishedSemaphore when rendering is complete
    // - Signal inFlightFence when the command buffer is submitted
    VulkanSyncInfo syncInfo;
    syncInfo.imageAvailableSemaphore = swapchain->imageAvailableSemaphore();
    syncInfo.renderFinishedSemaphore = swapchain->renderFinishedSemaphore();
    syncInfo.inFlightFence = swapchain->inFlightFence();
    syncInfo.frameIndex = swapchain->currentFrame();
    vulkanRenderTarget->setSyncInfo(syncInfo);

    // Create a RenderTarget with the VulkanRenderTarget
    RenderTarget renderTarget(vulkanRenderTarget.release());

    // Open a render-time measurement for this frame. Must be closed and
    // attached to the OutputFrame in doEndFrame() — without it RenderJournal
    // is fed zeros and the RenderLoop scheduler under-budgets every frame.
    x11Backend->beginRenderTimeQuery();

    // Buffer-age repaint region: how stale this swapchain image's contents are
    // determines how much must be redrawn. infiniteRegion() => full repaint.
    return OutputLayerBeginFrameInfo{
        .renderTarget = std::move(renderTarget),
        .repaint = x11Backend->bufferDamage(imageIndex),
    };
}

bool VulkanLayer::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    Q_UNUSED(renderedRegion);

    auto *x11Backend = static_cast<X11StandaloneVulkanBackend *>(m_backend);

    // Close the render-time measurement opened in doBeginFrame() and hand it
    // to the OutputFrame so RenderLoopPrivate::notifyFrameCompleted() can feed
    // RenderJournal a real duration instead of zero.
    x11Backend->endAndAttachRenderTimeQuery(frame);

    // Feed this frame's damage into the backend's manual buffer-age tracking so
    // subsequent frames can compute a correct partial-repaint region.
    x11Backend->recordFrameDamage(damagedRegion);

    // Rendering has been submitted by the ItemRendererVulkan
    // The actual presentation happens in X11StandaloneVulkanBackend::present()
    return true;
}

DrmDevice *VulkanLayer::scanoutDevice() const
{
    return nullptr;
}

QHash<uint32_t, QList<uint64_t>> VulkanLayer::supportedDrmFormats() const
{
    return {};
}

X11StandaloneVulkanBackend::X11StandaloneVulkanBackend(X11StandaloneBackend *backend)
    : m_backend(backend)
{
    m_overlayWindow = std::make_unique<OverlayWindowX11>(backend);
    m_layer = std::make_unique<VulkanLayer>(this);

    // Damage-driven partial repaint is on by default; KWIN_VULKAN_PARTIAL_REPAINT=0
    // forces the full-repaint path.
    m_partialRepaint = !(qEnvironmentVariableIsSet("KWIN_VULKAN_PARTIAL_REPAINT")
                         && qEnvironmentVariableIntValue("KWIN_VULKAN_PARTIAL_REPAINT") == 0);
    qCWarning(KWIN_X11STANDALONE) << "Vulkan: damage-driven partial repaint"
                                  << (m_partialRepaint ? "enabled" : "disabled");

    // Note: We can't connect to workspace() here as it might not be available yet
    // This connection should be made after workspace is initialized
}

X11StandaloneVulkanBackend::~X11StandaloneVulkanBackend()
{
    // Stop the present-timing monitor first: its helper thread may be blocked
    // in vkWaitForPresent2KHR against m_swapchain, and that handle must outlive
    // the wait. Destroying the monitor joins the helper thread.
    m_presentTimingMonitor.reset();

    // Stop the vsync monitor next so no vblank() callback fires during teardown.
    m_vsyncMonitor.reset();

    if (isFailed()) {
        m_overlayWindow->destroy();
    }

    // Destruction order is important:
    // 1. Swapchain (holds framebuffers, image views, sync objects)
    // 2. Context (holds VMA allocator, command/descriptor pools)
    // 3. Surface (needs instance)
    // 4. X11 resources
    // 5. Base class handles device and instance destruction
    m_swapchain.reset();
    m_context.reset();

    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }

    if (m_colormap != XCB_COLORMAP_NONE) {
        xcb_free_colormap(connection(), m_colormap);
        m_colormap = XCB_COLORMAP_NONE;
    }

    if (m_window) {
        xcb_destroy_window(connection(), m_window);
    }

    m_overlayWindow->destroy();
}

void X11StandaloneVulkanBackend::init()
{
    if (!initInstance()) {
        setFailed(QStringLiteral("Failed to initialize Vulkan instance"));
        return;
    }

    if (!initPhysicalDevice()) {
        setFailed(QStringLiteral("Failed to initialize Vulkan physical device"));
        return;
    }

    if (!initDevice()) {
        setFailed(QStringLiteral("Failed to initialize Vulkan device"));
        return;
    }

    m_context = std::make_unique<KWin::VulkanContext>(this);
    if (!m_context->isValid()) {
        setFailed(QStringLiteral("Failed to initialize Vulkan context"));
        return;
    }

    qCDebug(KWIN_X11STANDALONE) << "Successfully created Vulkan context";

    // Create overlay window first - we need the X11 window for the Vulkan surface
    if (!initOverlayWindow()) {
        setFailed(QStringLiteral("Failed to initialize overlay window"));
        return;
    }

    // Now create the Vulkan surface using the overlay window
    if (!initSurface()) {
        setFailed(QStringLiteral("Failed to initialize Vulkan surface"));
        return;
    }

    // Decide the presentation-feedback path: VK_EXT_present_timing when the device
    // and surface both support it (and it is not disabled), else the VsyncMonitor.
    // Must run before initSwapchain() — the swapchain needs the timing create flag.
    detectPresentTimingSupport();
    qCWarning(KWIN_X11STANDALONE) << "Vulkan presentation feedback:"
                                  << (m_presentTimingActive ? "VK_EXT_present_timing" : "VsyncMonitor");

    // Finally create the swapchain
    if (!initSwapchain()) {
        setFailed(QStringLiteral("Failed to initialize Vulkan swapchain"));
        return;
    }

    // Connect to workspace geometry changes for resize handling
    connect(workspace(), &Workspace::geometryChanged, this, &X11StandaloneVulkanBackend::screenGeometryChanged);

    // Real presentation-timing feedback for the RenderLoop. The VsyncMonitor is
    // the fallback — when VK_EXT_present_timing + VK_KHR_present_wait2 are
    // active, the async monitor created in setupPresentTimingQueue() (called
    // from initSwapchain()) reports timestamps instead. Keep a VsyncMonitor on
    // standby anyway so handlePresentTimingMonitorError() can hand off cleanly
    // when the EXT path drops out — its arm() is a no-op until that happens.
    if (!m_presentTimingActive) {
        initVsyncMonitor();
    }

    qCDebug(KWIN_X11STANDALONE) << "Successfully initialized Vulkan backend";
}

void X11StandaloneVulkanBackend::initVsyncMonitor()
{
    // Same monitor cascade as the GLX backend: prefer a hardware vblank source,
    // fall back to a software (timer-based) one. Each monitor is self-contained
    // (its own X11 connection / thread), so no GL or Vulkan context is needed.
    m_vsyncMonitor = SGIVideoSyncVsyncMonitor::create();
    if (!m_vsyncMonitor) {
        m_vsyncMonitor = OMLSyncControlVsyncMonitor::create();
    }
    if (!m_vsyncMonitor) {
        std::unique_ptr<SoftwareVsyncMonitor> monitor = SoftwareVsyncMonitor::create();
        if (monitor) {
            RenderLoop *renderLoop = m_backend->renderLoop();
            monitor->setRefreshRate(renderLoop->refreshRate());
            connect(renderLoop, &RenderLoop::refreshRateChanged, this, [this, m = monitor.get()]() {
                m->setRefreshRate(m_backend->renderLoop()->refreshRate());
            });
            m_vsyncMonitor = std::move(monitor);
        }
    }

    if (m_vsyncMonitor) {
        connect(m_vsyncMonitor.get(), &VsyncMonitor::vblankOccurred, this, &X11StandaloneVulkanBackend::vblank);
    } else {
        qCWarning(KWIN_X11STANDALONE) << "Vulkan: no vsync monitor available; presentation timing will be approximate";
    }
}

void X11StandaloneVulkanBackend::vblank(std::chrono::nanoseconds timestamp)
{
    if (m_frame) {
        m_frame->presented(timestamp, PresentationMode::VSync);
        m_frame.reset();
    }
}

// Depth of the swapchain's VK_EXT_present_timing result queue. The monitor's
// helper thread drains the entry matching the awaited presentId after each
// vkWaitForPresent2KHR unblock, so only a few entries are ever pending; this is
// generous headroom.
static constexpr uint32_t kPresentTimingQueueSize = 16;

static std::chrono::nanoseconds monotonicNow()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

void X11StandaloneVulkanBackend::detectPresentTimingSupport()
{
    setPresentTimingEnabled(false);
    m_presentTimingActive = false;

    // Device-level support (extensions + features) was established in
    // createDevice(); honour the env override and require surface support too.
    // The present-wait2 device extension is required: without it the monitor
    // would have to busy-poll vkGetPastPresentationTimingEXT, which defeats the
    // point — fall back to VsyncMonitor in that case.
    const bool disabled = qEnvironmentVariableIsSet("KWIN_VULKAN_PRESENT_TIMING")
        && qEnvironmentVariableIntValue("KWIN_VULKAN_PRESENT_TIMING") == 0;
    if (!supportsPresentTiming() || !supportsPresentWait2() || disabled || m_surface == VK_NULL_HANDLE) {
        return;
    }

    // VK_EXT_present_timing also requires the surface to support present timing
    // and VK_KHR_present_id2; VK_KHR_present_wait2 likewise needs surface support.
    // Query them all via vkGetPhysicalDeviceSurfaceCapabilities2KHR.
    VkSurfaceCapabilitiesPresentWait2KHR wait2Caps{};
    wait2Caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR;
    VkSurfaceCapabilitiesPresentId2KHR id2Caps{};
    id2Caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
    id2Caps.pNext = &wait2Caps;
    VkPresentTimingSurfaceCapabilitiesEXT timingCaps{};
    timingCaps.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    timingCaps.pNext = &id2Caps;
    VkSurfaceCapabilities2KHR caps2{};
    caps2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    caps2.pNext = &timingCaps;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceInfo.surface = m_surface;

    const VkResult r = vkGetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice(), &surfaceInfo, &caps2);
    qCWarning(KWIN_X11STANDALONE).nospace()
        << "VK_EXT_present_timing surface check: result=" << int(r)
        << " presentTimingSupported=" << bool(timingCaps.presentTimingSupported)
        << " presentId2Supported=" << bool(id2Caps.presentId2Supported)
        << " presentWait2Supported=" << bool(wait2Caps.presentWait2Supported)
        << " supportedStages=0x" << QString::number(timingCaps.presentStageQueries, 16);

    if (r != VK_SUCCESS || !timingCaps.presentTimingSupported || !id2Caps.presentId2Supported
        || !wait2Caps.presentWait2Supported) {
        return;
    }

    // Request timing for the latest pipeline stage the surface supports — the one
    // closest to "actually on screen". QUEUE_OPERATIONS_END is always supported.
    VkPresentStageFlagsEXT stages = timingCaps.presentStageQueries
        & (VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT
           | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
           | VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT);
    if (stages == 0) {
        stages = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
    }
    setPresentTimingStages(stages);
    setPresentTimingEnabled(true);
    m_presentTimingActive = true;
}

void X11StandaloneVulkanBackend::setupPresentTimingQueue()
{
    // Tear down any previous monitor before its bound VkSwapchainKHR becomes
    // stale. Destruction joins the helper thread; the worker may be blocked in
    // vkWaitForPresent2KHR but will unblock at the timeout if the swapchain
    // teardown hasn't already caused it to return VK_ERROR_OUT_OF_DATE_KHR.
    m_presentTimingMonitor.reset();

    if (!m_presentTimingActive || !m_swapchain || !m_swapchain->isValid()) {
        return;
    }
    if (auto fn = setSwapchainPresentTimingQueueSizeEXT()) {
        const VkResult r = fn(device(), m_swapchain->swapchain(), kPresentTimingQueueSize);
        if (r != VK_SUCCESS) {
            qCWarning(KWIN_X11STANDALONE) << "vkSetSwapchainPresentTimingQueueSizeEXT failed:" << r;
        }
    }

    // Pick the time domain for VkPresentTimingInfoEXT.timeDomainId — prefer
    // CLOCK_MONOTONIC, the clock RenderLoop (and steady_clock) uses.
    setPresentTimeDomainId(0);
    if (auto domainsFn = getSwapchainTimeDomainPropertiesEXT()) {
        VkSwapchainTimeDomainPropertiesEXT props{};
        props.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
        if (domainsFn(device(), m_swapchain->swapchain(), &props, nullptr) == VK_SUCCESS
            && props.timeDomainCount > 0) {
            std::vector<VkTimeDomainKHR> domains(props.timeDomainCount);
            std::vector<uint64_t> ids(props.timeDomainCount);
            props.pTimeDomains = domains.data();
            props.pTimeDomainIds = ids.data();
            if (domainsFn(device(), m_swapchain->swapchain(), &props, nullptr) == VK_SUCCESS) {
                uint64_t chosen = ids.front();
                for (uint32_t i = 0; i < props.timeDomainCount; ++i) {
                    if (domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) {
                        chosen = ids[i];
                        break;
                    }
                }
                setPresentTimeDomainId(chosen);
            }
        }
    }

    // Bring up the async monitor against the current swapchain handle. If
    // creation fails for any reason, fall through to the VsyncMonitor path —
    // detectPresentTimingSupport() already verified the extensions, so this
    // should not normally happen.
    m_presentTimingMonitor = VulkanPresentTimingMonitor::create(this, m_swapchain.get());
    if (m_presentTimingMonitor) {
        connect(m_presentTimingMonitor.get(), &VsyncMonitor::vblankOccurred,
                this, &X11StandaloneVulkanBackend::vblank);
        connect(m_presentTimingMonitor.get(), &VsyncMonitor::errorOccurred,
                this, &X11StandaloneVulkanBackend::handlePresentTimingMonitorError);
    } else {
        qCWarning(KWIN_X11STANDALONE) << "Failed to create VulkanPresentTimingMonitor; falling back to VsyncMonitor";
        m_presentTimingActive = false;
        if (!m_vsyncMonitor) {
            initVsyncMonitor();
        }
    }
}

void X11StandaloneVulkanBackend::handlePresentTimingMonitorError()
{
    // The helper signalled an unrecoverable error (typically the swapchain
    // going out-of-date). Tear it down here on the main thread and let the
    // next swapchain recreation rebuild it. Until then, fall back to the
    // VsyncMonitor cascade so presentation feedback keeps flowing.
    qCWarning(KWIN_X11STANDALONE) << "Vulkan present-timing monitor reported an error; falling back to VsyncMonitor";
    m_presentTimingMonitor.reset();
    if (!m_vsyncMonitor) {
        initVsyncMonitor();
    }
}

bool X11StandaloneVulkanBackend::initInstance()
{
    // Required extensions for X11 surface
    QList<const char *> requiredExtensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XCB_SURFACE_EXTENSION_NAME,
        // Lets detectPresentTimingSupport() query VkPresentTimingSurfaceCapabilitiesEXT.
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};

    return createInstance(requiredExtensions);
}

bool X11StandaloneVulkanBackend::initPhysicalDevice()
{
    return selectPhysicalDevice();
}

bool X11StandaloneVulkanBackend::initDevice()
{
    // Required device extensions
    QList<const char *> requiredDeviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};

    return createDevice(requiredDeviceExtensions);
}

bool X11StandaloneVulkanBackend::initSurface()
{
    VkXcbSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    createInfo.connection = connection();
    createInfo.window = m_window;

    VkResult result = vkCreateXcbSurfaceKHR(instance(), &createInfo, nullptr, &m_surface);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_X11STANDALONE) << "Failed to create Vulkan surface:" << result;
        return false;
    }

    qCDebug(KWIN_X11STANDALONE) << "Successfully created Vulkan surface";

    // Check if the surface is supported by the selected physical device
    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice(), graphicsQueueFamily(), m_surface, &presentSupport);

    if (!presentSupport) {
        qCWarning(KWIN_X11STANDALONE) << "Vulkan surface is not supported by the physical device";
        return false;
    }

    return true;
}

bool X11StandaloneVulkanBackend::initSwapchain()
{
    if (!m_context) {
        return false;
    }

    const QSize size = workspace()->geometry().size();
    m_swapchain = VulkanSwapchain::create(m_context.get(), m_surface, size);
    if (!m_swapchain || !m_swapchain->isValid()) {
        qCWarning(KWIN_X11STANDALONE) << "Failed to create Vulkan swapchain";
        return false;
    }

    qCDebug(KWIN_X11STANDALONE) << "Successfully created Vulkan swapchain";

    m_colorFormat = m_swapchain->format();

    // Connect the swapchain's render pass to the pipeline manager
    m_context->pipelineManager()->setRenderPass(m_swapchain->renderPass()->renderPass());

    // Fresh swapchain images carry no usable previous contents.
    resetDamageTracking();
    setupPresentTimingQueue();

    return true;
}

bool X11StandaloneVulkanBackend::initOverlayWindow()
{
    if (overlayWindow()->create()) {
        xcb_connection_t *const c = connection();

        // Create colormap
        xcb_visualid_t visual = Xcb::defaultScreen()->root_visual;
        m_colormap = xcb_generate_id(c);
        xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, m_colormap, rootWindow(), visual);

        // Create window
        const QSize size = workspace()->geometry().size();
        m_window = xcb_generate_id(c);
        xcb_create_window(c, Xcb::defaultDepth(), m_window, overlayWindow()->window(),
                          0, 0, size.width(), size.height(), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          visual, XCB_CW_COLORMAP, &m_colormap);

        // Resize overlay BEFORE setup - this sets m_size which is needed for input shape
        overlayWindow()->resize(size);
        overlayWindow()->setup(m_window);
        qCDebug(KWIN_X11STANDALONE) << "Successfully created and setup overlay window";
        return true;
    } else {
        qCCritical(KWIN_X11STANDALONE) << "Failed to create overlay window";
        return false;
    }
}

std::unique_ptr<SurfaceTexture> X11StandaloneVulkanBackend::createSurfaceTextureX11(SurfacePixmapX11 *pixmap)
{
    return std::make_unique<X11StandaloneVulkanSurfaceTexture>(this, pixmap);
}

bool X11StandaloneVulkanBackend::present(Output *output, const std::shared_ptr<OutputFrame> &frame)
{
    Q_UNUSED(output);
    m_frame = frame;

    if (!m_swapchain || !m_swapchain->isValid()) {
        qCWarning(KWIN_X11STANDALONE) << "present() - no valid swapchain";
        return false;
    }

    // Tag the present for VK_EXT_present_timing feedback when that path is active.
    // The monitor's worker thread will block on this presentId via
    // vkWaitForPresent2KHR and emit vblankOccurred() with its real on-screen
    // timestamp — same async shape as the VsyncMonitor, no extrapolation, no
    // future timestamps fed to RenderLoop.
    uint64_t presentId = 0;
    if (m_presentTimingActive && m_presentTimingMonitor) {
        presentId = m_nextPresentId++;
    }

    // Present the rendered frame to the swapchain, hinting the changed region
    // (VK_KHR_incremental_present). m_presentDamage is cleared afterwards so a
    // present not preceded by a doEndFrame() (e.g. a failed beginFrame) falls
    // back to a regionless full present.
    bool presentSuccess = m_swapchain->present(m_presentDamage, presentId);
    m_presentDamage = QRegion();

    if (presentSuccess) {
        if (m_presentTimingActive && m_presentTimingMonitor) {
            // Real present timestamp arrives asynchronously via vblank() once
            // the helper thread's vkWaitForPresent2KHR(presentId) returns.
            m_presentTimingMonitor->armWithPresentId(presentId);
        } else if (m_vsyncMonitor) {
            // Real present timestamp arrives asynchronously via vblank().
            m_vsyncMonitor->arm();
        } else if (m_frame) {
            // No feedback source — immediate approximate timestamp.
            m_frame->presented(monotonicNow(), PresentationMode::VSync);
            m_frame.reset();
        }
    } else {
        // Present failed, likely the swapchain needs recreation.
        qCWarning(KWIN_X11STANDALONE) << "Present failed, checking if swapchain needs recreation";
        recreateSwapchainIfNeeded();
        // No timing will be reported for a failed present; mark the frame
        // presented immediately so the RenderLoop is not left waiting.
        if (m_frame) {
            m_frame->presented(monotonicNow(), PresentationMode::VSync);
            m_frame.reset();
        }
    }

    // Advance to the next frame in the swapchain
    m_swapchain->advanceFrame();

    // Show the overlay window
    if (overlayWindow()->window()) {
        qCDebug(KWIN_X11STANDALONE) << "Showing overlay window";
        overlayWindow()->show();
    } else {
        qCWarning(KWIN_X11STANDALONE) << "Overlay window is not valid, cannot show";
    }

    return presentSuccess;
}

bool X11StandaloneVulkanBackend::makeCurrent()
{
    // Vulkan doesn't have the concept of "making current" like OpenGL
    // but we can ensure the context is ready for use
    return m_context && m_context->isValid();
}

void X11StandaloneVulkanBackend::doneCurrent()
{
    // Vulkan doesn't have the concept of "making current" like OpenGL
    // This is a no-op
}

KWin::VulkanContext *X11StandaloneVulkanBackend::vulkanContext() const
{
    return m_context.get();
}

OverlayWindow *X11StandaloneVulkanBackend::overlayWindow() const
{
    return m_overlayWindow.get();
}

OutputLayer *X11StandaloneVulkanBackend::primaryLayer(Output *output)
{
    return m_layer.get();
}

void X11StandaloneVulkanBackend::screenGeometryChanged()
{
    const QSize size = workspace()->geometry().size();
    qCDebug(KWIN_X11STANDALONE) << "Vulkan: screen geometry changed, new size:" << size;
    doneCurrent();

    if (m_window) {
        const uint16_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        const uint32_t values[] = {uint32_t(size.width()), uint32_t(size.height())};
        xcb_configure_window(connection(), m_window, mask, values);
    }

    overlayWindow()->resize(size);
    Xcb::sync();
}

void X11StandaloneVulkanBackend::resetDamageTracking()
{
    m_damageJournal.clear();
    m_frameCounter = 1;
    m_imageLastRenderFrame.assign(m_swapchain ? m_swapchain->imageCount() : 0, 0);
}

bool X11StandaloneVulkanBackend::recreateSwapchainIfNeeded()
{
    if (!m_swapchain || !m_swapchain->needsRecreation()) {
        return true;
    }
    qCDebug(KWIN_X11STANDALONE) << "Swapchain needs recreation; rebuilding";
    const QSize size = workspace()->geometry().size();
    if (!m_swapchain->recreate(size)) {
        qCWarning(KWIN_X11STANDALONE) << "Failed to recreate swapchain";
        return false;
    }
    // Recreated images start blank — drop stale buffer-age tracking.
    resetDamageTracking();
    // The new swapchain restarts present-id numbering and its EXT_present
    // _timing queue is empty. Rebuild the queue and the async monitor against
    // the new swapchain handle. setupPresentTimingQueue() resets the monitor
    // before recreating it, so the helper thread is joined before the old
    // VkSwapchainKHR is destroyed.
    setupPresentTimingQueue();
    return true;
}

QRegion X11StandaloneVulkanBackend::bufferDamage(uint32_t imageIndex) const
{
    if (!m_partialRepaint || imageIndex >= m_imageLastRenderFrame.size()) {
        return infiniteRegion();
    }
    // Fullscreen post-passes (invert, colorblindness correction) resample the
    // entire framebuffer, so the scene must paint every pixel each frame.
    // Returning a sub-screen region here would leave undamaged pixels at the
    // main pass's clear value, which the post-pass would then process as if it
    // were the scene — manifesting as black/white flicker over the static parts
    // of the screen. Force a full repaint while any post-pass is registered.
    if (auto *compositor = Compositor::self()) {
        if (auto *scene = compositor->scene()) {
            if (auto *renderer = scene->renderer(); renderer && renderer->hasFullscreenPostPasses()) {
                return infiniteRegion();
            }
        }
    }
    const uint64_t last = m_imageLastRenderFrame[imageIndex];
    if (last == 0 || last >= m_frameCounter) {
        // This image has not been rendered into yet (or the counter was just
        // reset): its contents are unusable, so repaint everything.
        return infiniteRegion();
    }
    // Age = frames elapsed since this image last held a rendered result. Its
    // contents are that stale, so repaint everything damaged since then.
    const int age = static_cast<int>(m_frameCounter - last);
    return m_damageJournal.accumulate(age, infiniteRegion());
}

void X11StandaloneVulkanBackend::recordFrameDamage(const QRegion &damage)
{
    // Stashed for the next present()'s VK_KHR_incremental_present hint,
    // regardless of whether damage-driven partial repaint is enabled.
    m_presentDamage = damage;

    if (!m_partialRepaint) {
        return;
    }
    m_damageJournal.add(damage);
    if (m_swapchain) {
        const uint32_t imageIndex = m_swapchain->currentImageIndex();
        if (imageIndex < m_imageLastRenderFrame.size()) {
            m_imageLastRenderFrame[imageIndex] = m_frameCounter;
        }
    }
    ++m_frameCounter;
}

void X11StandaloneVulkanBackend::beginRenderTimeQuery()
{
    // CpuRenderTimeQuery starts the clock at construction; nothing else to do
    // here. Overwriting any prior value is safe: doBeginFrame/doEndFrame pair
    // 1:1 today, but if a previous frame somehow failed to attach its query
    // (e.g. doEndFrame skipped), discarding it is correct — the unattached
    // query has no observer and the new frame's measurement is what we want.
    m_renderTimeQuery = std::make_unique<CpuRenderTimeQuery>();
}

void X11StandaloneVulkanBackend::endAndAttachRenderTimeQuery(OutputFrame *frame)
{
    if (!m_renderTimeQuery || !frame) {
        return;
    }
    m_renderTimeQuery->end();
    frame->addRenderTimeQuery(std::move(m_renderTimeQuery));
}

xcb_connection_t *X11StandaloneVulkanBackend::connection() const
{
    return m_backend->connection();
}

X11StandaloneVulkanSurfaceTexture::X11StandaloneVulkanSurfaceTexture(X11StandaloneVulkanBackend *backend, SurfacePixmapX11 *pixmap)
    : KWin::VulkanSurfaceTextureX11(backend, pixmap)
    , m_backend(backend)
{
}

bool X11StandaloneVulkanSurfaceTexture::create()
{
    // Delegate to the base implementation
    return KWin::VulkanSurfaceTextureX11::create();
}

void X11StandaloneVulkanSurfaceTexture::update(const QRegion &region)
{
    // Delegate to the base implementation
    KWin::VulkanSurfaceTextureX11::update(region);
}

} // namespace KWin

#include "moc_x11_standalone_vulkan_backend.cpp"
