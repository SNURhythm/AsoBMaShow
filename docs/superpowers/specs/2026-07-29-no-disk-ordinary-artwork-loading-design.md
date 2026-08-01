# No-Disk Ordinary Artwork Loading Design

## Context

Ordinary chart jackets and banners became fast after commit `96b3b477`
because `ImageView` began writing and synchronously restoring 256-pixel RGBA
previews from the archive temporary cache. That commit did not change worker
count, priority, or queue order. The preview files can consume up to about
256 KiB per artwork and have no automatic size bound, so this is not an
acceptable authority for ordinary files.

Archive-entry artwork is different: its existing preview avoids reopening and
extracting the archive member. This design does not change that established
archive cache.

## Requirements

- Ordinary filesystem jackets and banners must never create or read persisted
  artwork previews.
- Archive-entry jackets and banners must retain their existing persisted
  preview behavior and cache format.
- Decoded ordinary artwork remains reusable through the process-local
  `ImageView` cache.
- A newly visible ordinary list item must run before older queued work that is
  no longer likely to be visible.
- Selected artwork retains its dedicated priority worker.
- Slow-load diagnostics must distinguish queue delay, source open, source
  load/decode, RGBA copy, and archive-preview work.
- Artwork must still appear independently as soon as its decode finishes.

## Design

### Cache ownership

`ImageView` will restore and write disk thumbnails only when the source is an
archive virtual path. Ordinary paths will use the lexical async key and the
existing process-local decoded-image cache. The ordinary replacement-writing
extension to `archive_file::materializeFileBytes` will be removed, returning
that shared API to its prior archive-materialization contract.

### Cold-load diagnostics

The decode worker will collect phase timings for each task. On ordinary POSIX
paths, opening the file is measured separately from `stbi_load_from_file`.
Android descriptor acquisition and archive-member reads receive the analogous
source-open/read phase. The existing slow-load line will report the individual
phases instead of calling the complete operation "decode." Diagnostics remain
thresholded and do not add filesystem probes to UI-thread binding or polling.

### Queue policy

The background queue represents speculative list artwork. New bindings are
more likely to be visible than old queued bindings after scrolling, so workers
will take the most recently queued background task first. Already running work
is not cancelled. The selected-artwork priority queue remains separate and
unchanged.

## Testing

- Replace the persisted ordinary-thumbnail regression with a cold-reload test
  proving no immediate disk preview is restored after process-cache reset.
- Keep archive-thumbnail coverage unchanged.
- Add a real FIFO-backed scheduling regression: occupy all background workers,
  queue stale work, then queue a newly visible image and prove the new image
  completes first when a worker becomes available.
- Run the focused image tests, full configured CTest suite, desktop `main`, and
  the non-deploying iOS compile.

## Safety

No profile, chart, or replay data is modified. Existing ordinary preview files
from build 298 remain removable through the existing temporary-cache cleanup;
the new code creates no more of them. Archive previews remain compatible.
