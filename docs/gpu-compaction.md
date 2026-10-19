# GPU compaction offload

LSM Lab offloads RocksDB 11.8 compaction to an NVIDIA GPU through a
`CompactionBlockTransform` table option added by a version-locked engine patch
([cmake/ApplyEnginePatch.cmake](../cmake/ApplyEnginePatch.cmake)). With a null
transform RocksDB runs unchanged. Three offload levels exist; each is a
superset of the previous one.

| Level | Transform / mode | GPU work | CPU work that remains |
|---|---|---|---|
| Output only | `cuda_hybrid` (`gpu_output` in the benchmarks, `cuda_local` in `lsm_db`) | LZ4 compression and CRC32C of output data blocks | reading and merging input, `CompactionIterator`, building every block |
| Merge (Stage 2) | `cuda_merge` with `LSM_DISABLE_GPU_BUILD=1` | read input blocks, CRC32C check, LZ4 decode, parse, internal-key merge sort, drop hidden versions; output compression as above | `CompactionIterator` and per-key table building |
| Merge + block building (Stage 3) | `cuda_merge` | everything above, plus sequence-number zeroing, block cutting, key delta encoding, restart arrays, LZ4, CRC32C and Bloom-filter key hashes | file cuts, index entries (one per block), filter construction from hashes, properties, footer, file I/O |

`cpu_merge` runs the same Stage 2 and Stage 3 algorithms on the CPU
([src/gpu/merge_cpu.cpp](../src/gpu/merge_cpu.cpp)). It is the reference
implementation the GPU is checked against.

## How a compaction is offloaded

For each subcompaction, `LsmMaybeCreateMergedInput`
([include/lsm_lab/compaction_merge_job.hpp](../include/lsm_lab/compaction_merge_job.hpp))
lists every data block of every input file within the subcompaction bounds
([compaction_merge_reader.hpp](../include/lsm_lab/compaction_merge_reader.hpp))
and sends one `CompactionMergeRequest` to the transform.

The CUDA engine ([src/gpu/cuda_merge.cu](../src/gpu/cuda_merge.cu)):

1. reads the blocks into pinned memory with `LSM_MERGE_READ_THREADS` threads;
2. verifies CRC32C and decodes LZ4 (nvCOMP) on the device;
3. parses every restart interval in parallel and merge-sorts all entries by
   internal key (CUB);
4. drops entries hidden by a newer entry of the same user key in the same
   snapshot stripe, exactly as `CompactionIterator` does without a
   `SnapshotChecker`;
5. **Stage 3:** for the kept entries, computes output sequence numbers (zeroed
   at the bottommost level when at or below the earliest snapshot), shared
   key prefixes and XXPH3 filter hashes; cuts blocks by replaying
   `BlockBuilder`'s size estimate and `FlushBlockBySizePolicy` (a block also
   ends every 2,048 entries so the cut can run in parallel); encodes the blocks;
   compresses them with nvCOMP LZ4, keeping a compressed block only within
   `max_compressed_bytes_per_kb`; and computes the masked CRC32C. Only the
   finished blocks, two boundary keys per block and the filter hashes are
   copied back.

With Stage 3 output, `LsmProcessBuiltOutput` replaces
`CompactionJob::ProcessKeyValue`. Each block goes through
`CompactionOutputs::LsmAddBuiltBlock`, which runs RocksDB's own file-cut
decision (`ShouldStopBefore`, refactored to take a key) and cuts files only
between user keys. `BlockBasedTableBuilder::LsmAddBuiltBlock` writes the block
with the precomputed checksum and context modifier, defers its index entry
until the next block's first key is known, and feeds the hashes to the
built-in Bloom/Ribbon builder. Job statistics (input and output records, raw
bytes, replaced records) are accounted so RocksDB's record-count verification
holds.

## Eligibility and fallback

A subcompaction uses the merged input only when there is no merge operator,
no compaction filter, a bytewise comparator, no blob files, no per-key
placement and no range tombstones. Input must be format version 6 with
CRC32C, binary-search data blocks, LZ4 or no compression, and internal keys
of at most 512 bytes. Otherwise the job runs on RocksDB's own iterators and the reason
is recorded in `merge_last_fallback`.

Block building additionally requires:

- **Engine and filter options:** no `SnapshotChecker`, no
  `paranoid_file_checks`, no ingest-behind, no SST partitioner and no table
  property collectors. Filters must be none, or a built-in Bloom/Ribbon filter
  with whole-key filtering and no prefix extractor (format version 5 or
  later).
- **Table and compression options:** binary-search index, LZ4 or no output
  compression without dictionaries, and default flush-block policy and
  alignment options.
- **Data:** every kept entry must be a Put whose user key is at most 240 bytes.

A job containing deletions therefore takes the Stage 2 path. A failed GPU
call, for example nvCOMP rejecting a very large batch, falls back to the CPU
for that job. A checksum or parse error fails the compaction as RocksDB would.

## Correctness checks

- `lsm_compaction_bench` builds the same database for every mode (base level,
  overlapping L0 overwrites and deletes, optional snapshots), runs one full
  compaction, then verifies checksums. It hashes every internal version (user
  key, sequence, type, value) and the results of up to 200,000 point lookups,
  which exercise the Bloom filters and indexes. It exits with status 3 if any
  mode differs from the first.
- `lsm_system_bench` checks every read online and every key and value after
  each run.
- CTest covers the transforms, engine integration, datasets and the worker
  protocol.

## Configuration

| Variable | Effect |
|---|---|
| `LSM_DISABLE_GPU_INPUT=1` | Disables the older batched input-decode path (`cuda_batched`). It is slower than the CPU and is set in every published run. |
| `LSM_DISABLE_GPU_MERGE=1` | Disables merged input (Stage 2 and 3). |
| `LSM_DISABLE_GPU_BUILD=1` | Disables block building; Stage 2 only. |
| `LSM_GPU_MERGE_KEEP_HIDDEN=1` | Keeps hidden versions in the merged stream; `CompactionIterator` drops them. Also disables block building. |
| `LSM_MERGE_READ_THREADS` | Threads reading input files (default 4). |
| `LSM_GPU_OUTPUT_MIN_BLOCK_BYTES` | Smallest block sent to the output-only GPU path (default 32 KiB; 1024 in published runs). |
| `LSM_DISABLE_GPU_OUTPUT=1`, `LSM_FORCE_CPU_OUTPUT=1` | Keep output-only compression on the CPU. |
| `LSM_CUDA_SYNC=auto\|spin\|blocking` | How host threads wait for the GPU; `blocking` is used in published runs. |
| `LSM_MERGE_TRACE=1` | Prints per-phase timings of each merge to stderr. |
| `LSM_DISABLE_JOB_PREFETCH=1` | Disables block prefetch in the batched input path. |

The transform's `statistics()` (in `lsm_db` summaries and benchmark output)
reports `merge_jobs`, `merge_fallbacks`, `merge_last_fallback`,
`merge_built_jobs`, `merge_built_blocks`, `merge_built_bytes`, entry and byte
counts, and read, device and wall time.

## Limits

- One GPU; merges run one at a time per process (the engine holds a mutex).
- Device and pinned memory grow with the largest compaction job.
- Very large jobs can exceed what nvCOMP accepts in a single batch and then
  fall back to the CPU. Splitting compression into smaller batches is still
  to do.
- Deletions and non-Put entries use Stage 2. Dropping tombstones on the GPU
  needs the lower-level key-range check that `CompactionIterator` performs.
- ZSTD is not offloaded; ZSTD compactions run on the CPU.
