# Database-backed selector paging

## Correction and goal

The previous paging change bounded rich records but built a complete C++ song
index before displaying a folder. MainMenuScene instead counts and requests
bounded SQL pages. Normal recursive-folder opening must use the latter model:
no streaming every chart into C++, per-chart score lookups, identity index, or
full-vector sort before the first page.

## Design

Resolve selector mode/difficulty fallback and count in SQLite, then fetch only
the requested page in final display order. Keep the cancellable directory
worker, existing row-provider interface, and six 128-row cache pages. SQL must
choose the first raw title/path-ordered representative for each exact SHA256
before filtering, including the existing empty-hash representative. Stable
display ties follow reversed raw order. Preserve hidden/all-hidden fallback,
note-profile difficulties, metadata sorts, and immutable score-cache semantics.
Score sorting may require SQLite-wide work; it must not construct chart vectors.
Any comparator behavior that cannot be safely represented in SQL needs an
explicitly tested compatibility decision, not a silent ordering change.

Folder category handling, explicit autoplay, stale request rejection and page
error retry remain unchanged. SQLite errors and cancellation propagate; a
failed query must never publish a successful empty folder. Provider clones own
their configuration and pages. Selection restoration uses a database identity
lookup rather than an eagerly built identity map.

Add order-compatible indexes where query plans demonstrate their value. Do not
materialize a full temporary selector table on every folder entry or merely
prewarm the previous C++ index elsewhere.

## Validation

Differential tests compare paged SQL output with the existing eager/indexed
reference across duplicates, nested paths, every filter and sort, scores,
missing values, cancellation and selection restoration. Instrument actual
production queries to assert bounded decoded records and absence of the narrow
whole-folder visitor. Benchmark count, first/wrapped pages and total first-view
preparation against MainMenuScene and the previous index using the same real
SQLite synthetic library. Report first-page timing including SQL, cold/warm
conditions, and any remaining global database work without a millisecond claim
unsupported by measurements. Build desktop main/all tests and unsigned iOS;
no deployment, parser changes, new worktrees or whole-file formatting.

## Implemented compatibility decisions

The old DURATION comparator narrows an average-judgement difference to a signed
32-bit integer. When the selected score range exceeds INT32_MAX, its ordering
cannot be expressed as a scalar SQL key without changing legacy behavior.
Only that detected DURATION case retains the old narrow song index; ordinary
metadata sorts and non-overflowing score sorts remain database-backed. This
exception is tested both on initial opening and configuration changes.

Resolved SQL queries retain a database/session revision marker, not a read
transaction across UI frames. Pages and identity lookups reject stale revisions
and request reopening rather than silently mixing differently ordered results.
The cache still holds at most six 128-row pages per provider. Opening primes
the first and wrapped last pages, so at most 256 rich records are decoded.

Counting distinct hashes still scans matching database entries. Broad TITLE
pages use a matching expression index and fetch the tail in reverse to avoid
walking the entire preceding offset. Narrow folders retain the folder-index
plan. Other sorts and identity ranking can still require global SQLite work;
bounded C++ decoding is not a claim of constant-time database execution.
