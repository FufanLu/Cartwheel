#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "cartwheel/cart/v1/cart.pb.h"
#include "client/kv_api.h"
#include "common/clock.h"
#include "common/status.h"
#include "services/cart/or_set.h"

namespace cartwheel::cart {

inline constexpr std::string_view kCartTable = "carts";
inline constexpr uint32_t kMaxQuantityPerLine = 99;
inline constexpr size_t kMaxLinesPerCart = 100;

// Cart business logic on top of the Dynamo-style KV API. No locks, no CAS
// loop. Every mutation is read → fold siblings into one OR-Set → apply → put
// with the read context. Two concurrent mutations both succeed and become DVV
// siblings, and the next read folds them back together. An add is never lost,
// which is the "always writable" property from the Dynamo paper.
class CartStore {
 public:
  CartStore(client::KvApi* kv, const Clock* clock = SystemClock::Default());

  [[nodiscard]] Result<v1::Cart> Get(std::string_view user_id);
  [[nodiscard]] Result<v1::Cart> AddItem(std::string_view user_id, const v1::CartItem& item,
                                         std::string_view idempotency_key);
  [[nodiscard]] Result<v1::Cart> RemoveItem(std::string_view user_id, std::string_view sku);
  [[nodiscard]] Result<v1::Cart> Clear(std::string_view user_id);

  // Folds raw sibling values into one cart (exposed for tests and docs).
  [[nodiscard]] static Result<CartOrSet> Fold(const client::KvRecord& record);
  [[nodiscard]] static v1::Cart ToView(std::string_view user_id, const CartOrSet& set,
                                       uint32_t merged_siblings);

 private:
  template <typename Mutation>
  Result<v1::Cart> Mutate(std::string_view user_id, Mutation&& mutate);

  client::KvApi* kv_;
  const Clock* clock_;
};

Status ValidateUserId(std::string_view user_id);
Status ValidateItem(const v1::CartItem& item);

// 128-bit random tag, hex encoded.
[[nodiscard]] std::string RandomTag();

}  // namespace cartwheel::cart
