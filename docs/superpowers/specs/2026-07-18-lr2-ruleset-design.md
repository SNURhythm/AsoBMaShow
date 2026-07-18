# Selectable LR2 and Beatoraja Rulesets

**Date:** 2026-07-18

**Status:** Approved design

**Target branch:** `develop`

**Implementation branch:** `feature/bokutachi-ir`

## Context

AsoBMaShow currently has one implicit gameplay model. Its chart-rank judge
windows are Beatoraja-derived, its gauges use Beatoraja mode-specific tables,
and its score provenance describes that single model. The new Bokutachi
integration can submit BMS 7K and 14K scores through Tachi Direct Manual, but
those scores need to come from LR2-compatible gameplay rather than merely be
formatted like LR2 scores after the play has ended.

LR2 compatibility is not only a replacement timing table. OpenLR2 and
LR2oraja show observable differences in all of the following areas:

- chart-rank judge windows;
- the early empty-POOR/KPoor window and automatic late POOR;
- same-lane multi-BAD behavior;
- long-note head and tail judgment;
- groove, easy, hard, ex-hard, hazard, and course gauges;
- non-integer and missing `TOTAL` handling;
- low-`TOTAL` and low-note-count hard-gauge damage;
- the survival-gauge death threshold.

The application therefore needs selectable, complete gameplay rulesets. The
user-facing choices are **LR2** and **Beatoraja**, with LR2 as the default.
Bokutachi submission is allowed only when the completed attempt proves that it
used canonical LR2 behavior.

## Goals

- Add a per-profile ruleset selector labeled `LR2` and `Beatoraja`.
- Make LR2 the default for settings that do not contain a valid selection.
- Implement source-compatible LR2 judgment, long-note, gauge, and `TOTAL`
  behavior.
- Preserve current gameplay as the Beatoraja ruleset.
- Prevent user-selectable mixed judge/gauge combinations during ordinary play.
- Make live play, practice, autoplay, replay, and result persistence consume
  the same compiled ruleset policy.
- Record enough provenance to reproduce a play and prove or reject Bokutachi
  eligibility.
- Ensure that no Beatoraja, modified, unknown, or legacy-unverified score is
  silently submitted to Bokutachi.
- Preserve the durable IR outbox guarantee without putting API keys or other
  credentials in outbox rows.

## Non-goals

- Replacing the modular IR driver architecture.
- Implementing LR2IR network or archive reading.
- Submitting course results to Bokutachi.
- Adding independently selectable judge and gauge models.
- Making Tachi settings implicitly change gameplay.
- Deploying mobile builds as part of this work.
- Editing the amalgamated BMS parser in this repository.

## Source-of-truth behavior

The design uses LR2oraja's LR2 rules as the maintainable reference and OpenLR2
as corroboration of original LR2 behavior. Where a later LR2oraja release note
documents a compatibility correction, the corrected behavior is intentional.

The source versions used for this design are:

- LR2oraja commit `3db78adff969b854fb3bcc68966449bd36cf7a5b`;
- LR2oraja 0.8.5 compatibility release commit
  `756a537352830c32523fceb6b72ca6416c72a430`;
- OpenLR2 commit `aa5500bd331d88d0cc2d81afe52c71939cc896c1`.

The implementation will encode the resulting rules in AsoBMaShow and cover
them with source-derived golden tests. It will not copy upstream runtime
structure or add upstream repositories as dependencies.

## Chosen approach

Add a first-class gameplay-ruleset policy. A ruleset is one coherent bundle
that owns:

- normal-note and scratch judge windows;
- long-note and long-scratch tail windows;
- KPoor, automatic POOR, candidate selection, and multi-BAD semantics;
- long-note behavior for LN, CN, and HCN;
- gauge tables, floors, borders, death thresholds, and guts;
- effective `TOTAL` resolution;
- a stable provenance identity and revision.

This is preferred to independent judge/gauge switches because independent
switches would permit combinations that cannot be described as LR2 or
Beatoraja. It is preferred to duplicate gameplay engines because the duplicate
paths would drift and would make replay parity harder to prove.

Explicitly authored course constraints remain inputs to the selected policy.
They do not expose a general user-facing mixed ruleset. LR2 uses LR2 course
gauges by default. Beatoraja keeps the existing course-constraint behavior,
including an explicitly authored LR2 gauge constraint. Course results are not
Bokutachi-eligible.

## Domain model

Introduce a stable persisted selection:

```cpp
enum class GameplayRuleset {
  LR2,
  Beatoraja,
};
```

Canonical persisted IDs and labels are:

| Enum | Persisted ID | UI label |
|---|---|---|
| `GameplayRuleset::LR2` | `lr2` | LR2 |
| `GameplayRuleset::Beatoraja` | `beatoraja` | Beatoraja |

Parsing is strict and centralized. The profile setting uses LR2 when the value
is absent or invalid. Stored attempt provenance is different: an unknown
provenance ID is never silently converted to LR2 or Beatoraja.

The registry returns immutable value objects:

```cpp
struct GameplayJudgeRules;
struct GameplayGaugeRules;

struct GameplayRulesetPolicy {
  GameplayRuleset id;
  RulesetDescriptor descriptor;
  GameplayJudgeRules judge;
  GameplayGaugeRules gauge;
};
```

The exact names may change during implementation, but these boundaries must
remain. Policies are resolved and compiled at play start. The per-input hot
path does not perform string parsing, registry lookup, or virtual dispatch.

## Ruleset identity and provenance

`RulesetDescriptor` gains a stable ruleset ID and continues to carry revision
and model names. The supported descriptors are conceptually:

| ID | Revision | Scoring | Judgment | Gauge |
|---|---:|---|---|---|
| `lr2` | 3 | `asobmashow-v1` | `lr2-v1` | `lr2-gauge-v1` |
| `beatoraja` | 2 | `asobmashow-v1` | `bms-rank-v1` | `beatoraja-profile-gauge-v2` |
| `legacy-unknown` | 0 | `legacy-unknown` | `legacy-unknown` | `legacy-unknown` |

The LR2 revision becomes the current default. The existing version-2
descriptor retains its exact values and maps to Beatoraja. Deserialization of
old provenance without an explicit ID infers Beatoraja only when the existing
version and model tuple match; version zero remains legacy-unverified.

Both supported rulesets may produce locally verified scores. Local
`ScoreEligibility` must not define "verified" as "equal to the default
ruleset." Unsupported rulesets, autoplay, practice, assist modifiers, unknown
judge sources, non-neutral playback, non-100-percent judge scaling, and custom
starting gauge values remain modified or unverified as appropriate.

Each score stage records:

- ruleset ID and revision through its containing provenance;
- source judge rank and rank source;
- effective judge windows for normal notes, scratches, LN tails, and scratch
  LN tails;
- effective integer LR2 `TOTAL`, or the effective Beatoraja total;
- the candidate-selection mode needed to reproduce Beatoraja play;
- existing gauge, auto-shift, note-option, assist, input, and playback data.

Judge-window provenance needs a note-context discriminator. Existing records
that contain only one window set map that set to every context required by the
Beatoraja replay path, which matches the current one-table implementation.

## Profile settings and UI

Add `selectedGameplayRuleset` to `AppSettings`. Settings schema migration and
sanitization follow these rules:

- a missing field becomes `lr2`;
- `lr2` remains `lr2`;
- `beatoraja` remains `beatoraja`;
- any other value becomes `lr2` and emits a non-secret diagnostic.

Because application settings are stored per player profile, profile create,
duplicate, export, import, and activation naturally preserve the selection.
New and upgraded profiles default to LR2 as explicitly requested.

The Play Options modal adds a two-button `Ruleset` section above `Gauge`.
Selecting a button saves the active profile immediately using the same pattern
as gauge and play-option controls. Switching rulesets keeps the selected gauge
type and auto-shift option but changes their behavior through the new policy.

The ready-screen settings summary includes the active ruleset. The displayed
`TOTAL` is resolved through the selected policy, so switching between LR2 and
Beatoraja updates the value immediately for charts without an authored
`TOTAL`, and displays the floored authored value under LR2.

Tachi enablement and auto-submit controls do not alter the ruleset. The IR
settings presentation may explain that only canonical LR2 plays are eligible,
but it is not a second ruleset selector.

## Play-start and replay selection

`StartOptions` carries `GameplayRuleset` directly for new play. The ruleset is
snapshotted before chart simulation, judge construction, or gauge construction
begins.

- Ordinary play uses the active profile selection.
- Practice uses the active selection plus practice modifiers and is marked
  modified.
- A course snapshots one ruleset at course creation and keeps it for every
  stage.
- Replay playback derives the ruleset from recorded provenance, ignoring the
  current profile selection.
- Result-scene retry keeps the ruleset of the completed attempt.
- A fresh play launched from chart selection uses the current profile
  selection.

Unknown future replay rulesets do not fall back. The score and replay metadata
remain viewable, but starting replay reports that the ruleset revision is not
supported.

## LR2 judgment windows

All listed boundaries are inclusive. Timing below uses AsoBMaShow's convention
`input time - note time`, where negative is early and positive is late.

### Normal notes and scratches

Normal notes and scratches share the same LR2 windows:

| BMS rank | Display name | PGREAT | GREAT | GOOD |
|---:|---|---:|---:|---:|
| 0 | Very Hard | ±8 ms | ±24 ms | ±40 ms |
| 1 | Hard | ±15 ms | ±30 ms | ±60 ms |
| 2 | Normal | ±18 ms | ±40 ms | ±100 ms |
| 3 | Easy | ±21 ms | ±60 ms | ±120 ms |
| 4 | Very Easy | ±18 ms | ±40 ms | ±100 ms |

Missing, negative, or otherwise unsupported BMS rank values use Normal. BAD is
always `[-200 ms, +200 ms]` and is not chart-rank-scaled.

### KPoor and automatic POOR

The LR2 empty-POOR window is `[-1000 ms, 0 ms]` in AsoBMaShow timing
coordinates. It is evaluated after PGREAT, GREAT, GOOD, and BAD, so its
effective region is the early part not already claimed by those judgments.

This judgment maps to AsoBMaShow `Kpoor` and has these semantics:

- it does not consume or vanish the target note;
- it applies KPoor gauge damage;
- it does not increment or break combo;
- it may be produced repeatedly against the same future note;
- it is never a late judgment.

An unplayed note remains hittable at the inclusive `+200 ms` BAD boundary. It
becomes automatic `Poor` strictly after that boundary. Automatic POOR consumes
the note and breaks combo. The compiled judge exposes the explicit automatic
miss deadline instead of deriving it from a differently scaled window.

### Candidate selection and multi-BAD

LR2 fixes candidate selection to LR2 behavior. The application-level
Beatoraja note-priority setting does not alter LR2 play. Beatoraja continues to
use the existing note-priority modes and records the effective selection for
replay.

LR2 same-lane candidate resolution reproduces the multi-BAD behavior described
and implemented by LR2oraja 0.8.5. A single press may apply BAD to additional
nearby notes in the BAD region as well as judge the selected note. The precise
selection and filtering order is ported from the reference and tested with
golden note clusters. It must not be approximated as "choose one nearest
note."

### Long notes

LR2 LN heads use the normal-note table with one exception: the late-BAD portion
of the head window is not accepted. An LN head hit later than the GOOD window
does not become a late BAD; if it remains unplayed it later becomes automatic
POOR after the standard deadline. The early-BAD region remains available.

Normal and scratch LN tail tables are:

| Tail judgment category | Window |
|---|---:|
| PGREAT | ±120 ms |
| GREAT | ±120 ms |
| GOOD | ±120 ms |
| BAD | ±200 ms |

Because evaluation is ordered, a release inside ±120 ms supplies PGREAT for
the tail comparison. Classic LN emits one final note judgment:

- holding through the tail preserves the stored head judgment;
- a release within ±120 ms preserves the head judgment;
- a release outside the ±120 ms tolerance yields BAD;
- the final classic-LN result is the worse applicable head/tail result.

CN and HCN keep separate head and tail judgments. Their release handling uses
the LR2oraja LR2 tail tables and state transitions, including early release and
scratch/BSS behavior. All LN modes share the same authoritative live/replay
implementation.

### Judge modifiers

Canonical LR2 play uses the table above without scaling. A local practice
modifier may scale PGREAT, GREAT, and GOOD using LR2oraja-style nesting and BAD
caps. BAD, KPoor, and the automatic-POOR deadline stay fixed. Non-neutral
playback and any judge scale other than 100 percent mark provenance modified
and make the result Bokutachi-ineligible.

Course `NO GOOD` or `NO GREAT` constraints are applied after the selected base
ruleset and recorded in provenance. Course results are never sent to
Bokutachi.

## LR2 gauge model

LR2 uses one standard gauge model across key modes. Gauge delta arrays are
ordered `PGREAT, GREAT, GOOD, BAD, POOR, KPOOR`.

| Gauge | Initial | Minimum | Clear border | Death | Base deltas |
|---|---:|---:|---:|---:|---|
| Assist Easy | 20 | 2 | 60 | none | `1.2, 1.2, 0.6, -3.2, -4.8, -1.6` |
| Easy | 20 | 2 | 80 | none | `1.2, 1.2, 0.6, -3.2, -4.8, -1.6` |
| Normal | 20 | 2 | 80 | none | `1, 1, 0.5, -4, -6, -2` |
| Hard | 100 | 0 | survival | `<2` | `0.1, 0.1, 0.05, -6, -10, -2` |
| Ex-Hard | 100 | 0 | survival | `<2` | `0.1, 0.1, 0.05, -12, -20, -2` |
| Hazard | 100 | 0 | survival | `<2` | `0.15, 0.06, 0, -100, -100, -10` |

For Assist Easy, Easy, and Normal, only positive deltas are multiplied by
`effectiveTotal / totalNotes`. Damage remains fixed.

Hard and Ex-Hard positive deltas are fixed. Every negative Hard and Ex-Hard
delta is multiplied by the LR2 damage multiplier described below. Hard, but
not Ex-Hard, then multiplies negative damage by `0.6` when the gauge value
before the judgment is strictly below 32 percent.

Survival gauges clamp normally, then become zero when the result is strictly
below 2 percent. Exactly 2 percent remains alive. A gauge that has died stays
dead unless an explicitly supported auto-shift policy selects another tracked
gauge.

### Hard damage multiplier

Let `T` be the effective floored total and `N` the positive note count:

```text
totalFactor = 10 / min(10, max(1, floor(T / 16) - 5))
```

The note factor is:

```text
N <= 20   : 10
N < 30    : 8 + 0.2 * (30 - N)
N < 60    : 5 + 0.2 * (60 - N) / 3
N < 125   : 4 + (125 - N) / 65
N < 250   : 3 + 0.008 * (250 - N)
N < 500   : 2 + 0.004 * (500 - N)
N < 1000  : 1 + 0.002 * (1000 - N)
otherwise : 1
```

The final multiplier is `max(totalFactor, noteFactor)`. It is computed from
validated play metadata once and applied to negative Hard and Ex-Hard deltas
before Hard guts.

### LR2 course gauges

LR2 courses start at 100 and die below 2. Their delta arrays are:

| Course gauge | Base deltas | Guts |
|---|---|---|
| Class | `0.10, 0.10, 0.05, -2, -3, -2` | damage ×0.6 below 32 |
| Ex-Class | `0.10, 0.10, 0.05, -6, -10, -2` | damage ×0.6 below 32 |
| Ex-Hard Class | `0.10, 0.10, 0.05, -12, -20, -2` | none |

Course auto-shift and inter-stage gauge restoration use the same policy-owned
minimum, death, and guts rules as a single stage.

## LR2 `TOTAL`

An authored positive `#TOTAL` is floored to an integer before any LR2 gauge
calculation. For example, `200.5` becomes `200`.

When `TOTAL` is absent or non-positive, let `N` be the chart's total playable
note count and compute:

```text
floor(160 + (N + clamp(N - 400, 0, 200)) * 0.16)
```

This is equivalent to OpenLR2's three segments:

```text
N < 400   : floor(160 + 0.16 * N)
N < 600   : floor(96 + 0.32 * N)
otherwise : floor(192 + 0.16 * N)
```

The value is resolved exactly once per chart stage and is used by:

- positive groove/easy recovery;
- Hard and Ex-Hard damage;
- ready-screen and practice display;
- score provenance;
- replay validation and reconstruction;
- Bokutachi eligibility validation.

The Beatoraja policy keeps the current authored-total handling and current
mode-specific default formula.

## Runtime architecture

The intended data flow is:

```text
Profile ruleset
      |
      v
StartOptions --> compiled ruleset policy
                    |           |
                    v           v
             judge semantics   gauge + TOTAL
                    \           /
                     v         v
                  gameplay result
                         |
                         v
               provenance + local DB
                         |
                         v
                Bokutachi eligibility
                         |
                         v
                   durable outbox
```

### Judge compilation

Replace the assumption that one `Judge` map serves every note context. Compile
a context-aware judge value containing normal, scratch, long-tail, and
long-scratch-tail windows plus explicit semantic flags and deadlines.

A single note-context classifier identifies:

- normal notes;
- scratch notes;
- classic LN heads and tails;
- CN/HCN heads and tails;
- scratch/BSS equivalents.

The classifier is shared by the real-time input controller and deterministic
simulation. No live-only fallback may judge the same replay event differently
from the worker simulation.

### Candidate and miss authority

Candidate selection, repeated KPoor, multi-BAD fan-out, automatic POOR, and LN
resolution belong to the authoritative simulation/input transaction path.
Rendering receives committed transactions and does not invent score-affecting
judgments.

The simulation may emit multiple deterministic transactions for one keypress
when LR2 multi-BAD applies. Ordering is stable by the source-compatible lane
scan and note identity so replay, gauge history, and result counts remain
deterministic.

### Gauge authority

`GameplayScoreState` consumes an immutable compiled gauge policy rather than
selecting global inline tables during each judgment. The compiled policy owns
base deltas, total scaling, damage modifier, guts, limits, borders, and death
thresholds.

Auto-shift continues to track the appropriate gauges in parallel. Every
tracked gauge uses the same ruleset and effective total; auto-shift cannot
cross from an LR2 gauge model into a Beatoraja gauge model.

## Beatoraja compatibility

The Beatoraja option preserves current AsoBMaShow behavior:

- current rank windows and scaling;
- current normal-note, miss, and LN behavior;
- current 5-key, 7-key, PMS, and 24-key gauge profiles;
- current default `TOTAL` formulas;
- current note-priority options;
- current course profiles and constraints.

Existing behavior is captured in golden tests before policy extraction. The
refactor is complete only if those tests remain unchanged under the
`beatoraja` selection.

## Score, replay, and course migration

Score-provenance schema is bumped for the ruleset ID, context-aware windows,
effective total, and candidate-selection proof.

Migration rules are:

- a valid current version-2 descriptor becomes Beatoraja revision 2;
- version-zero data remains legacy-unverified;
- old single-context judge windows populate the Beatoraja contexts needed for
  replay;
- an absent effective total is recomputed only for known Beatoraja revision-2
  replay and remains unverified if other required proof is absent;
- unknown future IDs or revisions remain stored but unsupported.

Replay validates finite totals, bounded windows, one complete entry per
required context/judgment, recognized note modes, and a supported policy
revision. Safe noncanonical values can be used to reproduce a local replay,
but they mark the resulting attempt modified. Malformed or unsafe values stop
replay with a clear error.

Course sessions store one ruleset descriptor. Every stage must match it.
Course-provenance merge treats a mismatch as inconsistent and modified, and a
resume operation refuses to silently combine different rulesets.

## Bokutachi submission policy

The provider-neutral `IrSubmission` includes immutable ruleset/provenance
proof. The Tachi driver validates that proof before constructing Batch Manual
JSON.

A Bokutachi score is eligible only when all of these are true:

- the driver supports score submission and is not read-only;
- the result is one ordinary chart rather than a course result;
- key mode is 7 or 14;
- ruleset ID is `lr2` at the supported revision;
- local provenance is verified;
- source chart rank is known and supported;
- effective normal, scratch, and LN windows equal the canonical LR2 policy;
- effective total equals the canonical floored LR2 value;
- playback is neutral;
- judge scale is exactly 100 percent;
- starting gauge was not overridden;
- autoplay, practice mode, and assist options were not used.

The following remain eligible when all other requirements pass:

- LR2 Assist Easy, Easy, Normal, Hard, Ex-Hard, and Hazard gauges;
- supported LR2 gauge auto-shift modes;
- chart-authored and selected supported LN modes using the LR2 policy;
- NORMAL, MIRROR, RANDOM, and other existing supported note-layout options.

The existing Batch Manual judgment mapping remains unchanged. LR2 KPoor is
kept in local score/provenance data and affects the LR2 gauge, but Tachi has no
KPoor judgment field, so it is omitted from the submitted judgment object and
from `bp`. Submitted `bp` remains BAD plus POOR.

Ineligible automatic results do not create an outbox row. The result UI keeps
manual submission visible but disabled and presents a stable, specific reason,
for example:

- `Beatoraja ruleset scores cannot be submitted.`
- `Modified judge windows cannot be submitted.`
- `This ruleset revision is not supported by Bokutachi.`

Provider-neutral coordination does not hard-code these strings. The driver
returns normalized eligibility status and a sanitized diagnostic for
presentation.

## Durable outbox proof

The outbox schema adds non-secret proof sufficient to prevent a later retry
from sending a row created under an unknown ruleset:

- ruleset ID;
- ruleset revision;
- provenance/eligibility fingerprint or equivalent immutable validation
  marker;
- existing attempt ID and chart hashes.

The proof is captured only after the Tachi driver validates the full
`IrSubmission`. The Tachi driver checks the stored proof again before any POST.
Payload JSON stays frozen for idempotent retry.

Existing rows that predate the proof columns migrate to an unknown proof and a
blocked state. They are not submitted, deleted, or rewritten as LR2. The user
may inspect the reason and discard them through the existing explicit action.

API keys remain exclusively in the profile credential store. They are loaded
at request time and are never copied into payload JSON, provenance, outbox
columns, diagnostics, or logs.

## Error handling

### Settings

- Missing or invalid profile selection: use LR2, preserve application startup,
  and emit a diagnostic without echoing unrelated settings.
- Settings write failure: keep the prior persisted selection and report the
  existing save failure path.

### Play start

- Unsupported selected policy: refuse to start rather than mixing fallback
  judge and gauge models.
- Invalid total or zero playable notes: use validated policy fallbacks where
  defined; otherwise fail start with a chart diagnostic rather than divide by
  zero.

### Replay

- Unknown future ruleset: metadata remains viewable; replay start is blocked.
- Safe noncanonical snapshot: replay allowed, result modified, IR blocked.
- Malformed context windows or non-finite total: replay blocked.

### IR

- Ineligible new result: no outbox row.
- Legacy outbox proof: row blocked with reason.
- Proof/payload mismatch: row blocked as an integrity error before network
  access.
- Provider disabled: existing validated rows remain paused under the current
  IR design.
- Missing credential: row remains blocked without persisting the credential.

## Test strategy

Implementation follows test-driven development. Source-derived tests are
written before each behavior change.

### Ruleset identity and settings

- Parse and serialize `lr2` and `beatoraja`.
- Default absent and invalid active-profile settings to LR2.
- Preserve selection across sanitize, save/load, profile duplication, export,
  import, and activation.
- Render and update both Play Options buttons and the ready summary.
- Update displayed effective total when the selection changes.

### LR2 judge golden tests

- Every inclusive edge and one microsecond outside PGREAT, GREAT, GOOD, BAD,
  and KPoor for ranks 0 through 4.
- Invalid rank fallback to Normal.
- Scratch equality with normal-note windows.
- Repeated KPoor without note consumption or combo change.
- Automatic POOR at exactly `+200 ms + 1 microsecond`, never at `+200 ms`.
- Source-derived same-lane multi-BAD clusters and transaction ordering.
- Removed late-BAD region for LN heads.
- Classic LN head/tail worse-result behavior and ±120 ms tolerance.
- CN, HCN, scratch LN, and BSS release cases.
- PGREAT/GREAT/GOOD-only practice scaling with fixed BAD/KPoor/deadline.

### LR2 gauge golden tests

- Every gauge type for all six judgments.
- Initial, minimum, maximum, border, and clear mapping.
- Authored totals such as `200`, `200.5`, and non-positive values.
- Default-total boundaries at 399, 400, 599, and 600 notes.
- Every hard-damage note breakpoint and representative values on each side.
- Total-factor boundaries around multiples of 16 and the corrected low-total
  cases.
- Hard guts at values immediately below, exactly at, and above 32.
- Survival death immediately below 2 and survival at exactly 2.
- Course Class, Ex-Class, and Ex-Hard Class tables and guts.
- Auto-shift tracking under LR2 without cross-policy state.

### Beatoraja regression tests

- Snapshot every current rank window.
- Snapshot existing mode-specific standard and course gauge tables.
- Preserve current total formulas, note-priority behavior, and replay output.
- Run existing gameplay, replay, practice, and course suites under Beatoraja.

### Provenance and replay tests

- Round-trip both supported descriptors.
- Infer existing version-2 data as Beatoraja.
- Keep legacy and unknown future records unverified/unsupported.
- Round-trip context-aware windows and effective total.
- Reject duplicate, missing, unbounded, or malformed context entries.
- Prove live input and replay produce the same transactions and final gauge for
  LR2 fixtures.
- Reject mixed-ruleset course resume and merge.

### Bokutachi and outbox tests

- Build a draft only for canonical verified LR2 7K/14K submissions.
- Reject Beatoraja, course, modified, unknown, non-neutral, scaled, and
  custom-start-gauge attempts with stable reasons.
- Allow supported gauge choices, auto-shift, note layout options, and LN modes.
- Persist and reload outbox ruleset proof without credentials.
- Block legacy rows and proof mismatches before an HTTP request is issued.
- Preserve the existing 202 polling, retry, discard, credential, and payload
  tests.

### Build verification

- `cmake --build cmake-build-debug --target main -j 6`
- the repository CTest suite;
- `scripts/ios_firebase_deploy.sh --build-only`;
- Android build-only verification with the existing signing environment when
  available.

No Firebase upload, TestFlight upload, or other deployment is authorized by
this design.

## Acceptance criteria

1. A profile with no ruleset setting displays and plays LR2.
2. The user can select Beatoraja and receive current AsoBMaShow gameplay.
3. A ruleset switch cannot create an LR2-judge/Beatoraja-gauge ordinary play.
4. LR2 rank windows, KPoor, automatic POOR, multi-BAD, and LN behavior match
   source-derived golden tests.
5. LR2 gauges, total flooring/defaults, damage, guts, and death thresholds
   match source-derived golden tests.
6. Live play and replay are deterministic under the recorded ruleset.
7. Existing version-2 replay provenance maps to Beatoraja without being
   mislabeled LR2.
8. Bokutachi drafts and outbox rows are created only for canonical verified
   LR2 7K/14K chart attempts.
9. Existing outbox rows without proof are blocked and preserved.
10. API keys never appear in outbox rows, payloads, provenance, or logs.
11. Existing Beatoraja behavior and the completed Bokutachi IR features remain
    covered by passing tests.
12. Desktop and mobile build-only verification succeeds without deployment.

## References

- [LR2oraja `JudgeProperty.java`](https://github.com/wcko87/lr2oraja/blob/3db78adff969b854fb3bcc68966449bd36cf7a5b/src/bms/player/beatoraja/play/JudgeProperty.java)
- [LR2oraja `JudgeManager.java`](https://github.com/wcko87/lr2oraja/blob/3db78adff969b854fb3bcc68966449bd36cf7a5b/src/bms/player/beatoraja/play/JudgeManager.java)
- [LR2oraja `GaugeProperty.java`](https://github.com/wcko87/lr2oraja/blob/3db78adff969b854fb3bcc68966449bd36cf7a5b/src/bms/player/beatoraja/play/GaugeProperty.java)
- [LR2oraja `GrooveGauge.java`](https://github.com/wcko87/lr2oraja/blob/3db78adff969b854fb3bcc68966449bd36cf7a5b/src/bms/player/beatoraja/play/GrooveGauge.java)
- [LR2oraja `BMSPlayerRule.java`](https://github.com/wcko87/lr2oraja/blob/3db78adff969b854fb3bcc68966449bd36cf7a5b/src/bms/player/beatoraja/play/BMSPlayerRule.java)
- [LR2oraja 0.8.5 compatibility release](https://github.com/wcko87/lr2oraja/releases/tag/build4501107977)
- [OpenLR2 `LR2_bmsload.cpp`](https://github.com/GOMazk/OpenLR2/blob/aa5500bd331d88d0cc2d81afe52c71939cc896c1/LR2/LR2_bmsload.cpp)
- [OpenLR2 `Scene04_Play.cpp`](https://github.com/GOMazk/OpenLR2/blob/aa5500bd331d88d0cc2d81afe52c71939cc896c1/LR2/Scene04_Play.cpp)
- [Tachi BMS 7K support](https://docs.tachi.ac/game-support/games/bms-7K/)
- [Tachi BMS 14K support](https://docs.tachi.ac/game-support/games/bms-14K/)
