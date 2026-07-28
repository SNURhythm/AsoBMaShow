# Legacy Summary Records Presentation Design

**Status:** Approved on 2026-07-28

## Context

Legacy chart and course rows are intentionally Records-only. The atomic
migration preserves independently stored header facts such as final score,
maximum combo, final gauge, clear lamp, and course progress, while dropping
all legacy playback detail.

The Records row currently discards those useful facts at the presentation
boundary. It hard-codes the detail text to `Legacy summary`, displays
`Unavailable` whenever maximum score is absent, and therefore makes a
partially populated migrated row look empty even when its header retained
useful information.

## Goals

- Present every independently stored legacy fact that fits the existing
  Records row.
- Keep legacy rows visually consistent with modern and remote result rows.
- Use the stored clear lamp when score rank cannot be computed because legacy
  headers do not contain maximum score.
- Use a neutral em dash only when the corresponding presentation slot has no
  durable fact.
- Preserve the Records-only capability boundary.

## Non-Goals

- Do not compute or reconstruct legacy maximum score, score rank, result
  detail, provenance, gauge history, or replay input.
- Do not parse `provenance_json` to recover play options or other setup facts.
- Do not consult the current chart file or any dropped legacy playback table.
- Do not enable View Result, Watch, Retry Same, G-Battle, ghost, video,
  sharing, deletion, or IR actions for legacy rows.
- Do not change modern, remote, or Auto Play row semantics.

## Selected Design

`ResultRecordSummary` remains the single projection consumed by the Records
list. It gains optional presentation facts for final gauge and course progress;
the existing optional maximum combo, score-availability flag, and clear-lamp
availability flag remain authoritative. Factories populate these fields from
their own durable result models, including legacy header summaries.

A focused Records formatting helper owns the visible labels:

- Legacy detail joins the available facts in this order:
  `Gauge 62.5%`, `Combo 555`, and `Course 3/5`.
- The score label is the stored final score, `AUTO` for Auto Play, or `—` when
  no score was stored.
- The secondary score label is the normal score rank when score and maximum
  score are available. If score rank cannot be computed but a stored clear
  lamp exists, it shows the clear-lamp label such as `HARD CLEAR`. Otherwise it
  shows `—`.
- A legacy detail row with none of the supported facts shows `—`; it never
  shows a type label such as `Legacy summary`.
- Existing Auto Play replay detail and modern/remote `IR` plus play-option
  detail remain unchanged.

The list view binds only these formatter results. It does not inspect nested
legacy payloads or infer missing facts.

## Data Flow

1. The legacy repository reads and bounds independently stored header facts.
2. `makeLegacyChartResultRecord` or `makeLegacyCourseResultRecord` projects
   those optional facts into `ResultRecordSummary`.
3. The Records formatting helper selects only facts whose availability is
   explicit in the summary.
4. `ResultRecordListItemView::setSummary` binds the returned detail, score, and
   secondary-score labels and continues to use the existing clear-lamp color.

No step reads a BRD, current chart, provenance payload, or legacy detail row.

## Failure and Partial-Data Behavior

Each field is independent. A malformed or missing gauge does not hide a valid
score, combo, lamp, or course progress value. Course progress is displayed only
when both stored values are present and valid. Missing fields render as `—`
only when their entire presentation slot has no other durable fact.

## Verification

- A focused row test must first fail against the current hard-coded legacy
  labels.
- Chart coverage verifies stored score, gauge, combo, and clear lamp.
- Course coverage verifies stored score, gauge, combo, clear lamp, and course
  progress.
- A partial row with no supported facts verifies neutral em dashes and no
  actions.
- Existing modern, remote, Auto Play, badge-recycling, filtering, and summary
  projection tests remain green.
- The full configured CTest suite, desktop `main`, and non-deploying iOS compile
  verification run before publishing.
