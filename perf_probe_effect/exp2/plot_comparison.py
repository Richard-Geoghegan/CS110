# plot_comparison.py
# Run: python3 plot_comparison.py

import csv

import matplotlib.pyplot as plt
import numpy as np


def load_results(filename):
    results = {
        "baseline": {"time": 0, "overhead": 0},
        "counting": {"time": 0, "overhead": 0},
        "sampling": {"freqs": [], "times": [], "overheads": []},
    }

    with open(filename, "r") as f:
        reader = csv.reader(f)
        next(reader)  # skip header

        for row in reader:
            test_type = row[0]
            freq_or_mode = row[1]
            exec_time = float(row[2])
            overhead = float(row[3])

            if test_type == "0":  # baseline
                results["baseline"]["time"] = exec_time
                results["baseline"]["overhead"] = overhead
            elif test_type == "1":  # counting
                results["counting"]["time"] = exec_time
                results["counting"]["overhead"] = overhead
            elif test_type == "2":  # sampling
                # Extract frequency from "sampling_XXXX"
                freq = int(freq_or_mode.split("_")[1])
                results["sampling"]["freqs"].append(freq)
                results["sampling"]["times"].append(exec_time)
                results["sampling"]["overheads"].append(overhead)

    return results


def plot_comparison(results):
    baseline_time = results["baseline"]["time"]
    counting_time = results["counting"]["time"]
    counting_overhead = results["counting"]["overhead"]

    sampling_freqs = results["sampling"]["freqs"]
    sampling_times = results["sampling"]["times"]
    sampling_overheads = results["sampling"]["overheads"]

    # Create figure with 2 subplots
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))

    # ===== LEFT PLOT: Execution Time Comparison =====
    modes = [
        "Baseline\n(No Monitoring)",
        f"Counting\nMode\n({counting_overhead:.1f}% overhead)",
        "Sampling\nMode\n(Variable)",
    ]
    times = [baseline_time, counting_time, np.mean(sampling_times)]
    colors = ["#2ecc71", "#3498db", "#e74c3c"]

    bars = ax1.bar(
        modes, times, color=colors, edgecolor="black", linewidth=1.2, alpha=0.8
    )
    ax1.axhline(
        y=baseline_time,
        color="#2ecc71",
        linestyle="--",
        linewidth=2,
        alpha=0.5,
        label=f"Baseline ({baseline_time:.4f}s)",
    )

    # Add value labels on bars
    for bar, time in zip(bars, times):
        height = bar.get_height()
        ax1.annotate(
            f"{time:.4f}s",
            xy=(bar.get_x() + bar.get_width() / 2, height),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=10,
            fontweight="bold",
        )

    ax1.set_ylabel("Execution Time (seconds)", fontsize=12, fontweight="bold")
    ax1.set_title(
        "Mode Comparison: Average Execution Time",
        fontsize=14,
        fontweight="bold",
        pad=20,
    )
    ax1.legend(loc="upper left", fontsize=10)
    ax1.grid(axis="y", alpha=0.3, linestyle="--")
    ax1.set_ylim([0, max(times) * 1.2])

    # ===== RIGHT PLOT: Overhead vs Frequency =====
    # Plot counting mode as horizontal line
    ax2.axhline(
        y=counting_overhead,
        color="#3498db",
        linestyle="-",
        linewidth=3,
        label=f"Counting Mode ({counting_overhead:.1f}%)",
        alpha=0.7,
    )

    # Plot sampling mode overhead
    ax2.plot(
        sampling_freqs,
        sampling_overheads,
        "ro-",
        linewidth=2.5,
        markersize=10,
        label="Sampling Mode",
        alpha=0.8,
        markerfacecolor="#e74c3c",
        markeredgecolor="black",
        markeredgewidth=1.5,
    )

    # Fill area between counting and sampling to show difference
    ax2.fill_between(
        sampling_freqs,
        counting_overhead,
        sampling_overheads,
        alpha=0.3,
        color="#e74c3c",
        label="Additional Overhead from Sampling",
    )

    ax2.set_xlabel("Sampling Frequency (Hz)", fontsize=12, fontweight="bold")
    ax2.set_ylabel("Performance Overhead (%)", fontsize=12, fontweight="bold")
    ax2.set_title(
        "Overhead Comparison: Counting vs Sampling Mode",
        fontsize=14,
        fontweight="bold",
        pad=20,
    )
    ax2.legend(loc="upper left", fontsize=10)
    ax2.grid(True, alpha=0.3, linestyle="--")
    ax2.set_xticks(sampling_freqs)
    ax2.set_xticklabels(sampling_freqs, rotation=45)

    # Add annotations for key points
    max_overhead_idx = np.argmax(sampling_overheads)
    ax2.annotate(
        f"Max: {sampling_overheads[max_overhead_idx]:.1f}%",
        xy=(
            sampling_freqs[max_overhead_idx],
            sampling_overheads[max_overhead_idx],
        ),
        xytext=(10, 10),
        textcoords="offset points",
        fontsize=10,
        fontweight="bold",
        bbox=dict(boxstyle="round,pad=0.3", facecolor="yellow", alpha=0.7),
    )

    plt.tight_layout()
    plt.savefig("mode_comparison.png", dpi=300, bbox_inches="tight")
    print("✓ Graph saved as 'mode_comparison.png'")
    plt.show()

    # Print summary statistics
    print("\n" + "=" * 60)
    print("SUMMARY STATISTICS")
    print("=" * 60)
    print(f"Baseline Execution Time:     {baseline_time:.6f} seconds")
    print(f"Counting Mode Time:          {counting_time:.6f} seconds")
    print(f"Counting Mode Overhead:      {counting_overhead:.2f}%")
    print(f"Sampling Mode Avg Overhead:  {np.mean(sampling_overheads):.2f}%")
    print(
        f"Sampling Mode Max Overhead:  {max(sampling_overheads):.2f}% at {sampling_freqs[max_overhead_idx]} Hz"
    )
    print(
        f"Sampling Mode Min Overhead:  {min(sampling_overheads):.2f}% at {sampling_freqs[np.argmin(sampling_overheads)]} Hz"
    )
    print("=" * 60)
    print(
        f"\nKEY FINDING: Counting mode adds ~{counting_overhead:.1f}% overhead,"
    )
    print(
        f"while sampling mode adds {min(sampling_overheads):.1f}-{max(sampling_overheads):.1f}% overhead"
    )
    print(
        f"depending on frequency - up to {max(sampling_overheads)/counting_overhead:.1f}x MORE overhead!"
    )
    print("=" * 60)


if __name__ == "__main__":
    results = load_results("mode_comparison.csv")
    plot_comparison(results)
