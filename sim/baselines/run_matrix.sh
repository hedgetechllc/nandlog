#!/bin/sh
# Run every job in jobs.txt (or only those whose names match $1), JOBS at a time, into results/<name>.txt.
# Each job is one ./bench process over a fresh simulated 4096-block part (about 1.2 GB of host RAM each).
set -e
H="$(cd "$(dirname "$0")" && pwd)"
JOBS="${JOBS:-6}"
mkdir -p "$H/results"
grep -v '^#' "$H/jobs.txt" | grep -E "${1:-.}" | while read -r name stack bytes recs flush opts; do
   [ -n "$name" ] && echo "$name $stack $bytes $recs $flush${opts:+ $opts}"
done | xargs -P "$JOBS" -L 1 sh -c 'n=$0; start=$(date +%s); "'"$H"'/bench" "$@" > "'"$H"'/results/$n.txt" 2>&1; rc=$?; echo "elapsed_s=$(( $(date +%s) - start )) exit=$rc" >> "'"$H"'/results/$n.txt"; echo "$n exit=$rc $(( $(date +%s) - start ))s"'
