import { Zap } from 'lucide-react';
import { formatCents } from '../api/cart';
import type { Deal, Order } from '../api/deals';
import { Plate } from './Plates';
import { PRODUCTS } from './products';
import type { DealsModel } from './useDeals';
import './deals.css';

type Phase = 'upcoming' | 'live' | 'soldout' | 'ended';

function phaseOf(d: Deal, serverNow: number): Phase {
  if (serverNow >= d.ends_at_ms) return 'ended';
  if (serverNow < d.starts_at_ms) return 'upcoming';
  return d.remaining <= 0 ? 'soldout' : 'live';
}

/** 754_000 -> "12:34", 4_000_000 -> "1:06:40" */
export function formatClock(ms: number): string {
  const s = Math.max(0, Math.ceil(ms / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const mm = h > 0 ? String(m).padStart(2, '0') : String(m);
  return `${h > 0 ? `${h}:` : ''}${mm}:${String(sec).padStart(2, '0')}`;
}

const PHASE_LABEL: Record<Phase, string> = {
  upcoming: 'Opens in',
  live: 'Closes in',
  soldout: 'Sold out',
  ended: 'Ended',
};

const PUSH_LABEL: Record<DealsModel['push'], string> = {
  off: 'push off',
  connecting: 'push connecting',
  live: 'push live',
  retrying: 'push reconnecting',
};

export function DealSheet({ model }: { model: DealsModel }) {
  const serverNow = model.now + model.offsetMs;
  const deals = model.deals;

  return (
    <section className="deals" aria-labelledby="deals-heading">
      <div className="entries-head">
        <h2 id="deals-heading">Lightning deals</h2>
        <p className="entries-note">
          One unit per customer. A Redis gate answers; Postgres keeps the record.
          <span className="deals-push figure" data-state={model.push}>
            {PUSH_LABEL[model.push]}
          </span>
        </p>
      </div>

      {model.authError && <p className="deals-auth">{model.authError}</p>}

      {deals === null ? (
        <p className="deals-empty">
          {model.error
            ? `Deal service unavailable (${model.error.status || 'no answer'} ${model.error.code}).`
            : 'Reading deals…'}
        </p>
      ) : deals.length === 0 ? (
        <p className="deals-empty">No deals on this sheet yet. deal-svc seeds three on start.</p>
      ) : (
        <ol className="deal-list">
          {deals.map((d, i) => (
            <DealRow
              key={d.deal_id}
              no={i + 1}
              deal={d}
              phase={phaseOf(d, serverNow)}
              serverNow={serverNow}
              order={model.orders.get(d.deal_id)}
              model={model}
            />
          ))}
        </ol>
      )}
    </section>
  );
}

function DealRow({
  no,
  deal: d,
  phase,
  serverNow,
  order,
  model,
}: {
  no: number;
  deal: Deal;
  phase: Phase;
  serverNow: number;
  order: Order | undefined;
  model: DealsModel;
}) {
  const product = PRODUCTS.find((p) => p.sku === d.sku);
  const claim = model.claims.get(d.deal_id);
  const mine = order && order.generation === d.generation ? order : undefined;
  const taken = d.quantity - d.remaining;
  const busy = claim?.phase === 'sending' || claim?.phase === 'retrying';
  const canClaim = phase === 'live' && !mine && !busy && !model.authError;
  const remainingMs = phase === 'upcoming' ? d.starts_at_ms - serverNow : d.ends_at_ms - serverNow;
  const off = Math.round((1 - d.price_cents / d.list_price_cents) * 100);

  return (
    <li className="deal" data-phase={phase} data-mine={mine ? true : undefined}>
      <span className="entry-no figure" aria-hidden="true">
        D{no}
      </span>
      {product ? <Plate id={product.plate} title={d.title} /> : <span />}
      <div className="entry-body">
        <h3 className="entry-title">{d.title}</h3>
        <p className="deal-price">
          <span className="num deal-now">{formatCents(d.price_cents)}</span>
          <s className="num deal-was">{formatCents(d.list_price_cents)}</s>
          <span className="deal-off num">−{off}%</span>
        </p>
        <p className="entry-sku figure">
          {d.sku} · gen {d.generation}
        </p>
      </div>

      <div className="deal-clock" aria-live="off">
        <span className="tb-label">{PHASE_LABEL[phase]}</span>
        {(phase === 'upcoming' || phase === 'live') && (
          <span className="deal-time num">{formatClock(remainingMs)}</span>
        )}
        {(phase === 'soldout' || phase === 'ended') && (
          <span className="deal-time deal-time-done num">{d.sold} sold</span>
        )}
      </div>

      <div className="deal-stock">
        <StockScale quantity={d.quantity} taken={taken} />
        <p className="deal-stock-text num">
          <strong>{d.remaining}</strong> of {d.quantity} left
        </p>
        <p className="deal-stock-sub figure">
          <span>gate {taken} taken</span>
          <span>db {d.sold} committed</span>
        </p>
      </div>

      <div className="entry-action">
        <button
          type="button"
          className="btn-add"
          disabled={!canClaim}
          aria-label={`Claim one ${d.title}`}
          onClick={() => void model.claim(d)}
        >
          <Zap aria-hidden="true" size={16} strokeWidth={2} />
          <span>
            {busy
              ? claim?.phase === 'retrying'
                ? `Retry ${claim.attempt}/4`
                : 'Claiming…'
              : mine
                ? 'Claimed'
                : phase === 'upcoming'
                  ? `Opens ${formatClock(remainingMs)}`
                  : phase === 'soldout'
                    ? 'Sold out'
                    : phase === 'ended'
                      ? 'Ended'
                      : 'Claim one'}
          </span>
        </button>
        <OrderLine order={mine} claimNote={claim} />
      </div>
    </li>
  );
}

/** A ruled scale: one tick per tenth, inked up to the units taken. */
function StockScale({ quantity, taken }: { quantity: number; taken: number }) {
  const frac = quantity > 0 ? Math.min(1, taken / quantity) : 0;
  return (
    <svg className="stock-scale" viewBox="0 0 100 10" preserveAspectRatio="none" aria-hidden="true">
      <line x1="0" y1="5" x2="100" y2="5" className="ss-base" />
      {Array.from({ length: 11 }, (_, i) => (
        <line key={i} x1={i * 10} x2={i * 10} y1={i % 5 === 0 ? 1 : 3} y2={i % 5 === 0 ? 9 : 7} className="ss-tick" />
      ))}
      <rect x="0" y="3.5" width="100" height="3" className="ss-taken" style={{ transform: `scaleX(${frac})` }} />
    </svg>
  );
}

function OrderLine({ order, claimNote }: { order: Order | undefined; claimNote: ReturnType<DealsModel['claims']['get']> }) {
  if (order) {
    const id = order.order_id.slice(0, 14);
    if (order.status === 'PENDING') {
      return (
        <span className="deal-order deal-order-pending figure" aria-live="polite">
          <span>pencilled · in the log</span>
          <span>{id}…</span>
        </span>
      );
    }
    if (order.status === 'CONFIRMED') {
      return (
        <span className="deal-order deal-order-inked figure" aria-live="polite" key={order.order_id}>
          <span>yours · confirmed</span>
          <span>{id}…</span>
        </span>
      );
    }
    return (
      <span className="deal-order deal-order-failed figure" aria-live="polite">
        rejected · {order.reason}
      </span>
    );
  }
  if (claimNote?.note) {
    return (
      <span className="deal-order figure" data-failed={claimNote.failed || undefined} aria-live="polite">
        #{claimNote.seq} {claimNote.note}
      </span>
    );
  }
  return <span className="deal-order figure" />;
}
