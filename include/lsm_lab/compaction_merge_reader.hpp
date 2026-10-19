#pragma once

// Included inside ROCKSDB_NAMESPACE in table/block_based/block_based_table_reader.cc.
// Lists the data blocks of one table for the merged-input compaction offload.
// Blocks entirely outside [start_user_key, end_user_key) are skipped; the
// offload clips individual entries itself.

Status BlockBasedTable::LsmCollectDataBlocks(const ReadOptions &read_options, const Slice *start_user_key,
                                             const Slice *end_user_key, CompactionMergeFile *file)
{
    const Rep *rep = rep_;
    if (rep->footer.format_version() != 6 || rep->footer.checksum_type() != kCRC32c)
        return Status::NotSupported("merged input requires format_version 6 with CRC32C");
    if (rep->compression_dict_handle.size() != 0)
        return Status::NotSupported("merged input does not support compression dictionaries");
    if (rep->separate_key_value_in_data_block)
        return Status::NotSupported("merged input does not support separated key/value data blocks");
    if (rep->table_properties && rep->table_properties->num_range_deletions != 0)
        return Status::NotSupported("merged input does not support range tombstones");
    const Comparator *ucmp = rep->internal_comparator.user_comparator();
    if (ucmp != BytewiseComparator() || ucmp->timestamp_size() != 0)
        return Status::NotSupported("merged input requires the bytewise comparator without timestamps");

    file->path = rep->file->file_name();
    file->file_size = rep->file_size;
    file->blocks.clear();
    BlockCacheLookupContext lookup_context{TableReaderCaller::kCompaction};
    std::unique_ptr<InternalIteratorBase<IndexValue>> index(
        NewIndexIterator(read_options, /*disable_prefix_seek=*/true, /*input_iter=*/nullptr,
                         /*get_context=*/nullptr, &lookup_context));
    const auto base = rep->footer.base_context_checksum();
    std::string previous_separator;
    bool have_previous = false;
    for (index->SeekToFirst(); index->Valid(); index->Next())
    {
        // Index key k of block i is >= every key in block i and < every key in
        // block i + 1, so block i covers user keys in (sep(i-1), sep(i)].
        // Index keys omit the sequence number when the table builder could
        // (index_key_includes_seq == false); they are then plain user keys.
        const Slice separator_user_key = rep->index_key_includes_seq ? ExtractUserKey(index->key()) : index->key();
        const bool before_start =
            start_user_key != nullptr && ucmp->Compare(separator_user_key, *start_user_key) < 0;
        const bool after_end = end_user_key != nullptr && have_previous &&
                               ucmp->Compare(Slice(previous_separator), *end_user_key) >= 0;
        previous_separator.assign(separator_user_key.data(), separator_user_key.size());
        have_previous = true;
        if (after_end)
            break;
        if (before_start)
            continue;
        const BlockHandle handle = index->value().handle;
        if (handle.size() > CompactionBlockTransform::max_block_bytes - kBlockTrailerSize ||
            handle.offset() > rep->file_size || handle.size() + kBlockTrailerSize > rep->file_size - handle.offset())
            return Status::Corruption("merged-input data block outside file bounds");
        file->blocks.push_back({handle.offset(), static_cast<std::uint32_t>(handle.size()),
                                ChecksumModifierForContext(base, handle.offset())});
    }
    return index->status();
}
