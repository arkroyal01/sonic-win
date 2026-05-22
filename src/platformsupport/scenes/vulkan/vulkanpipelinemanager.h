/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "vulkanpipeline.h"

#include <QByteArray>
#include <QStack>
#include <map>
#include <memory>

namespace KWin
{

class VulkanContext;

/**
 * @brief Manages Vulkan pipelines with trait-based caching.
 *
 * Similar to OpenGL's ShaderManager, this class caches pipelines based on
 * shader traits to avoid redundant pipeline creation.
 */
class KWIN_EXPORT VulkanPipelineManager
{
public:
    explicit VulkanPipelineManager(VulkanContext *context);
    ~VulkanPipelineManager();

    /**
     * @brief Get or create a pipeline for the given traits, built against
     *        the current default render pass.
     *
     * Equivalent to pipeline(traits, defaultRenderPass()). The cache is
     * keyed by (traits, renderPass), so consumers using a different render
     * pass — e.g. an offscreen target whose color format does not match the
     * swapchain — should call the two-argument overload directly to keep
     * their pipelines isolated from the main scene's.
     */
    VulkanPipeline *pipeline(VulkanShaderTraits traits);

    /**
     * @brief Get or create a pipeline for (traits, renderPass).
     *
     * Pipelines are not interchangeable across render passes of different
     * color formats (Vulkan spec §8.2 render-pass compatibility), so this
     * overload is the canonical entry point for offscreen consumers that
     * render into a non-swapchain format — most notably the zero-copy
     * window-thumbnail path which targets RGBA so QtQuick can import the
     * VkImage directly without an R/B swap.
     */
    VulkanPipeline *pipeline(VulkanShaderTraits traits, VkRenderPass renderPass);

    /**
     * @brief Set the default render pass used by the single-argument
     *        pipeline(traits) overload.
     *
     * Changing the default render pass invalidates every cached pipeline,
     * since most callers in the codebase request pipelines without naming
     * a render pass and the previous defaults would no longer match.
     * Render-pass-explicit consumers (the two-argument overload) are
     * unaffected by this beyond a rebuild-on-next-use.
     */
    void setRenderPass(VkRenderPass renderPass);

    /**
     * @brief Get the current render pass.
     */
    VkRenderPass renderPass() const
    {
        return m_renderPass;
    }

    /**
     * @brief Push a pipeline onto the binding stack.
     */
    void pushPipeline(VulkanPipeline *pipeline);

    /**
     * @brief Pop the top pipeline from the binding stack.
     */
    VulkanPipeline *popPipeline();

    /**
     * @brief Get the currently bound pipeline.
     */
    VulkanPipeline *currentPipeline() const;

    /**
     * @brief Clear all cached pipelines.
     *
     * Call this when the render pass changes.
     */
    void clearCache();

    /**
     * @brief Load compiled shader SPIR-V from resources or files.
     */
    bool loadShaders();

    /**
     * @brief Check if shaders are loaded.
     */
    bool shadersLoaded() const
    {
        return m_shadersLoaded;
    }

private:
    VulkanContext *m_context;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;

    using PipelineKey = std::pair<VulkanShaderTraits, VkRenderPass>;
    std::map<PipelineKey, std::unique_ptr<VulkanPipeline>> m_pipelines;
    QStack<VulkanPipeline *> m_pipelineStack;

    QByteArray m_vertexShaderSpirv;
    QByteArray m_fragmentShaderSpirv;
    bool m_shadersLoaded = false;
};

/**
 * @brief RAII helper for pipeline binding.
 */
class KWIN_EXPORT VulkanPipelineBinder
{
public:
    VulkanPipelineBinder(VulkanPipelineManager *manager, VulkanShaderTraits traits);
    ~VulkanPipelineBinder();

    VulkanPipeline *pipeline() const
    {
        return m_pipeline;
    }

private:
    VulkanPipelineManager *m_manager;
    VulkanPipeline *m_pipeline;
};

} // namespace KWin
