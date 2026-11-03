# Lightweight IR candidate summaries

Goal: open IR using indexed scalar metadata after a one-time bounded backfill,
without parsing historical result/snapshot JSON on every visit. Keep existing
canonical payloads and fingerprints unchanged, and authenticate selected facts
again immediately before manual upload.

## Boundaries

- Add a distinct lightweight candidate summary type in `IrUploadCandidates.h`.
  UI, selection controller and candidate loader consume summaries. The existing
  full candidate/projection APIs retain their validation contracts.
- A derived, versioned SQLite cache stores verified result/snapshot eligibility
  per provider and the few display option strings held only in provenance JSON.
  Other display fields already have scalar columns on modern_chart_results.
- Install the cache and invalidation triggers when opening a prepared replay
  database. Any change/deletion/replacement of a result or snapshot invalidates
  the corresponding summaries. The cache is not an upload authority.
- Populate Tachi summaries while staging new results. Backfill missing entries
  in bounded keyset pages on the existing background loader. Cache invalid
  source pairs too, avoiding repeated expensive validation of corrupt history.
- Read live scoped receipt/outbox metadata for current upload state. Provider
  eligibility policy changes must invalidate/version the derived cache.
- Load full candidate facts by result ID and attempt ID for each selected upload,
  in a consistent transaction. Reject changed, missing, ineligible, inconsistent
  or already queued/submitted results. Never enqueue from cached display data.

## Implementation and verification

1. Extend repository behavioral coverage for persisted backfill, reopen reuse,
   trigger invalidation, source deletion/replacement, provider/origin scope and
   live outbox/receipt updates. Instrument warm reads to prove JSON payloads are
   not fetched or decoded.
2. Implement the summary cache/schema hooks and page/single-candidate APIs in
   the replay repository, retaining the authenticated JSON APIs.
3. Switch list/controller/task to the summary type and rehydrate selected rows
   during preparation. Extend extracted scene tests for the revalidation boundary.
4. Benchmark copied private fixture data only; keep fixture contents outside git.
   Run focused repository, IR projection/preparation/UI and schema tests plus
   desktop main build. Review the diff, commit, push and update PR #177.

## Completed validation

- IR/replay groups: 76 tests passed; desktop `main` build passed.
- A SQLite authorizer verifies that warm summary reads, including after reopen,
  do not read historical JSON. Tests cover source corruption, negative-cache
  reuse, deletion, provider/origin scope, live queued/failed/receipt state,
  selected identity mismatch and upload rejection after source changes.
- Independent review found an unbounded first-page query plan. The display
  query now restricts joins to the IDs already selected for that page. A
  regression reuses the existing 16,384-row synthetic history and measures
  SQLite VM work: 27 blocks of 100 instructions for the first 32-row page,
  below a 200-block bound. The previous query failed at 10,499 blocks.
- Copied private profile fixture, local macOS desktop debug build, 32-row
  pages: full authenticated scan averaged 638.4 ms; cached summary scan
  averaged 2.12 ms (five runs each). The one-time backfill took 599.5 ms.
  Measurements were repeated after builds/tests finished. Both paths returned
  identical candidate IDs (8 candidates, no omissions); no private contents
  are included here. These measure repository scans, not device UI latency.
- Display summaries are deliberately not upload authority. Live scalar
  delivery metadata can affect list visibility; full selected result/snapshot
  and delivery-state validation still gates every upload.
