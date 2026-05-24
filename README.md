# sonic-win

## KWin/X11 with ports from KWin/Wayland, bug fixes, and other improvements

sonic-win is an X11 window manager and a compositing manager. Its primary usage is in conjunction with a Desktop Shell (e.g. [sonic-desktop](https://github.com/Sonic-DE/sonic-desktop)). sonic-win is designed to stay out of the way; users should not notice that they use a window manager at all. Nevertheless sonic-win provides a steep learning curve for advanced features, which are available, if they do not conflict with the primary mission. sonic-win does not have a dedicated targeted user group, but follows the targeted user group of the Desktop Shell using sonic-win as it's window manager.

## Why stay with X11?

On X11 there is [a working implementation](https://github.com/guiodic/material-decoration) for Locally Integrated Menus. Inertial scrolling works too, even under Wine. The scrolling in LibreOffice/Qt is more fluent, and some Chromium functionality, such as drag & drop, is more reliable than on Wayland. Global hotkeys are working for, e.g., push to talk in Telegram and the recording toggle in OBS Studio. Windows get restored at the positions they were closed, especially between different sessions. Applications like games know which screen is the primary one. You can also record your screen in remote desktop applications such as TeamViewer. There are some other minor aspects that just work and lead to an overall pleasant desktop experience.

KWin for X11 was created and for a long time maintained by the [KDE](https://kde.org) developers. Unfortunately, however, the KDE developers decided to abandon X11. In the wake of these events, KWin/X11 has been patched in 2025 by [guiodic](https://github.com/guiodic) at [guiodic/kwin-x11-improved](https://github.com/guiodic/kwin-x11-improved). The SonicDE project is here to pick up the baton, fix bugs, and make improvements to KWin/X11.

You may want to check out [guiodic's Plasma X11 improved guide](https://gist.github.com/guiodic/2bcc8f2f126d14b1f8a439f644fdc2c9) to get a better Plasma X11 experience. Note: Many instructions here are specific to intel video cards.

There is also a [critical comparison of X11 and Wayland by probonopd](https://gist.github.com/probonopd/9feb7c20257af5dd915e3a9f2d1f2277).

## Vulkan compositing backend

The `vulkan-backend` branch ships a Vulkan compositing backend for the X11 standalone session and **selects it by default** — see `options.cpp` (the default `compositingMode` is `VulkanCompositing`; KWin falls back to OpenGL if the device doesn't support Vulkan). Use `KWIN_COMPOSE=O` to force the OpenGL/GLX path, `KWIN_COMPOSE=V` to force Vulkan, or `KWIN_COMPOSE=N` to disable compositing entirely. The Vulkan backend is in active development and many features below are gated behind additional env vars.

All variables announce themselves with a one-shot log line at startup so a missing env var is obvious in `kwin_x11`'s stderr — silence means the value wasn't propagated to the process.

### General

| Env var | Default | Effect |
|---|---|---|
| `KWIN_COMPOSE` | (unset → Vulkan) | Force compositing backend: `V` = Vulkan, `O` = OpenGL/GLX, `Q` = QPainter (falls back to OpenGL on X11), `N` = no compositing. Without this set, Vulkan is selected with automatic OpenGL fallback if Vulkan isn't supported. |
| `KWIN_VULKAN_FORCE_CPU` | `0` | When `1`, forces a CPU-side surface-texture upload path. Diagnostic; bypasses DMA-BUF zero-copy when investigating texture issues. |
| `KWIN_VULKAN_PARTIAL_REPAINT` | `1` | Damage-driven partial-repaint with manual swapchain buffer-age tracking. Set to `0` to force full repaints. |
| `KWIN_VULKAN_VMA_STATS` | `0` | Periodic VMA (memory allocator) statistics dump for leak diagnosis. Frame-1 then every 300 frames. |

### Present-timing latency work (Phases 0–5)

The following set was added incrementally to leverage `VK_EXT_present_timing` + `VK_KHR_present_id2` + `VK_KHR_present_wait2` (available on X11 via [Mesa MR !39551](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/39551)).

| Env var | Default | Effect |
|---|---|---|
| `KWIN_VULKAN_PRESENT_TIMING` | `1` (when device + surface support it) | Async present-timing monitor backed by `vkWaitForPresent2KHR`. Set to `0` to fall back to the SGI/OML/software vsync-monitor cascade. |
| `KWIN_VULKAN_LATENCY_TELEMETRY` | `0` | Forces the RenderLoop perf CSV on (`kwin perf statistics <output>.csv` in cwd) with all per-frame columns: target/actual pageflip, render start/end, per-stage on-screen timestamps (`queue ops end`, `first pixel out`, `first pixel visible`), end-to-end latency, vblank-miss flag, safety margin, and (when GPU mode is on) GPU render duration. |
| `KWIN_VULKAN_GPU_RENDER_TIME` | `0` | Side-channel CSV column only: brackets the scene command buffer with `vkCmdWriteTimestamp` at TOP_OF_PIPE / BOTTOM_OF_PIPE and reports actual GPU work duration. Does **not** replace the CPU-measured `renderTime` fed to the scheduler — substituting it can hide main-thread dispatch stalls. |
| `KWIN_VULKAN_TIGHT_SCHED` | `0` | Replaces RenderJournal's `EMA + 2*variance` budget with a rolling p99 percentile of observed render durations, **and** runs a feedback controller that tunes `setPresentationSafetyMargin()` toward a target vblank-miss-rate band. Also replaces the legacy 10-frame double↔triple buffering hysteresis: enters triple-buffer immediately when the recent miss-rate crosses `TARGET_HIGH`, exits only after the *full* miss-rate window has stayed below `TARGET_LOW`. Triple-buffer adds ~1 vblank of latency, so the exit is conservative on purpose; the buffering and safety-margin loops react to the same signal but at different time scales (margin: per frame, sub-vblank; buffer depth: per spike, multi-vblank). |
| `KWIN_VULKAN_TIGHT_SCHED_STEP_FRACTION` | `0.015` | Per-frame safety-margin step as a fraction of one vblank. At 165 Hz: ~91 µs; at 60 Hz: ~250 µs. Same step count to saturate at any refresh rate. |
| `KWIN_VULKAN_TIGHT_SCHED_WINDOW_SECONDS` | `4.0` | Miss-rate window in wall-clock seconds. Multiplied by current refresh rate to size the ring buffer; auto-resizes if refresh rate drifts more than 15 %. Also gates how quickly the buffering hysteresis can recover from triple- back to double-buffer: triple→double requires the **entire** window to have stayed below `TARGET_LOW`, so shorter windows mean faster recovery to lower-latency double-buffer; longer windows are more conservative against thrashing. |
| `KWIN_VULKAN_TIGHT_SCHED_TARGET_LOW` | `0.001` (0.1 %) | If recent miss-rate falls below this, the controller shrinks the safety margin and may exit triple-buffer. Conservative — set higher (e.g. `0.005`) for faster shrinking. |
| `KWIN_VULKAN_TIGHT_SCHED_TARGET_HIGH` | `0.01` (1 %) | If recent miss-rate exceeds this, the controller grows the safety margin and enters triple-buffer. |
| `KWIN_VULKAN_PRESENT_TARGET` | `0` | Fills `VkPresentTimingInfoEXT.targetTime` with the scheduler's chosen vblank, plus the `NEAREST_REFRESH_CYCLE` flag — driver schedules the present to land exactly at the requested vblank rather than "the next one ≥ target". Best combined with `KWIN_VULKAN_TIGHT_SCHED=1` (which picks vblank-aligned targets); observed: 90 % of frames within 2 µs of the chosen target at 165 Hz. |

### Known limitations

- The Overview effect exhibits 100–200 ms main-thread dispatch stalls. These are upstream of the scheduler and not addressable by any of the knobs above — investigation is backlogged.
- `VK_EXT_present_timing` stages on Mesa MR !39551 are reported in `VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT` rather than raw `CLOCK_MONOTONIC`. Stage `FIRST_PIXEL_OUT` happens to be CLOCK_MONOTONIC ns; stage `QUEUE_OPERATIONS_END` is in a separate GPU-side clock — they are not directly comparable.

## sonic-win is not

* a standalone window manager (c.f. openbox, i3) and does not provide any functionality belonging to a Desktop Shell.
* a replacement for window managers designed for use with a specific Desktop Shell (e.g. GNOME Shell)
* a minimalistic window manager
* designed for use without compositing or for X11 network transparency, though both are possible.

## Contributing to sonic-win

We appreciate your interest in contributing! Please refer to the [CONTRIBUTING document](CONTRIBUTING.md) for everything you need to get started. To report a bug, please use the sonic-win bug tracker at [Issues · Sonic-DE/sonic-win](https://github.com/Sonic-DE/sonic-win/issues).

## Getting in contact

We'd love to hear from you on one of our channels. To get end-user support, please also check your distribution's chat or forum.

<img src="./.github/icons/bluesky.svg">&nbsp;[Bluesky](https://bsky.app/profile/sonicdesktop.bsky.social)&nbsp; <img src="./.github/icons/discord.svg">&nbsp;[Discord](https://discord.gg/cNZMQ62u5S) &nbsp; <img src="./.github/icons/mastodon.svg">&nbsp;[Mastodon](https://mastodon.social/@sonicdesktop) &nbsp; <img src="./.github/icons/matrix.svg">&nbsp;[Matrix](https://matrix.to/#/#sonicdesktop:matrix.org) &nbsp; <img src="./.github/icons/oftc.svg">&nbsp;[OFTC IRC](https://webchat.oftc.net/?channels=sonicde%2Csonicde-devel%2Csonicde-dist&uio=MT11bmRlZmluZWQb1) &nbsp; <img src="./.github/icons/telegram.svg">&nbsp;[Telegram](https://t.me/sonic_de) &nbsp; <img src="./.github/icons/x.svg">&nbsp;[X (Twitter)](https://x.com/SonicDesktop)
