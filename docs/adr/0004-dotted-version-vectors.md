# ADR 0004: Dotted version vectors instead of per-node vector clocks

- Status: accepted
- Date: 2026-11-06

## Context

Dynamo (§4.4) tracks causality with vector clocks keyed by the coordinating
node. Under that scheme, two clients can read the same version and both write
through the same coordinator. The coordinator increments its own entry twice:
the first write gets `{n1:2}` and the second gets `{n1:3}`. The second clock
*descends* the first, so the first write is discarded as obsolete, even though
neither client saw the other's write. `cw-trace` reproduces this:

```
plain VV: alice={wheel-1:2} bob={wheel-1:3} -> Compare(bob, alice) = kAfter: bob 'descends' alice, so alice is DROPPED
```

Riak shipped this bug for years before adopting dotted version vectors
(Preguiça et al., 2010).

## Decision

Each stored version is a **sibling** with two parts:

- a **dot** `(node, counter)`: the unique name of the write event;
- a **causal context**: the clock the writer had read.

A sibling is obsolete only if another sibling's context contains its dot
(`src/cluster/siblings.h`). Comparing collapsed clocks never makes a sibling
obsolete. The coordinator gives each new dot a counter above every dot it
holds for that key from this node.

Two API rules follow, and both are enforced by tests:

1. **Put returns every sibling**, Riak's `return_body`. The returned context
   is the join of those siblings, so a client's next write supersedes only
   versions it was actually shown. Returning the new version's own clock is
   wrong: collapsing dot `(n,3)` into a vector entry `n:3` would also cover a
   concurrent `(n,2)` the client never saw
   (`InProcessKvTest.PutResponseShowsConcurrentSiblingsItsContextCovers`).
2. **Replication ships whole sibling sets**, not single siblings (from M3 on).
   The join is a safe read context only if a replica holding dot `(n,k)` also
   holds, or has seen superseded, every earlier concurrent dot from `n`.

## Consequences

- With 16 threads writing the same key from the same context, all 16 writes
  survive (`LocalStoreTest.ConcurrentWritersFromSameContextAllSurvive`).
- `SiblingSet::Sync` is commutative, associative, and idempotent. A
  randomized property test over 300 trials checks all three. This is what lets
  anti-entropy apply merges in any order.
- Applications must reconcile siblings. The cart folds them as an OR-Set
  (ADR 0005), and clients that do not reconcile see `merged_siblings > 1`.
