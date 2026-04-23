#!/bin/bash
# run_comparison_cpu.sh
# Compares: Baseline vs Counting Mode vs Sampling Mode
# UPDATED: Includes CPU Frequency Locking for scientific rigor

BINARY="./benchmark_modes"
OUTPUT_DIR="comparison_results"
RESULTS_FILE="mode_comparison.csv"

# =========================================
# STEP 0: CPU Frequency Locking Setup
# =========================================
# Check if running as root (required for CPU locking and perf)
if [ "$EUID" -ne 0 ]; then 
  echo "Please run with sudo: sudo ./run_comparison_cpu.sh"
  exit 1
fi

# Function to restore CPU settings on exit (even if script fails)
restore_cpu_settings() {
    echo ""
    echo "========================================="
    echo "Cleanup: Restoring CPU Frequency"
    echo "========================================="
    
    # Restore Governor
    for CPUFREQ in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do 
      echo "$ORIG_GOVERNOR" | tee $CPUFREQ > /dev/null
    done
    
    # Re-enable Turbo Boost (if previously disabled)
    if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
      echo "$ORIG_TURBO" | tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null
    fi
    
    echo "CPU settings restored."
}

# Save original settings
ORIG_GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    ORIG_TURBO=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)
    TURBO_AVAILABLE=1
else
    TURBO_AVAILABLE=0
fi

# Set trap to restore settings on EXIT (Ctrl+C or completion)
trap restore_cpu_settings EXIT

# Lock CPU Settings
echo "========================================="
echo "STEP 0: Locking CPU Frequency"
echo "========================================="
echo "Original Governor: $ORIG_GOVERNOR"
echo "Original Turbo: $ORIG_TURBO"

echo "Setting CPU governor to 'performance'..."
for CPUFREQ in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do 
  echo performance | tee $CPUFREQ > /dev/null
done

if [ $TURBO_AVAILABLE -eq 1 ]; then
    echo "Disabling Turbo Boost..."
    echo 0 | tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null
fi

echo "CPU Frequency locked. Starting benchmarks..."
echo ""

# =========================================
# Main Experiment Logic
# =========================================
mkdir -p $OUTPUT_DIR
echo "test_type,sample_freq,execution_time,overhead_percent" > $RESULTS_FILE

# Test frequencies for sampling mode
FREQUENCIES=(100 500 1000 2000 4000 8000)

echo "========================================="
echo "Performance Mode Comparison Experiment"
echo "========================================="
echo ""

# 1. BASELINE (no perf at all)
echo "[1/3] Running BASELINE (no monitoring)..."
BASELINE_TIMES=()
for i in {1..5}; do
    OUTPUT=$($BINARY 0 2>&1)
    TIME=$(echo "$OUTPUT" | grep "Execution time" | awk '{print $3}')
    BASELINE_TIMES+=($TIME)
    echo "  Run $i: $TIME seconds"
done

# Calculate average baseline
BASELINE_SUM=0
for t in "${BASELINE_TIMES[@]}"; do
    BASELINE_SUM=$(echo "$BASELINE_SUM + $t" | bc -l)
done
BASELINE_AVG=$(echo "scale=6; $BASELINE_SUM / ${#BASELINE_TIMES[@]}" | bc -l)
echo "  Average Baseline: $BASELINE_AVG seconds"
echo "0,baseline,$BASELINE_AVG,0.0" >> $RESULTS_FILE
echo ""

# 2. COUNTING MODE (perf stat - minimal overhead)
echo "[2/3] Running COUNTING MODE (perf stat)..."
COUNTING_TIMES=()
for i in {1..5}; do
    OUTPUT=$(perf stat -e cycles,instructions,cache-references,cache-misses \
                $BINARY 1 2>&1)
    TIME=$(echo "$OUTPUT" | grep "Execution time" | awk '{print $3}')
    COUNTING_TIMES+=($TIME)
    echo "  Run $i: $TIME seconds"
done

# Calculate average counting
COUNTING_SUM=0
for t in "${COUNTING_TIMES[@]}"; do
    COUNTING_SUM=$(echo "$COUNTING_SUM + $t" | bc -l)
done
COUNTING_AVG=$(echo "scale=6; $COUNTING_SUM / ${#COUNTING_TIMES[@]}" | bc -l)
COUNTING_OVERHEAD=$(echo "scale=2; (($COUNTING_AVG - $BASELINE_AVG) / $BASELINE_AVG) * 100" | bc -l)
echo "  Average Counting: $COUNTING_AVG seconds (Overhead: ${COUNTING_OVERHEAD}%)"
echo "1,counting,$COUNTING_AVG,$COUNTING_OVERHEAD" >> $RESULTS_FILE
echo ""

# 3. SAMPLING MODE (perf record - higher overhead)
echo "[3/3] Running SAMPLING MODE (perf record) at different frequencies..."
echo "Frequency,Time,Overhead" > $OUTPUT_DIR/sampling_details.csv
for FREQ in "${FREQUENCIES[@]}"; do
    echo "  Testing sampling at ${FREQ} Hz..."
    
    SAMPLE_TIMES=()
    for run in {1..3}; do
        OUTPUT=$(perf record -F $FREQ -o $OUTPUT_DIR/perf_${FREQ}_run${run}.data \
                    --quiet $BINARY 2 2>&1)
        TIME=$(echo "$OUTPUT" | grep "Execution time" | awk '{print $3}')
        SAMPLE_TIMES+=($TIME)
    done
    
    # Calculate average for this frequency
    SAMPLE_SUM=0
    for t in "${SAMPLE_TIMES[@]}"; do
        SAMPLE_SUM=$(echo "$SAMPLE_SUM + $t" | bc -l)
    done
    SAMPLE_AVG=$(echo "scale=6; $SAMPLE_SUM / ${#SAMPLE_TIMES[@]}" | bc -l)
    
    # Calculate overhead percentage
    OVERHEAD=$(echo "scale=2; (($SAMPLE_AVG - $BASELINE_AVG) / $BASELINE_AVG) * 100" | bc -l)
    
    echo "    ${FREQ} Hz -> ${SAMPLE_AVG}s (Overhead: ${OVERHEAD}%)"
    echo "2,sampling_${FREQ},$SAMPLE_AVG,$OVERHEAD" >> $RESULTS_FILE
    echo "$FREQ,$SAMPLE_AVG,$OVERHEAD" >> $OUTPUT_DIR/sampling_details.csv
    
    # Clean up perf data files
    rm -f $OUTPUT_DIR/perf_${FREQ}_run*.data
done

echo ""
echo "========================================="
echo "Experiment Complete!"
echo "========================================="
echo "Results saved to: $RESULTS_FILE"
echo ""
echo "SUMMARY:"
echo "  Baseline:       $BASELINE_AVG seconds"
echo "  Counting Mode:  $COUNTING_AVG seconds (${COUNTING_OVERHEAD}% overhead)"
echo "  Sampling Mode:  See graph for frequency-dependent overhead"
echo ""

# Display results
cat $RESULTS_FILE

# Trap will automatically trigger restore_cpu_settings here
#
# Lock CPU governor to performance mode
for CPU in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance | sudo tee $CPU
done

# Disable Intel Turbo Boost
echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
