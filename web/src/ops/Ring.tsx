import type { OpsNode } from '../api/deals';

// The consistent-hash ring drawn as concentric tracks, one per node. A token
// owns the arc from the previous token up to itself; each arc is drawn on its
// owner's track, so identity is the track's radius, not a colour. Node state
// sets the ink of the arcs (dash patterns cannot read on arcs a few px long)
// and the pattern of the track's continuous guide circle. A node the visitor
// isolated gets a red guide beside its system state, never instead of it.

const SIZE = 420;
const C = SIZE / 2;
const OUTER = 168;
const GAP = 15;

function polar(r: number, frac: number): [number, number] {
  const a = frac * 2 * Math.PI - Math.PI / 2; // 0 at twelve o'clock, clockwise
  return [C + r * Math.cos(a), C + r * Math.sin(a)];
}

function arc(r: number, from: number, to: number): string {
  const [x1, y1] = polar(r, from);
  const [x2, y2] = polar(r, to);
  const large = to - from > 0.5 ? 1 : 0;
  return `M${x1.toFixed(2)} ${y1.toFixed(2)}A${r} ${r} 0 ${large} 1 ${x2.toFixed(2)} ${y2.toFixed(2)}`;
}

export function Ring({
  tokens,
  nodes,
  isolated,
  highlight,
  onHighlight,
}: {
  tokens: [number, string][];
  nodes: OpsNode[];
  isolated: Set<string>;
  highlight: string | null;
  onHighlight: (id: string | null) => void;
}) {
  const order = nodes.map((n) => n.id);
  const radius = (id: string) => OUTER - order.indexOf(id) * GAP;
  const sorted = [...tokens].sort((a, b) => a[0] - b[0]);
  const arcs: Record<string, string[]> = {};
  sorted.forEach(([pos, id], i) => {
    const prevPos = i === 0 ? sorted[sorted.length - 1]![0] - 1 : sorted[i - 1]![0];
    (arcs[id] ??= []).push(prevPos < 0 ? `${arc(radius(id), prevPos + 1, 1)}${arc(radius(id), 0, pos)}` : arc(radius(id), prevPos, pos));
  });

  return (
    <svg
      className="ring"
      viewBox={`0 0 ${SIZE} ${SIZE}`}
      role="img"
      aria-label={`Hash ring: ${tokens.length} tokens over ${nodes.length} nodes`}
    >
      {/* 2^64 scale: ticks every 1/16 of the ring */}
      {Array.from({ length: 16 }, (_, i) => {
        const [x1, y1] = polar(OUTER + 6, i / 16);
        const [x2, y2] = polar(OUTER + (i % 4 === 0 ? 14 : 10), i / 16);
        return <line key={i} x1={x1} y1={y1} x2={x2} y2={y2} className="ring-tick" />;
      })}
      {[0, 0.25, 0.5, 0.75].map((f) => {
        const [x, y] = polar(OUTER + 24, f);
        return (
          <text key={f} x={x} y={y + 3} className="ring-scale" textAnchor="middle">
            {f === 0 ? '0' : `${f * 4}/4`}
          </text>
        );
      })}

      {nodes.map((n) => {
        const r = radius(n.id);
        const state = n.reachable ? (n.state ?? 'UNKNOWN') : 'UNREACHABLE';
        const dim = highlight !== null && highlight !== n.id;
        return (
          <g
            key={n.id}
            className="ring-track"
            data-state={state}
            data-own={isolated.has(n.id) || undefined}
            data-dim={dim || undefined}
            onMouseEnter={() => onHighlight(n.id)}
            onMouseLeave={() => onHighlight(null)}
          >
            <circle cx={C} cy={C} r={r} className="ring-guide" />
            <path d={(arcs[n.id] ?? []).join('')} className="ring-arc" />
            <text x={C - 8} y={C - r + 3} className="ring-label" textAnchor="end">
              {n.id}
            </text>
          </g>
        );
      })}

      <text x={C} y={C - 4} className="ring-center" textAnchor="middle">
        {tokens.length} tokens
      </text>
      <text x={C} y={C + 12} className="ring-center-sub" textAnchor="middle">
        {nodes.length} nodes · {tokens.length / Math.max(1, nodes.length)} vnodes each
      </text>
    </svg>
  );
}
