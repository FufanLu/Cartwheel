# The lightning-deal pipeline is stuck

**What customers feel:** claims succeed at the gate, but orders stay PENDING. Or claims fail
with 503 "your unit is held, retry". No unit can be oversold either way: Postgres re-checks every
order under a row lock (illustrated/04 §4).

**Look first**

1. `cartwheel_deal_redrive_pending` (deal-svc) > 0: the order log (Kafka/Redpanda) is not
   acknowledging. Check the broker; `cartwheel_deal_publish_total{result!="delivered"}` shows
   failed vs unknown outcomes.
2. `cartwheel_order_batch_failures_total` (order-worker) rising: Postgres is refusing batches.
   The worker retries the same batch with backoff and never skips it. Its log has the SQL error.
3. Consumer lag: `kafka-consumer-groups --describe --group order-worker`. A partition with no
   consumer for more than 30 s makes the worker's watchdog recreate its consumer (log line
   "held no partition for 30 s").

**Actions**

- Broker down: bring it back. Holds are never released; the redrive queue and the hold sweeper
  publish every held order once the log answers.
- Postgres down: bring it back. Offsets were not committed, so nothing is lost.
- Never "fix" a stuck deal by releasing Redis holds by hand. A buyer may already hold that order
  id. Reset the deal (`POST /api/v1/admin/deals/{id}/reset`) only to start a new generation.
