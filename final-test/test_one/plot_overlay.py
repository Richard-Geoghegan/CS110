#!/usr/bin/env python3
"""
plot_overlay.py — Fetch traces from Jaeger and overlay perf PMU events with OTel spans.

Layout:
  Top panel:    Gantt of app-request spans (OTel), colored by latency
  Middle panel: perf pmu_sample events plotted as vertical ticks on the same timeline
  Bottom panel: Bar chart comparing P50/P99 latency per experiment phase from overhead_results.csv

Usage:
    python3 plot_overlay.py [--jaeger http://localhost:16686] [--service app-service] [--limit 50]
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

import requests
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import matplotlib.colors as mcolors
import pandas as pd

# ---------------------------------------------------------------------------

JAEGER_API = os.getenv("JAEGER_URL", "http://localhost:16686")
SERVICE    = os.getenv("OTEL_SERVICE_NAME", "app-service")
LIMIT      = 200
OUT_PNG    = Path(__file__).parent / "overlay_plot.png"
CSV_PATH   = Path(__file__).parent / "overhead_results.csv"

# ---------------------------------------------------------------------------

def fetch_traces(jaeger: str, service: str, limit: int) -> list[dict]:
    url = f"{jaeger}/api/traces"
    params = {"service": service, "limit": limit, "lookback": "1h"}
    try:
        r = requests.get(url, params=params, timeout=10)
        r.raise_for_status()
        return r.json().get("data", [])
    except Exception as e:
        print(f"[PLOT] Jaeger fetch failed: {e}")
        return []


def parse_spans(traces: list[dict]) -> tuple[list[dict], list[dict]]:
    """Return (app_spans, perf_spans) as flat lists with absolute times in ms."""
    app_spans, perf_spans = [], []
    for trace in traces:
        span_map = {s["spanID"]: s for s in trace.get("spans", [])}
        for span in trace.get("spans", []):
            start_us = span.get("startTime", 0)          # microseconds (Jaeger)
            dur_us   = span.get("duration", 0)
            start_ms = start_us / 1_000
            dur_ms   = dur_us   / 1_000
            tags     = {t["key"]: t["value"] for t in span.get("tags", [])}
            entry = {
                "trace_id": span.get("traceID", ""),
                "span_id":  span.get("spanID", ""),
                "op":       span.get("operationName", ""),
                "start_ms": start_ms,
                "end_ms":   start_ms + dur_ms,
                "dur_ms":   dur_ms,
                "tags":     tags,
            }
            if span["operationName"] == "app-request":
                app_spans.append(entry)
            elif span["operationName"] == "pmu_sample":
                perf_spans.append(entry)
    return app_spans, perf_spans


def plot(app_spans, perf_spans, csv_path: Path, out: Path):
    if not app_spans and not perf_spans:
        print("[PLOT] No spans to plot. Is Jaeger populated?")
        return

    # Normalize time to seconds from earliest event
    all_times = [s["start_ms"] for s in app_spans + perf_spans]
    t0 = min(all_times) if all_times else 0
    for s in app_spans + perf_spans:
        s["t_start"] = (s["start_ms"] - t0) / 1_000   # → seconds
        s["t_end"]   = (s["end_ms"]   - t0) / 1_000

    # Colour app spans by latency (cold=fast, hot=slow)
    lats = [s["dur_ms"] for s in app_spans] or [1]
    norm = mcolors.Normalize(vmin=min(lats), vmax=max(lats))
    cmap = plt.cm.RdYlGn_r

    has_csv = csv_path.exists()
    n_rows = 3 if has_csv else 2
    fig, axes = plt.subplots(n_rows, 1, figsize=(14, 3 * n_rows),
                              gridspec_kw={"height_ratios": [3, 1.5] + ([2] if has_csv else [])})
    fig.suptitle("perf PMU Events Overlaid on OTel Microservice Spans", fontsize=13, y=0.98)

    # --- Panel 1: OTel app-request Gantt ---
    ax1 = axes[0]
    for i, s in enumerate(sorted(app_spans, key=lambda x: x["t_start"])):
        color = cmap(norm(s["dur_ms"]))
        ax1.barh(i, s["t_end"] - s["t_start"], left=s["t_start"],
                 height=0.6, color=color, alpha=0.85)
    ax1.set_ylabel("Request #")
    ax1.set_xlabel("")
    ax1.set_title(f"OTel spans (app-request) — {len(app_spans)} total")
    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])
    plt.colorbar(sm, ax=ax1, orientation="vertical", label="Latency (ms)", pad=0.01)

    # --- Panel 2: perf PMU event ticks ---
    ax2 = axes[1]
    if perf_spans:
        times = [s["t_start"] for s in perf_spans]
        ax2.vlines(times, 0, 1, colors="steelblue", linewidth=0.4, alpha=0.6)
        ax2.set_title(f"perf PMU samples (cache-misses) — {len(perf_spans)} events")
    else:
        ax2.text(0.5, 0.5, "No perf PMU samples in Jaeger\n(is perf_daemon running?)",
                 ha="center", va="center", transform=ax2.transAxes, color="grey")
        ax2.set_title("perf PMU samples — (empty)")
    ax2.set_yticks([])
    ax2.set_xlabel("Time (s)")
    ax2.set_xlim(ax1.get_xlim())

    # --- Panel 3: Overhead bar chart (if CSV exists) ---
    if has_csv:
        ax3 = axes[2]
        df = pd.read_csv(csv_path)
        df["label"] = df["freq_hz"].apply(lambda f: "baseline" if f == 0 else f"{f} Hz")
        x = range(len(df))
        width = 0.35
        ax3.bar([i - width/2 for i in x], df["p50_ms"], width, label="P50", color="steelblue")
        ax3.bar([i + width/2 for i in x], df["p99_ms"], width, label="P99", color="tomato")
        ax3.set_xticks(list(x))
        ax3.set_xticklabels(df["label"])
        ax3.set_ylabel("Latency (ms)")
        ax3.set_title("Request Latency by perf Sampling Frequency (overhead measurement)")
        ax3.legend()
        # Annotate overhead % relative to baseline
        if len(df) > 1 and df.iloc[0]["p99_ms"] > 0:
            base_p99 = df.iloc[0]["p99_ms"]
            for i, row in df.iterrows():
                pct = ((row["p99_ms"] - base_p99) / base_p99) * 100
                if i > 0:
                    ax3.text(i + width/2, row["p99_ms"] + 0.5,
                             f"+{pct:.1f}%", ha="center", va="bottom", fontsize=8, color="darkred")

    plt.tight_layout()
    plt.savefig(out, dpi=150, bbox_inches="tight")
    print(f"[PLOT] Saved to {out}")


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jaeger",  default=JAEGER_API)
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--limit",   type=int, default=LIMIT)
    args = parser.parse_args()

    print(f"[PLOT] Fetching traces from {args.jaeger} (service={args.service}, limit={args.limit})")
    traces = fetch_traces(args.jaeger, args.service, args.limit)
    print(f"[PLOT] Got {len(traces)} traces")

    app_spans, perf_spans = parse_spans(traces)
    print(f"[PLOT] app-request spans: {len(app_spans)}, pmu_sample spans: {len(perf_spans)}")

    plot(app_spans, perf_spans, CSV_PATH, OUT_PNG)


if __name__ == "__main__":
    main()
