# iOS First-Release Stabilization Design

**Date:** 2026-08-01

**Branch:** `agent/release-readiness-audit-2026-08-01`

**Baseline:** `develop` at `868c091b`

**Status:** Approved and implemented; Firebase iteration policy amended 2026-08-01

## Purpose

Prepare AsoBMaShow for its first iOS release by fixing the confirmed shared and
iOS-relevant correctness, security, memory, decode, and release-engineering
findings recorded in `docs/release-readiness-audit-2026-08-01.md`.

The first release remains version `0.0.1` with an iOS/iPadOS 14 minimum. The
implementation must retain `NSAllowsArbitraryLoads` because difficulty-table
loading needs to support HTTP sources. Authenticated Internet Ranking (IR)
traffic is subject to a narrower HTTPS requirement described below.

## Scope

This stabilization pass covers:

- R3: required verification before iOS distribution;
- R4: bounded artwork memory and stale asynchronous decode work;
- R5: iOS Keychain storage and HTTPS-only authenticated IR traffic;
- R7: correct handling of failed chart-migration commits;
- R8: serialized, event-safe iOS distribution and build numbering;
- R9: bounded and lazy BGA/video-player construction;
- R10: correct YUV420 layout and FFmpeg end-of-stream draining;
- iOS dependency deployment-target alignment and release artifact checks; and
- iPhone/iPad simulator smoke and focused performance verification.

Android-only and macOS-only release findings remain documented but deferred.
No Android or macOS release claim is made by this work.

## Explicit Product Decisions

- Keep `MARKETING_VERSION` at `0.0.1`.
- Keep the app minimum at iOS/iPadOS 14.
- Align CocoaPods, SDL2, and SDL2_ttf with iOS 14 instead of their older
  deployment targets.
- Keep `NSAllowsArbitraryLoads = true` for difficulty-table and other
  user-directed anonymous content loading.
- Require HTTPS only when an IR operation includes credentials or private
  account/score data.
- Keep the approved credential and transport hardening.
- Do not add `PrivacyInfo.xcprivacy` manifests in this pass. The omission is an
  explicitly accepted release risk, even though the binaries import Apple
  required-reason APIs.
- Treat PR-to-`develop` Firebase distribution as a fast iteration track: do not
  schedule the release verifier for that event and do not make Firebase depend
  on it. TestFlight remains the release track and must continue to depend on
  the full verifier.
- Do not deploy or upload a build while implementing or verifying this work.

## Architecture

### 1. iOS Credential Storage and Migration

`IrCredentialStore` remains the platform-independent credential contract. Its
storage operations will be routed through an injected credential backend so
the existing validation, redaction, and profile-facing behavior can be tested
without invoking platform services.

On iOS, the backend stores one API key per profile/provider pair in Keychain.
The service/account identity must be stable, namespaced to AsoBMaShow, and must
not expose the key itself in logs or diagnostics. Keychain items use
`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly` so background-capable app
work can read them after the first unlock while preventing migration to another
device through a backup.

The existing plaintext `ir-credentials.json` file is a legacy migration source,
not a fallback store:

1. Load and fully validate the legacy document using the current schema and
   limits.
2. Write every valid provider key to Keychain.
3. Read back or otherwise verify every write.
4. Only after all keys are verified, remove the legacy file and its backup
   artifacts.

If any step fails, leave the legacy file intact for a later retry and disable
authenticated IR for the current session. Do not continue by reading the
plaintext key from the legacy file after a partial migration. This prevents a
split source of truth and avoids deleting the only recoverable copy.

Profile export, import, and duplication continue to omit credentials. Deleting
a profile removes all Keychain items belonging to its stable profile identity.
Removing a provider key deletes only that provider's Keychain item. Missing
items are idempotent success; malformed or inaccessible items fail closed with
redacted diagnostics.

Non-iOS platforms retain the current file-backed implementation for this
iOS-first release. The injected backend boundary prevents the iOS change from
silently altering those platforms.

### 2. Authenticated IR Transport Policy

Origin parsing continues to accept normalized HTTP and HTTPS origins so public,
anonymous ranking requests and existing configuration remain representable.
The release build enforces the transport rule at two boundaries:

- settings/UI validation prevents enabling or saving authenticated IR behavior
  against an HTTP origin; and
- the request layer independently refuses to attach a bearer token, submit a
  score, reconcile account history, or fetch private account data over HTTP.

An existing profile configured with an HTTP IR origin remains visible. Public
anonymous operations may continue, but authenticated controls are disabled with
a corrective explanation until the origin is changed to HTTPS. No credential
or score payload may be transmitted in this state.

This policy deliberately does not narrow App Transport Security globally.
`NSAllowsArbitraryLoads` remains enabled so difficulty tables and other
user-directed anonymous resources keep working.

### 3. Bounded Artwork Pipeline

The artwork pipeline will use a byte-budgeted least-recently-used cache rather
than the process-lifetime `ImageView::imageCache` map. The iOS decoded-image
budget is 64 MiB. Accounting includes every ready decoded RGBA result, including
results waiting for a view to consume them. GPU textures owned by visible views
are not double-counted as CPU cache bytes.

Images are decoded/downsampled to the requested render size rather than always
retaining full source resolution. The cache key includes the normalized source
identity and decode dimensions so a thumbnail cannot be mistaken for a
full-resolution image.

Each `ImageView` asynchronous bind owns a monotonically increasing request
ticket. Rebinding, `freeImage()`, or destruction releases interest in the old
ticket. Queued work with no consumers is removed; already-running work may
finish, but its result is discarded unless another consumer joined the same
deduplicated request. A stale completion can never replace a newer image.

iOS uses two total artwork decode workers shared by normal and priority work.
Priority changes scheduling order, not thread count. Deduplication allows
multiple views to share one decode without giving a stale view ownership of the
result.

On `SDL_APP_LOWMEMORY`, the app evicts all unpinned artwork, discards completed
work with no live consumer, cancels queued orphan work, and releases idle BGA
resources. Visible/active resources remain pinned long enough to render the
current screen safely.

### 4. Lazy, Bounded BGA Playback

Chart loading records visual descriptors, event timing, media identity, and
duration metadata. It does not construct a `VideoPlayer` for every referenced
video.

At runtime, the jukebox owns independent base and layer playback slots. A hard
limit of three materialized `VideoPlayer` instances applies on iOS:

- one active base slot;
- one active layer slot; and
- one next-event prefetch slot.

Duplicate BGA IDs share canonical media identity, probe metadata, and static
image cache entries, but never share mutable player/playback state. Replacing a
slot returns its player resources to the bounded pool or destroys them.

A single visual-loader worker materializes the prefetch candidate selected from
the next relevant event. Seeking invalidates obsolete prefetch, immediately
prioritizes the target base/layer media, and must not block or reset audio
playback. Missing or malformed visuals log a bounded diagnostic and leave audio
running.

Each video player buffers at most three decoded frames. The recycled-frame pool
holds at most two frames. Static BGA images use their own byte-budgeted LRU;
active base/layer images are pinned, while inactive entries participate in
eviction.

### 5. Correct Video Decode State Machine

YUV420 plane layout is centralized in a small checked helper. Luma uses the
source width and height; each chroma dimension uses ceiling division:

`chroma = (dimension + 1) / 2`

Pitch and byte-count multiplication must be overflow-checked before allocation
or texture upload. The same layout values drive FFmpeg plane copies and bgfx
texture creation so odd dimensions cannot under-allocate or disagree between
producer and consumer.

The predecode loop becomes an explicit packet/decoder state machine:

- drain every available decoded frame before reading/sending a new packet;
- retain an unsent packet when `avcodec_send_packet` returns `EAGAIN`;
- pause on a full frame buffer without dropping packet or decoder state;
- on demux EOF, send a null packet exactly once;
- continue receiving delayed frames until `AVERROR_EOF`; and
- mark player EOF only after the decoder is fully drained.

Seeking flushes the codec and frame buffer, clears pending packet/drain state,
resets EOF, and resumes decode from the requested timestamp. Existing timestamp
and audio synchronization semantics remain unchanged except for recovering
frames that were previously lost.

### 6. Chart Migration Commit Correctness

The chart metadata rebuild migration treats `RELEASE
chart_metadata_rebuild_migration` as the commit boundary. If it fails, the
repository performs best-effort savepoint rollback/cleanup, returns false, and
does not:

- log migration success;
- bump the library revision;
- mark the migration complete; or
- expose partially committed readiness to callers.

A narrow test-only fault seam will fail the release boundary while exercising
the real repository and SQLite file. Tests assert external persistent state,
not the seam itself: schema/version/rebuild markers and library revision remain
unchanged, and a later clean retry succeeds.

### 7. iOS Build and Distribution Boundaries

The iOS project, Podfile, SDL2, and SDL2_ttf deployment targets are aligned to
iOS 14. `scripts/ios_init.sh` remains the idempotent authority for generated or
third-party Xcode project adjustments, and `tests/ios_build_setup_tests.py`
locks those adjustments down.

Fastlane responsibilities are separated by intent:

- verification builds and inspects without upload;
- Firebase distribution retains its explicit pull-request-to-`develop` path;
  and
- TestFlight distribution runs only for its explicitly selected push/tag/manual
  events, never as the default branch of ambiguous event detection.

TestFlight depends on the required test/build gate and uses a workflow
concurrency group with `cancel-in-progress: false`. Serialized TestFlight jobs
may continue using `latest_testflight_build_number + 1` without selecting the
same build number concurrently. Firebase PR distribution intentionally skips
the verifier for fast iteration and continues to use the GitHub run number. The
public version remains `0.0.1`.

The workflow must not upload from the verification path. Local verification
uses `scripts/ios_firebase_deploy.sh --build-only`; a real deployment remains an
explicit user action under the repository instructions.

### 8. Release Artifact and Metadata Gate

A deterministic IPA/app inspection script verifies at least:

- `CFBundleShortVersionString` is `0.0.1`;
- minimum OS is iOS 14;
- the archive was built with an App-Store-supported iOS SDK/toolchain;
- the main executable and embedded frameworks have the expected architectures;
- the bundle identifier, device families, icon, usage descriptions, file
  sharing, document behavior, and ATS exception match intentional settings;
- code signatures, entitlements, and embedded framework signatures verify;
- every non-system dynamic dependency resolves inside the bundle or to an Apple
  system path;
- credentials, private environment files, temporary files, and unwanted symbol
  artifacts are absent; and
- privacy manifests are not required by this repository gate, per the explicit
  accepted-risk decision above.

The privacy policy is updated to describe iOS Keychain storage, device-only
accessibility, legacy migration, and HTTPS-only authenticated IR. The release
checklist retains App Store Connect tasks that cannot be proven from the repo:
privacy-label answers, current age-rating questions, support/privacy URLs,
encryption/export-compliance answers, content-rights review notes, screenshots,
and physical-device smoke results.

## Failure Handling

- Credential failures disable authenticated IR for the session without logging
  secrets or deleting the legacy recovery source.
- HTTP authenticated requests are rejected before constructing or dispatching a
  request containing a bearer token or score payload.
- Artwork and BGA failures degrade visuals while audio and navigation continue.
- Stale asynchronous work is discarded by ticket identity.
- Decode corruption or layout overflow fails that visual safely.
- Database transaction-boundary failure prevents application readiness from
  being reported as successful.
- TestFlight distribution never begins if required verification fails.
  Firebase PR distribution intentionally bypasses that release gate.

## Test Strategy

Implementation follows test-driven development with a failing regression test
before each production change.

### Credential and Transport Tests

- backend contract tests for save/load/replace/remove and redacted failures;
- legacy-file migration success, partial failure, retry, and deletion ordering;
- profile deletion, duplication, import, and export credential behavior;
- HTTP anonymous request allowed where intended;
- HTTP bearer/private/score operations refused at settings and request layers;
- HTTPS authenticated operations unchanged; and
- real iOS Simulator Keychain save/read/delete validation.

### Artwork Tests

- exact byte accounting and LRU order at/over 64 MiB;
- downsampled cache keys and decoded dimensions;
- request deduplication, priority ordering, rebind/destruction cancellation,
  stale completion discard, and bounded queue/worker count;
- visible-entry pinning; and
- low-memory eviction of unpinned/cache/idle resources.

### Jukebox and Video Tests

- descriptor-only chart setup and zero eager player construction;
- maximum three materialized players and one loader worker;
- independent base/layer state for duplicate media IDs;
- bounded three-frame buffer and two-frame recycle pool;
- event timing, prefetch replacement, seek targeting, and uninterrupted audio;
- odd-width/height YUV420 plane sizes and uploads;
- packet `EAGAIN` retention and full-buffer resume;
- delayed/B-frame EOF drain; and
- seek after EOF.

### Repository and Release Tests

- failed chart savepoint release leaves durable state/revision unchanged;
- clean retry commits once;
- iOS project setup remains idempotent and every deployment target is 14;
- workflow event routing proves Firebase PRs bypass verification while
  TestFlight remains dependent on it;
- IPA audit positive and negative fixtures cover every enforced check; and
- existing CTest and iOS setup suites remain green.

## Runtime Verification

After automated tests pass:

1. Run the unsigned iOS build-only path; do not upload.
2. Build and launch the exact simulator app using the iOS debugger workflow.
3. Smoke first launch, profile creation/migration, library navigation, artwork
   scrolling, chart launch, BGA playback, seeking, background/foreground, memory
   warning handling where injectable, and IR HTTP/HTTPS states.
4. Repeat core layout/navigation smoke on both an iPhone and iPad simulator.
5. Capture focused, symbolicated ETTrace runs for artwork-heavy library
   scrolling and BGA-heavy playback/seek. Record exact flow, simulator/runtime,
   processed flamegraph artifacts, first-party hotspots, and caveats.
6. If local signing credentials are available, build and inspect a signed
   archive without invoking a distribution/upload lane. Otherwise, run the
   artifact gate against the unsigned release app and record signature/export
   verification as pending.
7. Record physical iPhone/iPad smoke as a manual release gate if hardware is not
   available in this workspace.

## Completion Criteria

This stabilization pass is complete when:

- all scoped regression and existing test suites pass;
- the iOS build-only path succeeds with deployment targets aligned to 14;
- simulator smoke passes on iPhone and iPad targets;
- cache, player, frame, queue, and worker bounds are proven by tests;
- ETTrace shows no new dominant first-party hotspot in the two focused flows;
- the release workflow cannot upload to TestFlight before verification or race
  TestFlight build numbers, while Firebase PR iteration stays independent;
- the artifact audit passes a locally produced release candidate;
- the privacy policy and release checklist match shipped behavior; and
- deferred Android/macOS findings remain clearly marked as outside the first
  release claim.
