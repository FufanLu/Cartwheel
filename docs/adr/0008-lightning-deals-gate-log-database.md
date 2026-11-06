# ADR 0008: Lightning deals as gate, log and database, with holds that are never released

- Status: accepted
- Date: 2026-11-06

## Context

A lightning deal sells a fixed quantity, one unit per customer, in a short window. Demand
arrives as a spike: ten thousand claims in the first second for a thousand units. Two
requirements pull against each other:

- **Never oversell.** Unit 1,001 of 1,000 is a real-world failure. The cart's AP model (ADR 0007)
  cannot be used here.
- **Answer the spike quickly.** Nine of ten buyers must hear "sold out" in milliseconds, and the
  database must not take 10k transactions per second.

Options considered:

1. **Postgres only.** `UPDATE ... SET sold = sold + 1 WHERE sold < quantity` per claim. This is
   correct, but every claim is a transaction on one hot row, so throughput is bounded by row-lock
   hand-off and the database fails first under the spike.
2. **Redis only.** An atomic Lua decrement is fast, but Redis is not the system of record: its
   replication is asynchronous and persistence loses up to a second.
3. **A gate, a log and a database (chosen).** Redis answers, Kafka/Redpanda makes accepted
   reservations durable and absorbs the burst, and Postgres commits in batches and re-checks
   every rule.

## Decision

- **Gate.** One Redis Lua script checks the hold, checks the stock, decrements and records
  `user -> order id`, atomically. The keys are per (deal, generation) and share a hash tag so they
  stay in one cluster slot. A missing stock key is re-armed from Postgres (`quantity − sold`).
- **Log.** An idempotent producer with `acks=all`, keyed by user id. The buyer gets 202 only after
  the log acknowledges the record.
- **Holds are never released.** If the publish fails or its outcome is unknown, the hold stays and
  the record is re-published (`RedriveQueue`) until it is acknowledged. A sweeper re-publishes any
  hold that has no stored order after 30 s, which covers a crash between Reserve and Publish.
  Compensation (releasing the unit) was implemented first and removed. It raced with buyer
  retries, which had already been given the order id as `ALREADY_CLAIMED`; deal-rush's invariant
  check caught it (illustrated/04 §3).
- **Database.** order-worker commits up to 500 records per transaction. It locks the batch's deal
  rows in id order (`FOR UPDATE`), applies one shared rule set (`DecideBatch`: generation,
  one-per-customer, quantity), inserts with one `unnest()`, and commits the offsets afterwards. The
  constraints of last resort are `CHECK (sold <= quantity)` and a partial unique index on
  (deal, generation, user) for confirmed orders.
- **Delivery** is at-least-once from Kafka, and processing is idempotent on the order id, so a
  redelivery or a redriven duplicate changes nothing. Notifications are re-sent for duplicates, so
  pushes are at-least-once too.
- **Generations.** A reset bumps the generation. Reservations and orders carry it, so leftovers of
  an earlier run cannot count against the new one.

## Consequences

- 10,000 claims for 1,000 units: 14.8k claims/s at the edge, sold out in 0.28 s, all committed
  0.73 s after the opening, no oversell (illustrated/04 §6).
- A dead worker delays commits by `session.timeout.ms` (10 s), not correctness. A frozen broker
  delays the winners' confirmations, not the sold-out answers (§7).
- The database protects itself from a gate that lost data. Redis failover tests confirm exactly
  `quantity` orders (`DatabaseBackstopsAGateThatLostItsData`).
- Accepted gaps: a hold whose record never reached the log is lost if Redis loses the hold *and*
  deal-svc crashes before the redrive succeeds. A transactional outbox in Postgres would close
  this, at the cost of a database write on the claim path. Also, units held by buyers whose orders
  are rejected later (only possible after gate data loss) are not returned to the gate.
- Operating the stack needs three stateful systems. The `memory` backend runs the same rules
  in-process for demos and unit tests.
