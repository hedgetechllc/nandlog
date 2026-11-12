#!/bin/sh
# Host build of the operation-count harness, ./bench: this repository's nandlog, Alliance driver and RAM-backed
# simulator port, Dhara (map, journal, error), littlefs, and the glue in glue/, as one binary. Nothing under
# third_party/ is executed; only its C sources are compiled.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
N="$(cd "$H/../.." && pwd)"
D="$H/third_party/dhara/dhara"
L="$H/third_party/littlefs"
G="$H/glue"
CC="${CC:-cc}"
"$CC" -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
   -DNANDLOG_RECORD_FRAMING=1 -DDHARA_GLUE_COUNTERS -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN \
   -I"$N" -I"$N/chips" -I"$N/sim" -I"$D" -I"$L" -I"$G" \
   "$H/bench.c" "$N/nandlog.c" "$N/chips/nandlog_chip_AS5F18G04SND.c" "$N/sim/nandlog_port_sim.c" \
   "$D/map.c" "$D/journal.c" "$D/error.c" "$L/lfs.c" "$L/lfs_util.c" \
   "$G/dhara_nand_nandlog.c" "$G/dla.c" "$G/lfs_bd_dhara.c" "$G/lfs_bd_raw.c" "$G/lfa.c" \
   -o "$H/bench"
echo "built $H/bench"
