# Workloads

Besides the uniform and YCSB-style key-value runs, the system benchmark and RocksDB's own
`db_bench` run four workloads modelled on real RocksDB deployments. Every design runs the same
binary, database and settings; only the compaction path differs. Each run lasts a fixed time on
a fresh copy of a preloaded database, and every workload in `lsm_system_bench` is verified: each
read and scan is checked against what the client last wrote, and the whole database is checked
at the end.

## 1. Meta production workloads (MixGraph)

- **Source:** Cao et al., "Characterizing, Modeling, and Benchmarking RocksDB Key-Value
  Workloads at Facebook", FAST'20, and the ZippyDB model published with `db_bench --benchmarks=mixgraph`
  (RocksDB wiki, "Synthetic Workload Generation based on Models").
- **Driver:** `lsm_db_bench`, RocksDB's `db_bench` built from the pinned source with one added
  line that installs our compaction transform (`LSM_DB_BENCH_MODE`, see
  `bench/db_bench/transform_hook.cc`). Configure with `-DLSM_BUILD_DB_BENCH=ON` and gflags.
- **Runner:** `publication/mixgraph/run.sh`.
- **Parameters:**
  - 50 M keys of 48 B; value sizes from the published Generalized Pareto model (k 0.2615,
    sigma 25.45, mean about 35 B);
  - key-range hotness: two-term exponential over 30 key ranges (a 14.18, b −2.917,
    c 0.0164, d −0.08082);
  - scan lengths: Generalized Pareto (k 2.517, sigma 14.236);
  - `zippydb`: 85% get, 14% put, 1% seek, as published;
  - `zippydb_write`: 49% get, 50% put, 1% seek (ours, because a 14% put rate barely
    exercises compaction).
- **Deviation:** the published sine rate limit is left out, so a run measures the highest
  sustained rate rather than replaying the daily load curve.
- `native_cpu` runs double as the `db_bench` cross-check of the native baseline.

## 2. Stream-processing state store

- **Modelled on:** Apache Flink's RocksDB state backend and Kafka Streams state stores. Both
  keep operator state in RocksDB with the RocksDB write-ahead log disabled (durability comes
  from checkpoints or changelog topics; Flink: FLINK-3654, Kafka Streams: `RocksDBStore`).
- **Settings:** `--key-bytes 24 --value-bytes 64 --value-bytes-max 512 --distribution zipfian
  --zipf-theta 0.99 --rmw-percent 70 --delete-percent 10 --disable-wal true`.
- **Meaning:**
  - keys stand for a key group, the record key and a namespace (24 B);
  - 70% read-modify-writes (an event updates its key's aggregate);
  - 20% puts (window appends and overwrites);
  - 10% deletes (state expiry and window cleanup, as point deletes);
  - skewed keys (Zipfian 0.99), since a few keys receive most events.

## 3. Storage metadata

- **Modelled on:** metadata stores such as Ceph BlueStore (object, omap and allocation
  records under one-letter key prefixes) and TiKV's Raft and lock records: small keys and
  values, many overwrites and deletes, and short prefix scans.
- **Settings:** `--key-bytes 20 --key-bytes-max 60 --value-bytes 0 --value-bytes-max 200
  --key-families 6 --read-percent 25 --scan-percent 5 --scan-length 20 --delete-percent 20
  --distribution zipfian --zipf-theta 0.8` with the write-ahead log on.
- **Meaning:**
  - keys of 20–60 B, each starting with one of 6 family prefixes, so each family is a
    contiguous key range;
  - values of 0–200 B;
  - 50% puts (mostly overwrites), 20% deletes, 25% point reads, 5% scans of 20 keys.

## 4. Time series and log ingestion

- **Modelled on:** time-series and log stores on RocksDB: keys made of a series id and an
  increasing timestamp, batched ingestion, reads of recent data and retention deletes.
- **Settings:** `--workload timeseries --retention 1000 --value-bytes 32 --batch-size 100
  --read-percent 5 --scan-percent 5 --scan-length 60`, with the write-ahead log on.
- **Meaning:**
  - `records / retention` series (50,000 for 50 M records), each keeping its latest 1,000
    points;
  - keys are (series, timestamp), both big-endian, so a series is stored in time order;
  - an append writes the next point of a series and deletes the point that leaves the
    retention window, 100 appends per write batch;
  - 5% point reads of a random retained point, 5% scans of the latest 60 points.
- **Expectation:** new keys arrive in order within each series, so compaction does less
  merging work than in the other workloads. This is the case where offloading compaction is
  expected to help least.

## Features the GPU path does not run

Merge operators, range deletes and compaction filters run on RocksDB's CPU path. These
workloads use point deletes (which real systems can also use), and every run reports the GPU
fallback count (`cpu_fallback_batches` in the transform statistics).
