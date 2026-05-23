/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vulkanbackend.h"
#include "utils/common.h"
#include "vulkancontext.h"
#include "vulkanframebuffer.h"
#include "vulkantexture.h"

#include <QDebug>
#include <QVulkanInstance>
#include <cstring>
#include <vector>

namespace KWin
{

static QString getVulkanResultString(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return QStringLiteral("VK_SUCCESS");
    case VK_NOT_READY:
        return QStringLiteral("VK_NOT_READY");
    case VK_TIMEOUT:
        return QStringLiteral("VK_TIMEOUT");
    case VK_EVENT_SET:
        return QStringLiteral("VK_EVENT_SET");
    case VK_EVENT_RESET:
        return QStringLiteral("VK_EVENT_RESET");
    case VK_INCOMPLETE:
        return QStringLiteral("VK_INCOMPLETE");
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return QStringLiteral("VK_ERROR_OUT_OF_HOST_MEMORY");
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return QStringLiteral("VK_ERROR_OUT_OF_DEVICE_MEMORY");
    case VK_ERROR_INITIALIZATION_FAILED:
        return QStringLiteral("VK_ERROR_INITIALIZATION_FAILED");
    case VK_ERROR_DEVICE_LOST:
        return QStringLiteral("VK_ERROR_DEVICE_LOST");
    case VK_ERROR_MEMORY_MAP_FAILED:
        return QStringLiteral("VK_ERROR_MEMORY_MAP_FAILED");
    case VK_ERROR_LAYER_NOT_PRESENT:
        return QStringLiteral("VK_ERROR_LAYER_NOT_PRESENT");
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return QStringLiteral("VK_ERROR_EXTENSION_NOT_PRESENT");
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return QStringLiteral("VK_ERROR_FEATURE_NOT_PRESENT");
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return QStringLiteral("VK_ERROR_INCOMPATIBLE_DRIVER");
    case VK_ERROR_TOO_MANY_OBJECTS:
        return QStringLiteral("VK_ERROR_TOO_MANY_OBJECTS");
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return QStringLiteral("VK_ERROR_FORMAT_NOT_SUPPORTED");
    case VK_ERROR_FRAGMENTED_POOL:
        return QStringLiteral("VK_ERROR_FRAGMENTED_POOL");
    case VK_ERROR_UNKNOWN:
        return QStringLiteral("VK_ERROR_UNKNOWN");
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return QStringLiteral("VK_ERROR_OUT_OF_POOL_MEMORY");
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return QStringLiteral("VK_ERROR_INVALID_EXTERNAL_HANDLE");
    case VK_ERROR_FRAGMENTATION:
        return QStringLiteral("VK_ERROR_FRAGMENTATION");
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
        return QStringLiteral("VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS");
    case VK_ERROR_SURFACE_LOST_KHR:
        return QStringLiteral("VK_ERROR_SURFACE_LOST_KHR");
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return QStringLiteral("VK_ERROR_NATIVE_WINDOW_IN_USE_KHR");
    case VK_SUBOPTIMAL_KHR:
        return QStringLiteral("VK_SUBOPTIMAL_KHR");
    case VK_ERROR_OUT_OF_DATE_KHR:
        return QStringLiteral("VK_ERROR_OUT_OF_DATE_KHR");
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
        return QStringLiteral("VK_ERROR_INCOMPATIBLE_DISPLAY_KHR");
    case VK_ERROR_VALIDATION_FAILED_EXT:
        return QStringLiteral("VK_ERROR_VALIDATION_FAILED_EXT");
    case VK_ERROR_INVALID_SHADER_NV:
        return QStringLiteral("VK_ERROR_INVALID_SHADER_NV");
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
        return QStringLiteral("VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT");
    case VK_ERROR_NOT_PERMITTED_EXT:
        return QStringLiteral("VK_ERROR_NOT_PERMITTED_EXT");
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        return QStringLiteral("VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT");
    case VK_THREAD_IDLE_KHR:
        return QStringLiteral("VK_THREAD_IDLE_KHR");
    case VK_THREAD_DONE_KHR:
        return QStringLiteral("VK_THREAD_DONE_KHR");
    case VK_OPERATION_DEFERRED_KHR:
        return QStringLiteral("VK_OPERATION_DEFERRED_KHR");
    case VK_OPERATION_NOT_DEFERRED_KHR:
        return QStringLiteral("VK_OPERATION_NOT_DEFERRED_KHR");
    case VK_PIPELINE_COMPILE_REQUIRED_EXT:
        return QStringLiteral("VK_PIPELINE_COMPILE_REQUIRED_EXT");
    default:
        return QString("Unknown VkResult: %1").arg(result);
    }
}

#ifndef NDEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData)
{
    // List of "False Positive" VUIDs to ignore
    static const char *ignored_vuids[] = {
        "VUID-VkImageCreateInfo-pNext-00990",
        "VUID-VkImportMemoryWin32HandleInfoKHR-handleType-09861"};

    // Check if this message ID should be ignored
    if (pCallbackData->pMessageIdName) {
        for (const char *id : ignored_vuids) {
            if (strcmp(pCallbackData->pMessageIdName, id) == 0) {
                return VK_FALSE; // Skip this warning/error
            }
        }
    }

    switch (messageSeverity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        qCDebug(KWIN_VULKAN) << "Vulkan validation (verbose):" << pCallbackData->pMessage;
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        qCInfo(KWIN_VULKAN) << "Vulkan validation (info):" << pCallbackData->pMessage;
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        qCWarning(KWIN_VULKAN) << "Vulkan validation (warning):" << pCallbackData->pMessage;
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        qCCritical(KWIN_VULKAN) << "Vulkan validation (error):" << pCallbackData->pMessage;
        break;
    default:
        qCDebug(KWIN_VULKAN) << "Vulkan validation:" << pCallbackData->pMessage;
        break;
    }
    return VK_FALSE;
}
#endif

VulkanBackend::VulkanBackend()
    : RenderBackend()
    , m_failed(false)
{
}

VulkanBackend::~VulkanBackend()
{
    cleanup();
}

CompositingType VulkanBackend::compositingType() const
{
    return VulkanCompositing;
}

bool VulkanBackend::checkGraphicsReset()
{
    // Check if device is lost
    if (m_device != VK_NULL_HANDLE) {
        VkResult result = vkDeviceWaitIdle(m_device);
        if (result == VK_ERROR_DEVICE_LOST) {
            qCWarning(KWIN_VULKAN) << "Vulkan device lost";
            return true;
        }
    }
    return false;
}

void VulkanBackend::setFailed(const QString &reason)
{
    qCWarning(KWIN_VULKAN) << "Creating Vulkan backend failed:" << reason;
    m_failed = true;
}

bool VulkanBackend::createInstance(const QList<const char *> &requiredExtensions)
{
    qCDebug(KWIN_VULKAN) << "Creating Vulkan instance with extensions:" << requiredExtensions;
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "KWin";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "KWin";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    // Convert QList to std::vector
    std::vector<const char *> extensions;
    for (const char *ext : requiredExtensions) {
        extensions.push_back(ext);
    }

#ifndef NDEBUG
    // Enable validation layers in debug builds
    const std::vector<const char *> validationLayers = {
        "VK_LAYER_KHRONOS_validation"};
    createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
    createInfo.ppEnabledLayerNames = validationLayers.data();

    // Add debug utils extension
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#else
    createInfo.enabledLayerCount = 0;
#endif

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS) {
        setFailed(QString("Failed to create Vulkan instance: %1 (VK_ERROR: %2)").arg(result).arg(getVulkanResultString(result)));
        return false;
    }

#ifndef NDEBUG
    // Set up debug messenger
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCreateInfo.pfnUserCallback = debugCallback;

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(m_instance, &debugCreateInfo, nullptr, &m_debugMessenger);
    }
#endif

    qCDebug(KWIN_VULKAN) << "Vulkan instance created successfully";
    return true;
}

bool VulkanBackend::selectPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        setFailed("Failed to find GPUs with Vulkan support");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    // Pick the first suitable device (could be improved with device scoring)
    for (const auto &device : devices) {
        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(device, &deviceProperties);

        VkPhysicalDeviceFeatures deviceFeatures;
        vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

        // Find graphics queue family
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_physicalDevice = device;
                m_graphicsQueueFamily = i;
                qCDebug(KWIN_VULKAN) << "Selected Vulkan device:" << deviceProperties.deviceName;
                return true;
            }
        }
    }

    setFailed("Failed to find a suitable GPU");
    return false;
}

bool VulkanBackend::createDevice(const QList<const char *> &requiredDeviceExtensions)
{
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;

    float queuePriority = 1.0f;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pEnabledFeatures = &deviceFeatures;

    // Convert QList to std::vector
    std::vector<const char *> extensions;
    for (const char *ext : requiredDeviceExtensions) {
        extensions.push_back(ext);
    }

    // Check for VK_KHR_external_fence_fd extension support
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, availableExtensions.data());

    bool hasExternalFenceFd = false;
    bool hasExternalFenceCapabilities = false;
    bool hasIncrementalPresent = false;
    bool hasPresentTiming = false;
    bool hasPresentId = false;
    bool hasPresentWait2 = false;
    for (const auto &ext : availableExtensions) {
        if (strcmp(ext.extensionName, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME) == 0) {
            hasExternalFenceFd = true;
        }
        if (strcmp(ext.extensionName, VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME) == 0) {
            hasExternalFenceCapabilities = true;
        }
        if (strcmp(ext.extensionName, VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME) == 0) {
            hasIncrementalPresent = true;
        }
        if (strcmp(ext.extensionName, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) == 0) {
            hasPresentTiming = true;
        }
        if (strcmp(ext.extensionName, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) == 0) {
            hasPresentId = true;
        }
        if (strcmp(ext.extensionName, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME) == 0) {
            hasPresentWait2 = true;
        }
    }

    // Enable external fence fd extension if available (requires external_fence_capabilities)
    if (hasExternalFenceFd && hasExternalFenceCapabilities) {
        extensions.push_back(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
        extensions.push_back(VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME);
        m_supportsExternalFenceFd = true;
        qCDebug(KWIN_VULKAN) << "VK_KHR_external_fence_fd extension enabled";
    }

    // Enable incremental present if available — lets present() hint changed regions.
    if (hasIncrementalPresent) {
        extensions.push_back(VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME);
        m_supportsIncrementalPresent = true;
        qCDebug(KWIN_VULKAN) << "VK_KHR_incremental_present extension enabled";
    }

    // Enable VK_EXT_present_timing (+ its VK_KHR_present_id2 dependency) for real
    // per-frame presentation feedback, and VK_KHR_present_wait2 so the async
    // monitor can block on a specific presentId without a busy poll. Each has
    // a device feature that must be queried and then enabled by chaining the
    // feature structs into device creation. The structs are declared at function
    // scope so they outlive vkCreateDevice below.
    VkPhysicalDevicePresentTimingFeaturesEXT presentTimingFeatures{};
    presentTimingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
    VkPhysicalDevicePresentId2FeaturesKHR presentIdFeatures{};
    presentIdFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    VkPhysicalDevicePresentWait2FeaturesKHR presentWait2Features{};
    presentWait2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR;
    if (hasPresentTiming && hasPresentId) {
        presentTimingFeatures.pNext = &presentIdFeatures;
        if (hasPresentWait2) {
            presentIdFeatures.pNext = &presentWait2Features;
        }
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &presentTimingFeatures;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

        if (presentTimingFeatures.presentTiming && presentIdFeatures.presentId2) {
            extensions.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
            extensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
            // Enable only what we use: presentTiming + presentId2. The optional
            // target-time features stay off (no present scheduling yet).
            presentTimingFeatures.presentAtAbsoluteTime = VK_FALSE;
            presentTimingFeatures.presentAtRelativeTime = VK_FALSE;
            presentIdFeatures.pNext = nullptr;
            presentTimingFeatures.pNext = &presentIdFeatures;
            createInfo.pNext = &presentTimingFeatures;
            m_supportsPresentTiming = true;
            qCDebug(KWIN_VULKAN) << "VK_EXT_present_timing extension enabled";

            // present_wait2 is optional: if available, chain it into the device
            // create. The async monitor needs it; absence forces a fallback to
            // VsyncMonitor.
            if (hasPresentWait2 && presentWait2Features.presentWait2) {
                extensions.push_back(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
                presentIdFeatures.pNext = &presentWait2Features;
                presentWait2Features.pNext = nullptr;
                m_supportsPresentWait2 = true;
                qCDebug(KWIN_VULKAN) << "VK_KHR_present_wait2 extension enabled";
            }
        }
    }

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

#ifndef NDEBUG
    const std::vector<const char *> validationLayers = {
        "VK_LAYER_KHRONOS_validation"};
    createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
    createInfo.ppEnabledLayerNames = validationLayers.data();
#else
    createInfo.enabledLayerCount = 0;
#endif

    VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
    if (result != VK_SUCCESS) {
        setFailed(QString("Failed to create logical device: %1 (%2)").arg(result).arg(getVulkanResultString(result)));
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);

    // Load extension function pointers
    if (m_supportsExternalFenceFd) {
        m_vkGetFenceFdKHR = reinterpret_cast<PFN_vkGetFenceFdKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetFenceFdKHR"));
        if (!m_vkGetFenceFdKHR) {
            qCWarning(KWIN_VULKAN) << "Failed to load vkGetFenceFdKHR, disabling external fence support";
            m_supportsExternalFenceFd = false;
        }
    }

    if (m_supportsPresentTiming) {
        m_vkSetSwapchainPresentTimingQueueSizeEXT = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(
            vkGetDeviceProcAddr(m_device, "vkSetSwapchainPresentTimingQueueSizeEXT"));
        m_vkGetPastPresentationTimingEXT = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(
            vkGetDeviceProcAddr(m_device, "vkGetPastPresentationTimingEXT"));
        m_vkGetSwapchainTimeDomainPropertiesEXT = reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
            vkGetDeviceProcAddr(m_device, "vkGetSwapchainTimeDomainPropertiesEXT"));
        if (!m_vkSetSwapchainPresentTimingQueueSizeEXT || !m_vkGetPastPresentationTimingEXT
            || !m_vkGetSwapchainTimeDomainPropertiesEXT) {
            qCWarning(KWIN_VULKAN) << "Failed to load VK_EXT_present_timing functions, disabling present timing";
            m_supportsPresentTiming = false;
        }
    }

    if (m_supportsPresentWait2) {
        m_vkWaitForPresent2KHR = reinterpret_cast<PFN_vkWaitForPresent2KHR>(
            vkGetDeviceProcAddr(m_device, "vkWaitForPresent2KHR"));
        if (!m_vkWaitForPresent2KHR) {
            qCWarning(KWIN_VULKAN) << "Failed to load vkWaitForPresent2KHR, disabling present wait";
            m_supportsPresentWait2 = false;
        }
    }

    qCDebug(KWIN_VULKAN) << "Vulkan logical device created successfully";
    return true;
}

QVulkanInstance *VulkanBackend::qVulkanInstance()
{
    if (m_qVulkanInstance) {
        return m_qVulkanInstance.get();
    }
    if (m_instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    auto wrapper = std::make_unique<QVulkanInstance>();
    wrapper->setVkInstance(m_instance);
    // Match the API version requested in createInstance(); Qt uses this to gate which
    // device-level features its QRhi can rely on.
    wrapper->setApiVersion(QVersionNumber(1, 2));
    if (!wrapper->create()) {
        qCWarning(KWIN_VULKAN) << "QVulkanInstance::create() failed (errorCode="
                               << wrapper->errorCode() << ")";
        return nullptr;
    }
    m_qVulkanInstance = std::move(wrapper);
    return m_qVulkanInstance.get();
}

void VulkanBackend::cleanup()
{
    // Drop the Qt wrapper before the VkInstance it references. setVkInstance() means
    // the wrapper does not own the handle, so this only tears down Qt-side state.
    m_qVulkanInstance.reset();

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

#ifndef NDEBUG
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr) {
            func(m_instance, m_debugMessenger, nullptr);
        }
        m_debugMessenger = VK_NULL_HANDLE;
    }
#endif

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

void VulkanBackend::copyPixels(const QRegion &region, const QSize &screenSize)
{
    VulkanContext *context = vulkanContext();
    if (!context || !context->makeCurrent()) {
        return;
    }

    // Create command buffer for pixel copying
    VkCommandBuffer cmd = context->beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return;
    }

    // Get the current framebuffer or swapchain image
    VulkanFramebuffer *framebuffer = context->currentFramebuffer();
    if (!framebuffer || !framebuffer->colorTexture()) {
        context->endSingleTimeCommands(cmd);
        return;
    }

    // Get the source image
    VkImage srcImage = framebuffer->colorTexture()->image();
    Q_UNUSED(srcImage); // Will be used for actual pixel copy implementation

    // Create destination image for pixel data
    // This would typically be a staging buffer or host-visible image

    // Transition source image layout for transfer
    framebuffer->colorTexture()->transitionLayout(cmd,
                                                  framebuffer->colorTexture()->currentLayout(),
                                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Copy or blit image data based on region
    for (const QRect &rect : region) {
        // Use vkCmdCopyImage or vkCmdBlitImage to copy pixel data
        // Implementation would depend on the destination format
        Q_UNUSED(rect);
    }

    // Transition source image back to original layout
    framebuffer->colorTexture()->transitionLayout(cmd,
                                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                  framebuffer->colorTexture()->currentLayout(),
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // End and submit command buffer
    context->endSingleTimeCommands(cmd);
}

} // namespace KWin

#include "moc_vulkanbackend.cpp"
