---
version: 1
slug: "web-src-storefront-storefront-tsx"
primary_target: "web/src/storefront/Storefront.tsx"
related_targets: []
---

# Storefront (web/src/storefront)

Scope: M1 storefront cart page. M5 (built) extends the same world to the deal sheet (web/src/storefront/DealSheet.tsx) and the ops console (web/src/ops/), reviewed once by the finish reviewer and fixed; components recorded in DESIGN.md.
Mode: Operate. The visitor browses ~6 demo products, adds to and removes from the cart, and watches concurrent edits merge.
Audience: distributed-systems interviewers and engineers on a laptop, plus the author demoing live. The task stays obvious, and the mechanism stays visible.
Run: unattended by user instruction ("快写吧 别和我说了"). The assigned direction was built without a decision round. Code-led (no image generation).

## Direction contract
THESIS: The shop is an engineer's computation pad. Products are entries on a ruled green sheet, and the cart is the running tally worked in the margin. This refuses the white-card retail grid and its dark-terminal opposite.
OWN-WORLD: Pale green engineering computation-pad paper, with a fine grid and a ruled header block (Project / By / Date / Sheet n of n). Graphite-grey ink for system data. Red pencil is reserved for the visitor's own actions and current state. B612 / B612 Mono lettering with tabular numerals. Hairline rules and boxes, no shadows, no rounded cards.
STORY: The visitor sees a real shop sheet, adds items, and watches the margin tally recompute. When two tabs edit at once, a red bracket notes "2 concurrent versions merged". They learn the cart never loses an add.
FIRST VIEWPORT: The header block runs full width at the top. On the left, a two-thirds column numbers the product entries 01–06 in a ruled table (entry, item, price, add). The right third is the margin tally: line items, quantities, total, and a context readout strip with the merge note. The primary action is the add button on each row.
FORM: Engineering lab notebook / computation pad, position 4 of 7, seed e9fa06a2.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance

## Raises (from declined challengers)
- From the variable-font specimen: a live readout. Every mutation updates a monospace readout strip (request id, idempotency key, sibling count), the way an axis readout tracks a slider.
- From the Saville catalog sleeve: strict numbering. Every entry, line, and request carries a sequence number. Nothing is unnumbered.
- From the orienteering map: two inks that never mix. System data stays graphite and visitor-owned state stays red pencil. The course layer never edits the terrain.
- From the vertical media feed: the next state is already there. Adds render optimistically, as a pencil-sketch pending state, and are inked when the server confirms.

## Challenger verdicts
- Midnight transit diagram: competitive on audience identification, since network routing resonates with engineers. It is kept as the reference vocabulary for the M5 hash-ring view, inside this world.
- Warm consumer app canon: competitive on product clarity. It is kept as a reminder that the add action must read as instantly pressable.
- Variable-font specimen, Saville sleeve, orienteering map, vertical feed: declined. Their raises are listed above.

## Signature interaction
Open two windows on the same user and add items in both at once. The margin tally redraws, and a red bracket labelled "merged 2 concurrent versions" pencils in beside the lines that came from the other writer, then fades to graphite.

## Unresolved
- Real product photography: none exists. Each item gets authored drawings (line-art plates) on the sheet instead of photos, and any authored content is labelled demo data.
