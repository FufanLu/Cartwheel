#include "services/deal/deal_catalog.h"

#include <limits>

#include "common/logging.h"

namespace cartwheel::deal {

DealCatalog::DealCatalog(OrderStore* store, const Clock* clock, int64_t ttl_ms)
    : store_(store), clock_(clock), ttl_ns_(ttl_ms * 1'000'000) {}

Result<DealCatalog::Snapshot> DealCatalog::Get() {
  std::unique_lock lock(mu_);
  for (;;) {
    const bool fresh = cached_ && clock_->NowNanos() - loaded_at_ns_ < ttl_ns_;
    if (fresh) return cached_;
    if (!loading_) break;
    // Someone is already loading: wait for their result instead of piling on.
    loaded_.wait(lock, [&] { return !loading_; });
    if (cached_ && clock_->NowNanos() - loaded_at_ns_ < ttl_ns_) return cached_;
    if (!last_error_.ok()) {
      if (cached_) return cached_;  // stale beats down
      return last_error_;
    }
  }
  loading_ = true;
  const uint64_t epoch = epoch_;
  lock.unlock();

  auto rows = store_->LoadDeals();
  const Status load_status = rows.status();

  lock.lock();
  loading_ = false;
  ++loads_;
  Snapshot result;
  if (rows.ok()) {
    cached_ = std::make_shared<const std::vector<DealRecord>>(std::move(rows).value());
    // A load that started before an Invalidate may predate the change it
    // signalled, so it serves this caller but is not trusted as fresh.
    loaded_at_ns_ = epoch == epoch_ ? clock_->NowNanos() : std::numeric_limits<int64_t>::min() / 2;
    last_error_ = Status::Ok();
    result = cached_;
  } else {
    last_error_ = load_status;
    CW_LOG_WARN("deal catalog reload failed ({}); {}", last_error_.ToString(),
                cached_ ? "serving the previous snapshot" : "no snapshot to serve");
    result = cached_;
  }
  lock.unlock();
  loaded_.notify_all();
  if (!result) return load_status;
  return result;
}

Result<DealRecord> DealCatalog::Find(const std::string& deal_id) {
  CW_ASSIGN_OR_RETURN(auto snap, Get());
  for (const auto& d : *snap) {
    if (d.deal_id == deal_id) return d;
  }
  return Status::NotFound("no deal " + deal_id);
}

void DealCatalog::Invalidate() {
  std::lock_guard lock(mu_);
  ++epoch_;
  loaded_at_ns_ = std::numeric_limits<int64_t>::min() / 2;
}

uint64_t DealCatalog::loads() const {
  std::lock_guard lock(mu_);
  return loads_;
}

}  // namespace cartwheel::deal
