# Find BMS Download Folder Selection

**Date:** July 21, 2026  
**Status:** Approved for implementation

## Objective

Let users choose which writable BMS Library folder receives Find BMS
downloads. Make the first eligible manually added folder the default selection,
promote another eligible folder when the selection is removed, and retain
`Documents/BMS` as the fallback when no eligible manual folder exists.

Restore live downloaded-size and total-size text in the Find BMS modal while an
archive is downloading.

## Verified Current Behavior

`MainMenuScene::preferredBmsDownloadRoot()` currently derives the destination
from the first effective chart-library entry on iOS and desktop. Android always
uses `ChartRepository::DefaultBmsFolderPath()`. There is no explicit or
persistent download-folder selection in the chart repository or Settings UI.

The chart repository stores folder entries in SQLite with their normalized path
and iOS bookmark. Android may also expose document-tree entries represented by
an `@androidtree@` virtual path. Find BMS commits downloaded files with ordinary
filesystem directory operations, so those virtual document-tree entries are
not valid download destinations.

iOS and Android can expose `Documents/BMS` as a built-in or bootstrapped
library entry. It must remain the fallback and must not displace a manually
added eligible folder as the selected download destination.

The Find BMS progress callback still carries `downloadedBytes` and
`totalBytes`. The modal stores both values and the existing formatter can
already render human-readable byte counts. The simplified modal stopped
displaying those counts even though progress propagation remains intact.

## Folder Eligibility

An eligible manual download folder is a stored chart-library entry that:

- is not `ChartRepository::DefaultBmsFolderPath()`;
- is not an Android document-tree virtual path; and
- otherwise represents a normal filesystem folder supported by the existing
  platform-specific folder access flow.

The Settings UI shows Android document-tree entries as library sources but
disables their download-folder action with the note `Not writable by Find BMS`.
The repository must never persist one of those entries as the active download
folder.

The default `Documents/BMS` path remains available as an implicit fallback. It
is not an explicit manual selection, even if an older database contains it in
the `entries` table.

## Persistence and Selection Rules

Add selection state to chart-library entry persistence instead of duplicating
the selected path in general application settings. This keeps the selection
paired with the entry's normalized path and iOS bookmark.

The repository owns these invariants:

1. At most one eligible manual entry is the selected download folder.
2. When eligible manual entries exist but none is selected, the oldest eligible
   entry becomes selected.
3. The first eligible manual folder added to an empty manual-entry set becomes
   selected automatically.
4. Explicitly selecting another eligible folder clears the old selection and
   selects the requested entry in one transaction.
5. Removing the selected entry promotes the oldest remaining eligible manual
   entry in the same transaction.
6. If no eligible manual entries remain, no entry is selected and the effective
   download root is `Documents/BMS`.
7. Updating an existing entry's bookmark or metadata preserves its selection
   and insertion order.

For databases created before this feature, schema migration adds the selection
state without losing entries. The first repository read that resolves the
effective download entry normalizes missing or invalid selection state using
the rules above. Existing installations with multiple entries therefore choose
their oldest eligible manual entry deterministically.

## Repository Interface

`ChartEntry` exposes whether it is the selected Find BMS download folder and
whether it is eligible for selection. The chart repository provides operations
to:

- list entries with their selection state;
- set an eligible entry as the download folder; and
- resolve the selected eligible entry, if one exists.

Selection and reassignment belong in repository transactions so callers cannot
observe two selected entries or a deleted selection without its replacement.
Stored entry queries use stable insertion order for deterministic default and
promotion behavior.

The existing folder insertion flow must use an upsert that preserves selection
and insertion order. This matters because the native folder-picker flow can
insert the same entry immediately and again when its refresh task begins.

## Settings UI

In **Settings → BMS Library → Chart Folders**, each entry row communicates its
download status:

- the selected eligible row displays `Download folder`;
- an unselected eligible row has a `Use for Downloads` button;
- an Android document-tree row displays `Not writable by Find BMS`; and
- the built-in default path identifies itself as the fallback used when no
  manual download folder is available.

Selecting a row persists immediately, refreshes the folder rows, and reports a
success or repository error through the existing chart-folder status message.
No library rescan is required because changing the download destination does
not change chart metadata.

Deleting the active row uses the repository's automatic promotion rule. When
the Settings list refreshes, the promoted row is marked as the download folder,
or the default path is shown as the fallback when none remains.

## Find BMS Destination Resolution

Before automatic or candidate downloads begin,
`MainMenuScene::preferredBmsDownloadRoot()` asks the repository for the selected
eligible entry.

- On iOS, the selected entry is resolved through the existing security-scoped
  bookmark flow before use.
- On Android and desktop, a normal selected filesystem path is used directly.
- If the repository has no eligible manual entries, the app creates and uses
  `ChartRepository::DefaultBmsFolderPath()` without converting that fallback
  into a manual selection.

If a selected folder later becomes inaccessible or unwritable, Find BMS reports
the actual filesystem or folder-access error. It does not silently redirect the
download to `Documents/BMS`, because doing so would violate the user's explicit
selection. The fallback applies only when there are no eligible manual folder
entries.

Both automatic Find BMS lookup downloads and explicit candidate downloads use
the same resolved destination.

## Download Size Progress

While `findBmsProgressMessage` is `Downloading archive`, the modal's status line
shows the existing progress message, percentage, and human-readable sizes:

`Downloading archive — 42% (18.6 MB / 44.3 MB)`

When the server does not provide a total size but downloaded bytes are known,
the status line shows the current size only. Non-download stages retain their
existing text. The feature reuses `downloadedBytes`, `totalBytes`, and the
existing `formatFindBmsBytes()` formatter; it does not restore the removed
scrolling progress log.

The status view must remain readable at the existing modal width without
clipping the current/total display. Layout adjustments should be limited to the
status area and must preserve the current modal actions and non-dismissible
hash-mismatch decision state.

## Failure Handling

- Selecting an unknown, fallback, or ineligible entry fails without changing
  the previous selection and shows a specific Settings status message.
- Repository transaction failures preserve the previous valid selection.
- Removing a selected folder fails atomically if its replacement cannot be
  persisted.
- Failure to create the fallback `Documents/BMS` folder is surfaced as the
  existing Find BMS destination-folder error.
- A selected iOS bookmark resolution failure or write failure is surfaced in
  the Find BMS modal and does not trigger fallback redirection.

## Testing

Repository tests cover:

- schema migration from entries without selection state;
- automatic selection of the first eligible manual entry;
- preservation of selection across entry upserts;
- explicit selection and rejection of unknown or ineligible entries;
- deterministic promotion after deleting the selected entry;
- no selection and `Documents/BMS` fallback when no eligible manual entry
  exists; and
- normalization of invalid or duplicate stored selection state.

Settings and flow audit tests cover the canonical labels, button wiring,
selected-state presentation, ineligible Android presentation, and use of the
repository-selected entry by both Find BMS download paths.

Progress presentation tests cover known-total formatting, unknown-total
formatting, and preservation of non-download stage text. Verification includes
the focused repository and Find BMS tests, the desktop `main` build, the full
CTest suite, and the repository's iOS build-only command. No build is uploaded
or installed.

## Scope

This change affects chart-library entry persistence, the BMS Library Settings
rows, Find BMS destination resolution, and Find BMS download progress text.

It does not add write support for Android document-tree paths, change general
archive import destinations, restore the removed Find BMS scrolling log, or
silently relocate downloads when an explicitly selected folder is
inaccessible.
