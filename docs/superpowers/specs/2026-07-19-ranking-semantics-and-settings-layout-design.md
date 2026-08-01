# Ranking Semantics and Settings Layout Design

## Goal

Make Bokutachi rankings self-explanatory, show every judgment timing pair that Bokutachi supports, and restore the main-menu Settings button to a fixed footer position.

## Bokutachi Capability Boundary

Bokutachi's BMS configuration exposes ten optional timing metrics: `epg`/`lpg`, `egr`/`lgr`, `egd`/`lgd`, `ebd`/`lbd`, and `epr`/`lpr`. These represent early and late counts for PGREAT, GREAT, GOOD, BAD, and POOR. Bokutachi defines those five BMS judgments and does not expose KPOOR as a separate judgment, so the detail grid will contain five rows and will not invent a KPOOR value.

The existing native chart-PB endpoint returns the complete `scoreData`, including optional metrics, so no additional request is required.

## Ranking Table Header

The ranking list will gain a pinned header immediately above its virtualized rows. Wide mode will label all existing columns in their current order:

1. Rank
2. Player
3. EX Score
4. EX Rate
5. Lamp
6. BP
7. Max Combo
8. Achieved

Compact mode will show the labels for the columns that compact rows retain: Rank, Player, EX Rate, and Lamp.

The header and rows will share one compact-width policy and the same width constants. The recycler will expose its effective visible-item width, including its conditional scrollbar gutter, so the pinned header can use the exact width applied to virtualized rows. The header will be visible only while a successful ranking list is visible.

## Judgment Data Flow

`IrChartRankingEntry` and the score-detail presentation will add the five aggregate judgment totals returned in `scoreData.judgements`, plus optional early and late values for GOOD, BAD, and POOR alongside the existing PGREAT and GREAT pairs.

The native Bokutachi parser will read all five totals and all ten optional timing fields. A pair is available only when both its early and late values are valid nonnegative integers; a one-sided pair is treated as unavailable without rejecting the whole ranking page. Existing EX-score consistency validation for PGREAT and GREAT remains in place. When a total is available, its early and late pair must sum to that total; inconsistent timing degrades to an unavailable pair while preserving the aggregate value. Each lower judgment pair must not exceed the chart's total-note count.

For newly submitted scores, the canonical submission builder will reconstruct all five supported timing pairs from authentic replay judgment events. Press, Release, and automatic Miss judgment events participate; gauge-only and mine events do not. The full timing breakdown is eligible for upload only when every reconstructed early/late pair agrees with its corresponding aggregate judgment count. Tachi Batch Manual payloads will then include all ten optional timing metrics. KPOOR remains excluded from Tachi judgments while continuing to participate in BP, matching the existing submission policy.

Existing durable outbox rows remain unchanged because their frozen JSON payloads are not rewritten.

## Score-Detail Layout

The detail overlay will replace its four side-by-side cards with a semantic judgment table:

- A header row labels Total, Early, and Late.
- Five rows list PGREAT, GREAT, GOOD, BAD, and POOR.
- Each row places its aggregate total followed by its early and late values, with Early and Late side by side.
- Judgment labels use the established result-screen colors: cyan, lime, amber, orange, and coral.
- Early headers and values use the fast-feedback cyan; Late headers and values use the slow-feedback red.
- Missing totals or pairs render em dashes in their cells.
- A short footer explains that Bokutachi does not expose KPOOR separately and that BP remains an aggregate.

The grid remains visible when at least one supported total or timing pair is available, allowing scores without timing splits to retain useful judgment totals and older scores with only PGREAT/GREAT timing to retain those splits. The existing unavailable message is shown only when no supported judgment data is available. The modal's maximum height will increase enough to fit the five-row grid while remaining capped by safe-area-aware viewport geometry.

## Settings Footer Regression

The regression was introduced when the right-side action stack became scrollable: Settings moved into the intrinsic-height scroll content and its former flex spacer became a fixed 12-pixel gap.

The action stack will remain inside `rightScroll`, but Settings will move back to the right panel as a sibling after the flexing scroll view. The scroll view consumes remaining height, while a panel gap and bottom padding keep Settings visually separated and anchored to the bottom. This preserves scrolling for overflowing chart actions without scrolling the Settings footer away.

## Error Handling and Compatibility

- Malformed negative or non-integer optional timing values continue to reject the malformed ranking response.
- Partially absent timing pairs degrade to em dashes instead of hiding valid totals or pairs.
- Older scores with no extended timing metrics continue to show the existing unavailable message.
- Existing ranking pagination, row selection, comparison, refresh, and explicit modal dismissal behavior remain unchanged.
- No API key or credential data is added to ranking models, submission payloads, or outbox rows.

## Verification

- Extend native ranking parser tests for all five totals, all ten timing fields, partial pairs, and missing metrics.
- Extend canonical submission and Batch Manual tests to prove all supported pairs are reconstructed, validated, and emitted.
- Extend ranking modal model tests for five timing rows and partial availability.
- Extend the ranking source audit to require the pinned header, all column labels, and the semantic judgment rows.
- Add a main-menu source audit proving Settings is a sibling after `rightScroll` and not part of `rightContent`.
- Run the focused IR, view, and audit tests, then the desktop build and full configured CTest suite.
