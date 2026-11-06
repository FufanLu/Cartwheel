# ADR 0005: The cart is an add-wins OR-Set, and idempotency comes from its tags

- Status: accepted
- Date: 2026-11-06

## Context

CartwheelKV returns concurrent siblings (ADR 0004), and the cart service has
to fold them into one cart. Dynamo's shopping cart merged siblings by union,
which the paper admits "can cause deleted items to resurface". Retries are a
second problem. A client that times out on AddItem and retries must not add
the item twice. Usually that requires a deduplication table with a TTL.

## Decision

The stored cart value is an **observed-remove set** (`src/services/cart/or_set.h`):

- Every add carries a globally unique **tag**.
- A remove records the tags it **observed** for that SKU, not the SKU itself.
- Merging takes the union of adds and the union of removed tags, then drops
  every add whose tag was removed.

The result is add-wins semantics. An add that a remover never saw survives, and
an add the remover did see stays removed, so items never resurface.

Idempotency falls out of tag identity. When the client sends an
`Idempotency-Key`, the tag becomes `idem:<key>`, and re-inserting the same tag
does nothing. No deduplication table is needed, and the guarantee holds across
replicas and partitions, because the tag travels with the data.

Cart mutations take no locks. Each one reads, folds siblings, applies the
change, and writes with the read context. A racing writer becomes a sibling,
and the next read folds it back in.

## Consequences

- 12 threads making 20 concurrent adds each lose none of the 240 adds
  (`CartStoreTest.ConcurrentAddsFromManyClientsAreNeverLost`). The HTTP
  end-to-end smoke test shows the same: 20 of 20 concurrent adds kept, with 12
  siblings at read time.
- Removed tags accumulate. Carts are capped at 100 lines, so the tombstone set
  stays small. Garbage-collecting tags that every replica has seen is a later
  optimization, and an ORSWOT (OR-Set without tombstones) would remove the
  tombstones entirely.
- Quantity is the sum of live adds, so "add one more" is a new tag. Setting an
  absolute quantity would mean "remove observed + add", which is not yet
  exposed in the API.
