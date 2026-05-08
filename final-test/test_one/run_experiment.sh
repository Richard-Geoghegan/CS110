#!/usr/bin/env bash
# run_experiment.sh — Measure perf sampling overhead across frequencies
# Phases: baseline (no perf) → 100 Hz → 1000 Hz → 4000 Hz
# Outputs: overhead_results.csv, and launches plot_overlay.py when done.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET_URL="${TARGET_URL:-http://localhost:8080}"
VUS="${VUS:-25}"
DURATION="${DURATION:-30}"
RESULTS="$SCRIPT_DIR/overhead_results.csv"
DAEMON_BIN="$SCRIPT_DIR/perf_daemon"
FREQUENCIES=(0 100 1000 4000)   # 0 = baseline (daemon off)

# ------------------------------------------------------------------ helpers --

die() { echo "ERROR: $*" >&2; exit 1; }

wait_stack_ready() {
    echo "[EXP] Waiting for app-service to be ready..."
    for i in $(seq 1 30); do
        if curl -sf "$TARGET_URL" -o /dev/null 2>/dev/null; then
            echo "[EXP] Stack ready."
            return
        fi
        sleep 2
    done
    die "Stack not ready after 60s. Check: docker compose logs app-service"
}

resolve_cgroup_path() {
    local pid
    pid=$(docker inspect -f '{{.State.Pid}}' app-service 2>/dev/null) \
        || die "app-service container not found"
    local cg_rel
    cg_rel=$(awk -F: '{print $3}' /proc/"$pid"/cgroup | head -1)
    echo "/sys/fs/cgroup${cg_rel}/cgroup.procs"
}

run_load() {
    local label="$1"
    local out="$SCRIPT_DIR/results_${label}.json"
    local metrics_file="$SCRIPT_DIR/.metrics_tmp"

    echo "[EXP] Running load ($label): $VUS VUs × ${DURATION}s → $TARGET_URL"
    python3 "$SCRIPT_DIR/baseline_profiler.py" 2>&1 | tail -6

    cp "$SCRIPT_DIR/baseline_metrics.json" "$out"
    echo "[EXP] Results saved to $out"

    # Write metrics to a temp file so stdout isn't polluted
    python3 - <<PYEOF
import json
with open("$out") as f: r = json.load(f)
with open("$metrics_file", "w") as f:
    f.write(f"{r['p50_ms']},{r['p99_ms']},{r['rps']},{r['error_pct']}\n")
PYEOF
    cat "$metrics_file"
}

start_daemon() {
    local freq="$1" cg_path="$2"
    echo "[EXP] Starting perf_daemon @ ${freq} Hz (cgroup: $cg_path)"
    sudo "$DAEMON_BIN" "$cg_path" &
    DAEMON_PID=$!
    sleep 1   # let daemon initialize
    # Override sampling freq via control socket
    if [[ "$freq" != "100" ]]; then
        printf "TRIGGER_HIGH" | nc -U /tmp/perf_daemon_ctl 2>/dev/null || true
    fi
}

stop_daemon() {
    if [[ -n "${DAEMON_PID:-}" ]]; then
        sudo kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
        echo "[EXP] Daemon stopped."
    fi
}

# ------------------------------------------------------------------ main --

[[ ! -x "$DAEMON_BIN" ]] && die "perf_daemon not built. Run: make"

echo "[EXP] Starting Docker stack..."
docker compose down --remove-orphans --volumes -q 2>/dev/null || true
docker compose up -d --build
wait_stack_ready

DAEMON_PID=""
trap 'stop_daemon; docker compose down -q 2>/dev/null' EXIT

CG_PATH=$(resolve_cgroup_path)
echo "[EXP] cgroup path: $CG_PATH"

# Write CSV header
echo "freq_hz,p50_ms,p99_ms,rps,error_pct" > "$RESULTS"

for FREQ in "${FREQUENCIES[@]}"; do
    stop_daemon  # clean up previous daemon if any

    PHASE_LABEL="freq_${FREQ}hz"
    echo ""
    echo "=== Phase: $PHASE_LABEL ==="

    if [[ "$FREQ" -gt 0 ]]; then
        sudo "$DAEMON_BIN" "$CG_PATH" "$FREQ" &
        DAEMON_PID=$!
        sleep 2   # let daemon initialize and start sampling
        echo "[EXP] Daemon running @ ${FREQ} Hz (PID $DAEMON_PID)"
    else
        echo "[EXP] Baseline: no perf daemon."
    fi

    run_load "$PHASE_LABEL"
    METRICS=$(cat "$SCRIPT_DIR/.metrics_tmp")
    echo "$FREQ,$METRICS" >> "$RESULTS"
    echo "[EXP] $PHASE_LABEL → p50/p99/rps/err%: $METRICS"
done

echo ""
echo "=== Experiment complete ==="
cat "$RESULTS"
echo ""
echo "Launching overlay plot..."
python3 "$SCRIPT_DIR/plot_overlay.py" || echo "(plot_overlay.py failed — check Jaeger is reachable)"
