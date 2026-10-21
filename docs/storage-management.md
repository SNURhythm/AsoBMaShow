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
foreign-metadata storage. Truncated ancillary chunks retain their physically present
bytes; metadata exceeding FLAC's block-size limit remains unsupported. Vorbis
skips playback metadata it cannot preserve, such as sampler loops and cue points. Known descriptive tags
are best effort: duplicate fields, aliases and imperfect text may be normalized
or dropped. Invalid UTF-8 descriptions are not passed to the encoder, avoiding
unsafe FLAC comment handling for legacy WAV tags; lossless RIFF APPLICATION
blocks still preserve their original bytes. Stale outer RIFF sizes are intentionally
tolerated. Malformed data lengths, missing alignment bytes and incomplete final
samples use the complete PCM frames the source decoder can read. Lossless output
must match those decoded samples exactly; it does not invent missing samples.
Empty or undecodable audio still preserves the original.

BMS packs often include alternate encodings for legacy players. When the requested
same-name FLAC, Vorbis or H.264/HEVC/AV1 MP4 destination already exists, storage optimization
fully validates it, retains its bytes and removes the redundant source through the
recoverable transaction. This applies to local folders and ZIP/TAR entries. It
assumes the alternatives contain the same recording; it does not compare their
samples. Invalid, unsupported or changed destinations preserve the source.
Reuse leaves the destination's existing quality, resolution and frame rate intact.
Case-only local filename ambiguities and alternatives with no existing destination
remain skipped. Existing destinations are never overwritten.

## BGA video choices

**Compress video** writes MP4, with H.264 selected by default. It prefers working
hardware encoders (VideoToolbox on Apple, MediaCodec on Android, and supported
GPU encoders on Windows). H.264 falls back to software x264 when hardware cannot
encode or preserve a particular file. HEVC and AV1 are offered only with working
hardware encoding; they never silently switch to another codec.

The codec button opens an explanation of each available choice, with encoding
and playback information from this device. A short background encode/decode test
must succeed before an option appears. Apple playback uses VideoToolbox and Metal
for supported 8-bit SDR H.264/HEVC: decoded surfaces are sampled directly, with
YUV-to-RGB conversion in the fragment shader and no CPU pixel upload. The probe
checks native surface import and hardware decoder support for its stream before
reporting hardware playback. Unsupported formats, renderers and failed imports
use software decoding; skin movies with strict preallocation limits retain their
CPU memory-budget contract. Other platforms currently use the FFmpeg CPU playback
path, and AV1 playback includes dav1d. Hardware encoding support alone does not
imply hardware playback. Every complete output still passes the normal MP4
decode-back validation.

High, Balanced and Smaller choose encoder-specific quality or bitrate targets;
Balanced is the default. The x264 fallback uses the `fast` preset and CRF 18, 23
or 28 respectively. Up to two videos encode at a time, each with at most two
software encoder threads. Hardware drivers manage their own execution.

The main saving is replacing inefficient video such as MPEG-1; resizing is optional.
By default, H.264, HEVC/H.265, VP9, AV1 and VVC/H.266 sources are preserved before
full decoding. Enable **Include already efficient videos** to apply the selected
codec, quality and caps to those files too. Re-encoding is lossy, and an output
replaces its source only when validation succeeds and the result is smaller.

Audio uses separate worker capacity, so a large video can overlap later audio
and video targets, including files in the same folder or archive. Independent
folders and archives also share that capacity. Pending work and temporary space
are bounded; low free space or a full lookahead window applies backpressure.
Folder commits remain serialized, and archive replacements are written in their
original order. Cancel stops admission and waits for active encoders before
removing temporary files or releasing the library for other work.
Completed archive encodes retain their temporary-space reservations, but do not
fill the encoder queue. Later targets continue while scratch space is available.
Archive read-ahead reserves and extracts inputs in source order, including queued
videos, so a busy video pool does not hide later audio. Staged inputs wait for
their own codec pool without occupying encoder slots in the other pool.
Read-ahead continues across images, unchanged audio and unselected entries without
waiting between metadata batches. Scratch limits and existing-destination
validation can still pause admission; archive writes remain in source order.
The status shows active and finished archive encodes separately from the
source-order file count, so a slow video does not hide that activity.

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
AAC conversion also skips meaningful matrix-surround, downmix and audio-service
instructions that cannot survive in its sample-only input. Default values are
accepted; codec-native instructions remain intact when their bitstream is remuxed.
Remuxed audio must also decode to the same samples after conversion, including
trimmed intervals. This comparison uses the required decode passes and does not
add another file scan; decoder-ignored source metadata remains advisory.
Decoded color properties and chroma sample positioning must remain consistent
and representable throughout the video, including when resizing. MP4 uses a
microsecond movie clock to retain fractional-millisecond audio/video offsets.
Embedded audio must retain its sample format, sample rate and full channel
layout; changes preserve the original. Malformed frame rates that exceed encoder
limits are rejected before computing keyframe intervals.
Source declarations are intentionally advisory: stale starts, durations, ASF
sizes or packet counts, and Matroska/WebM endpoints do not block a clean decode.
This includes clean early EOF; conversion preserves the presentation actually
decoded from the source. Source decoding also accepts concealed frames and skips
recoverably invalid packets, continuing to later readable frames. Unrecoverable
I/O errors, unusable timelines and empty streams still preserve the original.
Output and existing-target decoding and completeness checks remain strict. Coarse
audio timestamps are checked against an anchored sample clock so rounding cannot accumulate drift.
Descriptive container tags are best effort and may be dropped by MP4. Captions,
ReplayGain/R128 volume metadata and unsupported visual playback metadata preserve
the original instead. Video and audio stream flags must survive MP4 conversion;
flags the container cannot represent cause the original to be preserved.

Videos that require conversion are fully decoded and validated. **An output must
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
ZIPs with archive-wide certificate stores or central-directory signatures also
remain unchanged. Converted entries require safe extra fields in both local and
central headers: timestamps, ownership and unchanged comments are retained,
Unicode filenames and ZIP64 values are regenerated, and unsupported fields skip
that entry. Untouched entries retain their original extra fields.
ZIP padding or extra records between local entries or before the central directory
are rejected before mutation; leading prefixes remain supported.
Apply also rejects conflicting Unicode names or any filename/size disagreement
between the browsing index and physical archive, including reader normalization.
Reading these formats elsewhere in the app remains separate from update support.
Android SAF/content-provider references require the existing local import flow.
Symlinks, hard links, special files and unsafe filesystem paths are rejected.
On iOS, active folder grants cover their descendants. A failed overlapping
bookmark does not revoke an active parent or child grant; access validation uses
the most specific active boundary and still excludes unauthorized siblings.
Android document renames identify open writers by filesystem identity before
moving their reservations, covering both case aliases and distinct case-sensitive
siblings.

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
at most 16 MiB accompany a bounded window of extracted inputs and encoded outputs,
plus one prepared replacement at a time. The aggregate media staging budget per
archive is 1 GiB, additionally limited by shared free-space reservations. Each
entry initially reserves three times its input size plus 8 MiB; growing outputs
must reserve additional space before installation. Completed outputs retain their
reservation until consumed or discarded.
If extraction fails before any archive mutation, the original archive stays
unchanged and partial staging is cleaned up. After earlier changes have begun,
the journal is retained for recovery; a failed input never replaces its original.
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
new work. Every platform displays a recovery screen with an activity indicator,
the current phase and item, completed operations, and the durable position within
the current archive when available. It stays responsive during filesystem work;
it does not estimate a percentage or scan archives merely to calculate one.
After successful recovery, abandoned private encoding candidates are
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

Audio targets can convert concurrently: up to four workers on
desktop. Mobile scanning and audio conversion use the same worker policy as chart
audio loading, reserving
processor capacity for foreground threads (for example, four workers on six
logical processors and six on eight). Actual concurrency is also limited by
available targets and temporary space. A folder and its selected
ancestors finish in order; up to two videos encode concurrently. Cancellation waits for active transactions to
settle before releasing library maintenance. A recovery failure stops new work
across all workers. Temporary-space budgets cover concurrent outputs and are
checked against available disk space before each file starts and as encoded outputs grow.

Before changing local media, the tool checks chart evidence in its folder and
selected ancestors and descendants. A nested chart edit invalidates the parent
media operation even when the parent's directory timestamp is unchanged.
Descendant chart membership is checked once when a folder starts; bounded,
spillable chart identities and directory stamps are checked again before commit.
Independent selected roots can continue when another root becomes stale.
These checks do not discover chart references in sibling folders; an edit there
does not invalidate this folder's conversion.

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

Storage tests belong to the library, audio, video, settings and UI groups listed
in [the testing guide](testing/README.md). `storage_optimizer_tests` includes a
disposable library with real FLAC, Vorbis and H.264 encoders, caps, deletion,
overlapping selections, archive eligibility, unchanged chart/profile identities,
and resource resolution/decoding after replacement. Download workflow fixtures
cover extraction precedence, cancellation, warnings, mismatch handling and commit
ordering without fetching a real package.

Archive update coverage includes ZIP/TAR batching, sparse ZIP64 offsets, legacy
filenames, real FLAC playback resolution, checkpointed recovery, ambiguous
writes/external edits and interrupted journal cleanup. Native Windows filesystem
behavior and physical-device throughput still need release validation.

Opt-in benchmarks use disposable fixtures and are excluded from normal CTest:

```sh
cmake --build cmake-build-debug --target storage_scale_benchmark storage_optimizer_tests -j 6
cmake-build-debug/storage_scale_benchmark synthetic 10000 1000 4 wide
cmake-build-debug/storage_scale_benchmark audio 8 32 4 wav-flac
cmake-build-debug/storage_scale_benchmark archive-review 20 1000 6 persisted
cmake-build-debug/storage_optimizer_tests --archive-traversal-benchmark 20000
cmake-build-debug/storage_optimizer_tests --archive-compaction-benchmark 10000 2048 1
```

The scale harness is POSIX-only. Cached synthetic timings do not predict cold
storage or phone throughput. Conversion still revalidates directory membership
at each commit; a densely populated directory can require quadratic traversal
work even though memory stays bounded.
