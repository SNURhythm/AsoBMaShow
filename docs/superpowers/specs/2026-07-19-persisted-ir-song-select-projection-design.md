# Persisted IR Song Select Projection Design

## Goal

Project synchronized Bokutachi scores into Song Select without a parallel
in-memory chart-list implementation. Imported scores must reuse the normal
score persistence and SQLite summary path, remain distinguishable from local
gameplay, and be replaceable from the durable IR mirror.

## Root cause and cleanup boundary

Commit `142a685` made clear-lamp and score-dependent chart queries load every
candidate chart, merge IR evidence in memory, then filter and sort the full
vector. Later caching reduced repeated work but retained the first-selection
stall and introduced a second chart-list ownership mode.

Remove the runtime `IrScoreHistoryProjection` overlay, its imported MD5 cache
maps, the owned/referenced chart-list modes, and the IR-specific refresh
revisions. Preserve `ir_remote_scores`, reconciliation receipts, Records
merging, and remote result recall.

## Persisted projection

Extend `scores` with source metadata:

- `score_source`: local gameplay or imported IR;
- `source_provider_id`, `source_server_origin`, and
  `source_remote_score_id`: stable remote identity;
- `source_sync_generation`: the durable mirror generation that produced the
  row.

Local gameplay writes retain their current defaults. An imported snapshot is
validated and transactionally replaces only rows for its provider and server
origin. Each imported score is converted to the same `ChartScoreWrite` used by
gameplay and inserted through the same low-level score insert/binding path.
Imported rows use legacy-unverified provenance and a null attempt ID, so they
can contribute to best-score and clear-lamp summaries but cannot be submitted
as local attempts.

The remote mirror remains authoritative for detailed Records and recalled
result scenes. Imported `scores` rows are a query projection and may use zero
storage placeholders for unavailable detailed metrics. Readers identify the
source and do not expose those placeholders as local result detail.

## Lamp and long-note semantics

Bokutachi supplies an authoritative lamp but no AsoBMaShow long-note mode.
Imported rows therefore use `ln_mode = -1`. The score summary trigger expands
that wildcard into the four playable long-note-mode summary keys, leaving the
existing indexed chart queries unchanged.

The effective clear-rank expression must never infer FULL COMBO from an
imported row's placeholder `combo_break`. It returns the remote `clear_type`
unchanged for imported rows; local gameplay retains the existing inference.

## Synchronization lifecycle

After the replay repository commits a reconciliation snapshot, the submission
service invokes a projection callback with the same validated scores and
generation. A projection failure makes reconciliation report failure while
leaving the durable remote mirror intact for retry.

At profile/service activation, the application compares the durable mirror
state with the imported rows. It loads and replaces the full snapshot only
when generation or count differs. Empty mirrors clear stale imported rows.
Credential or provider-identity invalidation clears imported rows as well as
replay-side account evidence.

## Song Select behavior

Song Select returns to the pre-`142a685` path for every folder and filter:

1. `CountChartMeta(query)` obtains the indexed count.
2. `ChartListPageCache` lazily requests 128-row SQL pages.
3. `FindChartMetaIndex(query, path)` restores selection by path.

No IR-specific full chart metadata snapshot, filter, sort, or page-reference
mode remains.

## Failure behavior

- Invalid remote identities or snapshots do not modify imported score rows.
- Storage failure rolls back the whole imported-row replacement and summary
  rebuild.
- A committed remote mirror survives projection failure and is retried from
  local storage; no additional server request is required at startup.
- Imported rows are scoped by provider and normalized origin, preventing
  credential or custom-server evidence from leaking between accounts.

## Verification

Repository tests cover schema migration, source identity, wildcard summary
fan-out, authoritative lamps, idempotence, replacement/deletion, rollback,
and local gameplay behavior. Service tests cover the post-reconciliation
callback and failure reporting. Main-menu regression checks prove the
IR-specific full-list path is absent and the original paginated SQL path is
used. The full CTest suite and desktop build are required before pushing.
