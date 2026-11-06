import { useCallback, useEffect, useRef, useState } from 'react';
import { ApiError, type CallTrace, type Cart, type CartItem, cartApi, newIdempotencyKey } from '../api/cart';

const POLL_MS = 2500;
const MAX_RETRIES = 3;

export interface PendingAdd {
  key: string;
  item: CartItem;
  attempt: number;
}

export interface MergeNote {
  siblings: number;
  /** SKUs whose quantity changed because of another writer, not this tab. */
  foreignSkus: string[];
  at: number;
}

export interface CartState {
  /** Monotonic number of the last request this tab made. */
  seq: number;
  cart: Cart | null;
  pending: PendingAdd[];
  trace: CallTrace | null;
  error: ApiError | null;
  merge: MergeNote | null;
  busySkus: Set<string>;
}

function quantities(cart: Cart | null): Map<string, number> {
  return new Map((cart?.items ?? []).map((i) => [i.sku, i.quantity]));
}

// SKUs whose quantity differs from what this tab expected: prev + own delta.
function foreignChanges(prev: Cart | null, next: Cart, own: Map<string, number | null>): string[] {
  const before = quantities(prev);
  const after = quantities(next);
  const skus = new Set([...before.keys(), ...after.keys()]);
  const out: string[] = [];
  for (const sku of skus) {
    const ownDelta = own.get(sku);
    const expected = ownDelta === null ? 0 : (before.get(sku) ?? 0) + (ownDelta ?? 0);
    if ((after.get(sku) ?? 0) !== expected) out.push(sku);
  }
  return out;
}

// Keep the same note object while nothing new happened, so polling does not
// restart its pencil-in animation every few seconds.
function nextMerge(prev: MergeNote | null, siblings: number, foreign: string[]): MergeNote | null {
  if (siblings <= 1 && foreign.length === 0) return prev;
  const sameForeign = foreign.length === 0 || (prev !== null && foreign.every((f) => prev.foreignSkus.includes(f)));
  if (prev && prev.siblings === Math.max(siblings, 1) && sameForeign) return prev;
  const foreignSkus = prev ? [...new Set([...prev.foreignSkus, ...foreign])] : foreign;
  return { siblings: Math.max(siblings, 1), foreignSkus, at: Date.now() };
}

// Full-jitter backoff, the same policy the C++ clients use.
function jitterDelay(attempt: number): number {
  return Math.random() * Math.min(2000, 200 * 2 ** attempt);
}

export function useCart(user: string) {
  const [state, setState] = useState<CartState>({
    seq: 0,
    cart: null,
    pending: [],
    trace: null,
    error: null,
    merge: null,
    busySkus: new Set(),
  });
  const cartRef = useRef<Cart | null>(null);
  const inflight = useRef(0);
  const seqRef = useRef(0);
  // SKUs this tab has writes in flight for. A change to one of them is ours,
  // not another writer's, even when it lands in a different response.
  const ownInflight = useRef(new Map<string, number>());
  const nextSeq = () => (seqRef.current += 1);

  const apply = useCallback((cart: Cart, trace: CallTrace, own: Map<string, number | null>, seq: number) => {
    const foreign = cartRef.current
      ? foreignChanges(cartRef.current, cart, own).filter((sku) => !ownInflight.current.has(sku))
      : [];
    cartRef.current = cart;
    setState((s) => ({
      ...s,
      seq,
      cart,
      trace,
      error: null,
      merge: nextMerge(s.merge, cart.merged_siblings, foreign),
    }));
  }, []);

  const refresh = useCallback(async () => {
    if (inflight.current > 0) return;  // a mutation response is about to land
    const seq = nextSeq();
    try {
      const { cart, trace } = await cartApi.get(user);
      if (inflight.current > 0) return;
      apply(cart, trace, new Map(), seq);
    } catch (e) {
      if (e instanceof ApiError) setState((s) => ({ ...s, error: e, trace: e.trace }));
    }
  }, [user, apply]);

  useEffect(() => {
    cartRef.current = null;
    setState((s) => ({ ...s, cart: null, merge: null, pending: [] }));
    void refresh();
    const id = window.setInterval(() => {
      if (document.visibilityState === 'visible') void refresh();
    }, POLL_MS);
    return () => window.clearInterval(id);
  }, [refresh]);

  const add = useCallback(
    async (item: CartItem) => {
      // One key per click: retries reuse it, so the server applies the add once.
      const key = newIdempotencyKey();
      inflight.current += 1;
      ownInflight.current.set(item.sku, (ownInflight.current.get(item.sku) ?? 0) + 1);
      setState((s) => ({ ...s, pending: [...s.pending, { key, item, attempt: 1 }] }));
      try {
        for (let attempt = 1; ; attempt += 1) {
          try {
            const seq = nextSeq();
            const { cart, trace } = await cartApi.add(user, item, key);
            apply(cart, trace, new Map([[item.sku, item.quantity]]), seq);
            return;
          } catch (e) {
            const err = e as ApiError;
            const retryable = err.status === 0 || err.status === 429 || err.status >= 503;
            if (!retryable || attempt >= MAX_RETRIES) {
              setState((s) => ({ ...s, error: err, trace: err.trace }));
              return;
            }
            setState((s) => ({
              ...s,
              trace: err.trace,
              pending: s.pending.map((p) => (p.key === key ? { ...p, attempt: attempt + 1 } : p)),
            }));
            const wait = err.retryAfterS ? err.retryAfterS * 1000 : jitterDelay(attempt);
            await new Promise((r) => setTimeout(r, wait));
          }
        }
      } finally {
        inflight.current -= 1;
        const left = (ownInflight.current.get(item.sku) ?? 1) - 1;
        if (left > 0) ownInflight.current.set(item.sku, left);
        else ownInflight.current.delete(item.sku);
        setState((s) => ({ ...s, pending: s.pending.filter((p) => p.key !== key) }));
      }
    },
    [user, apply],
  );

  const mutate = useCallback(
    async (sku: string | null, run: () => ReturnType<typeof cartApi.get>) => {
      inflight.current += 1;
      const own = new Map<string, number | null>();
      if (sku) own.set(sku, null);
      else for (const i of cartRef.current?.items ?? []) own.set(i.sku, null);
      setState((s) => ({ ...s, busySkus: new Set([...s.busySkus, sku ?? '*']) }));
      try {
        const seq = nextSeq();
        const { cart, trace } = await run();
        apply(cart, trace, own, seq);
      } catch (e) {
        if (e instanceof ApiError) setState((s) => ({ ...s, error: e, trace: e.trace }));
      } finally {
        inflight.current -= 1;
        setState((s) => {
          const busy = new Set(s.busySkus);
          busy.delete(sku ?? '*');
          return { ...s, busySkus: busy };
        });
      }
    },
    [apply],
  );

  const remove = useCallback((sku: string) => mutate(sku, () => cartApi.remove(user, sku)), [mutate, user]);
  const clear = useCallback(() => mutate(null, () => cartApi.clear(user)), [mutate, user]);
  const dismissMerge = useCallback(() => setState((s) => ({ ...s, merge: null })), []);

  return { ...state, add, remove, clear, refresh, dismissMerge };
}
