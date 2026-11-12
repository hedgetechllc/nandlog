#!/bin/sh
# The comparison with Dhara and littlefs, from scratch -- `make baselines` in sim/. Fetches the two baselines at
# pinned commits into third_party/, cross-compiles all three for a Cortex-M4F and measures their footprint (if
# arm-none-eabi-gcc is on the PATH), builds the host harness, runs every job in jobs.txt, and prints the tables.
# The simulator jobs take about five minutes on seven cores, and each holds a 1.2 GB simulated part. Needs a host
# cc, git and python3.
#
# WHAT IS COMPARED. Every stack runs on the same Alliance driver, its persistent bad-block table and the same
# simulated part (4 KiB pages, 64 pages a block, 4,096 blocks, the driver's 16-block reserve at the top), and gets
# the same record stream at the same flush cadence. nandlog uses its public API, beginning a new epoch whenever the
# region is full. Dhara runs over the driver's page calls (glue/dhara_nand_nandlog.c: is_free is one raw latch, copy
# the part's internal move, no read-back after a program) with gc_ratio 4, under DLA (glue/dla.c), a minimal append
# layer that writes a sector when a record will not fit and on a flush syncs the map; its sector ring wraps at
# Dhara's capacity with the datasheet's 80 bad blocks assumed, a number that never moves. littlefs runs over Dhara
# and alone (glue/lfa.c, glue/lfs_bd_*.c), appending to one file and syncing it on a flush. A "page read" is one
# PAGE DATA READ latch, a "program" one PROGRAM EXECUTE, an "erase" one BLOCK ERASE, counted by the simulator.
# Caveat: the driver's internal copy does not report the source's ECC verdict, so Dhara's copy cannot return
# DHARA_E_ECC; the operation count is the same.
# TIME AND ENERGY. model_time.py turns the counts into the modeled time of a durable flush cycle, and the part's share
# of its energy at the datasheet's maximum current, from board C's measured primitives (results/model_time.md).
#
#    sh reproduce.sh            everything
#    JOBS=4 sh reproduce.sh     fewer jobs at a time
set -e
B="$(cd "$(dirname "$0")" && pwd)"
sh "$B/fetch.sh"
if command -v arm-none-eabi-gcc > /dev/null 2>&1; then
   sh "$B/footprint/build.sh"
else
   echo "arm-none-eabi-gcc not found: footprint skipped"
fi
sh "$B/build.sh"
rm -rf "$B/results"
JOBS="${JOBS:-7}" sh "$B/run_matrix.sh"
python3 -I "$B/summarize.py" "$B/results" > "$B/results/summary.md"
python3 -I "$B/model_time.py" "$B/results" > "$B/results/model_time.md"
cat "$B/results/summary.md" "$B/results/model_time.md"
