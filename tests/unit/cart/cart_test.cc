#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kv/in_process_kv.h"
#include "kv/local_store.h"
#include "services/cart/cart_store.h"
#include "services/cart/or_set.h"

namespace cartwheel::cart {
namespace {

v1::CartItem Item(const std::string& sku, uint32_t qty, int64_t price = 100) {
  v1::CartItem item;
  item.set_sku(sku);
  item.set_quantity(qty);
  item.set_unit_price_cents(price);
  item.set_title("Item " + sku);
  return item;
}

uint32_t Qty(const v1::Cart& cart, const std::string& sku) {
  for (const auto& it : cart.items()) {
    if (it.sku() == sku) return it.quantity();
  }
  return 0;
}

// ---- OR-Set semantics -------------------------------------------------------

TEST(CartOrSetTest, AddsSumPerSku) {
  CartOrSet s;
  s.Add("t1", "apple", 2, 50, "Apple", 1);
  s.Add("t2", "apple", 1, 50, "Apple", 2);
  s.Add("t3", "pear", 1, 80, "Pear", 3);
  const auto lines = s.Lines();
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines[0].sku, "apple");
  EXPECT_EQ(lines[0].quantity, 3U);
  EXPECT_EQ(s.TotalCents(), 3 * 50 + 80);
}

TEST(CartOrSetTest, SameTagIsIdempotent) {
  CartOrSet s;
  s.Add("idem:k1", "apple", 1, 50, "Apple", 1);
  s.Add("idem:k1", "apple", 1, 50, "Apple", 2);
  EXPECT_EQ(s.Lines()[0].quantity, 1U);
}

// Dynamo §4.4: "deleted items can resurface". The OR-Set prevents it.
TEST(CartOrSetTest, RemovedItemDoesNotResurfaceOnMerge) {
  CartOrSet replica_a;
  replica_a.Add("t1", "apple", 1, 50, "Apple", 1);
  CartOrSet replica_b = replica_a;  // both have the apple

  replica_a.Remove("apple");  // user removes it on A
  replica_b.Merge(replica_a);
  replica_a.Merge(replica_b);
  EXPECT_TRUE(replica_a.Lines().empty());
  EXPECT_TRUE(replica_b.Lines().empty());
}

TEST(CartOrSetTest, ConcurrentAddSurvivesRemove) {
  CartOrSet a;
  a.Add("t1", "apple", 1, 50, "Apple", 1);
  CartOrSet b = a;
  a.Remove("apple");                        // A removed the apple it saw
  b.Add("t2", "apple", 1, 50, "Apple", 2);  // B concurrently added another
  a.Merge(b);
  ASSERT_EQ(a.Lines().size(), 1U);
  EXPECT_EQ(a.Lines()[0].quantity, 1U) << "add-wins: only the unseen add survives";
}

TEST(CartOrSetTest, EncodeDecodeIsDeterministic) {
  CartOrSet s;
  s.Add("b", "pear", 1, 80, "Pear", 2);
  s.Add("a", "apple", 2, 50, "Apple", 1);
  s.Remove("pear");
  auto decoded = CartOrSet::Decode(s.Encode());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->Encode(), s.Encode());
  EXPECT_TRUE(CartOrSet::Decode("\xff\xff garbage").status().IsCorruption());
}

// ---- CartStore on the real KV logic ---------------------------------------

class CartStoreTest : public ::testing::Test {
 protected:
  kv::LocalStore local_{"n1", storage::NewMemEngine()};
  kv::InProcessKv kv_{&local_};
  CartStore carts_{&kv_};
};

TEST_F(CartStoreTest, AddRemoveClear) {
  ASSERT_TRUE(carts_.AddItem("u1", Item("apple", 2), "").ok());
  auto cart = carts_.AddItem("u1", Item("pear", 1, 300), "");
  ASSERT_TRUE(cart.ok());
  EXPECT_EQ(cart->items_size(), 2);
  EXPECT_EQ(cart->total_cents(), 2 * 100 + 300);

  cart = carts_.RemoveItem("u1", "apple");
  ASSERT_TRUE(cart.ok());
  EXPECT_EQ(Qty(*cart, "apple"), 0U);
  EXPECT_EQ(Qty(*cart, "pear"), 1U);

  cart = carts_.Clear("u1");
  ASSERT_TRUE(cart.ok());
  EXPECT_EQ(cart->items_size(), 0);
  EXPECT_EQ(carts_.Get("u1")->items_size(), 0);
}

TEST_F(CartStoreTest, RetriedAddWithIdempotencyKeyAppliesOnce) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    ASSERT_TRUE(carts_.AddItem("u1", Item("apple", 1), "req-42").ok());
  }
  EXPECT_EQ(Qty(*carts_.Get("u1"), "apple"), 1U);
}

// Two devices add to the same cart at the same moment. No locks anywhere.
// Both writes become DVV siblings, and the read folds them as an OR-Set.
TEST_F(CartStoreTest, ConcurrentAddsFromManyClientsAreNeverLost) {
  constexpr int kClients = 12;
  constexpr int kAddsEach = 20;
  std::vector<std::thread> threads;
  threads.reserve(kClients);
  for (int c = 0; c < kClients; ++c) {
    threads.emplace_back([this, c] {
      for (int i = 0; i < kAddsEach; ++i) {
        ASSERT_TRUE(carts_.AddItem("shared", Item("sku-" + std::to_string(c), 1), "").ok());
      }
    });
  }
  for (auto& t : threads) t.join();
  const auto cart = carts_.Get("shared");
  ASSERT_TRUE(cart.ok());
  uint32_t total = 0;
  for (const auto& it : cart->items()) total += it.quantity();
  EXPECT_EQ(total, static_cast<uint32_t>(kClients * kAddsEach));
  EXPECT_EQ(cart->items_size(), kClients);
}

TEST_F(CartStoreTest, ValidatesInput) {
  EXPECT_EQ(carts_.AddItem("", Item("a", 1), "").status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(carts_.AddItem("u", Item("a", 0), "").status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(carts_.AddItem("u", Item("a", 100), "").status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(carts_.AddItem("u", Item("", 1), "").status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace cartwheel::cart
