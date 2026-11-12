#!/bin/sh
# The ablation table, as `make ablation` runs it. Each NANDLOG_MUTATE_* switch in nandlog.c takes one mechanism of the
# recovery argument away. Every such build -- and the library as it ships, for the first row -- is run through the
# regression tests, the bounded-exhaustive check, a short subset of the crash-point campaign and the fuzzer's seeds,
# and the table says which of them noticed. A run that crashes, or outlasts LIMIT seconds, counts as noticing.
#
#    sh ablation.sh                        build every mutant, run everything, print the table
#    MUTANTS="NONE ..." sh ablation.sh     only those rows again, keeping the others' results
#    CAMPAIGN=full OUT=ablation_full.out LIMIT=14400 FUZZ_SEEDS= sh ablation.sh
#                                          every campaign job, not the subset, against each build (hours, not minutes)
#    sh ablation.sh table                  print the table again from ablation.out/
set -u
CC="${CC:-cc}"
JOBS="${JOBS:-12}"
LIMIT="${LIMIT:-900}"
OUT="${OUT:-ablation.out}"
CAMPAIGN="${CAMPAIGN:-subset}"
SAN="-std=c99 -Wall -Wextra -Wno-unused-parameter -O1 -g -fsanitize=address,undefined -DNANDLOG_CHECK_CRC=1"
FAST="-std=c99 -Wall -Wextra -Wno-unused-parameter -O2 -g -DNANDLOG_CHECK_CRC=1"
INC="-I. -I.. -I../chips"
PORT=nandlog_port_sim.c
CORE=../nandlog.c
ALLIANCE=../chips/nandlog_chip_AS5F18G04SND.c
ALL="NONE IGNORE_PASSED_OVER NO_ERASE_GUARD READ_SLOT_TWICE NO_BLOCK_ENTRY_RULE NAIVE_HEAD_SEARCH NO_TOMBSTONES"
MUTANTS="${MUTANTS:-$ALL}"
FUZZ_SEEDS="${FUZZ_SEEDS-1 2 3 4 5 6 7 8 9 10 11 12}"
FUZZ_STEPS="${FUZZ_STEPS:-12000}"

# The campaign's subset: every sweep, and the traces that each take under about a minute
SUBSET='^sweep|^campaign tornloop|^campaign slottorn 2|^campaign tabletorn 2|^campaign blocktorn 2|^campaign first 0|^campaign unwritable 3'

run_limited()
{
   # Run a command for at most LIMIT seconds
   perl -e 'alarm shift; exec @ARGV or exit 127' "$LIMIT" "$@"
}

if [ "${1:-}" = "job" ]; then
   # One job of one mutant: its output and exit status land in that mutant's directory
   mutant=$2; kind=$3; shift 3
   dir=$OUT/$mutant
   name=$(echo "$kind $*" | sed 's/ *$//' | tr ' ' '_')
   case $kind in
      regress)    run_limited ./$dir/regress > $dir/$name.txt 2>&1 ;;
      exhaustive) run_limited ./$dir/exhaustive > $dir/$name.txt 2>&1 ;;
      fuzz)       run_limited ./$dir/fuzz $1 $FUZZ_STEPS 30 600 $(($1 % 3)) > $dir/$name.txt 2>&1 ;;
      *)          run_limited ./$dir/campaign $kind "$@" > $dir/$name.txt 2>&1 ;;
   esac
   echo $? > $dir/$name.status
   exit 0
fi

if [ "${1:-}" != "table" ]; then
   echo "ablation: building $(echo $MUTANTS | wc -w | tr -d ' ') builds"
   for m in $MUTANTS; do
      rm -rf $OUT/$m && mkdir -p $OUT/$m
      flag=""
      [ "$m" != "NONE" ] && flag="-DNANDLOG_MUTATE_$m=1"
      ( $CC $SAN -DNANDLOG_RECORD_FRAMING=1 -DNANDLOG_CHIP_BLOCK_COUNT=192 $flag $INC $PORT nandlog_sim_regress.c $CORE $ALLIANCE -o $OUT/$m/regress &&
        $CC $FAST -DNANDLOG_RECORD_FRAMING=1 -DNANDLOG_CHIP_PAGES_PER_BLOCK=4 -DNANDLOG_CHIP_BLOCK_COUNT=29 $flag $INC $PORT nandlog_sim_exhaustive.c $CORE $ALLIANCE -o $OUT/$m/exhaustive &&
        $CC $FAST -DNANDLOG_RECORD_FRAMING=1 -DNANDLOG_CHIP_BLOCK_COUNT=128 -DNANDLOG_CHIP_RESERVED_BLOCKS=80 $flag $INC $PORT nandlog_sim_campaign.c $CORE $ALLIANCE -o $OUT/$m/campaign &&
        $CC $SAN -DNANDLOG_RECORD_FRAMING=1 -DNANDLOG_CHIP_BLOCK_COUNT=192 $flag $INC $PORT nandlog_sim_fuzz.c $CORE $ALLIANCE -o $OUT/$m/fuzz ) ||
         { echo "ablation: the $m build failed"; exit 1; } &
   done
   wait
   for m in $MUTANTS; do
      [ -x $OUT/$m/fuzz ] || { echo "ablation: the $m build failed"; exit 1; }
   done
   first=$(echo $MUTANTS | cut -d ' ' -f 1)
   subset=$(./$OUT/$first/campaign list | grep -E "$SUBSET")
   [ "$CAMPAIGN" = full ] && subset=$(./$OUT/$first/campaign list)
   echo "ablation: per build, the regression tests, the exhaustive check, $(echo "$subset" | wc -l | tr -d ' ') campaign jobs and $(echo $FUZZ_SEEDS | wc -w | tr -d ' ') fuzz seeds, $JOBS at a time"
   for m in $MUTANTS; do
      echo "$m regress"
      echo "$m exhaustive"
      echo "$subset" | sed "s/^/$m /"
      for seed in $FUZZ_SEEDS; do echo "$m fuzz $seed"; done
   done | xargs -P "$JOBS" -L 1 sh "$0" job
fi

echo
echo "Ablation: each row takes one mechanism away (NANDLOG_MUTATE_<row> in nandlog.c). Regression: the tests that failed"
echo "(of every test in the framed build); exhaustive: combinations within the fault model that failed, then metadata"
echo "trials that failed; campaign $CAMPAIGN: trials that broke the contract, with jobs that crashed or ran out of time;"
echo "fuzz: seeds whose run reported a violation, crashed or ran out of time, of those run"
printf "%-20s %-44s %-16s %-22s %-8s %s\n" "mutation" "regression" "exhaustive" "campaign $CAMPAIGN" "fuzz" "caught"
for m in $ALL; do
   dir=$OUT/$m
   [ -d $dir ] || continue
   failed=$(awk '/^  [A-Za-z0-9-]+ +[a-z]/ { id = $1 } /FAIL:/ { if (!(id in seen)) { seen[id] = 1; printf "%s ", id } }' $dir/regress.txt)
   tests=$(grep -E '^[0-9]+ checks' $dir/regress.txt | sed 's/.*; \([0-9]*\) tests failed/\1/')
   [ -z "$tests" ] && tests="crashed"
   [ "$(cat $dir/regress.status)" -gt 128 ] 2>/dev/null && tests="crashed"
   [ "$(cat $dir/regress.status)" = 142 ] && tests="timed out"
   regression="$tests"
   [ -n "$failed" ] && regression="$tests: $(echo $failed | cut -c1-36)"
   states=$(grep 'within the model failed' $dir/exhaustive.txt | sed 's/.*; \([0-9]*\) within the model failed.*/\1/')
   meta=$(grep 'metadata trials' $dir/exhaustive.txt | sed 's/.* metadata trials, \([0-9]*\) failed.*/\1/')
   exhaustive="${states:-crashed} / ${meta:-crashed}"
   # A run that stops part-way still counts the metadata trials that failed before it did: each finished trace's
   # count, and the failures printed so far of the trace it stopped in
   partial=$(awk '/^  FAIL .* damage / { p++ } / refused to log after, [0-9]+ failed$/ { n += $(NF - 1); p = 0 } END { print n + p }' $dir/exhaustive.txt)
   [ "$(cat $dir/exhaustive.status)" -gt 128 ] 2>/dev/null && exhaustive="${states:-?} / $partial, crashed"
   [ "$(cat $dir/exhaustive.status)" = 142 ] && exhaustive="${states:-?} / $partial, timed out"
   violations=$(cat $dir/campaign_*.txt $dir/sweep_*.txt 2>/dev/null | awk '
      /^RESULT/   { v += ($5 - $6) + ($7 - $8) }
      /^SWEEP/    { v += $6 + $7 + $8 + $9 }
      /^METADATA/ { v += $5 + $6 + $7 + $8 + $9 + $10 + $11 + $13 }
      END         { print v + 0 }')
   broken=0
   for s in $dir/campaign_*.status $dir/sweep_*.status; do
      st=$(cat $s)
      [ "$st" -gt 1 ] && broken=$((broken + 1))
   done
   campaign="$violations"
   [ $broken -eq 1 ] && campaign="$violations, 1 job aborted"
   [ $broken -gt 1 ] && campaign="$violations, $broken jobs aborted"
   seeds=0; seeds_failed=0
   for s in $dir/fuzz_*.status; do
      [ -f "$s" ] || continue
      seeds=$((seeds + 1))
      [ "$(cat $s)" != 0 ] && seeds_failed=$((seeds_failed + 1))
   done
   fuzz="$seeds_failed/$seeds"
   caught=no
   { [ "$tests" != "0" ] || [ "$exhaustive" != "0 / 0" ] || [ "$campaign" != "0" ] || [ $seeds_failed -gt 0 ]; } && caught=yes
   [ "$m" = "NONE" ] && caught="(as shipped)"
   name=$m
   [ "$m" = "NONE" ] && name="none"
   printf "%-20s %-44s %-16s %-22s %-8s %s\n" "$name" "$regression" "$exhaustive" "$campaign" "$fuzz" "$caught"
   [ "$m" = "NONE" ] && [ "$caught" != "(as shipped)" ] && shipped_failed=1
   [ "$m" = "NONE" ] && { [ "$tests" != "0" ] || [ "$exhaustive" != "0 / 0" ] || [ "$campaign" != "0" ] || [ $seeds_failed -gt 0 ]; } && shipped_failed=1
done
if [ "${shipped_failed:-0}" = 1 ]; then
   echo "ABLATION FAILED: the library as shipped did not pass every check"
   exit 1
fi
exit 0
