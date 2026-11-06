import { useCallback, useEffect, useRef, useState } from 'react';
import { dealsApi, HttpError, opsApi, type ClusterView, type Deal } from '../api/deals';

export const HISTORY_S = 60;

export interface Sample {
  t: number; // sampled_at_ms
  /** Replica operations per second per node (gets + puts at the local store). */
  rate: Record<string, number | null>;
}

export interface OpsEvent {
  seq: number;
  at: number;
  text: string;
  /** Caused by the visitor (red pencil) rather than observed (graphite). */
  own?: boolean;
}

export interface ClusterModel {
  view: ClusterView | null;
  error: HttpError | null;
  history: Sample[];
  events: OpsEvent[];
  deals: Deal[] | null;
  /** node id -> local time until which the visitor's isolation holds. */
  isolatedByYou: Map<string, number>;
  isolate: (node: string, ms: number) => Promise<void>;
  resetDeal: (deal: Deal, quantity: number, startsInMs: number) => Promise<void>;
  adminToken: string;
  setAdminToken: (t: string) => void;
}

function readToken(): string {
  try {
    return window.localStorage.getItem('cartwheel.admin') ?? 'dev-admin';
  } catch {
    return 'dev-admin';
  }
}

export function useCluster(): ClusterModel {
  const [view, setView] = useState<ClusterView | null>(null);
  const [error, setError] = useState<HttpError | null>(null);
  const [history, setHistory] = useState<Sample[]>([]);
  const [events, setEvents] = useState<OpsEvent[]>([]);
  const [deals, setDeals] = useState<Deal[] | null>(null);
  const [isolatedByYou, setIsolatedByYou] = useState<Map<string, number>>(new Map());
  const [adminToken, setAdminTokenState] = useState(readToken);
  const seq = useRef(0);
  const prev = useRef<ClusterView | null>(null);

  const log = useCallback((text: string, own = false) => {
    seq.current += 1;
    const e = { seq: seq.current, at: Date.now(), text, own };
    setEvents((list) => [e, ...list].slice(0, 40));
  }, []);

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const [c, d] = await Promise.allSettled([opsApi.cluster(), dealsApi.list()]);
        if (!alive) return;
        if (d.status === 'fulfilled') setDeals(d.value.body.deals);
        if (c.status === 'rejected') {
          setError(c.reason as HttpError);
          return;
        }
        const next = c.value.body;
        setError(null);
        const before = prev.current;
        // Rates from counter deltas between consecutive samples.
        const rate: Record<string, number | null> = {};
        for (const n of next.nodes) {
          const old = before?.nodes.find((x) => x.id === n.id);
          const dt = before ? (next.sampled_at_ms - before.sampled_at_ms) / 1000 : 0;
          if (n.stats && old?.stats && dt > 0) {
            const ops =
              n.stats.gets_total + n.stats.puts_total - old.stats.gets_total - old.stats.puts_total;
            rate[n.id] = ops >= 0 ? ops / dt : null; // a restart resets the counters
          } else {
            rate[n.id] = null;
          }
          // Observed transitions (the first sample only sets the baseline).
          if (before && old) {
            if (old.state !== n.state && n.state) {
              log(`${n.id} ${old.state ?? '?'} → ${n.state} (seen by ${next.observer})`);
            }
            if (old.reachable !== n.reachable) {
              log(`${n.id} ${n.reachable ? 'answers the gateway again' : 'stopped answering the gateway'}`);
            }
            const hp = n.counters?.hints_pending ?? 0;
            const ohp = old.counters?.hints_pending ?? 0;
            if (ohp === 0 && hp > 0) log(`${n.id} holds hints for a dead peer`);
            if (ohp > 0 && hp === 0) log(`${n.id} handed off its hints (${n.counters?.hints_delivered ?? 0} delivered so far)`);
          }
        }
        prev.current = next;
        setView(next);
        setHistory((h) => [...h, { t: next.sampled_at_ms, rate }].slice(-HISTORY_S));
      } catch {
        /* allSettled never throws */
      }
    };
    void tick();
    const t = setInterval(tick, 1000);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, [log]);

  const setAdminToken = (t: string) => {
    setAdminTokenState(t);
    try {
      window.localStorage.setItem('cartwheel.admin', t);
    } catch {
      /* ignore */
    }
  };

  const isolate = useCallback(
    async (node: string, ms: number) => {
      try {
        await opsApi.isolate(node, adminToken, ms);
        setIsolatedByYou((m) => new Map(m).set(node, Date.now() + ms));
        log(`you isolated ${node} from its peers for ${ms / 1000} s`, true);
      } catch (e) {
        const err = e as HttpError;
        log(`isolate ${node} failed: ${err.status} ${err.message}`, true);
      }
    },
    [adminToken, log],
  );

  const resetDeal = useCallback(
    async (deal: Deal, quantity: number, startsInMs: number) => {
      try {
        const a = await dealsApi.reset(deal.deal_id, adminToken, quantity, startsInMs);
        log(`you reset ${deal.deal_id}: generation ${a.body.generation}, ${quantity} units, opens in ${startsInMs / 1000} s`, true);
      } catch (e) {
        const err = e as HttpError;
        log(`reset ${deal.deal_id} failed: ${err.status} ${err.message}`, true);
      }
    },
    [adminToken, log],
  );

  return { view, error, history, events, deals, isolatedByYou, isolate, resetDeal, adminToken, setAdminToken };
}
