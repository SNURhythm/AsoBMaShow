# PR #82 Ninth Review Fixes Design

## Scope

Address the three unresolved review findings added after commit `d5a7f47a`:

1. Reproduce Beatoraja DP FLIP (`doubleoption == 1`) and reject unsupported
   Beatoraja BATTLE modes.
2. Reject a chart replay whose long-note mode differs from the persisted
   result.
3. Ignore recognized private replay-finalization temporaries during profile
   export.

Review-thread replies and resolution are outside this change.

## Beatoraja double-play option

Upstream Beatoraja applies `doubleoption == 1` as a `PlayerFlipModifier`
before applying the 2P and 1P random modifiers. Values 2 and 3 are BATTLE
variants that can change an SP chart into DP and disable scoring. AsoBMaShow
will support the replay-safe FLIP behavior and fail closed on BATTLE values.

`ChartPlaybackSetup` will gain a small `DoublePlayOption` enum with `Normal`
and `Flip`. The stock JSON `doubleoption` field and the Aso extension setup
will both encode it. Extension decoding will treat the new setup member as
optional so existing schema-version-2 Aso BRD files remain readable; when it
is absent, the already-decoded stock field remains authoritative.

Replay chart preparation will apply FLIP before either player's normal,
mirror, or random option. The implementation will use the parser's existing
validated whole-chart lane-assignment modifier to exchange the two DP sides,
including scratches, for supported 10K and 14K charts. Non-normal
double-play options on unsupported key modes will be rejected rather than
silently misplayed.

Alternatives considered:

- Reject every nonzero `doubleoption`: safe, but leaves common stock FLIP
  replays incompatible.
- Swap player option metadata or replay controls: insufficient because
  Beatoraja swaps the chart before applying per-player modifiers.
- Support BATTLE too: out of scope because it changes mode and score
  eligibility rather than only reproducing a DP layout.

## Result/replay long-note identity

`ReplayRepository::loadChartReplayPlayback` will include
`setup.longNoteMode` in the same integrity comparison that already covers
chart SHA-256 and key mode. A mismatch returns `IntegrityConflict`, matching
course-stage behavior and preventing replay playback, analysis, and export
from judging a different undefined-LN interpretation than the saved result.

## Private replay temporaries in profile export

The filename recognizer currently embedded in stale replay cleanup will
become a shared replay-store helper. It recognizes only the private
finalization form `.<final-name>.brd.<safe-token>.tmp`. Startup cleanup will
continue applying its age, regular-file, symlink, and hard-link checks before
deletion. Profile export will skip recognized names without deleting them;
ordinary unknown replay-directory entries will continue to make export fail
closed.

## Tests

- Decode a stock 14K FLIP replay, preserve FLIP through Aso encode/decode,
  and reject stock BATTLE values.
- Prepare a two-sided DP chart and verify FLIP exchanges both key and scratch
  content before player options.
- Stage a valid result linked to a valid BRD with a different long-note mode
  and verify chart replay loading reports an integrity conflict.
- Export a profile while a recognized recent private replay temporary exists
  and verify the temporary is neither archived nor deleted.
- Run focused tests, the complete desktop test suite, the desktop build, and
  the iOS build-only check before pushing.
