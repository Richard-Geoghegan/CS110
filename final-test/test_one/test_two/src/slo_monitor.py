#!/usr/bin/env python3
"""
SLO Monitor: Watches OTel metrics stream for P99 latency breaches.
Triggers high-frequency perf sampling when SLO threshold exceeded.
"""
import json
import os
import socket
import time
from collections import deque
from statistics import mean

import requests

OTEL_METRICS_URL = os.getenv("OTEL_METRICS_URL", "http://localhost:4318/v1/metrics")
SLO_THRESHOLD_MS = float(os.getenv("SLO_THRESHOLD_MS", "500"))  # P99 > 500ms triggers
WINDOW_SIZE = 10  # Last 10 requests
TRIGGER_WINDOW_SEC = 15  # High-freq sampling duration
PERF_CTL_SOCK = "/tmp/perf_daemon_ctl"

latency_window = deque(maxlen=WINDOW_SIZE)
is_triggered = False
last_trigger_time = 0

def fetch_latest_latency():
    """Fetch latest latency from app-service (or parse OTel metrics)"""
    try:
        # Option 1: Direct HTTP probe (simpler for Phase 4)
        resp = requests.get("http://localhost:8080", timeout=2)
        data = resp.json()
        return data.get("latency_ms", 0)
    except:
        return 0

def check_slo_breach():
    """Calculate P99 from sliding window"""
    if len(latency_window) < 5:  # Need minimum samples
        return False
    
    sorted_latencies = sorted(latency_window)
    p99_idx = int(len(sorted_latencies) * 0.99)
    p99 = sorted_latencies[min(p99_idx, len(sorted_latencies)-1)]
    
    print(f"[SLO] P99: {p99:.2f}ms | Threshold: {SLO_THRESHOLD_MS}ms")
    return p99 > SLO_THRESHOLD_MS

def trigger_high_frequency_sampling():
    """Send TRIGGER_HIGH command to perf_daemon"""
    global is_triggered, last_trigger_time
    if is_triggered:
        return
    
    print(f"[SLO] BREACH DETECTED! Triggering 4kHz sampling...")
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(PERF_CTL_SOCK)
        sock.sendall(b"TRIGGER_HIGH\n")
        sock.close()
        is_triggered = True
        last_trigger_time = time.time()
    except Exception as e:
        print(f"[SLO] Failed to trigger daemon: {e}")

def revert_to_baseline():
    """Send REVERT_BASE command after window expires"""
    global is_triggered
    if not is_triggered:
        return
    
    elapsed = time.time() - last_trigger_time
    if elapsed >= TRIGGER_WINDOW_SEC:
        print(f"🔄 [SLO] Window expired. Reverting to 100Hz baseline...")
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(PERF_CTL_SOCK)
            sock.sendall(b"REVERT_BASE\n")
            sock.close()
            is_triggered = False
        except Exception as e:
            print(f"[SLO] Failed to revert daemon: {e}")

def main():
    print(f"[SLO Monitor] Watching P99 > {SLO_THRESHOLD_MS}ms")
    print(f"[SLO Monitor] Trigger window: {TRIGGER_WINDOW_SEC}s")
    
    while True:
        latency = fetch_latest_latency()
        if latency > 0:
            latency_window.append(latency)
        
        if check_slo_breach():
            trigger_high_frequency_sampling()
        else:
            revert_to_baseline()
        
        time.sleep(1)  # Check every second

if __name__ == "__main__":
    main()
