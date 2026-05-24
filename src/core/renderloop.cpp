/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "renderloop.h"
#include "options.h"
#include "renderloop_p.h"
#include "scene/surfaceitem.h"
#include "utils/common.h"
#include "window.h"
#include "workspace.h"

#include <filesystem>

using namespace std::chrono_literals;

namespace KWin
{

RenderLoopPrivate *RenderLoopPrivate::get(RenderLoop *loop)
{
    return loop->d.get();
}

static const bool s_printDebugInfo = qEnvironmentVariableIntValue("KWIN_LOG_PERFORMANCE_DATA") != 0
    || qEnvironmentVariableIntValue("KWIN_VULKAN_LATENCY_TELEMETRY") != 0;

// Phase 3 tight scheduling (KWIN_VULKAN_TIGHT_SCHED=1): swap RenderJournal's
// EMA + 2*variance budget for a rolling percentile of observed render
// durations, and tune setPresentationSafetyMargin() with a feedback loop
// against the observed vblank-miss rate. Read once at process startup.
static const bool s_tightSched = qEnvironmentVariableIntValue("KWIN_VULKAN_TIGHT_SCHED") != 0;

// Helper: read a double from an env var, fall back to @p fallback if unset
// or unparseable. Used for the env-overridable controller knobs below.
static double envDouble(const char *name, double fallback)
{
    if (!qEnvironmentVariableIsSet(name)) {
        return fallback;
    }
    bool ok = false;
    const double v = qEnvironmentVariable(name).toDouble(&ok);
    return ok ? v : fallback;
}

// Controller targets: shrink the safety margin while the recent miss-rate
// stays below s_targetMissRateLow; grow it when above s_targetMissRateHigh.
// Between the two edges the margin holds steady, avoiding oscillation.
// 0.1% lower bound is roughly "one miss per ~3 s at 165 Hz".
// 1% upper bound is the visible-jank threshold most users tolerate.
static const double s_targetMissRateLow = envDouble("KWIN_VULKAN_TIGHT_SCHED_TARGET_LOW", 0.001);
static const double s_targetMissRateHigh = envDouble("KWIN_VULKAN_TIGHT_SCHED_TARGET_HIGH", 0.01);

// Step size for each adjustment, expressed as a fraction of the current
// vblank interval so controller convergence is consistent across refresh
// rates (60 / 120 / 165 / 240 Hz monitors all need ~60 steps to saturate
// at 0.015). The actual ns value is computed per-call from the live
// vblankInterval — the refresh rate may change at runtime (display
// reconfiguration / VRR transitions).
static const double s_stepFraction = envDouble("KWIN_VULKAN_TIGHT_SCHED_STEP_FRACTION", 0.015);

// Miss-rate window in *seconds*, multiplied by the live refresh rate at
// first use to size the ring buffer. Same wall-clock memory of ~4 s
// regardless of refresh rate; long enough to smooth noise and short enough
// to react to a sustained regression in a few seconds. The ring is
// reallocated if the refresh rate later changes by more than ~15 % so the
// time window stays roughly constant.
static const double s_windowSeconds = envDouble("KWIN_VULKAN_TIGHT_SCHED_WINDOW_SECONDS", 4.0);

// Phase 6 main-thread frame breakdown (KWIN_FRAME_BREAKDOWN=1). When set,
// RenderLoop::recordFrameBoundary() stamps steady_clock per boundary and
// notifyFrameCompleted() emits the deltas as extra CSV columns. Off by
// default — the calls become no-ops so the dispatch path pays no cost
// outside investigation runs.
static const bool s_frameBreakdown = qEnvironmentVariableIntValue("KWIN_FRAME_BREAKDOWN") != 0;

RenderLoopPrivate::RenderLoopPrivate(RenderLoop *q, Output *output)
    : q(q)
    , output(output)
{
    compositeTimer.setSingleShot(true);
    compositeTimer.setTimerType(Qt::PreciseTimer);

    QObject::connect(&compositeTimer, &QTimer::timeout, q, [this]() {
        dispatch();
    });

    delayedVrrTimer.setSingleShot(true);
    delayedVrrTimer.setInterval(1'000 / 30);
    delayedVrrTimer.setTimerType(Qt::PreciseTimer);

    QObject::connect(&delayedVrrTimer, &QTimer::timeout, q, [q]() {
        q->scheduleRepaint(nullptr, nullptr);
    });

    if (s_tightSched) {
        // Materialise the percentile journal only when the env var is on —
        // keeps the per-RenderLoop overhead at zero in the default path.
        // The miss-rate ring is allocated lazily in notifyFrameCompleted()
        // once the live refresh rate is known. Logged once per process so
        // the env-var landing is observable from the terminal. Plain static
        // bool: RenderLoops are constructed on the main thread.
        renderJournalPercentile.emplace();
        static bool s_loggedTightSched = false;
        if (!s_loggedTightSched) {
            s_loggedTightSched = true;
            qCWarning(KWIN_CORE).nospace()
                << "RenderLoop: tight scheduler active "
                << "(KWIN_VULKAN_TIGHT_SCHED=1: p99 render-journal + adaptive safety margin, "
                << "target miss-rate band ["
                << s_targetMissRateLow * 100.0 << "%, " << s_targetMissRateHigh * 100.0 << "%], "
                << "step=" << s_stepFraction * 100.0 << "% of vblank, "
                << "window=" << s_windowSeconds << "s)";
        }
    }
}

void RenderLoopPrivate::scheduleNextRepaint()
{
    if (kwinApp()->isTerminating() || compositeTimer.isActive()) {
        return;
    }
    scheduleRepaint(nextPresentationTimestamp);
}

void RenderLoopPrivate::scheduleRepaint(std::chrono::nanoseconds lastTargetTimestamp)
{
    pendingReschedule = false;
    const std::chrono::nanoseconds vblankInterval(1'000'000'000'000ull / refreshRate);
    const std::chrono::nanoseconds currentTime(std::chrono::steady_clock::now().time_since_epoch());

    // Estimate when it's a good time to perform the next compositing cycle.
    // the 1ms on top of the safety margin is required for timer and scheduler inaccuracies.
    // Tight-scheduler path (Phase 3, KWIN_VULKAN_TIGHT_SCHED=1) uses a rolling
    // p99 of observed render durations instead of EMA + 2*variance; safetyMargin
    // itself is tuned by the miss-rate feedback in notifyFrameCompleted().
    const std::chrono::nanoseconds journalResult = renderJournalPercentile
        ? renderJournalPercentile->result()
        : renderJournal.result();
    std::chrono::nanoseconds expectedCompositingTime = std::min(journalResult + safetyMargin + 1ms, 2 * vblankInterval);

    if (presentationMode == PresentationMode::VSync) {
        // normal presentation: pageflips only happen at vblank
        const uint64_t pageflipsSince = std::max<int64_t>((currentTime - lastPresentationTimestamp) / vblankInterval, 0);
        if (pageflipsSince > 100) {
            // if it's been a while since the last frame, the GPU is likely in a low power state and render time will be increased
            // -> take that into account and start compositing very early
            expectedCompositingTime = std::max(vblankInterval - 1us, expectedCompositingTime);
        }
        const uint64_t pageflipsSinceLastToTarget = std::max<int64_t>(std::round((lastTargetTimestamp - lastPresentationTimestamp).count() / double(vblankInterval.count())), 0);
        uint64_t pageflipsInAdvance = std::min<int64_t>(expectedCompositingTime / vblankInterval + 1, maxPendingFrameCount);

        if (s_tightSched && renderJournalPercentile) {
            // Phase 4: replace the legacy hysteresis with a direct miss-rate
            // signal. m_inTripleBuffer is flipped in notifyFrameCompleted()
            // based on the observed miss rate (Phase 3 ring): true when the
            // recent rate crosses target_high, false again only after a full
            // window stays below target_low. The compositing-time-derived
            // minimum still applies — if expectedCompositingTime physically
            // exceeds a vblank (e.g. Phase 3 has grown the safety margin to
            // its cap), we *must* render >= 2 vblanks ahead or schedule a
            // start in the past; take the max so both signals are honored.
            const uint64_t pageflipsByMissRate = std::min<uint64_t>(m_inTripleBuffer ? 2 : 1, uint64_t(maxPendingFrameCount));
            pageflipsInAdvance = std::max(pageflipsInAdvance, pageflipsByMissRate);
        } else {
            // switching from double to triple buffering causes a frame drop
            // -> apply some amount of hysteresis to avoid switching back and forth constantly
            if (pageflipsInAdvance > 1) {
                // immediately switch to triple buffering when needed
                wasTripleBuffering = true;
                doubleBufferingCounter = 0;
            } else if (wasTripleBuffering) {
                // but wait a bit before switching back to double buffering
                if (doubleBufferingCounter >= 10) {
                    wasTripleBuffering = false;
                } else if (expectedCompositingTime >= vblankInterval * 0.95) {
                    // also don't switch back if render times are just barely enough for double buffering
                    pageflipsInAdvance = 2;
                    doubleBufferingCounter = 0;
                    expectedCompositingTime = vblankInterval;
                } else {
                    doubleBufferingCounter++;
                    pageflipsInAdvance = 2;
                    expectedCompositingTime = vblankInterval;
                }
            }
        }

        if (compositeTimer.isActive()) {
            // we already scheduled this frame, but we got a new timestamp
            // which might require starting to composite earlier than we planned
            // It's important here that we do not change the targeted vblank interval,
            // otherwise with a pessimistic compositing time estimation we might
            // unnecessarily drop frames
            const uint32_t intervalsSinceLastTimestamp = std::max<int32_t>(std::round((nextPresentationTimestamp - lastPresentationTimestamp).count() / double(vblankInterval.count())), 0);
            nextPresentationTimestamp = lastPresentationTimestamp + intervalsSinceLastTimestamp * vblankInterval;
        } else {
            nextPresentationTimestamp = lastPresentationTimestamp + std::max(pageflipsSince + pageflipsInAdvance, pageflipsSinceLastToTarget + 1) * vblankInterval;
        }
    } else {
        wasTripleBuffering = false;
        doubleBufferingCounter = 0;
        if (presentationMode == PresentationMode::Async || presentationMode == PresentationMode::AdaptiveAsync) {
            // tearing: pageflips happen ASAP
            nextPresentationTimestamp = currentTime;
        } else {
            // adaptive sync: pageflips happen after one vblank interval
            // TODO read minimum refresh rate from the EDID and take it into account here
            nextPresentationTimestamp = lastPresentationTimestamp + vblankInterval;
        }
    }

    const std::chrono::nanoseconds nextRenderTimestamp = nextPresentationTimestamp - expectedCompositingTime;
    compositeTimer.start(std::max(0ms, std::chrono::duration_cast<std::chrono::milliseconds>(nextRenderTimestamp - currentTime)));

    if (s_frameBreakdown) {
        // Record the intended fire time so timer-delay (actual dispatch vs
        // scheduled) is computable in notifyFrameCompleted().
        const auto schedNs = std::max(currentTime, nextRenderTimestamp);
        m_timerScheduledAt = std::chrono::steady_clock::time_point(schedNs);
    }
}

void RenderLoopPrivate::delayScheduleRepaint()
{
    pendingReschedule = true;
}

void RenderLoopPrivate::notifyFrameDropped()
{
    Q_ASSERT(pendingFrameCount > 0);
    pendingFrameCount--;

    if (!inhibitCount && pendingReschedule) {
        scheduleNextRepaint();
    }
}

void RenderLoopPrivate::notifyFrameCompleted(std::chrono::nanoseconds timestamp, std::optional<RenderTimeSpan> renderTime, PresentationMode mode, OutputFrame *frame)
{
    if (s_printDebugInfo && !m_debugOutput) {
        // On X11 standalone the backend constructs a single shared
        // RenderLoop(nullptr) (x11_standalone_backend.cpp:105) — there is no
        // per-output loop, so output->name() is unavailable. Fall back to a
        // fixed suffix in that case so the CSV actually opens.
        const std::string suffix = output ? output->name().toStdString() : std::string("(default)");
        const std::string path = "kwin perf statistics " + suffix + ".csv";
        m_debugOutput = std::fstream(path, std::ios::out);
        // One-shot confirmation so it's obvious from the terminal that the env
        // var landed and which file to tail. Resolve to absolute so cwd is
        // unambiguous.
        std::error_code ec;
        const auto absPath = std::filesystem::absolute(path, ec);
        qCWarning(KWIN_CORE).noquote()
            << "RenderLoop: writing perf statistics to"
            << QString::fromStdString(ec ? path : absPath.string());
        *m_debugOutput << "target pageflip timestamp,pageflip timestamp,render start,render end,safety margin,refresh duration,vrr,tearing,predicted render time,"
                       << "queue ops end,first pixel out,first pixel visible,end to end latency,vblank miss,gpu render duration,"
                       << "timer delay,dispatch to composite,prepaint,beginframe,paint,endframe,postpaint,present\n";
    }
    if (m_debugOutput) {
        auto times = renderTime.value_or(RenderTimeSpan{});
        const bool vrr = mode == PresentationMode::AdaptiveSync || mode == PresentationMode::AdaptiveAsync;
        const bool tearing = mode == PresentationMode::Async || mode == PresentationMode::AdaptiveAsync;
        // VK_EXT_present_timing stage timestamps; 0 if the stage was not
        // reported (or the timing path is inactive / GLX fallback in use).
        const auto qEnd = frame->queueOperationsEndTimestamp().value_or(std::chrono::nanoseconds::zero());
        const auto fpOut = frame->firstPixelOutTimestamp().value_or(std::chrono::nanoseconds::zero());
        const auto fpVisible = frame->firstPixelVisibleTimestamp().value_or(std::chrono::nanoseconds::zero());
        // End-to-end render-to-photons: render start → on-screen. Prefer
        // FIRST_PIXEL_VISIBLE; fall back to FIRST_PIXEL_OUT (latest CLOCK_MONOTONIC
        // stage Mesa MR !39551 reports today). QUEUE_OPS_END lives in a
        // different clock and would produce garbage here, so it is not a
        // fallback. 0 if no stage was reported or renderTime is unset.
        const auto renderStartNs = times.start.time_since_epoch();
        const auto e2eEnd = fpVisible.count() != 0 ? fpVisible : fpOut;
        const std::chrono::nanoseconds e2e =
            (e2eEnd.count() != 0 && renderStartNs.count() != 0)
            ? (e2eEnd - renderStartNs)
            : std::chrono::nanoseconds::zero();
        // Vblank miss: presented past the target by more than half a refresh
        // (conservative — a "just made it" frame is not a miss).
        const std::chrono::nanoseconds targetNs = frame->targetPageflipTime().time_since_epoch();
        const bool miss = timestamp > targetNs + frame->refreshDuration() / 2;
        // GPU-measured render duration (Phase 2 side channel). 0 when
        // KWIN_VULKAN_GPU_RENDER_TIME is off, when the device does not
        // support timestamps, or when the query was not yet available at
        // read time (e.g. aborted submit). Independent from the scheduler's
        // CPU-measured renderTime above.
        const auto gpuRd = frame->queryGpuRenderDuration().value_or(std::chrono::nanoseconds::zero());

        // Phase 6 main-thread breakdown. All-zero when KWIN_FRAME_BREAKDOWN=0
        // (recordFrameBoundary was a no-op so the slots stayed
        // default-constructed). Each delta is consecutive-boundary ns;
        // timer_delay is "actual dispatch time - timer scheduled fire time"
        // (positive = event-loop made us late dispatching this frame).
        using Boundary = RenderLoop::FrameBoundary;
        auto stamp = [&](Boundary b) {
            return m_frameBoundary[size_t(b)];
        };
        auto delta = [&](Boundary a, Boundary b) {
            const auto sa = stamp(a), sb = stamp(b);
            return (sa.time_since_epoch().count() == 0 || sb.time_since_epoch().count() == 0)
                ? std::chrono::nanoseconds::zero()
                : std::chrono::duration_cast<std::chrono::nanoseconds>(sb - sa);
        };
        const auto dispatchStart = stamp(Boundary::DispatchStart);
        const std::chrono::nanoseconds timerDelay =
            (dispatchStart.time_since_epoch().count() == 0 || m_timerScheduledAt.time_since_epoch().count() == 0)
            ? std::chrono::nanoseconds::zero()
            : std::chrono::duration_cast<std::chrono::nanoseconds>(dispatchStart - m_timerScheduledAt);
        const auto dispToComposite = delta(Boundary::DispatchStart, Boundary::CompositeStart);
        const auto prepaint = delta(Boundary::CompositeStart, Boundary::PrepaintEnd);
        const auto beginframe = delta(Boundary::PrepaintEnd, Boundary::BeginFrameEnd);
        const auto paint = delta(Boundary::BeginFrameEnd, Boundary::PaintEnd);
        const auto endframe = delta(Boundary::PaintEnd, Boundary::EndFrameEnd);
        const auto postpaint = delta(Boundary::EndFrameEnd, Boundary::PostpaintEnd);
        const auto present = delta(Boundary::PostpaintEnd, Boundary::PresentEnd);

        *m_debugOutput << targetNs.count() << "," << timestamp.count() << "," << times.start.time_since_epoch().count() << "," << times.end.time_since_epoch().count()
                       << "," << safetyMargin.count() << "," << frame->refreshDuration().count() << "," << (vrr ? 1 : 0) << "," << (tearing ? 1 : 0) << "," << frame->predictedRenderTime().count()
                       << "," << qEnd.count() << "," << fpOut.count() << "," << fpVisible.count() << "," << e2e.count() << "," << (miss ? 1 : 0) << "," << gpuRd.count()
                       << "," << timerDelay.count() << "," << dispToComposite.count() << "," << prepaint.count() << "," << beginframe.count() << "," << paint.count() << "," << endframe.count() << "," << postpaint.count() << "," << present.count() << "\n";
    }

    Q_ASSERT(pendingFrameCount > 0);
    pendingFrameCount--;

    notifyVblank(timestamp);

    if (renderTime) {
        const auto rt = renderTime->end - renderTime->start;
        renderJournal.add(rt, timestamp);
        if (renderJournalPercentile) {
            renderJournalPercentile->add(rt, timestamp);
        }
    }

    // Phase 3 miss-rate feedback controller. Same miss criterion as the CSV's
    // vblank_miss column (presented past target by more than half a vblank).
    // Steps safetyMargin toward the [low, high] miss-rate band; outside the
    // band the next frame's scheduleRepaint sees the new margin via
    // expectedCompositingTime. Only runs when the tight scheduler is active.
    if (renderJournalPercentile) {
        // Live vblank interval and the time-anchored ring size. Reallocate
        // if the refresh rate moved enough that the ring would cover a
        // visibly wrong window (>15 % drift), so the controller's reaction
        // speed stays consistent in wall-clock terms after display reconfig.
        const std::chrono::nanoseconds vblankInterval(1'000'000'000'000ull / refreshRate);
        const double refreshHz = double(refreshRate) / 1000.0;
        const std::size_t targetWindow = std::max<std::size_t>(60, std::size_t(s_windowSeconds * refreshHz));
        if (m_missRing.size() == 0
            || (m_missRing.size() < targetWindow * 17 / 20)
            || (m_missRing.size() > targetWindow * 23 / 20)) {
            // Reset history on resize — preserving a stale window across a
            // 60 → 165 Hz transition would feed the controller inconsistent
            // samples for ~4 s afterwards. Cleaner to lose the history.
            m_missRing.assign(targetWindow, false);
            m_missRingCursor = 0;
            m_missRingFilled = 0;
            m_missCount = 0;
        }

        const std::chrono::nanoseconds targetNs = frame->targetPageflipTime().time_since_epoch();
        const bool miss = timestamp > targetNs + frame->refreshDuration() / 2;
        // Replace the oldest entry; keep a running m_missCount so we don't
        // rescan the whole ring per frame.
        const bool prior = m_missRing[m_missRingCursor];
        if (m_missRingFilled == m_missRing.size()) {
            m_missCount -= prior ? 1 : 0;
        } else {
            ++m_missRingFilled;
        }
        m_missRing[m_missRingCursor] = miss;
        m_missCount += miss ? 1 : 0;
        m_missRingCursor = (m_missRingCursor + 1) % m_missRing.size();

        // Wait for at least a quarter window before reacting, so the controller
        // doesn't yank the margin on the first handful of samples.
        if (m_missRingFilled >= m_missRing.size() / 4) {
            const double rate = double(m_missCount) / double(m_missRingFilled);
            // Step size scales with vblank so saturation distance is
            // refresh-rate-invariant. Floor at 1 µs to keep progress real
            // even on hypothetical >1 kHz panels.
            const std::chrono::nanoseconds marginStep = std::max<std::chrono::nanoseconds>(
                std::chrono::nanoseconds(int64_t(double(vblankInterval.count()) * s_stepFraction)),
                std::chrono::microseconds(1));
            if (rate < s_targetMissRateLow && safetyMargin > 0ns) {
                const auto next = std::max(safetyMargin - marginStep, 0ns);
                q->setPresentationSafetyMargin(next);
            } else if (rate > s_targetMissRateHigh && safetyMargin < vblankInterval) {
                const auto next = std::min(safetyMargin + marginStep, vblankInterval);
                q->setPresentationSafetyMargin(next);
            }

            // Phase 4 buffering hysteresis. Triple-buffering adds ~1 vblank
            // of presentation latency, so the controller is conservative
            // about staying out of double-buffer mode: it enters triple as
            // soon as the recent rate crosses the high target (Phase 3's
            // safety margin alone hasn't absorbed the spike), and only
            // leaves triple after an entire window has stayed below the low
            // target. Same target band as the margin controller — both react
            // to the same signal but on different latencies (margin: per
            // frame; buffer depth: per spike).
            const bool wasInTriple = m_inTripleBuffer;
            if (m_inTripleBuffer) {
                if (rate < s_targetMissRateLow && m_missRingFilled == m_missRing.size()) {
                    m_inTripleBuffer = false;
                }
            } else {
                if (rate > s_targetMissRateHigh) {
                    m_inTripleBuffer = true;
                }
            }
            if (wasInTriple != m_inTripleBuffer) {
                qCWarning(KWIN_CORE).nospace()
                    << "RenderLoop: buffering "
                    << (m_inTripleBuffer ? "double->triple" : "triple->double")
                    << " (miss rate " << (rate * 100.0) << "% over "
                    << m_missRingFilled << " frames)";
            }
        }
    }
    if (compositeTimer.isActive()) {
        // reschedule to match the new timestamp and render time
        scheduleRepaint(lastPresentationTimestamp);
    }
    if (!inhibitCount && pendingReschedule) {
        scheduleNextRepaint();
    }

    Q_EMIT q->framePresented(q, timestamp, mode);
}

void RenderLoopPrivate::notifyVblank(std::chrono::nanoseconds timestamp)
{
    if (lastPresentationTimestamp <= timestamp) {
        lastPresentationTimestamp = timestamp;
    } else {
        qCDebug(KWIN_CORE,
                "Got invalid presentation timestamp: %lld (current %lld)",
                static_cast<long long>(timestamp.count()),
                static_cast<long long>(lastPresentationTimestamp.count()));
        lastPresentationTimestamp = std::chrono::steady_clock::now().time_since_epoch();
    }
}

void RenderLoopPrivate::dispatch()
{
    if (s_frameBreakdown) {
        // Reset the ring for this frame so unfilled slots read as zero in
        // notifyFrameCompleted(). Then stamp DispatchStart immediately —
        // this is "now the timer fired", and m_timerScheduledAt holds when
        // it was *supposed* to. Their delta is the event-loop delay.
        m_frameBoundary.fill(std::chrono::steady_clock::time_point{});
        m_frameBoundary[size_t(RenderLoop::FrameBoundary::DispatchStart)] = std::chrono::steady_clock::now();
    }

    // On X11, we want to ignore repaints that are scheduled by windows right before
    // the Compositor starts repainting.
    pendingRepaint = true;

    Q_EMIT q->frameRequested(q);

    // The Compositor may decide to not repaint when the frameRequested() signal is
    // emitted, in which case the pending repaint flag has to be reset manually.
    pendingRepaint = false;
}

RenderLoop::RenderLoop(Output *output)
    : d(std::make_unique<RenderLoopPrivate>(this, output))
{
}

RenderLoop::~RenderLoop()
{
}

void RenderLoop::inhibit()
{
    d->inhibitCount++;

    if (d->inhibitCount == 1) {
        d->compositeTimer.stop();
    }
}

void RenderLoop::uninhibit()
{
    Q_ASSERT(d->inhibitCount > 0);
    d->inhibitCount--;

    if (d->inhibitCount == 0) {
        d->scheduleNextRepaint();
    }
}

void RenderLoop::prepareNewFrame()
{
    d->pendingFrameCount++;
}

void RenderLoop::beginPaint()
{
    d->pendingRepaint = false;
}

int RenderLoop::refreshRate() const
{
    return d->refreshRate;
}

void RenderLoop::setRefreshRate(int refreshRate)
{
    if (d->refreshRate == refreshRate) {
        return;
    }
    d->refreshRate = refreshRate;
    Q_EMIT refreshRateChanged();
}

void RenderLoop::recordFrameBoundary(FrameBoundary which)
{
    if (!s_frameBreakdown) {
        return;
    }
    d->m_frameBoundary[size_t(which)] = std::chrono::steady_clock::now();
}

void RenderLoop::setPresentationSafetyMargin(std::chrono::nanoseconds safetyMargin)
{
    d->safetyMargin = safetyMargin;
}

void RenderLoop::scheduleRepaint(Item *item, RenderLayer *layer, OutputLayer *outputLayer)
{
    if (d->pendingRepaint) {
        return;
    }
    const bool vrr = d->presentationMode == PresentationMode::AdaptiveSync || d->presentationMode == PresentationMode::AdaptiveAsync;
    const bool tearing = d->presentationMode == PresentationMode::Async || d->presentationMode == PresentationMode::AdaptiveAsync;
    if ((vrr || tearing) && workspace()->activeWindow() && d->output) {
        Window *const activeWindow = workspace()->activeWindow();
        if ((item || layer || outputLayer) && activeWindow->isOnOutput(d->output) && activeWindow->surfaceItem() && item != activeWindow->surfaceItem() && activeWindow->surfaceItem()->frameTimeEstimation() <= std::chrono::nanoseconds(1'000'000'000) / 30) {
            d->delayedVrrTimer.start();
            return;
        }
    }
    d->delayedVrrTimer.stop();
    const int effectiveMaxPendingFrameCount = (vrr || tearing) ? 1 : d->maxPendingFrameCount;
    if (d->pendingFrameCount < effectiveMaxPendingFrameCount && !d->inhibitCount) {
        d->scheduleNextRepaint();
    } else {
        d->delayScheduleRepaint();
    }
}

std::chrono::nanoseconds RenderLoop::lastPresentationTimestamp() const
{
    return d->lastPresentationTimestamp;
}

std::chrono::nanoseconds RenderLoop::nextPresentationTimestamp() const
{
    return d->nextPresentationTimestamp;
}

void RenderLoop::setPresentationMode(PresentationMode mode)
{
    if (mode != d->presentationMode) {
        qCDebug(KWIN_CORE) << "Changed presentation mode to" << mode;
    }
    d->presentationMode = mode;
}

void RenderLoop::setMaxPendingFrameCount(uint32_t maxCount)
{
    d->maxPendingFrameCount = maxCount;
}

std::chrono::nanoseconds RenderLoop::predictedRenderTime() const
{
    return d->renderJournal.result();
}

} // namespace KWin

#include "moc_renderloop.cpp"
