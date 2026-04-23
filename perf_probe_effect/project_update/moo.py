import matplotlib.pyplot as plt
import numpy as np


def calculate_moores_law(years, base_perf=1.0, saturation_year=5):
    """
    Models Moore's Law with a saturation point.
    Historically exponential, but physically limited (contradicting indefinite growth).
    """
    # Standard exponential growth: 2^(t/2)
    exponential_growth = base_perf * (2 ** (years / 2))

    # Sigmoid saturation factor to simulate physical limits (End of Moore's Law)
    # This creates the 'contradiction' to the historical trend
    saturation_factor = 1 / (1 + np.exp(0.8 * (years - saturation_year)))

    # Combined: Growth that flattens out
    return exponential_growth * saturation_factor


def calculate_cpu_optimization(years, base_eff=1.0, growth_rate=0.15):
    """
    Models performance gains from Algorithmic Optimization (CPU).
    This is linear/logarithmic improvement independent of transistor count.
    """
    # Steady improvement through better algorithms (e.g., O(n^2) to O(n log n))
    return base_eff * (1 + growth_rate * years)


def calculate_memory_optimization(years, base_eff=1.0, growth_rate=0.20):
    """
    Models performance gains from Memory Optimization (Space-Time Tradeoff).
    Using more RAM to save CPU cycles (caching, pre-computation).
    """
    # Often yields higher initial gains but depends on memory bandwidth limits
    return base_eff * (1 + growth_rate * years)


def main():
    # Setup Time Range (Next 10 Years)
    years = np.linspace(0, 10, 100)

    # Calculate Metrics
    # Note: For the graph, we normalize Hardware as 'Potential Speedup'
    # and Optimization as 'Efficiency Multiplier'
    hardware_gain = calculate_moores_law(years)
    cpu_opt_gain = calculate_cpu_optimization(years)
    mem_opt_gain = calculate_memory_optimization(years)

    # Combined Optimization (CPU + Memory strategies)
    combined_opt = cpu_opt_gain * (mem_opt_gain / 1.0)

    # Create the Plot with larger figure size for better spacing
    plt.style.use("seaborn-v0_8-darkgrid")
    fig, ax = plt.subplots(figsize=(14, 9))

    # Plot Lines with distinct styles and colors
    ax.plot(
        years,
        hardware_gain,
        label="Hardware Scaling\n(Moore's Law)",
        color="blue",
        linewidth=2.5,
        linestyle="--",
        marker="o",
        markersize=4,
        markevery=10,
    )
    ax.plot(
        years,
        cpu_opt_gain,
        label="CPU Optimization\n(Algorithms)",
        color="green",
        linewidth=2.5,
        marker="s",
        markersize=4,
        markevery=10,
    )
    ax.plot(
        years,
        mem_opt_gain,
        label="Memory Optimization\n(Space-Time Tradeoff)",
        color="orange",
        linewidth=2.5,
        marker="^",
        markersize=4,
        markevery=10,
    )
    ax.plot(
        years,
        combined_opt,
        label="Combined Software\nOptimization",
        color="red",
        linewidth=3,
        alpha=0.9,
        marker="d",
        markersize=5,
        markevery=10,
    )

    # Highlight the "Contradiction" Point (Saturation)
    sat_point_x = 5
    sat_point_y = calculate_moores_law(np.array([sat_point_x]))[0]
    ax.axvline(
        x=sat_point_x, color="gray", linestyle=":", alpha=0.7, linewidth=2
    )

    # Position annotation away from lines with better formatting
    ax.annotate(
        "Hardware Saturation\n(End of Moore's Law)",
        xy=(sat_point_x, sat_point_y),
        xytext=(sat_point_x + 1.5, sat_point_y + 3),
        arrowprops=dict(facecolor="darkred", shrink=0.05, width=2),
        fontsize=11,
        fontweight="bold",
        color="darkred",
        bbox=dict(
            boxstyle="round,pad=0.5",
            facecolor="white",
            edgecolor="darkred",
            alpha=0.8,
        ),
    )

    # Labels and Title with better spacing
    ax.set_xlabel(
        "Time (Years from Now)", fontsize=13, fontweight="bold", labelpad=15
    )
    ax.set_ylabel(
        "Relative Performance Efficiency (Normalized)",
        fontsize=13,
        fontweight="bold",
        labelpad=15,
    )
    ax.set_title(
        "Post-Moore's Law Era: Software Optimization vs. Hardware Scaling",
        fontsize=15,
        fontweight="bold",
        pad=20,
    )

    # Legend with better positioning and formatting
    ax.legend(
        loc="upper left",
        fontsize=11,
        framealpha=0.9,
        frameon=True,
        facecolor="white",
        edgecolor="black",
        bbox_to_anchor=(0.02, 0.98),
        ncol=1,
    )

    # Add context text box positioned to avoid overlap
    textstr = (
        "Key Insight:\n"
        "As hardware gains plateau,\n"
        "optimization becomes the\n"
        "primary driver of performance."
    )
    props = dict(
        boxstyle="round,pad=0.7",
        facecolor="lightyellow",
        alpha=0.9,
        edgecolor="orange",
        linewidth=1.5,
    )
    ax.text(
        0.68,
        0.15,
        textstr,
        transform=ax.transAxes,
        fontsize=11,
        verticalalignment="center",
        horizontalalignment="center",
        bbox=props,
        fontweight="bold",
    )

    # Add grid for better readability
    ax.grid(True, linestyle="--", alpha=0.6)

    # Set axis limits with padding to prevent label cutoff
    ax.set_xlim(-0.3, 10.3)
    ax.set_ylim(0, max(combined_opt) * 1.15)

    # Add tick marks for better readability
    ax.set_xticks(np.arange(0, 11, 1))
    ax.set_yticks(np.arange(0, max(combined_opt) + 1, 2))

    # Rotate x-axis labels if needed (not needed here but good practice)
    plt.setp(ax.get_xticklabels(), rotation=0, ha="center")

    # Show Plot with tight layout
    plt.tight_layout(rect=[0, 0.03, 1, 0.95])
    plt.show()


if __name__ == "__main__":
    main()
