#include "services/deal/memory_ports.h"

#include <algorithm>
#include <chrono>

#include "services/deal/batch_rules.h"

namespace cartwheel::deal {

// --- MemoryInventoryGate -----------------------------------------------------

Result<Reservation> MemoryInventoryGate::Reserve(const std::string& deal_id, int64_t generation,
                                                 const std::string& user_id,
                                                 const std::string& order_id) {
  std::lock_guard lock(mu_);
  auto it = slots_.find({deal_id, generation});
  if (it == slots_.end()) {
    return Reservation{.kind = Reservation::Kind::kNotArmed, .order_id = {}, .remaining = 0};
  }
  Slot& s = it->second;
  if (auto held = s.holds.find(user_id); held != s.holds.end()) {
    return Reservation{
        .kind = Reservation::Kind::kAlreadyClaimed, .order_id = held->second, .remaining = s.stock};
  }
  if (s.stock <= 0) {
    return Reservation{.kind = Reservation::Kind::kSoldOut, .order_id = {}, .remaining = 0};
  }
  --s.stock;
  s.holds.emplace(user_id, order_id);
  return Reservation{
      .kind = Reservation::Kind::kReserved, .order_id = order_id, .remaining = s.stock};
}

Result<std::vector<std::pair<std::string, std::string>>> MemoryInventoryGate::Holds(
    const std::string& deal_id, int64_t generation) {
  std::lock_guard lock(mu_);
  std::vector<std::pair<std::string, std::string>> out;
  auto it = slots_.find({deal_id, generation});
  if (it == slots_.end()) return out;
  out.assign(it->second.holds.begin(), it->second.holds.end());
  return out;
}

Status MemoryInventoryGate::Arm(const std::string& deal_id, int64_t generation, int64_t quantity,
                                bool only_if_absent) {
  std::lock_guard lock(mu_);
  auto [it, inserted] = slots_.try_emplace({deal_id, generation});
  if (inserted || !only_if_absent) {
    it->second.stock = quantity;
    it->second.holds.clear();
  }
  return Status::Ok();
}

Result<int64_t> MemoryInventoryGate::Remaining(const std::string& deal_id, int64_t generation) {
  std::lock_guard lock(mu_);
  auto it = slots_.find({deal_id, generation});
  return it == slots_.end() ? 0 : it->second.stock;
}

Result<std::optional<std::string>> MemoryInventoryGate::HeldOrder(const std::string& deal_id,
                                                                  int64_t generation,
                                                                  const std::string& user_id) {
  std::lock_guard lock(mu_);
  auto it = slots_.find({deal_id, generation});
  if (it == slots_.end()) return std::optional<std::string>();
  auto held = it->second.holds.find(user_id);
  if (held == it->second.holds.end()) return std::optional<std::string>();
  return std::optional<std::string>(held->second);
}

void MemoryInventoryGate::ForgetEverything(const std::string& deal_id, int64_t generation,
                                           int64_t stock) {
  std::lock_guard lock(mu_);
  Slot& s = slots_[{deal_id, generation}];
  s.holds.clear();
  s.stock = stock;
}

// --- MemoryOrderLog ----------------------------------------------------------

void MemoryOrderLog::Publish(const v1::OrderPlaced& event, Done done) {
  PublishOutcome outcome = PublishOutcome::kDelivered;
  bool written = true;
  {
    std::lock_guard lock(mu_);
    if (fail_next_ > 0) {
      --fail_next_;
      outcome = fail_outcome_;
      written = outcome == PublishOutcome::kUnknown && !fail_lost_;
    }
    if (written) log_.push_back(event);
  }
  cv_.notify_all();
  done(outcome, outcome == PublishOutcome::kDelivered ? "" : "injected fault");
}

void MemoryOrderLog::FailNext(int n, PublishOutcome outcome, bool lost) {
  std::lock_guard lock(mu_);
  fail_next_ = n;
  fail_outcome_ = outcome;
  fail_lost_ = lost;
}

std::vector<v1::OrderPlaced> MemoryOrderLog::Poll(size_t max, int64_t wait_ms) {
  std::unique_lock lock(mu_);
  cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
               [&] { return closed_ || position_ < log_.size(); });
  std::vector<v1::OrderPlaced> out;
  while (position_ < log_.size() && out.size() < max) out.push_back(log_[position_++]);
  return out;
}

Status MemoryOrderLog::Commit() {
  std::lock_guard lock(mu_);
  committed_ = position_;
  return Status::Ok();
}

void MemoryOrderLog::Rewind() {
  std::lock_guard lock(mu_);
  position_ = committed_;
}

void MemoryOrderLog::Close() {
  {
    std::lock_guard lock(mu_);
    closed_ = true;
  }
  cv_.notify_all();
}

size_t MemoryOrderLog::size() const {
  std::lock_guard lock(mu_);
  return log_.size();
}

size_t MemoryOrderLog::committed() const {
  std::lock_guard lock(mu_);
  return committed_;
}

// --- MemoryOrderStore --------------------------------------------------------

Status MemoryOrderStore::UpsertDeal(const DealRecord& deal) {
  std::lock_guard lock(mu_);
  auto [it, inserted] = deals_.try_emplace(deal.deal_id, deal);
  if (!inserted) {
    // Seeding keeps progress (sold, generation) of an existing deal.
    DealRecord& d = it->second;
    d.sku = deal.sku;
    d.title = deal.title;
    d.price_cents = deal.price_cents;
    d.list_price_cents = deal.list_price_cents;
  }
  return Status::Ok();
}

Result<std::vector<DealRecord>> MemoryOrderStore::LoadDeals() {
  load_calls_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(mu_);
  std::vector<DealRecord> out;
  out.reserve(deals_.size());
  for (const auto& [id, d] : deals_) out.push_back(d);
  return out;
}

Result<std::vector<OrderRecord>> MemoryOrderStore::OrdersForUser(const std::string& user_id) {
  std::lock_guard lock(mu_);
  std::vector<OrderRecord> out;
  for (const auto& [id, o] : orders_) {
    if (o.user_id == user_id) out.push_back(o);
  }
  std::sort(out.begin(), out.end(), [](const OrderRecord& a, const OrderRecord& b) {
    return a.reserved_at_ms > b.reserved_at_ms;
  });
  return out;
}

Result<std::set<std::string>> MemoryOrderStore::KnownOrderIds(const std::vector<std::string>& ids) {
  std::lock_guard lock(mu_);
  std::set<std::string> out;
  for (const auto& id : ids) {
    if (orders_.contains(id)) out.insert(id);
  }
  return out;
}

Result<BatchResult> MemoryOrderStore::ApplyBatch(const std::vector<v1::OrderPlaced>& events) {
  std::lock_guard lock(mu_);  // the whole store is one "row lock"
  if (fail_batches_ > 0) {
    --fail_batches_;
    return Status::Unavailable("injected: database unavailable");
  }
  std::map<std::string, DealState> states;
  for (const auto& [id, d] : deals_) {
    states.emplace(id,
                   DealState{.generation = d.generation, .quantity = d.quantity, .sold = d.sold});
  }
  std::set<UserKey> confirmed_users;
  for (const auto& [id, o] : orders_) {
    if (o.confirmed) confirmed_users.emplace(o.deal_id, o.generation, o.user_id);
  }
  BatchResult result = DecideBatch(events, orders_, &states, &confirmed_users);
  // "COMMIT"
  for (const auto& [id, st] : states) deals_[id].sold = st.sold;
  for (const auto* group : {&result.confirmed, &result.rejected}) {
    for (const auto& o : *group) orders_.emplace(o.order_id, o);
  }
  return result;
}

Result<DealRecord> MemoryOrderStore::ResetDeal(const std::string& deal_id, int32_t quantity,
                                               int64_t starts_at_ms, int64_t ends_at_ms) {
  std::lock_guard lock(mu_);
  auto it = deals_.find(deal_id);
  if (it == deals_.end()) return Status::NotFound("deal " + deal_id);
  DealRecord& d = it->second;
  ++d.generation;
  d.sold = 0;
  d.quantity = quantity;
  d.starts_at_ms = starts_at_ms;
  d.ends_at_ms = ends_at_ms;
  return d;
}

void MemoryOrderStore::FailNextBatches(int n) {
  std::lock_guard lock(mu_);
  fail_batches_ = n;
}

size_t MemoryOrderStore::ConfirmedCount(const std::string& deal_id, int64_t generation) const {
  std::lock_guard lock(mu_);
  return static_cast<size_t>(std::count_if(orders_.begin(), orders_.end(), [&](const auto& kv) {
    const OrderRecord& o = kv.second;
    return o.confirmed && o.deal_id == deal_id && o.generation == generation;
  }));
}

// --- MemoryNotifier ----------------------------------------------------------

void MemoryNotifier::OrderUpdated(const v1::Order& order) {
  std::lock_guard lock(mu_);
  sent_.push_back(order);
}

std::vector<v1::Order> MemoryNotifier::Drain() {
  std::lock_guard lock(mu_);
  return std::exchange(sent_, {});
}

// --- conversions -------------------------------------------------------------

v1::Deal ToProto(const DealRecord& d, int64_t remaining) {
  v1::Deal p;
  p.set_deal_id(d.deal_id);
  p.set_sku(d.sku);
  p.set_title(d.title);
  p.set_price_cents(d.price_cents);
  p.set_list_price_cents(d.list_price_cents);
  p.set_quantity(d.quantity);
  p.set_remaining(static_cast<int32_t>(std::clamp<int64_t>(remaining, 0, d.quantity)));
  p.set_sold(d.sold);
  p.set_starts_at_ms(d.starts_at_ms);
  p.set_ends_at_ms(d.ends_at_ms);
  p.set_generation(d.generation);
  return p;
}

v1::Order ToProto(const OrderRecord& o) {
  v1::Order p;
  p.set_order_id(o.order_id);
  p.set_deal_id(o.deal_id);
  p.set_user_id(o.user_id);
  p.set_price_cents(o.price_cents);
  p.set_status(o.confirmed ? v1::Order::STATUS_CONFIRMED : v1::Order::STATUS_REJECTED);
  p.set_reserved_at_ms(o.reserved_at_ms);
  p.set_generation(o.generation);
  p.set_reason(o.reason);
  return p;
}

std::string NotificationChannel(const std::string& user_id) {
  return "orders:" + user_id;
}

}  // namespace cartwheel::deal
