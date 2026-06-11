#!/usr/bin/env bash
# run_all.sh — run all three allocator jobs in parallel
#
# Usage:  bash run_all.sh [duration_seconds]
#   120s  = quick sanity check
#   600s  = good for talk slides
#   3600s = overnight, very clean signal

DURATION=${1:-120}
BINARY=./heap_aging

if [ ! -x "$BINARY" ]; then
    echo "[run_all] Building first..."
    g++ -O2 -std=c++17 -o heap_aging heap_aging.cpp || exit 1
fi

find_lib() { find /usr/lib /usr/local/lib -name "$1" 2>/dev/null | head -1; }
JEMALLOC=$(find_lib "libjemalloc.so*")
TCMALLOC=$(find_lib "libtcmalloc.so*")

echo "[run_all] duration=${DURATION}s"
echo "[run_all] jemalloc: ${JEMALLOC:-NOT FOUND (sudo apt install libjemalloc-dev)}"
echo "[run_all] tcmalloc: ${TCMALLOC:-NOT FOUND (sudo apt install libgoogle-perftools-dev)}"
echo ""

rm -f heap_aging.pids

echo "[run_all] starting system malloc..."
$BINARY system "$DURATION" > results_system.tsv 2>stderr_system.log &
echo $! >> heap_aging.pids

if [ -n "$JEMALLOC" ]; then
    echo "[run_all] starting jemalloc..."
    LD_PRELOAD="$JEMALLOC" $BINARY jemalloc "$DURATION" > results_jemalloc.tsv 2>stderr_jemalloc.log &
    echo $! >> heap_aging.pids
fi

if [ -n "$TCMALLOC" ]; then
    echo "[run_all] starting tcmalloc..."
    LD_PRELOAD="$TCMALLOC" $BINARY tcmalloc "$DURATION" > results_tcmalloc.tsv 2>stderr_tcmalloc.log &
    echo $! >> heap_aging.pids
fi

echo ""
echo "[run_all] PIDs: $(cat heap_aging.pids | tr '\n' ' ')"
echo "[run_all] stop all:    kill \$(cat heap_aging.pids)"
echo "[run_all] watch live:  tail -f stderr_*.log"
echo "[run_all] waiting..."
wait
echo "[run_all] all done."
