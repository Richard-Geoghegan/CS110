#!/usr/bin/env bash
set -euo pipefail
SOCK="/tmp/perf_daemon_ctl"

echo "⚠️ Simulating SLO breach (tc netem 150ms delay)..."
sudo tc qdisc add dev lo root netem delay 150ms 2>/dev/null || true
echo "TRIGGER_HIGH" | nc -U -q 1 $SOCK
sleep 12 # 15s window capped by daemon logic

echo "🔄 Reverting to baseline..."
echo "REVERT_BASE" | nc -U -q 1 $SOCK
sudo tc qdisc del dev lo root netem 2>/dev/null || true
echo "✅ Trigger cycle complete. Check daemon logs & Jaeger."
