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
lsm_replace("table/block_based/block_based_table_reader.h"
  "  friend class TableCache;"
  "  friend class TableCache;\n  friend class BlockBasedTableIterator;")
lsm_replace("table/block_based/block_based_table_iterator.h"
  "  enum class IterDirection {"
  "  Status InitCompactionBatch(const BlockHandle& handle);\n  std::deque<std::pair<uint64_t, std::string>> compaction_batch_;\n  std::unique_ptr<InternalIteratorBase<IndexValue>> compaction_batch_index_;\n\n  enum class IterDirection {")
lsm_replace("table/block_based/block_based_table_iterator.cc"
  "#include \"monitoring/perf_context_imp.h\""
  "#include \"monitoring/perf_context_imp.h\"\n#include \"util/coding.h\"\n#include \"memory/memory_allocator_impl.h\"")
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
  "void BlockBasedTableIterator::InitDataBlock() {"
  [=[void BlockBasedTableIterator::InitDataBlock() {
  if (lookup_context_.caller == TableReaderCaller::kCompaction &&
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