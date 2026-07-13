# Fast-Iteration Audit Remediation Progress Note

> **For agentic workers:** Keep each finding in its own reviewable commit and update this note with the verification result after completing it.

**Goal:** Resolve all seven findings from the UI fast-iteration audit without broad refactors or parser changes.

**Architecture:** Fix each defect at the state owner: persisted-setting fixtures at their test boundary, replay gauge initialization in replay-state construction, eligibility precedence in provenance aggregation, gauge failure state in `RhythmState`, and UI presentation values at their model-to-view boundary. Preserve the global `View` rotation API and make its rendering contract coherent rather than moving rotation back into `TextView`.

**Tech Stack:** C++20, CMake/CTest, bgfx UI rendering.

## Global Constraints

- Audit range: `bda6e278c8fdf8ec1f413c2ffd19e3fef00acba2` through `c913d0df0e03b906e56384caed73424e6da3fe19`.
- Keep existing settings clear except where old replay compatibility explicitly requires legacy behavior.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp` directly.
- Keep the remediation narrow; update existing tests and add only focused regression coverage.
- Commit each independently reviewable finding separately.

## Baseline

- [x] Production target compiled before remediation.
- [x] 53 unaffected registered tests passed before remediation.
- [x] Four registered test targets were broken by stale gauge-auto-shift boolean fixtures.
- [x] Parser amalgamation matched the sibling parser build output.
- [x] Gauge property tables and guts thresholds matched beatoraja.

## Findings and Progress

### 1. Gauge auto-shift enum test migration

**Files:** `tests/practice_launch_tests.cpp`, `tests/practice_preset_store_tests.cpp`, `tests/profile_archive_tests.cpp`, `tests/app_settings_store_tests.cpp`

- [x] Replace stale boolean enum assignments and assertions with explicit `GaugeAutoShiftMode` values.
- [x] Preserve the intentional legacy-settings default of `None`; retain old pure-GAS migration to `BestClear` only where the legacy field actually requests GAS.
- [x] Build and run all four affected test targets.

### 2. Replay video initial and carried gauge state

**Files:** `src/ReplayVideoExporter.cpp`, `src/ReplayResultStateBuilder.h`, `src/ReplayResultStateBuilder.cpp`, relevant existing replay tests

- [x] Derive the first rendered gauge from `RhythmState::configureGauge`, including Best Clear/Select-to-Under and custom starting percentage.
- [x] Seed each course stage from the previous stage's full gauge snapshot so the HUD and simulation agree before the first event.
- [x] Treat an initially failed survival gauge as immediate failure during export.

### 3. Modified course eligibility precedence

**Files:** `src/ScoreProvenance.cpp`, `src/CoursePlaySession.cpp`, `tests/score_provenance_tests.cpp`

- [ ] Make `Modified` dominate `LegacyUnverified` when aggregating course stages.
- [ ] Keep a truly legacy-only or incomplete unmodified course `LegacyUnverified`.
- [ ] Verify mixed Modified/Legacy courses remain excluded from best-score queries.

### 4. Zero-percent survival gauge resolution

**Files:** `src/scene/play/RhythmState.h`, an existing focused gauge/practice test target

- [x] Synchronize survival-failure flags whenever a custom starting value is applied.
- [x] Resolve Best Clear, Select-to-Under, and Survival-to-Groove shifts immediately at time zero.
- [x] Preserve Continue behavior while making direct survival gauges fail immediately.

### 5. Practice default gauge slider position

**Files:** `src/scene/PracticePanelView.h`, `src/scene/PracticePanelView.cpp`, `src/scene/ChartViewer.cpp`

- [ ] Pass the configured gauge's real default value to the panel.
- [ ] Use that value for the `Default` thumb while keeping the optional override and dynamic maximum intact.

### 6. Autoplay summary final gauge

**Files:** `src/ReplayAutoPlay.h`, `tests/replay_summary_list_tests.cpp`

- [ ] Resolve the active autoplay gauge and profile instead of hardcoding `100`.
- [ ] Represent PMS groove capacity correctly and retain survival-gauge behavior.

### 7. Global `View` rotation contract

**Files:** `src/view/View.h`, `src/view/View.cpp`, affected view renderers and focused rendering tests where available

- [ ] Centralize rotation geometry in `View` so non-text renderers do not silently ignore a public base property.
- [ ] Keep existing vertical gauge labels visually unchanged.
- [ ] Verify ordinary zero-degree rendering remains unchanged.

## Final Verification

- [ ] `cmake --build cmake-build-debug --target main -j 6`
- [ ] Full registered CTest suite passes.
- [ ] `git diff --check` reports no errors.
- [ ] Worktree contains only intentional remediation commits and the completed progress note.

## Commit Log

- Finding 1 — `test: finish gauge auto shift enum migration`
- Finding 4 — `fix: resolve zero percent survival gauge starts`
- Finding 2 — `fix: carry effective gauge state into replay exports`
