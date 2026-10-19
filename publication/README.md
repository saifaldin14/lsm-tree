# Publication package

Results of offloading RocksDB compaction to an NVIDIA GPU.

| File | Contents |
|---|---|
| [RESULTS.md](RESULTS.md) | Setup, methods, results, correctness, threats to validity, next steps |
| [PLAIN-SUMMARY.md](PLAIN-SUMMARY.md) | Non-technical summary |
| [AUDIT.md](AUDIT.md) | Every measurement problem found and how it was fixed |
| [figures/](figures/) | Four figures (PNG and PDF) |
| [data/compaction/](data/compaction/) | Raw `lsm_compaction_bench` output: one JSON line per design and repetition |
| [data/database/](data/database/) | Raw `system_campaign.py` output (`batched-ingest.jsonl`) and its trial order |
| [data/summary.md](data/summary.md) | Every number in the figures, generated from the raw data |
| [make_figures.py](make_figures.py) | Regenerates `figures/` and `data/summary.md` from `data/` |
| [compaction_sweep.sh](compaction_sweep.sh) | Runs the isolated compaction sweep |
| [system_campaign.py](system_campaign.py) | Runs whole-database grids with core pinning, background load and RAPL/GPU energy |

Figures:

1. `1_compaction_cpu`: CPU seconds per GiB compacted, by design and workload.
2. `2_compaction_speedup`: compaction speed-up over native RocksDB.
3. `3_database_batched`: whole-database throughput, p99 write latency and
   CPU per operation with batched writes.
4. `4_deferred_compaction`: throughput during the run vs including catch-up
   compaction.

```sh
python3 publication/make_figures.py
```

Grid for the whole-database run:
[../configs/system/batched-ingest.json](../configs/system/batched-ingest.json).
The earlier compression-only evaluation (figures, tables, analysis code) is
in git history at commit `a97ae43`.
