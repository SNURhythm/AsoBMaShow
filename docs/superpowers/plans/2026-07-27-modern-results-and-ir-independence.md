# Modern Results and IR Independence Implementation Plan

> Delivery Slice 3 of the approved contract-first file replay restart. Execute
> inline with TDD. The design and this plan are pre-approved when they remain
> consistent with the umbrella design.

## Constraints

- Do not change the SQLite schema or activate BRD persistence in this slice.
- Do not derive modern result or IR facts from replay events.
- Preserve current `ReplayData` call sites behind explicit legacy adapters.
- Modern result and IR fingerprints contain no replay path, hash, state, or
  bytes.
- Every production step starts with a focused failing test.
- Commit each task separately, then review the whole slice against
  `origin/develop` and fix duplicated authorities at their shared boundary.

## Task 1: Shared Durable Primitives

**Files:**

- Create `src/CanonicalDigest.h`.
- Create `src/DurablePayloadLimits.h`.
- Modify `src/replay/ReplayFormat.h` and `src/replay/ReplayLimits.h` to consume
  the root authorities without changing replay behavior.
- Create `tests/durable_payload_contract_tests.cpp` and register it in CMake.

**TDD:** Pin lowercase digest syntax, the 16 KiB durable string bound, the
1,000,000-sample result history bound, and the shared 256-stage course bound.
First prove the root APIs are missing, then implement and refactor replay to
consume them.

**Commit:** `refactor: share durable payload primitives`

## Task 2: Replay-Free Modern Result Contracts

**Files:**

- Create `src/ModernResult.h` and `src/ModernResult.cpp`.
- Refactor `src/ResultPersistenceModel.h/.cpp` to reuse shared score/timing
  facts while retaining `ChartResultAttempt` and its replay-inclusive legacy
  fingerprint.
- Create `tests/modern_result_tests.cpp` and register it in CMake.

**TDD:** Define valid chart and partial-course fixtures. Prove raw input,
touch, lane-cover, BRD metadata, and file-state changes cannot affect a modern
result fingerprint. Pin full field coverage, bit-exact floats, strict
validation, overflow-safe score equations, course prefix/order/aggregate
rules, and the shared result-fact agreement API. Prove legacy fingerprints are
not accepted as modern fingerprints.

**Commit:** `feat: add replay-free modern result contracts`

## Task 3: Completion Capture and Legacy Adapter

**Files:**

- Modify `src/ModernResult.h/.cpp`.
- Modify `src/ResultPersistenceModel.h/.cpp` only for compatibility adapters.
- Extend `tests/modern_result_tests.cpp` and the existing result persistence
  tests.

**TDD:** Capture a chart result directly from parsed metadata, `RhythmState`,
provenance, adopted gauge, attempt UUID, and completion time. Capture course
stage facts without replay. Prove capture output passes the same reader
validator and that the temporary legacy attempt can project a modern result
only from independently captured score/state facts, never from raw events.

**Commit:** `feat: capture compact modern result facts`

## Task 4: Canonical Postponed-IR Snapshots

**Files:**

- Create `src/ir/IrSubmissionSnapshot.h/.cpp`.
- Modify `src/ir/IrSubmission.h/.cpp` to accept `ModernChartResult` while
  preserving the old overload as an adapter.
- Update `src/ir/CMakeLists.txt`, `src/CMakeLists.txt`, and root CMake targets.
- Create `tests/ir_submission_snapshot_tests.cpp`.

**TDD:** Pin capture from a validated modern result, exact schema-1 canonical
JSON, fingerprint round trip, size bounds, exact key sets, unknown version,
extra/missing fields, stale/tampered fingerprints, invalid provenance,
non-finite gauges, timing disagreement, and replay mutation independence.

**Commit:** `feat: add canonical postponed IR snapshots`

## Task 5: Replay-Free Modern Result Recall

**Files:**

- Extend `src/ResultRecallBuilder.h/.cpp` with modern chart/course overloads
  and result-only output types while retaining legacy overloads.
- Create `tests/modern_result_recall_tests.cpp` and register it in CMake.

**TDD:** Prove chart recall uses saved facts without replay, checks parsed
identity before applying stored display metadata, constructs gauge and
judgement presentation state, and is unchanged across present/missing/corrupt
replay states. Prove course recall uses ordered saved stages and full entry
facts, rejects mismatches atomically, and publishes no partial session.

**Commit:** `feat: recall modern results without replay files`

## Task 6: Boundary and Slice Gate

**Files:**

- Extend `tests/replay_contract_boundary_tests.cpp`.
- Update `docs/replay/file-replay-contract-matrix.md` with the activated modern
  result and postponed-IR rows.

**TDD:** Source-audit `ModernResult` and `IrSubmissionSnapshot` for replay
playback, replay-file, repository, SQLite, outbox, and receipt tokens. Assert
result/snapshot types have no raw collection or file metadata fields. Ensure
legacy adapters are explicitly named and no modern reader calls replay result
materialization.

Run all focused result, IR, recall, provenance, and boundary tests; full CTest;
and desktop `main`. Review the complete slice diff against `origin/develop`,
searching for duplicate identity, result arithmetic, fingerprint, provenance,
course-limit, and result-agreement authorities. Add a regression test and fix
every verified finding at its shared boundary.

**Commit:** `test: enforce modern result independence boundaries`

## Exit Criteria

- Modern chart/course results validate and fingerprint without replay data.
- Postponed IR snapshots capture, serialize, and read without replay data.
- Modern result recall never opens a replay file.
- Existing runtime paths remain buildable through named temporary adapters.
- Focused tests, full CTest, and desktop `main` pass.
- Whole-slice review finds no unresolved P1/P2 contract violation.
