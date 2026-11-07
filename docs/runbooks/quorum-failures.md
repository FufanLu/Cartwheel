# Requests fail to reach quorum

**What customers feel:** cart reads or writes fail after retries (UNAVAILABLE / 503) when every
retry lands on a coordinator that cannot reach W or R replicas.

**Look first**

1. `rate(cartwheel_cluster_events_total{kind="quorum_failure"}[1m]) by (instance)`. One node
   failing alone is that node being cut off (a partition), so its peers are fine and clients
   retry elsewhere. Many nodes failing means more than N − W replicas are unreachable for some keys.
2. `wheelctl ring` from two different nodes, and compare their member states.
3. The ops console (`/ops`): isolated or dead tracks, and the events log.

**Actions**

- Isolate the cause first (network or process). Quorum failures are the system refusing to
  pretend: a write that did not reach W replicas is reported as failed, never silently half-done.
- Lowering W or R to 1 trades consistency for availability. It is a configuration change
  (`cluster.w`, `cluster.r`), not an incident action. Prefer restoring replicas.
