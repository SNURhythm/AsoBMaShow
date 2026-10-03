# BMS parser consumer contract handoff — 2026-10-04

## Implementation progress — app consumer slice

Work started from app commit `68382de1627f321aa8a56c7a961d75b4f7b974b7`.
The user confirmed that a separate agent owns the in-progress upstream parser
work. **No new parser artifacts have been adopted in this slice.** The final
upstream commit, byte-format API and LN graph contract remain required before
PMS/LN adoption and the historical replay matrix can be completed.

Current reviewed artifact SHA-256 identities (baseline, not final adoption):

- `src/bms_parser.hpp`: `ce853b35c45f27b2fde0f3c2264ccdef0f6e01c148b005997837035cdb505c42`
- `src/bms_parser.cpp`: `ad33611897caa887c3e82dbb8edd6602ae40425647334d257acca0eda6c7433f`

Implemented decisions:

- **Finding 4:** club planning rejects negative/nonfinite authored STOPs,
  nonrepresentable durations and timestamp sums, invalid scales/timestamps,
  and plans exceeding 1,000,000 events. Failure returns no partial plan and
  an explicit status. Audio export reports failure and preserves existing
  output; live scheduling logs that generated club beats were disabled.
  Cancellation now reaches live club planning and is checked inside timeline
  traversal. Ordinary tempo changes and the existing selected export above
  100,000 beats remain supported. The two unused gameplay-end helpers now
  saturate their additions; this is separate arithmetic cleanup, not a
  claimed reachable gameplay crash fix.
- **Finding 5:** effective-LN preparation counts unique mine objects in either
  `Notes` or `LandmineNotes`, including aliases, without adding them to playable
  or scratch/LN counters. Mine damage is unchanged. Parsed normal/scratch/LN
  fixtures cover all effective LN modes and alternate-container ownership.
- **Finding 1:** schema 13 deliberately invalidates ordinary metadata, archive
  scan caches and checkpoints through the existing rebuild mechanism.
  Preserved dates use `CREATE TABLE IF NOT EXISTS` and `INSERT OR IGNORE`, so a
  prior incomplete preservation pass wins over partially rescanned dates.
  Tests cover schema-12 invalidation, unchanged ordinary/archive sources,
  cancellation, unavailable roots, scoped scans, fresh-process resumption,
  retained added dates and no second-launch invalidation. The scanner test
  simulates stale metadata; it is not evidence of an exact historical-parser
  comparison. **Ship schema 13 with the final parser adoption.** If schema 13
  is released before adoption, another revision/invalidation will be needed.

Reproductions recorded before fixes:

- Club-beat UBSan reported `9.22337e+18 is outside the range of representable
  values of type 'long long'` for the exact Infinity STOP fixture.
- The gameplay-end helper probe reported signed overflow for
  `(LLONG_MAX - 5) + 10`.
- Parsed mine preparation failed the expected retained-mine assertions in
  every effective LN mode, then passed after the recount change.
- Schema-12 upgrade retained a chart when the new regression expected an
  empty metadata cache, then was updated to use the rebuild migration.
- The exact `5e-324` LN fixture still reaches a null tail in the visual model
  under ASan/UBSan. **Finding 3 is not fixed by this slice.**

Additional timing inventory (not yet repaired or runtime-proven in this
slice): `PrepMetronome.cpp` and `ChartViewerScene.cpp` contain similar unchecked
STOP conversion/addition; `BMSRenderer.cpp` has STOP arithmetic in timeline
interpolation; `PlayfieldChartVisualModel.cpp::stopMicros` uses `llround` on a
potentially saturated double. These must be assessed alongside the final
numeric/LN boundary policy. Club planning is not a claim of whole-app numeric
safety.

Replay investigation so far (source tracing only):

- `ChartReplayConsumer` accepts a structurally playable `ResultMismatch` and
  carries its diagnostic; `CourseReplayConsumer` aggregates stage/result
  mismatch diagnostics.
- Watch, G-Battle, course watch and video-export entry points call diagnostic
  publishers. Both `MusicSelectScene::publishRecordsDiagnostic` and
  `MainMenuScene::publishReplayLoadDiagnostic` write logs/debug history; a
  successful-but-mismatching load is not clearly labelled in playback UI.
- `ChartRecordActions::prepareChartResult` drops a successful load's warning
  while retaining its Retry Same data. Practice-ghost load paths in
  `ChartViewerScene` likewise do not propagate the successful load warning.
- No old/new replay fixtures, judgment equivalence, note-lookup coverage,
  ghost/keysound parity or final compatibility policy are claimed yet.
  Original persisted evidence has not been changed. Findings 2, 3 and 6 remain
  pending final upstream coordination and follow-up implementation.

The implementation plan and verification record are in
[`../superpowers/plans/2026-10-04-parser-consumer-fixes.md`](../superpowers/plans/2026-10-04-parser-consumer-fixes.md).

Local implementation commit: `b64bf3d4` (`fix: harden parser timing, mine recount
and metadata rebuild`). Per the user's latest instruction, it has **not been
pushed**; the push waits for the upstream parser patch.

Verification for that code revision:

- `cmake --build cmake-build-debug -j 6`: passed, including the desktop app
  and all native test targets. Existing compiler/linker warnings remain.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: **419/419
  passed**, zero failures, 255.07 seconds.
- Expanded `club_beat_tests.cpp` compiled directly with
  `-fsanitize=undefined,float-cast-overflow,signed-integer-overflow
  -fno-sanitize-recover=all`: passed.
- The same tests compiled with `-fsanitize=address,undefined`: passed.
  The exact Infinity STOP standalone probe also passed with an empty plan.
- PMS baseline probe: path Parse/Scan mode 9, notes 4, parsed lanes
  `0,5,8,1,1`; bytes Parse/Scan mode 10, notes 4, parsed lanes `0,9,12,1,1`.
- Independent code review found no blocking defects in findings 1/4/5.
  Cancellation flipping during a timeline-only/zero-beat loop remains an
  optional coverage gap; pre-cancellation is tested and loop checks were
  reviewed. The green existing visual/replay suites do not resolve findings
  2/3/6 or the additional timing inventory above.

## Scope and provenance

This document hands **AsoBMaShow integration work** to a separate implementer.
It does not change application behavior or adopt a new parser amalgamation.
The review examined AsoBMaShow `68382de1627f321aa8a56c7a961d75b4f7b974b7`
(PR #118) and upstream `bms-parser-cpp` `14d7a235`. Paths and approximate line
numbers below refer to those snapshots; use symbol names after updates.
Upstream performance and Java-parity fixes are being handled separately.
Before implementation, record the final upstream commit and generated-artifact
identity here; do not assume the reviewed upstream revision is the final fix.

The review session reported all **419 application tests passing** before these
focused probes exposed gaps. This is historical baseline evidence, not a claim
that this documentation change reran the suite or that all findings are new
regressions. Null long-note tails and the mine recount mismatch predate the
reviewed parser change. Replay compatibility remains an unverified concern.

Follow [AGENTS.md](../../AGENTS.md). In particular, `src/bms_parser.hpp` and
`src/bms_parser.cpp` are generated artifacts. Parser behavior/API changes belong
in `../bms-parser-cpp`; regenerate, test, and copy both files together. Do not
hand-edit their application copies. This handoff is not authorization to deploy.

## Findings and acceptance criteria

### 1. Existing metadata can survive changed parser semantics

**Confirmed integration gap; high priority.**

- `src/repositories/ChartRepository.cpp:33` sets
  `kChartDatabaseSchemaVersion = 12`.
- `runChartDatabaseMigrationPasses` (around line 735) returns immediately for
  a current schema. `migrateChartDatabaseToVersion12` only rebuilds when the
  `rank_type` column is missing; a database already at schema 12 retains rows.
- `src/ChartLibraryScanner.cpp:1426`, `scheduleOrdinaryChart`, skips a known
  ordinary chart in the normal non-scoped scan.
- The known-archive handling around line 740 reuses metadata when archive
  size, modification time, and cached chart count match. Parser changes do not
  alter those file facts.

Consequently, a library indexed with old parsing/counting/key-mode behavior
can display old metadata while gameplay reparses the same bytes differently.
The current schema migration is not a parser-semantic revision marker.

**Reproduce:** create a schema-12 database using the older parser, index both
an ordinary chart and its archived equivalent, then update only the parser.
Keep source bytes and archive timestamps/size unchanged. Run normal discovery
and compare persisted key mode/counts/timing with fresh `Parser::Scan` output.
Choose a fixture whose semantics changed across the two exact parser commits;
record both outputs, rather than relying on wall-clock or file-touch effects.

**Expected implementation:** choose a durable parser metadata revision or a
deliberate schema migration, and invalidate every relevant ordinary/archive
cache once. Reuse `invalidateChartMetadataForNormalScan(db, completed, true)`
(around line 397), its rebuild-required state, and preserved `add_date` handling.
Preserve favorites, scores, replay associations, and user-added dates. Ensure
interrupted/offline/scoped scans cannot incorrectly declare the full rebuild
complete. Check retry/idempotence when preserved-date state already exists.

**Tests:** extend `tests/chart_repository_tests.cpp` and
`tests/chart_library_scanner_tests.cpp`: schema-12 upgrade, ordinary and archive
invalidation despite unchanged sources, preserved dates, interruption/resume,
unavailable source, and second launch without repeated invalidation. Existing
rebuild-state tests provide useful infrastructure.

### 2. Buffered PMS inputs lose their format identity

**Confirmed path-versus-bytes discrepancy; high priority.**

Upstream `src/Parser.cpp:973–986` has byte-based `Parser::Parse` and
`Parser::Scan` calls that reach `ParseInternal` with its default PMS flag false.
The filesystem overload detects `.pms` and passes the format explicitly.
Setting `Meta.BmsPath` after parsing cannot repair lane mapping already done.

Application consumers:

- `src/ArchiveFile.cpp`, `archive_file::parseChart`: Android SAF bytes around
  line 10934 and archive-entry bytes around 10964; ordinary files use the path
  overload.
- `src/ChartLibraryScanner.cpp`, `parseChartMeta`: buffered `Scan(*bytes, …)`
  around line 801 versus path `Scan(path, …)` around line 812. This covers
  archive entries, prefetched bytes, and Android tree reads.

**Self-contained fixture:** save these bytes as `mapping.pms`:

```text
#TITLE PMS lane mapping
#BPM 120
#WAV01 note.wav
#00011:01
#00022:01
#00025:01
#00152:0101
```

This matches upstream `test/testcases/parser/popn.pms`. In the reviewed parser,
path parsing yields key mode **9**, and non-null `timeline->Notes` lanes in
traversal order **0, 5, 8, 1, 1**. Byte parsing yields mode **10**, lanes
**0, 9, 12, 1, 1**. Compare both Parse and Scan metadata, and repeat through the
archive wrapper rather than testing only the upstream overload.

**Expected implementation:** coordinate an explicit format/source hint in the
upstream byte APIs first. Pass the actual entry/document filename or format
from every app buffer caller; archive outer suffixes must not determine the
inner chart format. Retain compatibility for existing byte callers. Do not
fix this by renaming files or editing the amalgamation directly.

**Tests:** path/bytes/archived PMS equivalence for key mode, lane mapping, LN
links, counts and Scan metadata; ordinary BMS unaffected; extension casing;
Android SAF logical-name handling. Extend scanner/archive coverage as well as
upstream tests. Confirm the final API with the upstream owner before adoption.

### 3. Accepted long-note heads can have no tail

**Confirmed crash; preexisting contract weakness, not proven introduced by PR #118.**

Input:

```text
#BPM 120
#00002:5e-324
#00151:01
```

The reviewed parser accepts this input with `Meta.TotalNotes == 1`. Calling
`buildPlayfieldChartVisualModel(*chart, 1)` segfaults in
`src/scene/play/PlayfieldChartVisualModel.cpp:454` at
`longNote->Tail->Timeline->Timing`. The very small measure scale is part of the
reproducer. Do not simplify it away without checking reproduction.

`src/scene/play/BMSRenderer.cpp:2005` also accesses `head->Tail->IsPlayed`.
That access is a consumer-audit target; the visual-model crash is the directly
reproduced runtime failure. Audit other tail/head/timeline assumptions with
`rg -n 'Tail->|Head->' src --glob '!bms_parser.*'`; assess surrounding guards
before labelling additional matches defects.

**Expected implementation:** establish the upstream contract for malformed,
unmatched, or detached LNs, then make consumers robust to the accepted graph.
Choose a consistent policy (reject chart, omit malformed LN, or supported
normalization) across rendering, judging, density, autoplay, and export. A
single local null check must not silently leave other consumers unsafe or
change counts inconsistently. Upstream repairs must be made upstream.

**Tests:** add the exact parser-to-visual-model fixture to
`tests/playfield_chart_visual_model_tests.cpp`; cover normal pairs, orphan
head/tail, null endpoint timeline, and applicable gameplay/rendering paths.
Run the exact input under ASan/UBSan. Assert the chosen behavior and counts,
not merely absence of a crash.

### 4. Club-beat timing consumes saturated parser values unsafely

**Confirmed undefined conversion on a reachable consumer path; high priority.**

Input:

```text
#BPM 120
#STOP01 Infinity
#00009:01
#00111:01
```

`src/audio/ClubBeat.cpp:46`, `club_beat::buildPlan` / `processTimeline`, casts
`timeline.GetStopDuration()` to `long long`, then adds `timeline.Timing`.
The parser's saturated duration is exposed as a double: converting
`LLONG_MAX` to double rounds to **2^63**, which is outside signed 64-bit range.
UBSan confirmed the floating-to-integer overflow. The caller is reachable
from `src/audio/Jukebox.cpp:3433` in club mode and also appears in
`src/audio/ChartAudioRenderer.cpp:780` for rendered audio.

**Expected implementation:** checked/saturating double conversion and checked
addition, with an explicit policy for non-finite, negative, and unplayable
durations. Check later `llround` and cursor additions in `buildPlan` too; a
guard on just this first cast does not establish safe timing end-to-end.
Make extreme charts terminate/cancel within a bounded workload.

Inventory app timing arithmetic at the parser boundary. In particular,
`src/ChartPlaybackDuration.h:81,86`, `GameplayEndMicros` and
`GameplayResultTransitionMicros`, can overflow when adding to saturated
timestamps. The review found no production callers of these two helpers;
this is an arithmetic audit candidate, **not proof of a runtime failure**.
Separate reachable failures from unused helper cleanup in the implementation.

**Tests:** extend `tests/club_beat_tests.cpp` with parsed Infinity STOP, very
large finite STOP, maximum timestamp plus offset, NaN/negative policy, normal
tempo changes, cancellation, and bounded event production. Use UBSan with
`float-cast-overflow` and `signed-integer-overflow` enabled. Exercise audio
export as well as live club mode where practical.

### 5. Effective-LN recount drops parser-provided mine counts

**Confirmed representation mismatch; preexisting in both compared parser versions.**

`src/CoursePlaySession.h:214–240`, `recalculateEffectiveLongNoteCounts`, skips
mines in `timeline->Notes`, then counts only `timeline->LandmineNotes`.
Both parser versions compared in the review place parsed mines in `Notes`.
`applyEffectiveLongNoteModeToChart` calls this recount and overwrites
`Meta.TotalLandmineNotes`, losing the parser's count even if no LN override is
needed.

**Reproduce:** parse `"#BPM 120\n#000D1:01\n"`, record the metadata mine count
and both containers, call `applyEffectiveLongNoteModeToChart(*chart)`, and
compare. Expected initial count is one; the current recount loses it.

**Expected implementation:** agree on the supported mine-container contract,
then count actual mines without double-counting aliases across containers.
Keep mines outside ordinary playable-note and LN counters. Add a parsed-chart
regression through the effective-LN preparation path, including normal notes,
mines, LN overrides, scratch lanes, and any supported alternate container.
`tests/replay_playfield_presentation_tests.cpp` already exercises
`applyEffectiveLongNoteModeToChart`; use that or an appropriately focused
preparation target rather than a fixture that only constructs the wrong
container by hand.

**Withdrawn hypothesis:** do not divide mine damage by two. The reviewed raw
mine damage now agrees with Java's direct gauge subtraction. That earlier
suspicion is not a finding and must not become an application behavior change.

### 6. Backward replay compatibility needs evidence and an explicit policy

**Unverified compatibility risk, not a demonstrated regression.**

Chart bytes/hash can stay constant while parser revisions change lane mapping,
timing, counts, random decisions, or malformed-LN treatment. Inspect these
distinct mechanisms rather than asserting all replay formats work alike:

- `src/ReplayData.h:49`, `replay_note::key`, and
  `src/scene/play/Pacemaker.h:115–143`, `buildReplayNoteLookup` /
  `findReplayNote`, use exact lane plus `noteTimeMicros` for the legacy/event
  representation. Small timing shifts can lose matches.
- `src/replay/ReplaySetup.h`, `ReplaySetup`, records chart identity, options,
  random seed/values and PRNG identity, but no general parser revision.
  `src/replay/ReplaySetup.cpp:73`, `validateReplaySetup`, validates those facts;
  a PRNG identifier alone does not version all parsing semantics.
- `src/replay/ChartReplayConsumer.cpp:73–105` compares prepared hashes/key mode
  and effective LN mode before materializing.
- `src/replay/ReplayPlaybackMaterializer.cpp:189–194` validates freshly built
  note counts/max score, then rejudges input and compares result facts.
  `ReplayPlaybackMaterializationOutcome::playable()` in its header explicitly
  permits `ResultMismatch` when playback data exists. The implementation is
  designed to retain some mismatching but structurally playable tracks; trace
  diagnostics through Watch, Retry Same, G-Battle, practice ghost, course, and
  video export before deciding whether incompatibility is visible to users.

**Required investigation/tests:** capture genuine replay fixtures and saved
results using the old parser; replay identical chart bytes using the new one.
Include legacy fixtures (`tests/ReplayLegacyFixture.h`) and modern input
replays, PMS, fractional timing/STOP, LN/LNOBJ, RANDOM, and course stages.
Compare setup, note lookup coverage, judgments, result fingerprint, ghost and
keysound scheduling. Existing `replay_setup_tests`, `replay_playback_tests`,
`replay_playback_driver_tests`, `course_replay_consumer_tests`, and
`replay_keysound_schedule_tests` are relevant targets; they are not evidence
of this old-parser/new-parser matrix until those fixtures exist.

Choose and document a compatibility policy: version parsing semantics,
retain compatible decoding, migrate only when equivalence is established,
or reject/clearly label incompatible playback. Do not silently call a
different replay result equivalent, overwrite original evidence, or assume
saved chart hashes establish semantic compatibility. Whether to add a parser
revision to persisted replay/provenance schemas is an open design decision.

## Recreating the two focused crash/UB probes

The original review used temporary standalone probes. Their complete essential
logic is below so this document does not depend on those temporary files.
Save this code as a scratch `parser_api_probe.cpp` outside tracked source:

```cpp
#include "bms_parser.hpp"
#include "audio/ClubBeat.h"
#include "scene/play/PlayfieldChartVisualModel.h"
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  const bool stop = argc > 1 && std::string(argv[1]) == "stop";
  const std::string input = stop
      ? "#BPM 120\n#STOP01 Infinity\n#00009:01\n#00111:01\n"
      : "#BPM 120\n#00002:5e-324\n#00151:01\n";
  std::vector<unsigned char> bytes(input.begin(), input.end());
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(bytes, &raw, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> chart(raw);
  if (!chart) return 2;
  std::cout << "parsed notes=" << chart->Meta.TotalNotes << std::endl;
  if (stop) {
    const auto plan = club_beat::buildPlan(*chart);
    std::cout << "beats=" << plan.size() << std::endl;
  } else {
    const auto model = buildPlayfieldChartVisualModel(*chart, 1);
    std::cout << "model notes=" << model.notes.size() << std::endl;
  }
}
```

From the AsoBMaShow root, substitute your scratch source/output paths:

```sh
clang++ -std=c++23 -O1 -g -Isrc -pthread \
  -fsanitize=undefined,float-cast-overflow,signed-integer-overflow \
  -fno-sanitize-recover=all \
  /path/to/parser_api_probe.cpp src/bms_parser.cpp \
  src/audio/ClubBeat.cpp src/scene/play/PlayfieldChartVisualModel.cpp \
  -o /path/to/parser_api_probe
/path/to/parser_api_probe ln
/path/to/parser_api_probe stop
```

Run each invocation separately: failure is expected at the reviewed snapshot.
For ASan, build a separate scratch binary with `-fsanitize=address,undefined`.
Promote the reproductions into existing test targets for the eventual fix;
the standalone probe is review evidence, not a replacement for integration
tests. If platform linking differs, use the sources/include settings from
`playfield_chart_visual_model_tests` and `club_beat_tests` in `CMakeLists.txt`.

## Suggested implementation order and remaining decisions

1. Pin the completed upstream revision and confirm byte-format, LN graph,
   numeric saturation, and mine-container contracts. Coordinate any upstream
   API work without hand-editing application amalgamations.
2. Address reproduced crashes/undefined timing conversion (findings 3–4).
3. Adopt verified artifacts and propagate PMS format (finding 2), then repair
   mine recount (finding 5). Verify the consumers against the final contract.
4. Ship metadata invalidation together with the semantic adoption (finding 1),
   so users do not retain stale cached metadata between releases.
5. Finish the historical replay matrix and compatibility policy (finding 6)
   before claiming the parser adoption is backward compatible.

Open decisions: exact upstream revision/API; handling malformed LNs; whether
unplayable numeric ranges are rejected or saturated at the app boundary;
parser metadata revision versus schema bump; replay revision/provenance and
user-facing incompatibility handling. Do not infer product policy solely from
a null guard, successful compilation, or a green preexisting test suite.

## Start prompt for the implementation agent

```text
Work in /Users/xf/workspace/SNURhythm/AsoBMaShow. Read AGENTS.md and
docs/audits/2026-10-04-bms-parser-api-handoff.md in full. Implement the
AsoBMaShow parser-consumer fixes and investigate the replay-compatibility risk
described there. Treat confirmed findings, preexisting weaknesses, withdrawn
mine-damage speculation, and unverified concerns distinctly.

First inspect git status and coordinate the final parser revision with the
upstream work in ../bms-parser-cpp. Do not edit src/bms_parser.hpp or
src/bms_parser.cpp directly. If upstream changes are necessary, make them in
the parser repository, run `make clean && make test && make test_amalgamation`,
and copy both generated build/bms_parser files together only after verification.
Follow AGENTS.md for parser commit/push and app branch integration. Do not
create a worktree, deploy, or disturb unrelated/concurrent changes.

Reproduce each confirmed finding and add meaningful regressions in the
existing native targets. Fix null-LN consumption, unsafe saturated-time
arithmetic, PMS format propagation, mine recount, and persistent metadata
invalidation preserving added dates. Establish and test an explicit historical
replay compatibility policy; do not claim parity from chart hashes alone.
Do not introduce a mine-damage /2 change.

Use the existing cmake-build-debug directory. Run only one Ninja/CMake build
at a time in that directory; combine targets in one invocation. Complete
`cmake --build cmake-build-debug -j 6` so the application and native tests are
current, then `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.
The documented desktop app compile command is
`cmake --build cmake-build-debug --target main -j 6`.
Run focused sanitizer probes as described in the handoff. Investigate damaged
Ninja metadata per AGENTS.md if unchanged builds unexpectedly recompile widely.
Do not run whole-file formatters. Update the handoff with actual decisions,
exact parser/app commits, tested old/new replay fixtures, results, and any
remaining limitations. Report verified fixes without implying every issue
was introduced by PR #118.
```
