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
        // Flushes use the same path (LSM_DISABLE_GPU_FLUSH keeps their compression on the CPU).
        (r->reason == TableFileCreationReason::kCompaction ||
         (r->reason == TableFileCreationReason::kFlush && std::getenv("LSM_DISABLE_GPU_FLUSH") == nullptr)) &&
        !r->IsParallelCompressionActive() && uncompressed.size() >= LsmGpuOutputMinBlockBytes() &&
        // The output-only GPU path produces LZ4 blocks.
        r->basic_compressor && r->basic_compressor->GetPreferredCompressionType() == kLZ4Compression &&
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
    r->props.num_deletions += block.deletions;
    r->props.raw_key_size += block.raw_key_bytes;
    r->props.raw_value_size += block.raw_value_bytes;
    r->props.key_smallest_seqno = std::min(r->props.key_smallest_seqno, block.smallest_seqno);
    r->props.key_largest_seqno = std::max(r->props.key_largest_seqno, block.largest_seqno);
    ++r->props.num_data_blocks;
    if (r->filter_builder != nullptr)
        r->filter_builder->OnDataBlockFinalized(r->props.num_data_blocks);
    // Only the size of the uncompressed form is used (warm cache is off).
    const Slice uncompressed(contents, block.uncompressed_bytes);
    bool skip_delta_encoding = false;
    if (block.flags & CompactionBuiltBlock::kCompressOnHost)
    {
        // RocksDB's own compression, max_compressed_bytes_per_kb rule and checksum (also sets the
        // data size properties).
        WriteBlock(uncompressed, &r->pending_handle, BlockType::kData, &skip_delta_encoding);
        if (!ok())
            return status();
    }
    else
    {
        const Slice stored(contents, block.contents_bytes);
        WriteMaybeCompressedBlock(stored, static_cast<CompressionType>(block.compressed), &r->pending_handle,
                                  BlockType::kData, &uncompressed, &skip_delta_encoding, &block.checksum);
        if (!ok())
            return status();
        r->props.data_size = r->get_offset();
        r->props.uncompressed_data_size += block.uncompressed_bytes;
    }
    r->last_ikey.assign(last_key.data(), last_key.size());
    lsm_pending_index_key_.assign(last_key.data(), last_key.size());
    lsm_pending_index_ = true;
    return status();
}

// Adds count finished data blocks that lie back to back in contents, each followed by a
// kBuiltTrailerGap-byte gap: writes every block's trailer (type byte and checksum with the
// format_version 6 context modifier for its file offset) into its gap and appends the whole
// run with one write. The file bytes, index entries, filter and properties are the same as
// adding the blocks one by one with LsmAddBuiltBlock.
Status BlockBasedTableBuilder::LsmAddBuiltRun(const CompactionBuiltBlock *blocks, size_t count, char *contents,
                                              const char *keys, const uint64_t *hashes)
{
    Rep *r = rep_.get();
    if (!ok())
        return status();
    if (count == 0)
        return Status::OK();
    if (r->state != Rep::State::kUnbuffered || r->IsParallelCompressionActive() || !r->data_block.empty() ||
        !compaction_output_.empty() || !submitted_compaction_output_.empty() ||
        r->table_properties_collectors.size() != 1 || r->warm_cache_config.enabled ||
        r->table_options.super_block_alignment_size != 0 || r->table_options.block_align)
        return Status::NotSupported("built data blocks require an idle unbuffered table builder");
    IOOptions io_options;
    IOStatus io_s = WritableFileWriter::PrepareIOOptions(r->write_options, io_options);
    if (!io_s.ok())
    {
        r->SetIOStatus(io_s);
        return io_s;
    }
    const uint64_t start = r->get_offset();
    uint64_t offset = start;
    for (size_t index = 0; index < count; ++index)
    {
        const auto &block = blocks[index];
        const Slice first_key(keys + block.keys_offset, block.first_key_bytes);
        const Slice last_key(first_key.data() + block.first_key_bytes, block.last_key_bytes);
        // The file's key hashes are contiguous across its blocks; the filter builder keeps a
        // pointer to them (and builds the filter on the GPU) instead of copying them.
        if (r->filter_builder != nullptr && block.hash_count > 0 &&
            !r->filter_builder->LsmAddExternalKeyHashes(hashes + block.hash_offset, block.hash_count,
                                                        r->table_options.compaction_block_transform.get()) &&
            !r->filter_builder->LsmAddKeyHashes(hashes + block.hash_offset, block.hash_count))
            return Status::NotSupported("filter builder does not accept precomputed key hashes");
        LsmFinishPendingIndex(&first_key);
        r->props.num_entries += block.entries;
        r->props.num_deletions += block.deletions;
        r->props.raw_key_size += block.raw_key_bytes;
        r->props.raw_value_size += block.raw_value_bytes;
        r->props.key_smallest_seqno = std::min(r->props.key_smallest_seqno, block.smallest_seqno);
        r->props.key_largest_seqno = std::max(r->props.key_largest_seqno, block.largest_seqno);
        ++r->props.num_data_blocks;
        if (r->filter_builder != nullptr)
            r->filter_builder->OnDataBlockFinalized(r->props.num_data_blocks);
        r->pending_handle.set_offset(offset);
        r->pending_handle.set_size(block.contents_bytes);
        char *trailer = contents + block.contents_offset + block.contents_bytes;
        trailer[0] = static_cast<char>(block.compressed);
        EncodeFixed32(trailer + 1, block.checksum + ChecksumModifierForContext(r->base_context_checksum, offset));
        r->compression_types_used.Add(static_cast<CompressionType>(block.compressed));
        r->pre_compression_size += block.uncompressed_bytes + kBlockTrailerSize;
        r->props.uncompressed_data_size += block.uncompressed_bytes;
        offset += block.contents_bytes + kBlockTrailerSize;
        lsm_pending_index_key_.assign(last_key.data(), last_key.size());
        lsm_pending_index_ = true;
        if (index + 1 == count)
            r->last_ikey.assign(last_key.data(), last_key.size());
    }
    io_s = r->file->Append(io_options, Slice(contents + blocks[0].contents_offset, offset - start));
    if (!io_s.ok())
    {
        r->SetIOStatus(io_s);
        return io_s;
    }
    r->set_offset(offset);
    r->props.data_size = offset;
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
