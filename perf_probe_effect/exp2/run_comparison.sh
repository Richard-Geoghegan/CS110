#!/bin/bash
# run_comparison.sh
# Compares: Baseline vs Counting Mode vs Sampling Mode

BINARY="./benchmark_modes"
OUTPUT_DIR="comparison_results"
RESULTS_FILE="mode_comparison.csv"

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
