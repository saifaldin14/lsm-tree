#pragma once
#include <cstdlib>

bool BlockBasedTableBuilder::QueueCompactionOutput(
    const Slice &uncompressed, const Slice &last_key, const Slice *first_key)
{
    constexpr std::size_t pipeline_blocks = 64;
    constexpr std::size_t pipeline_bytes = 8ULL << 20;
    Rep *r = rep_.get();
    const auto &transform = r->table_options.compaction_block_transform;
    const bool eligible =
        std::getenv("LSM_DISABLE_GPU_OUTPUT") == nullptr && transform &&
        r->reason == TableFileCreationReason::kCompaction &&
        !r->IsParallelCompressionActive() && uncompressed.size() >= (32ULL << 10) &&
        uncompressed.size() <= CompactionBlockTransform::max_block_bytes;
    if (!eligible)
    {
        FlushCompactionOutput();
        return false;
    }
    if (!compaction_output_.empty() &&
        (compaction_output_.size() == CompactionBlockTransform::max_blocks ||
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
        (compaction_output_.size() == pipeline_blocks ||
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
    for (const auto &pending : compaction_output_)
        input.push_back(pending.uncompressed);
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
            const Slice uncompressed(pending.uncompressed);
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
            r->props.uncompressed_data_size += pending.uncompressed.size();
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
