// cw-trace: runs the real DVV, LocalStore and cart OR-Set code on a
// deterministic scenario and prints what happened, byte for byte. Output is
// pasted into docs/illustrated/01-life-of-an-add-to-cart.md.

#include <cctype>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "cluster/siblings.h"
#include "cluster/vector_clock.h"
#include "common/clock.h"
#include "kv/in_process_kv.h"
#include "kv/local_store.h"
#include "services/cart/cart_store.h"
#include "services/cart/or_set.h"

namespace cartwheel {
namespace {

using cluster::SiblingSet;
using cluster::VectorClock;

void HexDump(std::string_view bytes) {
  for (size_t off = 0; off < bytes.size(); off += 16) {
    std::string hex;
    std::string ascii;
    for (size_t i = off; i < off + 16; ++i) {
      if (i < bytes.size()) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        hex += fmt::format("{:02x} ", c);
        ascii += std::isprint(c) ? static_cast<char>(c) : '.';
      } else {
        hex += "   ";
      }
    }
    fmt::print("  {:04x}  {} |{}|\n", off, hex, ascii);
  }
}

void TraceEncoding() {
  fmt::print("## 1. One write, as stored bytes\n\n");
  SiblingSet s;
  s.Update("wheel-1", {}, "hello", false, 1'762'400'000'000);
  const std::string enc = s.Encode();
  fmt::print("SiblingSet after Update(\"wheel-1\", {{}}, \"hello\"): {}\n", s.ToString());
  fmt::print("encoded = {} bytes\n```\n", enc.size());
  HexDump(enc);
  fmt::print("```\n\n");
}

void TraceRace() {
  fmt::print("## 2. Two writers, one context: DVV vs plain vector clocks\n\n");
  SiblingSet s;
  s.Update("wheel-1", {}, "base", false, 1);
  const VectorClock ctx = s.Join();
  fmt::print("after base write:         {}   join = {}\n", s.ToString(), ctx.ToString());
  s.Update("wheel-1", ctx, "alice", false, 2);
  fmt::print("alice writes with {}: {}\n", ctx.ToString(), s.ToString());
  s.Update("wheel-1", ctx, "bob", false, 3);
  fmt::print("bob   writes with {}: {}\n", ctx.ToString(), s.ToString());
  fmt::print("join now = {}  -> a reader sees {} siblings\n\n", s.Join().ToString(), s.LiveCount());

  // What a plain per-node vector clock would have done.
  VectorClock alice = ctx;
  alice.Set("wheel-1", ctx.Get("wheel-1") + 1);
  VectorClock bob = alice;  // coordinator increments its own entry again
  bob.Set("wheel-1", alice.Get("wheel-1") + 1);
  const auto order = VectorClock::Compare(bob, alice);
  fmt::print(
      "plain VV: alice={} bob={} -> Compare(bob, alice) = {}\n", alice.ToString(), bob.ToString(),
      order == VectorClock::Order::kAfter ? "kAfter: bob 'descends' alice, so alice is DROPPED"
                                          : "concurrent");
  fmt::print("\n");
}

void TraceCart() {
  fmt::print("## 3. A cart race through the real CartStore\n\n");
  ManualClock clock(1'762'400'000'000LL * 1'000'000);
  kv::LocalStore store("wheel-1", storage::NewMemEngine(), &clock);
  kv::InProcessKv kv(&store);
  cart::CartStore carts(&kv, &clock);

  cart::v1::CartItem kettle;
  kettle.set_sku("CW-1001");
  kettle.set_quantity(1);
  kettle.set_unit_price_cents(3400);
  kettle.set_title("Stovetop kettle");
  (void)carts.AddItem("ada", kettle, "tab-a-1");

  // Two tabs read the same version...
  auto read = kv.Get("carts", "ada").value();
  fmt::print("both tabs read: {} sibling, context = {}\n", read.values.size(),
             VectorClock::Decode(read.context).value().ToString());

  // ...and each writes its own change with that same context.
  auto tab_a = cart::CartOrSet::Decode(read.values[0]).value();
  tab_a.Add("idem:tab-a-2", "CW-1002", 1, 1400, "Stoneware mug", 2);
  auto tab_b = cart::CartOrSet::Decode(read.values[0]).value();
  tab_b.Remove("CW-1001");
  tab_b.Add("idem:tab-b-1", "CW-1003", 2, 950, "Grid notebook", 3);
  (void)kv.Put("carts", "ada", tab_a.Encode(), read.context);
  auto after = kv.Put("carts", "ada", tab_b.Encode(), read.context).value();

  const auto raw = store.Read("carts", "ada").value().siblings;
  fmt::print("stored sibling set: {} siblings\n", raw.siblings().size());
  for (const auto& sib : raw.siblings()) {
    auto cart = cart::CartOrSet::Decode(sib.value).value();
    fmt::print("  dot ({},{}) ctx={}  adds={} tombstoned tags={}  lines:", sib.dot.node,
               sib.dot.counter, sib.context.ToString(), cart.live_adds(), cart.tombstones());
    for (const auto& l : cart.Lines()) fmt::print(" {}x{}", l.quantity, l.sku);
    fmt::print("\n");
  }
  const auto view = carts.Get("ada").value();
  fmt::print("folded view (merged_siblings={}):", view.merged_siblings());
  for (const auto& it : view.items()) fmt::print(" {}x{}", it.quantity(), it.sku());
  fmt::print("  total=${:.2f}\n", static_cast<double>(view.total_cents()) / 100.0);
  fmt::print(
      "-> tab A's mug survived, tab B's notebook survived, and tab B's removal of the kettle "
      "held.\n\n");
  (void)after;
}

}  // namespace
}  // namespace cartwheel

int main() {
  cartwheel::TraceEncoding();
  cartwheel::TraceRace();
  cartwheel::TraceCart();
  return 0;
}
