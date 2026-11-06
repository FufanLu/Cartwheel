#pragma once

#include <functional>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "common/clock.h"
#include "services/deal/deal_catalog.h"
#include "services/deal/deal_ports.h"
#include "services/deal/redrive.h"

namespace cartwheel::deal {

// The claim path of a lightning deal, independent of gRPC:
//
//   window check (catalog) -> Reserve (gate, atomic) -> Publish (log)
//        |                         |                        |
//    NOT_STARTED / ENDED   SOLD_OUT / ALREADY_CLAIMED   delivered: RESERVED
//                                                       otherwise: keep the hold,
//                                                                  re-publish until acknowledged
//                                                                  (RedriveQueue), UNAVAILABLE
//
// A hold is never released. Once a buyer holds a unit it becomes an order:
// the redrive queue retries the publish, and SweepOnce re-publishes any hold
// that has no committed order after a grace period (covering a deal-svc crash
// between Reserve and Publish). Duplicates are free because order-worker is
// idempotent on the order id.
//
// Reserve and the window check are synchronous; the caller runs them on a
// worker pool. The publish completes asynchronously on a producer thread, so a
// worker is never parked waiting for the broker.
class DealEngine {
 public:
  using ClaimDone = std::function<void(Result<v1::ClaimResponse>)>;

  DealEngine(DealCatalog* catalog, InventoryGate* gate, OrderLog* log, OrderStore* store,
             const Clock* clock, prometheus::Registry& registry);

  void Claim(const std::string& deal_id, const std::string& user_id, ClaimDone done);

  Result<v1::ListDealsResponse> ListDeals();
  Result<v1::Deal> GetDeal(const std::string& deal_id);
  // Committed orders from Postgres plus reservations still in flight (held at
  // the gate, not yet committed), so a buyer sees PENDING immediately.
  Result<v1::ListOrdersResponse> ListOrders(const std::string& user_id);
  Result<v1::Order> GetOrder(const std::string& user_id, const std::string& order_id);
  Result<v1::Deal> Reset(const v1::ResetDealRequest& req);

  // Startup: make sure every deal's current generation has a stock key.
  // Existing keys are kept, so a restart never refills a running deal.
  Status ArmAll();

  // Time-ordered, unguessable: 11 hex digits of unix millis + 16 random.
  [[nodiscard]] std::string NewOrderId() const;

  // Re-publishes holds older than min_age_ms with no stored order. Returns
  // how many were queued. Safe on any number of deal-svc instances.
  Result<int> SweepOnce(int64_t min_age_ms);

  [[nodiscard]] size_t redrive_pending() const { return redrive_.pending(); }

 private:
  void Finish(const ClaimDone& done, v1::ClaimResponse::Outcome outcome, std::string order_id,
              int64_t remaining);

  DealCatalog* catalog_;
  InventoryGate* gate_;
  OrderLog* log_;
  OrderStore* store_;
  const Clock* clock_;

  prometheus::Family<prometheus::Counter>& outcomes_;
  prometheus::Histogram& gate_latency_;
  prometheus::Histogram& publish_latency_;
  prometheus::Family<prometheus::Counter>& publish_results_;
  prometheus::Counter& swept_;
  prometheus::Counter& rearms_;
  RedriveQueue redrive_;  // last: its thread may call back into this object
};

}  // namespace cartwheel::deal
