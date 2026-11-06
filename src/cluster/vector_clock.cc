#include "cluster/vector_clock.h"

#include <algorithm>

namespace cartwheel::cluster {
namespace {

auto LowerBound(std::vector<VectorClock::Entry>& v, std::string_view node) {
  return std::lower_bound(
      v.begin(), v.end(), node,
      [](const VectorClock::Entry& e, std::string_view n) { return e.first < n; });
}

}  // namespace

VectorClock::VectorClock(std::initializer_list<Entry> entries) {
  for (const auto& [node, counter] : entries) Set(node, counter);
}

uint64_t VectorClock::Get(std::string_view node) const noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), node,
                                   [](const Entry& e, std::string_view n) { return e.first < n; });
  return (it != entries_.end() && it->first == node) ? it->second : 0;
}

void VectorClock::Set(std::string_view node, uint64_t counter) {
  auto it = LowerBound(entries_, node);
  const bool present = it != entries_.end() && it->first == node;
  if (counter == 0) {
    if (present) entries_.erase(it);
    return;
  }
  if (present) {
    it->second = counter;
  } else {
    entries_.emplace(it, std::string(node), counter);
  }
}

void VectorClock::Advance(std::string_view node, uint64_t counter) {
  if (counter > Get(node)) Set(node, counter);
}

void VectorClock::Merge(const VectorClock& other) {
  // Linear merge of two sorted vectors.
  std::vector<Entry> out;
  out.reserve(entries_.size() + other.entries_.size());
  auto a = entries_.begin();
  auto b = other.entries_.begin();
  while (a != entries_.end() || b != other.entries_.end()) {
    if (b == other.entries_.end() || (a != entries_.end() && a->first < b->first)) {
      out.push_back(*a++);
    } else if (a == entries_.end() || b->first < a->first) {
      out.push_back(*b++);
    } else {
      out.emplace_back(a->first, std::max(a->second, b->second));
      ++a;
      ++b;
    }
  }
  entries_ = std::move(out);
}

bool VectorClock::Descends(const VectorClock& other) const noexcept {
  return std::all_of(other.entries_.begin(), other.entries_.end(),
                     [this](const Entry& e) { return Get(e.first) >= e.second; });
}

VectorClock::Order VectorClock::Compare(const VectorClock& a, const VectorClock& b) noexcept {
  const bool a_ge = a.Descends(b);
  const bool b_ge = b.Descends(a);
  if (a_ge && b_ge) return Order::kEqual;
  if (a_ge) return Order::kAfter;
  if (b_ge) return Order::kBefore;
  return Order::kConcurrent;
}

void VectorClock::EncodeTo(std::string* dst) const {
  PutVarint64(dst, entries_.size());
  for (const auto& [node, counter] : entries_) {
    PutLengthPrefixed(dst, node);
    PutVarint64(dst, counter);
  }
}

std::string VectorClock::Encode() const {
  std::string out;
  EncodeTo(&out);
  return out;
}

bool VectorClock::DecodeFrom(Decoder* in) {
  uint64_t n = 0;
  if (!in->GetVarint64(&n) || n > in->remaining()) return false;
  std::vector<Entry> entries;
  entries.reserve(static_cast<size_t>(n));
  for (uint64_t i = 0; i < n; ++i) {
    std::string_view node;
    uint64_t counter = 0;
    if (!in->GetLengthPrefixed(&node) || !in->GetVarint64(&counter)) return false;
    // Enforce the canonical form so equal clocks stay byte-identical.
    if (counter == 0 || (!entries.empty() && entries.back().first >= node)) return false;
    entries.emplace_back(std::string(node), counter);
  }
  entries_ = std::move(entries);
  return true;
}

Result<VectorClock> VectorClock::Decode(std::string_view in) {
  VectorClock clock;
  if (in.empty()) return clock;
  Decoder d(in);
  if (!clock.DecodeFrom(&d) || !d.empty()) return Status::Corruption("malformed vector clock");
  return clock;
}

std::string VectorClock::ToString() const {
  std::string out = "{";
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (i > 0) out += ", ";
    out += entries_[i].first;
    out += ':';
    out += std::to_string(entries_[i].second);
  }
  out += '}';
  return out;
}

}  // namespace cartwheel::cluster
