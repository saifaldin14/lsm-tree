#pragma once
#include <cstdlib>

// Smallest data block offloaded to the GPU output path. The default (32 KiB)
// keeps small blocks on the CPU; set LSM_GPU_OUTPUT_MIN_BLOCK_BYTES to batch
// small blocks (submissions are sized by bytes, up to max_output_blocks).
static std::size_t LsmGpuOutputMinBlockBytes()
{
    static const std::size_t value = [] {
        const char *configured = std::getenv("LSM_GPU_OUTPUT_MIN_BLOCK_BYTES");
        if (configured == nullptr || *configured == '\0')
            return std::size_t{32} << 10;
        return static_cast<std::size_t>(std::strtoull(configured, nullptr, 10));
    }();
    return value;
}

bool BlockBasedTableBuilder::QueueCompactionOutput(
    const Slice &uncompressed, const Slice &last_key, const Slice *first_key)
{
    constexpr std::size_t pipeline_bytes = 8ULL << 20;
    Rep *r = rep_.get();
    const auto &transform = r->table_options.compaction_block_transform;
    const bool eligible =
        std::getenv("LSM_DISABLE_GPU_OUTPUT") == nullptr && transform &&
        r->reason == TableFileCreationReason::kCompaction &&
        !r->IsParallelCompressionActive() && uncompressed.size() >= LsmGpuOutputMinBlockBytes() &&
        uncompressed.size() <= CompactionBlockTransform::max_block_bytes;
    if (!eligible)
    {
        FlushCompactionOutput();
        return false;
    }
    if (!compaction_output_.empty() &&
        (compaction_output_.size() == CompactionBlockTransform::max_output_blocks ||
         uncompressed.size() > CompactionBlockTransform::max_output_bytes - compaction_output_bytes_))
        SubmitCompactionOutput();
    PendingCompactionOutput pending;
    pending.uncompressed.assign(uncompressed.data(), uncompressed.size());
    pending.last_key.assign(last_key.data(), last_key.size());
    if (first_key)
        pending.first_key = first_key->ToString();
    compaction_output_bytes_ += pending.uncompressed.size();
    compaction_output_.push_back(std::move(pending));
    if (submitted_compaction_output_.empty() &&
        (compaction_output_.size() == CompactionBlockTransform::max_output_blocks ||
         compaction_output_bytes_ >= pipeline_bytes))
        SubmitCompactionOutput();
    return true;
}

void BlockBasedTableBuilder::SubmitCompactionOutput()
{
    if (compaction_output_.empty() || !ok())
        return;
    if (submitted_compaction_output_.size() == 2)
        DrainCompactionOutput(false);
    if (!ok())
        return;
    Rep *r = rep_.get();
    std::vector<std::string> input;
    input.reserve(compaction_output_.size());
    // The submission takes ownership and returns each block in
    // CompactionOutputBlock::uncompressed; no second copy is made.
    for (auto &pending : compaction_output_)
        input.push_back(std::move(pending.uncompressed));
    SubmittedCompactionOutput submitted;
    submitted.pending = std::move(compaction_output_);
    r->SetStatus(r->table_options.compaction_block_transform->SubmitOutput(
        std::move(input), &submitted.submission));
    compaction_output_.clear();
    compaction_output_bytes_ = 0;
    if (ok())
        submitted_compaction_output_.push_back(std::move(submitted));
}

void BlockBasedTableBuilder::DrainCompactionOutput(bool all)
{
    Rep *r = rep_.get();
    while (!submitted_compaction_output_.empty() && ok())
    {
        auto &submitted = submitted_compaction_output_.front();
        std::vector<CompactionOutputBlock> output;
        r->SetStatus(submitted.submission->Finish(&output));
        if (ok() && output.size() != submitted.pending.size())
            r->SetStatus(Status::Corruption("GPU output block count mismatch"));
        for (std::size_t index = 0; index < output.size() && ok(); ++index)
        {
            auto &pending = submitted.pending[index];
            auto &built = output[index];
            bool skip_delta_encoding = false;
            if (built.uncompressed.empty())
            {
                r->SetStatus(Status::Corruption("GPU output submission did not return the uncompressed block"));
                break;
            }
            const Slice uncompressed(built.uncompressed);
            const Slice contents(built.contents);
            WriteMaybeCompressedBlock(
                contents, built.compressed ? kLZ4Compression : kNoCompression,
                &r->pending_handle, BlockType::kData, &uncompressed,
                &skip_delta_encoding, &built.checksum);
            if (!ok())
                break;
            Slice first_key;
            const Slice *first_key_pointer = nullptr;
            if (pending.first_key)
            {
                first_key = *pending.first_key;
                first_key_pointer = &first_key;
            }
            r->index_builder->AddIndexEntry(
                pending.last_key, first_key_pointer, r->pending_handle,
                &r->index_separator_scratch, skip_delta_encoding);
            r->props.data_size = r->get_offset();
            r->props.uncompressed_data_size += built.uncompressed.size();
        }
        submitted_compaction_output_.pop_front();
        if (!all)
            break;
    }
}

void BlockBasedTableBuilder::FlushCompactionOutput()
{
    SubmitCompactionOutput();
    DrainCompactionOutput(true);
    compaction_output_.clear();
    compaction_output_bytes_ = 0;
}

// Stage 3: adds one finished data block (contents, masked checksum without the
// context modifier, keys, per-key statistics, filter hashes). Its index entry
// is emitted once the next block's first key is known, or by Finish().
Status BlockBasedTableBuilder::LsmAddBuiltBlock(const CompactionBuiltBlock &block, const char *contents,
                                                const Slice &first_key, const Slice &last_key,
                                                const uint64_t *hashes)
{
    Rep *r = rep_.get();
    if (!ok())
        return status();
    if (r->state != Rep::State::kUnbuffered || r->IsParallelCompressionActive() || !r->data_block.empty() ||
        !compaction_output_.empty() || !submitted_compaction_output_.empty() ||
        r->table_properties_collectors.size() != 1 || r->warm_cache_config.enabled)
        return Status::NotSupported("built data blocks require an idle unbuffered table builder");
    if (r->filter_builder != nullptr && block.hash_count > 0 &&
        !r->filter_builder->LsmAddKeyHashes(hashes, block.hash_count))
        return Status::NotSupported("filter builder does not accept precomputed key hashes");
    LsmFinishPendingIndex(&first_key);
    r->props.num_entries += block.entries;
    r->props.raw_key_size += block.raw_key_bytes;
    r->props.raw_value_size += block.raw_value_bytes;
    r->props.key_smallest_seqno = std::min(r->props.key_smallest_seqno, block.smallest_seqno);
    r->props.key_largest_seqno = std::max(r->props.key_largest_seqno, block.largest_seqno);
    ++r->props.num_data_blocks;
    if (r->filter_builder != nullptr)
        r->filter_builder->OnDataBlockFinalized(r->props.num_data_blocks);
    // Only the size of the uncompressed form is used (warm cache is off).
    const Slice uncompressed(contents, block.uncompressed_bytes);
    const Slice stored(contents, block.contents_bytes);
    bool skip_delta_encoding = false;
    WriteMaybeCompressedBlock(stored, block.compressed ? kLZ4Compression : kNoCompression, &r->pending_handle,
                              BlockType::kData, &uncompressed, &skip_delta_encoding, &block.checksum);
    if (!ok())
        return status();
    r->props.data_size = r->get_offset();
    r->props.uncompressed_data_size += block.uncompressed_bytes;
    r->last_ikey.assign(last_key.data(), last_key.size());
    lsm_pending_index_key_.assign(last_key.data(), last_key.size());
    lsm_pending_index_ = true;
    return status();
}

void BlockBasedTableBuilder::LsmFinishPendingIndex(const Slice *first_key_in_next_block)
{
    if (!lsm_pending_index_ || !ok())
        return;
    Rep *r = rep_.get();
    r->index_builder->AddIndexEntry(lsm_pending_index_key_, first_key_in_next_block, r->pending_handle,
                                    &r->index_separator_scratch, /*skip_delta_encoding=*/false);
    lsm_pending_index_ = false;
}
