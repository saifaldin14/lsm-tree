# Reproduction checklist

Steps to reproduce every number and figure in this package. Hardware used:
Zyra (4 × Xeon Gold 5220, 192 GB RAM, NVIDIA RTX A6000 on NUMA node 0,
Micron 5200 SATA SSD), Linux, CUDA 12.3, GCC 12.1.

## 1. Build (about 1 hour the first time)

```sh
bash scripts/bootstrap.sh
bash scripts/bootstrap-cuda.sh
cmake --preset dev -DLSM_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=native -DLSM_ROCKSDB_PORTABLE=0
cmake --build --preset dev -j
ctest --preset dev --output-on-failure      # all suites must pass
loginctl enable-linger $USER                 # keep /dev/shm data across logouts
```

## 2. Golden databases

```sh
build/dev/lsm_system_bench --phase preload --dir /dev/shm/$USER/sys/golden-50m --records 50000000 --threads 8
build/dev/lsm_system_bench --phase preload --dir /tmp/$USER/ssd/golden-200m --records 200000000 --threads 8
```

Both verify every key before exiting.

## 3. Campaigns

[`run_all.sh`](run_all.sh) runs everything in the order and with the seeds
of [PREREGISTRATION.md](PREREGISTRATION.md). It resumes where it stopped if
it is interrupted.

```sh
bash publication/run_all.sh /tmp/$USER/campaigns
```

`configs/system/ssd-scale.json` names the SSD golden database and scratch
directory by absolute path (`/tmp/sa29ali/ssd`); change both entries for
another user or host.

| Campaign | Trials | Approximate machine time |
|---|---|---|
| Isolated compaction | 6 workloads × 4 designs × 4 runs | 1.2 h |
| Main | 80 | 5.5 h |
| Equal cost | 40 | 3 h |
| YCSB | 48 | 3 h |
| Contention | 30 | 2 h |
| Read trade-off | 18 | 1.3 h |
| Shared GPU | 24 | 2 h |
| SSD scale | 12 | 4 h |
| `db_bench` cross-check | 6 | 1 h |

## 4. Figures and tables

```sh
publication/fetch_campaigns.sh zyra /tmp/$USER/campaigns   # or copy results.jsonl files into publication/data/campaigns/
python3 publication/make_figures.py
```

`make_figures.py` refuses any run that failed verification and writes
`publication/data/summary.md` with every number in the figures.

## 5. Checks before trusting a result

- Every isolated run reports `matches_reference: true`.
- Every database trial has `exit_code 0`, a `verification` section, and
  `merge_fallbacks` 0 for GPU designs.
- `foreign_load` shows no more than 5% of host CPU used by other users.
