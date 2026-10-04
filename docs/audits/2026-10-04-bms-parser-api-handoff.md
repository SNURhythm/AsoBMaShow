# BMS parser consumer contract handoff — 2026-10-04

## Current selective malformed-LN policy

The application adopts parser revision `33596fa855ec1b8fd92f12b719979c27bdce233a`, including both
amalgamated artifacts generated and tested upstream. This supersedes the
unchanged-graph policy in the historical adoption notes below.

Beatoraja's discards remain unchanged. Surviving null-pair heads and tails with
no active head become normal notes. CN/HCN heads with detached tails also become
normal notes because reference autoplay/miss processing cannot finish normally;
HCN additionally misses its passage-state reset. A healthy classic head retains
its detached tail. Classic holds corrupted by beatoraja's standard startup
shift, or having no positive time/section span, become normal notes.

Demotion preserves decoded lane, timing and WAV, and costs O(1) per endpoint.
Full/Scan/metadata-only results agree. There is no blanket chart rejection or
extra chart-wide repair scan. The complete source evidence and parser checks
are recorded in upstream `docs/audits/2026-10-04-malformed-long-note-policy.md`.

The subsequent baseline review removes a redundant timeline index, bounds
UTF-16 header-length counting, and adds cancellation polling to timing replay,
note publication, collection and hold closure. Successful decoding and replay
policy remain unchanged. Cancelled parsing returns no partial chart, as before.
Final paired corpus measurements show approximately 7% less Full parsing CPU
time and 10% less Scan/metadata CPU time than the preceding `fc7f84f` parser.
The remaining cost versus `2b964abd` is documented in upstream
`docs/audits/2026-10-04-baseline-review-loop.md`.

The subsequent timing consolidation stores cached timing in the existing
per-measure index and resolves source-order predecessors with a linear stack.
It removes duplicate tree allocations and combines timeline passes while
preserving exact timestamps, note graphs and all existing consumer policy.
Generic header matching uses a view after the leading ASCII `#`.
Paired 517-chart CPU measurements improve Scan by 9%, Full parsing by 6%, and
metadata parsing by 7% relative to `00f9fc7`. The remaining cost is still roughly
1.36–1.40x the earlier `2b964abd` parser. Full measurements and the discarded
chronological prototype's out-of-bounds timing case are recorded upstream in
`docs/performance/2026-10-04-timing-consolidation.md`.

The string follow-up copies an unbounded header suffix in bulk after resolving
its UTF-16 starting position. Finite slices and split-surrogate replacements
remain unchanged. Paired 517-chart measurements show 3.1% less Full parsing CPU
time than `4c40a2d`; Scan/metadata differences are within noise. Header-heavy
Full cases improve by 18–38%. Metadata trim views were not adopted because they
can retain oversized buffers after short replacement values. Details and raw
measurements are upstream in `docs/performance/2026-10-04-string-suffix-copy.md`.

For undefined LNs whose player-selected mode becomes known later,
`applyEffectiveLongNoteModeToChart` calls the parser library's
`TimeLine::DemoteUnusableLongNote` in the existing count pass. This happens before
building gameplay/visual models. The slot can be replaced/deleted, so callers
use the returned pointer; detached ownership is retained with its link cleared.
Changing modes requires a fresh parse. MainMenu clears its reusable-chart flag
on gameplay handoff so a fast return/relaunch cannot reuse an already-demoted
chart while the replacement preview is still loading.

Schema 14 invalidates old ordinary/archive metadata for the normal resumable
rescan, preserving added dates. Historical replay payloads and mismatch guards
remain intact. Only the current comparison expectations changed for the three
malformed fixtures: orphan, detached head and detached tail now use normal-note
graphs and still report saved-result mismatches. Their archived JSON, BRD,
SQLite data, chart bytes and immutable manifest were not changed. The user
authorized pushing the application branch; no deployment is included.

The current string-suffix adoption passes the full application build and all
420 CTest tests with zero failures (133.29 seconds). Upstream clean Clang
modular/resource/Python-comparison and regenerated-amalgamation suites pass.
Fresh sanitized Java comparison passes all 4,192 chart/seed selections, and
all 4,192 C++ snapshots are byte-identical to the preceding parser, including
timestamps. Independent parser and application-adoption source reviews found
no actionable issue.

The preceding timing adoption additionally passed GCC 15 unit/resource and
ASan/UBSan plus float-cast-overflow modular suites. Its independent validation
passed 4,800 sanitized Java comparisons and 19,200 byte-identical complete
snapshots; the initial exact prototype passed 8,170 real/generated snapshots.
The current generated files match upstream byte for byte; the public header
is unchanged from the previous adoption:

- `src/bms_parser.hpp`: `4339e276fd3f8f0a3693bafdd6a86dc1d667250811fb70eb2847e7bb776b703e`
- `src/bms_parser.cpp`: `7b151496bed4ea55bfb14e081e894dd61c1e64d7d45579fbbbf898a97140e202`

## Historical application adoption and compatibility policy

This section records the earlier adoption and is superseded by the current
selective policy above. At that stage, the application adopted
upstream `5c3bb2faf5d08aa19cc689273e5b107487bf8a5b`, based on the verified
`bb8658a3d68f06c11b4ce4ff0d6c9b91055f2961` handoff. The new upstream commit
adds source-filename overloads for buffered Parse/Scan; the old signatures
remain compatible. Upstream was tested, committed and pushed separately.
Both generated artifacts were copied together without local edits:

- `src/bms_parser.hpp`: `04e16303be71dbb5e6111346d2f43e8413132132070d8937a0d63115ffb90c8a`
- `src/bms_parser.cpp`: `3ef896b5debec37a826d9a577b44c801fc77e06dd8e13ef12dcb60f439751a1a`

The application continuation starts at `e51d50d2`, preserving the completed
consumer changes at `b64bf3d4` and `9a92823f`. The user's application push hold
remains in force. No deployment is part of this work.

### PMS discovery and metadata

Buffered Parse/Scan calls retain the actual archive entry or logical document
filename. The hint recognizes case-insensitive `.pms`, performs no I/O, and
does not assign metadata paths. Ordinary BMS retains its previous format.
Shared desktop/archive discovery and Android folder discovery now accept PMS;
the old discovery allowlists omitted it altogether.

Scanner tests exercise path, buffered, archive, uppercase-extension and SAF
logical-name parsing, including lane mapping, reciprocal LN pairs, counts and
timing. SAF coverage does not exercise a real Android document provider.
`tests/fixtures/parser/pms_scan_baseline.h` captures genuine metadata from the
immutable application parser at `68382de1`: path Scan is 9-key while the same
PMS bytes scanned without a filename are 10-key. The schema-12 migration test
seeds these old-parser facts, then verifies schema-13 ordinary/archive cache
rebuilding, retained dates, unchanged sources and idempotence. It is not a
captured old scanner database: that scanner did not discover PMS by default.
The broader interrupted/offline/scoped rebuild tests remain intact. Schema 13
must ship with this adoption; this branch has not been deployed separately.

### Long-note graph policy

At the user's request, the behavior reference is local beatoraja
`ad42f56c4658e968f93b24bf23440fe51cb9878e`. LaneRenderer and SongInformation
follow non-null pair pointers even when the partner was displaced from its
lane slot. The user's subsequent instruction is to follow that reference
without extra chart rejection, invented repair, or fallback policies.

The raw parser graph and its ownership remain unchanged. Parser adapters,
LN-mode/count preparation, gameplay-definition construction, synthetic autoplay
and direct audio scheduling no longer impose blanket LN graph admission.
Detached reciprocal partners remain supported. Gameplay
and visual models retain stable partner identities outside active lane scans,
without inventing playable notes or adding distribution counts. Classic held
LN tails remain reachable directly; charge-note autoplay still follows active
lane endpoints. Explicit MIRROR/RANDOM and DP-flip preparation also moves a
detached partner's lane to follow its active endpoint, retaining identity and
timing. These application behavior checks are derived from reference source;
they are not a runtime Java JudgeManager comparison. Reference locations are
`src/bms/player/beatoraja/song/SongInformation.java:119`,
`src/bms/player/beatoraja/play/LaneRenderer.java:552,563`, and
`src/bms/player/beatoraja/play/JudgeManager.java:270,273-285,431,445,572-573`. The exact tiny-scale
null-head reproducer, orphan/null-timeline constructed graphs, detached
head/tail cases and ordinary pairs have regression coverage. Mine recount and
raw mine damage remain unchanged.

The reference does not have a single exception policy for every caller.
Actual `SongData` probes confirm that lightweight scanning accepts the null
fixture, while full `SongData` construction and `setBMSModel` propagate
`NullPointerException`. The database information updater catches runtime
exceptions and skips that derived-information update. Gameplay's normal skin
draw method has no such catch; the per-object safe method is used by skin
previews. The bundled LWJGL thread catches and rethrows rather than resuming
the frame loop. These are source, bytecode and headless caller observations;
a whole-application crash was not reproduced.

Accordingly, the C++ visual model preserves the reference's missing-partner
failure at the actual song-information access, using a C++ exception instead
of a null-pointer dereference. It does not reject the chart at parse time,
drop the note, invent an endpoint, or manufacture successful density data.
Parser acceptance is not a claim that every later reference operation succeeds.
Synthetic classic-LN autoplay emits the surviving head press without a
fabricated tail event, following `JudgeManager.java:258–270`.

### Numeric consumers

`ChartTiming.h` reads integer `ParsedStopDuration` without a double round trip
and provides guarded rounding and saturating arithmetic. STOP/interpolation
consumers in the visual model, viewer, renderer, projection and prep metronome
use it. Judgment deadlines, candidate windows, visual offsets and near-limit
clock transitions use checked arithmetic too. The application parser adapters
retain the parser's saturated timing instead of imposing a separate timing
admission rule. An operation can still fail its own representability or output
budget checks; that is distinct from discarding the decoded chart.

Prep count-in work is bounded to 1,000,000 grid steps and 1,024 requested beats;
invalid or excessive plans return no partial output. Existing club planning
bounds, cancellation, output preservation and selected exports above 100,000
beats remain covered. Arithmetic helper safety is tested separately from
admission: accepting raw parser output does not mean it is playable.

### Saved replay policy and evidence

Saved chart and course replay consumers reject rejudged result disagreement
and return a diagnostic. This applies to Watch, Retry Same, G-Battle, practice
ghosts, course playback and video export through their shared consumer layer.
The lower-level materializer retains mismatching tracks for diagnosis; saved
evidence is never overwritten or relabelled as equivalent. Viewer ghost
failures now retain the consumer's diagnostic.

A detached endpoint can be simulated by identity, but an emitted event for
that identity cannot be represented safely by the current legacy lane/time
playback adapter. Such saved playback is rejected explicitly. Merely having a
detached partner does not itself reject a replay; the guard checks emitted
events. Live chart parsing, judging and audio export retain detached support.
The same runtime-only compatibility flag protects newly recorded practice
ghosts and synthetic autoplay video export. It is not serialized as a new
parser-version claim. When live result reconstruction lacks a representable
identity, its judgement distribution uses the existing omitted state; authored
chart data, authoritative scores and gauge history remain available.

The historical matrix and exact reproduction commands are recorded in
`tests/fixtures/historical_replays/README.md`. Matching chart hashes or results
alone are not proof of complete parser/audio equivalence. The matrix separately
compares setup, lookup coverage, judged facts, ghost events and keysound
scheduling. Legacy storage remains history-only under its existing capability
policy. See the matrix's limitations for fixture and device coverage.

### Verification record

- Reference-behavior follow-up: removed the additional chart-admission rules
  described above. New regressions first failed on path/byte admission,
  LN-mode preparation, gameplay-definition construction and synthetic autoplay;
  they now pass. Each LN/CN/HCN count-preparation case reparses a fresh chart
  and asserts the selected mode. Saturated timing remains accepted at the
  parser adapter, and direct audio scheduling accepts the unpaired-head
  fixture without changing its graph or counts.
- The follow-up full desktop build passed; CTest passed **420/420**, zero
  failures, in **149.70 seconds**. The final corrected mode-coverage test was
  rebuilt and passed separately. Visual-model and gameplay suites passed
  ASan/UBSan, including float-cast and signed-integer overflow checks, via
  `python3 tests/run_parser_consumer_sanitizers.py visual gameplay`.
- Independent read-only review found no remaining actionable issue after
  correcting the mode-coverage test. No deployment or application push was
  performed. Generated parser artifacts still match upstream byte-for-byte;
  this follow-up extends upstream reference tests/documentation without
  altering parser production behavior.
- The expanded upstream probes pass fifteen decoder/visibility cases and six
  actual `SongData` caller cases. The optional database runtime probe cannot
  initialize the bundled SQLite driver's native library on macOS ARM. Its
  catch-and-continue path is source-verified. Full graphics application launch
  and end-to-end malformed-chart playability remain unverified.
  The upstream reference follow-up is committed as `9d5f5e7`; the adopted
  generated parser pair remains unchanged from `5c3bb2f`.

Earlier adoption verification, retained for provenance:

- Executed the eleven-fixture Java probe added in upstream test-only commit
  `a4adbdff27c1b55dcbdb78e954e87b989973d20d` against the pinned local
  beatoraja decoder and actual `SongInformation` class. Valid/detached graphs
  passed the renderer visibility predicate; the exact null sentinel threw
  `NullPointerException` in both that predicate and `SongInformation`.
  This is not a GPU app launch or Java JudgeManager runtime comparison.
  The probe and reproduction are in
  `../bms-parser-cpp/docs/audits/2026-10-04-beatoraja-long-note-consumers.md`
  relative to the application checkout. That later upstream commit changes
  tests/documentation only; adopted parser artifacts remain the verified
  `5c3bb2f` pair above.
- Upstream: `make clean && make test && make test_amalgamation` passed;
  amalgamated ASan/UBSan tests passed before copying artifacts.
- Focused native integration: scanner, audio admission/export, gameplay,
  metronome, visual model/projection/scroll and saved replay consumers passed.
  Android extracted-method/JVM discovery checks passed 11/11.
- The finalized historical corpus passes
  `python3 tests/capture_historical_replays.py --check`, including the immutable
  manifest. The native CTest target runs the same fixture checks.
- Focused visual, metronome, projection and gameplay suites passed with
  `-fsanitize=address,undefined,float-cast-overflow,signed-integer-overflow`
  and `-fno-sanitize-recover=all`. The durable reproduction command is:

  ```sh
  python3 tests/run_parser_consumer_sanitizers.py visual metronome projection gameplay
  ```

  The runner uses include/define flags from the existing desktop configure and
  compiles scratch executables directly. Synthetic autoplay summary tests also
  passed those sanitizers, including six parsed LN/CN/HCN head/tail overwrite
  cases.
- Independent review reproduced and verified fixes for near-limit judgment
  deadline overflow, the saturated empty-queue sentinel collision, negative
  pre-roll arithmetic, visual offsets, detached lanes after modifiers, and
  replay graph-key collisions. Source-derived HCN work was confirmed bounded
  by existing terminal capacity checks. Practice analytics retains actual
  judgement/timing samples and maps collisions to the same section; it does
  not require the ambiguous note identity used by result distributions.
- Result-action integration fixtures now persist genuinely judged chart and
  carried-course replays. Separate mismatch cases verify that optional replay
  rejection preserves saved scores, fingerprints and chart graphs. The
  preparation-plan fixture supplies coherent timeline BPM/beat positions.
- Full iOS/Android builds and device/provider smoke tests were not run in this
  continuation. Desktop native/JVM checks do not establish device rendering,
  document-provider behavior or full Java JudgeManager parity.

- Final integrated `cmake --build cmake-build-debug -j 6` passed. The separate
  `main` no-op check performed only shader-copy/build-identity maintenance,
  with no compilation or linking.
- Final `ctest --test-dir cmake-build-debug --output-on-failure -j 6` passed
  **420/420 tests, zero failures**, in **116.51 seconds**. The first full run
  exposed three stale synthetic fixture assumptions; these were corrected
  with actual judged replay facts and coherent timeline metadata, then both
  targeted tests and the complete suite passed. Production admission and
  replay mismatch checks were retained.
- Application implementation and fixtures are committed locally as
  `d5dd5177`. The preceding consumer fixes remain intact. The application
  branch has not been pushed and no deployment was performed.

## Historical upstream handoff before application adoption

The parser performance/Java-parity follow-up is committed and pushed as
`bb8658a3d68f06c11b4ce4ff0d6c9b91055f2961` on `bms-parser-cpp/main`.
It fixes quadratic LN containment, ordinary Scan state retention, default
charset detection/replacement, VOLWAV/custom metadata, and authored metronome
cells. Its audit is `../bms-parser-cpp/docs/audits/2026-10-04-parser-review-fixes.md`.
Modular/amalgamated suites and sanitizers passed. Actual Java comparisons
passed 1,040 charts x four explicit RANDOM selections, under both sanitized
Clang and GCC 15 `-O2`. This does not verify app consumers or historical replays.

Generated artifacts at that revision (not yet copied into this application):

- `build/bms_parser.hpp`: `0de1a5b678e806f2240471e5eb8d0fc3be5716c020d87bb47c9e255119222f25`
- `build/bms_parser.cpp`: `311c451c4634e1f454a6c41eb82b86a54c9cf1160c822185fdfca8fe8ec27b39`

Confirmed contracts and remaining API work:

- **PMS remains unresolved:** path parsing uses the extension; byte Parse/Scan
  still default to BMS and have no explicit format hint. Add any necessary
  format API upstream and propagate the original filename/format through all
  app byte-loading paths. Do not treat adopting this revision as fixing PMS.
- **LN graph remains Java-compatible:** an unpaired visible head can have a
  null tail. A paired endpoint can live outside active playable slots while
  remaining chart-owned. App traversal/rendering must handle both; do not
  assume every partner occurs in the active lane arrays or silently delete
  upstream graph identities to avoid consumer fixes.
- **Timing remains saturating at the parser boundary:** `Timing` and
  `ParsedStopDuration` retain integer microseconds, including saturated values.
  `GetStopDuration()` still returns double and may round `LLONG_MAX` to 2^63.
  Consumer conversion/addition and playability policy remain app work.
- Mines remain in the main `Notes` slots for parsed charts; preserve the
  completed consumer recount work and raw damage semantics.
- `ChartMeta::VolWav` defaults to zero and `Values` stores case-sensitive
  `%`/`@` metadata. Automatic encoding now follows the reference; explicit
  charset directives remain C++ extensions. Recheck metadata invalidation
  together with adoption because decoded metadata/resource names can change.

The app work recorded below is retained. Findings 2, 3, 6 and the broader
numeric-consumer inventory still need completion. Verify findings 1, 4, 5
against the adopted artifacts. This document update does not adopt artifacts,
change app code, push the app branch, or override the user's existing push hold.
The implementer should resolve that hold under the user's current instructions.

## Historical implementation progress — first consumer slice

Work started from app commit `68382de1627f321aa8a56c7a961d75b4f7b974b7`.
The user confirmed that a separate agent owns the in-progress upstream parser
work. **No new parser artifacts have been adopted in this slice.** At the start
of this slice, the final upstream contract was pending. The Final upstream
handoff above now pins it and identifies the API work still required before
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

## Historical implementation prompt

```text
Work in /Users/xf/workspace/SNURhythm/AsoBMaShow. Read AGENTS.md and
docs/audits/2026-10-04-bms-parser-api-handoff.md in full. Implement the
AsoBMaShow parser-consumer fixes and investigate the replay-compatibility risk
described there. Treat confirmed findings, preexisting weaknesses, withdrawn
mine-damage speculation, and unverified concerns distinctly.

First inspect git status and preserve the completed consumer fixes at
b64bf3d4 and all unrelated changes. The upstream starting revision is
bb8658a3d68f06c11b4ce4ff0d6c9b91055f2961, already pushed in ../bms-parser-cpp.
Read the Final upstream handoff section: PMS byte-format support is still
missing and nullable/detached LN partners remain valid parser output. Do not edit src/bms_parser.hpp or
src/bms_parser.cpp directly. If upstream changes are necessary, make them in
the parser repository, run `make clean && make test && make test_amalgamation`,
and copy both generated build/bms_parser files together only after verification.
Follow AGENTS.md for parser commit/push and app branch integration. Do not
create a worktree, deploy, or disturb unrelated/concurrent changes.

Reproduce the remaining confirmed findings and add meaningful regressions
in the existing native targets. Complete null-LN consumption, PMS format
propagation, and the broader numeric-consumer inventory. Revalidate the
completed timing, mine recount, and metadata-invalidation fixes after adoption;
preserve added dates and ship invalidation with the semantic update. Establish and test an explicit historical
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
