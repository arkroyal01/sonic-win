#!/usr/bin/env bash
# Automated VRAM leak test for the kwin Vulkan backend.
#
# Sanitises any existing kwin_x11 (including --crashes zombies), launches a
# fresh kwin_x11 --replace with KWIN_VULKAN_VMA_STATS=1 and
# KWIN_LOG_PERFORMANCE_DATA=1, then:
#   Pass 1 (sweep)  -- cycles every effect in effects.tsv SWEEP_ITERS times to
#                      detect whether overall VRAM grows monotonically.
#   Pass 2 (drill)  -- for each effect, isolates it (unloads all others),
#                      hammers it DRILL_ITERS times, samples VRAM before/after
#                      and aggregates per-effect paint cost from the perf CSV.
#
# Run from a TTY or SSH session, NOT from inside the kwin session being
# tested. Requires: xdotool, qdbus (qt6 or qt5), xterm.

set -uo pipefail

#------------------------------------------------------------------ config ----

SWEEP_ITERS="${SWEEP_ITERS:-20}"
DRILL_ITERS="${DRILL_ITERS:-20}"
LEAK_KIB_PER_ITER="${LEAK_KIB_PER_ITER:-10}"
LEAK_ALLOC_PER_ITER="${LEAK_ALLOC_PER_ITER:-1}"
WORK_DIR="${WORK_DIR:-/tmp/vram-leak}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
EFFECTS_TSV="${EFFECTS_TSV:-$SCRIPT_DIR/effects.tsv}"
KWIN_BIN="${KWIN_BIN:-kwin_x11}"

# qdbus binary name varies (qdbus, qdbus6, qdbus-qt6)
QDBUS=""
for c in qdbus qdbus6 qdbus-qt6; do
    if command -v "$c" >/dev/null 2>&1; then QDBUS=$c; break; fi
done
[[ -z "$QDBUS" ]] && { echo "no qdbus binary found" >&2; exit 1; }
command -v xdotool >/dev/null 2>&1 || { echo "xdotool not installed" >&2; exit 1; }
command -v xterm   >/dev/null 2>&1 || { echo "xterm not installed"   >&2; exit 1; }
command -v amdgpu_top >/dev/null 2>&1 \
    || { echo "amdgpu_top not installed (needed for VRAM cross-check)" >&2; exit 1; }
command -v xprop >/dev/null 2>&1 || { echo "xprop not installed (needed for blur/contrast triggers)" >&2; exit 1; }

KWIN_PID=""
SAVED_EFFECTS=""

#------------------------------------------------------------------ utils -----

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

dbus_effects() { "$QDBUS" org.kde.KWin /Effects org.kde.kwin.Effects."$@" 2>/dev/null; }
kga_invoke()   { "$QDBUS" org.kde.kglobalaccel /component/kwin org.kde.kglobalaccel.Component.invokeShortcut "$1" >/dev/null 2>&1; }

cleanup() {
    log "cleanup: restoring effects + relaunching kwin without test env"
    if [[ -n "$SAVED_EFFECTS" ]]; then
        local now
        now=$(dbus_effects loadedEffects | tr ',' ' ' | tr -d '()')
        for e in $now; do
            dbus_effects unloadEffect "$e" >/dev/null 2>&1 || true
        done
        for e in $(echo "$SAVED_EFFECTS" | tr ',' ' '); do
            [[ -n "$e" ]] && dbus_effects loadEffect "$e" >/dev/null 2>&1 || true
        done
    fi
    if [[ -n "$KWIN_PID" ]]; then
        kill "$KWIN_PID" 2>/dev/null || true
        wait "$KWIN_PID" 2>/dev/null || true
    fi
    # Bring the user's compositor back without the test instrumentation,
    # so they're not left without a window manager.
    setsid "$KWIN_BIN" --replace >/dev/null 2>&1 </dev/null &
    disown 2>/dev/null || true
}
trap cleanup EXIT INT TERM

#---------------------------------------------------- session sanitisation ----

sanitize_kwin() {
    log "killing any existing kwin_x11 (incl. --crashes zombies)"
    pkill -9 -x kwin_x11 2>/dev/null || true
    local waited=0
    while pgrep -x kwin_x11 >/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
        if (( waited > 50 )); then
            echo "kwin_x11 won't die after 5s" >&2; exit 1
        fi
    done
}

launch_kwin() {
    rm -rf -- "$WORK_DIR"
    mkdir -p "$WORK_DIR"
    cd "$WORK_DIR"
    log "launching $KWIN_BIN --replace (cwd=$WORK_DIR)"
    # cd to WORK_DIR so the "kwin perf *.csv" files land here.
    KWIN_VULKAN_VMA_STATS=1 \
    KWIN_LOG_PERFORMANCE_DATA=1 \
        "$KWIN_BIN" --replace >"$WORK_DIR/kwin.stdout" 2>"$WORK_DIR/kwin.stderr" &
    KWIN_PID=$!
    log "kwin pid=$KWIN_PID; waiting for DBus interface"
    local waited=0
    until dbus_effects loadedEffects >/dev/null 2>&1; do
        sleep 0.2
        waited=$((waited + 1))
        if ! kill -0 "$KWIN_PID" 2>/dev/null; then
            echo "kwin died during startup; see $WORK_DIR/kwin.stderr" >&2; exit 1
        fi
        if (( waited > 100 )); then
            echo "kwin DBus interface never came up" >&2; exit 1
        fi
    done
    sleep 3   # let baseline allocations settle
    SAVED_EFFECTS=$(dbus_effects loadedEffects | tr -d '()' | tr -d ' ')
    log "baseline loaded effects: $SAVED_EFFECTS"
}

#---------------------------------------------------------- vram sampling ----
# Returns "allocs kib_used kib_reserved blocks" on stdout.
# Forces ~5s of repaints first so the 300-frame-cadence telemetry refreshes.

sample_vram() {
    local i
    for i in $(seq 1 12); do
        xdotool mousemove_relative -- 7 0 2>/dev/null || true
        xdotool mousemove_relative -- -- -7 0 2>/dev/null || true
        sleep 0.45
    done
    # KWin's stderr is intercepted by Qt's systemd journal handler under a
    # user session, so we read VMA stats from journald instead of the
    # captured kwin.stderr file.
    journalctl --user _PID="$KWIN_PID" -o cat --since "30 seconds ago" 2>/dev/null \
      | grep -E "VMA stats \[frame\]" \
      | tail -n 1 \
      | sed -nE 's/.*VMA stats \[frame\]: ([0-9]+) live allocations, ([0-9]+) KiB used, ([0-9]+) KiB reserved across ([0-9]+) blocks.*/\1 \2 \3 \4/p'
}

# Independent VRAM reading from amdgpu_top -p (kernel-side per-process view).
# Returns "vram_mib gtt_mib" for the harness's kwin PID, or "0 0" if missing.
sample_vram_amdgpu() {
    amdgpu_top -p 2>/dev/null \
      | awk -v pid="$KWIN_PID" '
            /^    kwin_x11/ {
                if (index($0, "(" pid ")") == 0 && index($0, "( " pid ")") == 0) next
                if (match($0, /VRAM[[:space:]]+[0-9]+[[:space:]]MiB/)) {
                    s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s); v = s
                }
                if (match($0, /GTT[[:space:]]+[0-9]+[[:space:]]MiB/)) {
                    s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s); g = s
                }
                printf "%d %d\n", v, g
                exit
            }
        '
}

# Convenience: vma fields + amdgpu fields as a single space-separated row.
# "vma_allocs vma_kib_used vma_kib_reserved vma_blocks gpu_vram_mib gpu_gtt_mib"
sample_vram_both() {
    local vma amd
    vma=$(sample_vram)
    [[ -z "$vma" ]] && vma="0 0 0 0"
    amd=$(sample_vram_amdgpu)
    [[ -z "$amd" ]] && amd="0 0"
    echo "$vma $amd"
}

#----------------------------------------------------- effect triggering -----

trigger_effect() {
    local id="$1" category="$2" detail="$3"
    case "$category" in
        overlay)
            # The effect plugin must be loaded for activation to do anything.
            dbus_effects loadEffect "$id" >/dev/null 2>&1 || true
            case "$detail" in
                kga:*)
                    local action="${detail#kga:}"
                    kga_invoke "$action"
                    sleep 0.7
                    kga_invoke "$action"
                    ;;
                kga2:*)
                    local pair="${detail#kga2:}"
                    local on="${pair%%:*}" off="${pair##*:}"
                    kga_invoke "$on"
                    sleep 0.7
                    kga_invoke "$off"
                    ;;
                kgaN:*)
                    # Round-robin across listed actions, one per call,
                    # using a state file so successive calls hit different
                    # activation variants over the 20 drill iterations.
                    local list="${detail#kgaN:}"
                    local rrfile="$WORK_DIR/kga-rr-$id"
                    local n
                    n=$(( $(cat "$rrfile" 2>/dev/null || echo 0) + 1 ))
                    echo "$n" > "$rrfile"
                    IFS='|' read -ra _actions <<< "$list"
                    local pick="${_actions[$(( (n - 1) % ${#_actions[@]} ))]}"
                    kga_invoke "$pick"
                    sleep 0.7
                    kga_invoke "$pick"
                    ;;
                load|*)
                    # No external activation possible; the plugin's mere
                    # presence is the exercise (debug overlays, input-driven
                    # effects). Cycle the plugin to fault its resources in
                    # and out.
                    sleep 0.4
                    dbus_effects unloadEffect "$id" >/dev/null 2>&1 || true
                    sleep 0.2
                    dbus_effects loadEffect "$id"   >/dev/null 2>&1 || true
                    ;;
            esac
            sleep 0.3
            ;;
        winevent)
            # Spawn xterm, give it time to map (fires window-add animations),
            # then close it (fires window-close animations).
            xterm -geometry 80x20 -e "sleep 0.5" >/dev/null 2>&1 &
            local xpid=$!
            sleep 0.9
            kill "$xpid" 2>/dev/null || true
            wait "$xpid" 2>/dev/null || true
            ;;
        winaction)
            # Spawn xterm, invoke a window-targeted kga action on it (the
            # most-recently-mapped window typically becomes focused), then kill.
            xterm -geometry 80x20 -e "sleep 2" >/dev/null 2>&1 &
            local xpid=$!
            sleep 0.6
            local wid
            wid=$(xdotool search --pid "$xpid" 2>/dev/null | tail -n 1)
            [[ -n "$wid" ]] && xdotool windowactivate "$wid" 2>/dev/null || true
            sleep 0.2
            case "$detail" in
                kga:*) kga_invoke "${detail#kga:}" ;;
            esac
            sleep 0.7
            kill "$xpid" 2>/dev/null || true
            wait "$xpid" 2>/dev/null || true
            ;;
        twinswap)
            # Two xterms, swap focus -- exercises windowActivated and
            # stackingOrderChanged signals (diminactive, slideback).
            xterm -geometry 60x15+100+100 -e "sleep 3" >/dev/null 2>&1 &
            local x1=$!
            xterm -geometry 60x15+400+300 -e "sleep 3" >/dev/null 2>&1 &
            local x2=$!
            sleep 0.7
            local w1 w2
            w1=$(xdotool search --pid "$x1" 2>/dev/null | tail -n 1)
            w2=$(xdotool search --pid "$x2" 2>/dev/null | tail -n 1)
            if [[ -n "$w1" && -n "$w2" ]]; then
                xdotool windowactivate "$w1" 2>/dev/null; sleep 0.35
                xdotool windowactivate "$w2" 2>/dev/null; sleep 0.35
                xdotool windowactivate "$w1" 2>/dev/null; sleep 0.35
            fi
            kill "$x1" "$x2" 2>/dev/null || true
            wait "$x1" "$x2" 2>/dev/null || true
            ;;
        blurwin)
            # Spawn xterm, stamp the named X11 atom on it so kwin's
            # blur/contrast plugin allocates its per-window capture texture,
            # then kill (which should free that texture under RAII).
            xterm -geometry 80x20 -e "sleep 2.5" >/dev/null 2>&1 &
            local xpid=$!
            sleep 0.7
            local wid
            wid=$(xdotool search --pid "$xpid" 2>/dev/null | tail -n 1)
            if [[ -n "$wid" ]]; then
                xprop -id "$wid" -f "$detail" 32c -set "$detail" 0 2>/dev/null || true
                sleep 1.0   # let kwin notice the PropertyNotify and allocate
            fi
            kill "$xpid" 2>/dev/null || true
            wait "$xpid" 2>/dev/null || true
            ;;
        desktop)
            xdotool key super+ctrl+Right 2>/dev/null || true
            sleep 0.5
            xdotool key super+ctrl+Left 2>/dev/null || true
            sleep 0.5
            ;;
        filter)
            dbus_effects unloadEffect "$id" >/dev/null 2>&1 || true
            sleep 0.25
            dbus_effects loadEffect "$id"   >/dev/null 2>&1 || true
            sleep 0.25
            ;;
    esac
}

isolate_effect() {
    local target="$1"
    local now
    now=$(dbus_effects loadedEffects | tr ',' ' ' | tr -d '()')
    for e in $now; do
        [[ "$e" == "$target" ]] && continue
        dbus_effects unloadEffect "$e" >/dev/null 2>&1 || true
    done
    dbus_effects loadEffect "$target" >/dev/null 2>&1 || true
    sleep 0.5
}

#---------------------------------------------- effect table iteration -------

# Skips comments/blanks. Calls the supplied function with id, cat, detail, perfcls.
foreach_effect() {
    local fn="$1"
    while IFS=$'\t' read -r id category detail perfcls; do
        [[ -z "${id:-}" || "$id" =~ ^# ]] && continue
        "$fn" "$id" "$category" "$detail" "$perfcls"
    done < "$EFFECTS_TSV"
}

#---------------------------------------------------------- pass 1: sweep ----

pass_sweep() {
    log "pass 1: sweep ($SWEEP_ITERS iterations across all effects)"
    echo "iter,wall_s,vma_allocs,vma_kib_used,vma_kib_reserved,vma_blocks,gpu_vram_mib,gpu_gtt_mib" \
      > "$WORK_DIR/sweep.csv"
    local t0=$SECONDS
    for ((i=1; i<=SWEEP_ITERS; i++)); do
        foreach_effect _sweep_one
        local s
        s=$(sample_vram_both)
        printf '%d,%d,%s\n' "$i" "$((SECONDS - t0))" "$(echo "$s" | tr ' ' ,)" \
          >> "$WORK_DIR/sweep.csv"
        log "  sweep iter $i/$SWEEP_ITERS  vma:[$(echo "$s" | awk '{print $1,$2"K",$3"K",$4"b"}')]  gpu:[$(echo "$s" | awk '{print $5"MiB",$6"GTT"}')]"
    done
}
_sweep_one() { trigger_effect "$1" "$2" "$3"; }

#---------------------------------------------------------- pass 2: drill ----

pass_drill() {
    log "pass 2: per-effect isolated drill"
    echo "effect,category,base_allocs,base_kib,base_vram_mib,base_gtt_mib,post_allocs,post_kib,post_vram_mib,post_gtt_mib,kib_delta,alloc_delta,vram_delta_mib,gtt_delta_mib,mean_paint_ns,p95_paint_ns,p99_paint_ns,max_paint_ns,stddev_paint_ns,cov_pct,n_paints" \
      > "$WORK_DIR/drill.csv"
    foreach_effect _drill_one
}

_drill_one() {
    local id="$1" cat="$2" detail="$3" perfcls="$4"
    log "  drill: $id ($cat)"
    isolate_effect "$id"
    sleep 1
    local base
    base=$(sample_vram_both)
    local b_a b_k b_vram b_gtt
    b_a=$(echo "$base" | awk '{print $1}')
    b_k=$(echo "$base" | awk '{print $2}')
    b_vram=$(echo "$base" | awk '{print $5}')
    b_gtt=$(echo "$base" | awk '{print $6}')

    local t_start_ns
    t_start_ns=$(date +%s%N)
    local i
    for ((i=1; i<=DRILL_ITERS; i++)); do
        trigger_effect "$id" "$cat" "$detail"
    done
    local t_end_ns
    t_end_ns=$(date +%s%N)

    sleep 1
    local post
    post=$(sample_vram_both)
    local p_a p_k p_vram p_gtt
    p_a=$(echo "$post" | awk '{print $1}')
    p_k=$(echo "$post" | awk '{print $2}')
    p_vram=$(echo "$post" | awk '{print $5}')
    p_gtt=$(echo "$post" | awk '{print $6}')

    local mean_ns=0 p95_ns=0 p99_ns=0 max_ns=0 sd_ns=0 cov_pct=0 n_paints=0
    if [[ "$perfcls" != "-" && -n "$perfcls" ]]; then
        read -r mean_ns p95_ns p99_ns max_ns sd_ns cov_pct n_paints \
            < <(compute_perf "$perfcls" "$t_start_ns" "$t_end_ns")
    fi

    printf '%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%s,%s,%s,%s\n' \
        "$id" "$cat" \
        "$b_a" "$b_k" "$b_vram" "$b_gtt" \
        "$p_a" "$p_k" "$p_vram" "$p_gtt" \
        "$((p_k - b_k))" "$((p_a - b_a))" "$((p_vram - b_vram))" "$((p_gtt - b_gtt))" \
        "$mean_ns" "$p95_ns" "$p99_ns" "$max_ns" "$sd_ns" "$cov_pct" "$n_paints" \
      >> "$WORK_DIR/drill.csv"
}

#------------------------------------------------- perf-csv aggregation ------

compute_perf() {
    local cls="$1" t_start="$2" t_end="$3"
    local csv
    csv=$(ls "$WORK_DIR"/kwin\ perf\ paint\ detail*.csv 2>/dev/null | head -1)
    [[ -z "$csv" || ! -f "$csv" ]] && { echo "0 0 0 0 0 0 0"; return; }
    # Emits: mean_ns p95_ns p99_ns max_ns stddev_ns cov_pct n_paints
    awk -F, -v cls="KWin::${cls}" -v ts="$t_start" -v te="$t_end" '
        NR == 1 { next }
        $2+0 < ts || $2+0 > te { next }
        {
            n = split($6, parts, ";")
            for (i = 1; i <= n; i++) {
                p = parts[i]
                eq = index(p, "=")
                if (eq == 0) continue
                name = substr(p, 1, eq - 1)
                if (index(name, cls) != 1) continue
                val = substr(p, eq + 1) + 0
                sum += val
                sumsq += val * val
                count++
                arr[count] = val
                if (val > mx) mx = val
            }
        }
        END {
            if (count == 0) { print "0 0 0 0 0 0 0"; exit }
            for (i = 2; i <= count; i++) {
                k = arr[i]; j = i - 1
                while (j >= 1 && arr[j] > k) { arr[j+1] = arr[j]; j-- }
                arr[j+1] = k
            }
            mean = sum / count
            p95 = arr[int(count * 0.95) > 0 ? int(count * 0.95) : 1]
            p99 = arr[int(count * 0.99) > 0 ? int(count * 0.99) : 1]
            # Population variance: E[X^2] - (E[X])^2
            var = (sumsq / count) - (mean * mean)
            if (var < 0) var = 0
            sd = sqrt(var)
            cov = (mean > 0) ? (sd / mean) * 100.0 : 0
            printf "%d %d %d %d %d %.1f %d\n", mean, p95, p99, mx, sd, cov, count
        }
    ' "$csv"
}

#---------------------------------------------------------- verdict ----------

print_verdict() {
    echo
    echo "=== VRAM leak test ==="

    # Sweep deltas across both VMA and amdgpu_top columns.
    # Columns: iter,wall_s,vma_allocs,vma_kib_used,vma_kib_reserved,vma_blocks,gpu_vram_mib,gpu_gtt_mib
    local first last
    first=$(awk -F, 'NR==2' "$WORK_DIR/sweep.csv")
    last=$(awk -F, 'END'    "$WORK_DIR/sweep.csv")
    local d_alloc=$(($(echo "$last" | cut -d, -f3) - $(echo "$first" | cut -d, -f3)))
    local d_kib=$(($(echo   "$last" | cut -d, -f4) - $(echo "$first" | cut -d, -f4)))
    local d_vram=$(($(echo  "$last" | cut -d, -f7) - $(echo "$first" | cut -d, -f7)))
    local d_gtt=$(($(echo   "$last" | cut -d, -f8) - $(echo "$first" | cut -d, -f8)))
    printf 'Sweep over %d iterations (last - first):\n' "$SWEEP_ITERS"
    printf '  VMA self-report : %+d allocations, %+d KiB used\n'  "$d_alloc" "$d_kib"
    printf '  amdgpu_top      : %+d MiB VRAM,    %+d MiB GTT\n'   "$d_vram"  "$d_gtt"

    echo
    echo "Top leakers (per-iteration post-delta, drill pass; columns: VMA KiB | VMA allocs | amdgpu VRAM MiB):"
    awk -F, -v thr_k="$LEAK_KIB_PER_ITER" -v thr_a="$LEAK_ALLOC_PER_ITER" -v n="$DRILL_ITERS" '
        NR == 1 { next }
        {
            kpi  = $11 / n
            api  = $12 / n
            vpi  = $13 / n
            flag = "[clean]"
            if (kpi > thr_k || api > thr_a || vpi > 0) flag = "[LEAK]"
            else if (kpi > 0 || api > 0) flag = "[growth]"
            printf "  %-26s VMA %+6d KiB/iter %+4d allocs/iter   amdgpu %+5d MiB/iter   %s\n",
                $1, kpi, api, vpi, flag
        }
    ' "$WORK_DIR/drill.csv" | sort -k4 -rn | head -15

    echo
    echo "Top frametime cost (mean paint, drill pass):"
    awk -F, '
        NR == 1 { next }
        $21 + 0 > 0 { printf "  %-26s mean %6.2f ms  p99 %6.2f ms  max %6.2f ms  (n=%s)\n",
            $1, $15 / 1e6, $17 / 1e6, $18 / 1e6, $21 }
    ' "$WORK_DIR/drill.csv" | sort -k3 -rn | head -15

    echo
    echo "Frametime jitter (high CoV%% = inconsistent / stuttery):"
    awk -F, '
        NR == 1 { next }
        $21 + 0 > 0 {
            # Tail-spread is more user-perceptible than CoV alone:
            # how much further out is the worst frame vs the mean?
            tail_x = ($15 + 0 > 0) ? ($18 / $15) : 0
            printf "  %-26s CoV %5.1f%%  p99/mean %4.1fx  max/mean %4.1fx  (mean %5.2f ms)\n",
                $1, $20, ($15 > 0 ? $17 / $15 : 0), tail_x, $15 / 1e6
        }
    ' "$WORK_DIR/drill.csv" | sort -k3 -rn | head -15

    echo
    echo "Artifacts:"
    echo "  $WORK_DIR/sweep.csv"
    echo "  $WORK_DIR/drill.csv"
    echo "  journalctl --user _PID=$KWIN_PID  (raw VMA stats lines)"
    ls "$WORK_DIR"/kwin\ perf*.csv 2>/dev/null | sed 's/^/  /'
}

#---------------------------------------------------------- main -------------

main() {
    sanitize_kwin
    launch_kwin
    pass_sweep
    pass_drill
    print_verdict
}

main "$@"
