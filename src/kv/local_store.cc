#include "kv/local_store.h"

#include <functional>

namespace cartwheel::kv {

LocalStore::LocalStore(std::string node_id, std::unique_ptr<storage::Engine> engine,
                       const Clock* clock)
    : node_id_(std::move(node_id)), engine_(std::move(engine)), clock_(clock) {}

std::string LocalStore::StorageKey(std::string_view table, std::string_view key) {
  std::string out;
  out.reserve(table.size() + 1 + key.size());
  out.append(table);
  out.push_back('\0');
  out.append(key);
  return out;
}

std::mutex& LocalStore::StripeFor(std::string_view storage_key) const {
  return stripes_[std::hash<std::string_view>{}(storage_key) % kLockStripes];
}

Result<cluster::SiblingSet> LocalStore::LoadLocked(const std::string& storage_key) const {
  auto raw = engine_->Get(storage_key);
  if (!raw.ok()) {
    if (raw.status().IsNotFound()) return cluster::SiblingSet{};
    return raw.status();
  }
  return cluster::SiblingSet::Decode(*raw);
}

Result<LocalStore::ReadResult> LocalStore::Read(std::string_view table,
                                                std::string_view key) const {
  const int64_t start = clock_->NowNanos();
  const std::string skey = StorageKey(table, key);
  // Reads need no stripe lock: the engine returns one atomic value, and a
  // concurrent writer replaces the whole encoded set at once.
  auto set = LoadLocked(skey);
  reads_.fetch_add(1, std::memory_order_relaxed);
  read_latency_.Record(clock_->NowNanos() - start);
  if (!set.ok()) return set.status().WithContext("read");
  return ReadResult{std::move(set).value()};
}

Result<LocalStore::WriteResult> LocalStore::CoordinateWrite(std::string_view table,
                                                            std::string_view key,
                                                            const cluster::VectorClock& context,
                                                            std::string value, bool tombstone) {
  const int64_t start = clock_->NowNanos();
  const std::string skey = StorageKey(table, key);
  WriteResult result;
  {
    std::lock_guard lock(StripeFor(skey));
    CW_ASSIGN_OR_RETURN(result.siblings, LoadLocked(skey));
    result.siblings.Update(node_id_, context, std::move(value), tombstone, clock_->WallMillis());
    CW_RETURN_IF_ERROR(engine_->Put(skey, result.siblings.Encode()));
  }
  writes_.fetch_add(1, std::memory_order_relaxed);
  write_latency_.Record(clock_->NowNanos() - start);
  return result;
}

Result<cluster::SiblingSet> LocalStore::Merge(std::string_view table, std::string_view key,
                                              const cluster::SiblingSet& remote) {
  const std::string skey = StorageKey(table, key);
  std::lock_guard lock(StripeFor(skey));
  CW_ASSIGN_OR_RETURN(cluster::SiblingSet set, LoadLocked(skey));
  const std::string before = set.Encode();
  set.Sync(remote);
  std::string after = set.Encode();
  if (after != before) CW_RETURN_IF_ERROR(engine_->Put(skey, after));
  return set;
}

}  // namespace cartwheel::kv
