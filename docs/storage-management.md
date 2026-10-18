# Chart storage management

Open **Settings → BMS Library → Storage Management** to reduce installed package
sizes. Select folders, choose independent audio and BGA actions, then use
**Review changes → Confirm & apply**. Defaults keep all media unchanged.

## What a selection includes

Each checkbox selects the entire folder and its subfolders: every difficulty's
resources, preview audio, and unreferenced media. Expanding a row shows its charts;
it does not narrow the operation to that chart. Nested selections are processed
as a union, so a file is changed only once. Search keeps hidden selections; the
selected count tells you how many are hidden.

The tool does not scan chart references or discover owners outside the selected
folders. Selecting a folder therefore also affects any other chart that uses its
media. Chart files and paths remain unchanged, preserving their hashes and the
identities used by scores, replays, favorites and playlists. The app resolves
converted resources through its existing extension fallback. Other BMS players
may have different fallback behavior.

Rows show logical file bytes, including separate audio, video and preserved-image
totals. These are not physical disk-allocation measurements. Unknown or unreadable
sizes remain unavailable. The review reports eligible input bytes; compression
savings are only known after encoding and validation.
Hard-linked regular files contribute their logical bytes at each selected path,
but are excluded from modification. Overlapping selected folders count each path
only once in the combined total.

Browsing measures sizes in the background and publishes completed folder totals
as they become available. It keeps folder summaries rather than every asset in
memory. Review scans only the selected folder union for conversion eligibility;
successful changes update known row totals directly. Uncertain results refresh
only rows inside the changed subtrees. A strict ancestor whose total cannot be
updated safely stays unavailable until **Refresh**, so a subset operation does
not recursively scan unrelated sibling folders. Opening confirmation does not
scan the entire library again.

Library refreshes invalidate sizes for their changed source paths, including
nonrepresentative charts and resource-only changes. Unrelated completed rows stay
cached. The bounded change history conservatively invalidates all sizes if it
cannot identify the changed scopes. Cached sizes are estimates: external edits
become visible after a library refresh or an explicit storage **Refresh**.

## Audio choices

| Choice | Behavior |
| --- | --- |
| Keep | Leave audio unchanged. |
| Lossless FLAC | Convert supported integer PCM WAV to FLAC, verifying identical decoded samples, rate, channels and length. |
| Lossy Ogg Vorbis | Convert supported WAV/FLAC to Vorbis at quality 0.5, preserving rate, channels and length. |

Lossless conversion supports 8-, 16- and 24-bit integer PCM. Float and 32-bit PCM
WAV are skipped rather than silently quantized. Existing Ogg and MP3 are never
recompressed. Unsupported channel layouts and playback-critical metadata that
cannot be preserved are skipped. Vorbis changes sample values; keep a separate
backup if you may want the originals later.

The output keeps the filename stem with `.flac` or `.ogg`. It replaces the source
only after full decode validation. Lossless FLAC is kept even when its size is
equal or larger, so supported WAV files use one format consistently. Lossy Vorbis
still requires a smaller output. WAV ancillary chunks are preserved byte for byte
in FLAC RIFF APPLICATION metadata, together with their RIFF envelopes, order and
ancillary-chunk padding. This includes `smpl`, `inst`, `cue `, `LIST` (INFO and cue labels), `bext`,
`id3 `/`ID3 `, XML, cart and unknown vendor chunks. Their contents are retained
without translating sampler loops or embedded tags into player-specific fields.
Plain format/fact chunks and disposable JUNK/PAD padding alone do not trigger
foreign-metadata storage. Malformed RIFF chunk bounds and metadata chunks exceeding
FLAC's block-size limit remain unsupported. Vorbis retains its stricter metadata
checks because it has no equivalent preservation path. A complete odd-sized PCM
payload may omit its final WAV alignment byte, including when RIFF counts that
byte; missing sample data still fails validation.

BMS packs often include alternate encodings for legacy players. When the requested
same-name FLAC, Vorbis or H.264/MP4 destination already exists, storage optimization
fully validates it, retains its bytes and removes the redundant source through the
recoverable transaction. This applies to local folders and ZIP/TAR entries. It
assumes the alternatives contain the same recording; it does not compare their
samples. Invalid, unsupported or changed destinations preserve the source.
Reuse leaves the destination's existing quality, resolution and frame rate intact.
Case-only local filename ambiguities and alternatives with no existing destination
remain skipped. Existing destinations are never overwritten.

## BGA video choices

**Compress to H.264** uses software x264, an MP4 container and the `medium` preset.
High, Balanced and Smaller quality correspond to CRF 18, 23 and 28; Balanced is
the default. One conversion runs at a time, with at most two encoder threads.
The main saving is replacing inefficient video such as MPEG-1; resizing is optional.

Resolution choices are Original, 1920 × 1080, 1280 × 720, 854 × 480 and 640 × 360.
They bound the displayed long and short edges, preserving aspect ratio and
orientation without upscaling. For example, a square 1024 × 1024 source capped at
1280 × 720 becomes 720 × 720. Original preserves geometry, except odd coded
dimensions may shrink by one pixel per axis for 4:2:0 encoder alignment, with
sample aspect ratio adjusted to preserve display shape.

FPS choices are Original, 60, 30, 24 and 15. Original retains presentation timing,
including variable-rate gaps. A cap drops frames when needed without duplicating
frames or slowing playback. Retained frames use a rational time grid when frame
reduction is necessary; timing adjustment is bounded by one cap interval. A
30000/1001 source remains unchanged under a 30 FPS cap.

Embedded audio is retained: compatible streams are remuxed; supported other audio
is converted to AAC at 128 kbit/s. Unsupported HDR, alpha, interlacing, stream
layouts or essential metadata produce a skip rather than a destructive conversion.
Decoded color properties must remain consistent and representable throughout the
video. Embedded audio must retain its sample format, sample rate and full channel
layout; changes preserve the original. Malformed frame rates that exceed encoder
limits are rejected before computing keyframe intervals.
WMV/ASF validation checks the physical packet/container extent and interprets its
declared duration as a file-wide endpoint. Coarse audio timestamps are checked
against an anchored sample clock so timestamp rounding cannot accumulate drift.
Matroska/WebM validation checks its declared container endpoint across both
streams. Video and audio stream flags must survive MP4 conversion; flags the
container cannot represent cause the original to be preserved.

Existing H.264 already within the caps is skipped, even if a different quality
preset is selected. Every candidate is decoded and validated. **An output must
be smaller to be installed, even when caps were requested.** A “not smaller” result
means the original remains and its requested caps were not applied.

**Delete BGA videos** permanently removes supported video files throughout the
selected folders, including nested, unreferenced and alternate versions. Supported
extensions are MP4, WMV, M4V, WebM, MPG, MPEG, M1V, M2V and AVI. All images are kept,
including static/layer/miss BGAs, stage artwork, banners and backgrounds. Audio,
charts and other files are also kept. Deletion has no undo after completion.

## Supported locations

Writable ordinary local folders are supported, including app-owned mobile
downloads/imports and accessible writable iOS folders. Writable local archives
can also be updated in place:

| Container | In-place support |
| --- | --- |
| ZIP / ZIP64 | Single-volume archives with stored or deflated regular files and directories. |
| TAR | Uncompressed USTAR regular files and directories. |
| Other formats or layouts | Excluded with a format/layout explanation. No full-copy fallback. |

Encrypted/split ZIPs, links, overlapping or ambiguous entries, compressed TARs,
GNU/PAX/sparse TAR records, RAR and 7z are not supported by the in-place updater.
ZIP padding or extra records between local entries or before the central directory
are rejected before mutation; leading prefixes remain supported.
Apply also rejects conflicting Unicode names or any filename/size disagreement
between the browsing index and physical archive, including reader normalization.
Reading these formats elsewhere in the app remains separate from update support.
Android SAF/content-provider references require the existing local import flow.
Symlinks, hard links, special files and unsafe filesystem paths are rejected.

Selecting several folders inside one archive creates one batch. Only the selected
media changes; charts, images and unselected entry contents stay intact. Review
uses the existing browsing index for uncompressed candidate input bytes and
collision checks, once per physical archive. It does not rebuild the physical
update manifest or seek through every local entry header. Archive layout safety
is validated when Apply starts, before encoding or any archive write; candidates
in encrypted, unsafe or otherwise unsupported layouts can therefore be rejected
at that point. Source identity is checked during both review and Apply.
Reported savings are the reduction in
the archive's file length, which can differ from filesystem allocation.

There is no second complete archive or extracted tree. A disk-backed index and
an 8 MiB copy batch with at most 1,024 chunks/records and a private redo file of
at most 16 MiB accompany one extracted input, encoded output and prepared
replacement at a time. The media staging budget is 1 GiB, additionally limited by
free space; an entry must fit a conservative three-times-input estimate plus
8 MiB. Encoded output and its replacement must also fit the staging budget.
Oversized entries are skipped. Lossy conversions must reduce the stored archive
record too. Lossless FLAC is retained even if the original ZIP compression was
more compact. Growing records are appended to the source archive, their original
slots are reclaimed by the forward compaction pass, and the appended records are
then compacted into place before rebuilding metadata. This requires temporary
space in the archive filesystem for the appended records, without a second full
archive or repeated shifting of the remaining archive.
Confirmation reads physical layout metadata, not all payloads. There is no separate
payload pass over deleted entries or unchanged prefixes. Compaction reads only the
source and destination ranges needed for its bounded forward-copy batches. Completion does not reread the
whole archive. Leading ZIP stubs and other prefix bytes remain at their original
offsets, including when the first entry is deleted or replaced; even huge sparse
prefixes are preserved without being read or copied. Consecutive deletions and
small retained files share durable batches. Adjacent destination ranges share
archive writes, using at most one additional 8 MiB memory buffer. Archive writes
remain ordered. Batches of at least 1 MiB and four chunks hash their already-read
payloads with up to four workers, reduced on machines with fewer available cores.
Small batches stay serial. Workers finish and any hash failure is reported before
the batch changes archive capacity or writes recovery evidence. File I/O and
SQLite commits remain on the ordered writer; hashing adds no payload buffer.

## Progress, cancellation and recovery

Progress separates scanning, archive preparation, encoding, verification and commit.
Archive compaction displays byte progress through the original container. Manual work stops
music/preview readers and reserves library maintenance; competing playback or
library operations cannot start during the mutation. Back requests cancellation
and returns after the worker releases maintenance. Mobile backgrounding cancels
manual work at a safe checkpoint.
Active artwork reads drain before mutation; new artwork requests wait until
maintenance releases its guard, including any recovery-required hold.
All scene admission checks run before media is stopped, so a rejected request
leaves existing playback alone.

Ordinary local files commit separately. Cancel keeps already completed changes and retains
uncommitted originals; it does not roll back the entire batch. Output is staged
privately, validated, then installed using a journal and same-volume temporary
files/backups. On POSIX systems, replacements preserve the source's access
permission bits. Startup recovery checks interrupted transactions before admitting
new work. After successful recovery, abandoned private encoding candidates are
removed; unrecognized files and unresolved recovery state are retained. Do not
manually remove recovery files while an operation is pending.
On iOS, recovery resolves app-owned Documents paths against the current app
container, while still requiring the recorded file identities and contents.

Archives use durable forward recovery instead of a full backup. Before overwriting
a bounded batch, the updater saves its before/after bytes and progress, then syncs
the archive and records its modification/change timestamps before advancing.
Recovery checks the pinned file identity and durable source timestamps, then
validates only the bounded redo ranges. It does not hash the whole archive.
A crash during a write or truncation, before the new source timestamp is durably
recorded, is ambiguous with an external edit. Automatic recovery stops without
further writes and retains the archive and journal for manual review; restarting
alone cannot resolve that state. Checkpointed interruptions can resume normally.
Legacy version-1 journals retain their original full-hash recovery check once
before upgrading; new operations never create them. Cancellation stops new
conversions and finishes the archive with the changes already accepted; this
finalization can take time for a large archive. Archive recovery runs before
playback and library readers are admitted. Keep both the archive and its private
recovery directory intact if recovery is required.
Completed archive journals enter a durable cleanup-only state before their
metadata is removed, so interrupted cleanup can finish on the next startup.

Independent folders can convert audio concurrently: up to four workers on
desktop. Mobile scanning and audio conversion use the same worker policy as chart
audio loading, reserving
processor capacity for foreground threads (for example, four workers on six
logical processors and six on eight). Actual concurrency is also limited by
available independent folders and temporary space. A folder and its selected
ancestors finish in order; video remains one job at a time. Cancellation waits for active transactions to
settle before releasing library maintenance. A recovery failure stops new work
across all workers. Temporary-space budgets cover concurrent outputs and are
checked against available disk space before each file starts.

The result includes converted, deleted, skipped and failed counts. Reclaimed
bytes are counted only when obsolete originals/backups have actually been removed.
Growth from lossless replacements offsets savings elsewhere in the same job; a
net increase reports zero reclaimed bytes rather than unsigned wraparound.
Common outcomes:

| Outcome | Meaning |
| --- | --- |
| Already optimized / not smaller | Original retained; no savings claimed for that file. |
| Unsupported format / codec unavailable | This build cannot safely perform the requested operation. |
| Collision / unsafe path / stale input | Filesystem evidence is ambiguous or changed; original retained. Refresh and review again. |
| Unavailable / insufficient space / I/O or validation failure | The affected scope/file could not be processed safely; inspect the reason before retrying. |
| Cleanup pending | A known obsolete backup remains. Its bytes are not counted as reclaimed; recovery can retry cleanup. |
| Recovery required | Retained state needs recovery before further mutation. Keep the affected files and private recovery records intact. |

## Automatic Find BMS downloads

Settings offers a separate download policy and **Save storage after download**
toggle, initially Off. It is profile-specific and independent of the manual policy.
Enabling video deletion requires an explicit acknowledgment. Find BMS previews
the captured policy; changing profiles/settings does not alter an active attempt.

A non-no-op enabled policy forces extraction for that attempt, even when
**Skip unarchive** is saved. The saved extraction preference is not changed.
Keep/Keep does not force extraction. Processing happens after chart verification,
inside private extracted staging, before package promotion and library registration.
All chart paths and content are verified unchanged again before commit.
This comparison streams hashes into a private temporary SQLite identity table
with a 1 MiB page-cache target and disk spilling, then checks and removes each
matching row. Changed, added or missing charts prevent installation. The table
is removed when verification ends, including cancellation or failure.

Ordinary per-file skips/failures keep originals and allow a verified download to
finish with a warning summary. Cancellation, unsettled recovery or cleanup state
prevents promotion. A hash-mismatch package explicitly kept by the user remains
unoptimized. Automatic work pauses during foreground gameplay or mobile
backgrounding and remains cancellable.

## Developer verification

See [scale measurements and platform limits](testing/storage-scale-2026-10-18.md)
for the opt-in benchmark, selected worker defaults and remaining dense-directory
validation cost.

Storage tests belong to the library, audio, video, settings and UI groups listed
in [the testing guide](testing/README.md). `storage_optimizer_tests` includes a
disposable library with real FLAC, Vorbis and H.264 encoders, caps, deletion,
overlapping selections, archive eligibility, unchanged chart/profile identities,
and resource resolution/decoding after replacement. Download workflow fixtures
cover extraction precedence, cancellation, warnings, mismatch handling and commit
ordering without fetching a real package.

Verification for this implementation used the desktop grouped suites and a real
disposable-library conversion, plus the iOS and Android build-only paths and native
codec/file transaction tests on iOS 27.1 Simulator and the Android 10 emulator.
The desktop music UI was inspected, but
that build reports native music playback unavailable. Windows build and filesystem
execution were not available in this session; they remain release validation work.

Archive update coverage includes desktop ZIP/TAR batching, sparse ZIP64 offsets,
legacy filenames, real FLAC playback resolution, checkpointed batch recovery,
fail-closed ambiguous writes/external edits and
interrupted journal cleanup. The archive additions also pass iOS and Android
build-only checks; giant-library device performance has not been measured.
