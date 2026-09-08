# Lazy recursive-folder selection

## Goal

Opening a large physical folder must not synchronously construct every song bar,
probe every replay file, or retain every complete chart record. Reuse the built-in
MainMenuScene bounded page-cache strategy without changing its behavior.

## Evidence and scope

MainMenuScene counts rows and requests 128-record pages, retaining six pages.
MusicSelectScene currently queries every recursive record, projects descendants
that may not be displayed, probes four replay slots per song, and copies/sorts
complete bars. These are code-path findings, not measured latency results.
Physical folders are the scope; tables, courses and search containers retain
their existing eager representation. No parser, shader, deployment or skin
validation behavior changes are involved.

## Data flow

1. A correct own-folder-or-immediate-child-folder existence probe distinguishes
   song/mixed folders from category folders. Category views create only immediate
   child folder bars from persisted metadata, preserving empty folders.
2. For song/mixed folders, a cancellable worker streams narrow chart selection
   fields in the existing raw query order. It builds a compact identity/sort index,
   not a vector of full chart records or song bars. This deliberately retains
   O(N) compact metadata so exact selector comparisons and arbitrary jumps do
   not require a second, subtly different SQL implementation of score semantics.
3. Globally deduplicate SHA256 identities using the existing representative and
   reversal rules. Resolve mode/difficulty fallback and hidden-chart handling,
   then apply existing stable sorting before selecting page boundaries.
4. Fetch complete records by the selected page's paths. Construct score/replay
   presentation only for these records. A shared cache retains at most six
   128-row pages. Page references must not outlive their cache ownership.
5. The bar manager and skin frame expose total count and indexed access rather
   than requiring a contiguous vector. Navigation, pointer selection, percentage
   jumps, wraparound, selected properties and preview use global indices.

## Compatibility

Preserve direct charts and recursive flattening in mixed folders, physical
SHA256 deduplication (including its existing empty-hash behavior), source-order
ties, all selector sort IDs, note-profile difficulty filters, cyclic filter
fallback, hidden-chart rules, score long-note fallback and missing-score order.
Folder statistics continue to count raw charts independently of displayed-row
deduplication. Existing asynchronous folder-stat loading remains separate.
Explicit directory autoplay may enumerate the directory to create its playlist;
normal opening/rendering must not do so.

## Ownership and failure handling

Worker queries use a worker-owned repository session and cancellation token.
Directory identity, library revision, score revision and request generation
guard installation. Navigating away, reloading or tearing down cancels pending
work; stale completion must never reopen a folder. Failures leave navigation
usable and permit retry. Opening progress must not block the input/render loop.
Page-cache misses remain bounded reads as in MainMenuScene; benchmark deep jumps
using path-batch reads rather than increasing SQL OFFSET over rich rows.

## Validation

Use production-code tests for cache eviction/error retry, query projection and
order parity, direct/category/mixed folders, duplicate hashes across page
boundaries, every filter/sort, navigation and frame access with 100,000 logical
rows, cancellation and stale completions. Assert bounded complete-record and
replay work rather than relying solely on flaky timing thresholds. Record
before/after timings and memory for a reproducible large synthetic library.
Build desktop main and test targets, then run CTest in parallel. Preserve
surrounding formatting and leave unrelated review documents untouched.

## Measurements

Local M1 Pro, 16 GiB RAM, desktop debug build; synthetic data, not a device
latency guarantee. The selector benchmark exercises production projection,
ordering, bar installation and replay-file existence checks, with generated
records replacing database reads. Run each mode in a separate process:

```sh
/usr/bin/time -l cmake-build-debug/music_select_paged_songs_tests --benchmark eager 100000
/usr/bin/time -l cmake-build-debug/music_select_paged_songs_tests --benchmark paged 100000
```

| 100,000-chart flat folder | Previous eager path | Paged path |
| --- | ---: | ---: |
| Total, including index/projection and cleanup | 4,279 ms | 1,477 ms |
| Maximum resident set | 1,392,640,000 bytes | 103,825,408 bytes |
| Complete records created for initial view | 100,000 | 160 |
| Replay-file existence checks | 400,000 | 640 |
| Bar installation and ordering | 1,936 ms | 0.048 ms |

The paged index takes 1,469 ms in this debug benchmark and runs on the directory
worker. The initial two pages contain 128 + 32 rows because the last page is
partial. Steady-state complete records remain bounded by the six-page cache.

A separate actual-SQLite 100,000-row test over six warm runs measured 582 ms for
the narrow visitor versus 1,101 ms for the rich query. It selects 19 rather than
44 columns and avoids an approximately 87 MiB rich-record vector. Both queries
still use a temporary ordering B-tree; the optimization does not claim to remove
global query/sort work. Indexed own/immediate-child probes measured approximately
0.28 ms; large legacy empty-folder populations require additional path checks.
