#include "storage/lsm/iterator.h"

#include <algorithm>

#include "storage/lsm/dbformat.h"

namespace cartwheel::storage::lsm {
namespace {

class ErrorIterator final : public InternalIterator {
 public:
  explicit ErrorIterator(Status s) : status_(std::move(s)) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view) override {}
  void Next() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override { return status_; }

 private:
  Status status_;
};

// Min-heap over child iterators by current key. With a heap, each Next() is
// O(log N) in the number of children (memtables + L0 files + one per level).
class MergingIterator final : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return !heap_.empty(); }
  std::string_view key() const override { return heap_.front()->key(); }
  std::string_view value() const override { return heap_.front()->value(); }

  Status status() const override {
    for (const auto& c : children_) {
      if (Status s = c->status(); !s.ok()) return s;
    }
    return Status::Ok();
  }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    Rebuild();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    Rebuild();
  }

  void Next() override {
    std::pop_heap(heap_.begin(), heap_.end(), Greater{});
    InternalIterator* top = heap_.back();
    top->Next();
    if (top->Valid()) {
      std::push_heap(heap_.begin(), heap_.end(), Greater{});
    } else {
      heap_.pop_back();
    }
  }

 private:
  struct Greater {
    bool operator()(const InternalIterator* a, const InternalIterator* b) const {
      return CompareInternalKey(a->key(), b->key()) > 0;
    }
  };

  void Rebuild() {
    heap_.clear();
    for (auto& c : children_) {
      if (c->Valid()) heap_.push_back(c.get());
    }
    std::make_heap(heap_.begin(), heap_.end(), Greater{});
  }

  std::vector<std::unique_ptr<InternalIterator>> children_;
  std::vector<InternalIterator*> heap_;
};

}  // namespace

std::unique_ptr<InternalIterator> NewMergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children) {
  if (children.size() == 1) return std::move(children.front());
  return std::make_unique<MergingIterator>(std::move(children));
}

std::unique_ptr<InternalIterator> NewErrorIterator(Status s) {
  return std::make_unique<ErrorIterator>(std::move(s));
}

}  // namespace cartwheel::storage::lsm
