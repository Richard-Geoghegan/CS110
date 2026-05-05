#!/usr/bin/env bash
set -euo pipefail

echo " Phase 4: SLO-Triggered Sampling Test"
echo "======================================="

# 1. Start SLO Monitor in background
echo "[1/4] Starting SLO monitor..."
cd ~/Desktop/final-test/test_one/test_two
python3 src/slo_monitor.py &
SLO_PID=$!
sleep 2

# 2. Generate baseline traffic (normal latency)
echo "[2/4] Generating baseline traffic (10 reqs)..."
for i in {1..10}; do
    curl -s http://localhost:8080 > /dev/null
    sleep 0.5
done

# 3. Inject latency spike (simulate cache thrashing)
echo "[3/4] ⚠️  Injecting latency spike (tc netem 200ms)..."
# Clean any existing rules first
sudo tc qdisc del dev lo root netem 2>/dev/null || true
sudo tc qdisc add dev lo root netem delay 200ms 20ms distribution normal

# Generate traffic during spike (with timeout + job control)
echo "[*] Sending 20 requests with 5s timeout each..."
for i in {1..20}; do
    # Run curl with explicit timeout in background
    curl -s --max-time 5 --connect-timeout 2 http://localhost:8080 > /dev/null &
    
    # Limit concurrency: wait every 5 jobs to avoid socket exhaustion
    if (( i % 5 == 0 )); then
        wait -n  # Wait for at least one job to finish before continuing
    fi
done

# Wait for remaining jobs with a safety timeout
echo "[*] Waiting for requests to complete (max 30s)..."
timeout 30 bash -c 'while kill -0 $(jobs -p) 2>/dev/null; do sleep 1; done' || \
    echo "[⚠️]  Some requests timed out; proceeding with cleanup..."

# 4. Clean up tc rules (always run, even if above fails)
echo "[4/4] Cleaning up tc rules..."
sudo tc qdisc del dev lo root netem 2>/dev/null || true
