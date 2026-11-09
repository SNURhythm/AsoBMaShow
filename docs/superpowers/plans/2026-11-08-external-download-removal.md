# External Download Removal Implementation Plan

**Goal:** Remove BMS network download and embedded browsing, and expose the
developer's apology offline from the start screen in all five app languages.

**Architecture:** Delete the dedicated network/browser subsystems and remove all
application entry points. Preserve independent local archive and library code.
Use the existing virtualized document reader for bundled apology assets.

**Spec:** `docs/superpowers/specs/2026-11-08-external-download-removal-design.md`

## Task 1: Remove download integration

- [x] Add removal contract coverage, then observe the old manifest fail it.
- [x] Remove `src/BmsSearchService*`, `src/bms_search`, `src/downloads`, Find BMS
  dialogs/workers, browser UI, download settings and obsolete tests/build inputs.
  Preserve any small local-only helper still used elsewhere under its proper
  owner (byte formatting and ignoring legacy transaction directories).
- [x] Remove selector/tutor download controls and Downloads in Tasks.
- [x] Retire legacy iOS background sessions and Android scheduled jobs without
  resuming requests, and remove their platform registrations.
- [x] Verify local archive/library paths and the affected platform contracts.

## Task 2: Add the public apology

- [x] Extend `tests/intro_scene_navigation_tests.cpp` with navigation, activation
  and bounds for the fourth start-screen action; observe failure first.
- [x] Bundle original Korean and four complete translations in `assets/notices`.
- [x] Add notice language/asset mapping and a scrollable in-app reader with a
  close action and Korean-original access; wire it into `IntroScene`.
- [x] Validate notice assets, navigation, input isolation, layout and lifecycle.

## Task 3: Document and verify

- [x] Update feature documentation and external-service review conventions with
  current removal state and the evidence needed before adding/re-enabling APIs.
- [x] Compile desktop main and affected native tests; run relevant groups in
  parallel at `-j 6`. Perform unsigned mobile compile checks where practical.
- [x] Review the complete uncommitted change, fix important findings and deploy
  iOS Firebase as subsequently requested. Report the branch, tests and remaining
  limitations. Commit and branch integration were subsequently requested on
  November 9 after a final translation review.

## Review focus

- Old OS download jobs must not restart after upgrading.
- Missing charts must not expose a download action through skins or tutorial.
- Local file import and completed chart libraries must remain usable.
- The notice must be readable and closable with touch, mouse and keyboard;
  opening it must not also launch gameplay through retained input.
- Every supported language must have complete offline content; Korean must be
  identical to the user's final draft, including its original wording and dates.

## Verification record

- Removal manifest and fourth Intro action tests were observed failing before
  implementation. Removal, navigation and real-view notice coverage now pass.
- The Korean asset was written directly from the final user message and compared
  against it again; the content is identical (apart from a terminal newline).
- A fresh read-only review found dangling iOS `-framework` flags after deleting
  WebKit. A regression check failed first, then passed after all four flags were
  removed. The stale source-group metadata entry was removed too.
- The same review found outdated tutorial welcome copy. It now describes
  difficulty tables, local folder import and play options in all five languages.
- Full-suite execution exposed a few already-stale extracted-view/IR fixtures;
  their small stubs were brought into line with current production interfaces.
- Ruling: the user's later iOS Firebase request supersedes the original
  no-deployment constraint. Use the repository deployment script after checking
  the complete working tree. On November 9, the user subsequently authorized
  commit/push, merge into `develop`, and fast-forward of `main`.
- Real-device upgrade cancellation has been reviewed in code but is not yet a
  device-tested scenario; permission claims are not revalidated by network calls.

- iOS Firebase deployment succeeded via `scripts/ios_firebase_deploy.sh`: version
  `0.0.1`, build `202611081153`, release `0up67c1muukuo`. Signed artifact audit
  passed. All five notice files in the exported IPA match the source bytes.
- Full CTest verification passed: 545/545 tests, 71.54 seconds at `-j 6`.
  Deployment-only Xcode signing and formatting changes were removed afterward;
  the task changes were left uncommitted for review at that point.

- Final translation review preserves the Korean original and its November 8
  drafting / November 9 publication dates. English and Chinese now explicitly
  identify AsoBMaShow as the API client; Japanese phrasing was refined without
  changing responsibility, permission uncertainty, or the promised actions.
