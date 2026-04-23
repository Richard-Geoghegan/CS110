#!/bin/bash
# run_experiment.sh

BINARY="./benchmark"
OUTPUT_DIR="perf_results"
RESULTS_FILE="overhead_results.csv"

mkdir -p $OUTPUT_DIR
echo "sample_freq,execution_time" > $RESULTS_FILE

# Frequencies to test (Hz)
FREQUENCIES=(100 500 1000 2000 4000 8000 16000 32000)

# Baseline (no perf)
echo "Running baseline (no perf)..."
BASELINE=$($BINARY | grep "Execution time" | awk '{print $3}')
echo "0,$BASELINE" >> $RESULTS_FILE
echo "Baseline: $BASELINE seconds"

for FREQ in "${FREQUENCIES[@]}"; do
    echo "Testing with sample frequency: $FREQ Hz"
    
    # Run perf with specific frequency and capture benchmark output
    OUTPUT=$(perf record -F $FREQ -o $OUTPUT_DIR/perf_$FREQ.data $BINARY 2>&1)
    TIME=$(echo "$OUTPUT" | grep "Execution time" | awk '{print $3}')
    
    echo "$FREQ,$TIME" >> $RESULTS_FILE
    echo "  Frequency: $FREQ Hz -> Time: $TIME seconds"
    
    # Clean up perf data (optional)
    # rm $OUTPUT_DIR/perf_$FREQ.data
done

echo ""
echo "Results saved to $RESULTS_FILE"
cat $RESULTS_FILE
