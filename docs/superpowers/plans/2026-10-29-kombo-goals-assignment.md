# Kombo fixed rewards and Goals assignment

> Use superpowers:executing-plans to implement these tasks in this checkout.

**Goal:** Regular rounds grant 10 points (capped at the remaining daily target), extra rounds retain 1 point, and every routine includes a Goals assignment.

**Design:** Keep historical daily rewards and totals readable. Add a built-in assignment identified separately from difficulty-table assignments, with a default quota of one newly achieved personal goal. Its quota is editable at creation or for tomorrow, and it cannot be deleted or adjusted today. Feed committed first-achievement events into the existing round accounting and save them atomically with the score. Goals need no table membership. Existing extra-phase rules retain one completion per round and the shared daily bonus cap.

**Tech stack:** C++23, SQLite, existing Kombo JSON storage and native views.

**Constraints:** No worktree or deployment. Preserve historical totals, savings, partial rounds and goal dates. No credit for imported, already achieved, practice, autoplay or replay results. Multiple goals/scopes and changed targets are distinct achievements. No payload scans.

## Tasks

- [x] Extend model/store tests for fixed future rewards, legacy totals, mandatory Goals assignment, configurable quotas, duplicate achievement events and shared bonus limits; observe failures. Implement in `KomboModel`/`KomboStore` with versioned migration.
- [x] Extend score repository integration tests for achievement/round atomicity and retry deduplication. Pass new goal events to Kombo before committing the score transaction.
- [x] Remove reward input, add the permanent Goals quota editor and localized progress labels in `KomboScene`, `KomboForm`, and result views. Route its action to Personal goals; exclude it from table-only assignment folders and availability adjustment.
- [x] Build desktop main and focused Kombo/result/music-select tests, run affected tests, inspect diff, update feature docs, then commit and push the current branch and link PR #162.

## Review focus

Historical points must not be recomputed. Both score-save failures and achievement-write failures must remain retryable. Goal scope identity must not collapse LN modes or changed targets. Goals must remain usable without installed tables. Tomorrow's quota must not change today's partial progress.

## Verification

- Desktop `main` and the results, music-select and UI test targets built successfully.
- All 111 tests in those groups passed, plus `ir_score_import_projection_tests`.
- Regression coverage exercises legacy rewards above and below 10, goal quotas,
  next-day edits, duplicate events, multiple achievements, bonus limits, score and
  goal write rollback, and retry after failure. Native row layout coverage checks
  the vertically centered recommendation title and recycled artist visibility.
- Independent read-only review found no blocking issues. Its bonus-editor reward
  label finding was fixed by retaining the existing no-reward-label behavior in
  Adjust today; the dashboard shows the current phase reward.
- Live gameplay, mobile builds and physical-device interaction were not run.

## Follow-up: missing Near lamp decorations

The shared accent depended on a resize callback that parent-driven layout can
skip, leaving its internal rounded view zero-sized. Moved it to a reusable view
header and synchronized its geometry on layout. A native rendering regression
failed with no draw before the fix, then passed for initial layout and movement.
Desktop main rebuilt and both chart-list rendering and goal-scene tests passed.
