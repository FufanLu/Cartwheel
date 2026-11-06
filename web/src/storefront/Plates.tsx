import type { PlateId } from './products';

// Line-art plates drawn like a pen plotter: one stroke weight, no fills, and
// a dimension line per object. Authored for the demo catalog.

const common = {
  fill: 'none',
  stroke: 'currentColor',
  strokeWidth: 1.4,
  strokeLinecap: 'round' as const,
  strokeLinejoin: 'round' as const,
  vectorEffect: 'non-scaling-stroke' as const,
};

function Dimension({ x1, x2, y, label }: { x1: number; x2: number; y: number; label: string }) {
  return (
    <g className="plate-dim">
      <path d={`M${x1} ${y - 3}v6M${x2} ${y - 3}v6M${x1} ${y}H${x2}`} {...common} strokeWidth={0.8} />
      <text x={(x1 + x2) / 2} y={y - 4} textAnchor="middle">
        {label}
      </text>
    </g>
  );
}

const drawings: Record<PlateId, JSX.Element> = {
  kettle: (
    <>
      <path d="M36 70 C34 52 42 40 60 38 C78 40 86 52 84 70 Z" {...common} />
      <path d="M44 41 C48 31 72 31 76 41" {...common} />
      <path d="M57 32 h6 M60 32 v-4" {...common} />
      <path d="M84 60 C92 58 96 52 100 44" {...common} />
      <path d="M38 52 C24 52 24 66 37 66" {...common} />
      <path d="M32 70 H88" {...common} />
      <Dimension x1={36} x2={84} y={82} label="Ø 160" />
    </>
  ),
  mug: (
    <>
      <ellipse cx="56" cy="30" rx="20" ry="5" {...common} />
      <path d="M36 30 V66 C36 72 76 72 76 66 V30" {...common} />
      <path d="M76 38 C90 38 90 58 76 58" {...common} />
      <path d="M76 43 C84 43 84 53 76 53" {...common} />
      <path d="M40 34 C46 36 66 36 72 34" {...common} strokeDasharray="2 3" />
      <Dimension x1={36} x2={76} y={82} label="Ø 90" />
    </>
  ),
  notebook: (
    <>
      <path d="M38 20 H80 V72 H38 Z" {...common} />
      <path d="M42 20 V72" {...common} />
      {[30, 38, 46, 54, 62].map((y) => (
        <path key={y} d={`M48 ${y} H74`} {...common} strokeWidth={0.7} strokeDasharray="1 2.5" />
      ))}
      <path d="M72 20 V72" {...common} strokeWidth={2.2} />
      <path d="M80 26 C86 26 86 32 80 32" {...common} />
      <Dimension x1={38} x2={80} y={84} label="148" />
    </>
  ),
  lamp: (
    <>
      <ellipse cx="44" cy="72" rx="16" ry="4" {...common} />
      <path d="M44 68 L52 44 L78 30" {...common} />
      <circle cx="52" cy="44" r="2.6" {...common} />
      <circle cx="78" cy="30" r="2.6" {...common} />
      <path d="M78 30 L90 40 L80 52 L68 40 Z" {...common} />
      <path d="M74 52 C78 58 84 58 88 54" {...common} strokeDasharray="2 3" />
      <Dimension x1={28} x2={92} y={89} label="R 420" />
    </>
  ),
  headphones: (
    <>
      <path d="M34 54 C34 26 86 26 86 54" {...common} />
      <path d="M38 52 C38 32 82 32 82 52" {...common} strokeWidth={0.8} />
      <rect x="28" y="50" width="12" height="20" rx="5" {...common} />
      <rect x="80" y="50" width="12" height="20" rx="5" {...common} />
      <path d="M34 70 C34 80 52 78 60 82 C70 86 82 80 90 84" {...common} />
      <Dimension x1={28} x2={92} y={22} label="180" />
    </>
  ),
  tote: (
    <>
      <path d="M34 38 H86 L82 76 H38 Z" {...common} />
      <path d="M46 38 C46 18 58 18 58 38" {...common} />
      <path d="M62 38 C62 18 74 18 74 38" {...common} />
      <path d="M36 46 H84" {...common} strokeWidth={0.7} strokeDasharray="2 2" />
      <Dimension x1={34} x2={86} y={86} label="380" />
    </>
  ),
};

export function Plate({ id, title }: { id: PlateId; title: string }) {
  return (
    <svg className="plate" viewBox="0 0 120 92" role="img" aria-label={`Line drawing: ${title}`}>
      {drawings[id]}
    </svg>
  );
}
