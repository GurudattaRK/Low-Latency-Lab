#!/usr/bin/env bash
# TeraCrypt build + PGO + perf benchmarking script.
# Run from the teracrypt directory. Needs sudo for perf hardware counters.
#
# Usage:
#   sudo ./bench.sh                 # full run
#   sudo ./bench.sh --skip-pgo      # skip PGO
#   sudo ./bench.sh --file-size 2G  # custom test file size (default 1G)
#   sudo ./bench.sh --cooldown 10   # seconds between runs (default 5)

set -uo pipefail

TEST_FILE_SIZE="1G"
SKIP_PGO=0
RUNS=5
COOLDOWN_SECS=5

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-pgo) SKIP_PGO=1; shift ;;
        --file-size) TEST_FILE_SIZE="$2"; shift 2 ;;
        --runs) RUNS="$2"; shift 2 ;;
        --cooldown) COOLDOWN_SECS="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "This script needs root (perf hardware counters require it). Re-run with sudo."
    exit 1
fi

TS=$(date +%Y%m%d_%H%M%S)
OUT="results_${TS}.txt"
WORKDIR="$(pwd)/.bench_workdir"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"
# Avoid tmpfs -- it measures RAM speed, not disk I/O
WORKDIR_FSTYPE=$(findmnt -no FSTYPE -T "$WORKDIR" 2>/dev/null)
if [[ "$WORKDIR_FSTYPE" == "tmpfs" ]]; then
    echo "WARNING: $WORKDIR is on tmpfs (RAM-backed) -- throughput numbers will"
    echo "         reflect RAM speed, not real disk I/O. Run this script from"
    echo "         a directory on a real disk-backed filesystem instead."
fi

log() { echo "$@" | tee -a "$OUT"; }
section() { echo; echo "=================================================================="; echo "  $1"; echo "=================================================================="; } 

exec > >(tee -a "$OUT") 2>&1

section "SYSTEM INFO"
echo "Date: $(date)"
echo "Kernel: $(uname -a)"
echo "--- CPU ---"
lscpu 2>/dev/null || cat /proc/cpuinfo | grep -m1 "model name"
echo "Physical cores / logical cores:"
lscpu 2>/dev/null | grep -E "^CPU\(s\)|Core\(s\) per socket|Socket\(s\)|Thread\(s\) per core"
echo "--- Memory ---"
free -h
echo "--- Storage (device backing current directory) ---"
df -h .
findmnt -T . 2>/dev/null
echo "--- Disk model (best effort) ---"
lsblk -d -o NAME,MODEL,ROTA,SIZE 2>/dev/null
echo "--- NUMA ---"
numactl --hardware 2>/dev/null || echo "(numactl not installed or single-node)"
echo "--- perf_event_paranoid ---"
cat /proc/sys/kernel/perf_event_paranoid

section "TOOLCHAIN CHECK"
for tool in clang lld ld.lld llvm-profdata perf make; do
    if command -v "$tool" &>/dev/null; then
        echo "OK   $tool: $(command -v "$tool")"
    else
        echo "MISSING $tool"
    fi
done
clang --version | head -1
echo "liburing:"
pkg-config --modversion liburing 2>/dev/null || ldconfig -p | grep liburing || echo "  (liburing not found via pkg-config/ldconfig -- build will fail without it)"

section "BUILD: plain optimized (-Ofast -flto -march=native)"
make clean >/dev/null 2>&1
time make all
if [[ ! -x ./teracrypt ]]; then
    echo "BUILD FAILED. Aborting."
    exit 1
fi
cp teracrypt "$WORKDIR/teracrypt_plain"
ls -la teracrypt

if [[ $SKIP_PGO -eq 0 ]]; then
    section "BUILD: PGO (instrument -> train -> merge -> optimize)"
    rm -f ./teracrypt  # so a partial/failed pipeline can't leave a stale binary that looks like success
    PGO_RC=0
    time make pgo || PGO_RC=$?
    # Check exit code AND that the binary was actually rebuilt
    # (a failed llvm-profdata merge can leave a stale binary around)
    if [[ $PGO_RC -ne 0 || ! -x ./teracrypt || ! -f .pgo/merged.profdata ]]; then
        echo "PGO BUILD FAILED (exit code $PGO_RC). Falling back to plain build."
        echo "(if this is a 'failed to uncompress data (zlib)' error,"
        echo " it's a clang/llvm-profdata version mismatch)"
        cp "$WORKDIR/teracrypt_plain" teracrypt
        SKIP_PGO=1
    else
        cp teracrypt "$WORKDIR/teracrypt_pgo"
    fi
fi

section "PREPARE TEST DATA (${TEST_FILE_SIZE})"
head -c 128 /dev/urandom > "$WORKDIR/key.bin"
fallocate -l "$TEST_FILE_SIZE" "$WORKDIR/test.bin" 2>/dev/null || \
    dd if=/dev/urandom of="$WORKDIR/test.bin" bs=1M count=1024 2>/dev/null
# Overwrite with real random data so round-trip verification is meaningful
dd if=/dev/urandom of="$WORKDIR/test.bin" bs=1M count="$(du -m "$WORKDIR/test.bin" | cut -f1)" conv=notrunc 2>/dev/null
ls -la "$WORKDIR/test.bin"

cooldown() {
    echo "--- cooling down ${COOLDOWN_SECS}s (thermal headroom) ---"
    sleep "$COOLDOWN_SECS"
}

run_benchmark() {
    local binary_path=$1
    local label=$2
    section "BENCHMARK: $label ($RUNS runs, encrypt)"
    for i in $(seq 1 $RUNS); do
        rm -f "$WORKDIR/out.enc"
        echo "--- run $i ---"
        perf stat -e task-clock,cycles,instructions,cache-references,cache-misses,page-faults,context-switches,branch-misses \
            -- "$binary_path" encrypt "$WORKDIR/test.bin" "$WORKDIR/out.enc" "$WORKDIR/key.bin" 2>&1
        [[ $i -lt $RUNS ]] && cooldown
    done

    section "BENCHMARK: $label ($RUNS runs, decrypt)"
    for i in $(seq 1 $RUNS); do
        rm -f "$WORKDIR/out.dec"
        echo "--- run $i ---"
        perf stat -e task-clock,cycles,instructions,cache-references,cache-misses,page-faults,context-switches,branch-misses \
            -- "$binary_path" decrypt "$WORKDIR/out.enc" "$WORKDIR/out.dec" "$WORKDIR/key.bin" 2>&1
        [[ $i -lt $RUNS ]] && cooldown
    done

    section "CORRECTNESS CHECK: $label"
    file_bytes=$(stat -c%s "$WORKDIR/test.bin")
    if cmp -n "$file_bytes" "$WORKDIR/test.bin" "$WORKDIR/out.dec"; then
        echo "ROUND-TRIP OK (first $file_bytes bytes match)"
    else
        echo "!!! ROUND-TRIP MISMATCH -- DO NOT TRUST THIS BUILD !!!"
    fi
}

run_benchmark "$WORKDIR/teracrypt_plain" "PLAIN (-Ofast -flto -march=native)"
if [[ $SKIP_PGO -eq 0 ]]; then
    run_benchmark "$WORKDIR/teracrypt_pgo" "PGO"
fi

section "PERF RECORD: hotspot profile (plain build, single encrypt run)"
rm -f "$WORKDIR/out.enc"
perf record -F 999 -g -o "$WORKDIR/perf.data" -- "$WORKDIR/teracrypt_plain" encrypt "$WORKDIR/test.bin" "$WORKDIR/out.enc" "$WORKDIR/key.bin"
echo "--- perf report (top 40 symbols by self time) ---"
perf report -i "$WORKDIR/perf.data" --stdio --sort=overhead,symbol -n 2>/dev/null | head -60

section "PERF C2C / CACHE ANALYSIS (if supported)"
if perf c2c record -o "$WORKDIR/perf.c2c" -- "$WORKDIR/teracrypt_plain" encrypt "$WORKDIR/test.bin" "$WORKDIR/out.enc" "$WORKDIR/key.bin" 2>"$WORKDIR/c2c_err.txt"; then
    tail -5 "$WORKDIR/c2c_err.txt"
else
    echo "(perf c2c not supported on this kernel/CPU -- skipped: $(tail -1 "$WORKDIR/c2c_err.txt"))"
fi

section "IO STATS DURING RUN (iostat, if available)"
if command -v iostat &>/dev/null; then
    rm -f "$WORKDIR/out.enc"
    iostat -x 1 3 > "$WORKDIR/iostat_idle.txt" 2>&1 &
    IOSTAT_PID=$!
    "$WORKDIR/teracrypt_plain" encrypt "$WORKDIR/test.bin" "$WORKDIR/out.enc" "$WORKDIR/key.bin" > /dev/null
    kill $IOSTAT_PID 2>/dev/null
    echo "(iostat only captures samples around the run; for live disk util")
else
    echo "iostat not installed (package: sysstat). Skipping."
fi

section "MEMORY / CACHE BEHAVIOR DURING A LARGE RUN"
rm -f "$WORKDIR/out.enc"
sync
echo "Before run:"; free -h; grep -E "^Dirty|^Cached" /proc/meminfo
( for i in $(seq 1 30); do sleep 1; stdbuf -oL grep -E "^Dirty|^Cached|^MemAvailable" /proc/meminfo; echo "t=${i}s"; done ) > "$WORKDIR/mem_trace.txt" &
MEM_PID=$!
"$WORKDIR/teracrypt_plain" encrypt "$WORKDIR/test.bin" "$WORKDIR/out.enc" "$WORKDIR/key.bin" > /dev/null
kill $MEM_PID 2>/dev/null
echo "After run:"; free -h; grep -E "^Dirty|^Cached" /proc/meminfo
echo "--- peak values during run ---"
if [[ -s "$WORKDIR/mem_trace.txt" ]]; then
    awk '/^Dirty/{d=$2} /^Cached/{c=$2} /^MemAvailable/{a=$2; if(d>maxd)maxd=d; if(c>maxc)maxc=c; if(mina==0||a<mina)mina=a} END{print "peak Dirty (KB):", maxd+0; print "peak Cached (KB):", maxc+0; print "min MemAvailable (KB):", mina+0}' "$WORKDIR/mem_trace.txt"
else
    echo "(run completed in under 1s -- no samples captured; this is fine, just means the file was small/fast)"
fi

section "DONE"
echo "Results saved to: $OUT"
echo "Working files were kept in: $WORKDIR (delete manually if you don't need them: rm -rf $WORKDIR)"
echo "Send $OUT back for analysis."
