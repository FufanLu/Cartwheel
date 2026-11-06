#include "cluster/ring.h"

#include <algorithm>
#include <limits>

#include <fmt/format.h>

namespace cartwheel::cluster {

uint64_t RingHash(std::string_view data) noexcept {
  uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
  for (const char c : data) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;
  }
  // murmur3 fmix64: spreads FNV's weak low-bit mixing across all 64 bits.
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

Ring::Ring(int vnodes_per_node) : vnodes_(vnodes_per_node) {}

void Ring::AddNode(const std::string& node) {
  if (nodes_.insert(node).second) Rebuild();
}

void Ring::RemoveNode(const std::string& node) {
  if (nodes_.erase(node) > 0) Rebuild();
}

void Ring::Rebuild() {
  tokens_.clear();
  tokens_.reserve(nodes_.size() * static_cast<size_t>(vnodes_));
  for (const auto& node : nodes_) {
    for (int i = 0; i < vnodes_; ++i)
      tokens_.push_back({RingHash(fmt::format("{}#{}", node, i)), node});
  }
  std::sort(tokens_.begin(), tokens_.end(), [](const Token& a, const Token& b) {
    return a.position != b.position ? a.position < b.position : a.node < b.node;
  });
}

size_t Ring::TokenIndexFor(uint64_t hash) const {
  // First token at or after the hash, wrapping around the ring.
  auto it = std::lower_bound(tokens_.begin(), tokens_.end(), hash,
                             [](const Token& t, uint64_t h) { return t.position < h; });
  if (it == tokens_.end()) return 0;
  return static_cast<size_t>(it - tokens_.begin());
}

std::vector<std::string> Ring::WalkFrom(size_t start, size_t n,
                                        const std::set<std::string>& skip) const {
  std::vector<std::string> out;
  if (tokens_.empty()) return out;
  for (size_t step = 0; step < tokens_.size() && out.size() < n; ++step) {
    const std::string& node = tokens_[(start + step) % tokens_.size()].node;
    if (skip.contains(node)) continue;
    if (std::find(out.begin(), out.end(), node) == out.end()) out.push_back(node);
  }
  return out;
}

std::vector<std::string> Ring::PreferenceList(std::string_view key, size_t n,
                                              const std::set<std::string>& skip) const {
  return WalkFrom(TokenIndexFor(RingHash(key)), n, skip);
}

uint32_t Ring::PartitionOf(std::string_view key) const {
  return static_cast<uint32_t>(TokenIndexFor(RingHash(key)));
}

std::vector<std::string> Ring::PartitionReplicas(uint32_t partition, size_t n) const {
  return WalkFrom(partition, n, {});
}

std::map<std::string, double> Ring::Ownership() const {
  std::map<std::string, double> share;
  if (tokens_.empty()) return share;
  constexpr double kSpace = static_cast<double>(std::numeric_limits<uint64_t>::max());
  for (size_t i = 0; i < tokens_.size(); ++i) {
    // Token i owns (previous token, token i].
    const uint64_t prev = tokens_[(i + tokens_.size() - 1) % tokens_.size()].position;
    const uint64_t span = tokens_[i].position - prev;  // unsigned wrap handles the first token
    share[tokens_[i].node] += static_cast<double>(span) / kSpace;
  }
  return share;
}

}  // namespace cartwheel::cluster
