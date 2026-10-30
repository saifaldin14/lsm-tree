# GPU compaction offload

LSM Lab offloads RocksDB 11.8 compaction to an NVIDIA GPU through a
`CompactionBlockTransform` table option added by a version-locked engine patch
([cmake/ApplyEnginePatch.cmake](../cmake/ApplyEnginePatch.cmake)). With a null
transform RocksDB runs unchanged. Three offload levels exist; each is a
superset of the previous one.

| Level | Transform / mode | GPU work | CPU work that remains |
|---|---|---|---|
| Output only | `cuda_hybrid` (`gpu_output` in the benchmarks, `cuda_local` in `lsm_db`) | LZ4 compression and CRC32C of output data blocks (LZ4 column families only) | reading and merging input, `CompactionIterator`, building every block |
| Merge (Stage 2) | `cuda_merge` with `LSM_DISABLE_GPU_BUILD=1` | read input blocks, CRC32C check, LZ4/ZSTD decode, parse, internal-key merge sort, drop hidden versions; output compression as above | `CompactionIterator` and per-key table building |
| Merge + block building (Stage 3) | `cuda_merge` | everything above, plus dropping obsolete deletions, sequence-number zeroing, block cutting, key delta encoding, restart arrays, LZ4 or ZSTD compression, CRC32C and Bloom-filter key hashes | file cuts, index entries (one per block), filter construction from hashes, properties, footer, file I/O |

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
2. verifies CRC32C and decodes LZ4 and ZSTD blocks (nvCOMP, one call per codec,
   in sub-batches of at most `LSM_NVCOMP_BATCH` blocks) on the device;
3. parses every restart interval in parallel and orders all entries by internal
   key. Entries arrive as sorted runs (each L0 file, and each lower level, whose
   files are ordered and disjoint), so the runs are merged pairwise with a
   merge-path kernel rather than sorted (`LSM_GPU_FULL_SORT=1` uses a full CUB
   merge sort instead);
4. drops entries hidden by a newer entry of the same user key in the same
   snapshot stripe, exactly as `CompactionIterator` does without a
   `SnapshotChecker`;
5. **Stage 3:** drops deletions as `CompactionIterator` would. A deletion at
   or below the earliest snapshot is dropped when the key cannot exist below
   the output level: always at the bottommost level, otherwise when the key
   falls outside the merged key ranges of every deeper file. At the
   bottommost level, a deletion with no older version of the same key left
   is also dropped. For the remaining entries it computes output sequence
   numbers (Puts zeroed at the bottommost level when at or below the earliest
   snapshot), shared
   key prefixes and XXPH3 filter hashes; cuts blocks by replaying
   `BlockBuilder`'s size estimate and `FlushBlockBySizePolicy` (a block also
   ends every 2,048 entries so the cut can run in parallel); encodes the blocks;
   compresses them with nvCOMP LZ4 or ZSTD (same block layout as RocksDB: a
   varint32 size prefix and a standard frame), keeping a compressed block only within
   `max_compressed_bytes_per_kb`; and computes the masked CRC32C (one warp per
   block). Each block is followed by a 5-byte gap for its trailer. Only the
   finished blocks, two boundary keys per block and the filter hashes are
   copied back.

With Stage 3 output, `LsmProcessBuiltOutput` replaces
`CompactionJob::ProcessKeyValue`. `CompactionOutputs::LsmAddBuiltBlocks` runs
RocksDB's own file-cut decision (`ShouldStopBefore`, refactored to take a key)
for every block and cuts files only between user keys. Each run of blocks that
stays in one file goes to `BlockBasedTableBuilder::LsmAddBuiltRun`, which writes
every block's trailer (type byte, precomputed checksum plus the format-6 context
modifier for its file offset) into its gap and appends the whole run with one
write, adding index entries as it goes. The filter builder keeps a pointer to the
file's key hashes; when the file is finished, the GPU sets the FastLocalBloom bits
(`BuildBloomFilter`), while RocksDB still chooses the filter size, probe count and
metadata. Without trailer gaps (or with `LSM_DISABLE_BUILT_RUNS=1`) blocks are added
one at a time (`LsmAddBuiltBlock`), with hashes copied into the filter builder. Job statistics (input and output records, raw
bytes, replaced records) are accounted so RocksDB's record-count verification
holds.

## Eligibility and fallback

A subcompaction uses the merged input only when there is no merge operator,
no compaction filter, a bytewise comparator, no blob files, no per-key
placement and no range tombstones. Input must be format version 6 with
CRC32C, binary-search data blocks, LZ4, ZSTD or no compression, and internal keys
of at most 512 bytes. Otherwise the job runs on RocksDB's own iterators and the reason
is recorded in `merge_last_fallback`.

Block building additionally requires:

- **Engine and filter options:** no `SnapshotChecker`, no
  `paranoid_file_checks`, no ingest-behind, no SST partitioner and no table
  property collectors. Filters must be none, or a built-in Bloom/Ribbon filter
  with whole-key filtering and no prefix extractor (format version 5 or
  later).
- **Table and compression options:** binary-search index, LZ4, ZSTD or no
  output compression without dictionaries, and default flush-block policy
  and alignment options.
- **Data:** every kept entry must be a Put or a Delete whose user key is at
  most 240 bytes. Merge operands, single deletes and wide-column entries use
  the Stage 2 path.

A failed GPU call falls back to the CPU for that job. A checksum or parse
error fails the compaction as RocksDB would.

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
| `LSM_GPU_MERGE_STREAMS` | GPU merge engines (CUDA streams with their own buffers) that compactions can use at once (default 2). |
| `LSM_NVCOMP_BATCH` | Largest number of blocks per nvCOMP call (default 16,384); larger jobs are split. |
| `LSM_GPU_OUTPUT_MIN_BLOCK_BYTES` | Smallest block sent to the output-only GPU path (default 32 KiB; 1024 in published runs). |
| `LSM_DISABLE_GPU_OUTPUT=1`, `LSM_FORCE_CPU_OUTPUT=1` | Keep output-only compression on the CPU. |
| `LSM_GPU_CPU_DECOMPRESS=1` | Earlier-design emulation: when a merge's input contains compressed blocks, the CPU checks and decompresses every input block (on the read threads) and the GPU receives them uncompressed. |
| `LSM_GPU_CPU_COMPRESS=1` | Earlier-design emulation: Stage 3 returns uncompressed blocks flagged `kCompressOnHost`, and RocksDB's own `WriteBlock` compresses and checksums them in the compaction thread. No effect with no compression. |
| `LSM_CUDA_SYNC=auto\|spin\|blocking` | How host threads wait for the GPU; `blocking` is used in published runs. |
| `LSM_MERGE_TRACE=1` | Prints per-phase timings of each merge to stderr. |
| `LSM_DISABLE_BUILT_RUNS=1` | Adds built blocks one at a time instead of in runs. |
| `LSM_GPU_FULL_SORT=1` | Orders merged entries with a full merge sort instead of merging sorted runs. |
| `LSM_DISABLE_GPU_FLUSH=1` | Keeps flush compression on the CPU (by default flushes use the output-only GPU path). |
| `LSM_TRACE_PINNED=1` | Prints every pinned host allocation (time, size, duration) to stderr. |
| `LSM_GPU_PINNED_PREALLOC_MB` | Creates every merge engine when the database opens and pins this much host memory per engine for inputs (and twice that for outputs), so compactions do not pin memory later. Pinning holds the process's memory-map lock and stalls threads that page-fault meanwhile. Published runs use 1024. |
| `LSM_DISABLE_JOB_PREFETCH=1` | Disables block prefetch in the batched input path. |

The transform's `statistics()` (in `lsm_db` summaries and benchmark output)
reports `merge_jobs`, `merge_fallbacks`, `merge_last_fallback`,
`merge_built_jobs`, `merge_built_blocks`, `merge_built_bytes`,
`merge_host_compressed_blocks`, `merge_cpu_decoded_bytes`, entry and byte
counts, read, device and wall time, and GPU filter builds (`filter_builds`,
`filter_keys`, `filter_ns`).

## Limits

- One GPU per process; up to `LSM_GPU_MERGE_STREAMS` compactions use it at
  once, and further ones wait.
- Device and pinned memory grow with the largest compaction job, per engine; pinned
  buffers grow to powers of two.
- The GPU's own throughput (about 1.4 GiB/s of LZ4 input, 0.9 GiB/s of ZSTD input on an
  RTX A6000 with 8 concurrent subcompactions) is limited mainly by nvCOMP compression.
- GPU ZSTD output uses nvCOMP's compression level, so compressed bytes and
  ratio differ from libzstd at RocksDB's configured level. The stored data is
  identical once decompressed.
- With direct I/O, RocksDB bypasses the page cache, but the GPU path reads
  input files with buffered `pread`. Input files are read once by
  compaction, so the page cache gives the GPU path little advantage.
- Merge operands, single deletes, wide-column entries and range tombstones
  are not offloaded.
