# Find BMS Review Follow-ups Design

## Goal

Finish the incremental Find BMS workflow by reconciling database records for
package variants removed during commit and by completing preview handoff when
the newly indexed chart is hidden by the current list query.

## Constraints

- Do not run a full library scan after a Find BMS download.
- Keep downloaded-path work serialized with full scans on the existing library
  task worker.
- Reconcile only package paths that the storage commit actually removed.
- Do not clear or otherwise change the user's active folder, search text, chart
  filters, or sort state for a Find BMS preview handoff.
- Keep the existing selection-generation and durable-hash eligibility checks.
- Keep title-only mismatches from guessing a preview target.

## Removed Variant Reconciliation

`commitFindBmsPendingArtifact` will optionally report a vector of removed root
paths. The commit will append a root only when that alternate or stale variant
existed and `remove_all` completed without an error. Transaction directories,
staging directories, and replaced content at the current destination are not
part of this manifest.

`BmsSearchResult` will carry the manifest through both successful automatic
download commits and the manual Keep Files resolution. The Find BMS UI will
copy it into the downloaded-path library task.

Before the additions-only scan, the serialized library worker will call
`ChartRepository::Session::DeleteChartMetaInDirectory` for every removed root.
That existing targeted operation removes chart rows beneath ordinary folders
or archive virtual paths together with matching solid-archive and archive-cache
records. A negative result fails the task; zero is a successful no-op. The new
output path is then indexed as before.

Because reconciliation and indexing remain in the same FIFO library task, a
currently running full scan finishes first. The downloaded task then removes
obsolete records and inserts the committed output without concurrent database
mutation or a second full scan.

If disk cleanup is partial or fails, the commit does not report that root, so
the database does not discard records for files that may still exist. This
preserves the current best-effort cleanup behavior while making successful
removals visible to the library layer.

## Filter-independent Preview Handoff

`selectChartByPathAfterReload` will first retain its current visible-query
selection behavior. If the Find BMS `Load` handoff cannot find the target in
the recycler query, it will load that exact path through
`SelectChartMetaByPaths`, which is independent of folder, table, search, and
record filters.

A small pure helper will accept the path lookup outcome only when storage
loaded successfully and it contains the exact requested record. For that
record, the scene will unselect any visible row, set the recycler selection to
no visible index, and invoke the existing `onSelected` callback with the
hydrated record. That callback remains the single owner of selected-record
state, Ready-panel controls, artwork, and preview loading. The active query and
all filter state remain unchanged.

The off-list fallback applies only to `AutoSelectionPreview::Load`. Unzip
selection with preview suppression retains its existing All Songs fallback and
visible-row behavior.

## Error Handling

- A failed removed-root database reconciliation marks the downloaded indexing
  task Failed and still requests a library reload.
- An empty or unsuccessful exact-path lookup does not fabricate a selection;
  the existing fallback behavior remains available.
- Selection generation and identity are checked before the unfiltered lookup,
  so a user selection change during download or parsing still cancels the
  handoff.

## Tests

- Download staging tests will prove that archive/extracted alternates and
  renamed same-identity variants are reported only when successfully removed.
- Workflow and pending-resolution tests will prove the removal manifest reaches
  `BmsSearchResult` for automatic and manual commits.
- Main-menu policy tests will prove exact-path lookup accepts only a successful
  exact record and rejects missing, mismatched, or storage-failure outcomes.
- The Find BMS flow audit will require removed paths to enter the serialized
  task, targeted reconciliation to precede additions-only scanning, and the
  unfiltered lookup to occur without clearing search or filters.
- Focused binaries, the desktop target, and the complete CTest suite will run
  before publishing.
