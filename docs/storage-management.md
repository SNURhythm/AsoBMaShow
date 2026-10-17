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
only after full decode validation and only if strictly smaller. Ambiguous
same-stem alternatives or existing destinations are skipped, never overwritten.

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
downloads/imports and accessible writable iOS folders. Archives and Android
SAF/content-provider sources must first use the existing extraction/import flow
to become local folders. Storage management does not rewrite archive containers
or copy a library automatically. Symlinks, hard links, special files, unsafe paths
and unsupported filesystem operations are rejected.

## Progress, cancellation and recovery

Progress separates scanning, encoding, verification and commit. Manual work stops
music/preview readers and reserves library maintenance; competing playback or
library operations cannot start during the mutation. Back requests cancellation
and returns after the worker releases maintenance. Mobile backgrounding cancels
manual work at a safe checkpoint.

Each file commits separately. Cancel keeps already completed changes and retains
uncommitted originals; it does not roll back the entire batch. Output is staged
privately, validated, then installed using a journal and same-volume temporary
files/backups. Startup recovery checks interrupted transactions before admitting
new work. Do not manually remove recovery files while an operation is pending.

The result includes converted, deleted, skipped and failed counts. Reclaimed
bytes are counted only when obsolete originals/backups have actually been removed.
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

Verification for this implementation used the desktop grouped suites and a real
disposable-library conversion, plus the iOS and Android build-only paths and native
codec/file transaction tests on iOS 27.1 Simulator and the Android 10 emulator.
The desktop music UI was inspected, but
that build reports native music playback unavailable. Windows build and filesystem
execution were not available in this session; they remain release validation work.
