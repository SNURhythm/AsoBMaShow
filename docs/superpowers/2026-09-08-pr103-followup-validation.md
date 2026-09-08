# PR 103 follow-up validation

## Scope

After pushing selector SQL paging in `dfa5bba8`, fetched all PR review bodies,
inline comments and conversation comments. Filtered findings to 2026-09-08 in
Asia/Seoul, excluded quota notices, and skipped previously handled findings.
This follow-up covers six new inline findings:

| Finding | Change | Verification |
| --- | --- | --- |
| [3954193196](https://github.com/SNURhythm/AsoBMaShow/pull/103#discussion_r3954193196) | Restore original CMake blank-line separators without changing source registrations. | Whitespace-insensitive diff is empty; ordinary diff contains only restored blank lines. |
| [3954193205](https://github.com/SNURhythm/AsoBMaShow/pull/103#discussion_r3954193205) | Pass shared launch cancellation to parsing/audio loading; reject deferred completion after cleanup or failure. | Extracted production worker/cleanup regression fails before the fix and passes afterward, including parser/audio cancellation and stale handoff. |
| [3954193214](https://github.com/SNURhythm/AsoBMaShow/pull/103#discussion_r3954193214) | Support Escape/gamepad Back to Intro and Enter/gamepad activation to Settings; do not retain the failed selector. | Extracted production error/event/Settings regressions fail before the fix and pass afterward with Lua enabled and disabled. Pointer events bypass stale selector modals; healthy Settings retains its previous return behavior. |
| [3954193223](https://github.com/SNURhythm/AsoBMaShow/pull/103#discussion_r3954193223) | Poll waiter cancellation outside archive-index mutexes without cancelling the shared builder. | Real-ZIP gated regressions fail before the fix, pass afterward, and pass ten consecutive suite runs. Covers a paused builder, completion during callback, healthy waiters and cache reuse. |
| [3954503989](https://github.com/SNURhythm/AsoBMaShow/pull/103#discussion_r3954503989) | Abort course/folder-autoplay launch on unsuccessful audio staging. | Extracted production course/autoplay regressions fail before the fix and pass afterward. Covers failed staging, successful retry, cancellation and chart ownership. |
| [3954504000](https://github.com/SNURhythm/AsoBMaShow/pull/103#discussion_r3954504000) | Compare recent improvements against combined applicable legacy/shared/exact LN-mode history. | 48 real SQLite fixtures fail before the fix and pass afterward. Covers worse/equal/better scores, independent score/lamp gains, unrelated-mode exclusion and all playable modes. |

## Final checks

- Desktop all-target build and explicit `main` build pass using `-j 6`.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: **329/329
  pass**, 73.92 seconds. This includes the previously intermittently failing
  renderer characterization test; no renderer changes were made.
- Independent scoped reviews of all five behavioral fixes report no blockers.
- Unsigned iOS `scripts/ios_firebase_deploy.sh --build-only --skip-init` passes
  after all fixes. Initialization had already completed during SQL-paging
  validation; this command does not archive, sign or upload.
- `git diff --check` passes. No distribution or mobile-device smoke test is part
  of this follow-up.
