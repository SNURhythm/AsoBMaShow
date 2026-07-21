# Find BMS Non-Solid Archive Preservation

**Date:** 2026-07-20

**Status:** Approved design

**Implementation branch:** `feature/skip-unzip`

## Context

Find BMS currently downloads every archive into a temporary `_archives`
location, unarchives it into a generated `BMSSEARCH` folder, validates that
the result contains the requested chart when a hash is available, and deletes
the downloaded archive. The chart library can already browse non-solid
archives efficiently through `ArchiveFile`, so unconditional unarchiving uses
extra time and storage for users who prefer to retain the original package.

Hash mismatches need an explicit retention decision. A non-solid download may
still be useful as an archive, and a conventionally unarchived download may
contain other useful charts. The user must be able to keep or delete either
form without a modal dismissal silently choosing for them.

## Goals

- Add one persistent, opt-in Find BMS setting named exactly
  `Skip unarchiving for non-solid archives`.
- Keep the setting off by default so existing downloads continue to
  unarchive.
- When enabled, retain a downloaded archive only when the existing archive
  reader confirms that it is readable and non-solid.
- Preserve the existing requested-chart hash validation before reporting a
  successful download.
- Fall back to the current unarchiving path for solid archives and archives
  that cannot be classified or validated reliably through direct reads.
- Require the user to keep or delete downloaded files after a confirmed hash
  mismatch, for both retained archives and unarchived folders.
- Keep cleanup scoped to files produced by the current Find BMS attempt.

## Non-goals

- Preserving solid archives without unarchiving them.
- Changing which websites or package sources Find BMS searches.
- Letting filename extensions decide whether an archive is solid.
- Changing general library scanning, archive preview, or archive import
  behavior outside Find BMS.
- Adding a persistent preference for how hash mismatches are resolved.
- Recovering an interrupted mismatch prompt after the application process is
  terminated. Normal in-process dismissal must still be impossible.

## Chosen approach

Add a persisted boolean setting and pass an immutable snapshot of it through
both automatic Find BMS downloads and explicit candidate downloads. After the
download is confirmed not to be an HTML response, inspect it with
`archive_file::listEntries`. The archive is eligible to remain packed only
when listing succeeds, it contains at least one file, and no file entry is
marked solid.

For an eligible archive, validate chart presence and the requested hash by
reading BMS entries through the existing archive reader. A conclusive match
keeps the archive without unarchiving. A conclusive mismatch enters the
mandatory retention-decision state with the archive as the staged artifact.
If direct inspection or reading cannot produce a reliable answer, unarchive
and validate the resulting files instead of treating the uncertainty as a
hash mismatch.

This decision at the download boundary is preferred to extension heuristics,
which cannot identify solid RAR or 7-Zip archives, and to deferring the
decision to the library refresh, which would delay validation and make
fallback unarchiving difficult to report in the active Find BMS operation.

## Settings model and UI

Add `findBmsSkipUnarchivingForNonSolidArchives` to `AppSettings` with a
default value of `false`. Serialize and load it with the existing profile
settings JSON. Older settings documents that omit the field retain the false
default; no schema-version increment or explicit migration is required.

Add the setting to Settings -> Misc alongside the existing archive controls.
Its visible name is exactly:

`Skip unarchiving for non-solid archives`

The control shows a direct `On` or `Off` state and persists changes through
the existing settings save flow. The explanatory copy states that the option
applies only to Find BMS downloads and that solid archives are still
unarchived.

When a Find BMS worker starts, `MainMenuScene` captures the current boolean
value and passes it with the request. The worker does not read mutable scene
settings from its background thread.

## Attempt staging and commit

Each Find BMS download uses a UUID-named attempt directory beneath
`std::filesystem::temp_directory_path() / "AsoBMaShowFindBms"`. This staging
root is outside the configured chart-library roots. The staged download and
any staged unarchived folder remain private to that attempt until validation
succeeds or the user explicitly keeps mismatched files. This prevents a
library refresh from discovering undecided files and makes deletion safe when
a generated destination already contains files from an earlier download.

For a successful eligible non-solid archive, promote the staged archive to
`BMSSEARCH/_archives/<preferred archive name>`, using the existing preferred
name and overwrite policy. Promotion writes a sibling temporary file and
replaces the destination only after the copy completes, so a failed commit
leaves both the previous destination and the staged source recoverable. Set
the result output path to the final archive and refresh the library so its
virtual chart paths are indexed.

For a successful unarchived result, transactionally commit the staged
extracted files to the existing `BMSSEARCH/<storage key>` destination with the
current merge-and-overwrite semantics, then delete the staged source archive.
When that destination already exists, build a sibling commit directory from
the previous destination plus the staged overlay, swap it into place with a
recoverable backup, and restore the previous destination if the swap fails.
Staging must not change the visible destination layout of ordinary Find BMS
downloads.

All cancellation, download, HTML-response, classification, validation, and
unarchiving failures clean the current attempt's staging location unless the
result is a confirmed hash mismatch awaiting the user's decision. If the
process terminates, an abandoned attempt remains only in the operating
system's temporary directory and is left to normal operating-system cleanup;
the application does not recover or commit it on its next launch.

## Direct archive validation

Build the direct-validation candidate list from regular archive entries whose
paths are recognized BMS chart files. Read those entries through
`ArchiveFile`; do not duplicate archive-format parsing in `bms_search`.

When the request supplies a 64-character SHA-256 or 32-character MD5 key,
calculate the corresponding hash from each readable BMS entry using the same
hash implementation as extracted-file validation:

1. Return a match as soon as one BMS entry has the requested hash.
2. Return a conclusive mismatch only after every candidate BMS entry has been
   read and none matches.
3. Treat a failure to list the archive, an empty file list, a failed candidate
   read, or an otherwise incomplete scan as inconclusive and fall back to
   unarchiving.

When no valid hash key is available, the direct path mirrors current behavior:
it reports whether a BMS filename was found but does not invent a mismatch.
The result may still succeed with the existing no-BMS warning.

## Hash-mismatch retention decision

Extend `BmsSearchResult` with explicit pending-artifact metadata sufficient to
distinguish a staged archive from a staged unarchived folder and to identify
its intended library destination. Pending metadata exists only when the
status is `HashMismatch` and the staged artifact is available for a retention
decision.

For a mismatch on the skip-unarchiving path, the pending artifact is the
downloaded archive. For a mismatch after conventional unarchiving, the
pending artifact is the staged extracted folder; the downloaded source
archive is not independently offered and is removed.

While a pending mismatch exists, the Find BMS dialog presents only these
decisions:

- `Keep Files`: commit the staged archive or extracted files to the normal
  destination, clear the pending state, start a library refresh, and then
  allow the dialog to close.
- `Delete Files`: remove only the current attempt's staged artifact, clear the
  pending state, and then allow the dialog to close without refreshing the
  library.

Close, Cancel, Escape, Back, overlay clicks, scene dismissal, and other normal
UI routes must not dismiss the dialog while the decision remains pending. If
keep or deletion fails, show the error in the dialog, retain the pending
state, and allow the same action to be retried. An already-missing staged
artifact counts as successful deletion.

The retention operation uses a focused service helper rather than blocking
the download worker while it waits for UI input. This keeps the worker
lifecycle simple and makes keep/delete behavior testable without scene event
loops.

## Progress and result messages

The direct path reports an archive-inspection/validation phase instead of an
unarchiving phase. Successful messages distinguish `Downloaded BMS archive.`
from `Downloaded and unarchived BMS archive.` The existing warning form is
retained when no BMS file is found.

A confirmed mismatch explains that the downloaded files did not contain the
selected chart and that the user must keep or delete them. Resolving the
decision adds an explicit kept or deleted outcome before the normal Close
action becomes available.

## Failure handling and safety

- A solid entry always disables the skip path for the entire archive.
- Inconclusive archive-reader results fall back to unarchiving; they are not
  reported as mismatches.
- A confirmed mismatch does not automatically try another source because the
  user must first resolve ownership of the current files.
- Keep and delete operations validate that their source is the exact staged
  attempt artifact and that the destination is the derived Find BMS location.
- Recursive deletion is permitted only for the unique staging folder. It must
  never target the library root, `BMSSEARCH` root, `_archives` root, or an
  existing final extracted folder.
- Transactional promotion may overwrite the same destination files as the
  current implementation, but a failed promotion restores the previous
  destination and leaves the staged source available for retry.
- Deleting a rejected attempt cannot remove or alter pre-existing destination
  files.

## Verification

Automated coverage will verify:

- the setting defaults to false, survives JSON round trips, and uses false
  when loading an older document with no field;
- the exact UI label and persisted On/Off behavior;
- request propagation through automatic and candidate download paths;
- eligibility for readable non-solid entries and fallback for any solid,
  empty, or unreadable archive result;
- direct SHA-256 and MD5 matches, confirmed mismatches, and fallback when any
  candidate read is incomplete;
- the disabled setting and solid archives use staged unarchiving;
- successful kept archives and successful extracted folders commit to their
  established destinations and request a library refresh;
- archive and extracted-folder mismatches expose the correct pending artifact;
- Keep Files commits only the staged artifact, while Delete Files removes only
  staging data and preserves pre-existing destination content;
- a failed transactional promotion restores the existing destination and
  leaves the pending decision retryable;
- the dialog cannot close through any normal route before a pending decision
  succeeds;
- keep/delete failures retain the decision state and can be retried;
- cancellation and non-mismatch failures clean attempt staging data.

The implementation will finish with the focused settings, Find BMS, archive,
and scene tests plus the desktop compile check documented in `AGENTS.md`.
