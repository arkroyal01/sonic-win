/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "renderjournalpercentile.h"

#include <algorithm>
#include <cmath>

namespace KWin
{

RenderJournalPercentile::RenderJournalPercentile(double percentile, std::size_t capacity)
    : m_percentile(percentile)
    , m_capacity(capacity)
    , m_ring(capacity, std::chrono::nanoseconds::zero())
{
}

void RenderJournalPercentile::add(std::chrono::nanoseconds renderTime, std::chrono::nanoseconds presentationTimestamp)
{
    (void)presentationTimestamp;
    if (m_capacity == 0) {
        return;
    }
    m_ring[m_cursor] = renderTime;
    m_cursor = (m_cursor + 1) % m_capacity;
    if (m_filled < m_capacity) {
        ++m_filled;
    }
}

std::chrono::nanoseconds RenderJournalPercentile::result() const
{
    if (m_filled == 0) {
        return std::chrono::nanoseconds::zero();
    }
    // Copy only the populated prefix; for a partially-filled ring the unused
    // tail still holds the default-constructed zeros and would skew p99 low.
    std::vector<std::chrono::nanoseconds> copy(m_ring.begin(), m_ring.begin() + m_filled);
    // ceil(percentile * (n - 1)) — picks the highest sample at or below the
    // requested percentile rank rather than interpolating. For p99 across 100
    // samples this is index 99 (= the worst observed), which matches the
    // scheduler's intent ("worst-case we're willing to plan for").
    const std::size_t idx = std::min<std::size_t>(
        static_cast<std::size_t>(std::ceil(m_percentile * double(copy.size() - 1))),
        copy.size() - 1);
    std::nth_element(copy.begin(), copy.begin() + idx, copy.end());
    return copy[idx];
}

} // namespace KWin
