#!/usr/bin/env bash
# Cold single-run benchmark: encrypt, cooldown, decrypt -- each from cold cache.
# Run from a root terminal OUTSIDE the IDE for accurate results:
#   sudo ./cold_bench.sh
set -uo pipefail

WORKDIR="$(pwd)/.bench_workdir"
BIN="$(pwd)/teracrypt"

# Use the 4GB file on the NVMe SSD (not /tmp which is tmpfs/RAM)
INPUT="/home/strange/Documents/GitHub/4.data"
KEY="/home/strange/Documents/GitHub/test.key"

if [[ ! -x "$BIN" ]]; then
    echo "Build teracrypt first: make"
    exit 1
fi

if [[ ! -f "$INPUT" ]]; then
    echo "Input file not found: $INPUT"
    exit 1
fi
if [[ ! -f "$KEY" ]]; then
    echo "Key file not found: $KEY"
    exit 1
fi

mkdir -p "$WORKDIR"

echo "==================================================================="
echo "  System state"
echo "==================================================================="
echo "CPU boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo 'N/A')"
echo "Governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
echo "Available: $(free -h | awk '/Mem/{print $7}')"
echo "Binary:    $(ls -la $BIN | awk '{print $5, $6, $7, $8}')"
echo ""

# --- Cold encrypt ---
echo "==================================================================="
echo "  COLD ENCRYPT (single run)"
echo "==================================================================="
rm -f "$WORKDIR/out.enc"
sync && echo 3 > /proc/sys/vm/drop_caches
sleep 3
perf stat -e task-clock,cycles,instructions,cache-references,cache-misses,context-switches,branch-misses \
    -- "$BIN" encrypt "$INPUT" "$WORKDIR/out.enc" "$KEY" 2>&1

echo ""
echo "--- Cooldown 15s ---"
sleep 15

# --- Cold decrypt ---
echo ""
echo "==================================================================="
echo "  COLD DECRYPT (single run, after full cooldown)"
echo "==================================================================="
rm -f "$WORKDIR/out.dec"
sync && echo 3 > /proc/sys/vm/drop_caches
sleep 3
perf stat -e task-clock,cycles,instructions,cache-references,cache-misses,context-switches,branch-misses \
    -- "$BIN" decrypt "$WORKDIR/out.enc" "$WORKDIR/out.dec" "$KEY" 2>&1

echo ""
echo "--- Cooldown 15s ---"
sleep 15

# --- Second cold encrypt (to check repeatability) ---
echo ""
echo "==================================================================="
echo "  COLD ENCRYPT #2 (repeatability check)"
echo "==================================================================="
rm -f "$WORKDIR/out.enc"
sync && echo 3 > /proc/sys/vm/drop_caches
sleep 3
perf stat -e task-clock,cycles,instructions,cache-references,cache-misses,context-switches,branch-misses \
    -- "$BIN" encrypt "$INPUT" "$WORKDIR/out.enc" "$KEY" 2>&1

# --- Correctness ---
echo ""
echo "==================================================================="
echo "  CORRECTNESS CHECK"
echo "==================================================================="
if cmp -n "$(stat -c%s "$INPUT")" "$INPUT" "$WORKDIR/out.dec"; then
    echo "ROUND-TRIP OK"
else
    echo "!!! ROUND-TRIP MISMATCH !!!"
fi
echo ""
echo "Done. Results are above."
