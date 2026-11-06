#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>
#include <random>
#include <vector>

#include "storage/lsm/arena.h"

namespace cartwheel::storage::lsm {

// Concurrent skiplist with one writer and any number of lock-free readers
// (the LevelDB design).
//
// - Inserts are externally serialised. In the DB only the group-commit leader
//   writes the memtable.
// - Readers take no locks. A node is fully initialised before it is published
//   with a release store into its predecessor's next pointer, and readers
//   follow pointers with acquire loads, so a reader always sees a complete
//   node.
// - Nodes are never deleted while the list is alive. The arena frees them all
//   together.
//
// Expected cost is O(log n) for both insert and search: each level keeps
// about 1/4 of the nodes of the level below (kBranching = 4).
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  static constexpr int kMaxHeight = 12;

  SkipList(Comparator cmp, Arena* arena);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: no equal key is present; external synchronisation among writers.
  void Insert(const Key& key);
  [[nodiscard]] bool Contains(const Key& key) const;

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}
    [[nodiscard]] bool Valid() const { return node_ != nullptr; }
    [[nodiscard]] const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

  // Height distribution, exposed for the illustrated docs.
  [[nodiscard]] int max_height() const { return max_height_.load(std::memory_order_relaxed); }
  // Nodes linked at each level (level 0 = every node). Walks the list.
  [[nodiscard]] std::vector<size_t> LevelCounts() const {
    std::vector<size_t> counts(static_cast<size_t>(max_height()), 0);
    for (int level = 0; level < max_height(); ++level) {
      for (Node* x = head_->Next(level); x != nullptr; x = x->Next(level))
        ++counts[static_cast<size_t>(level)];
    }
    return counts;
  }

 private:
  struct Node {
    Node(const Key& k, std::atomic<Node*>* links) : key(k), next_(links) {}
    Key key;
    Node* Next(int n) { return next_[n].load(std::memory_order_acquire); }
    void SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_release); }
    Node* NoBarrierNext(int n) { return next_[n].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }
    // Points at the node's tower of `height` links, allocated right after it
    // in the arena. An explicit pointer, rather than the classic
    // `next_[1]` over-allocation, keeps UBSan's bounds checks meaningful.
    std::atomic<Node*>* next_;
  };

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  [[nodiscard]] bool Equal(const Key& a, const Key& b) const { return cmp_(a, b) == 0; }
  [[nodiscard]] bool KeyIsAfterNode(const Key& key, Node* n) const {
    return n != nullptr && cmp_(n->key, key) < 0;
  }
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  Comparator const cmp_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_{1};
  // Writer-only. A fixed seed makes tower heights reproducible run to run;
  // heights need balance, not unpredictability.
  std::minstd_rand rnd_{0xdeadbeef};  // NOLINT(bugprone-random-generator-seed)
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                             int height) {
  static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0);
  char* mem = arena_->AllocateAligned(sizeof(Node) +
                                      sizeof(std::atomic<Node*>) * static_cast<size_t>(height));
  auto* links = reinterpret_cast<std::atomic<Node*>*>(mem + sizeof(Node));
  for (int i = 0; i < height; ++i) new (&links[i]) std::atomic<Node*>(nullptr);
  return new (mem) Node(key, links);
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : cmp_(cmp), arena_(arena), head_(NewNode(Key{}, kMaxHeight)) {
  for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  constexpr unsigned kBranching = 4;
  int height = 1;
  while (height < kMaxHeight && rnd_() % kBranching == 0) ++height;
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  Node* x = head_;
  int level = max_height() - 1;
  for (;;) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // keep moving right on this level
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;  // drop down a level
    }
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  assert(x == nullptr || !Equal(key, x->key));
  const int height = RandomHeight();
  if (height > max_height()) {
    for (int i = max_height(); i < height; ++i) prev[i] = head_;
    // Readers that see the new height before the new node simply find null
    // pointers in head_ at those levels and drop down: harmless.
    max_height_.store(height, std::memory_order_relaxed);
  }
  x = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    // The node's own pointers need no barrier: it is not published yet.
    x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
    prev[i]->SetNext(i, x);  // publish with release
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(key, x->key);
}

}  // namespace cartwheel::storage::lsm
