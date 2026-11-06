#include "storage/lsm/table.h"

#include <cassert>

#include "common/coding.h"
#include "storage/crc32c.h"

namespace cartwheel::storage::lsm {

// ---- builder ----------------------------------------------------------------

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.restart_interval),
      index_block_(1),  // index keys are few and diverse: no prefix sharing needed
      filter_(options.bloom_bits_per_key) {}

void TableBuilder::Add(std::string_view internal_key, std::string_view value) {
  if (!status_.ok()) return;
  assert(last_key_.empty() || CompareInternalKey(last_key_, internal_key) < 0);
  filter_.AddKey(ExtractUserKey(internal_key));
  data_block_.Add(internal_key, value);
  last_key_.assign(internal_key);
  ++num_entries_;
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) FlushDataBlock();
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty() || !status_.ok()) return;
  BlockHandle handle;
  status_ = WriteBlock(data_block_.Finish(), &handle);
  data_block_.Reset();
  if (!status_.ok()) return;
  // Index entry: the block's last key maps to its handle. A seek for k picks
  // the first index entry >= k, which is the only block that can hold k.
  std::string encoded;
  handle.EncodeTo(&encoded);
  index_block_.Add(last_key_, encoded);
}

Status TableBuilder::WriteBlock(std::string_view contents, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = contents.size();
  char trailer[kBlockTrailerSize];
  trailer[0] = 0;  // no compression
  uint32_t crc = crc32c::Value(contents.data(), contents.size());
  crc = crc32c::Extend(crc, trailer, 1);
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  CW_RETURN_IF_ERROR(file_->Append(contents));
  CW_RETURN_IF_ERROR(file_->Append(std::string_view(trailer, kBlockTrailerSize)));
  offset_ += contents.size() + kBlockTrailerSize;
  return Status::Ok();
}

Status TableBuilder::Finish() {
  FlushDataBlock();
  CW_RETURN_IF_ERROR(status_);
  Footer footer;
  const std::string filter = filter_.Finish();
  CW_RETURN_IF_ERROR(WriteBlock(filter, &footer.filter));
  CW_RETURN_IF_ERROR(WriteBlock(index_block_.Finish(), &footer.index));
  std::string encoded;
  footer.EncodeTo(&encoded);
  CW_RETURN_IF_ERROR(file_->Append(encoded));
  offset_ += encoded.size();
  return file_->Flush();
}

// ---- reader --------------------------------------------------------------------

Result<std::shared_ptr<Table>> Table::Open(std::unique_ptr<RandomAccessFile> file,
                                           uint64_t file_number, BlockCache* cache) {
  const uint64_t size = file->Size();
  if (size < Footer::kEncodedLength) return Status::Corruption("sst: file too short");
  char buf[Footer::kEncodedLength];
  std::string_view got;
  CW_RETURN_IF_ERROR(file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &got, buf));
  Footer footer;
  CW_RETURN_IF_ERROR(footer.DecodeFrom(got));

  auto table = std::shared_ptr<Table>(new Table());
  CW_ASSIGN_OR_RETURN(std::string index_bytes, ReadBlock(*file, footer.index));
  table->index_ = std::make_shared<const Block>(std::move(index_bytes));
  CW_ASSIGN_OR_RETURN(table->filter_, ReadBlock(*file, footer.filter));
  table->file_ = std::move(file);
  table->file_number_ = file_number;
  table->cache_ = cache;
  return table;
}

Result<std::shared_ptr<const Block>> Table::DataBlock(std::string_view handle_bytes) const {
  Decoder d(handle_bytes);
  BlockHandle handle;
  if (!handle.DecodeFrom(&d)) return Status::Corruption("sst: bad index entry");
  if (cache_ != nullptr) {
    if (auto hit = cache_->Lookup(file_number_, handle.offset)) return hit;
  }
  CW_ASSIGN_OR_RETURN(std::string contents, ReadBlock(*file_, handle));
  auto block = std::make_shared<const Block>(std::move(contents));
  if (cache_ != nullptr) cache_->Insert(file_number_, handle.offset, block);
  return block;
}

Result<MemTable::GetResult> Table::Get(const LookupKey& lookup, std::string* value) const {
  if (!MayContain(lookup.user_key())) {
    bloom_skips_.fetch_add(1, std::memory_order_relaxed);
    return MemTable::GetResult::kNotFound;
  }
  auto index_it = Block::NewIterator(index_);
  index_it->Seek(lookup.internal_key());
  CW_RETURN_IF_ERROR(index_it->status());
  if (!index_it->Valid()) return MemTable::GetResult::kNotFound;
  CW_ASSIGN_OR_RETURN(auto block, DataBlock(index_it->value()));
  auto it = Block::NewIterator(block);
  it->Seek(lookup.internal_key());
  CW_RETURN_IF_ERROR(it->status());
  if (!it->Valid()) return MemTable::GetResult::kNotFound;
  ParsedInternalKey parsed;
  if (!ParseInternalKey(it->key(), &parsed)) return Status::Corruption("sst: bad internal key");
  if (parsed.user_key != lookup.user_key()) return MemTable::GetResult::kNotFound;
  if (parsed.type == ValueType::kDeletion) return MemTable::GetResult::kDeleted;
  value->assign(it->value());
  return MemTable::GetResult::kFound;
}

namespace {

// Walks the index block and, for each entry, the data block it points to.
class TableIterator final : public InternalIterator {
 public:
  explicit TableIterator(std::shared_ptr<const Table> table, std::shared_ptr<const Block> index)
      : table_(std::move(table)), index_it_(Block::NewIterator(std::move(index))) {}

  bool Valid() const override { return data_it_ != nullptr && data_it_->Valid(); }
  std::string_view key() const override { return data_it_->key(); }
  std::string_view value() const override { return data_it_->value(); }
  Status status() const override {
    if (!status_.ok()) return status_;
    if (!index_it_->status().ok()) return index_it_->status();
    return data_it_ != nullptr ? data_it_->status() : Status::Ok();
  }

  void SeekToFirst() override {
    index_it_->SeekToFirst();
    LoadBlock();
    if (data_it_) data_it_->SeekToFirst();
    SkipEmptyBlocks();
  }

  void Seek(std::string_view target) override {
    index_it_->Seek(target);
    LoadBlock();
    if (data_it_) data_it_->Seek(target);
    SkipEmptyBlocks();
  }

  void Next() override {
    data_it_->Next();
    SkipEmptyBlocks();
  }

 private:
  void LoadBlock() {
    data_it_.reset();
    if (!index_it_->Valid()) return;
    auto block = table_->DataBlock(index_it_->value());
    if (!block.ok()) {
      status_ = block.status();
      return;
    }
    data_it_ = Block::NewIterator(std::move(block).value());
  }

  void SkipEmptyBlocks() {
    while (data_it_ == nullptr || !data_it_->Valid()) {
      if (data_it_ != nullptr && !data_it_->status().ok()) return;
      if (!index_it_->Valid() || !status_.ok()) {
        data_it_.reset();
        return;
      }
      index_it_->Next();
      LoadBlock();
      if (data_it_) data_it_->SeekToFirst();
    }
  }

  std::shared_ptr<const Table> table_;
  std::unique_ptr<InternalIterator> index_it_;
  std::unique_ptr<InternalIterator> data_it_;
  Status status_;
};

}  // namespace

std::unique_ptr<InternalIterator> Table::NewIterator(std::shared_ptr<const Table> self) {
  auto index = self->index_;
  return std::make_unique<TableIterator>(std::move(self), std::move(index));
}

}  // namespace cartwheel::storage::lsm
