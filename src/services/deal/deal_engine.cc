#include "services/deal/deal_engine.h"

#include <chrono>
#include <memory>
#include <random>
#include <unordered_set>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/metrics.h"

namespace cartwheel::deal {
namespace {

using Outcome = v1::ClaimResponse;

double SecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

const char* OutcomeLabel(v1::ClaimResponse::Outcome o) {
  switch (o) {
    case v1::ClaimResponse::OUTCOME_RESERVED: return "reserved";
    case v1::ClaimResponse::OUTCOME_ALREADY_CLAIMED: return "already_claimed";
    case v1::ClaimResponse::OUTCOME_SOLD_OUT: return "sold_out";
    case v1::ClaimResponse::OUTCOME_NOT_STARTED: return "not_started";
    case v1::ClaimResponse::OUTCOME_ENDED: return "ended";
    default: return "unspecified";
  }
}

}  // namespace

DealEngine::DealEngine(DealCatalog* catalog, InventoryGate* gate, OrderLog* log, OrderStore* store,
                       const Clock* clock, prometheus::Registry& registry)
    : catalog_(catalog),
      gate_(gate),
      log_(log),
      store_(store),
      clock_(clock),
      outcomes_(prometheus::BuildCounter()
                    .Name("cartwheel_deal_claims_total")
                    .Help("Claim attempts by outcome")
                    .Register(registry)),
      gate_latency_(prometheus::BuildHistogram()
                        .Name("cartwheel_deal_gate_duration_seconds")
                        .Help("Latency of the atomic reservation at the inventory gate")
                        .Register(registry)
                        .Add({}, LatencyBucketsSeconds())),
      publish_latency_(prometheus::BuildHistogram()
                           .Name("cartwheel_deal_publish_duration_seconds")
                           .Help("Reservation to order-log acknowledgement")
                           .Register(registry)
                           .Add({}, LatencyBucketsSeconds())),
      publish_results_(prometheus::BuildCounter()
                           .Name("cartwheel_deal_publish_total")
                           .Help("Order-log publishes by result (delivered, failed, unknown)")
                           .Register(registry)),
      swept_(prometheus::BuildCounter()
                 .Name("cartwheel_deal_swept_total")
                 .Help("Holds found without a committed order and re-published by the sweeper")
                 .Register(registry)
                 .Add({})),
      rearms_(prometheus::BuildCounter()
                  .Name("cartwheel_deal_rearms_total")
                  .Help("Stock keys recreated from Postgres after the gate lost them")
                  .Register(registry)
                  .Add({})),
      redrive_(log, registry) {}

std::string DealEngine::NewOrderId() const {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  return fmt::format("ord_{:011x}{:016x}", clock_->WallMillis(), rng());
}

void DealEngine::Finish(const ClaimDone& done, v1::ClaimResponse::Outcome outcome,
                        std::string order_id, int64_t remaining) {
  outcomes_.Add({{"outcome", OutcomeLabel(outcome)}}).Increment();
  v1::ClaimResponse resp;
  resp.set_outcome(outcome);
  resp.set_order_id(std::move(order_id));
  resp.set_remaining(static_cast<int32_t>(std::max<int64_t>(remaining, 0)));
  done(std::move(resp));
}

void DealEngine::Claim(const std::string& deal_id, const std::string& user_id, ClaimDone done) {
  auto deal_or = catalog_->Find(deal_id);
  if (!deal_or.ok()) {
    outcomes_.Add({{"outcome", "error"}}).Increment();
    done(deal_or.status());
    return;
  }
  const DealRecord deal = std::move(deal_or).value();
  const int64_t now_ms = clock_->WallMillis();
  if (now_ms < deal.starts_at_ms) {
    Finish(done, Outcome::OUTCOME_NOT_STARTED, "", deal.quantity - deal.sold);
    return;
  }
  if (now_ms >= deal.ends_at_ms) {
    Finish(done, Outcome::OUTCOME_ENDED, "", 0);
    return;
  }

  std::string order_id = NewOrderId();
  const auto gate_start = std::chrono::steady_clock::now();
  auto res = gate_->Reserve(deal.deal_id, deal.generation, user_id, order_id);
  if (res.ok() && res->kind == Reservation::Kind::kNotArmed) {
    // The gate lost its stock key (Redis restarted without persistence, or
    // was flushed). Rebuild it from Postgres, the source of truth. Units that
    // were reserved but not yet committed are counted as available again; if
    // that over-admits, the database rejects the excess (it re-checks the
    // quantity under a row lock), so the cost is a rejected order, never an
    // oversold one.
    rearms_.Increment();
    CW_LOG_WARN("gate has no stock for {} gen {}; re-arming with {} from Postgres", deal.deal_id,
                deal.generation, deal.quantity - deal.sold);
    auto armed = gate_->Arm(deal.deal_id, deal.generation, deal.quantity - deal.sold,
                            /*only_if_absent=*/true);
    if (!armed.ok()) {
      res = armed;
    } else {
      res = gate_->Reserve(deal.deal_id, deal.generation, user_id, order_id);
    }
  }
  gate_latency_.Observe(SecondsSince(gate_start));
  if (!res.ok()) {
    outcomes_.Add({{"outcome", "error"}}).Increment();
    done(res.status().WithContext("inventory gate"));
    return;
  }
  switch (res->kind) {
    case Reservation::Kind::kAlreadyClaimed:
      Finish(done, Outcome::OUTCOME_ALREADY_CLAIMED, res->order_id, res->remaining);
      return;
    case Reservation::Kind::kSoldOut:
    case Reservation::Kind::kNotArmed: Finish(done, Outcome::OUTCOME_SOLD_OUT, "", 0); return;
    case Reservation::Kind::kReserved: break;
  }

  v1::OrderPlaced event;
  event.set_order_id(order_id);
  event.set_deal_id(deal.deal_id);
  event.set_user_id(user_id);
  event.set_price_cents(deal.price_cents);
  event.set_generation(deal.generation);
  event.set_reserved_at_ms(now_ms);
  const int64_t remaining = res->remaining;
  const auto publish_start = std::chrono::steady_clock::now();
  log_->Publish(event, [this, done = std::move(done), event, order_id, remaining, publish_start](
                           PublishOutcome outcome, const std::string& error) {
    publish_latency_.Observe(SecondsSince(publish_start));
    if (outcome == PublishOutcome::kDelivered) {
      publish_results_.Add({{"result", "delivered"}}).Increment();
      Finish(done, Outcome::OUTCOME_RESERVED, order_id, remaining);
      return;
    }
    // Not acknowledged. The hold is kept either way: releasing a "failed"
    // unit races with the buyer's retry, which may already have been told
    // ALREADY_CLAIMED with this order id, and releasing an "unknown" one could
    // sell it twice. Instead the same record is published again until the log
    // has it; a second copy is a harmless duplicate for order-worker.
    publish_results_.Add({{"result", outcome == PublishOutcome::kFailed ? "failed" : "unknown"}})
        .Increment();
    redrive_.Add(event);
    outcomes_.Add({{"outcome", "error"}}).Increment();
    done(Status::Unavailable("order log did not confirm (" + error +
                             "); your unit is held, retry to see the order"));
  });
}

namespace {

// ord_<11 hex digits of unix millis><16 random>
int64_t ReservedAtMs(const std::string& order_id) {
  if (order_id.size() < 15 || !order_id.starts_with("ord_")) return 0;
  int64_t v = 0;
  for (size_t i = 4; i < 15; ++i) {
    const char c = order_id[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
    if (d < 0) return 0;
    v = v * 16 + d;
  }
  return v;
}

}  // namespace

Result<int> DealEngine::SweepOnce(int64_t min_age_ms) {
  CW_ASSIGN_OR_RETURN(auto snap, catalog_->Get());
  const int64_t now = clock_->WallMillis();
  int redriven = 0;
  for (const auto& d : *snap) {
    CW_ASSIGN_OR_RETURN(auto holds, gate_->Holds(d.deal_id, d.generation));
    std::vector<std::pair<std::string, std::string>> stale;  // user, order id
    std::vector<std::string> ids;
    for (auto& [user, order] : holds) {
      const int64_t at = ReservedAtMs(order);
      if (at == 0 || now - at < min_age_ms) continue;
      ids.push_back(order);
      stale.emplace_back(std::move(user), std::move(order));
    }
    if (ids.empty()) continue;
    CW_ASSIGN_OR_RETURN(auto known, store_->KnownOrderIds(ids));
    for (const auto& [user, order] : stale) {
      if (known.contains(order) || redrive_.Contains(order)) continue;
      // A hold whose record never reached the log: deal-svc crashed between
      // Reserve and Publish, or its redrive queue died with it.
      v1::OrderPlaced e;
      e.set_order_id(order);
      e.set_deal_id(d.deal_id);
      e.set_user_id(user);
      e.set_price_cents(d.price_cents);
      e.set_generation(d.generation);
      e.set_reserved_at_ms(ReservedAtMs(order));
      redrive_.Add(e);
      ++redriven;
    }
  }
  if (redriven > 0) {
    swept_.Increment(redriven);
    CW_LOG_WARN("hold sweeper: re-publishing {} reservations with no committed order", redriven);
  }
  return redriven;
}

Result<v1::ListDealsResponse> DealEngine::ListDeals() {
  CW_ASSIGN_OR_RETURN(auto snap, catalog_->Get());
  v1::ListDealsResponse resp;
  for (const auto& d : *snap) {
    auto remaining = gate_->Remaining(d.deal_id, d.generation);
    *resp.add_deals() = ToProto(d, remaining.ok() ? *remaining : d.quantity - d.sold);
  }
  resp.set_server_time_ms(clock_->WallMillis());
  return resp;
}

Result<v1::Deal> DealEngine::GetDeal(const std::string& deal_id) {
  CW_ASSIGN_OR_RETURN(auto d, catalog_->Find(deal_id));
  auto remaining = gate_->Remaining(d.deal_id, d.generation);
  return ToProto(d, remaining.ok() ? *remaining : d.quantity - d.sold);
}

Result<v1::ListOrdersResponse> DealEngine::ListOrders(const std::string& user_id) {
  CW_ASSIGN_OR_RETURN(auto committed, store_->OrdersForUser(user_id));
  CW_ASSIGN_OR_RETURN(auto snap, catalog_->Get());
  v1::ListOrdersResponse resp;
  std::unordered_set<std::string> known;
  for (const auto& o : committed) known.insert(o.order_id);
  for (const auto& d : *snap) {
    auto held = gate_->HeldOrder(d.deal_id, d.generation, user_id);
    if (!held.ok() || !held->has_value() || known.contains(**held)) continue;
    v1::Order* p = resp.add_orders();
    p->set_order_id(**held);
    p->set_deal_id(d.deal_id);
    p->set_user_id(user_id);
    p->set_price_cents(d.price_cents);
    p->set_status(v1::Order::STATUS_PENDING);
    p->set_generation(d.generation);
  }
  for (const auto& o : committed) *resp.add_orders() = ToProto(o);
  return resp;
}

Result<v1::Order> DealEngine::GetOrder(const std::string& user_id, const std::string& order_id) {
  CW_ASSIGN_OR_RETURN(auto all, ListOrders(user_id));
  for (const auto& o : all.orders()) {
    if (o.order_id() == order_id) return o;
  }
  return Status::NotFound("no order " + order_id + " for this user");
}

Result<v1::Deal> DealEngine::Reset(const v1::ResetDealRequest& req) {
  if (req.quantity() <= 0) return Status::InvalidArgument("quantity must be positive");
  const int64_t now = clock_->WallMillis();
  const int64_t starts = now + std::max<int64_t>(req.starts_in_ms(), 0);
  const int64_t duration = req.duration_ms() > 0 ? req.duration_ms() : int64_t{10} * 60 * 1000;
  CW_ASSIGN_OR_RETURN(auto d,
                      store_->ResetDeal(req.deal_id(), req.quantity(), starts, starts + duration));
  CW_RETURN_IF_ERROR(gate_->Arm(d.deal_id, d.generation, d.quantity, /*only_if_absent=*/false));
  catalog_->Invalidate();
  CW_LOG_INFO("deal {} reset: generation {}, {} units, starts in {} ms", d.deal_id, d.generation,
              d.quantity, starts - now);
  return ToProto(d, d.quantity);
}

Status DealEngine::ArmAll() {
  CW_ASSIGN_OR_RETURN(auto snap, catalog_->Get());
  for (const auto& d : *snap) {
    CW_RETURN_IF_ERROR(
        gate_->Arm(d.deal_id, d.generation, d.quantity - d.sold, /*only_if_absent=*/true));
  }
  return Status::Ok();
}

}  // namespace cartwheel::deal
