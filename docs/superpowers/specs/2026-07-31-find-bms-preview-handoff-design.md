# Find BMS Preview Handoff Design

**Date:** July 31, 2026
**Status:** Self-reviewed and approved for implementation

## Objective

When Find BMS finishes downloading and the serialized library worker finishes
parsing/indexing the committed output, replace the still-selected unavailable
chart with its newly available local chart. Use the ordinary selection path so
preview loading begins and the Ready panel refreshes. If the user selected a
different chart at any point after the download started, leave their current
selection, preview, and Ready panel untouched.

## Existing Behavior

Find BMS now commits an archive or extracted directory and enqueues an
`IndexDownloadedPath` task. The task shares the FIFO library worker with full
scans and calls `ScanAdded()` for the exact output, then requests a library UI
reload. The reload preserves the unavailable record but does not select the
newly indexed path, so the normal preview callback never runs and the Ready
panel continues to describe the unavailable record.

The existing unzip flow has a separate post-reload path selection mechanism,
but that mechanism deliberately suppresses preview loading. Find BMS needs the
opposite behavior and an additional stale-selection guard.

## Considered Approaches

### Selection generation plus conditional normal selection (selected)

Capture the chart-selection generation when an actual Find BMS download
starts. Carry it, the target hashes, and the committed output path through the
queued indexing task. After scanning, resolve the indexed target with an
indexed hash query restricted to that exact output. Publish a mutex-protected
UI handoff. After the requested list reload, apply the handoff only when the
generation is unchanged and the current record is still the original target.

Selecting the resolved path through `RecyclerView::onSelected` loads artwork,
starts the normal debounced preview parser, refreshes action state, and updates
the Ready panel through the existing play-option refresh chain. This keeps one
owner for selection UI behavior.

### Update preview and Ready widgets directly

The worker could post separate preview and widget updates. This would duplicate
selection logic, couple the worker result to UI details, and risk showing a
mixture of old and new chart state. It would also require maintaining another
path for every future Ready-panel field.

### Compare only the current chart identity at completion

Checking only hashes when indexing finishes would miss an away-and-back race:
the user could select another chart, return to the original chart, and then
have the old download unexpectedly take over. A monotonic generation records
that intervening choice.

## Selection Generation

`MainMenuScene` owns a UI-thread `chartSelectionGeneration`. The chart selection
callback increments it only when the selected record identity changes; invoking
the callback again for the same record does not manufacture a user change.

An actual automatic or candidate download captures the current generation.
The capture is not replaced when resolving a Keep/Delete mismatch decision,
because a selection change during the preceding download still invalidates the
handoff. Completed mismatched-file Keep operations remain indexable but do not
request an automatic preview: they did not validate as the selected chart.

The final UI predicate requires both:

- the captured and current generations are equal; and
- the current record still matches the original Find BMS target identity.

Hash identity is fail-closed: canonical SHA-256 is authoritative when present,
otherwise canonical MD5 is used. A target without a durable hash may still be
downloaded and indexed, but it is not auto-selected because a title-only match
could choose the wrong chart from a package.

## Indexed Target Resolution

Add a repository query that returns chart metadata matching one normalized
SHA-256, or MD5 only when SHA-256 is unavailable. Existing indexes on
`chart_meta.sha256` and `chart_meta.md5` make this proportional to matching
duplicates rather than the full library.

After `ScanAdded()` completes, filter those matches to paths lexically inside
the exact `downloadedPath`. This works for both extracted directories and the
repository's archive virtual paths (`archive.zip/inner/chart.bms`). If several
matching copies exist inside the same output, choose the stable lexical first
path. If no exact hashed match is found, request the normal library reload but
publish no selection handoff.

## Threading and Full-Scan Race

The selection request remains part of `LibraryTaskRequest`, so a full scan that
is already running or paused completes before the downloaded-path scan and
target lookup. The scanner transaction commits before the handoff is
published. A mutex protects the worker-to-UI pending handoff; existing atomic
reload flags provide the wake-up signal.

The UI consumes the handoff only after reloading the chart projection. The
generation check and `onSelected` call execute in the same UI turn, so user
input cannot interleave between validation and selection. Multiple completions
may replace a pending handoff; only the latest can still match the current
selection generation.

## Post-Reload Selection

Generalize `selectChartByPathAfterReload()` with an explicit preview policy.
The unzip caller keeps `Suppress`; Find BMS uses `Load`. Both may fall back to
All Songs when the current query does not expose the resolved chart. The Find
BMS path invokes the existing selection callback without setting
`suppressPreviewForChartPath`.

## Testing

Focused policy tests cover unchanged, changed, and away-and-back generations;
SHA-256 authority and MD5 fallback; missing hashes; exact directory and archive
path scoping; and deterministic duplicate selection. Repository tests cover
the indexed normalized-hash query. The Find BMS flow audit verifies capture,
task propagation, post-scan publication, and preview-enabled selection.

Verification also includes the existing scanner and Find BMS download tests,
the desktop `main` build, and the full CTest suite. No mobile build is deployed.

## Scope

This change only adds the conditional post-index selection handoff. It does not
change download matching, archive commit semantics, scan ordering, manual
refresh/rebuild behavior, preview parsing, Ready-panel layout, or unzip preview
suppression.
