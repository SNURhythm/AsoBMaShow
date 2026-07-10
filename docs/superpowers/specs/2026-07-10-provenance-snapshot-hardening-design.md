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

### Raw clean-header fast path plus explicit no-lock read-only VFS

Selected. A clean database with no sidecars is inspected as raw bytes: validate the SQLite header and read the big-endian `user_version` at offset 60. If WAL/SHM state exists, open the original twice with `SQLITE_OPEN_READONLY` through SQLite's explicit no-lock VFS (`unix-none`, `win32-none`, or `win32-longpath-none`). In the bundled SQLite implementation those VFSes disable database locks and expose no shared-memory mapping method, so WAL reconstruction stays in heap memory and cannot create or rewrite `-shm`, checkpoint the main file, or delete sidecars. Both reads must return the same version, and raw family presence/size/write-time state must remain stable around them. A rollback journal is conservatively ambiguous and fails closed without SQLite open. Any changing, malformed, unavailable-VFS, or unreadable state also fails closed.

This keeps the clean-connect path bounded to a 100-byte header read and WAL preflight bounded to two read-only SQLite queries without copying the potentially large database. Ordinary `mode=ro` and URI `nolock=1` are not used: the former changed `-shm` in a production probe and the latter did not open WAL reliably. `immutable=1` is also excluded because it ignores WAL and returned the stale main-header version.

## Components

### Shared schema preflight

`SqliteRAII.h` will expose a small header-only preflight used by both score and replay helpers. It accepts the database path and maximum supported `user_version`, and returns one of: new/supported, unsupported future, or unreadable/ambiguous with a diagnostic.

For a missing or zero-byte file, version 0 is supported so the normal open may create/initialize it. For a clean existing file, the preflight validates the SQLite magic, page-size encoding, read/write format bytes, minimum header size, and signed big-endian user version. A present rollback journal is rejected as ambiguous. WAL/SHM state is read twice through an available explicit no-lock VFS, with matching versions and unchanged raw family metadata required before approval.

`ScoreDBHelper::Connect()` and `ReplayDBHelper::Connect()` run preflight before `openSqliteDatabase()`. Future or ambiguous databases return `nullptr`; supported/new databases then take the existing read-write open and pragma path. Public methods that receive a caller-owned `sqlite3*` retain their current in-connection future-version guard.

### Coherent replay summary snapshot

After schema initialization, `ListReplays()` and `ListCourseReplays()` start an explicit deferred read transaction. The first candidate step pins a WAL snapshot; candidate JSON/index validation, linked-stage validation, bounded keyset chunks, and detail/count hydration all use that same snapshot. Success commits the read transaction; every early return rolls it back through the existing RAII handle.

The scan budget, 64-row keyset chunks, aggregate diagnostics, and deferred event/touch counts remain unchanged. A concurrent writer may commit invalid values, but the in-flight list either returns the previously validated coherent snapshot or rejects the value on a later call; it never mixes validation from one snapshot with details from another.

### Loadable course summaries

Linked-stage validation returns false for zero rows, a missing replay, malformed/future/index-mismatched provenance, more than 256 stages, or a SQLite step error. Limited and unlimited course summaries apply the same rule. Full load already rejects empty/missing/invalid stages, so the summary and load contracts become consistent.

## Testing

- Raw snapshots read bytes and sidecar presence without opening the original.
- Forked child fixtures use the repository SQLite build and `_exit()` to leave committed future WAL state and a hot rollback journal. WAL state is read without mutation; rollback-journal state is rejected as ambiguous without SQLite open.
- Score and replay tests cover direct `Connect`, high-level chart/course saves, and caller-owned direct insert/schema entry points without original-family mutation.
- A deterministic POSIX two-process WAL fixture detects the list reader's WAL lock, commits an indexed-version mismatch while a large candidate scan is active, and proves chart/course summaries return only the original coherent snapshot or omit the record.
- Course tests cover zero-stage, missing-stage, malformed JSON, future ruleset, and indexed mismatch in limited and unlimited scans while preserving bounded-log behavior.

## Failure Semantics

- Missing database: allow normal creation as schema version 0.
- Clean valid supported database: open normally after raw-header approval.
- Future database, including a version present only in WAL or recoverable journal state: return `nullptr` without changing original bytes or sidecar presence.
- Unreadable, malformed, racing, or ambiguous preflight: log once and return `nullptr`.
- Summary transaction start/commit failure: log and return no summaries; RAII releases any active transaction.
