# Provenance Owned-Open Review Remediation Design

**Date:** 2026-07-10

**Base:** `41e586f47a95bc78846762840e52ba95a84cd0a1`

## Purpose

Close every Critical/Important and requested bounded-I/O/documentation finding in `provenance-task-2-snapshot-review.md` without weakening exact-family preservation for unsupported databases. The remediation must retain coherent concurrent-WAL summary reads and normal returned-connection concurrency.

## Validated root causes

- A supported `-wal` without `-shm` reaches the current read-write writer probe, which creates the missing SHM before returning failure.
- The probe rolls back and closes before `Connect()` opens a different handle, so a writer can commit a future version in that unlocked interval.
- Transactional guards use an integer version helper that maps prepare/step failure to zero and accepts negative values.
- The clean raw-header check establishes only header shape, not that SQLite can traverse the schema b-tree, and `Connect()` returns handles after required pragma failures.
- Summary validation and full course hydration use different queries and structural rules; neither requires a matchable stage identity in both paths.
- Main/WAL copy and comparison I/O is not bounded as one total operation.

The review claims are technically sound for this codebase. A bundled-SQLite probe additionally established that `PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE` with `SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE` reads both supported and future WAL-visible versions without changing main, WAL, or SHM bytes, including WAL-without-SHM. The normal-locking equivalent changes SHM and is not suitable. A second lazy SQLite handle can be opened while the exclusive guard is held; once the guard closes, the second handle permits a different process to begin a writer transaction normally.

## Alternatives

### Reject every WAL without SHM and retain the old probe

This fixes one mutation but not the approval/open race. Repeating isolated validation after a normal open still leaves another unlocked check/use interval.

### Return the exclusive validation connection

This makes validation and return atomic, but a WAL connection that entered exclusive locking mode retains that lock until it leaves WAL mode or closes. It blocks the concurrent writer used by the summary snapshot contract and would reduce application concurrency.

### Exclusive guard plus pre-opened normal production handle

Selected. Isolated validation first establishes that the stable family is supported. A guard connection then disables checkpoint-on-close, enters exclusive locking mode, begins an exclusive transaction, and rereads `user_version` with error-aware range validation. This catches a writer that committed after isolated validation without modifying the raced future family. While the guard still owns the database, it establishes WAL journal mode and opens/configures a second lazy production handle. Closing the guard releases ownership only after the production handle already exists. The returned handle never performs a mutating pragma in an unowned interval.

Required production-handle options use non-I/O APIs while the guard is held: busy timeout, `SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE`, and replay foreign-key enablement. `synchronous=NORMAL` is not transferred between handles and is removed rather than executed after ownership release. SQLite's default synchronous setting is correctness-safe.

## Snapshot validation and bounded I/O

All nonempty existing databases use an isolated SQLite family for usability validation. Both clean and WAL databases copy the bounded main file in full so `SELECT count(*) FROM sqlite_schema` can traverse the real schema b-tree; WAL databases additionally copy the complete measured WAL. The temporary connection reads `user_version`, traverses `sqlite_schema` through terminal `SQLITE_DONE`, and never checkpoints on close.

One overflow-safe 512 MiB budget covers logical main bytes, WAL bytes, and a fixed temporary-SHM reserve on every platform. Copy and compare helpers read exactly the previously measured size and reject an extra byte, so a racing source cannot cause unbounded copy/verification I/O. Files beyond the budget fail closed before allocation or copying.

Original main and WAL bytes are compared to their isolated copies before and after the query, and full family presence/size/write-time state is rechecked. Rollback journals remain rejected. WAL-without-SHM is accepted only through the proven exclusive guard; supported, future, active-writer, and injected guard-configuration error paths all get exact-family tests.

## Error-aware version ownership

A shared `readSqliteUserVersion(sqlite3 *, std::string &)` returns `optional<int>` and treats prepare failure, non-row step, negative values, and versions above the caller maximum as rejection. Score/replay migration helpers propagate the optional failure instead of treating it as zero.

Autocommit caller-owned handles retain non-mutating isolated validation so stable future WAL families are rejected before a page query. Caller-owned transactions must use the error-aware in-connection read because uncommitted state is not visible to a copy. Their existing snapshot prevents a writer from changing the version between that read and later writes; negative or authorizer-denied PRAGMA reads fail before any schema delta.

Every required connection setup failure closes both guard and production handles and returns null. Persistent journal setup occurs only after supported ownership is revalidated.

## Shared course-stage loadability contract

One stage reader/query supplies both summary validation and full hydration. It returns ordered descriptors containing index, replay id, rest duration, chart identity fields, and provenance fields from a `LEFT JOIN`.

The reader requires:

- exactly 1 through 256 rows;
- indexes exactly `0..count-1`, rejecting negative, duplicate, and gapped values;
- a present linked replay for every stage;
- at least one nonempty trimmed SHA-256, MD5, or stored path identity that the replay match predicate can bind;
- coherent stage provenance when requested;
- `SQLITE_DONE` after the last row.

`ListCourseReplays()` calls this reader inside its existing coherent read transaction. `LoadCourseReplay()` starts its own read transaction, reads the aggregate, uses the same descriptor reader, and hydrates each replay through a connection-local replay loader on that same connection/snapshot. It commits only after every stage and child collection loads successfully. It never silently skips or default-fills malformed stages.

Public tests cover empty identity, one missing row among valid rows, `0,2`, duplicate indexes, negative plus valid, 257 stages, and an injected terminal step error for limit-one, unlimited, and full load. SQLite auto-extension tracing supplies the deterministic step interruption without adding a production-only test hook.

## Deterministic ownership seam

The shared validated-open helper accepts optional function-pointer hooks used only by tests. An after-isolated-validation hook lets a child commit and `_exit` with version 99 before the exclusive guard opens; the helper must reject it and preserve the post-writer family exactly. A guard-configuration hook injects `SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE` failure before original page access. Production callers pass no hooks.

## Success criteria

- All review reproductions fail before production changes and pass afterward.
- Unsupported/future families are never changed by helper code.
- Supported WAL-without-SHM can be opened through the proven guard, while injected/active-writer failures preserve it exactly.
- No required pragma/configuration error returns a connection.
- Summary and full course load agree for every malformed structure and use one snapshot.
- Focused stress, requested build, full 834-test CTest inventory, formatting, diff, and project-file lint pass.
