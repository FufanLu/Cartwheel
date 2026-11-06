#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "storage/engine.h"

namespace cartwheel::storage {
namespace {

// Iterates over a point-in-time copy. Fine for the reference engine; the LSM
// engine iterates over immutable memtables and SSTables instead.
class SnapshotIterator final : public Iterator {
 public:
  explicit SnapshotIterator(std::vector<std::pair<std::string, std::string>> rows)
      : rows_(std::move(rows)), pos_(rows_.size()) {}

  bool Valid() const override { return pos_ < rows_.size(); }
  void SeekToFirst() override { pos_ = 0; }
  void Seek(std::string_view target) override {
    pos_ = static_cast<size_t>(
        std::lower_bound(rows_.begin(), rows_.end(), target,
                         [](const auto& row, std::string_view t) { return row.first < t; }) -
        rows_.begin());
  }
  void Next() override { ++pos_; }
  std::string_view key() const override { return rows_[pos_].first; }
  std::string_view value() const override { return rows_[pos_].second; }
  Status status() const override { return Status::Ok(); }

 private:
  std::vector<std::pair<std::string, std::string>> rows_;
  size_t pos_;
};

class MemEngine final : public Engine {
 public:
  Status Put(std::string_view key, std::string_view value, const WriteOptions& /*opts*/) override {
    std::unique_lock lock(mu_);
    auto [it, inserted] = map_.try_emplace(std::string(key));
    if (!inserted)
      bytes_ -= it->second.size();
    else
      bytes_ += key.size();
    it->second.assign(value);
    bytes_ += value.size();
    return Status::Ok();
  }

  Status Delete(std::string_view key, const WriteOptions& /*opts*/) override {
    std::unique_lock lock(mu_);
    if (auto it = map_.find(key); it != map_.end()) {
      bytes_ -= it->first.size() + it->second.size();
      map_.erase(it);
    }
    return Status::Ok();
  }

  Result<std::string> Get(std::string_view key) const override {
    std::shared_lock lock(mu_);
    if (auto it = map_.find(key); it != map_.end()) return it->second;
    return Status::NotFound();
  }

  std::unique_ptr<Iterator> NewIterator() const override {
    std::shared_lock lock(mu_);
    return std::make_unique<SnapshotIterator>(
        std::vector<std::pair<std::string, std::string>>(map_.begin(), map_.end()));
  }

  EngineStats Stats() const override {
    std::shared_lock lock(mu_);
    return {.keys = map_.size(), .bytes = bytes_};
  }

  std::string_view Name() const override { return "mem"; }

 private:
  mutable std::shared_mutex mu_;
  std::map<std::string, std::string, std::less<>> map_;
  uint64_t bytes_ = 0;
};

}  // namespace

std::unique_ptr<Engine> NewMemEngine() {
  return std::make_unique<MemEngine>();
}

}  // namespace cartwheel::storage
