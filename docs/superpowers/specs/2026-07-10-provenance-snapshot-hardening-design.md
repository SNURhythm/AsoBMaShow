# Provenance Snapshot Hardening Design

**Date:** 2026-07-10
**Branch:** `feature/foundation-provenance`
**Reviewed base:** `4fa37944e038b0d19898b741a4b324ca6dd9e9c7`

## Purpose

Make every score and replay schema open fail closed without changing an unsupported or ambiguous SQLite family. Validation must include WAL-visible state, remain bounded, retain ownership until the production connection is ready, and reject every unreadable or negative schema version. Replay summaries and full course hydration must also share one loadability contract and one SQLite snapshot.

IR, server work, unrelated database helpers, and BMS parser behavior remain out of scope.

## Selected Architecture

### Bounded isolated validation

`SqliteRAII.h` captures the main, rollback-journal, WAL, and SHM family state before validation. Missing and zero-byte databases are accepted as version zero only when no sidecar exists. A rollback journal is conservatively rejected because recovery would be required.

Every existing database is queried through a private temporary family. A clean database is copied completely so bundled SQLite can verify both `PRAGMA user_version` and `SELECT count(*) FROM sqlite_schema`. A WAL database uses a sparse main file containing the original first page plus the complete measured WAL; bundled SQLite then resolves the WAL-visible version in isolation. This also handles a committed WAL whose SHM file is absent without opening or rebuilding the original family.

Main bytes, WAL bytes, and a 64 KiB auxiliary reserve must total at most 512 MiB. Overflow-safe arithmetic rejects larger families before allocation or copy. Copy and comparison helpers process exactly the measured byte count, reject a short read or one extra byte, and never scan beyond the budget. The original first page, sidecar bytes, presence, sizes, and write times are checked around the isolated query.

### Guarded owned open pair

Isolated approval is followed by one guarded ownership operation:

1. Open a guard connection with checkpoint-on-close disabled and zero busy timeout.
2. Enter exclusive locking mode and acquire `BEGIN EXCLUSIVE`.
3. Reread `user_version`, require `0 <= version <= maximum`, and require it to equal the isolated snapshot version.
4. Roll back the read transaction while retaining exclusive connection ownership, then require `journal_mode=WAL` to succeed.
5. Open a second, still-lazy production handle and configure checkpoint-on-close, optional foreign keys, and its busy timeout through SQLite C APIs.
6. Close the guard only after the production handle is ready, then return the production handle.

The second handle avoids an unlocked validation-to-production-open gap while allowing the exclusive guard to be closed before normal use. Any open, lock, version, journal-mode, or configuration error closes both handles and returns null. Exact-family tests cover supported and future WAL-without-SHM states, an active writer, a writer committing immediately after isolated approval, and injected configuration failures.

### Error-aware schema guards

The shared in-connection version reader returns `std::optional<int>` plus a diagnostic. Caller-owned score and replay schema entry points accept only a successful nonnegative version at or below the supported maximum. When a caller transaction is active, the check uses that same transaction snapshot; query denial, malformed results, and negative versions are rejected before any schema delta. Autocommit entry points retain isolated validation.

### Coherent replay reads

`ListReplays()` and `ListCourseReplays()` hold an explicit deferred read transaction across candidate validation and detail/count hydration. The existing 64-row chunks, `limit + 512` candidate budget, aggregate diagnostics, and bounded scans remain unchanged.

Course summaries and full loads use the same ordered stage descriptor query. It requires 1..256 exact contiguous stage indexes, a present positive linked replay id, a matchable path/MD5/SHA-256 identity, valid provenance, and a terminal `SQLITE_DONE`. Full course hydration keeps the aggregate, descriptors, and every replay/event/touch/lane-cover read on one connection and one deferred snapshot. It appends only fully loaded ordered stages and returns null on any structural, query, or hydration error.

## Alternatives Considered

Opening the original read-only was rejected because ordinary read-only WAL access can create or change SHM, while `immutable=1` intentionally ignores WAL. A normal-locking writer probe was rejected for the same SHM mutation. Manual WAL and rollback-journal parsing was rejected because recovery, salts, commit frames, and rolling checksums would duplicate SQLite correctness logic.

Closing a validation connection and later opening production was also rejected because a writer can commit a future schema in that gap. The exclusive guard plus pre-opened lazy production handle retains the required ownership boundary without exposing the returned connection to exclusive-locking behavior.

## Verification Coverage

- Supported and future child-exit WAL fixtures, with and without SHM, preserve exact original bytes and sidecar presence.
- A deterministic fork seam commits version 99 after isolated approval and proves guarded open rejects the exact post-writer family.
- Header-shaped corrupt clean files and denied required pragmas return null without mutation.
- Pure boundary predicates run on every platform; a POSIX sparse oversized fixture proves rejection before snapshot copying.
- Negative and authorizer-denied version reads inside caller transactions leave schema and version state unchanged.
- Malformed course fixtures cover empty identity, partial missing links, gaps, duplicates, mixed negative indexes, zero stages, and 257 stages for limited/unlimited summaries and full load.
- An SQLite trace callback interrupts the stage query after its first row and proves all three public paths fail closed on terminal step error.
- Concurrent WAL writers prove replay summary validation/detail hydration and full course hydration do not mix snapshots.
- Focused and full verification are registered and run through CTest.

## Failure Semantics

- Missing or empty database without sidecars: allow normal creation as version zero.
- Supported usable database: return a configured production connection only after guarded revalidation.
- Future, negative, malformed, racing, oversized, recovery-dependent, or query/configuration-error state: log and return null.
- Caller-owned transaction version error: return false before schema mutation and leave rollback ownership with the caller.
- Summary or full-load transaction/query/structure error: return no summaries or no course; RAII releases the active read transaction.
