# Release blocker review — 2026-10-03

Base: `484a2d69` (`develop`, PR #115 merged).
Fix branch: `fix/release-blockers-2026-10-03`.

## Confirmed release-gate blocker

The macOS workflow's required cross-platform release check fails before
packaging or publication. `test_download_redirects_cannot_downgrade_https_to_http`
expects exactly three curl redirect guards in `DownloadSupport.cpp`. The new
`probeDownloadUrl` HEAD request correctly adds a fourth, producing `4 != 3`.

The fix audits each curl request between URL setup and execution. Every request
must restrict redirects using the initial URL's protocol policy. Adding another
protected request no longer fails the release gate, and an extra guard in one
request cannot compensate for a missing guard in another. Runtime download
behavior is unchanged.

The original failure was reproduced with:

```sh
python3 -m unittest discover -s tests -p '*release*tests.py'
```

After the fix, all 68 release-contract tests pass. Independently replacing each
of the five policy calls in memory with unrestricted `http,https` makes the
updated check fail, including the new HEAD probe and difficulty-table request.

## Intermittent verification failures

Rebuilding all targets and repeating CTest exposed additional timing assumptions:

- `find_bms_ios_file_bridge_tests` successfully downloaded the exact expected
  archive using offsets `0,16384,16384`, but required `0,16384,32768`. Foundation
  can checkpoint fewer bytes than the server has sent. Resume assertions now
  require the expected attempt count, bounded resume offsets, and the
  existing exact final-byte comparison. The server-ignores-range case receives
  the same checkpoint tolerance. A subsequent run returned `0,0,16384` with the
  correct complete archive, confirming that a checkpoint may also be empty.
  Zero-byte restart remains valid. The user-approved recovery scenario still
  checks native resume availability and partial-file reuse. Restart,
  cancellation, and retry limits remain covered.
- `lua_skin_runtime_tests` ran alongside sibling tests despite retaining the
  production 4 ms callback deadline. It failed with
  `skin_lua_wall_time_limit_exceeded`; a focused rerun passed. It now uses
  `RUN_SERIAL`, as the existing Lua host and session tests already do. Runtime
  deadlines are unchanged.
- `play_skin_session_tests` used the production wall clock while verifying
  state, timers, input, and rendering. Diagnostics confirmed
  `skin_lua_wall_time_limit_exceeded` in otherwise valid custom-timer and
  input-snapshot callbacks; other runs failed different callbacks. The session
  suite now controls wall time through the existing test-only runtime hooks.
  Its instruction and allocation limits remain active. Production builds do
  not compile the clock override and retain their real monotonic clock and
  unchanged deadlines. The old session timing assertion would become vacuous
  under a controlled clock, so it is replaced by runtime tests for the exact
  4 ms callback and 6 ms cumulative frame boundaries, overrun rejection, and
  frame reset behavior. The existing real-clock host-call timeout test was
  defined but absent from the test runner; it is now explicitly invoked.
  Both Lua suites passed five consecutive focused runs after these changes.

The failed Lua test also prevented its dependent
`beatoraja_music_select_skin_ledger_evidence_contract` from running. These
failures were observed during concurrent Android CI work on this machine.
Serial CTest scheduling cannot isolate tests from external processes, and no
production deadline was relaxed to accommodate machine contention.

## Review scope and limits

Review covered recent first-launch navigation, scene/view callback lifetimes,
download recovery, and release workflow checks. The desktop app and all test
targets build successfully. After the final changes and full rebuild, the
complete parallel CTest run passed 411/411 tests in 184.50 seconds, including
the native iOS transfer and dependent evidence checks.

Additional verification:

- Release contracts: 68/68 passed.
- iOS build setup: 49/49 passed.
- iOS artifact-auditor tests: 16/16 passed.
- macOS artifact-auditor tests: 7/7 passed.
- Independent code review: findings addressed; final review approved with no
  remaining actionable findings.

No additional runtime blocker was confirmed in the reviewed paths.

This review does not establish signed release readiness. No full iOS/Android
build, physical-device smoke test, Windows build, signing/notarization, store
validation, or deployment was performed. Artifact-audit unit tests validate the
auditors; they do not substitute for auditing a signed release artifact.
