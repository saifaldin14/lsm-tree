# Publication package

Results of offloading RocksDB compaction to an NVIDIA GPU.

| File | Contents |
|---|---|
| [RESULTS.md](RESULTS.md) | Setup, methods, results, correctness, threats to validity, next steps |
| [PLAIN-SUMMARY.md](PLAIN-SUMMARY.md) | Non-technical summary |
| [AUDIT.md](AUDIT.md) | Every measurement problem found and how it was fixed |
| [figures/](figures/) | Figures (PDF and PNG) |
| [data/compaction/](data/compaction/) | Raw `lsm_compaction_bench` output: one JSON line per design and repetition |
| [data/database/](data/database/) | Raw `system_campaign.py` output (`batched-ingest.jsonl`) and its trial order |
| [data/summary.md](data/summary.md) | Every number in the figures, generated from the raw data |
| [make_figures.py](make_figures.py) | Regenerates `figures/` and `data/summary.md` from `data/` |
| [compaction_sweep.sh](compaction_sweep.sh) | Runs the isolated compaction sweep |
| [system_campaign.py](system_campaign.py) | Runs whole-database grids with core pinning, background load and RAPL/GPU energy |

Figures (vector PDF with embedded fonts, and 300 dpi PNG):

1. `fig1a_compaction_lz4` and `fig1b_compaction_other_codecs`: isolated
   compaction throughput (MiB/s) and CPU time per GiB, medians of 3 runs with
   min-max whiskers, for the LZ4 workloads and for ZSTD / no compression.
2. `fig2_database_batched`: whole-database throughput including catch-up
   (kops/s), catch-up time (s), p99 write latency (ms) and CPU per operation
   (µs) with batched writes.
3. `fig3_deferred_compaction`: throughput during the run vs including
   catch-up compaction, single and batched writes.

```sh
python3 publication/make_figures.py
```

Grid for the whole-database run:
[../configs/system/batched-ingest.json](../configs/system/batched-ingest.json).
The earlier compression-only evaluation (figures, tables, analysis code) is
in git history at commit `a97ae43`.
