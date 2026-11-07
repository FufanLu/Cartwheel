# Hinted writes are piling up

**What it means:** stand-in nodes hold writes for a peer they cannot reach. The data is safe,
with W copies acknowledged, but one home replica is missing those writes until the hints are
delivered.

**Look first**

1. `sum(cartwheel_cluster_hints_pending) by (instance)`: which nodes hold hints.
2. `wheelctl -t <holder> ring` shows the `hints stored / delivered / pending` counters and the
   member states. Delivery only starts once the target is ALIVE again.
3. If the target is ALIVE and the backlog does not shrink, look at the holder's log for
   `REPLICA_WRITE` failures to that peer.

**Actions**

- Bring the target back (see [node-down](node-down.md)). Delivery runs every 500 ms per node; the
  measured drain was about 5.5k hints in under a second.
- If a node is gone for good, its hints will never drain. Anti-entropy still converges the
  replicas that own those keys. Removing permanently dead nodes from the ring is a manual step
  today (no decommission command yet).
