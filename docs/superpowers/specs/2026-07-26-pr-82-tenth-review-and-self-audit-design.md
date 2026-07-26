# PR 82 Tenth Review and Self-Audit Design

## Scope

Address the five review findings created at 2026-07-26 06:58 UTC, then audit
the complete `develop...feature/file-based-replays` diff in independent risk
passes. Confirm every audit finding against the current implementation before
changing code, add a regression test before each behavior change, and repeat
the audit after fixes until no concrete issue remains.

This work does not reply to or resolve GitHub review threads. It does not
restore legacy replay reconstruction or slot relocation.

## Long-note mode boundary

`ReplayPlaybackData::setup.longNoteMode` remains in AsoBMaShow's application
domain:

- `0`: no applicable long-note interpretation;
- `1`: LN;
- `2`: CN;
- `3`: HCN.

Beatoraja's stock replay domain is `0` for LN, `1` for CN, and `2` for HCN.
A single shared conversion helper projects application values to stock values
and restores stock values to application values. Application value `0`
projects to stock LN (`0`) because stock BRD has no separate no-LN value and
the field is immaterial for a chart without long notes. Invalid values fail
closed.

The codec and Beatoraja filename helpers use this boundary only when producing
or consuming stock fields and path prefixes. Playback, compact results,
provenance, course state, migration inputs, and the Aso extension retain the
application value. Undefined-long-note paths use the converted prefixes
`""`, `"C"`, and `"H"`; an undefined-long-note path with application value
`0` is invalid.

New Aso extension setup data records the exact application `longNoteMode` and
`chartSha256`. Decoding first preserves the stock SHA-256 and stock LN mode,
then decodes the extension and rejects it unless the extension SHA-256 is
identical and its application LN mode projects to the same stock mode. The
existing double-play consistency check remains. Final replay-file validation
also includes the expected application LN mode for each stage, not only the
stage SHA-256.

Schema-v10 migration treats stored long-note modes as application values
through `3`; it must not clamp HCN to CN. The generated BRD and filename use
the shared conversion boundary.

## Undefined long scratches

Gameplay recording marks a chart as containing undefined long notes when its
authored LN mode is `0` and either `TotalLongNotes` or `TotalBackSpinNotes` is
positive. This makes a chart containing only undefined long scratches use the
same CN/HCN Beatoraja prefix rules as an undefined keyboard-long-note chart.

## Replay capture failure

`ReplayInputRecorder` distinguishes redundant state samples from fatal
capture rejection. Duplicate presses and unmatched releases remain tolerated
and do not invalidate recording. Invalid controls, timestamps outside the
pre-roll, decreasing timestamps, transition-capacity overflow, unavailable
clock mapping, and recording exceptions make the recorder terminal result
invalid even if earlier transitions were accepted.

`finish()` returns an optional transition stream: an engaged empty vector is
a valid all-miss replay, while no value is a failed capture with a diagnostic.
`GamePlayScene` records that failure. A chart attempt with failed raw capture
does not construct a persistence attempt. A course stage with failed capture
is not appended to the raw course replay, so final course persistence refuses
the incomplete replay envelope instead of storing an accepted prefix.

## Materialization arithmetic

Replay materialization computes the final completion timestamp with checked
signed arithmetic. If the maximum of the chart timeline and last input time
cannot safely accommodate the late-judgement window plus the final tick, the
replay is invalid and simulation is not advanced. This protects imported or
damaged BRDs containing timestamps such as `INT64_MAX` without imposing an
arbitrary chart-duration limit.

## Audit and verification

After the review fixes pass focused tests, independent read-only reviewers
inspect:

1. BRD codec, stock compatibility, paths, and replay-file integrity;
2. migration, schema cutover, profile archive, and persistence atomicity;
3. gameplay capture/playback, result recall, course lifecycle, and IR
   decoupling.

The primary session validates each report against the current diff and writes
all fixes. It then repeats a full local audit of changed-file boundaries,
integer and size limits, identity propagation, failure cleanup, and
cross-platform build membership. Every confirmed behavior fix receives a
red-green regression test.

Final verification consists of focused tests, the full desktop test suite,
the desktop `main` build, `git diff --check`, and the repository's iOS
build-only script. Only then are commits pushed and the PR head confirmed.
