# baseline_profiler.py
import json
import os
import statistics
import subprocess
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

TARGET = os.getenv("TARGET_URL", "http://localhost:8080")
VUS = int(os.getenv("VUS", "10"))
DURATION = int(os.getenv("DURATION", "30"))
OUT = "baseline_metrics.json"

def run_load():
    latencies, errors, reqs = [], 0, 0
    t0 = time.time()
    def worker():
        nonlocal errors, reqs
        try:
            s = time.monotonic()
            urllib.request.urlopen(TARGET, timeout=10)
            latencies.append((time.monotonic()-s)*1000)
            reqs += 1
        except: errors += 1

    print(f"[PROFILER] 🔫 {VUS} VUs for {DURATION}s...")
    with ThreadPoolExecutor(max_workers=VUS) as ex:
        futs = []
        while time.time() - t0 < DURATION:
            futs.append(ex.submit(worker))
            time.sleep(1.0/VUS)
        for f in as_completed(futs): pass

    latencies.sort()
    p50 = statistics.median(latencies) if latencies else 0
    p99 = latencies[int(len(latencies)*0.99)] if latencies else 0
    return {
        "vus": VUS, "duration_s": DURATION, "total_reqs": reqs, "errors": errors,
        "p50_ms": round(p50, 2), "p99_ms": round(p99, 2),
        "rps": round(reqs/DURATION, 2) if DURATION else 0,
        "error_pct": round(errors/reqs*100, 2) if reqs else 0
    }

def get_cgroup_stats():
    try:
        out = subprocess.check_output(
            ["docker", "stats", "--no-stream", "--format", "{{.Name}}|{{.CPUPerc}}|{{.MemUsage}}"],
            text=True
        )
        return {line.split("|")[0]: {"cpu": line.split("|")[1], "mem": line.split("|")[2]} for line in out.strip().split("\n")}
    except Exception as e: return {"error": str(e)}

def validate_slo(res):
    # Doc thresholds: P99 ≤ 500ms trigger, ≤1% CPU delta tolerance
    status = "PASS" if res["p99_ms"] <= 500 else "FAIL (exceeds Phase 4 trigger)"
    return status

if __name__ == "__main__":
    print("[PHASE1] 📊 Running baseline profiler...")
    res = run_load()
    res["cgroup_metrics"] = get_cgroup_stats()
    res["slo_validation"] = validate_slo(res)
    
    with open(OUT, "w") as f: json.dump(res, f, indent=2)
    print(json.dumps(res, indent=2))
    print(f"[PHASE1] ✅ Baseline saved to {OUT}")
