# plot_overhead.py
# Run: python3 plot_overhead.py

import csv

import matplotlib.pyplot as plt


def load_results(filename):
    freqs = []
    times = []
    with open(filename, "r") as f:
        reader = csv.reader(f)
        next(reader)  # skip header
        for row in reader:
            freqs.append(int(row[0]))
            times.append(float(row[1]))
    return freqs, times


def plot_overhead(freqs, times):
    baseline = times[0]
    overheads = [(t - baseline) / baseline * 100 for t in times]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

    # Plot 1: Execution Time
    ax1.plot(freqs, times, "bo-", linewidth=2, markersize=8)
    ax1.axhline(
        y=baseline,
        color="r",
        linestyle="--",
        label=f"Baseline ({baseline:.4f}s)",
    )
    ax1.set_xlabel("Sampling Frequency (Hz)")
    ax1.set_ylabel("Execution Time (seconds)")
    ax1.set_title("Probe Effect: Execution Time vs Sampling Frequency")
    ax1.grid(True, alpha=0.3)
    ax1.legend()

    # Plot 2: Overhead Percentage
    ax2.plot(freqs, overheads, "ro-", linewidth=2, markersize=8)
    ax2.axhline(y=0, color="k", linestyle="-", linewidth=0.5)
    ax2.set_xlabel("Sampling Frequency (Hz)")
    ax2.set_ylabel("Overhead (%)")
    ax2.set_title("Performance Overhead Relative to Baseline")
    ax2.grid(True, alpha=0.3)
    ax2.set_xticks(freqs)
    ax2.set_xticklabels(freqs, rotation=45)

    plt.tight_layout()
    plt.savefig("probe_effect.png", dpi=300)
    print("Graph saved as 'probe_effect.png'")
    plt.show()


if __name__ == "__main__":
    freqs, times = load_results("overhead_results.csv")
    plot_overhead(freqs, times)
