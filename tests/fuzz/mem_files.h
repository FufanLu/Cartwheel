#pragma once
// In-memory files so fuzz targets never touch the disk.
#include <algorithm>
#include <cstring>
#include <string>

#include "storage/env.h"

namespace cartwheel::fuzz {

class MemSequentialFile final : public storage::SequentialFile {
 public:
  explicit MemSequentialFile(std::string data) : data_(std::move(data)) {}
  Status Read(size_t n, std::string_view* result, char* scratch) override {
    const size_t k = std::min(n, data_.size() - pos_);
    std::memcpy(scratch, data_.data() + pos_, k);
    pos_ += k;
    *result = std::string_view(scratch, k);
    return Status::Ok();
  }
  Status Skip(uint64_t n) override {
    pos_ = std::min(data_.size(), pos_ + static_cast<size_t>(n));
    return Status::Ok();
  }

 private:
  std::string data_;
  size_t pos_ = 0;
};

class MemRandomAccessFile final : public storage::RandomAccessFile {
 public:
  explicit MemRandomAccessFile(std::string data) : data_(std::move(data)) {}
  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    if (offset > data_.size()) {
      *result = {};
      return Status::Ok();
    }
    const size_t k = std::min(n, data_.size() - static_cast<size_t>(offset));
    std::memcpy(scratch, data_.data() + offset, k);
    *result = std::string_view(scratch, k);
    return Status::Ok();
  }
  uint64_t Size() const override { return data_.size(); }

 private:
  std::string data_;
};

}  // namespace cartwheel::fuzz
