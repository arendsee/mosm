# mosm benchmarks

`bench/run.sh [FASTA]` builds `mosm`, converts the FASTA to a stream file
once (timed), then runs each command at `--threads 1` and `--threads 0` (one
worker per core), `MOSM_BENCH_REPS` times each (default 3). It writes
`bench/results.tsv`, one row per run, and prints a summary via
`bench/summarize.sh`.

Two clocks:

- `wall_s`: wall time of the whole run, pools included.
- `batch_*`: morloc's own benchmark rows. The parallel stage of every
  streaming command is labeled `batch@pfilterWith` / `batch@pmapWith`
  (`lib/filter.yaml`, `lib/stat.yaml`; row format in `main.yaml`), so the
  pool times every call of it -- one call per batch of the stream -- and the
  nexus reports count, mean, min, max and total on stderr at exit. Silence
  the rows in ordinary use with `mosm --quiet`.

The whole streaming pass is not labeled: a label on an effectful call
measures only the construction of its thunk (FINDINGS.md, item 5), so wall
time is the only whole-pass number.

## gencode.v50.transcripts.fa, 2026-09-11

1.5 GB FASTA, 1.2M transcripts; 12 cores; stream file written at `-z 3`
(96 MB, 98 batches of about 1 MB payload). Medians of 3.

    command               threads wall_med_s  batch_tot_s  speedup
    convert                     -     15.315            -        -
    cut                         -      0.112            -        -
    filter-none                 1     10.186        1.399    1.00x
    filter-none                 0      9.717        0.505    1.05x
    filter-length               1      7.447        1.288    1.00x
    filter-length               0      6.633        0.393    1.12x
    filter-composition          1      6.170        1.234    1.00x
    filter-composition          0      5.313        0.342    1.16x
    stat                        1      4.277        0.083    1.00x
    stat                        0      4.289        0.078    1.00x
    cstat                       1     15.493        1.190    1.00x
    cstat                       0     14.933        0.283    1.04x

Reading it:

- The parallel stage itself scales: 3.3-4.2x on 12 threads for the filters
  and `cstat` (per batch, 13 ms -> 4 ms). `stat`'s per-element work is a
  `size`, so threads buy nothing there.
- That stage is 10-20% of wall time. The rest is pulling the stream
  (decompressing 96 MB back into 1.5 GB of sequences) and, for the filters,
  rendering the survivors as FASTA and writing them: `filter-none`, which
  keeps everything, is the floor under every filter and spends about 6 s of
  its 10 s on output. `cstat` gathers 1.2M x 256 histogram counts (about
  2.5 GB) through a temp file before its summary sees them.
- So `--threads` is worth at most about 1.2x on wall today; the lever for
  the rest is the stream pull and the render path, not the compute.
