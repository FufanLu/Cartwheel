#pragma once

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "services/deal/deal_ports.h"

namespace cartwheel::deal {

// What the database knows about a deal inside the batch transaction (read
// under a row lock, so no other worker changes it until COMMIT).
struct DealState {
  int64_t generation = 0;
  int32_t quantity = 0;
  int32_t sold = 0;
};

using UserKey = std::tuple<std::string, int64_t, std::string>;  // deal, generation, user

// The commit rules, shared by the Postgres and in-memory stores so both
// behave identically. For each reservation, in log order:
//   already committed (same order id)      -> duplicate (returned as stored)
//   unknown deal                           -> rejected "unknown_deal"
//   generation != the deal's generation    -> rejected "stale_generation"
//   user already has a confirmed order     -> rejected "one_per_customer"
//   sold == quantity                       -> rejected "sold_out"
//   otherwise                              -> confirmed, sold += 1
// `deals` and `confirmed_users` are updated in place.
BatchResult DecideBatch(const std::vector<v1::OrderPlaced>& events,
                        const std::map<std::string, OrderRecord>& existing,
                        std::map<std::string, DealState>* deals,
                        std::set<UserKey>* confirmed_users);

}  // namespace cartwheel::deal
