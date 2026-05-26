/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/output.h"
#include "effect/effecthandler.h"

#include <QObject>

#include <functional>
#include <unordered_map>
#include <utility>

namespace KWin
{

/**
 * @brief Per-output state container for multi-monitor V2 effects.
 *
 * Both OverviewEffectV2 and CubeEffectV2 currently render a single
 * scene spanning `effects->virtualScreenGeometry()` — the union rect
 * of all outputs. On a multi-monitor setup that stretches their UI
 * across the whole desktop and breaks input dispatch (a click on
 * monitor B hits coordinates the cube on A doesn't recognise).
 *
 * V1's QuickSceneEffect avoids this by instantiating one independent
 * QML view per `Output` via `addScreen` / `handleScreenAdded`. V2
 * effects need the C++ equivalent: a `PerOutputState<T>` whose `T`
 * carries everything an effect's per-output instance owns —
 * atlas slots, framebuffers, camera state, animation factor,
 * MSAA attachments, …
 *
 * Usage shape (OverviewEffectV2 and CubeEffectV2 both follow this):
 *
 * @code
 *   struct CubePerOutput {
 *       CameraState camera{};
 *       OffscreenAa aa{};
 *       std::unordered_map<VirtualDesktop *, DesktopSlot> slots;
 *       ...
 *   };
 *
 *   class CubeEffectV2 : public Effect {
 *       PerOutputState<CubePerOutput> m_outputs;
 *       Output *m_currentOutput = nullptr; // set in paintScreen
 *
 *       CubeEffectV2() {
 *           m_outputs.setSetupCallback([this](Output *o, CubePerOutput &s) {
 *               // alloc per-output resources when an output appears
 *               // or first activate touches this output.
 *           });
 *           m_outputs.setTeardownCallback([this](Output *o, CubePerOutput &s) {
 *               // free per-output resources when an output goes away
 *               // or the effect deactivates.
 *           });
 *           m_outputs.bindToEffectsHandler();
 *       }
 *       ...
 *   };
 * @endcode
 *
 * Lifetime: PerOutputState autoconnects to `EffectsHandler::screenAdded`
 * and `EffectsHandler::screenRemoved` once `bindToEffectsHandler()` is
 * called, so an output hotplug during the active phase fires the
 * setup/teardown callbacks naturally. Entries are also reachable via
 * `forOutput()` for lazy allocation. `clear()` runs teardown on every
 * remaining entry — call from the effect's deactivate / release path
 * per the V2 active-memory rule ([[feedback_v2_active_memory_release]]).
 */
template<typename T>
class PerOutputState
{
public:
    using SetupCb = std::function<void(Output *, T &)>;
    using TeardownCb = std::function<void(Output *, T &)>;

    PerOutputState() = default;
    ~PerOutputState()
    {
        clear();
        if (m_addedConn) {
            QObject::disconnect(m_addedConn);
        }
        if (m_removedConn) {
            QObject::disconnect(m_removedConn);
        }
    }

    // Non-copyable, non-movable. T may carry non-movable GPU
    // resource handles; keeping the container fixed avoids
    // accidentally invalidating the map iterators kwin signals
    // capture by reference.
    PerOutputState(const PerOutputState &) = delete;
    PerOutputState &operator=(const PerOutputState &) = delete;
    PerOutputState(PerOutputState &&) = delete;
    PerOutputState &operator=(PerOutputState &&) = delete;

    void setSetupCallback(SetupCb cb)
    {
        m_setup = std::move(cb);
    }
    void setTeardownCallback(TeardownCb cb)
    {
        m_teardown = std::move(cb);
    }

    /**
     * @brief Auto-track output hotplug.
     *
     * Wires this container to `effects->screenAdded` / `screenRemoved`
     * so an output appearing during the active phase gets a default-
     * constructed entry + setup callback, and a disappearing output
     * runs teardown + erases. Safe to call without an active
     * EffectsHandler (no-op when `effects` is null).
     *
     * Pass an optional `parent` QObject so the connections share
     * its lifetime — usually the owning effect. Without a parent,
     * the destructor disconnects manually.
     */
    void bindToEffectsHandler(QObject *parent = nullptr)
    {
        if (!effects) {
            return;
        }
        if (m_addedConn) {
            QObject::disconnect(m_addedConn);
        }
        if (m_removedConn) {
            QObject::disconnect(m_removedConn);
        }
        m_addedConn = QObject::connect(effects, &EffectsHandler::screenAdded, parent,
                                       [this](Output *output) {
            (void)forOutput(output);
        });
        m_removedConn = QObject::connect(effects, &EffectsHandler::screenRemoved, parent,
                                         [this](Output *output) {
            releaseOutput(output);
        });
    }

    /**
     * @brief Return the state for @p output, allocating + running
     * the setup callback on first access.
     *
     * Returns `nullptr` only when @p output is null. The returned
     * pointer remains valid until `releaseOutput(output)`, `clear()`,
     * or destruction — `std::unordered_map` doesn't invalidate
     * element pointers on insert.
     */
    T *forOutput(Output *output)
    {
        if (!output) {
            return nullptr;
        }
        auto it = m_state.find(output);
        if (it != m_state.end()) {
            return &it->second;
        }
        auto [iter, inserted] = m_state.emplace(std::piecewise_construct,
                                                std::forward_as_tuple(output),
                                                std::forward_as_tuple());
        if (inserted && m_setup) {
            m_setup(output, iter->second);
        }
        return &iter->second;
    }

    /**
     * @brief Look up an existing entry without creating one.
     *
     * Returns nullptr when no entry exists for the output. Use this
     * in hot paths (post-pass, paintScreen) where allocating on
     * miss would be a bug.
     */
    T *peekOutput(Output *output)
    {
        if (!output) {
            return nullptr;
        }
        auto it = m_state.find(output);
        return (it != m_state.end()) ? &it->second : nullptr;
    }

    /**
     * @brief Run teardown + drop the entry for @p output.
     *
     * Safe to call for outputs that don't have an entry — no-op.
     * Called automatically by `bindToEffectsHandler` on
     * `screenRemoved`.
     */
    void releaseOutput(Output *output)
    {
        auto it = m_state.find(output);
        if (it == m_state.end()) {
            return;
        }
        if (m_teardown) {
            m_teardown(it->first, it->second);
        }
        m_state.erase(it);
    }

    /// Iterate every active entry.
    auto begin()
    {
        return m_state.begin();
    }
    auto end()
    {
        return m_state.end();
    }
    auto begin() const
    {
        return m_state.begin();
    }
    auto end() const
    {
        return m_state.end();
    }

    /**
     * @brief Run teardown on every entry, then erase.
     *
     * Used from the owning effect's deactivate / release-resources
     * path. Honours the V2 active-memory rule by giving the per-
     * output GPU resources a definite drop point. The screen-add /
     * screen-remove connections stay live, so a subsequent activate
     * still hot-tracks outputs.
     */
    void clear()
    {
        if (m_teardown) {
            for (auto &[output, state] : m_state) {
                m_teardown(output, state);
            }
        }
        m_state.clear();
    }

    bool empty() const
    {
        return m_state.empty();
    }
    std::size_t size() const
    {
        return m_state.size();
    }

private:
    std::unordered_map<Output *, T> m_state;
    SetupCb m_setup;
    TeardownCb m_teardown;
    QMetaObject::Connection m_addedConn;
    QMetaObject::Connection m_removedConn;
};

} // namespace KWin
