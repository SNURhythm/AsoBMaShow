# Final corrective integration report: skinned replay video export

## Scope and invariants

- Base: `4efce2d6d6093044196c5b8f509703f65f684384`.
- This correction is limited to the nine confirmed findings in
  `final-fix-brief.md`, their focused tests, and the necessary CMake boundary.
- Normal and course result-screen rendering was not changed. The existing
  built-in result paths remain intact.
- No-selection replay export still creates the built-in presentation. A
  selected skin still fails fast without submitting the built-in fallback.
- No deployment or distribution action was run. `docs/todo.md` was not
  changed.

## Focused TDD record

The focused test executables exercise the production coordinator, replay
adapter, preflight, projection, renderer-ownership, and playback helper paths.
No test-local replay reducer was introduced.

### 1. Authoritative selected-skin transaction diagnostic

RED command:

```sh
cmake --build cmake-build-debug \
  --target playfield_presentation_coordinator_tests -j 12
./cmake-build-debug/playfield_presentation_coordinator_tests
```

Expected and observed RED:

- `selected path asks the failed skin transaction for its diagnostic`
- `selected path returns the authoritative transaction diagnostic`

The coordinator used to replace a critical prepare failure with
`skin.presentation.frame_not_submitted` before asking the failed skin
transaction for its result. The selected/no-fallback path now calls the skin
transaction render boundary, returns its first diagnostic, and never renders
the warmed built-in candidate.

GREEN: the same command passes with
`playfield presentation coordinator tests passed`.

### 2. Initial selected-skin state and replay note semantics

RED command:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests -j 12
./cmake-build-debug/replay_playfield_presentation_tests
```

Expected and observed RED assertions covered:

- the selected session did not receive the complete initial state/start-time
  snapshot;
- CN and HCN release left endpoint holding state active (two cases);
- a Mine replay event did not resolve the authored Mine visual source.

The adapter now preserves scene/play start timestamps in its state store,
resolves Mine events against `ChartVisualNoteSource::Mine`, and clears both
CN/HCN endpoints directly on release so state propagation cannot reactivate
either endpoint.

GREEN: the same command passes with
`Replay playfield presentation tests passed`.

### 3. Logical export bounds

RED command:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests -j 12
```

Expected and observed RED: compilation failed because the production
`replayGameplayLogicalUiBounds` mapping did not exist. The test requires
3840x2160 to map to 1920x1080 and 3840x1600 to map to 1920x800.

The mapping now uses the same 1920-wide logical design space as export
projection. The normal production preflight test checks 4K widescreen bounds;
the course production preflight test checks 4K non-16:9 bounds.

GREEN:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests -j 12
./cmake-build-debug/replay_playfield_presentation_tests
```

Result: `Replay playfield presentation tests passed`.

### 4. Replay frame timers and start timestamps

RED command:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests -j 12
```

Expected and observed RED: compilation failed because the production
`replayGameplayFrameState` authority did not exist.

The new helper mirrors live `GamePlayScene` timing: it derives chart time
through `preparation::Plan`, applies audio/visual offsets, uses the skin
animation origin for scene start, uses play origin zero for replay export,
activates Timer 41 at gameplay time zero, and supplies the exact Beatoraja
play-time limit (`PlayLength` or autoplay `TotalLength`, Java `int` wrapping,
plus 5000 ms). Both normal and course frame loops consume that helper rather
than assembling a partial clock.

GREEN: the focused replay command passes. Assertions cover the initial clock,
a live gameplay clock, scene/play starts, nonzero serials, timer active state,
timer origin, exact elapsed mode, and play-time limit.

### 5. Renderer/lifecycle ownership and real selected preflight

RED command:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests -j 12
```

Expected and observed RED: the expanded production preflight ownership API,
renderer-protected destruction helper, and lifetime observation seam did not
exist.

The production normal helper now acquires the existing
`display::RendererAccessCoordinator` export reservation before constructing
either built-in or selected-skin presentation, including selected-skin
lifecycle acquisition. It supplies serial 1, the complete initial state and
projection, correct starts/timer data, and logical bounds. Destruction also
reacquires that same reservation. Normal and course exporter scope guards
destroy every prepared presentation before releasing renderer ownership; a
later course preflight failure destroys already-prepared stages the same way.
Preflight remains before Photos permission, files, audio rendering, and MP4
creation.

GREEN: the focused replay test uses the real selected-skin creation seam and
proves display acquisition is blocked during both creation and destruction,
then becomes available afterward. It also proves production normal preflight
succeeds without `skin.session.initial_state_invalid`, and production course
preflight receives the non-16:9 logical bounds.

### 6. Lane-cover pulse and progressive course maximum combo

RED command:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests -j 12
```

Expected and observed RED: compilation failed because the shared production
`ReplayLaneCoverPlayback` and adapter `progressiveMaximumCombo` authorities
did not exist.

The first GREEN exposed one remaining course-lifetime edge during final
inspection, so a second tests-only RED was added before changing production:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests -j 12
```

Expected and observed second RED: compilation failed with
`no type named 'ReplayCourseMaximumComboPlayback' in namespace
'replay_video_export'`. The added assertions require a maximum earned in
stage one to remain visible before stage two's first event, to resist a lower
stage-two combo, and to advance on a later higher combo.

Both exporter loops now use the same lane-cover playback helper; its reset
flag is false at the start of each frame and pulses only on the frame that
consumes a reset event. Maximum combo begins at zero and grows only as replay
judgement events are applied. The course loop no longer exposes the final
stage `resultState.maxCombo` from frame one. A course-lifetime observer folds
each stage-local adapter's applied-event maximum without allowing a new stage
to reset the already-earned value.

GREEN: the focused replay test passes one-frame-pulse assertions for both
normal and course consumers; within-stage maximum-combo assertions at zero,
2, and 5; and cross-stage carry/lower/higher assertions at 5, 5, and 8.

### 7. Lua-off real-source-set regression

The valid off-feature build was configured with the existing vcpkg artifacts:

```sh
cmake -S . -B cmake-build-lua-off-vcpkg -G Ninja \
  -DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=OFF \
  -DASOBMASHOW_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=/Users/xf/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR=/Users/xf/workspace/SNURhythm/AsoBMaShow/cmake-build-debug/vcpkg_installed
```

RED command:

```sh
cmake --build cmake-build-lua-off-vcpkg --target main -j 12
```

Expected and observed RED: the real application compiled and then failed at
link with undefined `ReplayPlayfieldPresentation` symbols, including
`create`, `renderFrame`, `applyReplayEvent`, `applyAuthorityUpdate`,
`releaseDueClassicLongNoteTails`, and the destructor. This proved that the
exporter referenced sources omitted by the disabled-feature source set.

The coordinator and replay adapter are now unconditional application sources.
Only the concrete Lua session factory remains conditional. A small
renderer-independent `CoordinatedPlaySkinSession` interface and separated
session identity type let the disabled-feature build retain the built-in
replay presentation without linking the Lua/resource graph.

GREEN command:

```sh
cmake --build cmake-build-lua-off-vcpkg --target main -j 12
```

Result: a 317-step real `main` rebuild compiled and linked; a fresh exact
rerun exited 0 with no work remaining.

### 8. Dead duplicate reducer removal

Before removal, repository search proved that these legacy exporter helpers
had no external call sites and only called each other:

```sh
rg -n \
  'replayNoteKey|buildReplayNoteLookup|resetChartNotes|collectReplayAutoReleaseTails|releaseDueReplayLongNoteTails|ScopedChartNoteReset|findReplayNote|longNoteTailJudgedBeforeTiming|markReplayMissedNote|applyReplayEventForVideo' \
  src/ReplayVideoExporter.cpp
```

After the active adapter's initial state, classic LN, CN/HCN, Mine, HUD,
gauge, lane-cover, and maximum-combo semantics were green, the unused block
was removed. Repeating the same command returns no matches. The separate live
`applyReplayEventToPacemakerState` authority remains.

## Replay overlay compatibility ruling

Reference preflight:

```sh
git -C ../beatoraja rev-parse HEAD
git -C ../beatoraja status --porcelain
```

The reference was re-read at exactly
`c2ed5db1a46145ed10790c3872f717e95b59db9d`; status output was empty.

Pinned source evidence:

- `play/PlaySkin.java:14-56` defines the play-skin fields for timing/line/BPM/
  stop data, lane/group regions, judge behavior, lane cover, practice, and
  character processing. It defines no replay-ghost, miss-marker, or replay-
  touch presentation object.
- `play/SkinNote.java:19-62` delegates skin note drawing to `LaneRenderer`;
  `SkinNote.java:72-102` exposes only note, long-note, mine, hidden-note, and
  processed-note sources.
- `skin/json/JsonPlaySkinObjectLoader.java:30-180` loads notes, long-note
  states, mines, lines, BPM/stop/time lines, and lane geometry. The following
  play-only objects begin with hidden/lift/practice at lines 182 onward; there
  is no ghost/miss/touch object contract.
- `play/BMSPlayer.java:412-417` treats replay like autoplay by disabling live
  input; it does not add a replay overlay presentation.

Existing AsoBMaShow evidence:

- `src/scene/play/GamePlayScene.cpp:4958-4971` captures one visual state and
  projection and submits the active presentation. Its virtual-controller
  overlay at lines 4972-4985 is only drawn when `!options.autoPlay`, so it is
  not a selected-skin replay overlay contract.
- Replay ghosts and miss markers are private built-in renderer work in
  `src/scene/play/BMSRenderer.cpp:3648-3650`; replay touch points are likewise
  a built-in renderer pass at lines 3847-3851.

Ruling: the pinned Beatoraja play-skin surface and AsoBMaShow's live selected-
skin path provide no authoritative shared contract for replay ghosts, miss
markers, or touch visualization over a selected skin. No new overlay was
invented. Existing built-in replay overlays remain unchanged.

## Final verification

Focused:

```sh
./cmake-build-debug/playfield_presentation_coordinator_tests
./cmake-build-debug/replay_playfield_presentation_tests
```

- Coordinator: passed.
- Replay presentation/preflight: passed.

Desktop build and full suite:

```sh
cmake --build cmake-build-debug -j 12
ctest --test-dir cmake-build-debug --output-on-failure -j 12
```

- Full build: passed.
- CTest: 259/259 passed in the final uncontended run (44.27 seconds).
- One intermediate rerun was intentionally overlapped with the 317-step
  Lua-off rebuild and produced only a 30-second timeout in
  `foundation_profile_switch`. The test immediately passed alone in 8.81
  seconds; the subsequent uncontended full run passed all 259 tests.

Lua-off:

```sh
cmake --build cmake-build-lua-off-vcpkg --target main -j 12
```

- Real disabled-feature `main`: compiled and linked.

iOS unsigned release verification:

```sh
scripts/ios_release_verify.sh
```

- Final fresh run passed 61/61 release-critical tests. Python contract suites
  passed 48/48, 13/13, 15/15, and 3/3. The unsigned arm64 device build reported
  `BUILD SUCCEEDED`; signature checking was correctly skipped and the iOS
  artifact audit passed. No upload or distribution occurred.

Static hygiene:

```sh
git diff --check
```

- Passed.

## Changed files

- `CMakeLists.txt` — link the renderer ownership implementation into the
  focused replay test.
- `src/ReplayVideoExporter.cpp` — use authoritative frame/lane-cover/combo
  state, protect presentation destruction, map course/normal preflight, and
  remove the dead reducer.
- `src/scene/play/CMakeLists.txt` — keep the coordinator and replay adapter in
  both feature source sets.
- `src/scene/play/CoordinatedPlaySkinSession.h` — renderer-independent
  coordinator session boundary.
- `src/scene/play/GameplaySkinSessionFactory.cpp` and `.h` — isolate the
  concrete Lua session include to the enabled implementation.
- `src/scene/play/PlayfieldPresentationCoordinator.cpp` and `.h` — preserve
  the authoritative transaction failure and use the common session boundary.
- `src/scene/play/ReplayPlayfieldPresentation.cpp` and `.h` — initial starts,
  Mine/CN/HCN semantics, progressive combo, and lifetime test seam.
- `src/scene/play/ReplayVideoGameplayPreflight.cpp` and `.h` — production
  logical bounds, live-equivalent frame state, renderer-owned preflight/
  destruction, course cleanup, and lane-cover playback.
- `src/skin/beatoraja/PlaySkinSession.h` and
  `src/skin/beatoraja/PlaySkinSessionIdentity.h` — implement the common session
  boundary without pulling the concrete resource graph into Lua-off builds.
- `tests/playfield_presentation_coordinator_tests.cpp` — authoritative
  selected failure regression.
- `tests/replay_playfield_presentation_tests.cpp` — selected initial state,
  production preflight/order/lifetime, logical bounds, timers, CN/HCN, Mine,
  lane-cover, and maximum-combo regressions.
- This report.

## Round-2 corrective integration

This round starts from clean base
`c2d75efa8e1c0e6e4e9ff259b810fb119e032c77` and addresses only the two
Important final-review findings: replay HCN intermediate state and prepared
renderer geometry at the export boundary. Result screens, selected-skin
replay overlays, and preflight ownership/order were not changed.

### HCN compatibility re-check

The Beatoraja reference was re-read at the pinned, clean revision
`c2ed5db1a46145ed10790c3872f717e95b59db9d`. In particular,
`play/LaneRenderer.java:701-720` selects HCN body state from processing,
passing, and hell-charge judgment state. AsoBMaShow's live equivalent was
re-checked in `src/scene/play/GamePlayScene.cpp:4474-4487`: once the head has
reached judgment, lane-down creates the reactive state, holding or reactive
creates the active state, and a reached but inactive HCN is damaged.
`src/skin/beatoraja/Skin2DRenderer.cpp:2208-2222` consumes those flags for the
selected-skin HCN slots.

### Round-2 TDD

#### Replay HCN release, re-press, and missed-head recovery

The focused behavior tests were added first for an accepted HCN head followed
by a judgment-less body release and re-press, and for a missed HCN head
followed by a recorded no-note press. The RED run reported:

```text
FAIL: accepted HCN head projects active reactive state to both endpoints
FAIL: judgement-less HCN body release projects damage to both endpoints
FAIL: HCN body re-press projects reactive recovery to both endpoints
FAIL: missed HCN head projects damage to both endpoints
FAIL: recorded lane press recovers both endpoints after a missed HCN head
5 replay playfield presentation test(s) failed
```

The adapter now records replay lane-down state, tracks the held state of each
HCN pair, and recomputes the live-equivalent reactive/active/damaged flags for
both endpoints at capture/render time. A release without a judgment therefore
shows damage, a later lane press restores the reactive body, and the same
recovery works after a missed head. Existing classic LN/CN tail, judged HCN
tail, Mine, and HUD behavior remains on the existing paths. The focused GREEN
run reported `Replay playfield presentation tests passed`.

#### First-frame prepared renderer geometry

A real-camera behavior test prepares the actual replay presentation at the
primary 1920x1080 camera, switches to a non-primary 3840x1600 export camera,
and calls the public `renderFrame` boundary. The final expectation observes
both the prepared `BMSRenderer` traversal upper bound and its touch-layout
revision. With production unchanged, the RED run reported:

```text
FAIL: first non-primary-aspect export frame refreshes prepared BMSRenderer geometry (primary=8.544023, export=8.544023)
1 replay playfield presentation test(s) failed
```

An earlier probe used an arbitrary minimum numeric delta; the real camera
difference is smaller, so that probe was discarded. The production fix was
reverted before the final observable-state expectation above was established
and RED was recaptured.

`ReplayPlayfieldPresentation::renderFrame` now refreshes its prepared built-in
renderer after the exporter has installed export geometry/camera state and
before either the built-in or selected presentation renders. The same public
render boundary is used by normal and course exports, including selected-skin
fallback, so the correction requires no exporter ordering or presentation
ownership change. The focused GREEN run reported
`Replay playfield presentation tests passed`; the export traversal value and
geometry revision both changed on that first frame.

### Round-2 verification

Focused behavior and shared wiring:

```sh
cmake --build cmake-build-debug --target replay_playfield_presentation_tests -j 12
./cmake-build-debug/replay_playfield_presentation_tests
cmake --build cmake-build-debug --target playfield_presentation_coordinator_tests -j 12
./cmake-build-debug/playfield_presentation_coordinator_tests
```

- Replay presentation tests: passed.
- Coordinator tests: passed.
- The geometry assertion exercises the public shared replay render boundary;
  the real desktop and iOS builds compile both normal and course exporter
  call sites through that boundary.

Desktop and Lua-off:

```sh
cmake --build cmake-build-debug --target main -j 12
ctest --test-dir cmake-build-debug --output-on-failure -j 12
cmake --build cmake-build-lua-off-vcpkg --target main -j 12
```

- Desktop `main`: compiled and linked.
- Full desktop CTest: 259/259 passed (42.51 seconds).
- Lua-off real `main`: compiled and linked.

iOS unsigned release verification:

```sh
scripts/ios_release_verify.sh
```

- Release-critical CTest: 61/61 passed.
- Python contract suites: 48/48, 13/13, 15/15, and 3/3 passed.
- The unsigned arm64 device build reported `BUILD SUCCEEDED`; signature
  checking was correctly skipped and the artifact audit passed.
- No deployment or upload occurred.

Static hygiene:

```sh
git diff --check
```

- Passed.

Round-2 changed files are limited to
`src/scene/play/ReplayPlayfieldPresentation.cpp`,
`src/scene/play/ReplayPlayfieldPresentation.h`,
`tests/replay_playfield_presentation_tests.cpp`, and this report.
