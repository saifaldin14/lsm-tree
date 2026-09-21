#pragma once

Status BlockBasedTableIterator::InitCompactionBatch(const BlockHandle &current)
{
    try
    {
        const auto *rep = table_->get_rep();
        const auto &transform = rep->table_options.compaction_block_transform;
        if (rep->footer.format_version() != 6 || rep->footer.checksum_type() != kCRC32c ||
            rep->compression_dict_handle.size() != 0 || rep->separate_key_value_in_data_block ||
            !read_options_.verify_checksums)
        {
            return Status::NotSupported("compaction batch requires format6 CRC32C without dictionary");
        }
        if (compaction_batch_.empty() || compaction_batch_.front().first != current.offset())
        {
            compaction_batch_.clear();
            if (!compaction_batch_index_)
            {
                compaction_batch_index_.reset(table_->NewIndexIterator(
                    read_options_, true, nullptr, nullptr, &lookup_context_));
            }
            std::string seek_key = index_iter_->key().ToString();
            if (!rep->index_key_includes_seq)
            {
                PutFixed64(&seek_key, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
            }
            compaction_batch_index_->Seek(seek_key);
            std::vector<std::string> serialized;
            std::vector<BlockHandle> handles;
            serialized.reserve(CompactionBlockTransform::max_blocks);
            handles.reserve(CompactionBlockTransform::max_blocks);
            size_t input_bytes = 0;
            size_t output_bytes = 0;
            IOOptions io_options;
            auto io_status = rep->file->PrepareIOOptions(read_options_, io_options);
            if (!io_status.ok()) return io_status;
            while (compaction_batch_index_->Valid() && handles.size() < CompactionBlockTransform::max_blocks)
            {
                const auto handle = compaction_batch_index_->value().handle;
                if (handle.size() > CompactionBlockTransform::max_block_bytes - 5 ||
                    handle.offset() > rep->file_size || handle.size() + 5 > rep->file_size - handle.offset())
                {
                    return Status::Corruption("compaction batch block outside file bounds");
                }
                const auto bytes = static_cast<size_t>(handle.size() + 5);
                if (bytes > CompactionBlockTransform::max_input_bytes - input_bytes) break;
                std::string buffer(bytes, '\0');
                Slice read;
                io_status = rep->file->Read(io_options, handle.offset(), bytes, &read, buffer.data());
                if (!io_status.ok()) return io_status;
                if (read.size() != bytes) return Status::Corruption("truncated compaction batch block");
                if (read.data() != buffer.data()) buffer.assign(read.data(), read.size());
                const auto compression = static_cast<unsigned char>(buffer[handle.size()]);
                Slice payload(buffer.data(), handle.size());
                uint32_t decoded = static_cast<uint32_t>(handle.size());
                if (compression == 4)
                {
                    if (!GetVarint32(&payload, &decoded)) return Status::Corruption("invalid LZ4 size in compaction batch");
                }
                else if (compression != 0)
                {
                    return Status::NotSupported("compaction batch codec is not LZ4 or uncompressed");
                }
                if (decoded > CompactionBlockTransform::max_block_bytes)
                    return Status::Corruption("decoded compaction block exceeds limit");
                if (decoded > CompactionBlockTransform::max_output_bytes - output_bytes) break;
                input_bytes += bytes;
                output_bytes += decoded;
                handles.push_back(handle);
                serialized.push_back(std::move(buffer));
                compaction_batch_index_->Next();
            }
            const auto index_status = compaction_batch_index_->status();
            if (!index_status.ok()) return index_status;
            if (handles.empty() || handles.front().offset() != current.offset())
            {
                return Status::Corruption("compaction batch index did not locate current block");
            }
            std::vector<CompactionBlockInput> input;
            for (size_t index = 0; index < handles.size(); ++index)
            {
                input.push_back({serialized[index], ChecksumModifierForContext(rep->footer.base_context_checksum(), handles[index].offset())});
            }
            std::vector<std::string> output;
            auto transformed = transform->Transform(input, &output);
            if (!transformed.ok()) return transformed;
            for (size_t index = 0; index < output.size(); ++index)
            {
                compaction_batch_.emplace_back(handles[index].offset(), std::move(output[index]));
            }
        }
        auto decoded = std::move(compaction_batch_.front().second);
        compaction_batch_.pop_front();
        auto allocation = AllocateBlock(decoded.size(), nullptr);
        memcpy(allocation.get(), decoded.data(), decoded.size());
        CachableEntry<Block> block;
        block.SetOwnedValue(std::make_unique<Block>(BlockContents(std::move(allocation), decoded.size()),
            rep->table_options.read_amp_bytes_per_bit, rep->ioptions.stats, rep->data_block_restart_interval));
        table_->NewDataBlockIterator<DataBlockIter>(read_options_, block, &block_iter_, Status::OK());
        return block_iter_.status();
    }
    catch (const std::exception &failure)
    {
        return Status::Aborted(failure.what());
    }
    catch (...)
    {
        return Status::Aborted("compaction batch threw an unknown exception");
    }
}