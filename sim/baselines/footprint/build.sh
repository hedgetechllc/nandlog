#!/bin/sh
# Cross-compile nandlog, Dhara and littlefs (plus the baseline glue) for a Cortex-M4F with identical flags, then
#   1. size every object as compiled (the whole library),
#   2. link four probe applications with --gc-sections and attribute the kept bytes to each object (what an append-only
#      logger with a sequential offload actually links), and
#   3. compute worst-case stack depth of each probe's call tree from GCC's -fcallgraph-info=su output.
# Results go to out/. Nothing under third_party/ is executed; only its C sources are compiled.
set -e
F="$(cd "$(dirname "$0")" && pwd)"
B="$(cd "$F/.." && pwd)"
N="$(cd "$B/../.." && pwd)"
D="$B/third_party/dhara/dhara"
L="$B/third_party/littlefs"
G="$B/glue"
O="$F/out"
rm -rf "$O" && mkdir -p "$O/obj"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
CFLAGS="$ARCH -O2 -std=c99 -ffunction-sections -fdata-sections -fno-strict-aliasing"
LFSDEFS="-DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_ASSERT"
# Include paths relative to out/obj (where every compile runs), because the absolute path holds a space
R=../../..
INC="-I$R/../.. -I$R/../../chips -I$R/third_party/dhara/dhara -I$R/third_party/littlefs -I$R/glue"
ANALYSIS="-fstack-usage -fcallgraph-info=su"

$CC --version | head -1 > "$O/compiler.txt"
echo "CFLAGS=$CFLAGS" >> "$O/compiler.txt"
echo "littlefs defines: $LFSDEFS" >> "$O/compiler.txt"

cc1() {  # cc1 <object name> <source> [extra flags]
   name="$1"; src="$2"; shift 2
   ( cd "$O/obj" && $CC $CFLAGS $ANALYSIS $INC "$@" -c "$src" -o "$name.o" )
}

cc1 nandlog                 "$N/nandlog.c"
cc1 nandlog_chip_AS5F18G04SND "$N/chips/nandlog_chip_AS5F18G04SND.c"
cc1 dhara_map               "$D/map.c"
cc1 dhara_journal           "$D/journal.c"
cc1 dhara_error             "$D/error.c"
cc1 lfs                     "$L/lfs.c" $LFSDEFS
cc1 lfs_util                "$L/lfs_util.c" $LFSDEFS
cc1 glue_dhara_nand         "$G/dhara_nand_nandlog.c"
cc1 glue_dla                "$G/dla.c"
cc1 glue_lfs_bd_dhara       "$G/lfs_bd_dhara.c" $LFSDEFS
cc1 glue_lfs_bd_raw         "$G/lfs_bd_raw.c" $LFSDEFS
cc1 glue_lfa                "$G/lfa.c" $LFSDEFS
cc1 port_stub               "$F/port_stub.c"
cc1 app_nandlog             "$F/app_nandlog.c"
cc1 app_dhara               "$F/app_dhara.c"
cc1 app_lfs_dhara           "$F/app_lfs_dhara.c" $LFSDEFS
cc1 app_lfs_raw             "$F/app_lfs_raw.c" $LFSDEFS

# 1. Whole objects, as compiled
( cd "$O/obj" && arm-none-eabi-size -t nandlog.o nandlog_chip_AS5F18G04SND.o dhara_map.o dhara_journal.o dhara_error.o \
      lfs.o lfs_util.o glue_dhara_nand.o glue_dla.o glue_lfs_bd_dhara.o glue_lfs_bd_raw.o glue_lfa.o port_stub.o ) > "$O/objects_berkeley.txt"
for o in "$O"/obj/*.o; do echo "== $(basename "$o")"; arm-none-eabi-size -A "$o" | awk '$2 > 0'; done > "$O/objects_sections.txt"

# 2. Linked probes. No startup code and no vector table: just the call tree from main(), with newlib-nano for memcpy etc.
link() {  # link <image> <objects...>
   img="$1"; shift
   ( cd "$O/obj" && $CC $ARCH -O2 --specs=nano.specs --specs=nosys.specs -nostartfiles -Wl,-e,main -Wl,--gc-sections \
        -Wl,-Map="$O/$img.map" -o "$O/$img.elf" "$@" )
   arm-none-eabi-size "$O/$img.elf" > "$O/$img.size"
   arm-none-eabi-nm -S --size-sort -t d "$O/$img.elf" > "$O/$img.nm"
}
link nandlog    app_nandlog.o nandlog.o nandlog_chip_AS5F18G04SND.o port_stub.o
link dhara      app_dhara.o glue_dla.o glue_dhara_nand.o dhara_map.o dhara_journal.o dhara_error.o nandlog_chip_AS5F18G04SND.o port_stub.o
link lfs_dhara  app_lfs_dhara.o glue_lfa.o glue_lfs_bd_dhara.o lfs.o lfs_util.o glue_dhara_nand.o dhara_map.o dhara_journal.o dhara_error.o nandlog_chip_AS5F18G04SND.o port_stub.o
link lfs_raw    app_lfs_raw.o glue_lfa.o glue_lfs_bd_raw.o lfs.o lfs_util.o nandlog_chip_AS5F18G04SND.o port_stub.o

# Attribution of the linked bytes, and stack depth, by the analysis script (run isolated: python3 -I)
[ -f "$F/analyze.py" ] && python3 -I "$F/analyze.py" "$O" > "$O/footprint_report.txt"
cat "$O/footprint_report.txt"
