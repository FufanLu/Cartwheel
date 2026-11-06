---
name: Cartwheel
description: An engineer's computation pad. Graphite for what the system says, red pencil for what the visitor did.
colors:
  ink: "#1d2922"
  ink-2: "#3f5145"
  ink-3: "#4f6155"
  pencil: "#7d8e81"
  red: "#a8241c"
  red-ink: "#8f1d16"
  red-wash: "rgb(168 36 28 / 0.09)"
  red-line: "rgb(168 36 28 / 0.55)"
  paper: "#e3eed8"
  paper-deep: "#d5e5c6"
  paper-edge: "#c3d8b2"
  grid-fine: "rgb(64 112 74 / 0.13)"
  grid-major: "rgb(64 112 74 / 0.26)"
  rule: "#86a98c"
  rule-strong: "#4f7357"
typography:
  display:
    fontFamily: "B612, Helvetica Neue, Arial, sans-serif"
    fontSize: "1.5rem"
    fontWeight: 700
    lineHeight: 1.1
    letterSpacing: "-0.01em"
  headline:
    fontFamily: "B612, Helvetica Neue, Arial, sans-serif"
    fontSize: "1.125rem"
    fontWeight: 700
    lineHeight: 1.25
  body:
    fontFamily: "B612, Helvetica Neue, Arial, sans-serif"
    fontSize: "0.9375rem"
    fontWeight: 400
    lineHeight: 1.5
    fontFeature: "'tnum' 1, 'lnum' 1"
  body-sm:
    fontFamily: "B612, Helvetica Neue, Arial, sans-serif"
    fontSize: "0.8125rem"
    fontWeight: 400
    lineHeight: 1.5
  label:
    fontFamily: "B612, Helvetica Neue, Arial, sans-serif"
    fontSize: "0.75rem"
    fontWeight: 400
    lineHeight: 1.5
    letterSpacing: "0.04em"
  figure:
    fontFamily: "B612 Mono, ui-monospace, Menlo, monospace"
    fontSize: "0.75rem"
    fontWeight: 400
    lineHeight: 1.5
    fontFeature: "'tnum' 1, 'zero' 1"
rounded:
  none: "0px"
  hair: "2px"
spacing:
  sq: "8px"
  s1: "8px"
  s2: "16px"
  s3: "24px"
  s4: "32px"
  s6: "48px"
  s8: "64px"
components:
  button-add:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.paper}"
    typography: "{typography.body-sm}"
    rounded: "{rounded.hair}"
    padding: "0 16px"
    height: "40px"
  button-add-hover:
    backgroundColor: "{colors.red-ink}"
    textColor: "{colors.paper}"
  button-quiet:
    backgroundColor: "transparent"
    textColor: "{colors.ink}"
    typography: "{typography.body-sm}"
    rounded: "{rounded.hair}"
    padding: "0 8px"
    height: "32px"
  button-quiet-hover:
    backgroundColor: "{colors.paper-deep}"
  icon-button:
    backgroundColor: "transparent"
    textColor: "{colors.ink-2}"
    rounded: "{rounded.hair}"
    size: "28px"
  field-owner:
    backgroundColor: "transparent"
    textColor: "{colors.red-ink}"
    typography: "{typography.figure}"
    rounded: "{rounded.none}"
    padding: "2px 0"
  title-block:
    backgroundColor: "{colors.paper-deep}"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
    padding: "8px 16px"
  mobile-total-bar:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.paper}"
    typography: "{typography.body}"
    rounded: "{rounded.none}"
    padding: "16px"
---

# Design System: Cartwheel

## Overview

**Creative North Star: "The Computation Pad"**

Every Cartwheel surface is a sheet torn from a pale-green engineering computation pad. A fine 8px grid with a heavier line every fifth square covers the page. A ruled title block (name, owner, date, sheet n of n) runs across the top, and the content is worked in hairline-ruled tables and margins. The sheet is lettered like a drafting office: B612 for words, B612 Mono for measured data, and tabular figures everywhere so columns of numbers line up.

Two inks are on the pad and they never mix. Graphite is the system: catalog entries, readouts, ids, latencies, rules. Red pencil is the visitor: the cart they own, the quantity they chose, the field they type in, the focus ring under their hand, and the note that marks a change to their own data. A red mark fades to graphite once it becomes history. Work in progress is drawn in pencil (a pale grey with dashed underlines) and inked once the server confirms it.

The pad is flat and dense, but it is not cramped. Nothing floats, nothing is rounded beyond 2px, and depth comes only from ruling. Columns that run out of content keep their ruled lines down to the sheet edge, the way an unused part of a pad still carries its rules.

**Key Characteristics:**
- Pale green gridded paper with a ruled title block, not cards on a white or dark canvas.
- Two inks: graphite for system data, red pencil for visitor-owned state and actions.
- B612 lettering and B612 Mono figures, always with tabular numerals.
- Hairline and 1.5px rules, square corners (2px at most), no elevation.
- Every entry, line, and request carries a sequence number.
- Pending work is pencilled, then inked. Motion draws, inks, and settles. It never bounces.

## Colors

A low-chroma green-grey paper carries two inks: a near-black graphite and a single brick-red pencil.

### Primary
- **Graphite Ink** (ink): body text, headings, entry drawings, the primary add button fill, the total's double rule. It is the default voice of the system.
- **Graphite Second** (ink-2): secondary text such as specs, notes, title-block labels, column heads, and icon buttons at rest (7:1 on paper).
- **Graphite Third** (ink-3): tertiary data such as entry numbers, SKUs, readout terms, tally line numbers, and pending text (5.5:1 on paper, the lightest grey allowed for running text).
- **Pencil Grey** (pencil): the not-yet-inked state. It is used only for dashed underlines on pending lines and as the start colour of the ink-in transition. It is decorative only (2.9:1), so never set readable text in it.

### Secondary
- **Red Pencil** (red): the visitor's live marks, meaning the focus outline, the text caret, the owner field's focused underline, the merge bracket stroke, the dashed rules around a failed request, and the visitor's own node in the mark.
- **Red Ink** (red-ink): red text that must be read, such as the owner id, tally quantities, "in tally x n", the merge title, the failed status in the readout, and the add button's hover fill (7.4:1 on paper).
- **Red Wash** (red-wash): a 9% red ground behind a fresh merge note and behind text selection. It fades out as the note settles.
- **Red Line** (red-line): link underlines.

### Neutral
- **Pad Paper** (paper): the page ground and the text colour on graphite fills.
- **Deep Paper** (paper-deep): the title block ground, quiet button hover, and the entry row hover tint (at 60%).
- **Paper Edge** (paper-edge): the darkest paper tone, kept for paper-on-paper separation.
- **Fine Grid / Major Grid** (grid-fine, grid-major): the background grid lines, every 8px and every 40px.
- **Rule** (rule): hairlines between rows, dashed tally separators, the ruled continuation, and the scrollbar.
- **Strong Rule** (rule-strong): sheet and title-block frames, header underlines, quiet button borders, and the margin's double rule (4.5:1, enough for control borders).

### Named Rules
**The Two-Ink Rule.** Graphite is the system and red pencil is the visitor. Red marks only state the visitor owns or just caused: their cart, their quantities, their id, their focus, a change to their data, and the failure of their own request. System data is never red, and visitor state is never graphite while it is still news.

**The Settle Rule.** Red is news, not alarm. A red note about a past event pencils in, holds, and settles to graphite (the merge note and bracket do this over 2.4s). Only red that describes the visitor's current state stays red.

**The Pencil-Then-Ink Rule.** An optimistic, unconfirmed value is drawn in Graphite Third with a dashed Pencil underline and an honest label ("pencilled", "retry 2/3"). It becomes Graphite Ink only when the server confirms it.

## Typography

**Display Font:** B612 (with Helvetica Neue, Arial)
**Body Font:** B612 (with Helvetica Neue, Arial)
**Label/Mono Font:** B612 Mono (with ui-monospace, Menlo)

**Character:** B612 is a cockpit-display face, built for legibility under load. It letters the pad like a drafting stencil. B612 Mono sets anything measured or machine-issued, so it reads as instrument output rather than prose.

### Hierarchy
- **Display** (700, 1.5rem, 1.1, -0.01em): the sheet title in the title block only. There is no hero size, because the pad's headline is its title block.
- **Headline** (700, 1.125rem, 1.25): section heads ("Entries", "Your tally"), entry titles, the total, and unit prices.
- **Body** (400, 0.9375rem, 1.5): running text, title-block values, and the footer, held to 72ch.
- **Body Small** (400, 0.8125rem): specs, notes, buttons, tally amounts, try-it notes, and merge detail.
- **Label** (400, 0.75rem, 0.04em tracking, sentence case): title-block labels and column heads.
- **Figure** (B612 Mono 400, 0.75rem, tabular, slashed zero): SKUs, ids, request ids, idempotency keys, dates, sheet counts, entry numbers, and the readout.

### Named Rules
**The Measured-in-Mono Rule.** Machine-issued or measured values (ids, keys, SKUs, dates, calls) are set in B612 Mono with slashed zero. Money and counts stay in B612 with tabular lining figures, so decimal points stay narrow and columns still align.

**The Tabular Everywhere Rule.** The body sets tabular numerals globally. Any new number, such as a countdown, QPS, p99, or vector-clock counter, inherits it. Never switch to proportional figures for numbers that update live.

## Layout

The spatial unit is the 8px grid square (sq). All spacing steps are multiples of it (s1 to s8), row heights are whole squares (title cell 9 squares, entry row 14, tally line 5), and the ruled continuation repeats every 5 squares (40px), matching the major grid line.

A sheet is at most 1360px wide with 32px padding. The title block is a single bordered row of cells, one per field, with fractional columns (name widest, a stamp cell last). Below it, the sheet is one framed grid. On the storefront this is a main column plus a margin column (min 320px, about a third). The two are separated by the margin's double hairline, and the margin content is sticky.

**The Ruled Continuation Rule.** A column that ends before the sheet does is not left as an empty box. The leftover space fills with ruled lines at the 40px pitch in Rule, at 55% opacity, down to the frame.

**The Numbering Rule.** Nothing is unnumbered. Catalog entries carry two-digit mono numbers (01 to 06) in their own column. Tally lines are numbered in a ruled gutter cell, so a line number never runs into a quantity. Requests carry a monotonic sequence ("#5 GET ...") in the readout, instructions are ordered lists with mono markers, and the title block states "Sheet n of n". A new list, row type, or event stream must carry its own sequence.

Responsive behaviour has two steps. At 1080px or narrower, the title block wraps to three columns with the name spanning full width, the stamp is dropped, the sheet stacks to one column, and the margin's double rule turns horizontal. At 720px or narrower, the padding drops to 16px, column heads hide, entries become a plate/body/price-and-action grid with icon-only 44px add buttons, and a fixed graphite tally bar stays pinned to the bottom of the screen.

## Elevation & Depth

The pad is flat. There are no drop shadows, no blur, and no stacked layers. Depth and grouping come from ruling: 1.5px Strong Rule frames around the sheet and title block, 1px hairlines between rows, dashed hairlines between tally lines, and a tonal step to Deep Paper for the title block.

### Shadow Vocabulary
- **Margin double rule** (`box-shadow: inset 1px 0 0 var(--rule-strong), inset 4px 0 0 var(--paper), inset 5px 0 0 var(--rule-strong)`): draws the pad's two-line margin rule. It turns to the top edge when the sheet stacks. This is a ruling device. It does not create elevation.

### Named Rules
**The Ruled-Not-Raised Rule.** Separation is drawn with rules, never cast with shadows. Inset box-shadow is permitted only to draw lines, as in the margin rule.

**The Accountant's Total Rule.** A total sits under a 1.5px graphite rule and over a 3px double graphite rule.

## Shapes

The form language is square and drafted. Frames, rows, the title block, readouts, and notes have no radius. Pressable controls (add button, quiet button, icon button) take a 2px corner at most, just enough to read as a physical key. Borders come in two weights: hairline (1px) and line (1.5px). Dashed borders mean provisional or unavailable: the editable owner field, disabled quiet buttons, pending underlines, and the dashed red frame of a failed request.

Illustration is line art drawn as if by a pen plotter. It uses one 1.4px stroke in currentColor, round caps, and no fills, and each object carries a 0.8px dimension line with a mono label. The mark is a graphite ring with three graphite nodes and one larger red node, the visitor's own position on the wheel. Icons are stroke icons at 14 to 16px in currentColor.

## Components

### Buttons
- **Shape:** squared key (2px radius).
- **Primary (Add to tally):** a solid Graphite Ink fill with Paper text, bold body-small, 40px high, 16px side padding, and a leading plus icon. It is the one solid control per row, so the add reads as instantly pressable.
- **Hover / Active / Focus:** the fill turns Red Ink on hover, because the visitor is now acting. It presses down 1px when active. Focus is a 2px Red Pencil outline offset 2px (global). Transitions run at 160ms on the pad ease, and the press at 120ms.
- **Quiet:** transparent with a Strong Rule hairline, 32px minimum height, and an optional 14px leading icon. It tints to Deep Paper on hover. Disabled quiet buttons turn dashed and Graphite Third. The destructive variant ("Clear tally") turns Red Ink with a Red border only on hover.
- **Icon button:** a 28px transparent square in Graphite Second that gains a Strong Rule hairline on hover. Disabled is 40% opacity.

### Inputs / Fields
- **Style:** a title-block field written on the line. It has no box, only a dashed Strong Rule underline, set in B612 Mono in Red Ink because the value (the cart owner) belongs to the visitor.
- **Focus:** the underline becomes a solid 1.5px Red Pencil line, and the caret is Red Pencil.

### Title Block
A full-width ruled header on Deep Paper, framed in 1.5px Strong Rule, with hairline dividers between cells. Each cell stacks a Label over a value (9-square minimum height). The first cell holds the mark and the Display title. Fields are Project/name, By (owner), Date, and Sheet n of n, followed by a provenance stamp cell.

### Entry Row
A numbered catalog row: mono entry number, line-art plate (132 x 100), title, spec and SKU, right-aligned unit price, and the add action with a Red Ink "in tally x n" or "inking n..." status under it. Rows are separated by Rule hairlines and tint to Deep Paper on hover (180ms).

### Tally Line
A margin ledger line: ruled number gutter, Red Ink quantity, name with an optional "@ unit" calc, right-aligned amount, and +/- icon buttons. Lines are separated by dashed Rule hairlines. A confirmed line inks in (420ms, Pencil to Ink with a 0.6px blur clearing). A line from another writer carries a small mono "other writer" tag with a hairline border and a red bracket in the gutter. While busy, the line drops to Graphite Third.

### Merge Note
This is the signature moment. A status note opens on Red Wash with a bold Red Ink title stating the fact ("Merged 2 concurrent versions") and a Graphite Second detail. A red pencil bracket draws itself beside the affected lines (600ms stroke draw). Both hold red for about 70% of 2.4s, then settle: the note to transparent and Graphite Ink, the bracket to Graphite Second.

### Readout
A two-column mono strip between 1.5px Strong Rule borders, with hairline cell dividers. Terms are in Graphite Third and values in Graphite Ink. Wide rows (call, request id) span both columns. A notable value ("hot", such as siblings above 1) turns bold. A value describing the visitor's failed request turns bold Red Ink. Every mutation updates it.

### Request Error
An inline band in the margin with dashed Red Pencil hairlines above and below and Red Ink text. It gives the status and code in bold, then the message, then a quiet "Read again" button.

## Do's and Don'ts

### Do:
- **Do** keep the two inks apart. Graphite (ink, ink-2, ink-3) for system data, red pencil (red, red-ink) only for what the visitor owns or just caused.
- **Do** number every entry, line, and request, and give line numbers their own ruled gutter.
- **Do** draw pending state in Graphite Third with a dashed Pencil underline and an honest label, then ink it on confirmation.
- **Do** let red notes about past events settle to graphite over about 2.4s on the pad ease (cubic-bezier(0.16, 1, 0.3, 1)).
- **Do** continue the ruled lines (40px pitch, 55% opacity) under columns that end early.
- **Do** set ids, keys, SKUs, and dates in B612 Mono with slashed zero, and keep tabular figures on every number.
- **Do** honour reduced motion. The global rule collapses all animations and transitions.

### Don't:
- **Don't** cast drop shadows or raise surfaces. Inset box-shadow is for drawing rules only.
- **Don't** round corners beyond 2px, and don't round frames, rows, or notes at all.
- **Don't** set readable text in Pencil Grey (pencil). It is 2.9:1 and decorative only.
- **Don't** use red for system data or as a general alarm colour.
- **Don't** fill or shade plates. Line art is one stroke weight with a dimension line.
- **Don't** imitate Amazon in colour, type, layout signatures, or terminology.
