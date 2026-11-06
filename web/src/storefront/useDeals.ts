import { useCallback, useEffect, useRef, useState } from 'react';
import { dealsApi, HttpError, type Deal, type Order } from '../api/deals';

const USER_ID = /^[a-z0-9_-]{1,32}$/;
const MAX_ATTEMPTS = 4;

export type PushState = 'off' | 'connecting' | 'live' | 'retrying';

/** What the visitor's last claim on a deal is doing right now. */
export interface ClaimState {
  phase: 'sending' | 'retrying' | 'done';
  attempt: number;
  /** Short outcome line shown under the claim button. */
  note?: string;
  /** The note describes the visitor's own failed request. */
  failed?: boolean;
  seq: number;
}

export interface DealsModel {
  deals: Deal[] | null;
  /** Server clock minus local clock, so countdowns follow the server. */
  offsetMs: number;
  now: number;
  error: HttpError | null;
  authError: string | null;
  orders: Map<string, Order>;
  claims: Map<string, ClaimState>;
  push: PushState;
  /** Monotonic count of claim requests sent from this sheet. */
  seq: number;
  claim: (deal: Deal) => Promise<void>;
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

export function useDeals(user: string): DealsModel {
  const [deals, setDeals] = useState<Deal[] | null>(null);
  const [offsetMs, setOffsetMs] = useState(0);
  const [now, setNow] = useState(() => Date.now());
  const [error, setError] = useState<HttpError | null>(null);
  const [authError, setAuthError] = useState<string | null>(null);
  const [orders, setOrders] = useState<Map<string, Order>>(new Map());
  const [claims, setClaims] = useState<Map<string, ClaimState>>(new Map());
  const [push, setPush] = useState<PushState>('off');
  const [token, setToken] = useState<string | null>(null);
  const seqRef = useRef(0);
  const [seq, setSeq] = useState(0);

  // Catalog + live stock, once a second.
  useEffect(() => {
    let alive = true;
    const load = async () => {
      try {
        const sent = Date.now();
        const a = await dealsApi.list();
        if (!alive) return;
        const rtt = Date.now() - sent;
        setOffsetMs(a.body.server_time_ms - (sent + rtt / 2));
        setDeals(a.body.deals);
        setError(null);
      } catch (e) {
        if (alive) setError(e as HttpError);
      }
    };
    void load();
    const t = setInterval(load, 1000);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, []);

  // Countdown clock.
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 250);
    return () => clearInterval(t);
  }, []);

  const mergeOrders = useCallback((list: Order[]) => {
    setOrders((prev) => {
      const next = new Map(prev);
      for (const o of list) {
        const cur = next.get(o.deal_id);
        // Keep the newest generation. Within one, never downgrade: a push can
        // confirm an order before the claim's own response says "pending".
        const downgrade = cur && cur.status !== 'PENDING' && o.status === 'PENDING';
        if (
          !cur ||
          o.generation > cur.generation ||
          (o.generation === cur.generation && !downgrade &&
            (cur.status === 'PENDING' || o.order_id === cur.order_id))
        ) {
          next.set(o.deal_id, o);
        }
      }
      return next;
    });
  }, []);

  // Sign in as the cart owner (demo login: the token is what the API trusts).
  useEffect(() => {
    setToken(null);
    setOrders(new Map());
    setClaims(new Map());
    if (!USER_ID.test(user)) {
      setAuthError('Deals need an owner id of 1–32 lowercase letters, digits, _ or -.');
      return;
    }
    setAuthError(null);
    let alive = true;
    dealsApi
      .login(user)
      .then(async (a) => {
        if (!alive) return;
        setToken(a.body.token);
        const o = await dealsApi.orders(a.body.token);
        if (alive) mergeOrders(o.body.orders);
      })
      .catch((e: HttpError) => alive && setAuthError(`Sign-in failed: ${e.status} ${e.code}`));
    return () => {
      alive = false;
    };
  }, [user, mergeOrders]);

  // Order pushes over WebSocket, reconnecting with capped backoff.
  useEffect(() => {
    if (!token) {
      setPush('off');
      return;
    }
    let ws: WebSocket | null = null;
    let closed = false;
    let attempt = 0;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const open = () => {
      setPush(attempt === 0 ? 'connecting' : 'retrying');
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      ws = new WebSocket(`${proto}//${window.location.host}/api/v1/ws`, ['cartwheel.v1', token]);
      ws.onopen = () => {
        attempt = 0;
        setPush('live');
      };
      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(String(ev.data)) as { type: string; order?: Order };
          if (msg.type === 'order' && msg.order) mergeOrders([msg.order]);
        } catch {
          /* ignore malformed frames */
        }
      };
      ws.onclose = () => {
        if (closed) return;
        attempt += 1;
        setPush('retrying');
        timer = setTimeout(open, Math.random() * Math.min(5000, 250 * 2 ** attempt));
      };
    };
    open();
    return () => {
      closed = true;
      clearTimeout(timer);
      ws?.close();
    };
  }, [token, mergeOrders]);

  const setClaim = (dealId: string, c: ClaimState) =>
    setClaims((prev) => new Map(prev).set(dealId, c));

  const claim = useCallback(
    async (deal: Deal) => {
      if (!token) return;
      for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        seqRef.current += 1;
        const mySeq = seqRef.current;
        setSeq(mySeq);
        setClaim(deal.deal_id, { phase: attempt === 1 ? 'sending' : 'retrying', attempt, seq: mySeq });
        try {
          const a = await dealsApi.claim(deal.deal_id, token);
          const r = a.body;
          if (r.order_id) {
            mergeOrders([
              {
                order_id: r.order_id,
                deal_id: deal.deal_id,
                user_id: user,
                price_cents: deal.price_cents,
                status: 'PENDING',
                reserved_at_ms: Date.now() + offsetMs,
                generation: deal.generation,
                reason: '',
              },
            ]);
          }
          const note =
            r.outcome === 'RESERVED'
              ? 'Unit reserved at the gate. Waiting for the database.'
              : r.outcome === 'ALREADY_CLAIMED'
                ? 'You already hold one. One per customer.'
                : r.outcome === 'SOLD_OUT'
                  ? 'Sold out. Every unit is held by someone.'
                  : r.outcome === 'NOT_STARTED'
                    ? 'Not open yet.'
                    : 'This deal has ended.';
          setClaim(deal.deal_id, {
            phase: 'done',
            attempt,
            note,
            failed: r.outcome === 'SOLD_OUT',
            seq: mySeq,
          });
          return;
        } catch (e) {
          const err = e as HttpError;
          if (err.status === 429) {
            setClaim(deal.deal_id, {
              phase: 'done',
              attempt,
              note: `429 · held to one claim a second. Try again in ${err.retryAfterS ?? 1} s.`,
              failed: true,
              seq: mySeq,
            });
            return;
          }
          const retryable = err.status === 0 || err.status >= 500;
          if (!retryable || attempt === MAX_ATTEMPTS) {
            setClaim(deal.deal_id, {
              phase: 'done',
              attempt,
              note: `${err.status || 'no answer'} · ${err.message}`,
              failed: true,
              seq: mySeq,
            });
            return;
          }
          // Full-jitter backoff. A 503 here usually means the unit is held
          // and the log has not confirmed it yet: the retry returns the order.
          await sleep(Math.random() * Math.min(2000, 200 * 2 ** attempt));
        }
      }
    },
    [token, user, offsetMs, mergeOrders],
  );

  return { deals, offsetMs, now, error, authError, orders, claims, push, seq, claim };
}
