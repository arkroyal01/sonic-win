/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/globals.h"

#include <QByteArray>
#include <QObject>

#include <chrono>
#include <utility>
#include <vector>

namespace KWin
{

class RenderLoopPrivate;
class SurfaceItem;
class Item;
class Output;
class RenderLayer;
class OutputLayer;

/**
 * The RenderLoop class represents the compositing scheduler on a particular output.
 *
 * The RenderLoop class drives the compositing. The frameRequested() signal is emitted
 * when the loop wants a new frame to be rendered. The frameCompleted() signal is
 * emitted when a previously rendered frame has been presented on the screen. In case
 * you want the compositor to repaint the scene, call the scheduleRepaint() function.
 */
class KWIN_EXPORT RenderLoop : public QObject
{
    Q_OBJECT

public:
    explicit RenderLoop(Output *output);
    ~RenderLoop() override;

    /**
     * Pauses the render loop. While the render loop is inhibited, scheduleRepaint()
     * requests are queued.
     *
     * Once the render loop is uninhibited, the pending schedule requests are going to
     * be re-applied.
     */
    void inhibit();

    /**
     * Uninhibits the render loop.
     */
    void uninhibit();

    /**
     * This function must be called before the Compositor sumbits the next
     * frame.
     */
    void prepareNewFrame();

    /**
     * This function must be called before the Compositor starts rendering the next
     * frame.
     */
    void beginPaint();

    /**
     * Returns the refresh rate at which the output is being updated, in millihertz.
     */
    int refreshRate() const;

    /**
     * Sets the refresh rate of this RenderLoop to @a refreshRate, in millihertz.
     */
    void setRefreshRate(int refreshRate);

    void setPresentationSafetyMargin(std::chrono::nanoseconds safetyMargin);

    /**
     * @brief Per-frame compositor-dispatch boundary. Used by the main-thread
     * frame-breakdown instrumentation (KWIN_FRAME_BREAKDOWN=1) to pinpoint
     * where on the dispatch path a long stall is happening.
     *
     * Captured by recordFrameBoundary(); logged as per-frame deltas to the
     * perf CSV. The boundaries are listed in dispatch order; differences
     * between consecutive boundaries identify which sub-pass blocked.
     */
    enum class FrameBoundary {
        DispatchStart, ///< RenderLoopPrivate::dispatch() entry.
        CompositeStart, ///< X11Compositor::composite() entry.
        PrepaintEnd, ///< after prePaintPass (effects' prePaintScreen).
        BeginFrameEnd, ///< after primaryLayer->beginFrame().
        PaintEnd, ///< after paintPass (scene + effect paint chain).
        EndFrameEnd, ///< after primaryLayer->endFrame().
        PostpaintEnd, ///< after postPaintPass (effects' postPaintScreen).
        PresentEnd, ///< after backend()->present().
    };

    /**
     * @brief Stamp the current steady_clock at the given boundary.
     *
     * No-op when KWIN_FRAME_BREAKDOWN=0 (the default). The breakdown is
     * orthogonal to KWIN_VULKAN_LATENCY_TELEMETRY — that flag enables the
     * CSV, this one fills the breakdown columns. Both are needed for the
     * breakdown to land in a file.
     */
    void recordFrameBoundary(FrameBoundary which);

    /**
     * @brief Sub-phase of the prepaint/paint dispatch that the detail
     * breakdown (KWIN_FRAME_BREAKDOWN_DETAIL=1) decomposes.
     */
    enum class FrameDetailPhase {
        Prepaint, ///< CompositeStart → PrepaintEnd (scene prePaint + effects' prePaintScreen).
        Paint, ///< BeginFrameEnd → PaintEnd (scene paint + effects' paintScreen).
    };

    /**
     * @brief Stash the per-effect trace and scene-side time for the given
     * phase, to be written to the sidecar detail CSV by
     * notifyFrameCompleted().
     *
     * Called by WorkspaceScene at the end of prePaint / paint. The trace
     * is in iterator order (outermost-first); exclusive per-effect times
     * are derived at write time by differencing consecutive inclusive
     * values. No-op when KWIN_FRAME_BREAKDOWN_DETAIL=0.
     */
    void recordFrameDetail(FrameDetailPhase phase,
                           std::chrono::nanoseconds sceneTime,
                           const std::vector<std::pair<QByteArray, std::chrono::nanoseconds>> &trace);

    /**
     * @brief True when the breakdown-detail env var is on, so callers can
     * skip building a trace when it would be discarded. Cheap (reads a
     * process-static).
     */
    static bool frameBreakdownDetailEnabled();

    /**
     * Schedules a compositing cycle at the next available moment.
     */
    void scheduleRepaint(Item *item = nullptr, RenderLayer *layer = nullptr, OutputLayer *outputLayer = nullptr);

    /**
     * Returns the timestamp of the last frame that has been presented on the screen.
     * The returned timestamp is sourced from the monotonic clock.
     */
    std::chrono::nanoseconds lastPresentationTimestamp() const;

    /**
     * If a repaint has been scheduled, this function returns the expected time when
     * the next frame will be presented on the screen. The returned timestamp is sourced
     * from the monotonic clock.
     */
    std::chrono::nanoseconds nextPresentationTimestamp() const;

    void setPresentationMode(PresentationMode mode);

    void setMaxPendingFrameCount(uint32_t maxCount);

    /**
     * Returns the expected time how long it is going to take to render the next frame.
     */
    std::chrono::nanoseconds predictedRenderTime() const;

Q_SIGNALS:
    /**
     * This signal is emitted when the refresh rate of this RenderLoop has changed.
     */
    void refreshRateChanged();
    /**
     * This signal is emitted when a frame has been actually presented on the screen.
     * @a timestamp indicates the time when it took place.
     */
    void framePresented(RenderLoop *loop, std::chrono::nanoseconds timestamp, PresentationMode mode);

    /**
     * This signal is emitted when the render loop wants a new frame to be composited.
     *
     * The Compositor should make a connection to this signal using Qt::DirectConnection.
     */
    void frameRequested(RenderLoop *loop);

private:
    std::unique_ptr<RenderLoopPrivate> d;
    friend class RenderLoopPrivate;
};

} // namespace KWin
