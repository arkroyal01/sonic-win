/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "renderbackend.h"
#include "renderjournal.h"
#include "renderjournalpercentile.h"
#include "renderloop.h"

#include <QTimer>

#include <fstream>
#include <optional>
#include <vector>

namespace KWin
{

class SurfaceItem;
class OutputFrame;

class KWIN_EXPORT RenderLoopPrivate
{
public:
    static RenderLoopPrivate *get(RenderLoop *loop);
    explicit RenderLoopPrivate(RenderLoop *q, Output *output);

    void dispatch();

    void delayScheduleRepaint();
    void scheduleNextRepaint();
    void scheduleRepaint(std::chrono::nanoseconds lastTargetTimestamp);

    void notifyFrameDropped();
    void notifyFrameCompleted(std::chrono::nanoseconds timestamp, std::optional<RenderTimeSpan> renderTime, PresentationMode mode, OutputFrame *frame);
    void notifyVblank(std::chrono::nanoseconds timestamp);

    RenderLoop *const q;
    Output *const output;
    std::optional<std::fstream> m_debugOutput;
    std::chrono::nanoseconds lastPresentationTimestamp = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds nextPresentationTimestamp = std::chrono::nanoseconds::zero();
    bool wasTripleBuffering = false;
    int doubleBufferingCounter = 0;
    QTimer compositeTimer;
    RenderJournal renderJournal;
    // Phase 3 tight-scheduling path (KWIN_VULKAN_TIGHT_SCHED=1). When the
    // env var is set this percentile journal feeds scheduleRepaint() in
    // place of `renderJournal`, and a miss-rate controller (m_missRing
    // below) tunes `safetyMargin` to target a chosen vblank-miss rate
    // window. When unset both fields stay default and the existing
    // EMA + 2*variance budget is used. Optional so the unused path costs
    // nothing.
    std::optional<RenderJournalPercentile> renderJournalPercentile;
    int refreshRate = 60000;
    int pendingFrameCount = 0;
    int inhibitCount = 0;
    bool pendingReschedule = false;
    bool pendingRepaint = false;
    std::chrono::nanoseconds safetyMargin{0};

    // Miss-rate feedback ring (only populated under KWIN_VULKAN_TIGHT_SCHED).
    // Each notifyFrameCompleted() writes a 0/1 miss flag here; the controller
    // shrinks safetyMargin when the recent miss-rate falls below the target
    // band's low edge and grows it when above the high edge. Sized for ~5 s
    // at 120 Hz; bool packing keeps memory cost trivial.
    std::vector<bool> m_missRing;
    std::size_t m_missRingCursor = 0;
    std::size_t m_missRingFilled = 0;
    std::size_t m_missCount = 0;

    // Phase 4 buffering hysteresis (tight scheduler only). Flipped to true
    // when the observed miss rate exceeds the high target, back to false
    // only after a full window has stayed below the low target. Replaces
    // the legacy wasTripleBuffering / doubleBufferingCounter heuristic for
    // the tight-scheduler path; the upstream heuristic still runs when
    // KWIN_VULKAN_TIGHT_SCHED=0.
    bool m_inTripleBuffer = false;

    // Phase 6 frame-breakdown instrumentation (KWIN_FRAME_BREAKDOWN=1).
    // recordFrameBoundary() writes into these per-frame slots; the
    // dispatch-order layout matches RenderLoop::FrameBoundary. Cleared at
    // each dispatch() so an unfilled slot reads as zero. Cost is one
    // steady_clock::now() per boundary; the entire breakdown is skipped
    // when the env var is off.
    std::array<std::chrono::steady_clock::time_point, 8> m_frameBoundary{};
    std::chrono::steady_clock::time_point m_timerScheduledAt{};

    PresentationMode presentationMode = PresentationMode::VSync;
    int maxPendingFrameCount = 1;

    QTimer delayedVrrTimer;
};

} // namespace KWin
