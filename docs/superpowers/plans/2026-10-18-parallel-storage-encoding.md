# Parallel Storage Encoding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Let later encoding targets progress while a large video is running.

**Architecture:** A shared admission budget bounds audio/video work and scratch
space. Directory owners serialize evidence and commits; archive owners stage a
bounded lookahead window and consume encoded results in source order.

**Tech Stack:** C++23, standard threads/futures, existing SQLite archive journals.

**Spec:** `docs/superpowers/specs/2026-10-18-parallel-storage-encoding.md`

## Global Constraints

- At most two video encoders; each video codec retains two threads.
- Retain the existing platform audio-worker defaults.
- Join encoders before deleting staging or releasing maintenance.
- Serialize directory evidence and archive mutation; preserve recovery safety.
- Bound candidate windows and reserve scratch for all pending outputs.
- Work in the current checkout; no worktrees, deployment or user-library writes.

## Review Focus

- Launch failure after scratch admission must release capacity (Task 1).
- A slow video must not prevent later same-directory commits (Task 2).
- External edits while encoding must still reject stale commits (Task 2).
- Cancellation with speculative archive media must join before cleanup (Task 3).
- A crash must recognize owned speculative staging without accepting unknown files (Task 3).

### Task 1: Nonblocking shared admission and encoder lifetimes

**Files:** `src/storage/StorageTypes.h`, `StorageWorkScheduler.{h,cpp}`,
`tests/storage_work_scheduler_tests.cpp` (all source paths under `src/storage/`).

**Interfaces:** Add `StorageWorkBudget::TryAcquire(MediaKind, uint64_t, bool &waiting)`
returning an optional lease, and `Lease::ReleaseEncoder()` to retain scratch after
codec completion. `Lease::GrowTo(uint64_t)` admits growing output against the same
shared scratch reservations. Keep blocking `Acquire` compatible. Add a small move-only
asynchronous encoding job owner with readiness, result and join semantics.

- [x] Add budget tests for two video slots, audio independence, retained scratch,
  cancellation and exception/launch cleanup; run them and record expected failures.
- [x] Implement nonblocking admission and RAII encoder lifetime; run scheduler tests.

### Task 2: Concurrent encoding within a directory

**Files:** `src/storage/StorageOptimizer.cpp`, `tests/storage_optimizer_tests.cpp`.

**Interfaces:** Directory work consumes the shared budget and submits a bounded
window of candidates. Each worker performs hashing/encoding outside the owner
mutex and validates/commits under it. Reports remain per-directory.

- [x] Add a blocked-first-video test asserting later audio and video encode and
  commit before release, plus cancellation and stale-input coverage; record RED.
- [x] Implement bounded dispatch, unique outputs, locked evidence/commit/report
  sections and deterministic draining; run optimizer and scheduler tests.

### Task 3: Archive lookahead and shared owner scheduling

**Files:** `src/storage/StorageArchiveTransaction.{h,cpp}`,
`src/storage/StorageOptimizer.cpp`, `src/storage/StorageWorkScheduler.{h,cpp}`,
`tests/storage_archive_tests.cpp`, `tests/storage_work_scheduler_tests.cpp`.

**Interfaces:** Archive dependencies optionally borrow the shared work budget.
The archive owner extracts unique staged inputs, dispatches encoding futures and
consumes them in source order. Extend owner scheduling with archive summaries;
aggregate archive reports through the existing synchronized progress handoff.

- [x] Add blocked-video archive overlap, cancellation/staging cleanup, scratch
  bounds and interrupted-update recovery tests; record RED.
- [x] Implement bounded archive lookahead with recognized owned staging names,
  and schedule independent archives with directory owners.
- [x] Run archive, optimizer, scheduler and file-transaction checks.

### Task 4: Verification and review

**Files:** `docs/storage-management.md`, implementation and tests above.

- [x] Update scheduling documentation and run `git diff --check`.
- [x] Build `main tests_library tests_audio tests_archives` together at `-j 6`;
  run CTest with `-L '^area_(library|audio|archives)$' -j 6 --output-on-failure`.
- [x] Obtain an independent review of the full change, address actionable findings,
  then commit and push verified task changes to the current upstream.

Validation: desktop application build and all 87 affected library/audio/archive
tests passed. Independent review identified delayed recovery-stop publication
and unaccounted directory output growth; both were fixed with failing-then-passing
regressions before the final test run.
