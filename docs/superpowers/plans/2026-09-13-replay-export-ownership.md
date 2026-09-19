# Shared Records implementation plan

**Goal:** Complete Music Select Records integration and remove duplicate Records
execution/data ownership from the selectors.

**Spec:** [Shared Records integration](../specs/2026-09-13-replay-export-ownership-design.md)

## Constraints

- Keep behavior unchanged outside explicit user decisions and correctness fixes
  required by shared ownership and retained navigation.
- Preserve replay validation, scoring, schemas, encoders, and course constraints.
- Use the supplied checkout; do not create a worktree or run whole-file formatters.
- Build in `cmake-build-debug` with `-j 6`; do not deploy.
- Commit cohesive verified changes: worker ownership, shared record services,
  then selector integration and regression coverage.

## Implementation

- [x] Establish the original desktop build and CTest baseline before scene edits.
- [x] Extract lightweight export types and `ReplayExportJob`; test its lifecycle
  with real workers and failure injection.
- [x] Extract `ReplayRecordTask`; test ownership, cancellation, late publication,
  and termination without a result.
- [x] Extract chart/course recall, merged record loading, file actions, and IR
  actions. Validate data and resource lifetime with repository-backed tests.
- [x] Connect both selectors to shared services. Complete Music Select course,
  remote recall, sharing/deletion, IR actions, and live status refresh.
- [x] Preserve retained selector navigation through local/remote results and
  course replay stages.
- [x] Apply the user's background, pacemaker, audio-failure, and diagnostic
  decisions, and ask about additional reachable differences.
- [x] Finish policy convergence and independent review findings.
- [x] Update scene fixtures and run the complete rebuilt CTest suite.
- [x] Prepare the verified integration for the third cohesive commit.

## Validation record

The original desktop app built successfully. The initial full suite had four
failures; isolated rechecks passed `music_select_settings_runtime` and
`visual_catch_up`, while `lua_skin_coroutine_callback` and `image_view_fade_tests`
still failed before scene edits. Rebuilding test targets is part of final
validation, since the initial app-only build did not refresh every test binary.

The worker-ownership and shared-record-service native suites passed before their
respective commits. Final validation results are recorded with the completed
integration, distinguishing baseline failures from regressions.

Final policy regression coverage exercises matching and mismatched preloaded
RANDOM choices, selected ClubMode, and explicit autoplay presentation options.
The 50-test Music Select behavior module and the desktop app/all-target build
passed after integration. Independent review found no remaining correctness
blockers. The final `ctest --test-dir cmake-build-debug --output-on-failure -j 6`
run passed all 367 tests in 90.86 seconds, including the original failing
baseline cases after rebuilding their binaries. `git diff --check` passed.
Physical-device verification and deployment were not performed.

Delivery uses the current `chore/repo-refactor` branch, with worker ownership,
shared services, and selector integration in three commits. Push this branch
upstream after recording the final integration commit.
