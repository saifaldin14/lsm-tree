# Publication package: GPU-offloaded RocksDB compaction

## The results in plain language

**What we tested.** RocksDB is a database that stores data in sorted files.
In the background, it regularly merges these files into bigger ones; this is
called *compaction*. Part of that work is compressing the data and computing
checksums. We moved that part from the CPU to a GPU and compared it with the
normal CPU-only version, running each test in matched pairs.

**The one-sentence answer:** the GPU makes the background merging about
**16–20% faster**, but it does **not** make the system cheaper to run. It uses
the same CPU, more electricity, and users see no difference.

**What we found:**

1. **Merging is faster.** Each merge job takes about 52 ms instead of 66 ms.
   This held in all 14 combinations of workload and request rate we tried,
   and on a 2 GiB database (17.5% faster per GiB).
2. **The CPU saving was an illusion.** RocksDB reported that the GPU version
   used 33% less CPU. It didn't. The GPU version hands its work to about 100
   short-lived helper threads, which RocksDB's counter doesn't see. When we
   counted every thread, CPU use was the same.
3. **The overhead is in moving data around.** The GPU removes the compression
   work, but adds about the same amount back: copying data to and from the
   GPU, setting up threads, and busy-waiting for the GPU to finish. Two
   settings (letting threads sleep while waiting, and keeping the database
   process on the CPU socket next to the GPU) cut this down, but only to break
   even with the CPU.
4. **It uses more electricity.** Total power for the CPU, memory and GPU is
   17–27% higher with the GPU, because the CPU doesn't use less power and the
   GPU adds about 58 W.
5. **Users don't notice.** Read and write response times and the maximum
   request rate are the same with or without the GPU.
6. **Where processes run matters more than the GPU.** Keeping the database on
   one CPU socket saved 8–16% CPU, with or without the GPU. That is more than
   the GPU ever saved.
7. **Moving more work to the GPU makes things worse.** When the GPU also
   unpacks the files being read, merging becomes 35–45% *slower* than the CPU.
8. **The original result depended on a setting nobody recorded.** The earlier
   speedup only reproduces with the GPU input path turned off
   (`LSM_DISABLE_GPU_INPUT=1`). We now record all such settings automatically.

**Bottom line:** GPU offload makes compaction faster, not cheaper. To be worth
it, the copying, thread setup and waiting around the GPU must be removed. The
detailed numbers, confidence intervals and figures are in
[RESULTS.md](RESULTS.md).

## Files

| File | Contents |
|---|---|
| [PLAIN-SUMMARY.md](PLAIN-SUMMARY.md) | Non-technical summary: what was measured, scenarios tested, results |
| [RESULTS.md](RESULTS.md) | Paper-ready results: findings, setup, figures, tables, threats, next steps |
| [AUDIT.md](AUDIT.md) | Claim-by-claim audit of `results/cuda-results-summary.md`, plus reproducibility findings |
| [analyze.py](analyze.py) | Independent analysis (numpy + matplotlib only) |
| [campaign.py](campaign.py) | Randomized revision campaign: CUDA sync × NUMA placement × workload, equal-resource baseline, RAPL energy |
| [profile.sh](profile.sh), [profile_summary.py](profile_summary.py) | Fixed-period `perf` CPU attribution of one paired trial |
| [data/trials.csv](data/trials.csv) | One row per trial: metadata, integrity, compaction, CPU, energy, latency |
| [data/jobs.csv](data/jobs.csv) | One row per compaction job |
| [data/profile-attribution.csv](data/profile-attribution.csv) | CPU seconds per process, thread, and symbol category |
| [tables/](tables/) | Paired, pooled/point-level, Holm, sensitivity, per-point, revision, and scale tables; `stats.json` |
| [figures/](figures/) | Figures 1–7 as vector PDF and 300 dpi PNG |

Regenerate everything from the archived raw bundles:

```sh
.venv/bin/python publication/analyze.py extract artifacts/zyra-final \
    --revision artifacts/zyra-revision/rev-campaign \
    --revision artifacts/zyra-revision/rev-campaign-gpu-input \
    --revision artifacts/zyra-revision/rev-scale \
    --profile artifacts/zyra-revision/rev-profile3-auto \
    --profile artifacts/zyra-revision/rev-profile3-blocking
.venv/bin/python publication/analyze.py report
.venv/bin/python -m unittest discover -s tests -p "test_publication_stats.py"
```

Raw bundles (`artifacts/zyra-final/`, `artifacts/zyra-revision/`; gitignored)
come from `zyra:/tmp/sa29ali/`, excluding database files and per-request logs
for the revision runs.
