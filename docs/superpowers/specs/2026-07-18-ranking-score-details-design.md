# Ranking Score Details Design

## Goal

Make every Bokutachi ranking row actionable. Selecting a row opens a
score-detail overlay containing all useful detail already present in the
ranking response, without issuing another network request or disturbing the
ranking list's scroll position.

## Interaction

Selecting any row opens a second modal surface above the rankings modal. The
detail surface shows:

- Rank and player name.
- EX score, maximum EX score, percentage rate, and clear lamp.
- Early and late PGREAT counts.
- Early and late GREAT counts.
- Bad points, maximum combo, and achievement time.

Missing optional values use the existing em dash representation. The current
user retains the `You` label and accent treatment. The detail surface closes
with its Close button, Escape/Android Back, or a press outside its panel. Those
actions return to the rankings modal at the same scroll position. Closing or
refreshing the parent rankings modal also closes the detail surface.

This replaces the compact-only inline row expansion. Rows remain fixed-height
and virtualized on every viewport, and the same click behavior is available on
wide and compact layouts.

## Data Model and Parsing

`IrChartRankingEntry` will retain four additional provider-neutral judgement
counts: early PGREAT, late PGREAT, early GREAT, and late GREAT. The Tachi
ranking parser already validates the response's `epg`, `lpg`, `egr`, and `lgr`
fields to derive EX score; it will also copy the validated values into the
normalized entry.

The parser continues rejecting negative counts, counts above the chart note
count, impossible PGREAT/GREAT totals, malformed rows, and oversized
responses. No raw JSON, credential, or provider-specific response object is
stored beyond parsing.

## Presentation Boundary

`IrRankingModalModel` will expose a pure score-detail presentation builder for
a valid row index. It formats the same rank, player, score, rate, lamp, BP,
combo, and time values used by the list and adds the four judgement strings.
Invalid indices or unavailable rankings return no detail presentation.

The View layer owns whether the nested overlay is open. A row selection asks
the model for its detail presentation, binds the reusable detail views, and
presents the detail scrim after the rankings scrim in the existing
`OverlayPortal`. This keeps formatting testable without requiring a renderer
and gives the nested modal correct render and input priority.

## Layout

The detail panel is centered within the same safe-area calculation as the
rankings panel, with a narrower maximum width. Its structure is:

1. Header row: rank/player title and Close.
2. Summary row: EX score, rate, and lamp.
3. Judgement section: PGREAT and GREAT, each split into Early and Late.
4. Metadata row: BP, max combo, and achieved time.

The panel uses the existing theme, lamp colors, typography, modal shadow, and
safe-area margins. The layout compacts horizontally with the viewport rather
than changing list row heights.

## Lifecycle and Error Handling

- A row whose normalized data is valid always opens immediately.
- Invalid or stale indices are ignored safely.
- Refresh, parent close, scene cleanup, and modal destruction dismiss the
  detail overlay before clearing ranking data.
- No detail state is written to the database or ranking cache.
- The nested scrim consumes its own input so a close press cannot also select
  an underlying ranking row.

## Testing

- Extend parser tests to prove early/late PGREAT and GREAT counts survive
  normalization and malformed bounds remain rejected.
- Extend modal-model tests to prove every detail field is formatted, missing
  optional values use an em dash, and invalid indices return no presentation.
- Add a source-level interaction audit requiring row selection to open the
  detail overlay and parent close/refresh to dismiss it.
- Build the desktop target, run focused parser/modal tests, and run the full
  CTest suite.

