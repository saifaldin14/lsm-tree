# Why the GPU design writes more bytes per user byte than native RocksDB

Scope: archived data and code only; nothing was built or run for this analysis.
Scripts and their outputs are in [`wa/`](wa/) (run from any directory, e.g. `python3 publication/wa/wa_decompose.py`):

| Script | What it does | Output |
|---|---|---|
| `wa_decompose.py` | Per-cell medians of every WA-related field, for 10 campaigns | `wa_medians.txt`, `wa_medians.json` |
| `wa_table.py` | One row per cell, key columns | stdout |
| `wa_lag_model.py` | Per-trial table and regressions of compaction bytes on L0 depth and design | `wa_lag_model.txt`, `wa_trials.json` |
| `n6_iostat.py` | N6 device writes and %util from `iostat` against the benchmark's WA | `n6_iostat.txt` |
| `profile_stats.py` | Full RocksDB statistics of the two zyra profile runs (`data/profiles/phase-c`) | `profile_stats.txt` |

"Ours" = `full` design (`--mode cuda_merge`), "native" = `native_cpu`. All paths below are relative to
the repository root. `build/dev/_deps/rocksdb-src/...` is the locally patched RocksDB 11.8 tree
(the result of `cmake/ApplyEnginePatch.cmake`).

---

## 1. Summary

**Measured facts**

1. **The metric is the same for both designs, and the extra bytes reach the drive.** The two WA
   numbers use one formula from RocksDB tickers. At the process level (`/proc` write bytes) the
   ratio matches it to within 1% (ours 5.56 against 5.53; native 4.04 against 4.02, N6). At the
   device level (`iostat`, N6) ours wrote 655 MB/s and native 467 MB/s, a ratio of 1.40. The WA
   ratio is 1.38. Ingest-plus-drain WA equals window WA to within 1% (5.59/5.53 and 4.02/4.02).
   So native does not just push its compaction past the measured window.
2. **Flush bytes are equal. All of the extra is compaction output.** Flush/user is 0.646
   against 0.642. Compaction-write/user is 4.89 against 3.38 (N6).
3. **Ours does not write more bytes per entry.** Data bytes per entry are 91.2 against 90.9
   (+0.3%). Filter bytes per entry are 1.250 for both. Index bytes per entry are 0.602 against
   0.600. Compaction input bytes per record are 94.28 against 94.22 (+0.06%). Data block size
   is 2,364 against 2,363 B, and the entry counts after drain are equal. Hypothesis (a) is
   ruled out (≤0.5%).
4. **Ours rewrites each entry more times.** Compaction input records per user write are 8.69
   against 6.37 in N6 (+37%) and 10.0 against 6.35 in N1 (+58%). The extra is in the number
   of rewrites, not in the size of each record.
5. **Ours compacts with less accumulated data (hypothesis (b) is supported).**
   - Mean L0 files during the window: 5.6 against 9.4 (N6) and 2.2 against 8.5 (N1).
   - Pending compaction bytes: 4.8 against 7.1 GB (N6).
   - In the zyra profile runs, ours did one L0→base compaction every 4.0 flushes. Native did
     one every ≤26 flushes (73 against 8 multi-subcompaction L0 compactions).
   - Ours' WA falls steadily as the L0 trigger rises: 6.36, 5.22, 4.22, 3.67 and 3.09 at
     triggers 4, 8, 12, 16 and 24.
   - Across 84 trials, log L0 depth alone takes R² from 0.35 to 0.93.
6. **A design residual of about +13% remains after controlling for L0 depth** (malphite NVMe,
   38 trials, SE 2.2%; zyra trigger grids +13% ± 5%). It is about 0 on SATA (−4% ± 7%,
   n=12). Without the control, the design effect is +30% on NVMe.
7. **Settings differ only in `subcompactions`** (4 for ours, 8 or 16 for native). In the tune
   grid, ours at 8 subcompactions had WA 3.87–4.24, against 3.75–3.87 at 4. So more
   subcompactions do not lower ours' WA. The N2 data that `PREREGISTRATION.md:677` cites
   ("N2 ruled out subcompactions") is **not archived**. `followup/run.log` stops after 4 of
   6 N2 runs and records only throughput.

**Hypotheses (not measured)**

- H-lag: the main cause is eager compaction. In leveled compaction over a fixed key space, the
  bytes rewritten per byte moved down from L(n) are about `1 + size(L(n+1))/size(L(n))`.
  Native lets L0 and the other levels run above target, which batches more new data into each
  merge. Ours keeps up, so each merge carries less new data per rewritten byte. This is the
  mechanism behind fact 5. It is causal in the trigger experiments, which set L0 depth by
  configuration. Across designs it is still correlational.
- H-align: the +13% residual comes, at least partly, from **block-granular file cutting** in
  the GPU output path. RocksDB's grandparent-boundary file cut is evaluated only at the first
  key of each GPU-built block (`cmake/ApplyEnginePatch.cmake:500-523`). Cuts therefore land
  up to one block past the next-level file boundary. Each such output file then overlaps one
  extra next-level file by a sliver, and that whole file joins a later compaction. This
  follows from the code. Its size has not been measured.

**Ranking** (section 6): (1) eager compaction / less batching, strong; (2) design residual of
about 13%, cause unconfirmed, with file-cut misalignment as the leading code candidate,
moderate; (3) subcompaction setting, weak or ruled out; (4) larger output per entry, ruled
out; (5) metric or window artefact, ruled out.

---

## 2. Data used

| Campaign | Path under `publication/data/` | Machine and storage | Cells | Trials |
|---|---|---|---|---|
| N6 | `malphite-nvme/followup/n6` | malphite NVMe, buffered I/O, 120 s + 300 s | 8 cores, default trigger | 3 + 3 |
| N1 | `malphite-nvme/followup/n1` | malphite `/dev/shm` | as N6 | 3 + 3 |
| confirm | `malphite-nvme/retune/confirm` | malphite NVMe, direct I/O | 8 cores | 3 + 3 |
| tune | `malphite-nvme/retune/tune` | malphite NVMe, direct I/O | 8 flag sets per design | 8 + 8 |
| X8 | `malphite-nvme/x8/out` | malphite NVMe, direct I/O | 8 cores | 3 + 3 |
| H31 | `malphite-nvme/h31` | malphite NVMe, buffered, 60 s + 180 s | C4, BZ, TR | 2 per arm |
| SATA | `v3/v3-ssd` | SATA SSD, direct I/O | 8 and 16 cores | 3 per arm |
| trigger grid | `wa-trigger/run`, `wa-trigger/run16` | zyra `/dev/shm`, 30 s + 120 s | L0 trigger 4/8/12/16 | 1–2 per arm |
| W | `wa-trigger/w` | zyra `/dev/shm` | trigger 16/24, slowdown 2×, stop 3× | 2 per arm |
| Profile P | `profiles/phase-c/{full,native}` | zyra `/dev/shm`, 30 s + 60 s, `--stats-all` | 8 cores | 1 + 1 |

W ("Contention, write amplification and baselines on zyra", `PREREGISTRATION.md:757-760`,
outcome at `:796-799`) is `data/wa-trigger/w`. All 8 runs exited 0. Medians: native/ours WA
2.52/3.64 at trigger 16 and 2.17/3.09 at trigger 24. This matches the registered outcome.

Fields used per trial (`result.*`): `write_amplification`,
`ingest_and_compact.write_amplification`, `window_counters` (flush and compaction bytes,
compaction-listener totals), `table_properties`, `timeline` (1 s samples of L0 files, pending
compaction bytes, running compactions, immutable memtables), `levels_after_run`,
`accounting.window.process_write_mib_s` and `config`.

---

## 3. How WA is computed (hypothesis (d))

- Counters come from RocksDB tickers: `BYTES_WRITTEN`, `FLUSH_WRITE_BYTES`,
  `COMPACT_WRITE_BYTES`, `COMPACT_READ_BYTES`. A listener adds compaction totals
  (`apps/lsm_system_bench.cpp:1129-1147`; listener at `:285-309`).
- Window WA = (Δflush + Δcompaction write) / Δuser bytes over the measured window
  (`apps/lsm_system_bench.cpp:1311`, `:1338`).
- Ingest-and-compact WA uses the same formula from run start (warm-up included) to the end of
  the post-run drain, which waits for all compaction (`:1276-1283`, `:1343-1357`).
- Both designs share the code path. The only per-design difference is the backend object:
  `make_backend` returns `nullptr` for `native_cpu` (`apps/lsm_system_bench.cpp:350-359`).
- Flush bytes are ticked in RocksDB's `FlushJob` (`build/dev/_deps/rocksdb-src/db/flush_job.cc:170`).
  Compaction bytes are ticked from thread-local `IOSTATS(bytes_written)` at the end of each
  compaction (`.../db/compaction/compaction_job.cc:2484`). The GPU path writes outputs through
  RocksDB's own table builder and file writer on the compaction thread (section 5), so its
  writes are counted the same way.

Cross-checks (N6; `wa_medians.txt`, `n6_iostat.txt`):

| N6 medians | Ours | Native |
|---|---|---|
| WA, window (tickers) | 5.53 | 4.02 |
| WA, ingest + drain | 5.59 | 4.02 |
| `/proc` write bytes / user bytes | 5.56 | 4.04 |
| Device writes (`iostat`, mean MB/s, window) | 655 | 467 |
| Device writes / user bytes | 5.29 | 3.85 |
| Device %util (median) | 90% | 59% |
| Listener compaction output bytes = `COMPACT_WRITE_BYTES` | equal | equal |

**Conclusion (measured):** the WA difference is not a measurement artefact. Native does not
defer compaction past the window either: its drain took 12.1 s against 7.8 s for ours and
leaves WA unchanged. That contradicts the explanation at `PREREGISTRATION.md:571-573`
("native postpones compaction beyond the window"), at least for N6 and N1.

---

## 4. Decomposition (measured; medians per cell)

### 4.1 Same-I/O NVMe (N6), in memory on the same machine (N1), and NVMe with direct reads

| Metric | N6 ours | N6 native | N1 ours | N1 native | confirm ours | confirm native |
|---|---|---|---|---|---|---|
| Writes/s (M) | 0.838 | 0.822 | 1.620 | 0.948 | 0.975 | 0.909 |
| WA (window) | **5.53** | **4.02** | 6.40 | 4.05 | 3.76 | 3.18 |
| Flush / user | 0.646 | 0.642 | 0.649 | 0.643 | 0.646 | 0.643 |
| Compaction write / user | 4.89 | 3.38 | 5.75 | 3.40 | 3.11 | 2.54 |
| Compaction input records / user write | **8.69** | **6.37** | 10.02 | 6.35 | 5.87 | 4.99 |
| Compaction input bytes / input record | 94.28 | 94.22 | 94.56 | 94.38 | 94.66 | 94.51 |
| Compaction output / input bytes | 0.884 | 0.840 | 0.899 | 0.842 | 0.828 | 0.797 |
| Compaction jobs per GB user | 22.0 | 18.1 | 24.3 | 19.1 | 12.7 | 12.7 |
| MB input per job | 252 | 218 | 264 | 211 | 298 | 244 |
| MB output per job | 223 | 183 | 237 | 175 | 246 | 195 |
| Mean L0 files (window) | **5.6** | **9.4** | 2.2 | 8.5 | 13.2 | 15.3 |
| p90 L0 files | 8 | 13 | 4 | 18 | 19 | 21 |
| Mean pending compaction GB | 4.77 | 7.12 | 0.79 | 6.00 | 9.92 | 10.62 |
| Mean running compactions | 5.3 | 5.5 | 1.4 | 4.1 | 5.1 | 4.7 |
| Mean immutable memtables | 2.13 | 1.34 | 0.96 | 0.78 | 2.19 | 1.14 |
| Share of seconds with delayed writes | 0.45 | 0.14 | 0.00 | 0.07 | 0.36 | 0.23 |
| Stall fraction | 0.027 | 0.009 | 0.000 | 0.004 | 0.022 | 0.014 |

Output format at the end of the run (after drain and verification; `table_properties`,
`levels_after_run`):

| Metric | N6 ours | N6 native | N1 ours | N1 native | confirm ours | confirm native |
|---|---|---|---|---|---|---|
| Data bytes / entry | 91.20 | 90.89 | 91.51 | 90.98 | 91.28 | 90.92 |
| Filter bytes / entry | 1.250 | 1.250 | 1.250 | 1.250 | 1.250 | 1.250 |
| Index bytes / entry | 0.602 | 0.600 | 0.601 | 0.599 | 0.602 | 0.600 |
| Data bytes / block | 2,364 | 2,363 | 2,372 | 2,365 | 2,366 | 2,364 |
| Entries / block | 25.92 | 26.00 | 25.93 | 26.00 | 25.92 | 26.00 |
| Raw / stored (compression ratio) | 1.667 | 1.672 | 1.661 | 1.671 | 1.665 | 1.672 |
| Entries in live SSTs (M) | 57.01 | 57.06 | 57.10 | 57.37 | 57.07 | 57.02 |
| Live SST GB | 5.30 | 5.29 | 5.33 | 5.32 | 5.31 | 5.28 |
| Files, levels > 0 | 89 | 107 | 92 | 105 | 96 | 106 |
| Mean MB per file, levels > 0 | 58.8 | 50.4 | 57.6 | 50.6 | 55.7 | 49.0 |
| Shape | L0 1 file, L4–L6 | L4–L6 | | | | |

### 4.2 Other campaigns

| Campaign, cell | Design | Writes/s (M) | WA | Comp. write / user | In-records / write | Bytes / in-record | Mean L0 files | Pending GB |
|---|---|---|---|---|---|---|---|---|
| SATA 8 cores | ours | 0.462 | 3.65 | 3.01 | 5.73 | 94.13 | 13.2 | 10.4 |
| SATA 8 cores | native | 0.496 | 3.13 | 2.48 | 4.72 | 94.35 | 17.6 | 11.8 |
| SATA 16 cores | ours | 0.516 | 3.26 | 2.61 | 5.10 | 94.22 | 14.6 | 11.2 |
| SATA 16 cores | native | 0.497 | 2.98 | 2.34 | 4.62 | 94.23 | 17.2 | 10.1 |
| H31 C4 (4 cores) | ours | 0.713 | 5.71 | 5.07 | 9.03 | 93.57 | 3.9 | 2.5 |
| H31 C4 (4 cores) | native | 0.498 | 3.96 | 3.32 | 6.12 | 94.75 | 7.2 | 7.6 |
| H31 BZ (ZSTD at the bottom) | ours | 0.818 | 5.47 | 4.83 | 8.83 | 91.74 | 5.9 | 5.0 |
| H31 BZ (ZSTD at the bottom) | native | 0.751 | 3.83 | 3.19 | 6.17 | 92.25 | 8.7 | 7.6 |
| H31 TR (ours trigger 12, native 8) | ours | 1.069 | 3.79 | 3.15 | 5.97 | 94.11 | 11.7 | 3.6 |
| H31 TR (ours trigger 12, native 8) | native | 0.956 | 2.65 | 2.01 | 4.20 | 94.31 | 15.5 | 7.6 |
| Trigger grid, trigger 4 | ours | 1.273 | 6.36 | 5.72 | 10.04 | 93.64 | 2.5 | 1.1 |
| Trigger grid, trigger 8 | ours | 1.335 | 5.22 | 4.58 | 8.27 | 93.39 | 5.7 | 1.4 |
| Trigger grid, trigger 12 | ours | 1.384 | 4.22 | 3.58 | 6.65 | 93.81 | 8.7 | 1.4 |
| Trigger 16 (run16) | ours | 1.391 | 3.67 | 3.03 | 5.76 | 94.08 | 11.5 | 1.6 |
| W, trigger 16 | ours | 1.419 | 3.64 | 3.00 | 5.71 | 94.08 | 11.9 | 1.6 |
| W, trigger 24 | ours | 1.467 | 3.09 | 2.44 | 4.82 | 94.49 | 18.2 | 1.9 |
| Trigger grid, trigger 4 | native | 0.859 | 3.54 | 2.90 | 5.59 | 94.28 | 12.5 | 9.1 |
| Trigger grid, trigger 8 | native | 0.936 | 2.53 | 1.89 | 3.90 | 94.31 | 15.1 | 7.4 |
| Trigger grid, trigger 12 (n=1) | native | 0.704 | 3.39 | 2.75 | 5.39 | 94.18 | 15.9 | 4.6 |
| Trigger 16 (run16) | native | 0.624 | 3.00 | 2.37 | 4.77 | 94.64 | 16.4 | 3.9 |
| W, trigger 16 | native | 0.946 | 2.52 | 1.88 | 3.90 | 94.79 | 23.6 | 8.2 |
| W, trigger 24 | native | 0.971 | 2.17 | 1.54 | 3.37 | 95.09 | 34.4 | 10.0 |

The pattern is the same in every campaign:

- Flush bytes per user byte are equal.
- Bytes per stored entry are equal to within 1%. Ours is slightly higher on NVMe and slightly
  lower in BZ and C4.
- Ours compacts with fewer L0 files waiting and a smaller backlog.
- Ours rewrites each entry more times.

### 4.3 Zyra profile runs with full RocksDB statistics (`profile_stats.txt`)

RocksDB records `NUM_SUBCOMPACTIONS_SCHEDULED` only when a compaction forms two or more
subcompactions (`build/dev/_deps/rocksdb-src/db/compaction/compaction_job.cc:297-316`). In
leveled style, subcompactions are formed only for L0→base compactions (and manual ones)
(`.../db/compaction/compaction.cc:915-940`), and only when the input exceeds two target
files (`compaction_job.cc:688-698`). The count of this histogram is therefore a lower bound
on the number of L0→base compactions.

| Profile run (30 s + 60 s, `/dev/shm`) | Ours | Native |
|---|---|---|
| WA, window | 6.34 | 3.75 |
| Flushes | 294 | 210 |
| Non-trivial compactions | 373 | 160 |
| L0→base compactions with ≥2 subcompactions | 73 (median 4 subcompactions) | 8 (median 7.7) |
| Flushes per such L0 compaction | 4.0 | ≤26.2 (upper bound) |
| User GB per such L0 compaction | 0.23 | ≤1.52 |
| Compaction time p50 / p95 | 0.34 s / 1.06 s | 2.48 s / 7.40 s |

Ours starts an L0→base compaction at the trigger (4 files) almost every time. Native's are
fewer and larger. Some of native's L0 files may also have been merged by intra-L0
compactions, which this counter does not see.

---

## 5. Code paths that could change WA (hypotheses (a) and (c), plus others)

| Possible cause | What the code does | WA effect | Evidence |
|---|---|---|---|
| Options per design | `make_db_options` builds identical options for both designs. The only difference is `table.compaction_block_transform = transform` (`include/lsm_lab/db_options.hpp:71-147`, `:144`). Same block size, Bloom 10 bits/key, format_version 6, target file size, triggers and level base (`:79-91`, `:139-143`). The archived `config` differs only in `subcompactions` (N6, confirm, SATA, H31 C4/BZ, W), plus the trigger and level base in TR. | none, except subcompactions | config diff check |
| Compaction picking, triggers, trivial moves | `ApplyEnginePatch.cmake` patches no picker file and no `ComputeCompactionScore`. Patched files: `compaction_job.cc`, `compaction_outputs.{h,cc}`, `version_set.cc` (only `LevelIterator::Prepare/FinishCompactionBlocks`, `:169-226`), `table_cache`, block-based table builder/reader/iterator, filter builders. Trivial moves never reach `ProcessKeyValueCompaction`. | none | code |
| Which compactions are offloaded | `LsmMaybeCreateMergedInput` swaps only the *input iterator* of a subcompaction and leaves boundaries unchanged (`include/lsm_lab/compaction_merge_job.hpp:167-288`; hook in `ApplyEnginePatch.cmake:300-331`). Built output requires options the encoder can reproduce exactly (`compaction_merge_job.hpp:127-165`). | none on what is picked | code |
| Subcompaction boundaries | RocksDB's `GenSubcompactionBoundaries` is unchanged (`compaction_job.cc:547-711`). | only via the setting | tune data (section 6.3) |
| Obsolete versions dropped | The GPU drops hidden versions with the job's snapshot list (`compaction_merge_job.hpp:221-226`). Drops are added back to job statistics (`:293-313`). | none measured | entries after drain equal (57.0 against 57.1 M) |
| Block format and compression | The GPU replays `BlockBuilder`'s size estimate and `FlushBlockBySizePolicy`, compresses with nvCOMP LZ4/ZSTD in RocksDB's block layout, and keeps a compressed block only within `max_compressed_bytes_per_kb` (`docs/gpu-compaction.md:48-56`; spec fields at `compaction_merge_job.hpp:228-240`). | ≤0.3% | data bytes/entry +0.3%, bytes/block equal, entries/block −0.3% |
| Bloom filter, index, metadata | The filter builder takes key hashes. "Sizing, probe count and metadata stay RocksDB's own" (`ApplyEnginePatch.cmake:554`, `LsmFinishExternal` `:599-642`). Index entries come from RocksDB's builder. | none | filter 1.250 B/entry both; index 0.602 against 0.600 |
| **File cut granularity** | Native evaluates `ShouldStopBefore` for **every key** (`compaction_outputs.cc:530`). With built output, `LsmAddBuiltBlocks` evaluates `LsmShouldStopBefore` once per block, on the block's **first key** (`ApplyEnginePatch.cmake:496-523`; patched tree `compaction_outputs.cc:418-424`, and `:475` for the single-block path; driver `compaction_merge_job.hpp:340-361`). Grandparent crossings (`compaction_outputs.cc:133-187`) are seen only at block starts. The grandparent-aligned pre-cut (`:307-363`) therefore fires one block late whenever a boundary falls inside a block. With about 26 entries per block, a boundary lands exactly on a block start only about 1 time in 26. | plausible extra overlap at L(n)→L(n+1) | not measurable from archived data; ours' files are larger (55.7–58.8 against 48.9–50.6 MB), but subcompaction ends also shrink native's files |
| Output file size | Same `target_file_size_base` and the same `max_output_file_size` rule. The size estimate during runs is `EstimatedFileSize() + run_bytes` (`ApplyEnginePatch.cmake:503-505`). | small | see above |
| Write path / WAL / flush | Unchanged. The WAL is off in all campaigns. Flushes do not use the GPU. | none | flush/user equal |

Why late cuts can raise WA (code-level reasoning, unmeasured). Suppose block *B_i* contains
the boundary between grandparent files *g_j* and *g_(j+1)*. Ours cuts before *B_(i+1)*, so
the output file ends inside *g_(j+1)*. RocksDB includes every next-level file that overlaps
an input file's key range (any overlap, even one key). A later compaction of this file
therefore also reads and rewrites all of *g_(j+1)*, about 50–60 MB, to merge in about one
block of new data. In the N6 end-state shape, the extra file per compaction would be:

- L4→L5 (an L4 file overlaps about 1.5–3 L5 files): about +30–60% on that step.
- L5→L6 (about 11–12 L6 files): about +8–9% on that step.

How much of total WA each step represents is unknown. The harness records no per-level
compaction bytes.

---

## 6. Testing the four hypotheses

### 6.1 (a) Larger output per entry: ruled out

Data bytes per entry are +0.3% (N6), +0.6% (N1) and +0.4% (confirm). Compaction input bytes
per record are +0.06% (N6). Filter and index bytes are identical. The compression ratio is
0.3–0.6% worse for ours. Together these explain at most about 0.5% of a 38% WA gap.

### 6.2 (b) Eager compaction (less data per merge): supported, main cause

- Within a design, raising the L0 trigger (with the level base scaled) lowers ours' WA
  monotonically. The trigger grid and W give 6.36 → 5.22 → 4.22 → 3.67/3.64 → 3.09 as mean
  L0 depth rises from 2.5 to 18.2 files. Here L0 depth is set by configuration, so the
  relationship is causal.
- At similar L0 depth the designs come close:
  - Ours at trigger 16 (mean 11.5 L0 files): WA 3.67. Native at its default trigger 4 (12.5
    files): WA 3.54 (+4%).
  - In the NVMe tune grid, ours at 10.3–11.8 files had WA 3.75–3.87. Native at 10.4–11.2
    files had WA 3.68–3.94.
- Regression of log(compaction bytes / user bytes) on design and log(mean L0 files) across
  trials (`wa_lag_model.txt`):

| Trial set | n | Design effect, raw | Design effect after L0 depth | L0 elasticity | R², design only → with L0 |
|---|---|---|---|---|---|
| malphite NVMe, 8 cores, default trigger | 38 | +30.3% (SE 5.7) | **+13.3% (SE 2.2)** | −0.58 | 0.37 → 0.92 |
| malphite, all cells | 52 | +38.6% | +16.4% (SE 2.6) | −0.43 | 0.37 → 0.91 |
| zyra trigger grids | 20 | +72.3% | +13.4% (SE 5.1) | −0.47 | 0.51 → 0.96 |
| SATA | 12 | +18.0% | −3.7% (SE 7.3) | −0.93 | 0.69 → 0.84 |
| all | 84 | +42.0% | +13.8% (SE 2.0) | −0.45 | 0.35 → 0.93 |

  Caveat: L0 depth is itself an outcome of each design's speed, so across designs this is
  association, not proof. Adding log pending bytes leaves the malphite residual at +12.5%.
- Output/input bytes per compaction are 0.884 for ours against 0.840 for native (N6). Ours
  removes fewer overwritten versions per byte it compacts, which fits each merge carrying
  less accumulated data.
- About the README wording: "faster compactions start earlier with less data each" holds for
  *new* data per L0 compaction (4.0 flushes against ≤26). It does not hold for bytes per
  compaction job: ours' jobs are *larger* (252 against 218 MB input, N6), because with
  uniform keys every L0→base job rewrites the whole base level. A more accurate statement is
  that ours keeps L0 and the backlog small, so each merge combines less new data with the
  same existing data, and existing data is rewritten more often per user byte.
- Back-of-envelope (hypothesis), using the profile counts and a base level of about 250 MB,
  about 40 MB flushed files and 0.645 flush bytes per user byte. L0→base rewrite per flushed
  byte is (250+160)/160 = 2.6 for ours and (250+1,040)/1,040 = 1.2 for native. That is about
  0.85 extra compaction bytes per user byte, about a third of the profile gap (5.66 against
  2.96). The rest must come from lower levels. Native's backlog there (pending 8.7 against
  1.1 GB) means those levels sit above target, which lowers `size(L(n+1))/size(L(n))` per
  merge. The harness does not record the per-level sizes during the run needed to check
  this. `levels_after_run` is taken after the drain.

### 6.3 (c) Setting differences: weak or ruled out

- Only `subcompactions` differs (section 5).
- NVMe tune grid, direct I/O, n=1 per flag set:

| Design, subcompactions | Ours WA | Native WA |
|---|---|---|
| 4 | 3.75, 3.80, 3.82, 3.87 | 2.61, 2.97, 3.68, 3.94 |
| 8 | 3.87, 3.88, 4.10, 4.24 | 2.93, 3.13, 3.18, 3.66 |

  Moving ours to 8 subcompactions did not reduce its WA. If anything it rose, with lower L0
  depth (8.9–10.6 against 10.3–11.8 files).
- N2 (ours at 8 subcompactions, NVMe direct I/O) is cited as having ruled this out
  (`PREREGISTRATION.md:612-615`, `:677`). Its results are not in the archive:
  `malphite-nvme/followup/` has `n1`, `n5` and `n6` only. `followup/run.log:14-18` shows 4 of
  6 runs with throughput only (native 894.6k and 889.1k; ours 868.1k and 935.3k writes/s).
  The claim agrees with the tune data, but its own evidence cannot be checked.
- TR changed the trigger and level base together, and ours got the *higher* trigger (12
  against 8). Ours still wrote more (3.79 against 2.65). Native's chosen trigger is not what
  produces the gap.

### 6.4 (d) Metric or window artefact: ruled out

Section 3: the same formula; process-level and device-level bytes agree; ingest-plus-drain
equals window WA.

---

## 7. Ranked causes

| Rank | Cause | Evidence strength | Key numbers |
|---|---|---|---|
| 1 | Eager compaction: ours keeps L0 and the backlog small, so each merge carries less new data and existing data is rewritten more often | **Strong.** Consistent in 10 campaigns; causal within design (trigger grid); explains about 55–65% of the log gap in the pooled regression | N6 L0 5.6 against 9.4 files, pending 4.8 against 7.1 GB, records rewritten per write 8.69 against 6.37; profile 4.0 against ≤26 flushes per L0 compaction; ours' WA 6.36 → 3.09 from trigger 4 to 24 |
| 2 | Design residual after L0 depth, about +13%; leading code candidate: block-granular grandparent cut misalignment | **Moderate** for the residual (SE 2%, NVMe and zyra; absent on SATA); **hypothesis only** for its cause | residual +13.3% (NVMe), +13.4% (zyra), −3.7% (SATA); larger output files for ours (55.7–58.8 against 48.9–50.6 MB) |
| 3 | Subcompactions 4 against 8 | Weak / ruled out (tune grid); N2 data missing | ours at 8 subcompactions: WA 3.87–4.24 against 3.75–3.87 at 4 |
| 4 | Larger GPU output per entry (compression, block, filter, index) | Ruled out | +0.3% bytes per entry; filter and index equal |
| 5 | Metric or window differences | Ruled out | `/proc` 5.56/4.04 and `iostat` 5.29/3.85 against tickers 5.53/4.02 |

Effect on NVMe throughput (fact plus projection):

- Fact: in N6 ours kept the drive 90% busy (655 MB/s of writes); native kept it 59% busy.
- Projection, not measured: at a fixed device write rate of about 655 MB/s, WA 4.0 instead of
  5.5 would allow about 156 MiB/s of user data, about 1.11M writes/s (1.35× native's 0.822M),
  if the drive stays the only limit.
- Partial empirical support: in H31 TR, ours at trigger 12 reached 1.069M writes/s at WA 3.79
  on the same drive. This uses a shorter protocol (60 s + 180 s) than N6.

---

## 8. Proposed fixes

1. **Settings (cheap; addresses cause 1).** Give the GPU design a deeper L0 and a larger base
   level on storage: `--l0-trigger` 16–24, `--l0-slowdown` 2× and `--l0-stop` 3× the
   trigger, `--max-bytes-for-level-base` = trigger × 64 MiB. These are W's settings
   (`db_options.hpp:94-102` already supports them). In memory this cut ours' WA from 6.36
   to 3.09 and raised throughput from 1.27M to 1.47M (trigger 4 against W trigger 24, zyra).
   It has not been tested on NVMe beyond trigger 12 (TR).
2. **Adaptive trigger (code; addresses cause 1 without hurting read amplification when the
   drive is idle).** A controller in the library or `lsm_system_bench` that raises
   `level0_file_num_compaction_trigger` (and scales the base level) with `SetOptions` when the
   device is busy or the compaction backlog is small, and lowers it when L0 read
   amplification or stalls grow. The GPU's spare compaction speed then turns into batching
   instead of extra rewrites.
3. **Align built-output file cuts with grandparent boundaries (code; addresses cause 2 if
   confirmed).** Pass the user-key boundaries (`largest`) of the next-level files inside the
   subcompaction range in `CompactionMergeRequest.output`, next to the existing
   `beyond_ranges` (`compaction_merge_job.hpp:252-257`). Have the GPU block cutter end a block
   at each boundary, as it already ends blocks every 2,048 entries
   (`docs/gpu-compaction.md:50`). `LsmAddBuiltBlocks` (`ApplyEnginePatch.cmake:453-545`) can
   then cut exactly where native would. Cost: about one extra short block per grandparent
   file, negligible against about 2.2 M blocks.
   - Cheaper partial fix: in `LsmAddBuiltBlocks`, also run the grandparent check on each
     block's *last* key. When the block straddles a boundary and the file is above the
     pre-cut threshold, cut *before* that block. The straddle then moves to the next file's
     start, so this only helps if the pre-cut rule's asymmetry favours it. The GPU-side
     block end is the clean fix.
4. **Instrumentation (needed for both experiments).**
   - Record per-level compaction bytes: `rocksdb.cfstats` map `compaction.L<n>.WriteGB`,
     `ReadGB`, `Rn`, `Rnp1`, `Moved` at window start and end. `apps/lsm_db.cpp:395-405`
     already reads cf stats.
   - Extend `CompactionTotals` (`lsm_system_bench.cpp:285-309`) with per-(start level,
     output level) counts, input files per level, trivial moves and compaction reason from
     `CompactionJobInfo`.
   - Record level sizes in the 1 s timeline.
   - At the end of the run, dump per-file smallest and largest keys
     (`GetColumnFamilyMetaData`) to measure "sliver overlaps": next-level files overlapped
     by less than one block.

---

## 9. Proposed registerable experiments (in the style of `PREREGISTRATION.md`)

### WA-1: Is the residual caused by block-granular file cuts?

- **Run** (zyra, `/dev/shm`, 8 cores, LZ4, batched writes, `v3-headline.json` 8-core
  settings; 30 s warm-up plus 120 s; verification on; 3 blocks; new schedule seed). New
  instrumentation from fix 4 in every arm. Arms:
  - native;
  - ours (`cuda_merge`, built output, current cut logic);
  - ours with `LSM_DISABLE_GPU_BUILD=1` (merged input on the GPU, blocks built per key by
    RocksDB, so per-key file cuts; switch at `compaction_merge_job.hpp:130`);
  - ours with the boundary-aligned block cut (fix 3), once implemented.
  Each arm runs at L0 trigger 4 and 16 (level base trigger × 64 MiB).
- **Primary metric:** "sliver overlaps" per output file in levels 1 to n−1 at the end of the
  run (next-level files overlapped by less than 64 KiB). This is pure geometry and does not
  depend on timing.
- **Secondary:** WA over the window, per-level compaction write bytes per user byte, mean L0
  files, writes/s.
- **Hypothesis WA-1a:** ours with built output has ≥0.5 sliver overlaps per file. Native and
  ours with `LSM_DISABLE_GPU_BUILD` have ≤0.1.
- **Hypothesis WA-1b:** at the same trigger, the aligned-cut arm's compaction bytes per user
  byte are at least 5% below current ours, with mean L0 files within 10% of each other.
  Throughput may not drop by more than 3%.
- **Decision rule:**
  - WA-1a holds and WA-1b holds: misalignment is reported as a cause and its share of the gap
    is stated.
  - WA-1a fails: misalignment is ruled out, and the residual is reported as unexplained.
  - The `LSM_DISABLE_GPU_BUILD` arm compacts more slowly, so its WA comparison is reported
    only together with its L0 depth.

### WA-2: Does reducing ours' WA recover NVMe throughput?

- **Run** (malphite NVMe, `use-direct-io` false for both, as N6; 50 M records; 120 s warm-up
  plus 300 s; verification on; 3 blocks; new seed). Settings frozen from
  `retune/tuned-malphite.json` except:
  - ours at L0 trigger {4, 12, 24};
  - native at {4, 8, 24};
  - slowdown 2× and stop 3× the trigger, level base trigger × 64 MiB, as W.
  If fix 3 is accepted from WA-1, add ours with aligned cuts at the trigger ours selects.
- **Reported:** writes/s, WA (window and ingest plus drain), per-level compaction bytes, mean
  L0 files, device write MB/s and %util (`iostat`), CPU per write, p99 and p99.9 write
  latency, and read amplification proxies (L0 files, and point-lookup latency in a 10-second
  read probe after each run).
- **Selection rule (fixed before data):** per design, the trigger with the highest median
  writes/s whose stall fraction is below 5% and whose runs all pass verification.
- **Hypothesis WA-2a:** ours' WA at trigger 24 ≤ native's WA at trigger 4 (N6: 4.0).
- **Hypothesis WA-2b:** at the selected settings, ours' throughput ≥1.20× native's, with the
  bootstrap 95% CI of the block-paired ratio above 1.10.
- **Hypothesis WA-2c:** at the selected settings, ours' WA is within 15% of native's.
- **Decision rule:**
  - WA-2a and WA-2b hold: the README limitation is restated as "removed by an L0 setting on
    NVMe", with the settings named.
  - WA-2a holds but WA-2b fails: WA is not what caps ours on NVMe. The next candidate (flush
    and memtable-limit delays: N6 shows 2.1 against 1.3 immutable memtables and 45% against
    14% delayed seconds) is examined.
  - WA-2a fails: batching alone does not close the gap. WA-1's result decides whether the
    residual is in the output path.

---

## 10. Statements in the repository that this analysis bears on

| Location | Statement | Finding |
|---|---|---|
| `README.md:334-335` | "about 1.4× higher (5.5 against 4.0) … faster compactions start earlier with less data each" | Numbers are confirmed (5.53/4.02 = 1.38). The mechanism is supported for new data per L0 compaction, but jobs are not smaller in bytes (252 against 218 MB). About 13% remains unexplained on NVMe. |
| `README.md:208-214`, `FULL-RESULTS.md:362-368` | Drive about 90% busy for ours, more bytes per user byte | Confirmed by `iostat` (655 against 467 MB/s; 90% against 59%) |
| `FULL-RESULTS.md:376-379` | "Compaction scheduling is a candidate explanation, not an isolated cause; the trigger experiments … change level sizing too" | Accurate. Within-design trigger data now support it as the main cause. |
| `FULL-RESULTS.md:409-411`, `:683-684`; `PREREGISTRATION.md:743-746` | TR 3.79 against 2.65, "the extra-writing issue is not resolved" | Accurate. At TR, ours had 11.7 against 15.5 L0 files and 3.6 against 7.6 GB pending: still less batched. |
| `PREREGISTRATION.md:571-573` | Window WA higher for ours because "native postpones compaction beyond the window" | Not supported in N6 and N1: ingest-plus-drain WA equals window WA for both designs. |
| `PREREGISTRATION.md:674-678` | "Flush bytes are equal; the extra is compaction. N2 ruled out subcompactions." | Flush claim confirmed. N2 data are not archived. The tune grid is consistent with the claim. |
| `PREREGISTRATION.md:796-799` | W: 2.52/3.64 at trigger 16, 2.17/3.09 at trigger 24 | Reproduced from `data/wa-trigger/w`. |
| `publication/AUDIT.md`, `docs/gpu-compaction.md` | No statements about WA. `docs/gpu-compaction.md:58-62` says file cuts run "for every block … only between user keys" | That per-block cut is the code basis for H-align. |
