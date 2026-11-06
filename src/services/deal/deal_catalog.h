#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "common/clock.h"
#include "services/deal/deal_ports.h"

namespace cartwheel::deal {

// Read-through cache of the deal catalog in front of Postgres.
//
// At a deal's start every buyer asks for the catalog in the same second. Two
// rules keep that burst away from the database:
//   - singleflight: concurrent misses wait for one load instead of each
//     sending a query (the "thundering herd" on cache expiry);
//   - serve stale on error: if a reload fails, the previous snapshot keeps
//     answering, so a brief database blip does not take down the storefront.
class DealCatalog {
 public:
  using Snapshot = std::shared_ptr<const std::vector<DealRecord>>;

  DealCatalog(OrderStore* store, const Clock* clock, int64_t ttl_ms);

  Result<Snapshot> Get();
  Result<DealRecord> Find(const std::string& deal_id);
  // Forces the next Get to reload (after a reset).
  void Invalidate();

  [[nodiscard]] uint64_t loads() const;

 private:
  OrderStore* store_;
  const Clock* clock_;
  const int64_t ttl_ns_;

  mutable std::mutex mu_;
  std::condition_variable loaded_;
  Snapshot cached_;
  int64_t loaded_at_ns_ = 0;
  uint64_t epoch_ = 0;  // bumped by Invalidate; a load from an older epoch is born stale
  bool loading_ = false;
  Status last_error_;
  uint64_t loads_ = 0;
};

}  // namespace cartwheel::deal
