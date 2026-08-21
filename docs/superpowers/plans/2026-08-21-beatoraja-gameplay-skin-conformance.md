# Beatoraja Gameplay-Skin Conformance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove complete gameplay-skin parity across Lua, JSON, and LR2, prevent loading/rendering regressions, pass ModernChic acceptance, and deliver a review-clean pushed branch.

**Architecture:** Drive completion from the source ledger. Use redistributable cross-format fixtures and source-generated traces for correctness, deterministic lifecycle counters plus cold/warm benchmarks for performance, and local third-party skins only as noncommitted acceptance inputs.

**Tech Stack:** C++23, Python 3, pinned Beatoraja Java sources, CMake/CTest, platform build scripts, GitHub CLI.

**Spec:** [`docs/superpowers/specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md`](../specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md)

## Global Constraints

- Execute this plan only after every model/visual/resource/host task and the JSON/LR2 production-dispatch task pass.
- Third-party skin assets and screenshots derived from them remain untracked.
- The committed source ledger must contain no `missing` row at completion.
- Compare LN selectors and every other ambiguous rule with pinned Beatoraja source, not a locally inferred shift.
- Do not deploy to Firebase or TestFlight; only non-distribution build/verification scripts are allowed.
- Keep `vcpkg_installed/` untracked and do not run a whole-file formatter.
- Resolve only valid review findings and preserve one focused commit per correction.

---

### Task 1: Cross-format conformance matrix and complete ledger

**Files:**

- Create: `tests/fixtures/beatoraja_skin/json/all_gameplay_objects.json`
- Create: `tests/fixtures/beatoraja_skin/lr2/all_gameplay_objects.lr2skin`
- Create: `tests/fixtures/beatoraja_skin/model/all_gameplay_objects.expected.json`
- Create: `tests/beatoraja_gameplay_cross_format_tests.cpp`
- Modify: `tests/fixtures/beatoraja_skin/lua/model/all_v1_objects.luaskin`
- Modify: `docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json`
- Modify: `docs/todo.md`
- Modify: `docs/progress.md`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: all three production decoders, canonical model serialization, draw-command fixture serialization, and the complete source ledger.
- Produces: equivalent model/draw assertions for overlapping format features and evidence paths for every ledger row.

- [ ] **Step 1: Write the failing completeness/cross-format test**

  Add a test that decodes equivalent Lua, JSON, and LR2 fixtures, strips only explicit format provenance, and compares canonical object order, bindings, destinations, resources, and draw-command traces. Add a Python ledger assertion that rejects `missing` when invoked with `--require-complete`.

  ```cpp
  expect(normalizeForCrossFormat(luaModel) == normalizeForCrossFormat(jsonModel),
         "Lua and JSON canonical models must match");
  expect(normalizeForCrossFormat(jsonModel) == normalizeForCrossFormat(lr2Model),
         "JSON and LR2 canonical models must match");
  expect(luaCommands == jsonCommands && jsonCommands == lr2Commands,
         "overlapping formats must emit equivalent draw commands");
  ```

- [ ] **Step 2: Run the tests to expose remaining ledger/fixture gaps**

  Run: `cmake --build cmake-build-debug --target beatoraja_gameplay_cross_format_tests -j 6 && ./cmake-build-debug/beatoraja_gameplay_cross_format_tests && python3 tests/beatoraja_gameplay_skin_ledger_tests.py --require-complete`

  Expected: failures identify any row whose implementation/test evidence or cross-format fixture is incomplete.

- [ ] **Step 3: Complete fixtures and ledger evidence**

  Add one minimal fixture case for every valid source-surface row. For format-exclusive behavior, assert the pinned format result rather than fabricating equivalence. Change each implemented ledger row to:

  ```json
  {
    "id": "lua.object.timingvisualizer",
    "status": "implemented",
    "implementation": [
      "src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp"
    ],
    "tests": [
      "tests/skin_draw_command_tests.cpp",
      "tests/beatoraja_gameplay_cross_format_tests.cpp"
    ]
  }
  ```

  Retain `source-defined-noop` only when its pinned path/symbol proves the loader intentionally produces no behavior. Update human-readable docs to point to the complete ledger and describe only verified behavior.

- [ ] **Step 4: Run complete conformance tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_gameplay_cross_format_tests beatoraja_skin_model_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/beatoraja_gameplay_cross_format_tests && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests && python3 tests/beatoraja_gameplay_skin_ledger_tests.py --require-complete`

  Expected: all commands pass and no valid gameplay surface remains missing.

- [ ] **Step 5: Commit the conformance matrix**

  Run: `git add CMakeLists.txt docs/todo.md docs/progress.md docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json tests/beatoraja_gameplay_cross_format_tests.cpp tests/fixtures/beatoraja_skin/lua/model/all_v1_objects.luaskin tests/fixtures/beatoraja_skin/json/all_gameplay_objects.json tests/fixtures/beatoraja_skin/lr2/all_gameplay_objects.lr2skin tests/fixtures/beatoraja_skin/model/all_gameplay_objects.expected.json && git commit -m "test: prove gameplay skin format parity"`

### Task 2: Pinned Beatoraja differential oracle

**Files:**

- Create: `tests/fixtures/beatoraja_skin/oracle/GameplaySkinOracle.java`
- Create: `scripts/generate_beatoraja_gameplay_skin_oracle.py`
- Create: `tests/fixtures/beatoraja_skin/traces/gameplay_objects_pinned_v1.json`
- Create: `tests/beatoraja_gameplay_oracle_tests.py`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: pinned `../beatoraja`, redistributable fixtures, fixed frame times, fixed runtime state, and fixed configuration selections.
- Produces: deterministic source-shaped traces for normalized defaults, selector results, visualizer geometry/statistics, and LR2 command translation.

- [ ] **Step 1: Write a failing committed-trace verifier**

  Require the trace to declare the exact pinned commit, fixture SHA-256 values, oracle schema, fixed viewport/time/state, and one result for every differential-capable ledger row. Compare Aso output to the committed trace with explicit float tolerances and exact integer/order/color values.

- [ ] **Step 2: Run the verifier to confirm the trace is absent**

  Run: `python3 tests/beatoraja_gameplay_oracle_tests.py`

  Expected: failure because the pinned trace and generator are missing.

- [ ] **Step 3: Implement and run the oracle generator**

  The Java harness loads only redistributable fixtures through pinned Beatoraja classes and serializes source values/geometry, not copyrighted assets. The Python wrapper verifies `git -C ../beatoraja rev-parse HEAD`, compiles/runs the harness with the repo's build classpath, canonicalizes JSON ordering, and supports `--check`.

  ```bash
  python3 scripts/generate_beatoraja_gameplay_skin_oracle.py \
    --beatoraja-root ../beatoraja \
    --output tests/fixtures/beatoraja_skin/traces/gameplay_objects_pinned_v1.json
  ```

- [ ] **Step 4: Verify reproducibility and Aso comparison**

  Run: `python3 scripts/generate_beatoraja_gameplay_skin_oracle.py --beatoraja-root ../beatoraja --check && python3 tests/beatoraja_gameplay_oracle_tests.py && ctest --test-dir cmake-build-debug -R beatoraja_gameplay_oracle --output-on-failure`

  Expected: regeneration is byte-stable and Aso matches every asserted trace.

- [ ] **Step 5: Commit oracle evidence**

  Run: `git add CMakeLists.txt scripts/generate_beatoraja_gameplay_skin_oracle.py tests/beatoraja_gameplay_oracle_tests.py tests/fixtures/beatoraja_skin/oracle/GameplaySkinOracle.java tests/fixtures/beatoraja_skin/traces/gameplay_objects_pinned_v1.json && git commit -m "test: compare gameplay skins with pinned Beatoraja"`

### Task 3: Loading performance and resource-lifecycle gate

**Files:**

- Create: `tests/gameplay_skin_loading_benchmark_tests.cpp`
- Create: `scripts/benchmark_gameplay_skin_loading.sh`
- Modify: `src/skin/beatoraja/SkinPerformanceTelemetry.h`
- Modify: `src/skin/beatoraja/SkinPerformanceTelemetry.cpp`
- Modify: `src/skin/beatoraja/SkinLiveResourceCounters.h`
- Modify: `tests/play_skin_session_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: cold/warm session creation for representative Lua/JSON/LR2 fixtures, optional local skin paths, and a clean `develop` baseline worktree.
- Produces: parse/decode/resource/upload phase timings, deterministic no-frame-I/O counters, lifecycle balance assertions, and median baseline comparison.

- [ ] **Step 1: Write failing deterministic lifecycle/per-frame tests**

  Assert resource/file/movie/font/audio reads stop after preparation; five hundred rendered frames do not increase them; repeated shared paths decode once; cancelled/failed sessions publish nothing; and every CPU/GPU/movie/audio resource counter returns to baseline after teardown.

- [ ] **Step 2: Run focused tests to verify missing telemetry**

  Run: `cmake --build cmake-build-debug --target play_skin_session_tests gameplay_skin_loading_benchmark_tests -j 6`

  Expected: phase and per-resource counters needed by the assertions are absent.

- [ ] **Step 3: Implement telemetry and the baseline runner**

  Extend existing telemetry with value-owned phase durations/counters without logging paths or user data. The shell script creates a temporary isolated worktree through the repository's approved worktree workflow, builds the same benchmark target on `develop` and HEAD, runs at least seven cold and seven warm samples, discards the first sample, and compares medians. Accept `--skin PATH` for uncommitted local acceptance skins.

  ```bash
  scripts/benchmark_gameplay_skin_loading.sh \
    --baseline develop \
    --candidate HEAD \
    --maximum-regression-percent 10
  ```

- [ ] **Step 4: Run lifecycle and benchmark gates**

  Run: `cmake --build cmake-build-debug --target play_skin_session_tests gameplay_skin_loading_benchmark_tests -j 6 && ./cmake-build-debug/play_skin_session_tests && ./cmake-build-debug/gameplay_skin_loading_benchmark_tests && scripts/benchmark_gameplay_skin_loading.sh --baseline develop --candidate HEAD --maximum-regression-percent 10`

  Expected: resource counters balance, frames perform no I/O/decode, and both cold/warm median regressions are at most 10 percent.

- [ ] **Step 5: Commit the performance gate**

  Run: `git add CMakeLists.txt src/skin/beatoraja/SkinPerformanceTelemetry.h src/skin/beatoraja/SkinPerformanceTelemetry.cpp src/skin/beatoraja/SkinLiveResourceCounters.h tests/play_skin_session_tests.cpp tests/gameplay_skin_loading_benchmark_tests.cpp scripts/benchmark_gameplay_skin_loading.sh && git commit -m "test: gate gameplay skin loading performance"`

### Task 4: ModernChic gameplay acceptance

**Files:**

- Create: `scripts/verify_modernchic_gameplay_skin.py`
- Modify: `docs/skin-compat/modernchic-scuro-4.6-acceptance.md`
- Modify: `tests/skin_acceptance_contract_tests.py`
- Modify: `tests/fixtures/beatoraja_skin/traces/scuro_property_frames_v1.json`

**Interfaces:**

- Consumes: a user-supplied ModernChic archive/directory, the synthetic acceptance chart, fixed LN/CN/HCN modes, and the selected gameplay session recorder.
- Produces: nonasset acceptance evidence for entry discovery, unsupported diagnostics, draw families, selector results, and load timing.

- [ ] **Step 1: Write failing acceptance-contract assertions**

  Require the verifier to check: zero unsupported valid gameplay object/field/API diagnostics; note distribution, timing, BPM, and hit-error commands present; all referenced resources prepared; Lua callbacks within budgets; and LN/CN/HCN selector/draw mapping equal to pinned Beatoraja. Reject a simple cyclic shift as an oracle.

- [ ] **Step 2: Run the contract test before the verifier exists**

  Run: `python3 tests/skin_acceptance_contract_tests.py`

  Expected: failure because the complete ModernChic verifier contract is absent.

- [ ] **Step 3: Implement local noncommitting verification**

  Accept `--skin PATH` and write all extracted data/screenshots beneath a `mktemp -d` directory. Import into a temporary skin store, run the fixed chart/state frames for every gameplay entry and LN mode, emit a JSON report, and delete the temporary package on normal exit. The committed acceptance document records commands and numeric results but no third-party bytes or derived screenshots.

  ```bash
  python3 scripts/verify_modernchic_gameplay_skin.py \
    --skin /Users/xf/Downloads/Skins/ModernChic460.zip \
    --beatoraja-root ../beatoraja
  ```

- [ ] **Step 4: Run ModernChic and contract verification**

  Run: `python3 scripts/verify_modernchic_gameplay_skin.py --skin /Users/xf/Downloads/Skins/ModernChic460.zip --beatoraja-root ../beatoraja && python3 tests/skin_acceptance_contract_tests.py`

  Expected: both pass, all four graph families appear, LN modes match pinned Beatoraja, and no copyrighted asset becomes tracked.

- [ ] **Step 5: Commit acceptance tooling/evidence**

  Run: `git add scripts/verify_modernchic_gameplay_skin.py docs/skin-compat/modernchic-scuro-4.6-acceptance.md tests/skin_acceptance_contract_tests.py tests/fixtures/beatoraja_skin/traces/scuro_property_frames_v1.json && git commit -m "test: verify ModernChic gameplay skin parity"`

### Task 5: Full verification, review loop, and push

**Files:**

- Modify if evidence changes: `docs/skin-compat/beatoraja-lua-gameplay-final-review.md`
- Modify if evidence changes: `docs/progress.md`
- Modify only for valid findings: files identified by self-review, GitHub review, or CI.

**Interfaces:**

- Consumes: complete ledger, all local tests/builds, PR review threads, GitHub checks, and clean-worktree audit.
- Produces: a review-clean, verified, feature-by-feature branch pushed to its configured upstream.

- [ ] **Step 1: Run source/placeholder/worktree audits**

  Run: `python3 tests/beatoraja_gameplay_skin_ledger_tests.py --require-complete && python3 scripts/extract_beatoraja_gameplay_skin_surface.py --beatoraja-root ../beatoraja --check && rg -n "unsupported|TBD|TODO|FIXME" docs/skin-compat src/skin/beatoraja tests/fixtures/beatoraja_skin && git status --short && git log --oneline origin/feature/skin-compat..HEAD`

  Expected: ledger/source checks pass; every remaining `unsupported` occurrence is an intentional invalid-input test or explicit source-defined case; only pre-existing `vcpkg_installed/` is untracked; commits are feature-sized.

- [ ] **Step 2: Run complete local verification**

  Run: `cmake --build cmake-build-debug --target main -j 6 && ctest --test-dir cmake-build-debug --output-on-failure -j 6 && scripts/android_firebase_deploy.sh --build-only --variant playDebug && scripts/ios_release_verify.sh`

  Expected: desktop build/tests, Android playDebug compilation, release-critical native tests, and unsigned iOS build all pass; no distribution occurs.

- [ ] **Step 3: Perform the first complete self-review**

  Review every commit and the aggregate diff against the spec and ledger. Inspect ownership/cancellation, format defaults, draw ordering, frame allocations/I/O, sandbox expansion, replay/export state, diagnostics, and tests. For each actionable finding, write a failing regression test, fix it, rerun focused/full verification as appropriate, and commit only that correction as `fix: <specific behavior>`. Repeat until a full pass finds nothing actionable.

- [ ] **Step 4: Push the locally complete candidate and process PR feedback**

  Run: `git push origin feature/skin-compat`

  Then inspect PR checks and all unresolved review threads using the repository GitHub review workflow. Verify each comment against the pinned source/spec. For every valid finding: add a failing test, implement the smallest fix, run focused verification, commit separately, push, and resolve only the fixed thread. Ignore invalid findings with an evidence-backed reply. Repeat until checks pass and no valid unresolved thread remains.

- [ ] **Step 5: Run the final post-review self-review and verification**

  Run: `cmake --build cmake-build-debug --target main -j 6 && ctest --test-dir cmake-build-debug --output-on-failure -j 6 && scripts/android_firebase_deploy.sh --build-only --variant playDebug && scripts/ios_release_verify.sh && git diff --check && git status --short --branch`

  Expected: every command passes, the branch is synchronized with its upstream, and only pre-existing `vcpkg_installed/` remains untracked. If this review finds an issue, return to Step 3 rather than declaring completion.
