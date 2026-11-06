#include "cluster/siblings.h"

#include <algorithm>
#include <limits>

#include "common/coding.h"

namespace cartwheel::cluster {
namespace {

constexpr uint8_t kFlagTombstone = 0x1;
constexpr uint8_t kEncodingVersion = 1;

bool DotLess(const Sibling& a, const Sibling& b) {
  return a.dot.node != b.dot.node ? a.dot.node < b.dot.node : a.dot.counter < b.dot.counter;
}

}  // namespace

VectorClock Sibling::Clock() const {
  VectorClock c = context;
  c.Advance(dot.node, dot.counter);
  return c;
}

const Sibling& SiblingSet::Update(std::string_view node, const VectorClock& context,
                                  std::string value, bool tombstone, int64_t now_ms) {
  // The new dot must be unique for this key. It must exceed every dot from
  // `node` this replica holds, and anything from `node` the client has seen.
  const uint64_t counter = std::max(Join().Get(node), context.Get(node)) + 1;

  std::erase_if(siblings_, [&](const Sibling& s) { return s.CoveredBy(context); });
  siblings_.push_back(Sibling{
      .dot = {std::string(node), counter},
      .context = context,
      .value = std::move(value),
      .tombstone = tombstone,
      .written_at_ms = now_ms,
  });
  Normalize();
  const auto it = std::find_if(siblings_.begin(), siblings_.end(), [&](const Sibling& s) {
    return s.dot.node == node && s.dot.counter == counter;
  });
  return *it;
}

void SiblingSet::Sync(const SiblingSet& other) {
  for (const auto& s : other.siblings_) {
    const bool dup = std::any_of(siblings_.begin(), siblings_.end(),
                                 [&](const Sibling& mine) { return mine.dot == s.dot; });
    if (!dup) siblings_.push_back(s);
  }
  Normalize();
}

void SiblingSet::Normalize() {
  // A sibling is obsolete if some *other* sibling's context contains its dot.
  std::vector<Sibling> kept;
  kept.reserve(siblings_.size());
  for (const auto& s : siblings_) {
    const bool obsolete = std::any_of(siblings_.begin(), siblings_.end(), [&](const Sibling& o) {
      return !(o.dot == s.dot) && s.CoveredBy(o.context);
    });
    if (!obsolete) kept.push_back(s);
  }
  std::sort(kept.begin(), kept.end(), DotLess);
  siblings_ = std::move(kept);
}

VectorClock SiblingSet::Join() const {
  VectorClock joined;
  for (const auto& s : siblings_) {
    joined.Merge(s.context);
    joined.Advance(s.dot.node, s.dot.counter);
  }
  return joined;
}

bool SiblingSet::IsDeleted() const noexcept {
  return LiveCount() == 0;
}

size_t SiblingSet::LiveCount() const noexcept {
  return static_cast<size_t>(std::count_if(siblings_.begin(), siblings_.end(),
                                           [](const Sibling& s) { return !s.tombstone; }));
}

// Layout (all integers varint unless noted):
//   u8 version | count | { node(lp) counter context flags(u8) written_at_ms value(lp) }*
void SiblingSet::EncodeTo(std::string* dst) const {
  dst->push_back(static_cast<char>(kEncodingVersion));
  PutVarint64(dst, siblings_.size());
  for (const auto& s : siblings_) {
    PutLengthPrefixed(dst, s.dot.node);
    PutVarint64(dst, s.dot.counter);
    s.context.EncodeTo(dst);
    dst->push_back(static_cast<char>(s.tombstone ? kFlagTombstone : 0));
    PutVarint64(dst, static_cast<uint64_t>(s.written_at_ms));
    PutLengthPrefixed(dst, s.value);
  }
}

std::string SiblingSet::Encode() const {
  std::string out;
  EncodeTo(&out);
  return out;
}

Result<SiblingSet> SiblingSet::Decode(std::string_view in) {
  Decoder d(in);
  std::string_view version;
  if (!d.GetBytes(1, &version) || static_cast<uint8_t>(version[0]) != kEncodingVersion) {
    return Status::Corruption("sibling set: unknown encoding version");
  }
  uint64_t n = 0;
  if (!d.GetVarint64(&n) || n > d.remaining()) return Status::Corruption("sibling set: bad count");
  SiblingSet set;
  set.siblings_.reserve(static_cast<size_t>(n));
  for (uint64_t i = 0; i < n; ++i) {
    Sibling s;
    std::string_view node;
    std::string_view flags;
    std::string_view value;
    uint64_t written = 0;
    if (!d.GetLengthPrefixed(&node) || !d.GetVarint64(&s.dot.counter) ||
        !s.context.DecodeFrom(&d) || !d.GetBytes(1, &flags) || !d.GetVarint64(&written) ||
        !d.GetLengthPrefixed(&value) || s.dot.counter == 0 ||
        written > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return Status::Corruption("sibling set: truncated sibling");
    }
    s.dot.node = std::string(node);
    s.tombstone = (static_cast<uint8_t>(flags[0]) & kFlagTombstone) != 0;
    s.written_at_ms = static_cast<int64_t>(written);
    s.value = std::string(value);
    set.siblings_.push_back(std::move(s));
  }
  if (!d.empty()) return Status::Corruption("sibling set: trailing bytes");
  return set;
}

std::string SiblingSet::ToString() const {
  std::string out = "[";
  for (size_t i = 0; i < siblings_.size(); ++i) {
    const auto& s = siblings_[i];
    if (i > 0) out += " | ";
    out += '(';
    out += s.dot.node + "," + std::to_string(s.dot.counter) + ") ctx=" + s.context.ToString();
    out += s.tombstone ? " <tombstone>" : " '" + s.value + "'";
  }
  out += ']';
  return out;
}

}  // namespace cartwheel::cluster
