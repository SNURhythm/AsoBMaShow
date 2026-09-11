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
