#include "services/cart/cart_store.h"

#include <random>

#include <fmt/format.h>

namespace cartwheel::cart {

Status ValidateUserId(std::string_view user_id) {
  if (user_id.empty() || user_id.size() > 128) {
    return Status::InvalidArgument("user_id must be 1-128 bytes");
  }
  return Status::Ok();
}

Status ValidateItem(const v1::CartItem& item) {
  if (item.sku().empty() || item.sku().size() > 64) {
    return Status::InvalidArgument("sku must be 1-64 bytes");
  }
  if (item.quantity() == 0 || item.quantity() > kMaxQuantityPerLine) {
    return Status::InvalidArgument("quantity must be 1-99");
  }
  if (item.unit_price_cents() < 0) return Status::InvalidArgument("price must be >= 0");
  if (item.title().size() > 200) return Status::InvalidArgument("title too long");
  return Status::Ok();
}

std::string RandomTag() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  return fmt::format("{:016x}{:016x}", rng(), rng());
}

CartStore::CartStore(client::KvApi* kv, const Clock* clock) : kv_(kv), clock_(clock) {}

Result<CartOrSet> CartStore::Fold(const client::KvRecord& record) {
  CartOrSet folded;
  for (const auto& value : record.values) {
    CW_ASSIGN_OR_RETURN(const CartOrSet sibling, CartOrSet::Decode(value));
    folded.Merge(sibling);
  }
  return folded;
}

v1::Cart CartStore::ToView(std::string_view user_id, const CartOrSet& set,
                           uint32_t merged_siblings) {
  v1::Cart cart;
  cart.set_user_id(std::string(user_id));
  for (const auto& line : set.Lines()) {
    auto* item = cart.add_items();
    item->set_sku(line.sku);
    item->set_quantity(line.quantity);
    item->set_unit_price_cents(line.unit_price_cents);
    item->set_title(line.title);
  }
  cart.set_total_cents(set.TotalCents());
  cart.set_merged_siblings(merged_siblings);
  return cart;
}

Result<v1::Cart> CartStore::Get(std::string_view user_id) {
  CW_RETURN_IF_ERROR(ValidateUserId(user_id));
  CW_ASSIGN_OR_RETURN(const auto record, kv_->Get(kCartTable, user_id));
  CW_ASSIGN_OR_RETURN(const auto set, Fold(record));
  return ToView(user_id, set, static_cast<uint32_t>(record.values.size()));
}

template <typename Mutation>
Result<v1::Cart> CartStore::Mutate(std::string_view user_id, Mutation&& mutate) {
  CW_RETURN_IF_ERROR(ValidateUserId(user_id));
  CW_ASSIGN_OR_RETURN(const auto before, kv_->Get(kCartTable, user_id));
  CW_ASSIGN_OR_RETURN(auto set, Fold(before));
  CW_RETURN_IF_ERROR(mutate(set));
  // Writing with the read context supersedes every sibling we just folded.
  // A writer that raced us survives as a sibling of this put, and the
  // response returns it, so the view below already includes its changes.
  CW_ASSIGN_OR_RETURN(const auto after,
                      kv_->Put(kCartTable, user_id, set.Encode(), before.context));
  CW_ASSIGN_OR_RETURN(const auto merged, Fold(after));
  return ToView(user_id, merged, static_cast<uint32_t>(after.values.size()));
}

Result<v1::Cart> CartStore::AddItem(std::string_view user_id, const v1::CartItem& item,
                                    std::string_view idempotency_key) {
  CW_RETURN_IF_ERROR(ValidateItem(item));
  // A client-supplied key makes the OR-Set tag deterministic: a retried add
  // re-inserts the same tag, which the set ignores. Exactly-once comes from
  // the data model, so no dedup table is needed.
  const std::string tag =
      idempotency_key.empty() ? RandomTag() : fmt::format("idem:{}", idempotency_key);
  return Mutate(user_id, [&](CartOrSet& set) -> Status {
    if (set.Lines().size() >= kMaxLinesPerCart) {
      return Status::FailedPrecondition("cart is full");
    }
    set.Add(tag, item.sku(), item.quantity(), item.unit_price_cents(), item.title(),
            clock_->WallMillis());
    return Status::Ok();
  });
}

Result<v1::Cart> CartStore::RemoveItem(std::string_view user_id, std::string_view sku) {
  if (sku.empty()) return Status::InvalidArgument("sku required");
  return Mutate(user_id, [&](CartOrSet& set) {
    set.Remove(sku);
    return Status::Ok();
  });
}

Result<v1::Cart> CartStore::Clear(std::string_view user_id) {
  return Mutate(user_id, [](CartOrSet& set) {
    set.Clear();
    return Status::Ok();
  });
}

}  // namespace cartwheel::cart
