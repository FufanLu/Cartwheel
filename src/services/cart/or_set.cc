#include "services/cart/or_set.h"

namespace cartwheel::cart {

Result<CartOrSet> CartOrSet::FromState(const v1::CartState& state) {
  CartOrSet set;
  for (const auto& add : state.adds()) {
    if (add.tag().empty() || add.sku().empty()) {
      return Status::Corruption("cart state: add without tag or sku");
    }
    set.adds_.emplace(add.tag(), add);
  }
  for (const auto& tag : state.removed_tags()) set.removed_.insert(tag);
  return set;
}

Result<CartOrSet> CartOrSet::Decode(std::string_view bytes) {
  v1::CartState state;
  if (!state.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
    return Status::Corruption("cart state: protobuf parse failed");
  }
  return FromState(state);
}

void CartOrSet::Add(std::string_view tag, std::string_view sku, uint32_t quantity,
                    int64_t unit_price_cents, std::string_view title, int64_t now_ms) {
  const std::string t(tag);
  if (quantity == 0 || adds_.contains(t) || removed_.contains(t)) return;
  v1::CartAdd add;
  add.set_tag(t);
  add.set_sku(std::string(sku));
  add.set_quantity(quantity);
  add.set_unit_price_cents(unit_price_cents);
  add.set_title(std::string(title));
  add.set_added_at_ms(now_ms);
  adds_.emplace(t, std::move(add));
}

void CartOrSet::Remove(std::string_view sku) {
  for (auto it = adds_.begin(); it != adds_.end();) {
    if (it->second.sku() == sku) {
      removed_.insert(it->first);
      it = adds_.erase(it);
    } else {
      ++it;
    }
  }
}

void CartOrSet::Clear() {
  for (const auto& [tag, add] : adds_) removed_.insert(tag);
  adds_.clear();
}

void CartOrSet::Merge(const CartOrSet& other) {
  removed_.insert(other.removed_.begin(), other.removed_.end());
  for (const auto& [tag, add] : other.adds_) adds_.emplace(tag, add);
  std::erase_if(adds_, [this](const auto& kv) { return removed_.contains(kv.first); });
}

std::vector<CartOrSet::Line> CartOrSet::Lines() const {
  std::map<std::string, Line> by_sku;
  for (const auto& [tag, add] : adds_) {
    auto& line = by_sku[add.sku()];
    line.sku = add.sku();
    line.quantity += add.quantity();
    // The most recently added entry decides the displayed price and title.
    line.unit_price_cents = add.unit_price_cents();
    line.title = add.title();
  }
  std::vector<Line> out;
  out.reserve(by_sku.size());
  for (auto& [sku, line] : by_sku) out.push_back(std::move(line));
  return out;
}

int64_t CartOrSet::TotalCents() const {
  int64_t total = 0;
  for (const auto& line : Lines()) total += line.unit_price_cents * line.quantity;
  return total;
}

size_t CartOrSet::live_adds() const {
  return adds_.size();
}

v1::CartState CartOrSet::ToState() const {
  v1::CartState state;
  for (const auto& [tag, add] : adds_) *state.add_adds() = add;
  for (const auto& tag : removed_) state.add_removed_tags(tag);
  return state;
}

std::string CartOrSet::Encode() const {
  std::string out;
  // Deterministic: maps are ordered, so equal sets encode identically.
  // Only fails above 2 GiB; carts are capped at kMaxLinesPerCart lines.
  static_cast<void>(ToState().SerializeToString(&out));
  return out;
}

}  // namespace cartwheel::cart
