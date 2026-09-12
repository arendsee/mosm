#!/usr/bin/env bash
# Benchmark the mosm commands, single-threaded against one worker per core.
#
# Two clocks: wall time around each run (the whole pass, pools included), and
# morloc's own benchmark rows, which time every call of the labeled parallel
# stage -- one call per batch of the stream. The rows come out on stderr as
#   BENCH <group> <name> <lang> <count> <mean> <min> <max> <total>
# per main.yaml, and are folded into the results table.
#
# Usage: bench/run.sh [FASTA]      (default data/gencode.v50.transcripts.fa)
#   MOSM_BENCH_REPS=N   repetitions per command and thread count (default 3)
#   MOSM_BENCH_THREADS  thread counts to compare (default "1 0"; 0 = nproc)
# Writes bench/results.tsv (one row per run) and prints a summary.
set -euo pipefail
cd "$(dirname "$0")/.."

FASTA=${1:-data/gencode.v50.transcripts.fa}
REPS=${MOSM_BENCH_REPS:-3}
THREADS=${MOSM_BENCH_THREADS:-"1 0"}
DAT=bench/$(basename "${FASTA%.*}").dat
OUT=bench/results.tsv
ERR=bench/last.err

[ -r "$FASTA" ] || { echo "no such FASTA: $FASTA" >&2; exit 1; }

# Every build and run is capped: the compiler and the pools are under
# development and either can allocate without bound on a novel input.
guard() { (ulimit -v 8000000; timeout 1200 "$@"); }

# Wall-clock a command whose stdout is discarded; BENCH rows land in $ERR.
timed() {
    local t0 t1
    t0=$EPOCHREALTIME
    guard "$@" > /dev/null 2> "$ERR"
    t1=$EPOCHREALTIME
    awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", b - a }'
}

# The batch rows of the last run: "<count>\t<mean>\t<total>", or "-\t-\t-".
batch_row() {
    awk -F'\t' '$1 == "BENCH" && $2 == "batch" { printf "%s\t%s\t%s", $5, $6, $9; found = 1 }
                END { if (!found) printf "-\t-\t-" }' "$ERR"
}

echo "building mosm" >&2
guard env GHCRTS=-M2g morloc make -o mosm main.loc > /dev/null 2> "$ERR" \
    || { cat "$ERR" >&2; exit 1; }

printf 'command\tthreads\trep\twall_s\tbatch_n\tbatch_mean_s\tbatch_total_s\n' > "$OUT"

# convert is the sequential FASTA parse that produces the stream file every
# other command reads; it is measured, and its last output is kept.
for rep in $(seq "$REPS"); do
    w=$(timed ./mosm -f packet -z 3 convert "$FASTA")
    printf 'convert\t-\t%s\t%s\t-\t-\t-\n' "$rep" "$w" >> "$OUT"
    echo "convert rep=$rep wall=${w}s" >&2
done
guard ./mosm -f packet -z 3 convert "$FASTA" > "$DAT" 2> "$ERR"

# name|arg|arg...; the stream file and (where the command takes it) --threads
# are appended. cut takes no thread count: it selects, it does not compute.
# filter-none keeps everything, so its wall time is the cost of pulling the
# stream and rendering it back out, the floor under every filter.
COMMANDS=(
    "cut|cut|0:100000"
    "filter-none|filter|-t"
    "filter-length|filter|-l|1000|-s|5000|-t"
    "filter-composition|filter|-c|GC > .5|-t"
    "stat|stat|-t"
    "cstat|cstat|-p|-t"
)

for spec in "${COMMANDS[@]}"; do
    IFS='|' read -r -a parts <<< "$spec"
    name=${parts[0]}
    args=("${parts[@]:1}")
    if [[ ${args[-1]} == "-t" ]]; then
        unset 'args[-1]'
        for t in $THREADS; do
            for rep in $(seq "$REPS"); do
                w=$(timed ./mosm "${args[@]}" --threads "$t" "$DAT")
                printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$t" "$rep" "$w" "$(batch_row)" >> "$OUT"
                echo "$name threads=$t rep=$rep wall=${w}s" >&2
            done
        done
    else
        for rep in $(seq "$REPS"); do
            w=$(timed ./mosm "${args[@]}" "$DAT")
            printf '%s\t-\t%s\t%s\t-\t-\t-\n' "$name" "$rep" "$w" >> "$OUT"
            echo "$name rep=$rep wall=${w}s" >&2
        done
    fi
done

echo >&2
echo "results: $OUT" >&2
bench/summarize.sh "$OUT"
