#!/usr/bin/env bash
# Decompose where the time goes, one factor per experiment. Complements
# bench/run.sh, which only compares thread counts per command.
#
# Every streaming command is: boot the nexus and the C++ pool, open the
# stream file, then per batch: pull (read, decompress, deserialize), transform
# (the parallel stage), render (the @render handler), write to stdout. The
# experiments below isolate each term by differencing commands that share all
# but one of them.
#
# Usage: bench/analyze.sh [FASTA]      (default data/gencode.v50.transcripts.fa)
#   MOSM_BENCH_THREADS  thread counts for the scaling curve (default "1 2 4 8 0")
# Prints a report; keeps the stream files it converts under bench/.
set -euo pipefail
cd "$(dirname "$0")/.."

FASTA=${1:-data/gencode.v50.transcripts.fa}
THREADS=${MOSM_BENCH_THREADS:-"1 2 4 8 0"}
STEM=bench/$(basename "${FASTA%.*}")
Z3=$STEM.dat        # the stream file bench/run.sh uses (-z 3)
Z0=$STEM.z0.dat     # the same stream, uncompressed
ERR=bench/last.err

[ -r "$FASTA" ] || { echo "no such FASTA: $FASTA" >&2; exit 1; }
[ -x mosm ] || { echo "build mosm first (make build)" >&2; exit 1; }

guard() { (ulimit -v 8000000; timeout 1200 "$@"); }

# wall seconds of a command whose stdout goes to $1; BENCH rows land in $ERR
timed_to() {
    local dest=$1; shift
    local t0 t1
    t0=$EPOCHREALTIME
    guard "$@" > "$dest" 2> "$ERR"
    t1=$EPOCHREALTIME
    awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", b - a }'
}
timed() { timed_to /dev/null "$@"; }

# the `run` row's total (the whole pass, inside the pool) from the last run
run_s()   { awk -F'\t' '$1 == "BENCH" && $2 == "run"   { printf "%.3f", $9; f = 1 } END { if (!f) printf "-" }' "$ERR"; }
# the `batch` row's total and count
batch_s() { awk -F'\t' '$1 == "BENCH" && $2 == "batch" { printf "%.3f", $9; f = 1 } END { if (!f) printf "-" }' "$ERR"; }
batch_n() { awk -F'\t' '$1 == "BENCH" && $2 == "batch" { printf "%s", $5; f = 1 } END { if (!f) printf "-" }' "$ERR"; }

row() { printf '%-34s %8s %8s %8s %6s\n' "$@"; }
hdr() { echo; echo "## $1"; echo; row experiment wall_s run_s batch_s n; }

[ -s "$Z3" ] || guard ./mosm -f packet -z 3 convert "$FASTA" > "$Z3" 2> "$ERR"
[ -s "$Z0" ] || guard ./mosm -f packet -z 0 convert "$FASTA" > "$Z0" 2> "$ERR"

echo "fasta: $FASTA ($(stat -c %s "$FASTA") bytes)"
echo "stream -z 3: $(stat -c %s "$Z3") bytes; -z 0: $(stat -c %s "$Z0") bytes"
echo "cores: $(nproc)"

hdr "1. Fixed cost: boot the nexus and the pool, open the stream, pull one batch"
w=$(timed ./mosm cut 0:1 "$Z3"); row "cut 0:1 (one record)" "$w" "$(run_s)" - -

hdr "2. Stream pull: decompress + deserialize, with a trivial per-element map"
w=$(timed ./mosm stat -t 1 "$Z3"); row "stat -t 1, -z 3 stream" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"
w=$(timed ./mosm stat -t 1 "$Z0"); row "stat -t 1, -z 0 stream" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"

hdr "3. Render + write: keep everything and print it back as FASTA"
w=$(timed ./mosm filter -t 1 "$Z3"); row "filter (none) -> /dev/null" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"
w=$(timed_to bench/out.fa ./mosm filter -t 1 "$Z3"); row "filter (none) -> file" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"
rm -f bench/out.fa
w=$(timed ./mosm filter -t 1 -l 1000 -s 5000 "$Z3"); row "filter -l 1000 -s 5000 (keeps ~1/4)" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"
w=$(timed ./mosm filter -t 1 -l 100000 "$Z3"); row "filter -l 100000 (keeps none)" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"

hdr "4. Whole-list gather: cstat collects every histogram before its summary"
w=$(timed ./mosm cstat -t 1 -p "$Z3"); row "cstat -p (proportions)" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"
w=$(timed ./mosm cstat -t 1 -s "$Z3"); row "cstat -s (summary)" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"

hdr "5. Scaling of the parallel stage with --threads (0 = one per core)"
for t in $THREADS; do
    w=$(timed ./mosm filter -t "$t" -c 'GC > .5' "$Z3"); row "filter -c 'GC > .5' -t $t" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"
done
for t in $THREADS; do
    w=$(timed ./mosm cstat -t "$t" -p "$Z3"); row "cstat -p -t $t" "$w" "$(run_s)" "$(batch_s)" "$(batch_n)"
done
