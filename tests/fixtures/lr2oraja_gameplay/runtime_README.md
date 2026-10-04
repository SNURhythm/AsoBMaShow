The runtime oracle compiles the full, unchanged `JudgeManager.java`,
`JudgeProperty.java`, and `JudgeAlgorithm.java` from local lr2oraja-endlessdream
commit `5233be081abee2a7f824b78aed0d783847deeb2f`. The script checks both the
revision and source contents before compilation and prints their SHA-256 hashes.

Run from the repository root after building `gameplay_simulation_tests`:

```sh
python3 scripts/compare_lr2oraja_runtime.py
```

`runtime_reference_support.json` supplies a headless Java host, lane input,
chart data, note identity/state, score storage, and no-op UI/audio. It does not
replace any judgement runtime method. The host gauge tracks mine hits and HCN
increase/decrease operations; numerical gauge changes are covered by the
separate `compare_lr2oraja_gauges.py` oracle. Native gameplay runtime and its
rules/definition are compiled from the current sources in a temporary directory.
Only the unchanged parser object and localization library are reused from the
existing desktop build. The script never invokes Ninja or writes build metadata.

Rows compare six judgement counts (PGREAT, GREAT, GOOD, BAD, POOR, empty POOR),
current combo, mine-hit count, HCN increase/decrease tick counts, and each
judgement's FAST/SLOW buckets. The scenarios
cover initial negative/zero/positive note boundaries and negative preroll samples,
ordinary one-microsecond miss/mine boundaries, classic/CN/HCN releases,
scratch reversals with nearby following notes, strict classic auto-end timing,
missed CN/HCN pairs, simultaneous missed lanes, and short/coarse HCN updates, survival failures within a single update, and 300
deterministic mixed input/mine/CN/HCN sequences.
Coarse cases describe behavior for identical host-update samples; they do not
assert that every host regularly wakes at that interval.

The Java input driver models directional scratch reversal as releasing the old
scratch key and pressing the opposite key before one update. Legacy adapter
scenarios use the native backspin release followed by a press at that timestamp.
The raw-key matrix stages clockwise/counterclockwise edges without updating,
then samples their latest physical states in one update and Java key order. Its
native driver calls the production scratch-key API in the matching phase. It
covers early/exact/late tails, both directions, release/repress recovery, and
key-change times preceding the host update. Lane stand-ins retain notes for scanner reset/marking and
preserve ordered timing/state; they do not emulate parsing, rendering, audio, or
external input threads. Those host differences remain outside this oracle.
