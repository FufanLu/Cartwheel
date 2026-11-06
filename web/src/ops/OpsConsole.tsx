import { useEffect, useState } from 'react';
import { RotateCcw, Unplug } from 'lucide-react';
import type { Deal, OpsNode } from '../api/deals';
import { formatClock } from '../storefront/DealSheet';
import { Mark, SheetNav } from '../shell/Sheet';
import { DASHES, RateChart } from './RateChart';
import { Ring } from './Ring';
import { useCluster, type ClusterModel } from './useCluster';
import '../storefront/storefront.css';
import './ops.css';

const today = new Date().toISOString().slice(0, 10);
const ISOLATE_MS = 10_000;

function us(v: number | undefined): string {
  if (v === undefined) return '—';
  if (v === 0) return 'idle';
  return v >= 1000 ? `${(v / 1000).toFixed(1)} ms` : `${Math.round(v)} µs`;
}

function count(v: number | undefined): string {
  return v === undefined ? '—' : v.toLocaleString();
}

export function OpsConsole() {
  const m = useCluster();
  const [highlight, setHighlight] = useState<string | null>(null);
  const [, setTick] = useState(0);
  useEffect(() => {
    const t = setInterval(() => setTick((n) => n + 1), 500);
    return () => clearInterval(t);
  }, []);

  const now = Date.now();
  const isolated = new Set([...m.isolatedByYou].filter(([, until]) => until > now).map(([id]) => id));
  const nodes = m.view?.nodes ?? [];
  const last = m.history[m.history.length - 1];

  return (
    <div className="pad">
      <header className="title-block">
        <div className="tb-cell tb-name">
          <Mark />
          <h1 className="tb-title">Cartwheel ops</h1>
        </div>
        <form className="tb-cell tb-user" onSubmit={(e) => e.preventDefault()}>
          <label className="tb-label" htmlFor="admin-token">
            Admin token (for isolate and reset)
          </label>
          <input
            id="admin-token"
            value={m.adminToken}
            spellCheck={false}
            autoComplete="off"
            onChange={(e) => m.setAdminToken(e.target.value)}
          />
        </form>
        <div className="tb-cell">
          <span className="tb-label">Date</span>
          <span className="tb-value figure">{today}</span>
        </div>
        <SheetNav current={2} />
        <div className="tb-cell tb-stamp" title="Every figure on this sheet is read from the running cluster">
          Live system
        </div>
      </header>

      <main className="sheet">
        <section className="entries" aria-labelledby="ring-heading">
          <div className="entries-head">
            <h2 id="ring-heading">CartwheelKV ring</h2>
            <p className="entries-note">
              {m.view?.n !== undefined
                ? `N=${m.view.n} R=${m.view.r} W=${m.view.w} · states as seen by ${m.view.observer}`
                : m.error
                  ? `Gateway: ${m.error.status || 'no answer'} ${m.error.code}`
                  : 'Reading the cluster…'}
            </p>
          </div>

          {m.view && m.view.tokens && nodes.length > 0 ? (
            <div className="ring-wrap">
              <Ring
                tokens={m.view.tokens}
                nodes={nodes}
                isolated={isolated}
                highlight={highlight}
                onHighlight={setHighlight}
              />
              <div className="ring-notes">
                <p>
                  Each track is one node. Its arcs are the key ranges it owns as the first replica; the
                  next two distinct nodes clockwise hold the other copies.
                </p>
                <dl className="ring-key">
                  <div>
                    <dt>
                      <span className="rk rk-alive" />
                    </dt>
                    <dd>alive</dd>
                  </div>
                  <div>
                    <dt>
                      <span className="rk rk-suspect" />
                    </dt>
                    <dd>suspect (no heartbeat for 1 s)</dd>
                  </div>
                  <div>
                    <dt>
                      <span className="rk rk-dead" />
                    </dt>
                    <dd>dead (3 s): writes go to stand-ins</dd>
                  </div>
                  <div>
                    <dt>
                      <span className="rk rk-own" />
                    </dt>
                    <dd>isolated by you</dd>
                  </div>
                </dl>
              </div>
            </div>
          ) : (
            <p className="ops-empty">
              {m.error
                ? 'The gateway did not return a cluster view. Is ops.kv_nodes configured?'
                : 'Waiting for the first sample…'}
            </p>
          )}

          <div className="entries-head ops-subhead">
            <h2>Nodes</h2>
            <p className="entries-note">p99 over the last second at each node&apos;s local store</p>
          </div>
          <NodeTable model={m} nodes={nodes} rates={last?.rate ?? {}} isolated={isolated} highlight={highlight} onHighlight={setHighlight} />

          <div className="entries-head ops-subhead">
            <h2>Replica ops/s, last 60 s</h2>
          </div>
          <RateChart history={m.history} nodes={nodes.map((n) => n.id)} highlight={highlight} />
        </section>

        <aside className="margin" aria-labelledby="events-heading">
          <div className="margin-inner">
            <div className="margin-head">
              <h2 id="events-heading">Events</h2>
              <span className="margin-count num">{m.events.length}</span>
            </div>
            {m.events.length === 0 ? (
              <p className="tally-empty">
                Nothing has changed since this sheet opened. Isolate a node to watch the cluster react.
              </p>
            ) : (
              <ol className="event-log">
                {m.events.map((e) => (
                  <li key={e.seq} data-own={e.own || undefined}>
                    <span className="ev-no figure">#{e.seq}</span>
                    <span className="ev-time figure">{new Date(e.at).toTimeString().slice(0, 8)}</span>
                    <span className="ev-text">{e.text}</span>
                  </li>
                ))}
              </ol>
            )}

            <DealPipeline model={m} />

            <div className="try-notes">
              <h3>Try it</h3>
              <ol>
                <li>
                  Put load on the cluster:{' '}
                  <code className="figure">wheel-bench --workload=A --threads=32 --duration=120 --targets=…</code>
                </li>
                <li>
                  <strong>Isolate</strong> a node. Within a second it turns suspect, after three it is dead,
                  and its peers start holding hints for it.
                </li>
                <li>When the isolation ends it rejoins, and the hints drain back to it.</li>
                <li>
                  <strong>Reset</strong> a deal and claim it from the shop sheet in a second window.
                </li>
              </ol>
            </div>
          </div>
        </aside>
      </main>

      <footer className="pad-foot">
        <p>
          The gateway asks every node for its own counters and one node for the membership view, once a
          second. Rates come from the difference between two samples; nothing on this sheet is simulated.
        </p>
      </footer>
    </div>
  );
}

function NodeTable({
  model,
  nodes,
  rates,
  isolated,
  highlight,
  onHighlight,
}: {
  model: ClusterModel;
  nodes: OpsNode[];
  rates: Record<string, number | null>;
  isolated: Set<string>;
  highlight: string | null;
  onHighlight: (id: string | null) => void;
}) {
  return (
    <div className="node-table-wrap">
      <table className="node-table">
        <thead>
          <tr>
            <th scope="col">No.</th>
            <th scope="col">Node</th>
            <th scope="col">State</th>
            <th scope="col" className="nt-r">Owns</th>
            <th scope="col" className="nt-r">Ops/s</th>
            <th scope="col" className="nt-r">Read p99</th>
            <th scope="col" className="nt-r">Write p99</th>
            <th scope="col" className="nt-r">Keys</th>
            <th scope="col" className="nt-r">Hints held</th>
            <th scope="col" className="nt-r">Repairs</th>
            <th scope="col">
              Fault, 10 s
            </th>
          </tr>
        </thead>
        <tbody>
          {nodes.map((n, i) => {
            const mine = isolated.has(n.id);
            const until = model.isolatedByYou.get(n.id) ?? 0;
            const state = n.reachable ? (n.state ?? '—') : 'NO ANSWER';
            const rate = rates[n.id];
            return (
              <tr
                key={n.id}
                data-state={state}
                data-mine={mine || undefined}
                data-hl={highlight === n.id || undefined}
                onMouseEnter={() => onHighlight(n.id)}
                onMouseLeave={() => onHighlight(null)}
              >
                <td className="figure nt-no">N{i + 1}</td>
                <td className="nt-id">
                  <span className="nt-node">
                    <svg width="22" height="8" aria-hidden="true">
                      <line x1="0" y1="4" x2="22" y2="4" strokeDasharray={DASHES[i % DASHES.length]} />
                    </svg>
                    <span className="figure">{n.id}</span>
                  </span>
                  <span className="nt-addr figure">{n.address}</span>
                </td>
                <td className="nt-state figure">
                  {state}
                  {mine && <span className="nt-own">isolated · {formatClock(until - Date.now())}</span>}
                </td>
                <td className="num nt-r nt-fig" data-term="owns">
                  {n.ownership !== undefined ? `${(n.ownership * 100).toFixed(1)}%` : '—'}
                </td>
                <td className="num nt-r nt-fig" data-term="ops/s">
                  {rate === null || rate === undefined ? '—' : Math.round(rate).toLocaleString()}
                </td>
                <td className="num nt-r nt-fig" data-term="read p99">{us(n.stats?.window_get_p99_us)}</td>
                <td className="num nt-r nt-fig" data-term="write p99">{us(n.stats?.window_put_p99_us)}</td>
                <td className="num nt-r nt-fig" data-term="keys">{count(n.stats?.keys)}</td>
                <td className="num nt-r nt-fig" data-term="hints" data-hot={(n.counters?.hints_pending ?? 0) > 0 || undefined}>
                  {count(n.counters?.hints_pending)}
                </td>
                <td className="num nt-r nt-fig" data-term="repairs" title="read repairs · anti-entropy keys repaired">
                  {n.counters
                    ? `${n.counters.read_repairs.toLocaleString()} · ${n.counters.anti_entropy_keys_repaired}`
                    : '—'}
                </td>
                <td className="nt-figs-mobile figure">
                  ops/s {rate === null || rate === undefined ? '—' : Math.round(rate)} · read p99{' '}
                  {us(n.stats?.window_get_p99_us)} · write p99 {us(n.stats?.window_put_p99_us)} · hints{' '}
                  {count(n.counters?.hints_pending)} · owns{' '}
                  {n.ownership !== undefined ? `${(n.ownership * 100).toFixed(1)}%` : '—'}
                </td>
                <td className="nt-act">
                  <button
                    type="button"
                    className="btn-quiet"
                    disabled={mine || !n.reachable}
                    onClick={() => void model.isolate(n.id, ISOLATE_MS)}
                    aria-label={`Isolate ${n.id} from its peers for 10 seconds`}
                  >
                    <Unplug size={14} aria-hidden="true" /> Isolate
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}

function DealPipeline({ model }: { model: ClusterModel }) {
  const deals = model.deals;
  return (
    <section className="pipeline" aria-labelledby="pipeline-heading">
      <div className="margin-head">
        <h2 id="pipeline-heading">Deal pipeline</h2>
      </div>
      {deals === null ? (
        <p className="tally-empty">deal-svc did not answer.</p>
      ) : (
        <ol className="pipe-list">
          {deals.map((d: Deal, i) => {
            const taken = d.quantity - d.remaining;
            return (
              <li key={d.deal_id} className="pipe">
                <span className="pipe-no num">D{i + 1}</span>
                <div className="pipe-body">
                  <p className="pipe-title">
                    {d.title} <span className="figure pipe-gen">gen {d.generation}</span>
                  </p>
                  <dl className="pipe-figs">
                    <div>
                      <dt>gate taken</dt>
                      <dd className="num">{taken}</dd>
                    </div>
                    <div>
                      <dt>db committed</dt>
                      <dd className="num" data-hot={d.sold < taken || undefined}>
                        {d.sold}
                      </dd>
                    </div>
                    <div>
                      <dt>quantity</dt>
                      <dd className="num">{d.quantity}</dd>
                    </div>
                  </dl>
                </div>
                <button
                  type="button"
                  className="icon-btn"
                  aria-label={`Reset ${d.title}: 100 units, opening in 10 seconds`}
                  title="Reset: 100 units, opens in 10 s"
                  onClick={() => void model.resetDeal(d, 100, 10_000)}
                >
                  <RotateCcw size={14} aria-hidden="true" />
                </button>
              </li>
            );
          })}
        </ol>
      )}
    </section>
  );
}
