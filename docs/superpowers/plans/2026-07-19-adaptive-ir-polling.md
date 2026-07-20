# Adaptive IR Polling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Poll accepted Bokutachi imports after 200 ms and progressively back off to 10 seconds without reposting or losing the polling stage across crashes.

**Architecture:** Add a polling-specific counter to each durable IR outbox row and advance it only after the server reports `ongoing`. `IrSubmissionService` maps that counter to the fixed 200 ms, 1 s, 2 s, 3 s, 5 s, and 10 s cadence; retry and transport-failure behavior remain separate.

**Tech Stack:** C++23, SQLite, CMake/CTest

## Global Constraints

- The first remote-status poll is due 200 ms after HTTP 202.
- Ongoing polls wait 1, 2, 3, 5, and then at most 10 seconds.
- Manual Retry polls immediately without reposting or resetting the poll stage.
- Transport failures continue using the existing transient-failure backoff.
- API keys and authorization values must never be persisted.

---

### Task 1: Persist the Remote Poll Count

**Files:**
- Modify: `src/ir/IrOutboxModels.h`
- Modify: `src/ir/IrOutboxModels.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Test: `tests/replay_repository_tests.cpp`

**Interfaces:**
- Produces: `IrOutboxEntry::remotePollCount` and `IrOutboxDeliveryUpdate::remotePollCount`, both non-negative `int` values.
- Produces: replay schema version 8 with `remote_poll_count INTEGER NOT NULL DEFAULT 0`.
- Consumes: existing `ApplyIrOutboxDelivery`, retry, claim, recovery, and row-decoding paths.

- [ ] **Step 1: Write failing repository and migration tests**

Extend the IR outbox schema assertions to require `remote_poll_count`. Add an
outbox delivery test which applies `remotePollCount = 2`, reloads the row, and
expects 2. Add a version-7 migration fixture using the exact old table SQL,
open it through `ReplayRepository`, and expect schema version 8 plus a preserved
row whose poll count is zero. Extend retry coverage to confirm an awaiting
row's count is unchanged.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6 && ./cmake-build-debug/replay_repository_tests
```

Expected: compilation or assertions fail because `remotePollCount`, schema
version 8, and `remote_poll_count` do not exist.

- [ ] **Step 3: Implement the durable field and migration**

Add `int remotePollCount = 0` to both outbox models and reject negative values.
Add the column to the canonical table SQL and row column/decode lists. Bind it
in `ApplyIrOutboxDelivery`; preserve it in awaiting retry/recovery paths and
reset it when a row starts a newly accepted remote job or leaves remote-awaiting
state. Increment `ReplayRepository::kCurrentSchemaVersion` to 8 and migrate an
exact version-7 schema using `ALTER TABLE ir_outbox ADD COLUMN
remote_poll_count INTEGER NOT NULL DEFAULT 0`, followed by canonical schema
inspection.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the command from Step 2. Expected: `replay_repository_tests` exits 0.

- [ ] **Step 5: Commit the persistence change**

```bash
git add src/ir/IrOutboxModels.h src/ir/IrOutboxModels.cpp \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositorySchema.cpp \
  src/repositories/ReplayRepositoryIrOutbox.cpp \
  tests/replay_repository_tests.cpp
git commit -m "feat: persist IR remote poll stage"
```

### Task 2: Apply the Adaptive Poll Schedule

**Files:**
- Modify: `src/ir/IrSubmissionService.cpp`
- Test: `tests/ir_submission_service_tests.cpp`

**Interfaces:**
- Consumes: `IrOutboxEntry::remotePollCount` and `IrOutboxDeliveryUpdate::remotePollCount` from Task 1.
- Produces: `remotePollDelay(int pollCount) -> std::int64_t`, mapping counts 0–5+ to 200, 1000, 2000, 3000, 5000, and 10000 ms.

- [ ] **Step 1: Write failing service tests**

Replace the 10-second initial-poll expectation with 200 ms. Drive one deferred
response followed by enough ongoing responses to assert persisted due times of
1, 2, 3, 5, 10, and 10 seconds and poll counts 1 through 6. Add a fixture with
earlier transient POST failures and assert its eventual 202 still schedules the
first poll after 200 ms with count zero. Retain the existing assertion that
manual retry performs a poll and never a second POST.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target ir_submission_service_tests -j 6 && ./cmake-build-debug/ir_submission_service_tests
```

Expected: delay assertions fail because the service still schedules every poll
after 10 seconds.

- [ ] **Step 3: Implement the schedule**

Replace `kPollDelayMs` with:

```cpp
std::int64_t remotePollDelay(int pollCount) {
  constexpr std::array<std::int64_t, 6> delays{200, 1'000, 2'000,
                                               3'000, 5'000, 10'000};
  return delays[std::min(static_cast<std::size_t>(std::max(0, pollCount)),
                         delays.size() - 1)];
}
```

For `Deferred`, store poll count zero and schedule `remotePollDelay(0)`. For
`Ongoing`, increment the claimed row's count and schedule the delay represented
by the incremented count. Preserve the current count for transient failures and
configuration blocking so those paths do not advance the adaptive cadence.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the command from Step 2. Expected: `ir_submission_service_tests` exits 0.

- [ ] **Step 5: Commit the scheduling change**

```bash
git add src/ir/IrSubmissionService.cpp tests/ir_submission_service_tests.cpp
git commit -m "fix: poll queued IR imports adaptively"
```

### Task 3: Verify and Publish

**Files:**
- Verify only; no expected source changes.

**Interfaces:**
- Consumes: completed Tasks 1 and 2.
- Produces: a tested branch pushed to its configured upstream.

- [ ] **Step 1: Run all configured tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2: Run source and desktop build checks**

```bash
git diff --check
cmake --build cmake-build-debug --target main -j 6
```

Expected: no whitespace errors and `main` builds successfully.

- [ ] **Step 3: Inspect and push the branch**

```bash
git status --short
git log --oneline --decorate -4
git push
```

Expected: the worktree is clean and the configured remote branch advances to
the adaptive-polling commits.
