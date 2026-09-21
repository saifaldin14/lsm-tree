# Compaction Block Transform

The engine integration is limited to the forward block-based compaction iterator. Foreground reads, native version selection, tombstones, snapshot handling, output compression, and installation are unchanged. A null transform uses the upstream path.

The version-locked CMake patch adds one table option and a bounded look-ahead at data-block initialization. A separate native index iterator supplies exact block handles. The adapter checks file bounds, limits each batch to 32 blocks, 4 MiB serialized input, and 16 MiB decoded output, and hands independently owned decoded blocks back to RocksDB. Individual blocks are limited to 2 MiB. This is a streaming limit, not a table-size limit. Reads beyond the current subcompaction boundary can occur within one batch and remain real measured I/O.

Supported inputs are format version 6, CRC32C, uncompressed or LZ4 data blocks, without dictionaries or separated key/value blocks. Unsupported inputs fail explicitly in the strict batch modes. Corruption never silently falls back. Input CRC covers the compression byte and applies RocksDB's native masking and context checksum rules. Native metadata parsing and checksum verification remain in place.

`cpu_native` keeps upstream RocksDB reads. `cpu_batched` is the control for this exact read-ahead and allocation path. GPU implementations must use this same interface, report actual executed blocks and dispatches, and produce byte-identical decoded contents. A GPU timing or speedup claim requires hardware execution and full primary commit verification. This integration is not GPU version merging or GPU output compression.

The batch path adds bounded per-iterator host storage, and each backend serializes access to its device workspace. Total memory still scales with simultaneous input-table iterators and worker jobs; per-job and system memory limits must be selected from measured occupancy. A 2 MiB block limit does not establish a whole-worker memory cap.