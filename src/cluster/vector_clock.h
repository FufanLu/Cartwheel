#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/coding.h"
#include "common/status.h"

namespace cartwheel::cluster {

// Version vector: node id -> number of write events that node has
// coordinated for one key. Entries are sorted by node id and counters are
// always > 0, so equal clocks have identical encodings.
class VectorClock {
 public:
  using Entry = std::pair<std::string, uint64_t>;

  enum class Order : uint8_t { kEqual, kBefore, kAfter, kConcurrent };

  VectorClock() = default;
  VectorClock(std::initializer_list<Entry> entries);

  [[nodiscard]] uint64_t Get(std::string_view node) const noexcept;
  // Sets node's counter; 0 removes the entry.
  void Set(std::string_view node, uint64_t counter);
  // Raises node's counter to at least `counter`.
  void Advance(std::string_view node, uint64_t counter);
  // Pointwise maximum: the clock of someone who has seen both.
  void Merge(const VectorClock& other);

  // True if every counter in `other` is <= ours (we have seen all of it).
  [[nodiscard]] bool Descends(const VectorClock& other) const noexcept;
  [[nodiscard]] static Order Compare(const VectorClock& a, const VectorClock& b) noexcept;

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

  void EncodeTo(std::string* dst) const;
  [[nodiscard]] std::string Encode() const;
  [[nodiscard]] bool DecodeFrom(Decoder* in);
  // Decodes a buffer that holds exactly one clock. Empty input is an empty clock.
  [[nodiscard]] static Result<VectorClock> Decode(std::string_view in);

  // "{a:2, b:1}"
  [[nodiscard]] std::string ToString() const;

  friend bool operator==(const VectorClock& a, const VectorClock& b) noexcept {
    return a.entries_ == b.entries_;
  }

 private:
  std::vector<Entry> entries_;
};

}  // namespace cartwheel::cluster
