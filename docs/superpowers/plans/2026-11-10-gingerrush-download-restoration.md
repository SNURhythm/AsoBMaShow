# GingerRush-only Download Restoration Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan.

**Goal:** Restore existing download UX with GingerRush as the only source.

**Architecture:** Selectively recover shared code from the parent of removal
commit `2396f859`; replace provider selection with a GingerRush-only driver.
Isolate mobile state from pre-retirement downloads and retain browser removal.

**Tech Stack:** C++23, CMake/CTest, libcurl, NSURLSession, Android Java transport.

**Spec:** `docs/superpowers/specs/2026-11-10-gingerrush-download-restoration-design.md`

## Global Constraints

Only GingerRush is authorized. No calls to HorieYuuka or other retired sources.
Preserve the original apology and unrelated Xcode changes; append the requested
November 10 update in all five languages. No deployment/worktree. Work on
`fix/restore-gingerrush-downloads`, created after checking out `develop`.

## Review Focus

- Invalid MD5 or missing metadata must make zero network requests.
- Lookup/transfer errors must not trigger any other source, including durable fallbacks.
- Old saved jobs and native background callbacks must not revive unapproved URLs.
- Both selectors, tutorial and task UI must agree with the MD5-only capability.
- Download cancellation, archive verification and local import must retain recovery.

## Task 1: Source boundary and shared download flow

- [x] Update packaging contracts and observe failure for missing GingerRush support.
- [x] Recover archive/workflow tests and shared implementation; replace source
  selection with `GingerRushDriver::lookupByMd5` and its single-source download.
- [x] Exercise real provider parsing/selection with controlled network responses:
  success, no result, malformed JSON, invalid URLs, rejection and cancellation.

## Task 2: UI and mobile recovery

- [x] Restore selector/task/settings wiring while omitting all browser/search actions.
- [x] Restore native transfer support with a new private-root/session namespace;
  keep retirement callbacks limited to legacy state.
- [x] Restore relevant download tests and update UI/platform fixtures to the new scope.

## Task 3: Verify and deliver

- [x] Build main and affected test groups with a single build per build directory.
- [x] Run download, selector, UI and mobile contracts; broaden when shared impact warrants.
- [x] Run unsigned/build-only platform checks where practical. No upload.
- [x] Review source references and changes and update docs for branch delivery.

## Verification results

- Desktop `main` build succeeded.
- All 149 affected CTests passed across downloads, music selection, settings,
  UI and library groups, including native transport and archive recovery tests.
- iOS unsigned build succeeded with `--build-only --skip-init`.
- Android restricted-file release build succeeded with `--build-only`.
- Review identified a discarded GingerRush lookup diagnostic; the modal now
  displays it, with a regression test covering the error-detail path.
- No live provider requests or deployment actions were used for verification.
- Task files are delivered on `fix/restore-gingerrush-downloads`; the pre-existing
  Xcode project-file modification is excluded.
