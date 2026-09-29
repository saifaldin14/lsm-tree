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
lsm_replace("table/block_based/block_based_table_iterator.cc"
  "  if (lookup_context_.caller == TableReaderCaller::kCompaction &&\n      table_->get_rep()->table_options.compaction_block_transform) {"
  "  if (lookup_context_.caller == TableReaderCaller::kCompaction &&\n      std::getenv(\"LSM_DISABLE_GPU_INPUT\") == nullptr &&\n      table_->get_rep()->table_options.compaction_block_transform) {")
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