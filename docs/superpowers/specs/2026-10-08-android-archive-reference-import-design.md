# Android archive import choices and SAF references

Status: implemented and verified on 2026-10-08.

## Intended behavior

The user wants non-solid archives to remain at their original location when
desired, avoiding both a second archive copy and extracted assets. The user
also explicitly requires a choice between keeping an archive and extracting it.
Solid archives must explain that extraction is required.

Inspect the selected archive before starting a full copy or extraction.

| Inspection result | User-facing action |
| --- | --- |
| Non-solid, supports direct reads and durable access | “Use archive” / “Extract” / “Cancel” |
| Solid | Explain “This is a solid archive. Extract it before playing.” Offer “Extract” / “Cancel”. |
| Non-solid but provider cannot support durable direct reads | Explain why direct use is unavailable. Offer extraction or cancellation; transient share grants may require selecting the file through Import Archive to enable direct use. |
| Corrupt, unreadable, or inspection cancelled | Report the inspection error or cancellation. Do not register a usable archive or silently extract it. |

“Use archive” explains that the original file must remain accessible. “Extract”
uses the existing extraction workflow and its progress, pause, cancellation,
space checks, and safe destination handling. Neither action removes the original
external archive. The choice applies to each import, with no new global setting.

## Baseline before this change

- `AsoBMaShowActivity.startNextPendingImportCopyLocked` copies every archive URI
  into the private `archive_imports/inbox` directory.
- `ChartLibraryOperations::runAndroidImport` then extracts every staged archive
  and deletes its temporary copy after successful indexing.
- The archive library already reports solid entries and can read non-solid
  archives directly, but its readers and scanner expect ordinary filesystem
  paths. They cannot yet use a persisted document URI as an archive source.
- Existing Android archive-picker results intentionally retain only transient
  grants. Direct references need durable read permission instead.

## Source access design

Use one small source-access adapter under the existing archive readers. It
resolves a stable logical archive identity to an owned readable source. Local
archives continue using their ordinary filesystem path. Android references
resolve through a persisted SAF URI and a seekable file descriptor.

This preserves the existing ZIP, RAR, 7z, and libarchive decoding behavior instead
of implementing new format-specific readers. Copying an archive and merely
skipping extraction is insufficient: it still duplicates the archive on disk.

Persist a stable reference ID, display filename, and document URI in the private
library metadata. Never persist a numeric file descriptor or a `/proc/.../fd/...`
path as chart identity. Re-register references before scanning or opening saved
charts after restart. The logical archive filename retains the real extension
for existing format selection and virtual member paths.

Open the SAF descriptor once per active archive access and share it across
inspection and batched asset reads. Keep a bounded cache of idle descriptors;
active reader leases prevent eviction from invalidating an in-flight read.
Parallel readers must use independent offsets or positional reads, rather than
sharing a seek cursor through `dup`.

Validate seekability and compatibility with the reader adapter before offering
direct use. SAF can return pipes or proxy descriptors; repeated read latency is
provider-dependent. The performance guarantee is avoiding repeated document
resolution/open calls for members of an active archive, not guaranteeing local
storage latency for every provider.

Source size, modification state, and archive cache identity must describe the
underlying archive, not its reference record. Failed access must remain an
unavailable source; it must not make a healthy library refresh delete its chart
records. Source changes invalidate the existing archive index and asset caches.

## Import and lifecycle integration

1. Retain the incoming URI and grant information. Open and inspect the source
   off the UI thread, respecting import cancellation and gameplay pauses.
2. Show the appropriate choice on the UI thread. Activity destruction or dialog
   cancellation releases the waiting worker and provisional source resources.
3. For direct use, obtain and verify persistent read permission, store the
   reference, and index it through the existing library task service. Do not copy
   the archive or extract its assets. Small metadata and existing index caches
   remain permitted.
4. For extraction, reuse the readable source when supported. If a provider
   supplies only a stream, retain the existing bounded staging-copy fallback.
   Register the extracted destination only through the existing task workflow.
5. Cleanup distinguishes owned temporary files from referenced external files.
   Failure or removal never deletes an external source. Release a newly acquired
   URI grant only when no retained library reference needs it; preserve existing
   grants used by other references.

Keep Android-specific registration and permission handling outside iOS paths.
The shared archive adapter's default remains ordinary local-file access.

## Verification

- Real non-solid ZIP/RAR/7z fixtures offer both choices and can be indexed and
  read through a descriptor-backed source without copying archive bytes.
- A solid fixture offers extraction only, with the required explanation.
- Repeated member reads share the source open; concurrent reads have independent
  positions; descriptor eviction and teardown do not invalidate active readers.
- Persisted references survive app restart and preserve stable chart identity.
- Missing, revoked, changed, non-seekable, and transient-only sources take the
  documented failure or extraction path without deleting existing library data.
- Cancel during inspection, choice, indexing, or extraction; destroy the Activity
  while a choice is pending; verify cleanup and original-file preservation.
- Exercise both choices on Android and check storage usage for direct import.
- Run relevant native and Java behavioral tests, the desktop build, Android
  release build-only verification, and the iOS verification script for shared
  archive-reader changes. No distribution upload is part of this task.

Reference: Android `ContentResolver.openFileDescriptor` and persisted URI grants:
https://developer.android.com/reference/android/content/ContentResolver

## Follow-up: direct local access

The user requested research and application of a more efficient alternative to
SAF. Known local files now open directly when the app already has durable file
access (app-owned storage, file URI, or the existing all-files-access flavor and
grant). External-volume paths come only from the system external-storage
provider, checked against the real volume root. Other providers and failed direct
opens retain the descriptor-backed SAF fallback. Direct local access does not
require an additional persisted URI grant. Both routes reuse one cached source
open and independent native reader cursors.

This removes provider calls where possible; it does not claim faster payload I/O
on every device. Android's FUSE implementation can make raw shared-storage paths
slower than provider-opened descriptors, particularly for random I/O. No new
storage permissions or file copying are introduced.

Sources checked 2026-10-08:
- https://developer.android.com/training/data-storage/manage-all-files
- https://source.android.com/docs/core/storage/scoped

## Verification results — 2026-10-08

- Relevant native/Android CTest selection: 23/23 passed. Release-critical iOS
  selection: 66/66 passed. Android release workflow checks: 20/20 passed.
- The final localization audit now includes Java message references; catalog
  and Android import checks passed after replacing constructed keys with explicit IDs.
- Desktop main, Android restricted-file-access release APK, and unsigned iOS
  release build succeeded; iOS artifact audit passed.
- Android 10 emulator: a 4,283,023-byte ZIP imported through the document picker
  with “Use archive”; no assets were extracted and Documents usage grew by about
  60 KiB of metadata. Chart loading/gameplay worked before and after app restart.
- Removing that library reference released its persisted read grant. The original
  ZIP remained unchanged. A transient share offered extraction/cancel only;
  extraction completed and preserved the source.
- Non-solid and solid 7z fixtures both extracted successfully. The solid fixture
  displayed the required extraction explanation. A non-solid 7z file in app-owned
  storage offered direct use without a URI grant, persisted a file URI reference,
  and remained indexed after restart.
- Emulator testing exposed missing Android 7-Zip registration objects. Explicit
  archive/codec object linking fixed the reproduced runtime failure; the workflow
  regression check failed before the fix and passed afterward.
- Host reader tests cover real ZIP/RAR/7z references. An Android native descriptor
  test covers independent cursors and active leases. Java tests cover direct-path
  policy, transient/stream sources, cancellation, and owned grant cleanup.
- The Android 11+ all-files-access branch has policy/unit coverage; a physical
  all-files-access build and remote/cloud provider performance were not measured.
  No distribution upload was performed.
