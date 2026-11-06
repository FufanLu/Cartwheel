#include "cluster/membership.h"

#include <algorithm>

namespace cartwheel::cluster {

Membership::Membership(NodeId self, std::string address, uint64_t incarnation, Runtime rt,
                       MembershipOptions options)
    : self_(std::move(self)), rt_(rt), options_(options), rng_(rt.seed ^ 0x6d656d62ULL) {
  Entry me;
  me.member.set_node_id(self_);
  me.member.set_address(std::move(address));
  me.member.set_incarnation(incarnation);
  me.member.set_heartbeat(0);
  me.member.set_state(v1::Member::STATE_ALIVE);
  me.last_heard_ns = rt_.clock->NowNanos();
  members_.emplace(self_, std::move(me));
}

void Membership::AddSeed(const NodeId& id, const std::string& address) {
  if (id == self_) return;
  std::lock_guard lock(mu_);
  if (members_.contains(id)) return;
  Entry e;
  e.member.set_node_id(id);
  e.member.set_address(address);
  e.member.set_state(v1::Member::STATE_ALIVE);
  // A seed counts as alive until its heartbeat has had time to arrive.
  e.last_heard_ns = rt_.clock->NowNanos();
  members_.emplace(id, std::move(e));
}

void Membership::SetListener(Listener l) {
  std::lock_guard lock(mu_);
  listener_ = std::move(l);
}

void Membership::Start() {
  {
    std::lock_guard lock(mu_);
    running_ = true;
  }
  rt_.scheduler->After(options_.gossip_interval_ns, [this] { Tick(); });
}

void Membership::Stop() {
  std::lock_guard lock(mu_);
  running_ = false;
}

v1::GossipRequest Membership::Digest() const {
  v1::GossipRequest req;
  for (const auto& [id, e] : members_) *req.add_members() = e.member;
  return req;
}

void Membership::Tick() {
  std::vector<NodeId> peers;
  std::string payload;
  std::vector<std::pair<NodeId, v1::Member::State>> changes;
  {
    std::lock_guard lock(mu_);
    if (!running_) return;
    auto& me = members_.at(self_).member;
    me.set_heartbeat(me.heartbeat() + 1);
    Evaluate(&changes);
    for (const auto& [id, e] : members_) {
      // Gossip to dead nodes too (less often in big clusters): that is how a
      // healed partition is noticed.
      if (id != self_ && e.member.state() != v1::Member::STATE_LEFT) peers.push_back(id);
    }
    std::shuffle(peers.begin(), peers.end(), rng_);
    if (peers.size() > static_cast<size_t>(options_.fanout)) {
      peers.resize(static_cast<size_t>(options_.fanout));
    }
    payload = Digest().SerializeAsString();
  }
  Notify(changes);
  for (const auto& peer : peers) {
    rt_.transport->Call(peer, v1::MESSAGE_TYPE_GOSSIP, payload, options_.rpc_timeout_ns,
                        [this](Result<std::string> resp) {
                          if (!resp.ok()) return;  // a silent peer simply goes stale
                          v1::GossipResponse r;
                          if (!r.ParseFromString(*resp)) return;
                          std::vector<std::pair<NodeId, v1::Member::State>> ch;
                          {
                            std::lock_guard lock(mu_);
                            for (const auto& m : r.members()) Merge(m, &ch);
                          }
                          Notify(ch);
                        });
  }
  rt_.scheduler->After(options_.gossip_interval_ns, [this] { Tick(); });
}

Result<std::string> Membership::HandleGossip(std::string_view payload) {
  v1::GossipRequest req;
  if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return Status::InvalidArgument("gossip: bad payload");
  }
  std::vector<std::pair<NodeId, v1::Member::State>> changes;
  std::string out;
  {
    std::lock_guard lock(mu_);
    for (const auto& m : req.members()) Merge(m, &changes);
    v1::GossipResponse resp;
    for (const auto& [id, e] : members_) *resp.add_members() = e.member;
    out = resp.SerializeAsString();
  }
  Notify(changes);
  return out;
}

void Membership::Merge(const v1::Member& remote,
                       std::vector<std::pair<NodeId, v1::Member::State>>* changes) {
  if (remote.node_id() == self_) return;  // nobody else speaks for us
  const int64_t now = rt_.clock->NowNanos();
  auto [it, inserted] = members_.try_emplace(remote.node_id());
  Entry& e = it->second;
  const bool newer_incarnation = inserted || remote.incarnation() > e.member.incarnation();
  const bool newer_heartbeat =
      remote.incarnation() == e.member.incarnation() && remote.heartbeat() > e.member.heartbeat();
  if (!newer_incarnation && !newer_heartbeat) {
    // Old news, except a graceful leave, which always propagates.
    if (remote.state() == v1::Member::STATE_LEFT && e.member.state() != v1::Member::STATE_LEFT &&
        remote.incarnation() >= e.member.incarnation()) {
      e.member.set_state(v1::Member::STATE_LEFT);
      changes->emplace_back(remote.node_id(), v1::Member::STATE_LEFT);
    }
    return;
  }
  const v1::Member::State before = inserted ? v1::Member::STATE_UNSPECIFIED : e.member.state();
  e.member.set_node_id(remote.node_id());
  e.member.set_address(remote.address());
  e.member.set_incarnation(remote.incarnation());
  e.member.set_heartbeat(remote.heartbeat());
  // A fresher heartbeat is direct evidence of life.
  e.member.set_state(remote.state() == v1::Member::STATE_LEFT ? v1::Member::STATE_LEFT
                                                              : v1::Member::STATE_ALIVE);
  e.last_heard_ns = now;
  if (e.member.state() != before) changes->emplace_back(remote.node_id(), e.member.state());
}

void Membership::Evaluate(std::vector<std::pair<NodeId, v1::Member::State>>* changes) {
  const int64_t now = rt_.clock->NowNanos();
  for (auto& [id, e] : members_) {
    if (id == self_ || e.member.state() == v1::Member::STATE_LEFT) continue;
    const int64_t silent = now - e.last_heard_ns;
    v1::Member::State next = v1::Member::STATE_ALIVE;
    if (silent >= options_.dead_after_ns) {
      next = v1::Member::STATE_DEAD;
    } else if (silent >= options_.suspect_after_ns) {
      next = v1::Member::STATE_SUSPECT;
    }
    if (next != e.member.state()) {
      e.member.set_state(next);
      changes->emplace_back(id, next);
    }
  }
}

void Membership::Notify(const std::vector<std::pair<NodeId, v1::Member::State>>& changes) {
  Listener l;
  {
    std::lock_guard lock(mu_);
    l = listener_;
  }
  if (!l) return;
  for (const auto& [id, state] : changes) l(id, state);
}

std::set<NodeId> Membership::AllNodes() const {
  std::lock_guard lock(mu_);
  std::set<NodeId> out;
  for (const auto& [id, e] : members_) {
    if (e.member.state() != v1::Member::STATE_LEFT) out.insert(id);
  }
  return out;
}

std::set<NodeId> Membership::DeadNodes() const {
  std::lock_guard lock(mu_);
  std::set<NodeId> out;
  for (const auto& [id, e] : members_) {
    if (e.member.state() == v1::Member::STATE_DEAD || e.member.state() == v1::Member::STATE_LEFT) {
      out.insert(id);
    }
  }
  return out;
}

bool Membership::IsAlive(const NodeId& id) const {
  std::lock_guard lock(mu_);
  auto it = members_.find(id);
  return it != members_.end() && (it->second.member.state() == v1::Member::STATE_ALIVE ||
                                  it->second.member.state() == v1::Member::STATE_SUSPECT);
}

std::string Membership::AddressOf(const NodeId& id) const {
  std::lock_guard lock(mu_);
  auto it = members_.find(id);
  return it == members_.end() ? std::string() : it->second.member.address();
}

std::vector<v1::Member> Membership::Snapshot() const {
  std::lock_guard lock(mu_);
  std::vector<v1::Member> out;
  out.reserve(members_.size());
  for (const auto& [id, e] : members_) out.push_back(e.member);
  return out;
}

}  // namespace cartwheel::cluster
