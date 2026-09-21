# Full-value LSM compaction over real storage

This C++20 research artifact compacts immutable sorted tables with complete value bytes, real file or TCP I/O, and durable output publication. It compares a parallel k-way CPU merge, a specialized parallel dense-key CPU implementation, and native Metal execution on Apple unified memory. An optional CUDA implementation is present but has not been built or tested on this Mac.

## Measured findings

The frozen 2026-09-06 study contains 120 measured trials on an Apple M3, using a separate storage process over loopback TCP. All trials passed oracle comparison, persisted-byte verification, and durable-write acknowledgement checks.

- Metal's high-fan-in median end-to-end latency advantage over conventional parallel k-way merge is **1.50 to 1.52 times**, with each seed's paired 95% speedup interval above 1.
- The specialized dense CPU is faster in processing wall time. Its end-to-end comparisons with Metal all have intervals containing 1, so the study does not establish a latency advantage over the strongest measured CPU implementation.
- Metal reduces processing client CPU time by 93.18% to 95.07% relative to dense CPU. Whole-job client CPU median reductions are 45.19% to 48.40%; no confidence intervals were computed for that whole-job endpoint. These measurements exclude the storage server's CPU time and do not establish energy savings, lower cost, or better foreground database throughput.

The 1.50 to 1.52 range describes point estimates, not a guaranteed 1.5-times lower bound. This is a same-host TCP study, not evidence of a cross-host remote GPU advantage. Exact medians, confidence intervals, failed earlier confirmation results, and provenance are in [docs/real-results.md](docs/real-results.md). The artifact inventory is [results/README.md](results/README.md).

## Build and run

Requires a C++20 compiler and CMake 3.20 or newer. On macOS, the build also uses Objective-C++, Foundation, and Metal. No Python environment is required for the current C++ workflow.

1. Run `make test` to configure a Release build, build the storage server and test executables, and run CTest.
2. Run `make pilot` for a small real-I/O smoke experiment using [configs/pilot.conf](configs/pilot.conf). It launches an actual sibling storage-server process, transfers full table bytes over TCP, and validates every trial. GPU availability is optional for this smoke run only.
3. Run `make paper` to build [paper/main.tex](paper/main.tex), using `pdflatex` and `bibtex`. The manuscript targets four pages and reports the frozen final study; a new pilot does not replace that evidence.

Release and AddressSanitizer/UndefinedBehaviorSanitizer validation passed all three CTest targets: 13 core, 6 storage, and 7 experiment test groups. The explicit Metal-required check also passed on the M3, including the exact-64-bit fallback, two-command output, repeated keys, and retained native-buffer ownership. To require actual Metal execution rather than allow a GPU skip, run [build/lsm_tests](build/lsm_tests) with `--require-metal`. CUDA was explicitly skipped because it was not compiled. Linux execution is not established by this session's results.

[CMakePresets.json](CMakePresets.json) provides `release` and `sanitizers` presets. The sanitizer workflow is `cmake --preset sanitizers`, `cmake --build --preset sanitizers`, then `ctest --preset sanitizers`. Host sanitizers do not instrument GPU instructions; GPU correctness is checked against full expected bytes. Build options include `-DLSM_ENABLE_METAL=OFF` and `-DLSM_ENABLE_CUDA=OFF`. CUDA is enabled only when a CUDA compiler is found. Uncompiled backends are absent from the registry, and unavailable compiled devices do not receive substitute timings.

For a separately declared experiment, [build/lsm_compact_bench](build/lsm_compact_bench) accepts `--config` and `--output`, with optional `--latex-table`. The final configuration is [configs/real-final.conf](configs/real-final.conf). Preserve the frozen outputs and source snapshot when reproducing or extending the study; do not rerun cases until a desired interval appears.

## What is implemented

Each live record has its complete seeded pseudorandom value, with a fixed value size per table. Tombstones have no payload. The custom little-endian format uses a 64-byte header, run prefixes, 32-byte records, and a full value area. It is not a RocksDB SST format. Recency uses signed 64-bit sequence numbers, with deterministic input-offset tie breaking. Tombstone dropping assumes bottommost compaction with no snapshots.

Both CPU implementations select winners in parallel and gather full payloads in parallel. Metal uses sorted per-run winner indexing and cross-run reduction, with an exact 64-bit atomic fallback for larger index matrices. Stable output ranking, metadata scatter, and full payload gathering execute on the GPU. The final workloads use one command buffer; an exact-size two-command path handles larger output bounds. This is an in-memory whole-table implementation, not a streaming compaction pipeline.

Every timed trial performs a new input read. Metal input buffers are allocated inside that read, and file reads or TCP receives fill their native pages directly. Output allocation, page commitment, full materialization, and device synchronization are timed. Reusable scratch is overwritten for each job; neither input values nor previous winners are cached.

The TCP service performs actual PUT and GET operations against files. A successful macOS PUT acknowledgement follows `F_FULLFSYNC`, atomic rename, and parent-directory `fsync`. The Linux implementation uses `fdatasync` in place of `F_FULLFSYNC`, but was not executed in this session. This protocol has not been subjected to a formal power-cut recovery study.

## Timing and deployment limits

End-to-end time runs from starting the input GET through durable output PUT acknowledgement. Input generation, initial upload, oracle construction, backend initialization, and declared warmups are excluded. Full oracle checking and persisted readback are performed afterward and reported separately, typically about 15 ms per final-study job.

OS page cache hits are allowed, with no eviction. Actual I/O still occurs on every trial, but the results are not cold physical SSD read measurements. The tested endpoint was loopback on one M3, sharing host resources with the client. A configured external TCP endpoint is supported, but no multi-host measurement is claimed. The protocol has neither authentication nor TLS and must be restricted to a trusted network.

Dense-key specialization, the 2 GiB table limit, and simplified bottommost point-version semantics bound the results. Snapshots, range tombstones, non-bottommost tombstone retention, arbitrary byte-string keys, codecs, streaming, and RocksDB integration are absent. CUDA source performs full input transfer, GPU selection and payload gathering, and complete output transfer; implementation alone is not CUDA validation.

Historical metadata-only, modeled-I/O results remain archived. Their earlier 1.22-times modeled result is ineligible as real-storage evidence. The current experiment engine rejects modeled-I/O settings and does not execute a hash-only or sleep-based storage path.

## Documentation and source

- [docs/architecture.md](docs/architecture.md): implemented format, backend contracts, storage, and correctness boundaries.
- [docs/experiment-plan.md](docs/experiment-plan.md): current protocol and claim rules.
- [docs/real-validation-plan.md](docs/real-validation-plan.md): preserved original confirmation plan and dated amendment.
- [include/remote_gpu_lsm/backend.hpp](include/remote_gpu_lsm/backend.hpp), [include/remote_gpu_lsm/model.hpp](include/remote_gpu_lsm/model.hpp), and [include/remote_gpu_lsm/storage.hpp](include/remote_gpu_lsm/storage.hpp): public interfaces.
- [tests/test_main.cpp](tests/test_main.cpp), [tests/storage_test.cpp](tests/storage_test.cpp), and [tests/experiment_test.cpp](tests/experiment_test.cpp): correctness and real-storage integration checks.
- [paper/main.tex](paper/main.tex) and [paper/references.bib](paper/references.bib): manuscript and bibliography.