// Pieces shared by both sheets of the pad: the mark and the "Sheet n of 2"
// cell, which doubles as navigation between the shop and the ops console.

export function Mark() {
  return (
    <svg className="mark" viewBox="0 0 32 32" aria-hidden="true">
      <circle cx="16" cy="16" r="10.5" />
      <circle cx="16" cy="5.5" r="2" className="mark-node" />
      <circle cx="25.6" cy="20" r="2" className="mark-node" />
      <circle cx="7" cy="21" r="2" className="mark-node" />
      <circle cx="22" cy="8" r="2.6" className="mark-own" />
    </svg>
  );
}

const SHEETS = [
  { no: 1, href: '/', label: 'Shop' },
  { no: 2, href: '/ops', label: 'Ops console' },
];

export function SheetNav({ current, keepQuery }: { current: 1 | 2; keepQuery?: string }) {
  return (
    <nav className="tb-cell tb-sheet" aria-label="Sheets">
      <span className="tb-label">Sheet {current} of 2</span>
      <span className="tb-sheets">
        {SHEETS.map((s) => (
          <a
            key={s.no}
            href={s.href + (keepQuery ?? '')}
            aria-current={s.no === current ? 'page' : undefined}
            className="figure"
          >
            {s.no} {s.label}
          </a>
        ))}
      </span>
    </nav>
  );
}
