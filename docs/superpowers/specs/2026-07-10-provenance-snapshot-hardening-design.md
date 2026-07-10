# Provenance Snapshot Hardening Design

**Date:** 2026-07-10  
**Branch:** `feature/foundation-provenance`  
**Reviewed base:** `4fa37944e038b0d19898b741a4b324ca6dd9e9c7`

## Purpose

Close the three remaining fail-closed boundaries from the final provenance review: determine a WAL-visible schema version without opening or normalizing the original database, keep replay summary validation and hydration on one coherent SQLite snapshot, and never advertise a course aggregate that cannot load a linked stage.

The parent task explicitly approves this scope and asks for autonomous implementation while the user is away. IR, server work, unrelated database helpers, and parser behavior remain out of scope.

## Alternatives Considered

### Open the original read-only

Rejected. A local production-SQLite probe showed `mode=ro` reads the WAL-visible version but changes bytes in `-shm`. Adding `immutable=1` preserves the files but deliberately bypasses WAL discovery and returns the stale main-file version. Neither satisfies both correctness and immutability.

### Parse SQLite WAL and rollback-journal formats manually

Rejected. Reading `user_version` from the main header is simple, but finding the last committed page-1 WAL frame requires salt and rolling-checksum validation, and rollback-journal recovery adds another file format. Reimplementing recovery would be a fragile security boundary tied to SQLite internals.

### Raw clean-header fast path plus isolated WAL recovery snapshot

Selected. A database without a WAL or rollback journal is inspected as raw bytes: validate the SQLite header and read the big-endian `user_version` at offset 60. If a WAL exists, build a private temporary family from the first main-database page plus the complete WAL and let the bundled SQLite recover/query that isolated copy. Raw original-family bytes and metadata must remain stable around the copy query. A rollback journal is conservatively ambiguous and fails closed without SQLite open. Any changing, malformed, negative-version, copy, or query failure also fails closed.

The implementation copies the first SQLite page, extends the temporary main file sparsely to the original logical size, and copies the WAL in full. Only page 1 is required to answer `PRAGMA user_version`; WAL frames supply a newer page 1 when present. POSIX sparse allocation is verified by a multi-page physical-block test. Because `std::filesystem::resize_file()` does not guarantee sparse allocation on Windows, the non-sparse fallback fails closed above 256 MiB instead of risking an unbounded temporary allocation. The original WAL is compared byte-for-byte with the snapshot both before and after the temporary query, the original first page is reread, and every family member's presence/size/write-time is rechecked.

This keeps the common clean-connect path bounded to two 100-byte header reads and pays full-copy cost only for the exceptional WAL. Ordinary `mode=ro` changed `-shm`; `immutable=1` ignored WAL; and bundled SQLite 3.43.1 explicitly rejects WAL when its pager uses the no-lock VFS. Read-only exclusive locking also failed because the read-only descriptor cannot take SQLite's write-style exclusive lock. Isolated recovery is therefore the smallest cross-platform mechanism that delegates WAL correctness to the bundled SQLite without touching the original.

An ordinary SQLite close may leave a derived `-shm` file after the WAL itself is gone, so SHM alone is not treated as authoritative recovery state. In that case the raw main-header path is used and the entire family still must remain unchanged. Rollback journals remain ambiguous and are rejected. After a WAL-visible version is proven supported, a zero-timeout `BEGIN IMMEDIATE` probe on the original, configured with `SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE`, rejects an active writer before the normal connection path proceeds. Future databases never reach this read-write probe.

## Components

### Shared schema preflight

`SqliteRAII.h` will expose a small header-only preflight used by both score and replay helpers. It accepts the database path and maximum supported `user_version`, and returns one of: new/supported, unsupported future, or unreadable/ambiguous with a diagnostic.

For a missing or zero-byte file, version 0 is supported so the normal open may create/initialize it. For a clean existing file, the preflight validates the SQLite magic, page-size encoding, read/write format bytes, minimum header size, and signed big-endian user version. A present rollback journal is rejected as ambiguous. WAL state is copied to a private temporary sibling family, recovered/queried there, and approved only if the original WAL bytes, page 1, and family metadata remain unchanged and no active writer exists.

`ScoreDBHelper::Connect()` and `ReplayDBHelper::Connect()` run preflight before `openSqliteDatabase()`. Future or ambiguous databases return `nullptr`; supported/new databases then take the existing read-write open and pragma path. Public methods that receive a caller-owned `sqlite3*` retain their current in-connection future-version guard.

### Coherent replay summary snapshot

After schema initialization, `ListReplays()` and `ListCourseReplays()` start an explicit deferred read transaction. The first candidate step pins a WAL snapshot; candidate JSON/index validation, linked-stage validation, bounded keyset chunks, and detail/count hydration all use that same snapshot. Success commits the read transaction; every early return rolls it back through the existing RAII handle.

The scan budget, 64-row keyset chunks, aggregate diagnostics, and deferred event/touch counts remain unchanged. A concurrent writer may commit invalid values, but the in-flight list either returns the previously validated coherent snapshot or rejects the value on a later call; it never mixes validation from one snapshot with details from another.

### Loadable course summaries

Linked-stage validation returns false for zero rows, a missing replay, malformed/future/index-mismatched provenance, more than 256 stages, or a SQLite step error. Limited and unlimited course summaries apply the same rule. Full load already rejects empty/missing/invalid stages, so the summary and load contracts become consistent.

## Testing

- Raw snapshots read bytes and sidecar presence without opening the original.
- Forked child fixtures use the repository SQLite build and `_exit()` to leave committed future WAL state and a hot rollback journal. WAL state is copied/recovered away from the original; rollback-journal state is rejected as ambiguous without SQLite open.
- Score and replay tests cover direct `Connect`, high-level chart/course saves, and caller-owned direct insert/schema entry points without original-family mutation.
- A deterministic POSIX two-process WAL fixture detects the list reader's WAL lock, commits an indexed-version mismatch while a large candidate scan is active, and proves chart/course summaries return only the original coherent snapshot or omit the record.
- Course tests cover zero-stage, missing-stage, malformed JSON, future ruleset, and indexed mismatch in limited and unlimited scans while preserving bounded-log behavior.

## Failure Semantics

- Missing database: allow normal creation as schema version 0.
- Clean valid supported database: open normally after raw-header approval.
- Future database, including a version present only in WAL, or any rollback-journal state: return `nullptr` without changing original bytes or sidecar presence.
- Unreadable, malformed, racing, or ambiguous preflight: log once and return `nullptr`.
- Summary transaction start/commit failure: log and return no summaries; RAII releases any active transaction.
