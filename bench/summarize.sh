#!/usr/bin/env bash
# Summarize bench/results.tsv: per command and thread count, the median wall
# time and the mean of the labeled stage's per-run total, then the speedup of
# every thread count over the first one listed for that command.
set -euo pipefail
IN=${1:-bench/results.tsv}
[ -r "$IN" ] || { echo "no results at $IN" >&2; exit 1; }

awk -F'\t' '
NR == 1 { next }
{
    key = $1 SUBSEP $2
    if (!(key in seen)) { seen[key] = 1; order[++n] = key }
    walls[key] = walls[key] " " $4
    if ($7 != "-") { bsum[key] += $7; bcnt[key]++ }
}
function median(list,   a, m, i, j, t) {
    m = split(substr(list, 2), a, " ")
    for (i = 2; i <= m; i++) {
        t = a[i]
        for (j = i - 1; j >= 1 && a[j] + 0 > t + 0; j--) a[j + 1] = a[j]
        a[j + 1] = t
    }
    return (m % 2) ? a[(m + 1) / 2] : (a[m / 2] + a[m / 2 + 1]) / 2
}
END {
    printf "%-20s %8s %10s %12s %8s\n", "command", "threads", "wall_med_s", "batch_tot_s", "speedup"
    for (i = 1; i <= n; i++) {
        key = order[i]
        split(key, k, SUBSEP)
        med = median(walls[key])
        bt = (key in bcnt) ? sprintf("%.3f", bsum[key] / bcnt[key]) : "-"
        if (!(k[1] in base)) base[k[1]] = med
        sp = (k[2] == "-") ? "-" : sprintf("%.2fx", base[k[1]] / med)
        printf "%-20s %8s %10.3f %12s %8s\n", k[1], k[2], med, bt, sp
    }
}' "$IN"
