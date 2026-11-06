#include "storage/lsm/block.h"

#include <algorithm>
#include <cassert>

#include "common/coding.h"
#include "storage/crc32c.h"
#include "storage/lsm/dbformat.h"

namespace cartwheel::storage::lsm {

// ---- handles, footer, block IO ----------------------------------------------

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

bool BlockHandle::DecodeFrom(Decoder* in) {
  return in->GetVarint64(&offset) && in->GetVarint64(&size);
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t start = dst->size();
  index.EncodeTo(dst);
  filter.EncodeTo(dst);
  dst->resize(start + kEncodedLength - 8, '\0');
  PutFixed64(dst, kMagic);
}

Status Footer::DecodeFrom(std::string_view in) {
  if (in.size() != kEncodedLength) return Status::Corruption("sst: bad footer size");
  if (DecodeFixed64(in.data() + kEncodedLength - 8) != kMagic) {
    return Status::Corruption("sst: bad magic number (not a Cartwheel table)");
  }
  Decoder d(in.substr(0, kEncodedLength - 8));
  if (!index.DecodeFrom(&d) || !filter.DecodeFrom(&d)) {
    return Status::Corruption("sst: bad block handles in footer");
  }
  return Status::Ok();
}

Result<std::string> ReadBlock(const RandomAccessFile& file, const BlockHandle& handle) {
  const size_t n = static_cast<size_t>(handle.size);
  std::string buf(n + kBlockTrailerSize, '\0');
  std::string_view got;
  CW_RETURN_IF_ERROR(file.Read(handle.offset, buf.size(), &got, buf.data()));
  if (got.size() != buf.size()) return Status::Corruption("sst: truncated block read");
  // NOLINTBEGIN(bugprone-suspicious-stringview-data-usage): got.size() == n + 5 was checked above
  const uint32_t expected = crc32c::Unmask(DecodeFixed32(got.data() + n + 1));
  const uint32_t actual = crc32c::Value(got.data(), n + 1);
  if (expected != actual) return Status::Corruption("sst: block checksum mismatch");
  if (got[n] != 0) return Status::Corruption("sst: unsupported block compression");
  if (got.data() != buf.data()) buf.assign(got.data(), n);
  // NOLINTEND(bugprone-suspicious-stringview-data-usage)
  buf.resize(n);
  return buf;
}

// ---- builder ------------------------------------------------------------------

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
  restarts_.push_back(0);
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  size_t shared = 0;
  if (counter_ < restart_interval_) {
    const size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) ++shared;
  } else {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t unshared = key.size() - shared;
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(unshared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, unshared);
  buffer_.append(value.data(), value.size());
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, unshared);
  ++counter_;
}

std::string_view BlockBuilder::Finish() {
  for (const uint32_t r : restarts_) PutFixed32(&buffer_, r);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

// ---- reader ---------------------------------------------------------------------

Block::Block(std::string contents) : data_(std::move(contents)) {
  if (data_.size() < sizeof(uint32_t)) return;
  const uint32_t n = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
  const size_t max_restarts = (data_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  if (n > max_restarts) return;  // corrupt: ok() stays false
  num_restarts_ = n;
  restart_offset_ = static_cast<uint32_t>(data_.size() - (1 + n) * sizeof(uint32_t));
}

class BlockIterator final : public InternalIterator {
 public:
  explicit BlockIterator(std::shared_ptr<const Block> block)
      : block_(std::move(block)),
        data_(block_->data_.data()),
        restarts_(block_->restart_offset_),
        num_restarts_(block_->num_restarts_),
        current_(restarts_) {
    if (num_restarts_ == 0 && block_->data_.size() != sizeof(uint32_t)) {
      status_ = Status::Corruption("sst: bad block contents");
    }
  }

  bool Valid() const override { return current_ < restarts_; }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return status_; }

  void Next() override { ParseNextEntry(); }

  void SeekToFirst() override {
    if (num_restarts_ == 0) {
      current_ = restarts_;  // empty or corrupt block: nothing to iterate
      return;
    }
    SeekToRestart(0);
    ParseNextEntry();
  }

  void Seek(std::string_view target) override {
    if (num_restarts_ == 0) {
      current_ = restarts_;
      return;
    }
    // Binary search for the last restart point whose key is < target.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      std::string_view mid_key;
      if (!RestartKey(mid, &mid_key)) return;
      if (CompareInternalKey(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }
    // Then scan linearly to the first key >= target.
    SeekToRestart(left);
    for (;;) {
      if (!ParseNextEntry()) return;
      if (CompareInternalKey(key_, target) >= 0) return;
    }
  }

 private:
  [[nodiscard]] uint32_t RestartPoint(uint32_t i) const {
    return DecodeFixed32(data_ + restarts_ + i * sizeof(uint32_t));
  }

  void SeekToRestart(uint32_t i) {
    key_.clear();
    next_ = RestartPoint(i);  // ParseNextEntry() bounds-checks it against restarts_
  }

  // Key stored whole at restart point i (shared == 0 by construction).
  bool RestartKey(uint32_t i, std::string_view* out) {
    const uint32_t offset = RestartPoint(i);
    if (offset >= restarts_) {  // corrupt restart array (found by block_fuzz)
      Corrupt();
      return false;
    }
    Decoder d(std::string_view(data_ + offset, restarts_ - offset));
    uint32_t shared = 0;
    uint32_t unshared = 0;
    uint32_t vlen = 0;
    std::string_view k;
    if (!d.GetVarint32(&shared) || !d.GetVarint32(&unshared) || !d.GetVarint32(&vlen) ||
        shared != 0 || !d.GetBytes(unshared, &k)) {
      Corrupt();
      return false;
    }
    *out = k;
    return true;
  }

  bool ParseNextEntry() {
    current_ = next_;
    if (current_ >= restarts_) {
      current_ = restarts_;  // end of block
      return false;
    }
    Decoder d(std::string_view(data_ + current_, restarts_ - current_));
    uint32_t shared = 0;
    uint32_t unshared = 0;
    uint32_t vlen = 0;
    std::string_view delta;
    std::string_view v;
    if (!d.GetVarint32(&shared) || !d.GetVarint32(&unshared) || !d.GetVarint32(&vlen) ||
        shared > key_.size() || !d.GetBytes(unshared, &delta) || !d.GetBytes(vlen, &v)) {
      Corrupt();
      return false;
    }
    key_.resize(shared);
    key_.append(delta);
    value_ = v;
    next_ = restarts_ - static_cast<uint32_t>(d.remaining());
    return true;
  }

  void Corrupt() {
    current_ = restarts_;
    status_ = Status::Corruption("sst: bad entry in block");
  }

  std::shared_ptr<const Block> block_;
  const char* data_;
  const uint32_t restarts_;
  const uint32_t num_restarts_;
  uint32_t current_;
  uint32_t next_ = 0;
  std::string key_;
  std::string_view value_;
  Status status_;
};

std::unique_ptr<InternalIterator> Block::NewIterator(std::shared_ptr<const Block> self) {
  return std::make_unique<BlockIterator>(std::move(self));
}

}  // namespace cartwheel::storage::lsm
