# Parsing Log Export Design

## Goal

Add an `Export Log` action to the Parsing Logs modal so a user can save or
share the current performance log as a plain-text file on desktop, Android,
and iOS.

## Design

The modal snapshots `archive_file::debugLogText()` when the button is pressed.
It passes that text to a new in-memory text-export entry point in
`PlatformDocumentHandoff`. The handoff layer stages the snapshot in a unique,
owner-private temporary directory, retains an opaque cleanup owner until the
detached native export worker stops reading, and presents the existing native
save/share UI with MIME type `text/plain` and suggested name
`AsoBMaShow-performance-log.txt`.

The footer contains a flexible status label followed by `Export Log` and
`Close`. While an export is active, `Export Log` is disabled and reads
`Exporting...`. Completion reports `Performance log exported.`, cancellation
reports `Log export cancelled.`, and failure displays the platform diagnostic
or `Log export failed.`. Closing the modal does not cancel an active native
picker; scene cleanup closes it nonblockingly, while source lifetime remains
owned by the detached worker.

Desktop document handoff selects a text-file filter and generic text-export
title for `text/plain`; existing profile archive behavior is unchanged.

## Limits and Error Handling

- The snapshot is capped at 4 MiB, comfortably above the current 500-line log
  retention limit.
- Invalid names, zero limits, oversized text, temporary-storage errors, and
  write failures return a normal failed handoff result.
- Temporary files are mode/ACL restricted through the existing document
  handoff security helpers and removed by lifetime ownership.
- Export does not clear or mutate the live log.

## Testing

- A platform handoff unit test verifies exact UTF-8 bytes, `text/plain`, the
  suggested filename, explicit size limit, and cleanup after the source
  lifetime is released.
- Failure tests cover an oversized snapshot and invalid suggested filename.
- Existing platform handoff tests, focused scanner tests, and the desktop
  `main` build remain green.
- No archive performance benchmark is run on this machine.
