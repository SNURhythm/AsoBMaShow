# Storage management and download storage saving

Status: implemented. The companion plan records verification evidence and the
remaining Windows and end-to-end mobile playback validation limits.
Latest agreed scope: recursive folder-wide operations without chart-reference
scanning; BGA deletion removes videos and preserves all images.

## Intent and agreed requirements

Reduce the disk space used by installed BMS packages and newly downloaded BMS
packages while preserving chart identity and playability. The manual browser
must reuse the grouped chart representation in the music player.

Compression covers the entire selected folder subtree, including nested media
and resources used only by other sabuns. The group's representative chart is
for presentation only; its references never define the compression file set.

The requested operations are:

- Lossless WAV to FLAC conversion.
- Lossy WAV or FLAC to Ogg Vorbis conversion.
- BGA video compression using a modern codec, and BGA deletion.
- A persistent option to apply a chosen storage-saving strategy automatically
  to Find BMS downloads.
- Optional resolution and frame-rate upper bounds for BGA compression.

The primary BGA saving is MPEG-1 to H.264 conversion. Downscaling is optional;
the default preserves resolution and frame timing. Caps must never upscale,
increase frame rate, or change playback speed.

The defaults, selection behavior, supported storage backends, and quality
presets below are proposed implementation decisions, not additional requests
from the user.

## Existing integration points

- `MusicPlaylistRepository::SelectLibraryTracks()` groups charts by folder,
  falling back to chart path when the folder is empty. It supplies representative
  charts and counts. `SelectLibraryGroupTracks()` supplies expanded children.
- `music_playlist::MusicTrack`, `MakeTracks()`, and the local
  `MusicTrackRowView` in `MusicPlayerScene.cpp` supply identity and presentation.
  Extract the reusable row/presentation code instead of copying the scene.
- Audio consumers share `audio/ChartAssetExtensions.h`. The resolver tries the
  authored path first, then FLAC, WAV, Ogg, and MP3 alternatives.
- `audio/Jukebox.cpp` already resolves alternate BGA extensions. Its video list
  currently starts with MP4, then WMV, M4V, WebM, MPG, MPEG, M1V, M2V, and AVI;
  image fallback follows. Unlike audio, this is extension-priority lookup.
  Preserve that behavior and centralize it for inventory and playback.
- libsndfile is already linked, including FLAC/Vorbis dependencies in the iOS
  library packaging script. FFmpeg with x264 is already requested by
  `vcpkg.json` and `scripts/get_ios_libs.py`. Probe the actual linked encoders
  instead of assuming every packaged build has them.
- `DownloadedArchiveWorkflow` verifies and commits either an archive or an
  extracted directory. `FindBmsModal` starts a `FindBmsTask` with immutable
  download options; both `MainMenuScene` and `MusicSelectScene` supply options.
- Settings use `AppSettings`, `AppSettingsStore`, and the existing profile
  persistence coordinator. Library tasks already expose foreground-performance
  pause state through `SceneManager` and `ApplicationContext`.

## User experience

### Manual management

Add a Storage Management entry to Settings' library section. It opens a
responsive scene with the same representative artwork, title, artist, chart
count, search matching, and expandable chart presentation as the music player.
Selection is at the **group** level: expanded charts explain what is affected;
they do not imply that a shared keysound can be changed for only one difficulty.
Selecting a folder group selects its entire recursive folder contents for media
inventory, regardless of which chart represents the group or is expanded. Apply
the chosen compression policy to every eligible media file in that subtree,
including files absent from every chart's declarations. BGA deletion likewise
covers all supported videos in that subtree, preserving every image. No operation
scans chart references or builds a cross-chart ownership graph.

Provide group checkboxes, Select filtered, Clear selection, and sorting by title
or measured size. Search does not silently discard existing selections. Show the
selected count, including selected groups hidden by the current filter. A row
shows total bytes, audio bytes, video bytes, preserved image bytes, and eligibility.
Unknown sizes show as pending/unavailable, never as zero. Measurements run off
the UI thread and distinguish logical file bytes from guaranteed physical disk
space; sparse files, compression, and hard links can make physical savings differ.

Audio and BGA choices are independent and can run together as one job. A
preflight summary shows eligible files/bytes, skipped groups and reasons, and
the selected quality/caps. Exact compression savings are reported
after conversion; do not invent a pre-encode estimate. Deletion bytes can be
calculated from the eligible file union.

Before Apply, show that lossy conversion replaces originals and BGA deletion is
permanent. Lossless conversion also replaces the WAV only after verification.
The confirmation belongs to the app's future user flow; writing this feature
does not authorize modifying the developer's installed chart library.

Progress shows scanning, encoding, verifying, committing, and final counts of
converted/deleted/skipped/failed files plus bytes reclaimed. Cancel stops at a
safe checkpoint; completed files remain changed. While a manual job owns library
maintenance, navigation that starts playback, exports, imports, or another
mutation is unavailable. Back offers cancellation and returns after the worker
has released ownership, without blocking the UI thread.

### Strategy settings

Persist `StoragePreferences` in the active profile's `AppSettings`, consistent
with existing Find BMS settings. Store two independent policies: the last manual
policy and the download policy. Editing a manual job must not silently change
future downloads. Both use the same controls and validation rules.

| Setting | Values | Default |
| --- | --- | --- |
| Audio | Keep; Lossless FLAC; Lossy Ogg Vorbis | Keep |
| BGA | Keep; Compress video to H.264; Delete BGA | Keep |
| Video quality | High / Balanced / Smaller | Balanced |
| Maximum resolution | Original; 1920 × 1080; 1280 × 720; 854 × 480; 640 × 360 | Original |
| Maximum FPS | Original; 60; 30; 24; 15 | Original |
| Save storage after download | Off / On | Off |

Resolution/FPS/quality controls apply only to Compress video, and retain their
values while disabled. Keep + Keep is a valid no-op; Apply is disabled and the
download toggle alone does not force extraction. Enabling automatic processing
shows the selected policy in the Find BMS dialog. Selecting BGA deletion for
automatic downloads requires one explicit destructive-choice acknowledgment
when that policy is enabled; ordinary downloads do not ask again.

New or malformed settings default to Keep/Keep with automation Off. Unknown
enum values or malformed policy fields must not accidentally enable deletion or
lossy compression. Missing optional fields load their defaults. Active jobs
retain the policy and profile identity captured at submission.
If a supplied policy is malformed, reset that entire policy to Keep/Keep and
disable automation for the loaded preferences; do not preserve a destructive
action beside an invalid cap or quality value.

## Media behavior

### Audio

Use libsndfile directly; no dependency on an installed command-line executable.
Only `.wav` inputs are eligible for lossless FLAC; `.wav` and `.flac` are eligible
for Ogg Vorbis. Verify the decoded format rather than trusting extensions.
Existing Ogg/MP3 files are left alone. Enumerate eligible audio files recursively
throughout the selected folder subtree, including preview audio, sabun-specific
resources, and unreferenced files. Do not read chart declarations to decide
eligibility. Audio is never removed by the Delete BGA policy.

Lossless FLAC preserves the native sample rate, channel count, frame count, and
integer PCM samples exactly. Support 8/16/24-bit integer PCM that the linked
FLAC encoder can represent. Do not silently quantize float or 32-bit PCM WAV:
skip it with an unsupported-lossless-format reason. Compare decoded source and
output in bounded integer blocks before retiring the source. A file described
as lossless must have sample-exact verification, not just similar duration.

Ogg Vorbis uses quality `0.5` through libsndfile's VBR-quality control, with no
sample-rate conversion, gain normalization, or channel remixing. Validate that
the encoder supports the input's rate/channel layout. Compare decoded channel
count, sample rate, frame count, and complete decodability, retaining the original
on a mismatch. Fixture coverage must catch leading delay or truncated tails;
perceptual equality is not claimed. Preserve supported loop/timing metadata;
skip files with playback-critical metadata that cannot be represented faithfully.

Write the destination beside the source with the same stem and `.flac` or
`.ogg`. Never overwrite an existing different asset or merge two same-stem
sources. Conservatively skip conversion when another audio/visual candidate
with the same directory and stem makes extension fallback ambiguous. Inspect
filesystem siblings only; no chart-reference scan is required.
Keep an output only when it is strictly smaller than the source.

### BGA video

Enumerate supported video files recursively throughout the selected folder
subtree, including videos used only by another sabun or absent from all chart
declarations. Validate their actual media type before conversion; images and
non-media files are not compression candidates.

Use FFmpeg libraries with software `libx264` and an MP4 container. Use preset
`medium`, with CRF 18 / 23 / 28 for High / Balanced / Smaller respectively.
Start with at most two encoder threads and one active media conversion. The
replay exporter's `ultrafast` encoding policy is unsuitable for this feature's
storage objective and must not be reused as the compression policy.

Output uses a compatible 8-bit 4:2:0 pixel format. Preserve visible aspect ratio,
orientation, color range/matrix information, presentation timeline, and duration.
Normalize the container start timestamp while preserving relative timestamps.
Keep embedded audio: remux MP4-compatible audio or transcode it to AAC at
128 kbit/s; preserve its timeline and reject unsupported channel layouts rather
than dropping or remixing audio. Images are not converted to video. Skip video
with alpha, HDR, unsupported interlacing, extra video streams, or other essential
content/metadata that this pipeline cannot preserve.

Interpret resolution presets as orientation-independent bounding boxes: compare
the displayed long and short edges against the preset's long and short edges.
Apply one scale factor at most 1, preserve display aspect ratio, and use valid
encoder dimensions. For odd dimensions, reduce coded dimensions by at most one
pixel per axis and compensate sample aspect ratio; explain this encoder-alignment
exception to literal Original dimensions in technical documentation. Account for
sample aspect ratio and rotation before enforcing the bounds. Example: a square
1024 × 1024 video capped at 1280 × 720 becomes 720 × 720, not 1280 × 720.

Without an FPS cap, preserve source presentation timestamps, including VFR.
With a cap, retain the first frame and select frames against a rational time
grid, never emitting more than one frame per cap interval. When reduction is
necessary, timestamp the selected frames on that grid so adjacent retained source
timestamps cannot accidentally violate the cap; bound the timing adjustment to
one cap interval. Preserve gaps and the final presentation duration; do not
duplicate frames to fill gaps. A CFR source
already below the cap is unchanged: 30000/1001 stays 30000/1001 with a 30 FPS cap.
60000/1001 capped to 30 must retain duration rather than play at half speed.
Unknown or invalid timestamps are a skip reason, not permission to guess speed.

The destination is the same stem with `.mp4`; a source already named `.mp4`
uses the recoverable same-path replacement protocol. Existing H.264 content
that already satisfies the requested caps is skipped to avoid repeated lossy
encoding; quality selection alone does not force recompression of such files.
Other supported video codecs can be attempted, but larger outputs are discarded.
Check collisions and extension-priority resolution before installation.

Decode the full candidate in bounded memory. Check dimensions, caps, timestamp
ordering, complete video/audio decodability, and duration within one source/output
frame interval (and one audio packet for embedded audio). Reject empty, truncated,
invalid, or larger/equal outputs. Caps are subordinate to storage saving: when a
capped output is larger, retain the source and explicitly report that its cap was
not applied.

### BGA deletion and image preservation

Delete all supported video files recursively under the selected folders,
including sabun-specific, nested, and unreferenced videos. Identify candidates
from the shared supported-video extension list; do not inspect chart declarations
or require a representative chart to reference a file. Delete every supported
video alternative, not merely the extension playback would currently choose.

Preserve all images, including static/layer/miss BGAs, banners, stage artwork,
and background images. Also preserve audio, charts, and non-video files. The UI
must call this choice "Delete BGA videos" and explain that images are retained.
This is folder-wide video removal; charts elsewhere referencing those videos are
not discovered or protected by a reference scan.

## Inventory and storage support

Use repository groups for UI identity and enumerate real filesystem children
recursively for all operations and measurements. Never restrict inventory to
representative chart references, known charts, or expanded UI rows. A chart-path
fallback group uses its containing folder as the physical selection scope. Read
no chart resource declarations and build no ownership graph. Malformed charts,
conditional branches, encodings, and unindexed sabuns do not limit media discovery.

Deduplicate nested selected roots and physical files; aggregate selected totals
as a union, not the sum of overlapping rows. Selection includes every nested
folder without requiring its row to be selected separately. Never follow chart
references outside the selected folder union. Use the existing audio/visual
extension lists to classify files and check same-stem filesystem collisions.
Conservatively skip ambiguous conversions instead of rewriting charts. All
supported videos remain eligible for deletion even when multiple alternatives
share a stem; all images remain untouched.

Initial mutation support covers writable ordinary filesystem folders on desktop,
app-owned downloaded/imported folders on mobile, and local iOS folders with a
live write-capable security scope. Reject symlink traversal, hard-linked media,
path escape, special files, and unsupported atomic-rename semantics. Archived
charts and Android SAF/content-provider sources remain visible with an explicit
"Extract/import to a writable local folder first" reason. Do not rewrite archive
containers, mutate cloud/provider URLs as filesystem paths, or silently copy an
entire library to enable management. Existing extraction/import flows provide
the preparation route. Automatic downloads operate on their private extracted
staging tree and therefore use the same local-folder engine.

Use bounded reads and cancellable directory traversal, without decoding whole
songs or videos during inventory. A corrupt asset produces a per-file reason;
an unreadable folder produces an unavailable measurement and an explicit skip
for that scope, rather than a misleading zero-byte result.

## File changes, recovery, and lifecycle

Chart bytes and paths never change. Their MD5/SHA256 identity, scores, replays,
favorites, playlists, and download verification remain valid. Replacement is
entirely at the resource layer and relies on the app's established extension
fallback; compatibility with every external BMS player is not guaranteed.

Each file is a separate recoverable transaction, not an all-or-nothing batch:

1. Capture source identity and hash, selected-folder containment, destination,
   and policy. Recheck source/destination and inventory freshness before commit.
2. Encode into job-owned private temporary storage; validate it and compare size.
   Limit work to one candidate at a time. Check available space before encoding
   and handle mid-write disk-full errors without retiring the source.
3. Write a durable transaction record outside user-visible Documents. Transfer
   the validated candidate to an exclusive, hidden same-volume installation
   temporary file if needed, flush it, and revalidate its identity. Only this
   bounded installation temporary and a recovery backup may reside beside assets;
   caches, download staging, and transaction records stay private.
4. For extension changes, publish without clobbering a destination, then retire
   the original. For same-path changes, retain a recovery backup before publishing
   the candidate. Record transitions durably so interruption between renames is
   recoverable. Use the existing atomic-file durability approach where suitable;
   do not load video bytes into `atomic_file::write`'s in-memory payload API.
5. Count reclaimed bytes only after obsolete originals/backups are removed.
   Cleanup failure is a warning with retained-byte accounting, not a successful
   space-saving claim. Failed installs restore the original where needed.

Deletion uses the same journal: move each eligible file to a same-volume recovery
name, record the committed decision, then unlink it. On startup, recover pending
transactions before library access: restore uncommitted deletions/replacements;
finish cleanup of committed ones only when their recorded file identities match.
Never delete an unrelated file that appeared at a recorded path. Unknown or
corrupt records preserve files and surface recovery-required status. Recovery
does not resume unfinished encoding jobs or provide a user-facing recycle bin.

Manual execution first obtains an application-thread maintenance reservation:
refuse admission until existing library mutations finish, stop and join preview/music
preparation, release loaded media, and refuse admission while gameplay/export is
active. The reservation prevents new in-app playback/import/scan/export mutations
until the worker finishes and affected-path refresh is handed to the serialized
library task service. Playback starts only from fresh preparation afterward. Android app-owned Documents also
uses the existing `DocumentsMutationGuard` reservation through a narrow JNI
bridge. External filesystem editors cannot be globally locked; detect changes
before commit and skip stale inputs rather than promising cross-process isolation.

Workers publish immutable, bounded progress snapshots to the UI. Cancellation,
shutdown, profile switch, or background suspension must leave the current file in
a recoverable state; no UI pointers are retained by workers. Mobile backgrounding
requests cancellation at checkpoints rather than promising background encoding.
Download processing uses the same single-conversion admission limit, honors the
existing foreground-performance pause signal, and mutates only isolated staging.
It does not need the manual library-wide playback reservation.

After manual completion or partial cancellation, release stale asset-resolution,
decoded-image, and decoded-audio state, request affected-path library refresh,
and invalidate storage measurements. Preserve chart selection and playlist
identity; do not rebuild score databases or clear unrelated caches. The current
progressive music source owns transient rendered PCM, so it must be stopped and
discarded before manual mutation, not treated as a persistent cache to rewrite.
Refresh inventory evidence after each known committed change, ignoring only
owned transaction temporary names. A later unexpected directory or chart change
still invalidates preflight; the job must not mistake its own earlier conversion
for an external change or silently accept an unrelated external edit.

## Download integration

Persist the opt-in under Settings' library/download section and show its policy
summary in Find BMS. Capture options for both direct chart lookup and selected
candidate downloads from both built-in and skinned selectors.

For an effective non-no-op policy:

1. Download using the existing transport/retry flow.
2. Force extraction even if Skip unarchiving for non-solid archives is enabled;
   explain that effective override in the UI without changing the saved archive
   preference. Do not transform the compressed archive in place.
3. Verify the original extracted chart hash through `decideExtractedArchive`.
4. Inventory and optimize the private extracted staging tree with the captured
   policy, including all nested eligible media rather than only resources of the
   chart used for hash verification, then verify chart-file identities are unchanged.
5. Commit through `commitArtifact`, then use the existing `filesReady` and
   downloaded-index path. No library registration occurs before optimization
   completes or is explicitly skipped because of per-file failures.

Hash mismatches/inconclusive verification never enter automatic transformation.
The existing Keep Files/Delete Files flow remains intact; an explicitly kept
mismatch is imported unchanged and can later be managed manually. This avoids
destroying evidence before the user decides what to keep.

Codec, format, or per-file I/O failures retain the source asset and allow a
successful download with a structured optimization warning and counts. A failed
rollback or unresolved recovery state is fatal to committing that staging tree.
Cancellation cancels the download attempt, safely cleans owned temporary data,
and never commits a partially processed staging tree after cancellation. A retry
uses the original captured policy. Download policy changes affect later attempts.
Repeated downloads apply policy to the new staging tree, using existing artifact
replacement rules; they do not optimize unrelated installed packages.

## Component boundaries

| Component | Responsibility |
| --- | --- |
| `StoragePolicy` | Typed options, defaults, validation, effective/no-op policy, codec/cap decisions |
| `ChartResourceInventory` | Recursive media enumeration, collision checks, measurements, eligibility |
| Shared chart asset resolver | Preserve current audio and visual lookup order for engine and inventory |
| `AudioTranscoder` / `BgaTranscoder` | Bounded decode/encode/verification into temporary output; never remove sources |
| `StorageFileTransaction` | Durable commit, rollback, recovery, collision/identity checks, byte accounting |
| `StorageOptimizer` | Synchronous cancellable inventory/transform loop shared by manual and download workers |
| `StorageTask` / maintenance guard | Manual worker ownership, immutable snapshots, app/platform admission and lifecycle |
| Shared grouped presentation / row view | Reusable group identity, search, expansion and representative chart appearance |
| `StorageManagementScene` / policy view | Group selection, measurements, preflight, settings, progress and result UI |
| Download adapter | Force extraction, verify first, invoke optimizer, retain warnings, commit/index afterward |

Do not couple encoding to a scene, SQLite connection, music playback queue, or
shell process. Reuse existing repository queries rather than creating a second
grouping SQL implementation. Shared row extraction must preserve music-player
favorites, queue controls, scroll behavior, and asynchronous artwork lifetime.

## Acceptance and verification

- Small real codec fixtures prove exact FLAC samples, playable Vorbis with
  preserved timing, and MPEG-1 to smaller H.264 at original dimensions/FPS.
- Cap tests cover portrait/square/odd/SAR dimensions, 30000/1001 and 60000/1001,
  VFR gaps, final duration, embedded audio, and no upscaling/frame duplication.
- Inventory tests cover Unicode and mixed-case paths, nested/overlapping folders,
  unindexed charts, preserved images, alternate-extension collisions, path escape,
  unreadable roots, and virtual sources. Malformed chart text must not affect
  enumeration; no declaration parser or reference scan is introduced.
- Folder-scope tests use a representative chart and a sabun with disjoint audio
  and video references, plus nested and unreferenced media. All eligible files
  must enter compression once, independent of representative choice, indexing,
  or UI expansion. Deletion must remove all supported videos, including
  unreferenced videos, while retaining every image and audio file.
- Fault injection covers encode failure, disk full, rename/unlink failure,
  source mutation, cancellation at each commit boundary, and startup recovery.
  No failure may leave a previously playable chart without its original or a
  validated replacement; intentional confirmed BGA deletion is the exception.
- Download workflow tests prove verify → optimize → commit → index ordering,
  archive-option override, no-op/off equivalence, warning propagation, unchanged
  mismatch handling, cancellation, and immutable policy snapshots.
- Runtime tests exercise manual admission versus music/preview preparation,
  gameplay, exports, library scans, Android document writes, backgrounding, and
  scene destruction. UI tests verify group selection and retained music behavior.
- Use `area_library`, `area_audio`, `area_video`, `area_downloads`, `area_settings`,
  and `area_ui` tests proportional to changes. Keep fixtures tiny, use injected
  faults/clocks, and avoid network downloads or developer-library mutations.
- Compile `main` in `cmake-build-debug` at `-j 6`. Before claiming cross-platform
  readiness, verify linked codec capabilities and compile on iOS/Android/Windows
  through the established build paths. No distribution command is authorized.

## Scope limits

No HEVC/AV1 encoder, archive repacking, SAF mutation implementation, cloud-library
migration, unused-media cleanup, whole-library backup, durable resumable encoding
queue, or automatic historical-library conversion in this release. No parser
behavior change is planned. If implementation discovers one is necessary, use
`../bms-parser-cpp` and its required test/amalgamation workflow instead of editing
generated parser sources here.
