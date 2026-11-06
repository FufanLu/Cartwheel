#include "services/deal/batch_rules.h"

#include <unordered_map>

namespace cartwheel::deal {

BatchResult DecideBatch(const std::vector<v1::OrderPlaced>& events,
                        const std::map<std::string, OrderRecord>& existing,
                        std::map<std::string, DealState>* deals,
                        std::set<UserKey>* confirmed_users) {
  BatchResult result;
  // Index of orders decided in this batch (a record can repeat within one
  // batch when the producer's retry and the original both landed).
  std::unordered_map<std::string, OrderRecord> decided;
  for (const auto& e : events) {
    if (auto it = existing.find(e.order_id()); it != existing.end()) {
      result.duplicates.push_back(it->second);
      continue;
    }
    if (auto it = decided.find(e.order_id()); it != decided.end()) {
      result.duplicates.push_back(it->second);
      continue;
    }
    OrderRecord o{.order_id = e.order_id(),
                  .deal_id = e.deal_id(),
                  .user_id = e.user_id(),
                  .price_cents = e.price_cents(),
                  .generation = e.generation(),
                  .reserved_at_ms = e.reserved_at_ms(),
                  .confirmed = false,
                  .reason = {}};
    auto d = deals->find(e.deal_id());
    const UserKey user{e.deal_id(), e.generation(), e.user_id()};
    if (d == deals->end()) {
      o.reason = "unknown_deal";
    } else if (e.generation() != d->second.generation) {
      o.reason = "stale_generation";
    } else if (confirmed_users->contains(user)) {
      o.reason = "one_per_customer";
    } else if (d->second.sold >= d->second.quantity) {
      o.reason = "sold_out";
    } else {
      ++d->second.sold;
      o.confirmed = true;
      confirmed_users->insert(user);
    }
    decided.emplace(o.order_id, o);
    (o.confirmed ? result.confirmed : result.rejected).push_back(std::move(o));
  }
  return result;
}

}  // namespace cartwheel::deal
