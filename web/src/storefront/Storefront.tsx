import { useEffect, useMemo, useState } from 'react';
import { Minus, Plus, Shuffle, SquareArrowOutUpRight, X } from 'lucide-react';
import { formatCents } from '../api/cart';
import { Plate } from './Plates';
import { PRODUCTS, type Product } from './products';
import { useCart } from './useCart';
import { DealSheet } from './DealSheet';
import { useDeals } from './useDeals';
import { Mark, SheetNav } from '../shell/Sheet';
import './storefront.css';

function initialUser(): string {
  const fromUrl = new URLSearchParams(window.location.search).get('user');
  if (fromUrl) return fromUrl;
  try {
    const saved = window.localStorage.getItem('cartwheel.user');
    if (saved) return saved;
  } catch {
    /* storage unavailable: fall through */
  }
  return `demo-${Math.random().toString(36).slice(2, 6)}`;
}

const today = new Date().toISOString().slice(0, 10);

export function Storefront() {
  const [user, setUser] = useState(initialUser);
  const [draftUser, setDraftUser] = useState(user);
  const cart = useCart(user);
  const deals = useDeals(user);

  useEffect(() => {
    try {
      window.localStorage.setItem('cartwheel.user', user);
    } catch {
      /* ignore */
    }
    const url = new URL(window.location.href);
    url.searchParams.set('user', user);
    window.history.replaceState(null, '', url);
  }, [user]);

  const inTally = useMemo(() => {
    const m = new Map<string, number>();
    for (const i of cart.cart?.items ?? []) m.set(i.sku, i.quantity);
    return m;
  }, [cart.cart]);

  const lineCount = cart.cart?.items.reduce((n, i) => n + i.quantity, 0) ?? 0;

  const foreignTitles = (cart.merge?.foreignSkus ?? []).map(
    (sku) => PRODUCTS.find((p) => p.sku === sku)?.title ?? sku,
  );

  const addProduct = (p: Product) =>
    cart.add({ sku: p.sku, quantity: 1, unit_price_cents: p.unit_price_cents, title: p.title });

  const raceTwoWriters = () => {
    // Two adds leave this tab at the same instant. cart-svc reads the same
    // context for both, so CartwheelKV stores two DVV siblings and the
    // response reports the merge.
    const [a, b] = [PRODUCTS[1]!, PRODUCTS[2]!];
    void addProduct(a);
    void addProduct(b);
  };

  return (
    <div className="pad">
      <header className="title-block">
        <div className="tb-cell tb-name">
          <Mark />
          <h1 className="tb-title">Cartwheel shop</h1>
        </div>
        <form
          className="tb-cell tb-user"
          onSubmit={(e) => {
            e.preventDefault();
            const next = draftUser.trim();
            if (next) setUser(next);
          }}
        >
          <label className="tb-label" htmlFor="user-id">
            By (cart owner)
          </label>
          <input
            id="user-id"
            value={draftUser}
            maxLength={64}
            spellCheck={false}
            autoComplete="off"
            onChange={(e) => setDraftUser(e.target.value)}
            onBlur={() => draftUser.trim() && setUser(draftUser.trim())}
          />
        </form>
        <div className="tb-cell">
          <span className="tb-label">Date</span>
          <span className="tb-value figure">{today}</span>
        </div>
        <SheetNav current={1} />
        <div className="tb-cell tb-stamp" title="Catalog and prices are synthetic demo data">
          Demo data
        </div>
      </header>

      <main className="sheet">
        <section className="entries" aria-labelledby="entries-heading">
          <DealSheet model={deals} />
          <div className="entries-head">
            <h2 id="entries-heading">Entries</h2>
            <p className="entries-note">Six items at list price. Prices are demo figures; nothing ships.</p>
          </div>
          <div className="entry-cols" aria-hidden="true">
            <span>Entry</span>
            <span />
            <span>Item</span>
            <span className="ec-right">Unit price</span>
            <span />
          </div>
          <ol className="entry-list">
            {PRODUCTS.map((p) => {
              const owned = inTally.get(p.sku) ?? 0;
              const pending = cart.pending.filter((x) => x.item.sku === p.sku).length;
              return (
                <li key={p.sku} className="entry" data-owned={owned > 0 || undefined}>
                  <span className="entry-no figure" aria-hidden="true">
                    {p.entry}
                  </span>
                  <Plate id={p.plate} title={p.title} />
                  <div className="entry-body">
                    <h3 className="entry-title">{p.title}</h3>
                    <p className="entry-spec">{p.spec}</p>
                    <p className="entry-sku figure">{p.sku}</p>
                  </div>
                  <div className="entry-price num">{formatCents(p.unit_price_cents)}</div>
                  <div className="entry-action">
                    <button
                      type="button"
                      className="btn-add"
                      aria-label={`Add ${p.title} to tally`}
                      onClick={() => void addProduct(p)}
                    >
                      <Plus aria-hidden="true" size={16} strokeWidth={2} />
                      <span>Add to tally</span>
                    </button>
                    <span className="entry-owned figure" aria-live="polite">
                      {pending > 0 ? `inking ${pending}…` : owned > 0 ? `in tally × ${owned}` : ''}
                    </span>
                  </div>
                </li>
              );
            })}
          </ol>
        </section>

        <aside className="margin" id="tally" aria-labelledby="tally-heading">
          <div className="margin-inner">
          <div className="margin-head">
            <h2 id="tally-heading">Your tally</h2>
            <span className="margin-count num">
              {lineCount} {lineCount === 1 ? 'unit' : 'units'}
            </span>
          </div>

          {cart.merge && (
            <div className="merge-note" role="status" key={cart.merge.at}>
              <div>
                <p className="merge-title">
                  {cart.merge.siblings > 1
                    ? `Merged ${cart.merge.siblings} concurrent versions`
                    : 'Another writer changed this cart'}
                </p>
                <p className="merge-detail">
                  {foreignTitles.length > 0 ? `Changed by another writer: ${foreignTitles.join(', ')}. ` : ''}
                  {cart.merge.siblings > 2
                    ? `All ${cart.merge.siblings} writes kept; nothing was overwritten.`
                    : 'Both writes kept; nothing was overwritten.'}
                </p>
              </div>
              <button type="button" className="icon-btn" onClick={cart.dismissMerge} aria-label="Dismiss merge note">
                <X size={14} aria-hidden="true" />
              </button>
            </div>
          )}

          {cart.cart === null && !cart.error ? (
            <p className="tally-loading">Reading cart from CartwheelKV…</p>
          ) : cart.cart && cart.cart.items.length === 0 && cart.pending.length === 0 ? (
            <p className="tally-empty">
              No lines yet. Add an entry from the sheet and it will be tallied here.
            </p>
          ) : (
            <ol className="tally">
              {cart.cart?.items.map((i, n) => {
                const foreign = cart.merge?.foreignSkus.includes(i.sku);
                const busy = cart.busySkus.has(i.sku) || cart.busySkus.has('*');
                return (
                  <li
                    key={i.sku}
                    className="tally-line"
                    data-foreign={foreign || undefined}
                    data-busy={busy || undefined}
                  >
                    {foreign && (
                      <svg
                        className="line-bracket"
                        viewBox="0 0 8 40"
                        preserveAspectRatio="none"
                        aria-hidden="true"
                        key={cart.merge?.at}
                      >
                        <path d="M7 1.5 C3 1.5 3 4 3 9 V16 C3 19 1.5 20 1 20 C1.5 20 3 21 3 24 V31 C3 36 3 38.5 7 38.5" />
                      </svg>
                    )}
                    <span className="tl-no num">{n + 1}</span>
                    <span className="tl-qty num">{i.quantity}×</span>
                    <span className="tl-title">
                      <span className="tl-name">{i.title || i.sku}</span>
                      {(i.quantity > 1 || foreign) && (
                        <span className="tl-sub">
                          {i.quantity > 1 && (
                            <span className="tl-calc num">
                              @ {formatCents(i.unit_price_cents)}
                            </span>
                          )}
                          {foreign && <span className="tl-tag">other writer</span>}
                        </span>
                      )}
                    </span>
                    <span className="tl-amount num">{formatCents(i.unit_price_cents * i.quantity)}</span>
                    <span className="tl-actions">
                      <button
                        type="button"
                        className="icon-btn"
                        aria-label={`Add one more ${i.title}`}
                        onClick={() =>
                          void cart.add({ sku: i.sku, quantity: 1, unit_price_cents: i.unit_price_cents, title: i.title })
                        }
                      >
                        <Plus size={14} aria-hidden="true" />
                      </button>
                      <button
                        type="button"
                        className="icon-btn"
                        aria-label={`Remove ${i.title} from tally`}
                        disabled={busy}
                        onClick={() => void cart.remove(i.sku)}
                      >
                        <Minus size={14} aria-hidden="true" />
                      </button>
                    </span>
                  </li>
                );
              })}
              {cart.pending.map((p, n) => (
                <li key={p.key} className="tally-line tally-pending" aria-label={`Adding ${p.item.title}`}>
                  <span className="tl-no num">{(cart.cart?.items.length ?? 0) + n + 1}</span>
                  <span className="tl-qty num">{p.item.quantity}×</span>
                  <span className="tl-title">
                    <span className="tl-name">{p.item.title}</span>
                  </span>
                  <span className="tl-amount num">
                    {p.attempt > 1 ? `retry ${p.attempt}/3` : 'pencilled'}
                  </span>
                  <span className="tl-actions" />
                </li>
              ))}
            </ol>
          )}

          <div className="tally-total">
            <span>Total</span>
            <span className="num">{formatCents(cart.cart?.total_cents ?? 0)}</span>
          </div>

          {cart.error && (
            <div className="tally-error" role="alert">
              <p>
                <strong>{cart.error.status === 0 ? 'Gateway unreachable' : `${cart.error.status} ${cart.error.code}`}</strong>
                {' · '}
                {cart.error.message}
              </p>
              <button type="button" className="btn-quiet" onClick={() => void cart.refresh()}>
                Read again
              </button>
            </div>
          )}

          <div className="margin-actions">
            <button type="button" className="btn-quiet" onClick={raceTwoWriters}>
              <Shuffle size={14} aria-hidden="true" /> Race two writers
            </button>
            <a className="btn-quiet" href={`?user=${encodeURIComponent(user)}`} target="_blank" rel="noreferrer">
              <SquareArrowOutUpRight size={14} aria-hidden="true" /> Open a second writer
            </a>
            <button
              type="button"
              className="btn-quiet btn-danger"
              disabled={!cart.cart || cart.cart.items.length === 0}
              onClick={() => void cart.clear()}
            >
              Clear tally
            </button>
          </div>

          <Readout trace={cart.trace} seq={cart.seq} siblings={cart.cart?.merged_siblings ?? 0} />

          <div className="try-notes">
            <h3>Try it</h3>
            <ol>
              <li>
                <strong>Race two writers</strong> sends two adds at the same instant. cart-svc reads the same
                version for both, so the store keeps two siblings and the tally reports the merge.
              </li>
              <li>
                <strong>Open a second writer</strong> opens this cart in a new window. Edit in both; the lines
                the other window changed are bracketed.
              </li>
              <li>
                Remove a line in one window while adding it in the other: the add survives, the removal of
                what you saw holds.
              </li>
            </ol>
          </div>
          </div>
        </aside>
      </main>

      <footer className="pad-foot">
        <p>
          This cart lives in CartwheelKV, a Dynamo-style store. Writes never wait on a lock. Concurrent edits
          are kept as siblings and folded together as an OR-Set, so an add is never lost and a removed item
          does not come back.
        </p>
        <a className="mobile-total num" href="#tally">
          Tally · {lineCount} · {formatCents(cart.cart?.total_cents ?? 0)}
        </a>
      </footer>
    </div>
  );
}

function Readout({
  trace,
  seq,
  siblings,
}: {
  trace: ReturnType<typeof useCart>['trace'];
  seq: number;
  siblings: number;
}) {
  return (
    <dl className="readout" aria-label="Last request">
      <div className="ro-wide">
        <dt>call</dt>
        <dd className="figure">{trace ? `#${seq} ${trace.method} ${trace.path.replace('/api/v1', '')}` : '—'}</dd>
      </div>
      <div className="ro-wide">
        <dt>request id</dt>
        <dd className="figure">{trace?.requestId ?? '—'}</dd>
      </div>
      <div>
        <dt>status</dt>
        <dd className="num" data-bad={trace && trace.status >= 400 ? true : undefined}>
          {trace ? trace.status || 'no answer' : '—'}
        </dd>
      </div>
      <div>
        <dt>latency</dt>
        <dd className="num">{trace ? `${trace.latencyMs.toFixed(1)} ms` : '—'}</dd>
      </div>
      <div>
        <dt>idem key</dt>
        <dd className="figure">{trace?.idempotencyKey ? trace.idempotencyKey.slice(0, 8) : '—'}</dd>
      </div>
      <div>
        <dt>siblings</dt>
        <dd className="num" data-hot={siblings > 1 || undefined}>
          {siblings}
        </dd>
      </div>
    </dl>
  );
}
