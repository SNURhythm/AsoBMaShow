# Parallel storage encoding

The user wants a large video to stop blocking subsequent storage-optimization
targets. The approved direction is bounded parallel encoding with two video
slots, the existing audio-worker policy, serialized writes, bounded temporary
storage, and cancellation that drains active encoders.

## Execution

Use one shared admission budget across local directories and archives. Admit at
most two video encoders and the configured number of audio encoders. Keep each
video codec at its existing two threads. Separate codec-slot release from scratch
reservation release: completed outputs still occupy reserved storage until their
owner commits or discards them. Failed startup must release both reservations.

Each directory has a bounded candidate window and a single mutex protecting its
evidence, transactions, report and progress. Hashing and encoding run outside
that mutex; completed independent files may commit before a slow earlier video.
Revalidate the source and destination against current evidence before committing.
Parent/descendant evidence ordering remains intact.

Archives use bounded lookahead with unique staged inputs and outputs. Extraction,
SQLite access, journal updates and compaction remain on the archive owner thread.
Only encoding touches staged files concurrently. Consume results in source order
so existing compaction and recovery rules remain valid. Scratch limits cover all
pending entries, not one entry per encoder. A full window or insufficient scratch
space may apply backpressure; concurrency is not an unbounded promise that every
remaining file starts immediately.

Schedule independent archives alongside directory owners, sharing the same
encoding budget. Recovery still completes once before any owners start. Progress
is serialized and completed-file counts remain monotonic. Archive bytes and
file completion continue to reflect committed work, not speculative encodes.

## Failure and lifecycle

Cancellation stops new admission, requests cancellation in active encoders, and
joins them before deleting staging or releasing maintenance. Unresolved mutation
failure stops admission across all owners. Existing archive journals remain
recoverable, including a crash with speculative staged media present. Unknown
files in recovery directories remain protected.

## Verification

Use controlled encoder barriers to prove a held video overlaps later audio and
another video, including a single directory and a single archive. Assert real
transaction results, exact totals, unique outputs, concurrency limits, cancellation
draining, scratch backpressure and recovery cleanup. Run the affected library,
audio and archive groups, a desktop compile, and platform compile checks as needed
for new shared C++ code. Do not upload builds or modify the user's chart library.
