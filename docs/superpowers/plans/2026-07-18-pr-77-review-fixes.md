# PR 77 Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apply every still-actionable PR #77 review finding while preserving the already-fixed long-note timing behavior.

**Architecture:** Keep secret-file durability separate from ordinary versioned JSON backups, validate optional per-profile operational files at the profile boundary, reconstruct replay results with their recorded ruleset, and move outbox wake-up selection into an exact per-provider SQLite aggregate. Each behavior receives a regression test before production code changes.

**Tech Stack:** C++23, SQLite, nlohmann/json, CMake/CTest.

## Global Constraints

- Do not change the result-screen BREAK definition; it remains BAD + POOR without KPOOR.
- Do not put API keys into IR outbox rows, logs, diagnostics, or backup generations.
- Preserve legacy replay reconstruction as Beatoraja when no supported recorded ruleset exists.
- Preserve deferred Tachi/Bokutachi jobs as polls during Retry All.
- Do not push, merge, deploy, or upload as part of this plan.

---

### Task 1: Credential validation and no-backup atomic writes

**Files:**
- Modify: `tests/ir_credential_store_tests.cpp`
- Modify: `src/AtomicFile.h`
- Modify: `src/AtomicFile.cpp`
- Modify: `src/VersionedJson.h`
- Modify: `src/VersionedJson.cpp`
- Modify: `src/ir/IrCredentialStore.cpp`

**Interfaces:**
- Consumes: `atomic_file::Operations` and versioned JSON object encoding.
- Produces: `atomic_file::writeWithoutBackup(...)` and `versioned_json::saveAtomicWithoutBackup(...)`; credential saves reject bytes `<= 0x20` and `0x7f`.

- [ ] **Step 1: Write failing credential tests**

Add cases that reject leading/trailing whitespace, newline/control bytes, and DEL on save and load; assert rotating and removing a key leaves none of `.bak`, `.bak.pending`, or `.bak.previous`; retain the injected final-replace failure assertion that the old canonical credential file survives.

- [ ] **Step 2: Run the credential test to verify RED**

Run: `cmake --build cmake-build-debug --target ir_credential_store_tests -j 6 && ctest --test-dir cmake-build-debug -R '^ir_credential_store_tests$' --output-on-failure`

Expected: FAIL because whitespace keys are accepted and the ordinary JSON writer retains `ir-credentials.json.bak`.

- [ ] **Step 3: Implement minimal secret-safe persistence**

Implement a temporary-file-plus-replace writer that never creates a prior-generation backup, removes legacy backup artifacts, syncs the installed directory metadata, and leaves the old canonical file untouched when the replace operation fails. Route credential JSON through that writer. Change key validation to:

```cpp
std::ranges::none_of(value, [](unsigned char character) {
  return character <= 0x20 || character == 0x7f;
});
```

- [ ] **Step 4: Run the credential test to verify GREEN**

Run: `cmake --build cmake-build-debug --target ir_credential_store_tests -j 6 && ctest --test-dir cmake-build-debug -R '^ir_credential_store_tests$' --output-on-failure`

Expected: PASS.

### Task 2: Optional profile operational-file safety

**Files:**
- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `src/PlayerProfileManager.cpp`

**Interfaces:**
- Consumes: `ensureContainedPath(...)`, `hasUnsafeLink(...)`, and `PlayerProfilePaths`.
- Produces: profile validation that permits a missing operational file but requires an existing `ir-credentials.json` or `bokutachi-cache.json` to be a contained, non-link regular file.

- [ ] **Step 1: Write failing profile safety tests**

Create symlink fixtures at both optional paths and assert `validateProfile(...)` rejects each while leaving the external targets unchanged.

- [ ] **Step 2: Run the profile test to verify RED**

Run: `cmake --build cmake-build-debug --target player_profile_manager_tests -j 6 && ctest --test-dir cmake-build-debug -R '^foundation_profile_manager$' --output-on-failure`

Expected: FAIL because the optional files are not currently inspected.

- [ ] **Step 3: Validate optional files**

Add a helper that first checks containment, then uses `symlink_status`; `not_found` succeeds, while links, directories, reparse points, and other non-regular entries fail with an integrity diagnostic. Invoke it for both operational paths from `validateProfileFiles(...)`.

- [ ] **Step 4: Run the profile test to verify GREEN**

Run: `cmake --build cmake-build-debug --target player_profile_manager_tests -j 6 && ctest --test-dir cmake-build-debug -R '^foundation_profile_manager$' --output-on-failure`

Expected: PASS.

### Task 3: Recorded-ruleset replay gauge reconstruction

**Files:**
- Modify: `tests/replay_summary_list_tests.cpp`
- Modify: `src/ReplayResultStateBuilder.cpp`

**Interfaces:**
- Consumes: `ReplayData::provenance.ruleset`, `gameplayRulesetFromId(...)`, and the existing `RhythmState` ruleset constructor.
- Produces: replay result states whose active and non-active gauge histories use the replay's supported recorded ruleset; legacy/unknown provenance remains Beatoraja.

- [ ] **Step 1: Write a failing replay reconstruction test**

Build an LR2 replay, apply a scored event, and assert `state.gaugeRules().ruleset == GameplayRuleset::LR2` plus a non-active typed gauge history equals a directly simulated LR2 state. Also assert legacy provenance remains Beatoraja.

- [ ] **Step 2: Run the replay test to verify RED**

Run: `cmake --build cmake-build-debug --target replay_summary_list_tests -j 6 && ctest --test-dir cmake-build-debug -R '^replay_summary_list_tests$' --output-on-failure`

Expected: FAIL because `BuildInitialGaugeState` always constructs the default Beatoraja state.

- [ ] **Step 3: Initialize from supported replay provenance**

Resolve the recorded descriptor only when `isSupportedRulesetDescriptor(...)` succeeds, otherwise fall back to Beatoraja, and construct `RhythmState(&chart, false, ruleset, gaugeProfile)` before configuring GAS.

- [ ] **Step 4: Run the replay test to verify GREEN**

Run: `cmake --build cmake-build-debug --target replay_summary_list_tests -j 6 && ctest --test-dir cmake-build-debug -R '^replay_summary_list_tests$' --output-on-failure`

Expected: PASS.

### Task 4: Retry All includes delayed pending rows

**Files:**
- Modify: `tests/ir_submission_service_tests.cpp`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`

**Interfaces:**
- Consumes: `ReplayRepository::RetryAllIrOutbox(...)`.
- Produces: bulk retry updates states `(0,2,3,4)` while preserving state-2/state-3 remote poll identity rules.

- [ ] **Step 1: Write a failing delayed-pending Retry All test**

Add a future-scheduled Pending row to the existing failed/deferred fixture; expect four affected rows and assert the Pending row becomes due now with explicit user intent.

- [ ] **Step 2: Run the service test to verify RED**

Run: `cmake --build cmake-build-debug --target ir_submission_service_tests -j 6 && ctest --test-dir cmake-build-debug -R '^ir_submission_service_tests$' --output-on-failure`

Expected: FAIL because state `0` is absent from the bulk UPDATE predicate.

- [ ] **Step 3: Include Pending in the UPDATE**

Change the predicate to `state IN (0,2,3,4)` without altering the CASE expressions that preserve remote polling rows.

- [ ] **Step 4: Run the service test to verify GREEN**

Run the command from Step 2.

Expected: PASS.

### Task 5: Exact next eligible outbox wake time

**Files:**
- Modify: `tests/ir_submission_service_tests.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/ir/IrSubmissionService.cpp`

**Interfaces:**
- Consumes: enabled submission-capable providers in `IrActiveProfileConfig`.
- Produces: `ReplayRepository::NextIrOutboxAttemptAfter(std::string_view providerId, std::int64_t nowMs)` returning the provider's SQL `MIN(next_attempt_at_ms)` for ready Pending/Awaiting rows strictly after `nowMs`.

- [ ] **Step 1: Write a failing starvation regression test**

Fill the first 64 globally ordered rows with skipped-provider work, schedule a later `fake` provider row in the future, and capture the worker's wait deadline. Assert the deadline matches that future row instead of being absent.

- [ ] **Step 2: Run the service test to verify RED**

Run: `cmake --build cmake-build-debug --target ir_submission_service_tests -j 6 && ctest --test-dir cmake-build-debug -R '^ir_submission_service_tests$' --output-on-failure`

Expected: FAIL because the bounded global scan cannot see past 64 skipped rows.

- [ ] **Step 3: Query the exact per-provider minimum**

Add a parameterized SQLite aggregate over `local_result_ready=1`, `state IN (0,2)`, matching `provider_id`, and `next_attempt_at_ms>?`. Replace the bounded scan with one aggregate call per enabled submission-capable provider and take the minimum returned time.

- [ ] **Step 4: Run the service test to verify GREEN**

Run the command from Step 2.

Expected: PASS.

### Task 6: Final verification

**Files:**
- Verify all modified files.

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces: a review-ready local branch with no unresolved actionable behavior regressions.

- [ ] **Step 1: Build the desktop target**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: successful build.

- [ ] **Step 2: Run the full test suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 6`

Expected: 105/105 tests pass (or the updated total if a new test target is added).

- [ ] **Step 3: Inspect scope and secret safety**

Run: `git diff --check && git status --short && git diff --stat`

Expected: no whitespace errors, only planned files changed, and no credential material appears in generated artifacts or source fixtures beyond obvious non-secret sentinels.
