#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cartwheel/cart/v1/cart_state.pb.h"
#include "common/status.h"

namespace cartwheel::cart {

// Observed-Remove Set specialised for carts (add-wins semantics).
//
// Dynamo §4.4 notes that merging cart siblings by plain union "can cause
// deleted items to resurface". An OR-Set avoids that. Every add carries a
// unique tag, and a remove records the tags it observed rather than the SKU.
// Merging two replicas means taking the union of their adds and the union of
// their removed tags:
//   * an add the remover saw stays removed (its tag is in removed_tags);
//   * an add the remover did NOT see survives (its tag is new).
// The merge is commutative, associative, and idempotent, so any number of
// DVV siblings fold into the same cart in any order.
class CartOrSet {
 public:
  struct Line {
    std::string sku;
    uint32_t quantity = 0;
    int64_t unit_price_cents = 0;
    std::string title;
  };

  CartOrSet() = default;
  static Result<CartOrSet> FromState(const v1::CartState& state);
  static Result<CartOrSet> Decode(std::string_view bytes);

  // Adds `quantity` of `sku` under `tag`. Re-adding an existing or removed tag
  // is a no-op, which makes retries with the same idempotency key safe.
  void Add(std::string_view tag, std::string_view sku, uint32_t quantity, int64_t unit_price_cents,
           std::string_view title, int64_t now_ms);

  // Removes every live add of `sku` this replica has observed.
  void Remove(std::string_view sku);
  void Clear();

  void Merge(const CartOrSet& other);

  // Live view: quantities of all surviving adds of a SKU are summed.
  // Ordered by SKU.
  [[nodiscard]] std::vector<Line> Lines() const;
  [[nodiscard]] int64_t TotalCents() const;

  [[nodiscard]] v1::CartState ToState() const;
  [[nodiscard]] std::string Encode() const;

  [[nodiscard]] size_t live_adds() const;
  [[nodiscard]] size_t tombstones() const noexcept { return removed_.size(); }

 private:
  std::map<std::string, v1::CartAdd> adds_;  // tag -> add (ordered for deterministic encoding)
  std::set<std::string> removed_;            // tags
};

}  // namespace cartwheel::cart
