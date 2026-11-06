#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "common/status.h"
#include "storage/env.h"
#include "storage/lsm/block.h"
#include "storage/lsm/block_cache.h"
#include "storage/lsm/bloom.h"
#include "storage/lsm/dbformat.h"
#include "storage/lsm/memtable.h"

namespace cartwheel::storage::lsm {

struct TableOptions {
  size_t block_size = 4096;
  int restart_interval = 16;
  int bloom_bits_per_key = 10;
};

// SSTable layout (sorted string table, immutable once written):
//
//   [data block 0][trailer] ... [data block n][trailer]
//   [bloom filter][trailer]
//   [index block][trailer]        one entry per data block: last key -> handle
//   [footer: index handle | filter handle | padding | magic]   48 bytes
class TableBuilder {
 public:
  TableBuilder(const TableOptions& options, WritableFile* file);

  void Add(std::string_view internal_key, std::string_view value);  // strictly increasing
  Status Finish();  // writes filter, index, footer; does not sync or close

  [[nodiscard]] uint64_t num_entries() const { return num_entries_; }
  [[nodiscard]] uint64_t FileSize() const { return offset_; }
  [[nodiscard]] const Status& status() const { return status_; }

 private:
  void FlushDataBlock();
  Status WriteBlock(std::string_view contents, BlockHandle* handle);

  const TableOptions options_;
  WritableFile* file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  BloomFilterBuilder filter_;
  std::string last_key_;
  uint64_t num_entries_ = 0;
};

class Table {
 public:
  static Result<std::shared_ptr<Table>> Open(std::unique_ptr<RandomAccessFile> file,
                                             uint64_t file_number, BlockCache* cache);

  // Newest version of lookup.user_key() visible at the lookup's sequence.
  Result<MemTable::GetResult> Get(const LookupKey& lookup, std::string* value) const;

  // Iterator over all internal keys; holds the table alive.
  static std::unique_ptr<InternalIterator> NewIterator(std::shared_ptr<const Table> self);

  [[nodiscard]] bool MayContain(std::string_view user_key) const {
    return BloomMayContain(filter_, user_key);
  }
  [[nodiscard]] uint64_t file_number() const { return file_number_; }
  [[nodiscard]] uint64_t bloom_skips() const {
    return bloom_skips_.load(std::memory_order_relaxed);
  }

  // Loads (via the cache) the data block at `handle_bytes` (an index value).
  Result<std::shared_ptr<const Block>> DataBlock(std::string_view handle_bytes) const;

 private:
  Table() = default;
  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_number_ = 0;
  BlockCache* cache_ = nullptr;
  std::shared_ptr<const Block> index_;
  std::string filter_;
  mutable std::atomic<uint64_t> bloom_skips_{0};
};

}  // namespace cartwheel::storage::lsm
