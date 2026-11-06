import { useState } from 'react';
import { HISTORY_S, type Sample } from './useCluster';

// Replica ops/s per node over the last minute. One graphite line per node,
// told apart by dash pattern (never colour) and named in the legend (five
// series would collide as end labels). Hover draws a crosshair with every node's value at that second.

export const DASHES = ['', '7 3', '2 3', '10 3 2 3', '4 2 1 2'];

const W = 640;
const H = 200;
const PAD = { l: 0, r: 8, t: 8, b: 4 };

function niceMax(v: number): number {
  if (v <= 0) return 10;
  const p = 10 ** Math.floor(Math.log10(v));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
  return 10 * p;
}

export function RateChart({
  history,
  nodes,
  highlight,
}: {
  history: Sample[];
  nodes: string[];
  highlight: string | null;
}) {
  const [hover, setHover] = useState<number | null>(null);
  const values = history.flatMap((s) => nodes.map((n) => s.rate[n] ?? 0));
  const yMax = niceMax(Math.max(0, ...values));
  const pw = W - PAD.l - PAD.r;
  const ph = H - PAD.t - PAD.b;
  // Right edge is "now"; one sample per second.
  const x = (i: number) => PAD.l + pw - (history.length - 1 - i) * (pw / (HISTORY_S - 1));
  const y = (v: number) => PAD.t + ph - (v / yMax) * ph;
  const yTicks = [0, yMax / 2, yMax];

  const line = (id: string) => {
    let d = '';
    let open = false;
    history.forEach((s, i) => {
      const v = s.rate[id];
      if (v === null || v === undefined) {
        open = false; // a gap where the node did not answer
        return;
      }
      d += `${open ? 'L' : 'M'}${x(i).toFixed(1)} ${y(v).toFixed(1)}`;
      open = true;
    });
    return d;
  };

  const hovered = hover !== null ? history[hover] : undefined;

  return (
    <figure className="rate-chart">
      <ul className="rc-legend" aria-label="Legend">
        {nodes.map((n, i) => (
          <li key={n}>
            <svg width="28" height="8" aria-hidden="true">
              <line x1="0" y1="4" x2="28" y2="4" strokeDasharray={DASHES[i % DASHES.length]} />
            </svg>
            <span className="figure">{n}</span>
          </li>
        ))}
      </ul>
      <div className="rc-plot">
      {yTicks.map((v) => (
        <span key={v} className="rc-y" style={{ top: `${(y(v) / H) * 100}%` }}>
          {Math.round(v).toLocaleString()}
        </span>
      ))}
      <svg
        viewBox={`0 0 ${W} ${H}`}
        preserveAspectRatio="none"
        role="img"
        aria-label="Replica operations per second per node, last 60 seconds"
        onMouseMove={(e) => {
          const box = e.currentTarget.getBoundingClientRect();
          const px = ((e.clientX - box.left) / box.width) * W;
          const i = history.length - 1 - Math.round((PAD.l + pw - px) / (pw / (HISTORY_S - 1)));
          setHover(i >= 0 && i < history.length ? i : null);
        }}
        onMouseLeave={() => setHover(null)}
      >
        {yTicks.map((v) => (
          <line key={v} x1={PAD.l} x2={PAD.l + pw} y1={y(v)} y2={y(v)} className="rc-grid" />
        ))}
        {nodes.map((n, i) => (
          <path
            key={n}
            d={line(n)}
            className="rc-line"
            strokeDasharray={DASHES[i % DASHES.length]}
            data-dim={(highlight !== null && highlight !== n) || undefined}
          />
        ))}
        {hovered && hover !== null && (
          <line x1={x(hover)} x2={x(hover)} y1={PAD.t} y2={PAD.t + ph} className="rc-cross" />
        )}
      </svg>
      {[60, 30, 0].map((sec) => (
        <span
          key={sec}
          className={sec === 60 ? 'rc-x rc-x-start' : 'rc-x'}
          style={{ left: `calc(3.5rem + (100% - 3.5rem) * ${(PAD.l + pw - (sec / (HISTORY_S - 1)) * pw) / W})` }}
        >
          {sec === 0 ? 'now' : `−${sec} s`}
        </span>
      ))}
      </div>
      {hovered && hover !== null && (
        <div
          className="rc-tip figure"
          style={{ left: `calc(3.5rem + (100% - 3.5rem) * ${x(hover) / W})` }}
          role="status"
        >
          <p>−{history.length - 1 - hover} s</p>
          {nodes.map((n) => (
            <p key={n}>
              {n} <span className="num">{hovered.rate[n] === null ? 'no answer' : Math.round(hovered.rate[n] ?? 0)}</span>
            </p>
          ))}
        </div>
      )}
      <figcaption className="rc-caption">
        Reads and writes executed by each node&apos;s local store per second. A gap means the node
        did not answer the gateway.
      </figcaption>
    </figure>
  );
}
