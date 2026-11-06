#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/vector_clock.h"
#include "common/status.h"

namespace cartwheel::cluster {

// Causality tracking with Dotted Version Vectors (Preguiça et al., 2010;
// the scheme Riak 2.0 adopted).
//
// The problem with plain per-node vector clocks (Dynamo §4.4): when two clients
// read the same version and both write through the same coordinator, the
// coordinator increments its own entry twice. The second write gets clock
// {n:3}, which "descends" the first one's {n:2}, so the first write is silently
// dropped even though neither client saw the other's write.
//
// DVV fixes this by giving each write a *dot*, (node, counter), the unique
// name of that one event, separate from its *causal context*, the clock the
// client had read. A version is obsolete only if another version's context
// contains its dot. Comparing collapsed clocks is not enough. The two writes
// above become ((n,2), {n:1}) and ((n,3), {n:1}). Neither context contains
// the other's dot, so both survive as siblings.

struct Dot {
  std::string node;
  uint64_t counter = 0;
  friend bool operator==(const Dot&, const Dot&) = default;
};

struct Sibling {
  Dot dot;
  VectorClock context;  // what the writer had seen when it wrote
  std::string value;
  bool tombstone = false;
  int64_t written_at_ms = 0;

  // context ∪ {dot}: everything a reader of this sibling has observed.
  [[nodiscard]] VectorClock Clock() const;
  // True if `clock` includes this sibling's write event.
  [[nodiscard]] bool CoveredBy(const VectorClock& clock) const noexcept {
    return clock.Get(dot.node) >= dot.counter;
  }
};

// All concurrent versions of one key held by one replica.
//
// Invariant: no sibling's dot is contained in another sibling's context.
//
// Replication must ship the whole set, not single siblings (as Riak does). A
// read context is the join of the siblings' clocks, and that join is only safe
// if a replica holding dot (n,k) also holds, or has seen superseded, every
// earlier concurrent dot from n.
class SiblingSet {
 public:
  // Coordinator write at `node` on behalf of a client that read `context`.
  // Drops every sibling the client had seen (dot covered by context), then
  // adds the new version with a fresh dot. Returns the new sibling.
  const Sibling& Update(std::string_view node, const VectorClock& context, std::string value,
                        bool tombstone, int64_t now_ms);

  // Anti-entropy and replication: union with a remote replica's set, then
  // remove anything a surviving context has seen. Commutative, associative,
  // and idempotent, so replicas converge regardless of delivery order.
  void Sync(const SiblingSet& other);

  // Read context: join of all siblings' clocks.
  [[nodiscard]] VectorClock Join() const;

  [[nodiscard]] const std::vector<Sibling>& siblings() const noexcept { return siblings_; }
  [[nodiscard]] bool empty() const noexcept { return siblings_.empty(); }
  // No live value: empty, or only tombstones remain.
  [[nodiscard]] bool IsDeleted() const noexcept;
  [[nodiscard]] size_t LiveCount() const noexcept;

  void EncodeTo(std::string* dst) const;
  [[nodiscard]] std::string Encode() const;
  [[nodiscard]] static Result<SiblingSet> Decode(std::string_view in);

  // "[(n1,2) ctx={n1:1} 'v' | (n1,3) ctx={n1:1} 'w']"
  [[nodiscard]] std::string ToString() const;

 private:
  void Normalize();  // enforce the invariant; deterministic order by dot

  std::vector<Sibling> siblings_;
};

}  // namespace cartwheel::cluster
