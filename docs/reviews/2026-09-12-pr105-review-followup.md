# PR 105 review follow-up

This pass addresses the ten outstanding review threads at `9ec047f0`.

## Ownership and recovery

- Every full-unzip destination, including the hashed fallback after 100 occupied
  names, must be unused or have a matching completed ownership marker. Creation
  of a new output directory is exclusive; an occupied candidate is never removed.
- Root names reserved for incomplete, complete, and temporary completion markers
  are rejected before output admission, including case and trailing-dot aliases.
  Ordinary nested files with those basenames remain supported.
- Markers and returned results retain the extracted source identity. Deletion
  verifies the file ID and change time as well as the normalized path, size, and
  modification time. Same-size replacements and in-place changes that preserve
  mtime are rejected. This is a metadata identity check, not a cryptographic hash
  or an atomic compare-and-delete against a hostile concurrent filesystem writer.
- Recovery removes only partial folders whose nonsymlink ownership marker matches
  the journal. Missing, legacy, torn, or unowned markers leave recovery pending.
  Both complete and incomplete parsing are bounded and normalize iOS container
  paths. A failed cleanup preserves the journal instead of orphaning its output.

## Resource bounds

- Full extraction admits at most 100,000 entries per archive and 1,000,000 across
  a batch by default. Empty files and explicit and implicit directories count.
  Index building and restored indexes respect the per-archive ceiling. Admission
  precedes extraction, shares one synchronized batch counter, and is not charged
  again for completed-folder reuse. Exhaustion stops the batch. This preserves
  prior completed outputs rather than introducing all-or-nothing batch semantics.
- Serial/single-entry supported ZIP extraction uses the same 64 KiB direct-to-file
  reader as parallel extraction. The generic fallback delivers one bounded member
  at a time, limited to the smaller of 64 MiB and the per-archive memory allowance;
  an oversized fallback member fails closed before full allocation.
- Bounded asset reads cover single-file, single-worker, concurrent, and fallback
  paths. The loader divides its encoded-byte allowance between extraction and its
  consumer queue. Oversized buffers cannot bypass admission when a queue is empty.
  Solid SDK reads remain one extraction pass. Decoder dictionaries, archive-index
  metadata, and decoded audio are not part of this encoded-payload bound.
- ZIP integrity failures are terminal. Mixed supported/unsupported ZIP compression
  is preflighted before callbacks, so fallback cannot emit and charge files twice.
  A corrupted cached path now fails full extraction rather than marking a partial
  subset complete and making the source eligible for deletion.
- Bounded readers retain the existing extraction-pass diagnostics, including the
  single-pass sound/visual reload contract for solid archives.

## iOS download capacity

Downloads check temporary-volume capacity before creating/resuming a session,
on response/progress, and at completion. Known remaining bytes exclude bytes
already persisted. Unknown-length responses still undergo the free-space checks.
Cross-volume staging checks the additional destination copy before and during
bounded writes. Capacity-query failures fail closed; the reserve is 512 MiB.

`NSURLSessionDownloadTask` reports progress after periodic writes. These checks
cancel a download when the reserve is reached; they are not an absolute disk-space
reservation against writes between callbacks or unrelated processes.

The existing release-0.0.1 policy intentionally omits privacy manifests; this
review pass does not change that policy or its release-contract tests. Before
App Store submission, the release owner must reconcile it with Apple's
[required-reason API rules](https://developer.apple.com/documentation/bundleresources/app-privacy-configuration/nsprivacyaccessedapitypes/nsprivacyaccessedapitype),
including disk-space reason `E174.1` and file-metadata reasons `C617.1`/`3B52.1`
for app-container and explicitly selected files. Passing local build checks is
not an App Store compliance clearance.

## Replay and course compatibility

Course preparation rejects empty definitions and more than the shared 256-stage
limit before parsing or fact allocation; exactly 256 stages remain supported.

Completed replay capture retains the recorded abort timestamp, including negative
pre-roll time. Only explicit aborts may have negative completion, within configured
replay limits. Post-abort input, touch, or lane-cover evidence is rejected rather
than extending the terminal time. Codec, materialization, and Watch use the same
signed terminal fields in schema 4; no format bump is required. Older builds that
reject all negative completion values cannot play these newly preserved pre-song
aborts. Ordinary positive-completion and legacy compatibility coverage remains.

## Verification

Behavioral regressions were reproduced before their fixes. Focused suites cover
ownership/collision, cancellation/recovery, same-size source replacement, quota
boundaries, lying size metadata, mixed ZIP compression order, bounded allocation,
download lifecycle, course boundaries, and abort replay round-trips/Watch.

Final verification on 2026-09-12:

| Check | Result |
| --- | --- |
| `cmake --build cmake-build-debug -j 6` | All targets, including `main`, pass |
| `ctest --test-dir cmake-build-debug --output-on-failure -j 6` | 357/357 pass, 102.31 seconds |
| `IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh` | 66/66 native tests and 88/88 Python contract tests pass; unsigned arm64 iOS build and artifact audit pass |
| `scripts/android_firebase_deploy.sh --build-only` | `firebaseRelease` APK builds successfully, including release lint/signing validation |
| `git diff --check` | Pass |

The full suite exposed a missing bounded-reader diagnostic, reproduced by the
existing jukebox extraction-pass test and fixed before the final green run. The
iOS release contracts also caught an unsolicited privacy-manifest addition; it
was removed rather than weakening the existing intentional-omission policy.

No distribution upload or deployment was performed. The iOS artifact is unsigned;
Windows-native compilation and physical-device runtime checks were not run.

## Second review follow-up

The two additional findings at `446b69f6` concern single-archive recovery and
decoded sound reuse after archive replacement.

- Single extraction now uses the same operation lock and durable ownership
  journal as batch extraction. A journal-write failure prevents output creation;
  cancellation and extraction/indexing failures retain the recovery row. Only
  successfully indexed, readable output is acknowledged. Startup recovery can
  clean owned partial output, retain completed output for indexing, and retry
  without deleting the source archive.
- Decoded sounds use the existing file-ID/change-time source identity, not only
  size and mtime. The archive index and cached 7-Zip reader validate the same
  identity, so reordered members cannot redirect a sound read through stale
  entry positions. Unavailable identities are never treated as cache hits.
- Disk entry indexes advance from version 3 to version 4 and persist the source
  identity. Older indexes rebuild automatically; chart databases, replay formats,
  ownership marker formats, and user data are unchanged. Cache files remain
  keyed by archive path rather than accumulating one file per source generation.
- Regression tests render positive PCM from the original archive and negative
  PCM from its replacement, covering ZIP/7z, reordered entries, in-place changes,
  and file replacement with size/mtime preserved. They also cover hot/cold entry
  indexes and retention of unchanged decoded sounds. Recovery tests exercise
  actual partial writes, cancellation, journal/scan failures, and retry cleanup.

Second-pass verification on 2026-09-12:

- Full desktop build passes. Final parallel CTest is **355/357**: the unchanged
  150 ms scheduler-stop assertion in `foundation_av_jukebox_restore` and a
  disappearing temporary workspace during `foundation_profile_archive_portable`
  fail under the parallel run. These unrelated checks were not weakened or fixed.
- Jukebox, archive concurrency, single/batch unzip operations, unzip modal, and
  profile archive portability all pass **three consecutive isolated runs each**
  (`ctest --repeat until-fail:3 -j 1`, restricted to those five suites).
- An earlier parallel run also hit the existing `visual_catch_up` 40-second
  timeout; that test passes in the final parallel run.
- `IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh` passes: 66 native tests,
  88 Python contract tests, unsigned arm64 device build, and artifact audit.
- `scripts/android_firebase_deploy.sh --build-only` produces the release APK
  successfully, including release lint/signing validation. No upload is performed.
- Read-only follow-up review approves both fixes after strict storage-readability
  regression tests were added. `git diff --check` passes.

Windows-native and physical-device runtime checks remain untested. No release
policy, signing requirement, or distribution action is changed by this follow-up.

## Third review follow-up

Recovery now acknowledges a definitely missing output folder even when the source
archive has also been removed. The existing parent-storage accessibility,
filesystem-error, symlink, and unverified-existing-output guards remain intact.

- Eight regression cases cover source present/absent, keep/delete-original mode,
  and accessible/offline parent storage. They verify retention while storage is
  offline, acknowledgement after restoration, source preservation, and repeat
  recovery. The source-absent case fails before the fix and passes afterward.
- The older output-only rename expectation is removed: with accessible parent
  storage, a renamed output is indistinguishable from a deleted output at its
  recorded path. Invalid completion-marker retention remains tested; the new
  cases cover actual parent-storage unavailability.
- Full desktop build passes, the archive unzip operation suite passes three
  consecutive runs, and parallel CTest passes **357/357** tests.
- Read-only review approves the final delta. `git diff --check` passes.

Mobile builds were not rerun for this follow-up. No deployment was performed.
