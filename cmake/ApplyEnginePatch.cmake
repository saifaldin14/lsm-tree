function(lsm_replace relative before after)
  set(path "${rocksdb_SOURCE_DIR}/${relative}")
  file(READ "${path}" content)
  string(FIND "${content}" "${after}" applied)
  if(NOT applied EQUAL -1)
    return()
  endif()
  string(FIND "${content}" "${before}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Pinned engine patch anchor missing: ${relative}")
  endif()
  string(REPLACE "${before}" "${after}" content "${content}")
  file(WRITE "${path}" "${content}")
endfunction()

lsm_replace("include/rocksdb/table.h"
  "#include \"rocksdb/cache.h\""
  "#include \"rocksdb/cache.h\"\n#include \"rocksdb/compaction_block_transform.h\"")
lsm_replace("include/rocksdb/table.h"
  "struct BlockBasedTableOptions {"
  "struct BlockBasedTableOptions {\n  std::shared_ptr<CompactionBlockTransform> compaction_block_transform;")
lsm_replace("include/rocksdb/options.h"
  "#include \"rocksdb/compression_type.h\""
  "#include \"rocksdb/compression_type.h\"\n#include \"rocksdb/compaction_block_transform.h\"")
lsm_replace("include/rocksdb/options.h"
  "struct ReadOptions {\n  // *** BEGIN options relevant to point lookups as well as scans ***"
  "struct ReadOptions {\n  mutable std::shared_ptr<CompactionBlockSession> compaction_block_session;\n\n  // *** BEGIN options relevant to point lookups as well as scans ***")
lsm_replace("table/internal_iterator.h"
  "  virtual void SetRangeDelReadSeqno(SequenceNumber /* read_seqno */) {}"
  "  virtual void SetRangeDelReadSeqno(SequenceNumber /* read_seqno */) {}\n\n  virtual Status PrepareCompactionBlocks() { return Status::OK(); }\n  virtual Status FinishCompactionBlocks() { return Status::OK(); }")
lsm_replace("table/block_based/block_based_table_reader.h"
  "  friend class TableCache;"
  "  friend class TableCache;\n  friend class BlockBasedTableIterator;")
set(block_iterator_header "${rocksdb_SOURCE_DIR}/table/block_based/block_based_table_iterator.h")
file(READ "${block_iterator_header}" block_iterator_header_content)
string(REPLACE
  "  Status InitCompactionBatch(const BlockHandle& handle);\n  std::deque<std::pair<uint64_t, std::string>> compaction_batch_;\n  std::unique_ptr<InternalIteratorBase<IndexValue>> compaction_batch_index_;\n\n  enum class IterDirection {"
  "  Status InitCompactionDecoded(std::string decoded);\n  Status InitCompactionBatch(const BlockHandle& handle);\n  Status PrepareCompactionBlocks() override;\n  Status FinishCompactionBlocks() override;\n  std::deque<std::pair<uint64_t, std::string>> compaction_batch_;\n  std::unique_ptr<InternalIteratorBase<IndexValue>> compaction_batch_index_;\n\n  enum class IterDirection {"
  migrated_block_iterator_header "${block_iterator_header_content}")
if(NOT block_iterator_header_content STREQUAL migrated_block_iterator_header)
  file(WRITE "${block_iterator_header}" "${migrated_block_iterator_header}")
endif()
lsm_replace("table/block_based/block_based_table_iterator.h"
  "  enum class IterDirection {"
  "  Status InitCompactionDecoded(std::string decoded);\n  Status InitCompactionBatch(const BlockHandle& handle);\n  Status PrepareCompactionBlocks() override;\n  Status FinishCompactionBlocks() override;\n  std::deque<std::pair<uint64_t, std::string>> compaction_batch_;\n  std::unique_ptr<InternalIteratorBase<IndexValue>> compaction_batch_index_;\n\n  enum class IterDirection {")
lsm_replace("table/block_based/block_based_table_iterator.cc"
  "#include \"monitoring/perf_context_imp.h\""
  "#include \"monitoring/perf_context_imp.h\"\n#include <cstdlib>\n#include \"util/coding.h\"\n#include \"memory/memory_allocator_impl.h\"")
set(iterator_path "${rocksdb_SOURCE_DIR}/table/block_based/block_based_table_iterator.cc")
file(READ "${iterator_path}" iterator_content)
string(REPLACE "lsm_lab/compaction_batch.inc" "lsm_lab/compaction_batch.hpp" migrated_iterator "${iterator_content}")
if(NOT iterator_content STREQUAL migrated_iterator)
  file(WRITE "${iterator_path}" "${migrated_iterator}")
endif()
lsm_replace("table/block_based/block_based_table_iterator.cc"
  "namespace ROCKSDB_NAMESPACE {"
  "namespace ROCKSDB_NAMESPACE {\n\n#include \"lsm_lab/compaction_batch.hpp\"")
# Migrates trees patched before LSM_DISABLE_GPU_INPUT existed; fresh sources get it from the
# InitDataBlock insertion below.
file(READ "${iterator_path}" iterator_content)
string(REPLACE
  "  if (lookup_context_.caller == TableReaderCaller::kCompaction &&\n      table_->get_rep()->table_options.compaction_block_transform) {"
  "  if (lookup_context_.caller == TableReaderCaller::kCompaction &&\n      std::getenv(\"LSM_DISABLE_GPU_INPUT\") == nullptr &&\n      table_->get_rep()->table_options.compaction_block_transform) {"
  migrated_iterator "${iterator_content}")
if(NOT iterator_content STREQUAL migrated_iterator)
  file(WRITE "${iterator_path}" "${migrated_iterator}")
endif()
lsm_replace("table/block_based/block_based_table_iterator.cc"
  "void BlockBasedTableIterator::InitDataBlock() {"
  [=[void BlockBasedTableIterator::InitDataBlock() {
  if (lookup_context_.caller == TableReaderCaller::kCompaction &&
      std::getenv("LSM_DISABLE_GPU_INPUT") == nullptr &&
      table_->get_rep()->table_options.compaction_block_transform) {
    const auto handle = index_iter_->value().handle;
    if (!block_iter_points_to_real_block_ || handle.offset() != prev_block_offset_) {
      ResetDataIter();
      Status batch_status = InitCompactionBatch(handle);
      if (!batch_status.ok()) block_iter_.Invalidate(batch_status);
      block_iter_points_to_real_block_ = true;
      prev_block_offset_ = handle.offset();
      CheckDataBlockWithinUpperBound();
    }
    return;
  }
]=])
lsm_replace("table/block_based/block_based_table_builder.h"
  "#include <array>"
  "#include <array>\n#include <deque>")
set(builder_header_path "${rocksdb_SOURCE_DIR}/table/block_based/block_based_table_builder.h")
file(READ "${builder_header_path}" builder_header_content)
string(REPLACE
  "  struct PendingCompactionOutput {\n    std::string uncompressed;\n    std::string last_key;\n    std::optional<std::string> first_key;\n  };\n  bool QueueCompactionOutput(const Slice& uncompressed, const Slice& last_key,\n                             const Slice* first_key);\n  void FlushCompactionOutput();\n  std::vector<PendingCompactionOutput> compaction_output_;\n  std::size_t compaction_output_bytes_ = 0;\n\n  void EmitBlockForParallel(std::string& uncompressed,"
  "  struct PendingCompactionOutput {\n    std::string uncompressed;\n    std::string last_key;\n    std::optional<std::string> first_key;\n  };\n  struct SubmittedCompactionOutput {\n    std::vector<PendingCompactionOutput> pending;\n    std::unique_ptr<CompactionOutputSubmission> submission;\n  };\n  bool QueueCompactionOutput(const Slice& uncompressed, const Slice& last_key,\n                             const Slice* first_key);\n  void SubmitCompactionOutput();\n  void DrainCompactionOutput(bool all);\n  void FlushCompactionOutput();\n  std::vector<PendingCompactionOutput> compaction_output_;\n  std::deque<SubmittedCompactionOutput> submitted_compaction_output_;\n  std::size_t compaction_output_bytes_ = 0;\n\n  void EmitBlockForParallel(std::string& uncompressed,"
  migrated_builder_header "${builder_header_content}")
if(NOT builder_header_content STREQUAL migrated_builder_header)
  file(WRITE "${builder_header_path}" "${migrated_builder_header}")
endif()
lsm_replace("table/block_based/block_based_table_builder.h"
  "  void EmitBlockForParallel(std::string& uncompressed,"
  "  struct PendingCompactionOutput {\n    std::string uncompressed;\n    std::string last_key;\n    std::optional<std::string> first_key;\n  };\n  struct SubmittedCompactionOutput {\n    std::vector<PendingCompactionOutput> pending;\n    std::unique_ptr<CompactionOutputSubmission> submission;\n  };\n  bool QueueCompactionOutput(const Slice& uncompressed, const Slice& last_key,\n                             const Slice* first_key);\n  void SubmitCompactionOutput();\n  void DrainCompactionOutput(bool all);\n  void FlushCompactionOutput();\n  std::vector<PendingCompactionOutput> compaction_output_;\n  std::deque<SubmittedCompactionOutput> submitted_compaction_output_;\n  std::size_t compaction_output_bytes_ = 0;\n\n  void EmitBlockForParallel(std::string& uncompressed,")
lsm_replace("table/block_based/block_based_table_builder.h"
  "                                 bool* skip_delta_encoding = nullptr);"
  "                                 bool* skip_delta_encoding = nullptr,\n                                 const uint32_t* precomputed_checksum = nullptr);")
lsm_replace("table/block_based/block_based_table_builder.h"
  "      bool* skip_delta_encoding = nullptr);"
  "      bool* skip_delta_encoding = nullptr,\n      const uint32_t* precomputed_checksum = nullptr);")
set(block_builder_path "${rocksdb_SOURCE_DIR}/table/block_based/block_based_table_builder.cc")
file(READ "${block_builder_path}" block_builder_content)
string(REPLACE
  "namespace ROCKSDB_NAMESPACE {\n\n#include \"lsm_lab/compaction_output.hpp\""
  "namespace ROCKSDB_NAMESPACE {"
  migrated_block_builder "${block_builder_content}")
if(NOT block_builder_content STREQUAL migrated_block_builder)
  file(WRITE "${block_builder_path}" "${migrated_block_builder}")
endif()
lsm_replace("table/block_based/block_based_table_builder.cc"
  "BlockBasedTableBuilder::BlockBasedTableBuilder("
  "#include \"lsm_lab/compaction_output.hpp\"\n\nBlockBasedTableBuilder::BlockBasedTableBuilder(")
lsm_replace("table/block_based/block_based_table_builder.cc"
  "  assert(uncompressed.size() > 0);\n  // When data blocks are aligned"
  "  assert(uncompressed.size() > 0);\n  if (QueueCompactionOutput(uncompressed, last_key_in_current_block, first_key_in_next_block)) {\n    return;\n  }\n  // When data blocks are aligned")
lsm_replace("table/block_based/block_based_table_builder.cc"
  "  if (r->IsParallelCompressionActive()) {\n    StopParallelCompression(/*abort=*/false);\n  }\n\n  r->props.tail_start_offset"
  "  if (r->IsParallelCompressionActive()) {\n    StopParallelCompression(/*abort=*/false);\n  }\n  FlushCompactionOutput();\n\n  r->props.tail_start_offset")
lsm_replace("table/block_based/block_based_table_builder.cc"
  "    bool* skip_delta_encoding) {\n  // Must have pre-checked status in single-threaded context"
  "    bool* skip_delta_encoding, const uint32_t* precomputed_checksum) {\n  // Must have pre-checked status in single-threaded context")
lsm_replace("table/block_based/block_based_table_builder.cc"
  "      skip_delta_encoding));\n}\n\nIOStatus BlockBasedTableBuilder::WriteMaybeCompressedBlockImpl("
  "      skip_delta_encoding, precomputed_checksum));\n}\n\nIOStatus BlockBasedTableBuilder::WriteMaybeCompressedBlockImpl(")
lsm_replace("table/block_based/block_based_table_builder.cc"
  "    bool* skip_delta_encoding) {\n  // File format contains a sequence of blocks"
  "    bool* skip_delta_encoding, const uint32_t* precomputed_checksum) {\n  // File format contains a sequence of blocks")
lsm_replace("table/block_based/block_based_table_builder.cc"
  "  uint32_t checksum = ComputeBuiltinChecksumWithLastByte(\n      r->table_options.checksum, block_contents.data(), block_contents.size(),\n      /*last_byte*/ comp_type);\n  checksum += ChecksumModifierForContext(r->base_context_checksum, offset);"
  "  uint32_t checksum = precomputed_checksum\n                          ? *precomputed_checksum\n                          : ComputeBuiltinChecksumWithLastByte(\n                                r->table_options.checksum, block_contents.data(),\n                                block_contents.size(), /*last_byte*/ comp_type);\n  checksum += ChecksumModifierForContext(r->base_context_checksum, offset);")
lsm_replace("table/compaction_merging_iterator.cc"
  [=[  for (auto& child : children_) {
    child.iter.SeekToFirst();
    AddToMinHeapOrCheckStatus(&child);
  }
]=]
  [=[  for (auto& child : children_) {
    considerStatus(child.iter.iter()->PrepareCompactionBlocks());
  }
  for (auto& child : children_) {
    considerStatus(child.iter.iter()->FinishCompactionBlocks());
  }
  if (!status_.ok()) {
    return;
  }
  for (auto& child : children_) {
    child.iter.SeekToFirst();
    AddToMinHeapOrCheckStatus(&child);
  }
]=])
lsm_replace("db/version_set.cc"
  "  void SeekToFirst() override;\n  void SeekToLast() override;"
  "  void SeekToFirst() override;\n  void SeekToLast() override;\n  Status PrepareCompactionBlocks() override;\n  Status FinishCompactionBlocks() override;")
set(version_set_path "${rocksdb_SOURCE_DIR}/db/version_set.cc")
file(READ "${version_set_path}" version_set_content)
string(REPLACE
  "Status LevelIterator::PrepareCompactionBlocks() { return Status::OK(); }\n"
  [=[Status LevelIterator::PrepareCompactionBlocks() {
  const auto previous = file_index_;
  for (size_t file = 0; file < flevel_->num_files; ++file) {
    file_index_ = file;
    std::unique_ptr<InternalIterator> iterator(NewFileIterator());
    if (iterator) {
      Status status = iterator->PrepareCompactionBlocks();
      if (!status.ok()) {
        file_index_ = previous;
        return status;
      }
    }
  }
  file_index_ = previous;
  ClearRangeTombstoneIter();
  return Status::OK();
}
]=]
  migrated_version_set "${version_set_content}")
if(NOT version_set_content STREQUAL migrated_version_set)
  file(WRITE "${version_set_path}" "${migrated_version_set}")
endif()
lsm_replace("db/version_set.cc"
  "void LevelIterator::SeekToFirst() {"
  [=[Status LevelIterator::PrepareCompactionBlocks() {
  const auto previous = file_index_;
  for (size_t file = 0; file < flevel_->num_files; ++file) {
    file_index_ = file;
    std::unique_ptr<InternalIterator> iterator(NewFileIterator());
    if (iterator) {
      Status status = iterator->PrepareCompactionBlocks();
      if (!status.ok()) {
        file_index_ = previous;
        return status;
      }
    }
  }
  file_index_ = previous;
  ClearRangeTombstoneIter();
  return Status::OK();
}

Status LevelIterator::FinishCompactionBlocks() {
  if (!read_options_.compaction_block_session) {
    return Status::OK();
  }
  return read_options_.compaction_block_session->Finish();
}

void LevelIterator::SeekToFirst() {]=])
# Merged-input compaction offload (include/lsm_lab/compaction_merge_*.hpp).
lsm_replace("table/block_based/block_based_table_reader.h"
  "  bool TEST_BlockInCache(const BlockHandle& handle) const;"
  "  bool TEST_BlockInCache(const BlockHandle& handle) const;\n\n  Status LsmCollectDataBlocks(const ReadOptions& read_options,\n                              const Slice* start_user_key,\n                              const Slice* end_user_key,\n                              CompactionMergeFile* file);")
lsm_replace("table/block_based/block_based_table_reader.cc"
  "bool BlockBasedTable::TEST_BlockInCache(const BlockHandle& handle) const {"
  "#include \"lsm_lab/compaction_merge_reader.hpp\"\n\nbool BlockBasedTable::TEST_BlockInCache(const BlockHandle& handle) const {")
lsm_replace("db/table_cache.h"
  "  Status GetTableProperties(const FileOptions& toptions,"
  "  Status LsmCollectDataBlocks(const ReadOptions& read_options,\n                              const InternalKeyComparator& internal_comparator,\n                              const FileMetaData& file_meta,\n                              const MutableCFOptions& mutable_cf_options,\n                              const Slice* start_user_key,\n                              const Slice* end_user_key,\n                              CompactionMergeFile* file);\n\n  Status GetTableProperties(const FileOptions& toptions,")
lsm_replace("db/table_cache.cc"
  "Status TableCache::GetTableProperties("
  [=[Status TableCache::LsmCollectDataBlocks(
    const ReadOptions& read_options,
    const InternalKeyComparator& internal_comparator,
    const FileMetaData& file_meta, const MutableCFOptions& mutable_cf_options,
    const Slice* start_user_key, const Slice* end_user_key,
    CompactionMergeFile* file) {
  TypedHandle* table_handle = nullptr;
  TableReader* table = nullptr;
  Status s = FindTable(read_options, file_options_, internal_comparator,
                       file_meta, &table_handle, mutable_cf_options, &table);
  if (s.ok()) {
    auto* block_based = dynamic_cast<BlockBasedTable*>(table);
    if (block_based == nullptr) {
      s = Status::NotSupported("merged input requires block-based tables");
    } else {
      s = block_based->LsmCollectDataBlocks(read_options, start_user_key,
                                            end_user_key, file);
    }
  }
  if (table_handle) {
    cache_.Release(table_handle);
  }
  return s;
}

Status TableCache::GetTableProperties(]=])
lsm_replace("db/compaction/compaction_job.cc"
  "void CompactionJob::ProcessKeyValueCompaction(SubcompactionState* sub_compact) {"
  "#include \"lsm_lab/compaction_merge_job.hpp\"\n\nvoid CompactionJob::ProcessKeyValueCompaction(SubcompactionState* sub_compact) {")
set(compaction_job_path "${rocksdb_SOURCE_DIR}/db/compaction/compaction_job.cc")
file(READ "${compaction_job_path}" compaction_job_content)
string(REPLACE
  "        read_options, &lsm_merged_input);"
  "        read_options,\n        job_context_->snapshot_checker == nullptr ? &job_context_->snapshot_seqs\n                                                  : nullptr,\n        &lsm_merged_input);"
  migrated_compaction_job "${compaction_job_content}")
if(NOT compaction_job_content STREQUAL migrated_compaction_job)
  file(WRITE "${compaction_job_path}" "${migrated_compaction_job}")
endif()
# Stage 3 migration: the merged-input call gains the built-output context.
file(READ "${compaction_job_path}" compaction_job_content)
string(REPLACE [=[
    const Status lsm_status = LsmMaybeCreateMergedInput(
        sub_compact->compaction, cfd, boundaries.start, boundaries.end,
        read_options,
        job_context_->snapshot_checker == nullptr ? &job_context_->snapshot_seqs
                                                  : nullptr,
        &lsm_merged_input);]=] [=[
    LsmBuildContext lsm_build;
    lsm_build.allowed = job_context_->snapshot_checker == nullptr &&
                        compaction_progress_writer_ == nullptr &&
                        !sub_compact->compaction->SupportsPerKeyPlacement();
    lsm_build.zero_sequences = sub_compact->compaction->bottommost_level();
    lsm_build.zero_limit = std::min(earliest_snapshot_, preserve_seqno_after_);
    const Status lsm_status = LsmMaybeCreateMergedInput(
        sub_compact->compaction, cfd, boundaries.start, boundaries.end,
        read_options,
        job_context_->snapshot_checker == nullptr ? &job_context_->snapshot_seqs
                                                  : nullptr,
        &lsm_build, &lsm_merged_input);]=] migrated_compaction_job "${compaction_job_content}")
if(NOT compaction_job_content STREQUAL migrated_compaction_job)
  file(WRITE "${compaction_job_path}" "${migrated_compaction_job}")
endif()
lsm_replace("db/compaction/compaction_job.cc"
  "  InternalIterator* input_iter = CreateInputIterator(\n      sub_compact, cfd, iterators, boundaries, read_options);\n\n  assert(input_iter);\n"
  [=[  InternalIterator* input_iter = CreateInputIterator(
      sub_compact, cfd, iterators, boundaries, read_options);

  assert(input_iter);

  std::unique_ptr<InternalIterator> lsm_merged_input;
  {
    LsmBuildContext lsm_build;
    lsm_build.allowed = job_context_->snapshot_checker == nullptr &&
                        compaction_progress_writer_ == nullptr &&
                        !sub_compact->compaction->SupportsPerKeyPlacement();
    lsm_build.zero_sequences = sub_compact->compaction->bottommost_level();
    lsm_build.zero_limit = std::min(earliest_snapshot_, preserve_seqno_after_);
    lsm_build.earliest_snapshot = earliest_snapshot_;
    const Status lsm_status = LsmMaybeCreateMergedInput(
        sub_compact->compaction, cfd, boundaries.start, boundaries.end,
        read_options,
        job_context_->snapshot_checker == nullptr ? &job_context_->snapshot_seqs
                                                  : nullptr,
        &lsm_build, &lsm_merged_input);
    if (!lsm_status.ok()) {
      sub_compact->status = lsm_status;
      return;
    }
    if (lsm_merged_input) {
      input_iter = lsm_merged_input.get();
    }
  }
]=])
lsm_replace("db/compaction/compaction_job.cc"
  "  FinalizeSubcompaction(sub_compact, status, open_file_func, close_file_func,\n                        blob_file_builder.get(), c_iter.get(), input_iter,\n                        start_cpu_micros, prev_cpu_micros, io_stats);\n\n  NotifyOnSubcompactionCompleted(sub_compact);"
  "  FinalizeSubcompaction(sub_compact, status, open_file_func, close_file_func,\n                        blob_file_builder.get(), c_iter.get(), input_iter,\n                        start_cpu_micros, prev_cpu_micros, io_stats);\n  if (lsm_merged_input) {\n    LsmAccountMergedInputDrops(lsm_merged_input.get(),\n                               &sub_compact->compaction_job_stats);\n  }\n\n  NotifyOnSubcompactionCompleted(sub_compact);")

# Stage 3: finished data blocks from the accelerator replace CompactionIterator
# and per-key table building (include/lsm_lab/compaction_merge_job.hpp,
# include/lsm_lab/compaction_output.hpp).
lsm_replace("table/table_builder.h"
  "  virtual uint64_t EstimatedFileSize() const { return FileSize(); }"
  "  virtual uint64_t EstimatedFileSize() const { return FileSize(); }\n\n  // Adds a finished data block (block-based tables only).\n  virtual Status LsmAddBuiltBlock(const CompactionBuiltBlock& /*block*/,\n                                  const char* /*contents*/,\n                                  const Slice& /*first_key*/,\n                                  const Slice& /*last_key*/,\n                                  const uint64_t* /*hashes*/) {\n    return Status::NotSupported(\"built data blocks\");\n  }")
lsm_replace("table/block_based/block_based_table_builder.h"
  "  Status Finish() override;"
  "  Status Finish() override;\n\n  Status LsmAddBuiltBlock(const CompactionBuiltBlock& block, const char* contents,\n                          const Slice& first_key, const Slice& last_key,\n                          const uint64_t* hashes) override;\n  void LsmFinishPendingIndex(const Slice* first_key_in_next_block);\n  std::string lsm_pending_index_key_;\n  bool lsm_pending_index_ = false;")
lsm_replace("table/block_based/block_based_table_builder.cc"
  "  // To make sure properties block is able to keep the accurate size of index\n  // block, we will finish writing all index entries first, in Flush().\n  Flush(/*first_key_in_next_block=*/nullptr);"
  "  LsmFinishPendingIndex(nullptr);\n  // To make sure properties block is able to keep the accurate size of index\n  // block, we will finish writing all index entries first, in Flush().\n  Flush(/*first_key_in_next_block=*/nullptr);")
lsm_replace("table/block_based/filter_policy_internal.h"
  "  virtual size_t EstimateEntriesAdded() = 0;"
  "  virtual size_t EstimateEntriesAdded() = 0;\n\n  // Adds a precomputed Hash64 of a whole key; false if unsupported.\n  virtual bool LsmAddKeyHash(uint64_t /*hash*/) { return false; }")
lsm_replace("table/block_based/filter_policy.cc"
  "  size_t EstimateEntriesAdded() override {\n    return hash_entries_info_.entries_count.LoadRelaxed();\n  }"
  "  size_t EstimateEntriesAdded() override {\n    return hash_entries_info_.entries_count.LoadRelaxed();\n  }\n\n  // Same as AddKey with the hash already computed.\n  bool LsmAddKeyHash(uint64_t hash) override {\n    if (hash_entries_info_.entries.empty() ||\n        hash != hash_entries_info_.entries.back()) {\n      AddHash(hash);\n    }\n    return true;\n  }")
lsm_replace("table/block_based/filter_block.h"
  "  virtual size_t EstimateEntriesAdded() = 0;"
  "  virtual size_t EstimateEntriesAdded() = 0;\n\n  // Adds precomputed Hash64 values of whole user keys; false if unsupported.\n  virtual bool LsmAddKeyHashes(const uint64_t* /*hashes*/, size_t /*count*/) {\n    return false;\n  }")
lsm_replace("table/block_based/full_filter_block.h"
  "  void AddWithPrevKey(const Slice& key_without_ts,\n                      const Slice& prev_key_without_ts) override;"
  "  void AddWithPrevKey(const Slice& key_without_ts,\n                      const Slice& prev_key_without_ts) override;\n\n  bool LsmAddKeyHashes(const uint64_t* hashes, size_t count) override {\n    if (prefix_extractor_ != nullptr || !whole_key_filtering_) {\n      return false;\n    }\n    for (size_t i = 0; i < count; ++i) {\n      if (!filter_bits_builder_->LsmAddKeyHash(hashes[i])) {\n        return false;\n      }\n    }\n    return true;\n  }")
lsm_replace("db/compaction/compaction_outputs.h"
  "  bool ShouldStopBefore(const CompactionIterator& c_iter);"
  "  bool ShouldStopBefore(const CompactionIterator& c_iter);\n  bool LsmShouldStopBefore(const Slice& internal_key, const Slice& lsm_user_key);\n\n public:\n  // Adds a finished data block; may close the current file first when\n  // can_cut is set and the regular file-cutting rules say so.\n  Status LsmAddBuiltBlock(const CompactionBuiltBlock& block, const char* contents,\n                          const Slice& first_key, const Slice& last_key,\n                          const uint64_t* hashes, bool can_cut,\n                          const CompactionFileOpenFunc& open_file_func,\n                          const CompactionFileCloseFunc& close_file_func,\n                          const ParsedInternalKey& prev_iter_output_internal_key);\n\n private:")
lsm_replace("db/compaction/compaction_outputs.cc"
  "bool CompactionOutputs::ShouldStopBefore(const CompactionIterator& c_iter) {\n  assert(c_iter.Valid());\n  const Slice& internal_key = c_iter.key();"
  "bool CompactionOutputs::ShouldStopBefore(const CompactionIterator& c_iter) {\n  assert(c_iter.Valid());\n  return LsmShouldStopBefore(c_iter.key(), c_iter.user_key());\n}\n\nbool CompactionOutputs::LsmShouldStopBefore(const Slice& internal_key,\n                                            const Slice& lsm_user_key) {")
lsm_replace("db/compaction/compaction_outputs.cc"
  "last_key_for_partitioner_, c_iter.user_key(),"
  "last_key_for_partitioner_, lsm_user_key,")
lsm_replace("db/compaction/compaction_outputs.cc"
  "Status CompactionOutputs::AddToOutput(\n    const CompactionIterator& c_iter,"
  [=[Status CompactionOutputs::LsmAddBuiltBlock(
    const CompactionBuiltBlock& block, const char* contents,
    const Slice& first_key, const Slice& last_key, const uint64_t* hashes,
    bool can_cut, const CompactionFileOpenFunc& open_file_func,
    const CompactionFileCloseFunc& close_file_func,
    const ParsedInternalKey& prev_iter_output_internal_key) {
  Status s;
  // Always evaluated, as for every key on the regular path, so grandparent
  // and TTL tracking advance; files are only cut between user keys.
  const bool stop = LsmShouldStopBefore(first_key, ExtractUserKey(first_key));
  if (can_cut && stop && HasBuilder()) {
    s = close_file_func(Status::OK(), prev_iter_output_internal_key, first_key,
                        nullptr, *this);
    if (!s.ok()) {
      return s;
    }
    grandparent_boundary_switched_num_ = 0;
    grandparent_overlapped_bytes_ =
        GetCurrentKeyGrandparentOverlappedBytes(first_key);
    range_tombstone_lower_bound_.Clear();
  }
  if (!HasBuilder()) {
    s = open_file_func(*this);
    if (!s.ok()) {
      return s;
    }
  }
  assert(builder_ != nullptr);
  s = current_output().validator.Add(first_key, Slice());
  if (s.ok()) {
    s = current_output().validator.Add(last_key, Slice());
  }
  if (!s.ok()) {
    return s;
  }
  s = builder_->LsmAddBuiltBlock(block, contents, first_key, last_key, hashes);
  if (!s.ok()) {
    return s;
  }
  stats_.num_output_records += block.entries;
  current_output_file_size_ = builder_->EstimatedFileSize();
  s = current_output().meta.UpdateBoundaries(first_key, Slice(),
                                             block.smallest_seqno, kTypeValue);
  if (s.ok()) {
    s = current_output().meta.UpdateBoundaries(last_key, Slice(),
                                               block.largest_seqno, kTypeValue);
  }
  return s;
}

Status CompactionOutputs::AddToOutput(
    const CompactionIterator& c_iter,]=])
lsm_replace("db/compaction/compaction_job.cc"
  "#include \"db/compaction/compaction_job.h\""
  "#include \"db/compaction/compaction_job.h\"\n#include \"rocksdb/flush_block_policy.h\"\n#include \"table/block_based/filter_policy_internal.h\"")
lsm_replace("db/compaction/compaction_job.cc"
  "  status = ProcessKeyValue(sub_compact, cfd, c_iter.get(), open_file_func,\n                           close_file_func, prev_cpu_micros);"
  "  if (const auto* lsm_built = LsmBuiltOutputOf(lsm_merged_input.get())) {\n    status = LsmProcessBuiltOutput(*lsm_built, sub_compact, cfd, open_file_func,\n                                   close_file_func, compaction_aborted_);\n  } else {\n    status = ProcessKeyValue(sub_compact, cfd, c_iter.get(), open_file_func,\n                             close_file_func, prev_cpu_micros);\n  }")

# Deletions on the built-output path need CompactionIterator's earliest snapshot. Fresh trees
# get the line with the merged-input block above; this migrates trees patched before it existed.
lsm_replace("db/compaction/compaction_job.cc"
  "    lsm_build.zero_limit = std::min(earliest_snapshot_, preserve_seqno_after_);\n"
  "    lsm_build.zero_limit = std::min(earliest_snapshot_, preserve_seqno_after_);\n    lsm_build.earliest_snapshot = earliest_snapshot_;\n")

# Built output in runs: the table builder fills each block's trailer in the gap the
# accelerator left after it and appends a run of blocks with one write
# (include/lsm_lab/compaction_output.hpp); CompactionOutputs still decides file cuts block by
# block. Inserted next to (not inside) the text of earlier steps, so re-running stays a no-op.
lsm_replace("table/table_builder.h"
  "    return Status::NotSupported(\"built data blocks\");\n  }"
  "    return Status::NotSupported(\"built data blocks\");\n  }\n\n  // Adds a run of finished data blocks with trailer gaps (block-based tables only).\n  virtual Status LsmAddBuiltRun(const CompactionBuiltBlock* /*blocks*/,\n                                size_t /*count*/, char* /*contents*/,\n                                const char* /*keys*/,\n                                const uint64_t* /*hashes*/) {\n    return Status::NotSupported(\"built data block runs\");\n  }")
lsm_replace("table/block_based/block_based_table_builder.h"
  "  bool lsm_pending_index_ = false;"
  "  bool lsm_pending_index_ = false;\n  Status LsmAddBuiltRun(const CompactionBuiltBlock* blocks, size_t count,\n                        char* contents, const char* keys,\n                        const uint64_t* hashes) override;")
lsm_replace("db/compaction/compaction_outputs.h"
  "                          const ParsedInternalKey& prev_iter_output_internal_key);\n\n private:"
  "                          const ParsedInternalKey& prev_iter_output_internal_key);\n\n private:\n\n public:\n  // Adds blocks [begin, end) of built output, cutting files block by block as\n  // LsmAddBuiltBlock does and writing each run that stays in one file at once.\n  Status LsmAddBuiltBlocks(const CompactionBuiltOutput& built, size_t begin,\n                           size_t end,\n                           const CompactionFileOpenFunc& open_file_func,\n                           const CompactionFileCloseFunc& close_file_func,\n                           Slice* previous_last);\n\n private:")
lsm_replace("db/compaction/compaction_outputs.cc"
  "Status CompactionOutputs::LsmAddBuiltBlock(\n    const CompactionBuiltBlock& block, const char* contents,"
  [=[// *previous_last is the last key of the block before begin (empty at the start of
// the output). Cut decisions see the same file size as with one block at a time.
Status CompactionOutputs::LsmAddBuiltBlocks(
    const CompactionBuiltOutput& built, size_t begin, size_t end,
    const CompactionFileOpenFunc& open_file_func,
    const CompactionFileCloseFunc& close_file_func, Slice* previous_last) {
  Status s;
  char* contents = const_cast<char*>(built.contents);
  size_t run = begin;
  uint64_t run_bytes = 0;
  SequenceNumber run_smallest = kMaxSequenceNumber;
  SequenceNumber run_largest = 0;
  const auto flush_run = [&](size_t until) {
    Status status;
    if (until == run) {
      return status;
    }
    status = builder_->LsmAddBuiltRun(built.blocks + run, until - run, contents,
                                      built.keys, built.hashes);
    if (status.ok()) {
      const auto& first_block = built.blocks[run];
      const auto& last_block = built.blocks[until - 1];
      const Slice first(built.keys + first_block.keys_offset,
                        first_block.first_key_bytes);
      const Slice last(built.keys + last_block.keys_offset +
                           last_block.first_key_bytes,
                       last_block.last_key_bytes);
      for (size_t i = run; i < until; ++i) {
        stats_.num_output_records += built.blocks[i].entries;
      }
      current_output_file_size_ = builder_->EstimatedFileSize();
      status = current_output().meta.UpdateBoundaries(first, Slice(),
                                                      run_smallest, kTypeValue);
      if (status.ok()) {
        status = current_output().meta.UpdateBoundaries(
            last, Slice(), run_largest, kTypeValue);
      }
    }
    run = until;
    run_bytes = 0;
    run_smallest = kMaxSequenceNumber;
    run_largest = 0;
    return status;
  };
  for (size_t index = begin; index < end; ++index) {
    const auto& block = built.blocks[index];
    const Slice first(built.keys + block.keys_offset, block.first_key_bytes);
    const Slice last(first.data() + block.first_key_bytes,
                     block.last_key_bytes);
    const bool can_cut = !previous_last->empty() &&
                         ExtractUserKey(first) != ExtractUserKey(*previous_last);
    if (HasBuilder()) {
      current_output_file_size_ = builder_->EstimatedFileSize() + run_bytes;
    }
    // Always evaluated, as for every key on the regular path.
    const bool stop = LsmShouldStopBefore(first, ExtractUserKey(first));
    if (can_cut && stop && HasBuilder()) {
      s = flush_run(index);
      if (!s.ok()) {
        return s;
      }
      ParsedInternalKey previous;
      s = ParseInternalKey(*previous_last, &previous, /*log_err_key=*/false);
      if (!s.ok()) {
        return s;
      }
      s = close_file_func(Status::OK(), previous, first, nullptr, *this);
      if (!s.ok()) {
        return s;
      }
      grandparent_boundary_switched_num_ = 0;
      grandparent_overlapped_bytes_ =
          GetCurrentKeyGrandparentOverlappedBytes(first);
      range_tombstone_lower_bound_.Clear();
    }
    if (!HasBuilder()) {
      s = open_file_func(*this);
      if (!s.ok()) {
        return s;
      }
    }
    s = current_output().validator.Add(first, Slice());
    if (s.ok()) {
      s = current_output().validator.Add(last, Slice());
    }
    if (!s.ok()) {
      return s;
    }
    // The block's trailer is the same size as the gap left for it.
    run_bytes += block.contents_bytes + kBuiltTrailerGap;
    run_smallest = std::min<SequenceNumber>(run_smallest, block.smallest_seqno);
    run_largest = std::max<SequenceNumber>(run_largest, block.largest_seqno);
    *previous_last = last;
  }
  return flush_run(end);
}

Status CompactionOutputs::LsmAddBuiltBlock(
    const CompactionBuiltBlock& block, const char* contents,]=])

# Filters of built output: the table builder hands the filter builder the (contiguous) key
# hashes of each file instead of copying them into its entry queue, and Finish sets the
# FastLocalBloom bits through the compaction transform (GPU), or on the CPU when it declines.
# Sizing, probe count and metadata stay RocksDB's own.
lsm_replace("table/block_based/filter_policy_internal.h"
  "  virtual bool LsmAddKeyHash(uint64_t /*hash*/) { return false; }"
  "  virtual bool LsmAddKeyHash(uint64_t /*hash*/) { return false; }\n\n  // Uses count hashes held by the caller (alive until Finish) instead of copying\n  // them; false if unsupported, and then nothing was added.\n  virtual bool LsmAddExternalHashes(const uint64_t* /*hashes*/, size_t /*count*/,\n                                    CompactionBlockTransform* /*transform*/) {\n    return false;\n  }")
lsm_replace("table/block_based/filter_block.h"
  "  virtual bool LsmAddKeyHashes(const uint64_t* /*hashes*/, size_t /*count*/) {\n    return false;\n  }"
  "  virtual bool LsmAddKeyHashes(const uint64_t* /*hashes*/, size_t /*count*/) {\n    return false;\n  }\n\n  // As LsmAddKeyHashes, but the hashes stay with the caller until Finish.\n  virtual bool LsmAddExternalKeyHashes(const uint64_t* /*hashes*/,\n                                       size_t /*count*/,\n                                       CompactionBlockTransform* /*transform*/) {\n    return false;\n  }")
lsm_replace("table/block_based/full_filter_block.h"
  "      if (!filter_bits_builder_->LsmAddKeyHash(hashes[i])) {\n        return false;\n      }\n    }\n    return true;\n  }"
  "      if (!filter_bits_builder_->LsmAddKeyHash(hashes[i])) {\n        return false;\n      }\n    }\n    return true;\n  }\n\n  bool LsmAddExternalKeyHashes(const uint64_t* hashes, size_t count,\n                               CompactionBlockTransform* transform) override {\n    if (prefix_extractor_ != nullptr || !whole_key_filtering_) {\n      return false;\n    }\n    return filter_bits_builder_->LsmAddExternalHashes(hashes, count, transform);\n  }")
lsm_replace("table/block_based/filter_policy.cc"
  "  ~FastLocalBloomBitsBuilder() override = default;\n\n  using FilterBitsBuilder::Finish;\n\n  Slice Finish(std::unique_ptr<const char[]>* buf) override {\n    return Finish(buf, nullptr);\n  }\n\n  Slice Finish(std::unique_ptr<const char[]>* buf, Status* status) override {"
  "  ~FastLocalBloomBitsBuilder() override = default;\n\n  using FilterBitsBuilder::Finish;\n\n  Slice Finish(std::unique_ptr<const char[]>* buf) override {\n    return Finish(buf, nullptr);\n  }\n\n  Slice Finish(std::unique_ptr<const char[]>* buf, Status* status) override {\n    if (lsm_external_ != nullptr) {\n      return LsmFinishExternal(buf, status);\n    }")
lsm_replace("table/block_based/filter_policy.cc"
  "  // Target allocation per added key, in thousandths of a bit.\n  int millibits_per_key_;\n};"
  [=[ public:
  // External hashes (built compaction output): one contiguous range per file.
  // Only used while the entry queue is empty and no feature needs the queue.
  bool LsmAddExternalHashes(const uint64_t* hashes, size_t count,
                            CompactionBlockTransform* transform) override {
    if (detect_filter_construct_corruption_ || cache_res_mgr_ ||
        !hash_entries_info_.entries.empty() ||
        (lsm_external_ != nullptr && hashes != lsm_external_ + lsm_external_count_)) {
      // Move what was taken so far into the queue; the caller adds the rest there.
      for (size_t i = 0; i < lsm_external_count_; ++i) {
        LsmAddKeyHash(lsm_external_[i]);
      }
      lsm_external_ = nullptr;
      lsm_external_count_ = 0;
      return false;
    }
    if (lsm_external_ == nullptr) {
      lsm_external_ = hashes;
    }
    lsm_external_count_ += count;
    lsm_transform_ = transform;
    return true;
  }

  size_t EstimateEntriesAdded() override {
    return hash_entries_info_.entries_count.LoadRelaxed() + lsm_external_count_;
  }

  // Finish for external hashes: the same filter bytes as adding them to the
  // queue (which skips a hash equal to the one before it).
  Slice LsmFinishExternal(std::unique_ptr<const char[]>* buf, Status* status) {
    const uint64_t* hashes = lsm_external_;
    const size_t count = lsm_external_count_;
    lsm_external_ = nullptr;
    lsm_external_count_ = 0;
    if (status) {
      *status = Status::OK();
    }
    size_t num_entries = 0;
    for (size_t i = 0; i < count; ++i) {
      num_entries += (i == 0 || hashes[i] != hashes[i - 1]) ? 1 : 0;
    }
    if (num_entries == 0) {
      return FinishAlwaysFalse(buf);
    }
    size_t len_with_metadata = CalculateSpace(num_entries);
    std::unique_ptr<char[]> mutable_buf;
    len_with_metadata =
        AllocateMaybeRounding(len_with_metadata, num_entries, &mutable_buf);
    int num_probes = GetNumProbes(num_entries, len_with_metadata);
    uint32_t len = static_cast<uint32_t>(len_with_metadata - kMetadataLen);
    std::memset(mutable_buf.get(), 0, len_with_metadata);
    if (len > 0) {
      if (lsm_transform_ == nullptr ||
          !lsm_transform_
               ->BuildBloomFilter(hashes, count, len, num_probes,
                                  mutable_buf.get())
               .ok()) {
        for (size_t i = 0; i < count; ++i) {
          FastLocalBloomImpl::AddHash(Lower32of64(hashes[i]),
                                      Upper32of64(hashes[i]), len, num_probes,
                                      mutable_buf.get());
        }
      }
    }
    mutable_buf[len] = static_cast<char>(-1);
    mutable_buf[len + 1] = static_cast<char>(0);
    mutable_buf[len + 2] = static_cast<char>(num_probes);
    Slice rv(mutable_buf.get(), len_with_metadata);
    *buf = std::move(mutable_buf);
    final_filter_cache_res_handles_.push_back(nullptr);
    return rv;
  }

  const uint64_t* lsm_external_ = nullptr;
  size_t lsm_external_count_ = 0;
  CompactionBlockTransform* lsm_transform_ = nullptr;

 private:
  // Target allocation per added key, in thousandths of a bit.
  int millibits_per_key_;
};]=])
