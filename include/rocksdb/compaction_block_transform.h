#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE
{
    struct CompactionBlockInput
    {
        Slice serialized;
        std::uint32_t checksum_modifier = 0;
    };

    struct CompactionBlockKey
    {
        std::uint64_t file_context = 0;
        std::uint64_t block_offset = 0;
    };

    struct CompactionOutputBlock
    {
        std::string contents;
        std::uint32_t checksum = 0;
        bool compressed = false;
        // SubmitOutput hands each input block back here so the caller need
        // not keep its own copy (empty for BuildOutput).
        std::string uncompressed;
    };

    // One data block of a compaction input file: file offset and size of the
    // block contents (excluding the 5-byte trailer) and the checksum modifier
    // for format_version 6 context checksums.
    struct CompactionMergeBlock
    {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::uint32_t checksum_modifier = 0;
    };

    struct CompactionMergeFile
    {
        std::string path;
        std::uint64_t file_number = 0;
        std::uint64_t file_size = 0;
        int level = 0;
        std::vector<CompactionMergeBlock> blocks;
    };

    // Output encoding requested together with a merged input. When enabled and
    // every entry left after drop_hidden is a Put or Delete whose user key has
    // at most 240 bytes, the accelerator returns finished data blocks
    // (CompactionBuiltOutput) instead of an entry stream; otherwise it returns
    // the entry stream as usual. Blocks follow the block-based table format
    // (format_version 6, CRC32C, binary-search data blocks, delta-encoded keys,
    // no value delta encoding, no uniformity flag) and RocksDB's
    // FlushBlockBySizePolicy, except that a block also ends every
    // kBuiltOutputChunkEntries entries.
    struct CompactionOutputSpec
    {
        static constexpr std::uint32_t kBuiltOutputChunkEntries = 2048;
        bool enabled = false;
        std::uint32_t block_bytes = 4096;
        std::uint32_t block_size_deviation = 10;
        std::uint32_t restart_interval = 16;
        // RocksDB stores a block compressed only if the compressed form (with
        // its varint32 size prefix) fits in raw * max_compressed_bytes_per_kb / 1024.
        std::uint32_t max_compressed_bytes_per_kb = 896;
        bool lz4 = true;
        // ZSTD instead of LZ4 (same block layout: varint32 size prefix, then a
        // standard ZSTD frame). Compressed bytes may differ from libzstd's.
        bool zstd = false;
        // Bottommost output: sequence numbers <= earliest_snapshot become 0,
        // as CompactionIterator::PrepareOutput does.
        bool zero_sequences = false;
        std::uint64_t earliest_snapshot = 0;
        // Compute the 64-bit filter hash (RocksDB Hash64 of the user key) of
        // every entry whose user key differs from the previous entry's.
        bool filter_hashes = true;
        // Deletions follow CompactionIterator: a deletion at or below
        // earliest_snapshot_seq is dropped when the key cannot exist below the
        // output level (always at the bottommost level; otherwise when it lies
        // outside every range in beyond_ranges, if check_beyond_ranges is
        // set). At the bottommost level a deletion with no older version of the
        // same user key is also dropped. beyond_ranges holds the merged,
        // sorted, disjoint [smallest, largest] user-key ranges of all files
        // below the output level.
        bool bottommost = false;
        std::uint64_t earliest_snapshot_seq = ~0ULL;
        bool check_beyond_ranges = false;
        std::vector<std::pair<std::string, std::string>> beyond_ranges;
    };

    // One finished data block. contents excludes the 5-byte trailer; checksum
    // is the masked CRC32C of contents plus the compression-type byte, before
    // the format_version 6 context modifier. Keys are output internal keys
    // (after sequence zeroing).
    struct CompactionBuiltBlock
    {
        std::uint64_t contents_offset = 0;
        std::uint32_t contents_bytes = 0;
        std::uint32_t uncompressed_bytes = 0;
        std::uint32_t checksum = 0;
        // Block compression type byte: 0 (none), 4 (LZ4) or 7 (ZSTD).
        std::uint32_t compressed = 0;
        std::uint64_t keys_offset = 0;
        std::uint32_t first_key_bytes = 0;
        std::uint32_t last_key_bytes = 0;
        std::uint32_t entries = 0;
        std::uint32_t hash_count = 0;
        std::uint32_t deletions = 0;
        // kCompressOnHost: contents are the uncompressed block and checksum is unset; the table
        // builder compresses and checksums it as for any block it builds (earlier-design emulation).
        std::uint32_t flags = 0;
        static constexpr std::uint32_t kCompressOnHost = 1;
        std::uint64_t hash_offset = 0;
        std::uint64_t raw_key_bytes = 0;
        std::uint64_t raw_value_bytes = 0;
        std::uint64_t smallest_seqno = 0;
        std::uint64_t largest_seqno = 0;
    };

    // Finished output of one (sub)compaction, in key order. The first key of
    // block i is keys + blocks[i].keys_offset and its last key follows it.
    // Producers leave a kBuiltTrailerGap-byte gap after each block's contents, and
    // contents_capacity is the writable size of contents (0 if unknown). The table builder
    // then writes each block's trailer into its gap and appends whole runs of blocks with
    // one write; without the gaps it adds blocks one at a time.
    static constexpr std::uint32_t kBuiltTrailerGap = 5;
    struct CompactionBuiltOutput
    {
        const CompactionBuiltBlock *blocks = nullptr;
        std::size_t block_count = 0;
        const char *contents = nullptr;
        std::uint64_t contents_capacity = 0;
        const char *keys = nullptr;
        const std::uint64_t *hashes = nullptr;
        std::uint64_t entries = 0;
    };

    // Request to produce the merged input stream of one (sub)compaction: every
    // entry of the listed blocks, in internal-key order (bytewise user key
    // ascending, then packed sequence/type descending), restricted to user
    // keys in [start_user_key, end_user_key) when the bounds are set.
    struct CompactionMergeRequest
    {
        std::vector<CompactionMergeFile> files;
        bool has_start = false;
        bool has_end = false;
        std::string start_user_key;
        std::string end_user_key;
        // When set, drop every entry hidden by a newer entry of the same user
        // key in the same snapshot stripe (no snapshot in [older, newer)).
        // snapshots must be sorted ascending.
        bool drop_hidden = false;
        std::vector<std::uint64_t> snapshots;
        CompactionOutputSpec output;
    };

    // Packed layout: the internal key starts at offset, the value follows it.
    struct CompactionMergedEntry
    {
        std::uint64_t offset = 0;
        std::uint32_t key_bytes = 0;
        std::uint32_t value_bytes = 0;
    };

    // Entries removed by drop_hidden, so the caller can keep input statistics exact.
    struct CompactionMergeDropped
    {
        std::uint64_t entries = 0;
        std::uint64_t deletions = 0;
        std::uint64_t key_bytes = 0;
        std::uint64_t value_bytes = 0;
    };

    // Owns the merged entries; keys are internal keys. Slices stay valid for
    // the lifetime of the object.
    class CompactionMergedInput
    {
    public:
        virtual ~CompactionMergedInput() = default;
        virtual std::size_t size() const noexcept = 0;
        virtual Slice key(std::size_t index) const noexcept = 0;
        virtual Slice value(std::size_t index) const noexcept = 0;
        // Optional zero-overhead access to a packed layout.
        virtual const char *packed_data() const noexcept { return nullptr; }
        virtual const CompactionMergedEntry *packed_entries() const noexcept { return nullptr; }
        virtual CompactionMergeDropped dropped() const noexcept { return {}; }
        // Non-null when the request's output spec was honored; size() is then 0.
        virtual const CompactionBuiltOutput *built_output() const noexcept { return nullptr; }
    };

    class CompactionOutputSubmission
    {
    public:
        virtual ~CompactionOutputSubmission() = default;
        virtual Status Finish(std::vector<CompactionOutputBlock> *output) noexcept = 0;
    };

    class CompactionBlockSession
    {
    public:
        virtual ~CompactionBlockSession() = default;
        virtual Status Add(const std::vector<CompactionBlockKey> &keys,
                           const std::vector<CompactionBlockInput> &input) noexcept = 0;
        virtual Status Finish() noexcept = 0;
        virtual bool Take(const CompactionBlockKey &key, std::string *output) noexcept = 0;
    };

    class CompactionBlockTransform
    {
    public:
        static constexpr std::size_t max_blocks = 256;
        // Output submissions batch many small blocks (e.g. 4 KiB) per GPU dispatch.
        static constexpr std::size_t max_output_blocks = 4096;
        static constexpr std::size_t max_input_bytes = 16ULL << 20;
        static constexpr std::size_t max_output_bytes = 64ULL << 20;
        static constexpr std::size_t max_block_bytes = 2ULL << 20;
        virtual ~CompactionBlockTransform() = default;
        virtual Status Transform(const std::vector<CompactionBlockInput> &input,
                                 std::vector<std::string> *output) noexcept = 0;
        virtual Status BuildOutput(const std::vector<Slice> &input,
                                   std::vector<CompactionOutputBlock> *output) noexcept = 0;
        virtual Status SubmitOutput(
            std::vector<std::string> input,
            std::unique_ptr<CompactionOutputSubmission> *submission) noexcept = 0;
        virtual std::shared_ptr<CompactionBlockSession> CreateSession() = 0;

        // Merged-input offload. Returning NotSupported (or any non-Corruption
        // error) makes the compaction use RocksDB's own input iterators;
        // Corruption fails the compaction like a CPU checksum mismatch would.
        virtual bool SupportsMergedInput() const noexcept { return false; }
        virtual Status MergeCompactionInput(const CompactionMergeRequest & /*request*/,
                                            std::unique_ptr<CompactionMergedInput> * /*output*/) noexcept
        {
            return Status::NotSupported("merged compaction input is not implemented by this transform");
        }
        virtual void RecordMergeFallback(const Status & /*reason*/) noexcept {}
        // Sets the bits of a FastLocalBloom filter (RocksDB's format_version 5+ Bloom filter):
        // for every hash, FastLocalBloomImpl::AddHash(lower 32 bits, upper 32 bits, len_bytes,
        // num_probes, data). data holds len_bytes zeroed bytes. Any non-OK status makes the
        // caller set the bits on the CPU.
        virtual Status BuildBloomFilter(const std::uint64_t * /*hashes*/, std::size_t /*count*/,
                                        std::uint32_t /*len_bytes*/, int /*num_probes*/, char * /*data*/) noexcept
        {
            return Status::NotSupported("filter building is not implemented by this transform");
        }
    };
}