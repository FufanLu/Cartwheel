# PRFAQ: Cartwheel

*A working-backwards document. The press release describes the finished product from the
customer's side; the FAQ answers the questions a skeptical reviewer would ask. Cartwheel is a
portfolio project, so "customers" here are the shoppers of a hypothetical store and the
engineers who would run it.*

## Press release

**Cartwheel keeps the cart open when servers fail, and keeps a flash sale honest when everyone
clicks at once.**

*November 2026.* Online stores lose sales in two predictable moments. One is when part of
the infrastructure fails and the cart stops accepting items. The other is a limited sale at a
fixed hour, when the inventory system either collapses under the rush or sells stock it does
not have. Cartwheel is an open-source commerce backend, written in C++, built for both moments.

The shopping cart runs on CartwheelKV, a distributed key-value store in the style of the 2007
Dynamo paper. Writes go to three replicas and succeed when two answer. If a server dies, its
peers notice within seconds, accept its writes on its behalf, and hand them back when it
returns. Two devices editing the same cart at once keep both changes. In tests, killing a server
under load cost 0.075% of requests, all inside the few seconds it took to detect the failure.
Restarting it was invisible.

Lightning deals take the opposite approach. Each unit is reserved in a single atomic step, so
ten thousand people pressing "claim" in the same second get a definite answer within
milliseconds. A thousand of them get a unit; everyone else hears "sold out" in about a
millisecond. Every reservation becomes an order through a durable log and a database that
re-checks the rules, so the store cannot oversell. That holds even if the reservation cache
loses its data, a worker crashes, or the message broker freezes mid-sale.

"We wanted the cart and the sale to fail the way a customer would want them to," said the author.
"The cart should never say no. The sale should never say yes twice."

Operators get a console that draws the hash ring, shows each node's live throughput and latency,
and can cut a node off from its peers to rehearse an outage. Every request can be followed as one
trace across the edge, the services, the store and the database.

## FAQ

**Who is the customer?**
A shopper on a store that runs flash sales, and the engineer on call for that store. The
shopper's success is a cart that always works and a sale that is fair. The engineer's success
is knowing, from a dashboard or an alert, what failed and that the system already routed
around it.

**Why two consistency models? Isn't that more to operate?**
Because the requirements disagree. A cart that refuses writes during a partition loses money; a
cart that merges two devices' edits loses nothing. Inventory that accepts writes during a
partition oversells, which loses trust. Cartwheel puts each workload on the model whose failure
mode is acceptable for it, and documents why (ADR 0007, ADR 0008).

**What exactly does "never oversell" mean, and what would break it?**
- The number of confirmed orders for a deal generation never exceeds its quantity. A Postgres
  `CHECK` constraint enforces this under a row lock; the Redis gate only makes the common case
  fast.
- The failure that remains is not an oversell but a lost promise: a buyer who was told their
  unit is held, whose record never reached the log because Redis lost the hold *and* deal-svc
  crashed before redriving it.
- A transactional outbox would close that gap, at the cost of a database write on the claim path.

**How fast is it?**
Measured on one shared laptop, so treat these as floors (benchmarks.md):
- the deal path took 14.8k claims/s, sold 1,000 units in 0.28 s, and committed them all
  0.73 s after opening;
- the cart path through a node partition served 1,270 req/s with 0.04% errors;
- the storage engine reaches 62–92% of RocksDB.

**Why write a storage engine instead of using RocksDB?**
To own the failure semantics end to end, and to measure the gap honestly. The engine survives
SIGKILL and simulated power loss with no acknowledged write lost; a fuzzer found and fixed one
decoding bug. RocksDB stays wired in as a benchmark reference (ADR 0006).

**How do you know the distributed parts are correct, not just lucky in tests?**
A deterministic simulator runs the production cluster code with a seeded network that drops,
delays and partitions messages and crashes nodes. After healing, it checks three invariants:
no acknowledged write is lost, all replicas converge, no hints remain. Any failing seed replays
exactly. The same suites run under AddressSanitizer and ThreadSanitizer.

**What is not done?**
- Nodes joining the cluster do not stream data. They fill in through read repair and
  anti-entropy.
- Tombstones and stand-in copies are not garbage-collected.
- Rate limits are per gateway instance.
- The demo login has no password.

Each item is listed with its consequence in the chapter that introduces it.

**What would it take to run this for real?**
- Replication factor 3 on the broker with `min.insync.replicas=2`, and managed Postgres and Redis.
- A real identity provider in front of the token endpoint.
- Sampling for traces.
- The Helm chart already runs the KV store as a StatefulSet with a disruption budget of one node,
  so a rolling upgrade never takes away a write quorum.
