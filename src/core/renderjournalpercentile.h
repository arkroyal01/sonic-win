/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "kwin_export.h"

#include <chrono>
#include <vector>

namespace KWin
{

/**
 * @brief RenderJournal variant whose result() is a rolling percentile of the
 * observed render-time history rather than RenderJournal's EMA + 2*variance.
 *
 * Same public API shape (add() + result()) so RenderLoopPrivate can swap one
 * for the other behind KWIN_VULKAN_TIGHT_SCHED=1 without touching scheduleRepaint().
 * Backed by a ring buffer of the most recent kCapacity render durations; result()
 * copies + sorts the buffer and returns the requested percentile (default p99).
 * Sorting a 240-entry array is sub-microsecond on modern CPUs — negligible
 * compared to the rest of scheduleRepaint().
 *
 * Why: EMA + 2*variance is a Gaussian-flavoured worst-case bound that lags
 * behind individual spikes by ~500ms (the EMA time constant) and only
 * inflates the margin once the variance term has had time to absorb them.
 * With real present-timing feedback we can target a chosen miss rate directly
 * using a true percentile of the observed distribution. The miss-rate
 * controller in RenderLoopPrivate complements this by tuning the
 * setPresentationSafetyMargin() based on observed vblank-miss counts.
 */
class KWIN_EXPORT RenderJournalPercentile
{
public:
    /**
     * @param percentile in [0.0, 1.0], e.g. 0.99 for p99.
     * @param capacity history window in frames. Default 240 = 2 s at 120 Hz.
     */
    explicit RenderJournalPercentile(double percentile = 0.99, std::size_t capacity = 240);

    /**
     * @brief Record a frame's render duration.
     * @param renderTime measured render duration (CPU steady_clock end-start).
     * @param presentationTimestamp not used here (kept for API parity with RenderJournal).
     */
    void add(std::chrono::nanoseconds renderTime, std::chrono::nanoseconds presentationTimestamp);

    /**
     * @brief Percentile of the recorded history.
     *
     * Returns the configured percentile (default p99) of the most-recent
     * up-to-capacity render durations. While the ring buffer is below
     * capacity the percentile is computed over what's present. When empty,
     * returns zero — same behaviour as a freshly-constructed RenderJournal.
     */
    std::chrono::nanoseconds result() const;

private:
    const double m_percentile;
    const std::size_t m_capacity;
    std::vector<std::chrono::nanoseconds> m_ring;
    std::size_t m_cursor = 0; // next write position
    std::size_t m_filled = 0; // number of valid entries in m_ring
};

} // namespace KWin
