#pragma once

#include <rocksdb/compaction_block_transform.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lsm_lab
{
    using MergedEntry = rocksdb::CompactionMergedEntry;

    class PackedMergedInput final : public rocksdb::CompactionMergedInput
    {
    public:
        PackedMergedInput(std::shared_ptr<const char> data, std::vector<MergedEntry> entries,
                          rocksdb::CompactionMergeDropped dropped = {})
            : data_(std::move(data)), dropped_(dropped)
        {
            auto owned = std::make_shared<std::vector<MergedEntry>>(std::move(entries));
            count_ = owned->size();
            entries_ = std::shared_ptr<const MergedEntry>(owned, owned->data());
        }
        // Entries already in a buffer owned elsewhere (e.g. pinned memory).
        PackedMergedInput(std::shared_ptr<const char> data, std::shared_ptr<const MergedEntry> entries,
                          std::size_t count, rocksdb::CompactionMergeDropped dropped = {})
            : data_(std::move(data)), entries_(std::move(entries)), count_(count), dropped_(dropped) {}

        std::size_t size() const noexcept override { return count_; }
        rocksdb::Slice key(std::size_t index) const noexcept override
        {
            const auto &entry = entries_.get()[index];
            return {data_.get() + entry.offset, entry.key_bytes};
        }
        rocksdb::Slice value(std::size_t index) const noexcept override
        {
            const auto &entry = entries_.get()[index];
            return {data_.get() + entry.offset + entry.key_bytes, entry.value_bytes};
        }
        const char *packed_data() const noexcept override { return data_.get(); }
        const MergedEntry *packed_entries() const noexcept override { return entries_.get(); }
        rocksdb::CompactionMergeDropped dropped() const noexcept override { return dropped_; }

    private:
        std::shared_ptr<const char> data_;
        std::shared_ptr<const MergedEntry> entries_;
        std::size_t count_ = 0;
        rocksdb::CompactionMergeDropped dropped_;
    };

    // Finished data blocks (Stage 3 output offload). owner keeps the buffers
    // referenced by output alive.
    class BuiltMergedInput final : public rocksdb::CompactionMergedInput
    {
    public:
        BuiltMergedInput(std::shared_ptr<const void> owner, rocksdb::CompactionBuiltOutput output,
                         rocksdb::CompactionMergeDropped dropped)
            : owner_(std::move(owner)), output_(output), dropped_(dropped) {}
        std::size_t size() const noexcept override { return 0; }
        rocksdb::Slice key(std::size_t) const noexcept override { return {}; }
        rocksdb::Slice value(std::size_t) const noexcept override { return {}; }
        rocksdb::CompactionMergeDropped dropped() const noexcept override { return dropped_; }
        const rocksdb::CompactionBuiltOutput *built_output() const noexcept override { return &output_; }

    private:
        std::shared_ptr<const void> owner_;
        rocksdb::CompactionBuiltOutput output_;
        rocksdb::CompactionMergeDropped dropped_;
    };

    struct MergeRunStats
    {
        std::uint64_t blocks = 0;
        std::uint64_t input_bytes = 0;
        std::uint64_t decoded_bytes = 0;
        std::uint64_t entries = 0;
        std::uint64_t dropped = 0;
        std::uint64_t read_ns = 0;
        std::uint64_t device_ns = 0;
        // Stage 3: finished data blocks returned (0 when the entry stream was returned).
        std::uint64_t built_blocks = 0;
        std::uint64_t built_bytes = 0;
        // Earlier-design emulation: blocks left to the table builder to compress, and input bytes
        // decompressed on the CPU.
        std::uint64_t host_compressed_blocks = 0;
        std::uint64_t cpu_decoded_bytes = 0;
    };

    // Total bytes of all listed blocks including their 5-byte trailers.
    std::uint64_t merge_serialized_bytes(const rocksdb::CompactionMergeRequest &request);

    // Reads every listed block (contents plus 5-byte trailer) into destination,
    // which must hold merge_serialized_bytes(request) bytes, in request order.
    // offsets[i] is the start of block i in destination.
    rocksdb::Status read_merge_blocks(const rocksdb::CompactionMergeRequest &request, char *destination,
                                      std::vector<std::uint64_t> *offsets);

    // Earlier-design emulation (LSM_GPU_CPU_DECOMPRESS): the CPU verifies and decompresses the blocks
    // read by read_merge_blocks. decoded_merge_layout places block i's uncompressed contents at
    // (*decoded_offsets)[i] of a decoded stream of *decoded_total bytes; decode_merge_blocks decodes
    // blocks [first, last) into destination (which holds the stream from decoded_offsets[first]) using
    // LSM_MERGE_READ_THREADS threads.
    rocksdb::Status decoded_merge_layout(const rocksdb::CompactionMergeRequest &request, const char *serialized,
                                         const std::vector<std::uint64_t> &offsets,
                                         std::vector<std::uint64_t> *decoded_offsets, std::uint64_t *decoded_total);
    rocksdb::Status decode_merge_blocks(const rocksdb::CompactionMergeRequest &request, const char *serialized,
                                        const std::vector<std::uint64_t> &offsets,
                                        const std::vector<std::uint64_t> &decoded_offsets, std::uint64_t decoded_total,
                                        std::size_t first, std::size_t last, char *destination);

    // Reference block builder: encodes the kept entries (internal-key order) as
    // finished data blocks per spec. Sets *built = false and leaves *output
    // untouched when an entry is not eligible (non-Put or user key > 240 bytes).
    rocksdb::Status build_output_blocks(const char *packed, const MergedEntry *entries, std::size_t count,
                                       const rocksdb::CompactionOutputSpec &spec,
                                       rocksdb::CompactionMergeDropped dropped,
                                       std::unique_ptr<rocksdb::CompactionMergedInput> *output, bool *built,
                                       MergeRunStats *stats);

    // Reference implementation used for CPU backends and as the correctness
    // oracle for accelerator implementations.
    rocksdb::Status execute_cpu_merge(const rocksdb::CompactionMergeRequest &request,
                                      std::unique_ptr<rocksdb::CompactionMergedInput> *output, MergeRunStats *stats);
}
