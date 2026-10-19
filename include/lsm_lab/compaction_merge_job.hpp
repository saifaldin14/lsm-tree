#pragma once

// Included inside ROCKSDB_NAMESPACE in db/compaction/compaction_job.cc.
// Offers a merged input stream for eligible subcompactions. The stream must
// equal what CompactionMergingIterator would produce (every entry, in
// internal-key order); CompactionIterator and the output path are unchanged.
//
// Stage 3: when the job and table options allow it, the accelerator may
// instead return finished data blocks (CompactionBuiltOutput). They are added
// block by block to RocksDB's own table builder (LsmProcessBuiltOutput), which
// still writes the index, filter, properties and footer and decides file cuts.

// Facts known only to CompactionJob that decide whether finished blocks may
// replace CompactionIterator and per-key table building.
struct LsmBuildContext
{
    bool allowed = false;
    bool zero_sequences = false;
    // Sequence numbers <= zero_limit become 0 at the bottommost level
    // (min of the earliest snapshot and preserve_seqno_after).
    SequenceNumber zero_limit = 0;
    // CompactionIterator's earliest_snapshot_ (kMaxSequenceNumber without snapshots).
    SequenceNumber earliest_snapshot = kMaxSequenceNumber;
};

// User-key ranges of every file below the output level, merged into sorted
// disjoint ranges; a key outside all of them cannot exist beyond the output
// level (Compaction::KeyNotExistsBeyondOutputLevel).
static std::vector<std::pair<std::string, std::string>> LsmBeyondOutputRanges(const Compaction *compaction)
{
    std::vector<std::pair<std::string, std::string>> ranges;
    const auto *storage = compaction->input_version()->storage_info();
    for (int level = compaction->output_level() + 1; level < compaction->number_levels(); ++level)
    {
        for (const auto *file : storage->LevelFiles(level))
            ranges.emplace_back(file->smallest.user_key().ToString(), file->largest.user_key().ToString());
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<std::string, std::string>> merged;
    for (auto &range : ranges)
    {
        if (!merged.empty() && range.first <= merged.back().second)
        {
            if (range.second > merged.back().second)
                merged.back().second = std::move(range.second);
        }
        else
        {
            merged.push_back(std::move(range));
        }
    }
    return merged;
}

class LsmMergedInputIterator final : public InternalIterator
{
public:
    LsmMergedInputIterator(std::unique_ptr<CompactionMergedInput> input, const InternalKeyComparator *icmp)
        : input_(std::move(input)), icmp_(icmp), size_(input_->size()), index_(size_),
          data_(input_->packed_data()), entries_(input_->packed_entries()) {}

    bool Valid() const override { return index_ < size_; }
    void SeekToFirst() override { index_ = 0; }
    void SeekToLast() override { index_ = size_ == 0 ? 0 : size_ - 1; }
    void Seek(const Slice &target) override
    {
        std::size_t low = 0, high = size_;
        while (low < high)
        {
            const auto middle = low + (high - low) / 2;
            if (icmp_->Compare(input_->key(middle), target) < 0)
                low = middle + 1;
            else
                high = middle;
        }
        index_ = low;
    }
    void SeekForPrev(const Slice &target) override
    {
        Seek(target);
        if (index_ < size_ && icmp_->Compare(input_->key(index_), target) == 0)
            return;
        index_ = index_ == 0 ? size_ : index_ - 1;
    }
    void Next() override { ++index_; }
    void Prev() override { index_ = index_ == 0 ? size_ : index_ - 1; }
    Slice key() const override
    {
        if (entries_ != nullptr)
            return Slice(data_ + entries_[index_].offset, entries_[index_].key_bytes);
        return input_->key(index_);
    }
    Slice value() const override
    {
        if (entries_ != nullptr)
        {
            const auto &entry = entries_[index_];
            return Slice(data_ + entry.offset + entry.key_bytes, entry.value_bytes);
        }
        return input_->value(index_);
    }
    CompactionMergeDropped dropped() const { return input_->dropped(); }
    const CompactionBuiltOutput *built_output() const { return input_->built_output(); }
    Status status() const override { return Status::OK(); }
    bool IsKeyPinned() const override { return true; }
    bool IsValuePinned() const override { return true; }

private:
    std::unique_ptr<CompactionMergedInput> input_;
    const InternalKeyComparator *icmp_;
    std::size_t size_;
    std::size_t index_;
    const char *data_;
    const CompactionMergedEntry *entries_;
};

// Returns OK with *output unset to keep RocksDB's own input iterators. Only a
// Corruption from the offload (e.g. checksum mismatch) is returned as an error.
// snapshots (ascending) enables dropping hidden versions on the accelerator;
// pass nullptr when a SnapshotChecker is in use.
// Whether finished data blocks may replace per-key output building: every
// table, filter, compression and job option must match what the built-block
// encoder reproduces exactly.
static bool LsmBuiltOutputEligible(const Compaction *compaction, ColumnFamilyData *cfd,
                                   const BlockBasedTableOptions &table, const LsmBuildContext *build,
                                   bool drop_hidden)
{
    if (build == nullptr || !build->allowed || !drop_hidden || std::getenv("LSM_DISABLE_GPU_BUILD") != nullptr)
        return false;
    const auto &mutable_options = compaction->mutable_cf_options();
    const auto &ioptions = cfd->ioptions();
    const auto &compression = compaction->output_compression_opts();
    if (mutable_options.paranoid_file_checks || ioptions.cf_allow_ingest_behind || ioptions.allow_ingest_behind ||
        ioptions.sst_partitioner_factory ||
        !ioptions.table_properties_collector_factories.empty() || !cfd->internal_tbl_prop_coll_factories()->empty() ||
        cfd->user_comparator()->timestamp_size() != 0)
        return false;
    if (compaction->output_compression() != kLZ4Compression && compaction->output_compression() != kZSTD &&
        compaction->output_compression() != kNoCompression)
        return false;
    if (compression.max_dict_bytes != 0 || compression.zstd_max_train_bytes != 0 || compression.parallel_threads > 1)
        return false;
    if (table.format_version < 2 || table.checksum != kCRC32c || table.index_type != BlockBasedTableOptions::kBinarySearch ||
        table.data_block_index_type != BlockBasedTableOptions::kDataBlockBinarySearch || table.partition_filters ||
        !table.use_delta_encoding || table.block_restart_interval < 1 || table.block_align ||
        table.super_block_alignment_size != 0 || table.separate_key_value_in_data_block ||
        table.uniform_cv_threshold >= 0 || table.block_size == 0 || table.block_size > (1U << 20) ||
        table.block_size_deviation < 0 || table.block_size_deviation > 100 ||
        table.prepopulate_block_cache == BlockBasedTableOptions::PrepopulateBlockCache::kFlushAndCompaction)
        return false;
    if (!table.flush_block_policy_factory ||
        std::string(table.flush_block_policy_factory->Name()) != "FlushBlockBySizePolicyFactory")
        return false;
    if (table.filter_policy)
    {
        // Built-in Bloom/Ribbon builders hash user keys with Hash64 (XXPH3).
        if (dynamic_cast<const BloomLikeFilterPolicy *>(table.filter_policy.get()) == nullptr ||
            table.format_version < 5 || !table.whole_key_filtering || mutable_options.prefix_extractor)
            return false;
    }
    return true;
}

static Status LsmMaybeCreateMergedInput(const Compaction *compaction, ColumnFamilyData *cfd,
                                        const std::optional<const Slice> &start,
                                        const std::optional<const Slice> &end, const ReadOptions &read_options,
                                        const std::vector<SequenceNumber> *snapshots,
                                        const LsmBuildContext *build,
                                        std::unique_ptr<InternalIterator> *output)
{
    output->reset();
    const auto &mutable_options = compaction->mutable_cf_options();
    TableFactory *factory = mutable_options.table_factory.get();
    if (factory == nullptr || std::string(factory->Name()) != TableFactory::kBlockBasedTableName())
        return Status::OK();
    const auto *table_options = factory->GetOptions<BlockBasedTableOptions>();
    if (table_options == nullptr || !table_options->compaction_block_transform ||
        !table_options->compaction_block_transform->SupportsMergedInput() ||
        std::getenv("LSM_DISABLE_GPU_MERGE") != nullptr)
        return Status::OK();
    const auto &transform = table_options->compaction_block_transform;
    const auto fallback = [&](const Status &reason) {
        transform->RecordMergeFallback(reason);
        return Status::OK();
    };
    const auto &ioptions = cfd->ioptions();
    if (ioptions.merge_operator || ioptions.compaction_filter != nullptr || ioptions.compaction_filter_factory)
        return fallback(Status::NotSupported("merge operator or compaction filter configured"));
    if (cfd->user_comparator() != BytewiseComparator())
        return fallback(Status::NotSupported("non-bytewise comparator"));
    if (compaction->DoesInputReferenceBlobFiles() || mutable_options.enable_blob_files)
        return fallback(Status::NotSupported("blob files"));
    if (compaction->SupportsPerKeyPlacement())
        return fallback(Status::NotSupported("per-key placement"));

    CompactionMergeRequest request;
    if (start.has_value())
    {
        request.has_start = true;
        request.start_user_key = start->ToString();
    }
    if (end.has_value())
    {
        request.has_end = true;
        request.end_user_key = end->ToString();
    }
    if (snapshots != nullptr && std::getenv("LSM_GPU_MERGE_KEEP_HIDDEN") == nullptr)
    {
        request.drop_hidden = true;
        request.snapshots.assign(snapshots->begin(), snapshots->end());
        std::sort(request.snapshots.begin(), request.snapshots.end());
    }
    if (LsmBuiltOutputEligible(compaction, cfd, *table_options, build, request.drop_hidden))
    {
        auto &spec = request.output;
        spec.enabled = true;
        spec.block_bytes = static_cast<std::uint32_t>(table_options->block_size);
        spec.block_size_deviation = static_cast<std::uint32_t>(table_options->block_size_deviation);
        spec.restart_interval = static_cast<std::uint32_t>(table_options->block_restart_interval);
        spec.max_compressed_bytes_per_kb = static_cast<std::uint32_t>(
            std::min(1023, std::max(0, compaction->output_compression_opts().max_compressed_bytes_per_kb)));
        spec.lz4 = compaction->output_compression() == kLZ4Compression;
        spec.zstd = compaction->output_compression() == kZSTD;
        spec.zero_sequences = build->zero_sequences;
        spec.earliest_snapshot = build->zero_limit;
        spec.filter_hashes = table_options->filter_policy != nullptr;
        spec.bottommost = compaction->bottommost_level();
        spec.earliest_snapshot_seq = build->earliest_snapshot;
        if (!spec.bottommost && compaction->output_level() != 0 &&
            cfd->ioptions().compaction_style == kCompactionStyleLevel)
        {
            spec.check_beyond_ranges = true;
            spec.beyond_ranges = LsmBeyondOutputRanges(compaction);
        }
    }
    const Slice start_slice(request.start_user_key);
    const Slice end_slice(request.end_user_key);
    for (std::size_t level = 0; level < compaction->num_input_levels(); ++level)
    {
        for (std::size_t index = 0; index < compaction->num_input_files(level); ++index)
        {
            const FileMetaData *file = compaction->input(level, index);
            if (file->num_range_deletions != 0)
                return fallback(Status::NotSupported("range tombstones"));
            CompactionMergeFile merge_file;
            merge_file.level = compaction->level(level);
            merge_file.file_number = file->fd.GetNumber();
            Status status = cfd->table_cache()->LsmCollectDataBlocks(
                read_options, cfd->internal_comparator(), *file, mutable_options,
                request.has_start ? &start_slice : nullptr, request.has_end ? &end_slice : nullptr, &merge_file);
            if (!status.ok())
                return status.IsCorruption() ? status : fallback(status);
            if (!merge_file.blocks.empty())
                request.files.push_back(std::move(merge_file));
        }
    }
    std::unique_ptr<CompactionMergedInput> merged;
    Status status = transform->MergeCompactionInput(request, &merged);
    if (!status.ok())
        return status.IsCorruption() ? status : fallback(status);
    if (!merged)
        return fallback(Status::Aborted("merged-input offload returned no stream"));
    *output = std::make_unique<LsmMergedInputIterator>(std::move(merged), &cfd->internal_comparator());
    return Status::OK();
}

// Entries dropped by the accelerator never reach CompactionIterator; add them
// back so input-record verification and statistics match the CPU path. With
// built output no entry reaches CompactionIterator, so the kept ones count too.
static void LsmAccountMergedInputDrops(InternalIterator *merged, CompactionJobStats *stats)
{
    auto *iterator = static_cast<LsmMergedInputIterator *>(merged);
    const auto dropped = iterator->dropped();
    stats->num_input_records += dropped.entries;
    stats->num_input_deletion_records += dropped.deletions;
    stats->total_input_raw_key_bytes += dropped.key_bytes;
    stats->total_input_raw_value_bytes += dropped.value_bytes;
    stats->num_records_replaced += dropped.entries;
    if (const auto *built = iterator->built_output())
    {
        stats->num_input_records += built->entries;
        for (std::size_t index = 0; index < built->block_count; ++index)
        {
            stats->total_input_raw_key_bytes += built->blocks[index].raw_key_bytes;
            stats->total_input_raw_value_bytes += built->blocks[index].raw_value_bytes;
        }
    }
}

static const CompactionBuiltOutput *LsmBuiltOutputOf(InternalIterator *merged)
{
    return merged == nullptr ? nullptr : static_cast<LsmMergedInputIterator *>(merged)->built_output();
}

// Replaces ProcessKeyValue for built output: adds each finished block to the
// current output file, letting CompactionOutputs decide file cuts at block
// boundaries that do not split a user key.
static Status LsmProcessBuiltOutput(const CompactionBuiltOutput &built, SubcompactionState *sub_compact,
                                    ColumnFamilyData *cfd, const CompactionFileOpenFunc &open_file_func,
                                    const CompactionFileCloseFunc &close_file_func,
                                    const std::atomic<int> &compaction_aborted)
{
    CompactionOutputs &outputs = *sub_compact->Outputs(false);
    ParsedInternalKey previous;
    Slice previous_last;
    Status status;
    for (std::size_t index = 0; index < built.block_count && status.ok(); ++index)
    {
        if ((index & 63) == 0)
        {
            if (compaction_aborted.load(std::memory_order_acquire) > 0)
                return Status::Incomplete(Status::SubCode::kCompactionAborted);
            if (cfd->IsDropped())
                break;
        }
        const auto &block = built.blocks[index];
        const Slice first(built.keys + block.keys_offset, block.first_key_bytes);
        const Slice last(first.data() + block.first_key_bytes, block.last_key_bytes);
        const bool can_cut = index > 0 && ExtractUserKey(first) != ExtractUserKey(previous_last);
        status = outputs.LsmAddBuiltBlock(block, built.contents + block.contents_offset, first, last,
                                          built.hashes + block.hash_offset, can_cut, open_file_func,
                                          close_file_func, previous);
        previous_last = last;
        if (status.ok())
            status = ParseInternalKey(last, &previous, /*log_err_key=*/false);
    }
    return status;
}
