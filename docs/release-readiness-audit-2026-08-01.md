# Release-readiness audit — 2026-08-01

- Status: iOS-first automated remediation complete; signed/hardware gates pending
- Branch: `agent/release-readiness-audit-2026-08-01`
- Baseline: `develop` at `868c091b`

## Verdict

**iOS 0.0.1 candidate: conditional go for producing the signed release
archive; not yet cleared for App Store submission.** The automated iOS contract,
test, unsigned-device-build, package-audit, and iPhone/iPad simulator runtime
gates are green. Submission still requires auditing the signed archive and
completing the physical iPhone/iPad and App Store metadata checks in the
versioned release checklist.

**The later Android/macOS release remains no-go.** Their confirmed blockers are
unchanged, but they are outside the first iOS-only release stage.

## Remediation log

- `38ffb3dd` resolves R7: chart migration now fails atomically when SQLite
  rejects the savepoint release, with a real authorizer-driven regression test.
- `fc41b0af` resolves the R5 transport risk: stored HTTP
  origins remain supported, anonymous public ranking requests remain available
  without credentials, and every Tachi path that sends a bearer token or reads
  private scores now rejects HTTP before network I/O. The settings UI disables
  authenticated actions and persisted HTTP settings clear auto-submit. Manual
  result uploads now use the same HTTPS availability gate, so a configured API
  key cannot leave the upload action enabled for an HTTP origin and the UI
  directs the user to switch the origin to HTTPS.
- R5 credential-at-rest remediation is implemented and verified locally: iOS
  credentials use Keychain with device-only, after-first-unlock accessibility;
  legacy plaintext files are removed only after every entry has been written
  and byte-verified. A partial migration keeps the legacy source and disables
  authenticated IR for that profile for the session. Portable migration,
  profile archive/lifecycle, device compilation, and simulator build/launch
  checks pass. No privacy manifest was added, per the accepted release policy.
- Plaintext migration cleanup now treats the legacy-file parent-directory sync
  as part of the transaction. If the unlink succeeds but directory metadata
  cannot be persisted, migration remains failed instead of accepting a
  potentially reversible credential deletion.
- Profile deletion now coordinates its filesystem and Keychain work through a
  durable, non-secret marker. The marker is committed before profile deletion,
  cancelled if the profile survives, retained after a transient Keychain
  failure, and retried at the next successful startup only when the profile is
  absent. A restart-level regression verifies that failed secure cleanup can no
  longer be silently orphaned.
- Profile archive overwrite now stages a non-secret credential-reset marker
  inside the replacement profile. The marker moves atomically with the durable
  profile-directory commit, so rollback keeps the old account intact while a
  committed overwrite clears the old profile ID's Keychain credentials. A
  transient failure or crash retains the marker and retries cleanup on startup
  even though the replacement profile still exists.
- R10 odd-dimension rendering remediation is implemented and verified locally:
  one checked YUV420 layout now uses ceiling chroma dimensions for allocation,
  texture creation, and padded-plane uploads; invalid or unrepresentable
  layouts fail cleanly. Table-driven layout and jukebox regressions pass.
- R10 decoder lifecycle remediation is implemented and verified locally: codec
  output is drained before input, packets survive send backpressure, demux EOF
  sends exactly one accepted null flush, delayed output drains through decoder
  EOF, and seeks reset retained state. Per-player buffering is reduced from ten
  queued plus up to twenty recycled frames to three queued plus two recycled
  frames. Deterministic state-machine, real FFmpeg/jukebox, and main-build
  regressions pass.
- R4 artwork remediation is implemented and verified locally: decoded RGBA is
  downsampled to the requested render bounds and stored in a byte-accounted LRU
  capped at 64 MiB on iOS (128 MiB elsewhere). Async work is deduplicated by
  source and size, limited to two workers, owned by cancellable consumer
  tickets, priority-aware, and generation-safe against stale completion.
  Focused cache/coordinator tests and existing image-view/list regressions pass.
- Live image views now detect decode tickets invalidated by memory-pressure
  eviction and issue replacement work. Async artwork also tracks its requested
  layout bounds: growing a view starts a prioritized higher-resolution decode
  while retaining the usable smaller texture until replacement. Named-pipe
  regressions prove that layout growth initiates the refresh itself. Terminal
  decode failures also cancel their coordinator ticket, allowing a later bind
  to retry when a temporarily unavailable source becomes readable.
- iOS low-memory warnings now flow through one main-loop handler: evictable
  artwork and orphaned decode work are cleared, active video buffers are kept,
  and idle video frame/recycle buffers are discarded and suspended until their
  next activation/seek. Repeated notifications are safe and non-active media
  can resume through the normal visual activation path.
- The attempted R9 on-demand BGA remediation was removed after it caused visual
  drift: event-time materialization blocked the scheduler and then started the
  video from zero after the audio clock had advanced. Chart loading now fully
  materializes every referenced visual before playback is eligible, and event
  activation performs only lookup plus seek/play. A four-ID fixture verifies
  every video is ready before playback and that activation performs no loading
  or eviction. The fixture also verifies that the preloaded video's duration
  extends replay export through the BGA tail. R9's decoder-count/memory cost
  remains technical debt because rhythm-game synchronization takes precedence
  for release 0.0.1.
- Archived visuals preserve that eager-preload invariant without reopening a
  solid archive once per referenced ID: chart loading groups image and video
  targets by archive, reads each group as one batch, and initializes every
  referenced visual before playback. The compatibility fallback also runs only
  during chart loading. The proposed memory-pressure eviction of inactive BGA
  allocations is intentionally not applied because it would require later
  rematerialization or decoder setup on the timing-critical playback path;
  memory warnings continue to discard idle decoder frame/recycle buffers while
  keeping preloaded visual resources ready for synchronized activation.
- The agreed iOS constraints remain unchanged: app marketing version `0.0.1`,
  app deployment target iOS 14, and `NSAllowsArbitraryLoads = true` for
  difficulty-table loading.
- iOS dependency builds are now aligned with that contract: CocoaPods declares
  and post-processes iOS 14, generated bgfx projects use a 14.0 deployment
  target, and both unsigned and Fastlane build entrypoints pass the same target
  to referenced SDL/bgfx projects. Setup is idempotent with the project Ruby;
  the version, ATS exception, and intentional absence of a privacy manifest are
  covered by static release tests.
- iOS verification and distribution are now separate release gates. A
  no-upload script runs the release-critical C++ tests, iOS contract tests, and
  unsigned device build, including the durable IR credential-cleanup recovery
  regression. Firebase is an explicit PR-to-`develop` fast-iteration
  lane that neither schedules nor waits for the release verifier. TestFlight is
  a distinct non-PR lane whose GitHub job waits for verification, uses a global
  non-canceling concurrency group, and allocates the next build number only
  after entering that serialized lane. The end-to-end verification command
  passed without invoking signing or distribution. A fresh
  `IOS_RELEASE_CMAKE_BUILD_DIR` override is configured at the requested path
  with the debug preset rather than accidentally configuring the default
  directory and then trying to build an empty override directory.
- A deterministic app/IPA audit now verifies release version `0.0.1`, iOS 14
  plist and Mach-O minimums, iPhone/iPad families, device SDK, icons,
  permissions, the retained ATS exception, arm64 slices, resolvable embedded
  dependencies, unwanted metadata, credential patterns, and signatures when
  required. Synthetic positive/negative fixtures pass, and the unsigned device
  app produced by the no-upload build passed every applicable check; signature
  verification remains a signed-archive gate. No privacy manifest is required
  or generated by the audit.
- Artifact credential scanning now inspects the raw bytes of the validated app
  and embedded-framework Mach-O executables in addition to text resources, so
  binary payloads cannot bypass the gate. The TestFlight lane runs this audit
  with signature enforcement against the final post-processed IPA before any
  upload; the Firebase fast-iteration lane remains intentionally unchanged.
- Distribution architecture validation now requires every executable and
  embedded framework binary to contain exactly the device `arm64` slice and to
  declare the iOS platform. A synthetic fat binary containing an x86_64
  simulator slice is rejected before it can reach signing or upload.
- The public privacy policy now matches the shipped iOS Keychain migration and
  authenticated-HTTPS behavior while documenting the retained anonymous HTTP
  and difficulty-table paths. A versioned iOS-first checklist covers App Store
  metadata, screenshots, signed-artifact inspection, and physical iPhone/iPad
  smoke gates. Both documents are enforced by the no-upload verification script;
  privacy manifests remain intentionally omitted for release 0.0.1.
- iOS now selects bgfx's documented single-threaded renderer mode before
  initialization, keeping CAMetalLayer setup and presentation on the UIKit
  thread. A Release simulator launch with Main Thread Checker injected and
  crash-on-report enabled reached the main menu through Metal without the
  previously reproduced off-main-thread layer mutation.
- A focused idle-main-menu Time Profiler comparison removed per-frame
  `UIApplication.connectedScenes` enumeration from samples. A weak cache is
  reused only while its `UIWindowScene` remains foreground-active and the
  window is still key and visible; same-scene key-window changes therefore
  force a rescan instead of returning a detached controller;
  `FindActiveWindow` inclusive samples fell from 46 ms to 26 ms across matched
  10.7-second iPad simulator runs. Both runs reported zero potential hangs.
  Total simulator CPU was noisy between single runs, so no broader throughput
  claim is made from this targeted result.
- Release compilation exposed three uninitialized reads in
  `Quaternion::getEulerAngles` and a compound vector cross-product that reused
  already-overwritten components. The Euler accessor now delegates to the
  guarded canonical conversion and the cross-product assigns from a temporary;
  deterministic numeric and compiler-warning regressions cover both defects.

| Release gate | Result | Release significance |
| --- | --- | --- |
| iOS release contract and unsigned device build | pass | Version 0.0.1, iOS 14 minimum, retained ATS exception, no privacy manifest |
| iOS unsigned artifact audit | pass | Device Mach-O closure, min OS, architecture, resources, and sensitive-file checks pass |
| iPhone/iPad simulator runtime | pass | Release app reaches main menu through Metal with Main Thread Checker crash-on-report |
| Desktop and iOS release tests | pass | 186/186 CTest and 45/45 iOS release-policy tests pass |
| iOS signed archive audit | **pending** | Requires release signing material; signature enforcement was not bypassed |
| Physical iPhone/iPad smoke and performance | **pending** | Required before submission; not substitutable with the iOS 26 simulator |
| App Store metadata/checklist | **pending** | Human-owned screenshots, disclosures, support URLs, and final approval remain |
| Android supported-device startup/upgrade (later stage) | **fail** | Profile/database initialization fails reproducibly on API 29 |
| macOS package validation (later stage) | **fail** | macOS 26-only, invalid/ad-hoc signature, unbundled Homebrew dylib |
| Android lint (later stage) | **fail** | 28 errors, but Gradle is configured to return success |

## Audit scope

This audit covered source, workflows, builds, static analysis, package
inspection, Android emulator smoke, iPhone/iPad simulator smoke with Main
Thread Checker, and an iPad simulator Time Profiler comparison. No deployments,
uploads, archives, or signing operations were performed. The signed iOS
archive audit, physical-device smoke/performance trace, sanitizer campaign,
dependency CVE scan, and formal license/SBOM review remain outside this pass;
the first two are explicit iOS 0.0.1 submission gates rather than implications
of the green compile.

## Confirmed findings

### R0 — Android cannot preflight non-empty databases in its temporary directory (release blocker, bug)

The signed `firebaseRelease` APK installs and starts on the supported Android 10
(`API 29`) AVD, and bgfx reaches Vulkan, but profile initialization fails on two
consecutive launches:

```text
Refusing to open score database .../profiles/.staging-.../scores.db:
could not create schema preflight directory: Permission denied
Application profile initialization failed: unable to initialize profile score database
```

Root-cause evidence:

- The upgrade path snapshots the existing legacy score database into profile
  staging, then opens it through `PlayerProfileManager.cpp:1341-1407`.
- `repositories/SqliteRAII.h:549-610` places its isolated non-empty-database
  snapshot under `std::filesystem::temp_directory_path()`.
- The NDK `libc++_shared.so` packaged in this APK resolves its Android fallback
  to `/data/local/tmp`; the
  [official Android toolchain patch catalog](https://android.googlesource.com/toolchain/llvm_android/+/f48b69ba1dd680336d71f1fe989ea2e8da064692/patches/PATCHES.json)
  identifies this behavior as `Android temp dir is /data/local/tmp`.
- On the AVD, `/data/local/tmp` is `0771 shell:shell`, while the application is
  `u0_a154`; creating the preflight child fails with `EACCES`. No application
  startup code sets a writable `TMPDIR` or substitutes the already-available
  `GetAndroidCacheDir()`.
- Ten additional production call sites use raw
  `std::filesystem::temp_directory_path()` for archive caching, BMS download
  staging, profile import/export, replay work, and document handoff.

Impact: users with an existing score database cannot complete the profile
migration, so a normal upgrade can reach the renderer but not initialize usable
profile persistence. The same platform mismatch can break other temporary-file
features after startup.

Recommendation: centralize temporary storage behind a platform helper and use
the app's internal cache directory on Android (or set a verified writable
`TMPDIR` before any C++ filesystem call). Convert every raw call site. Add an
API-28/29 upgrade smoke fixture containing a non-empty WAL database, plus clean
first/second-launch and temporary-feature tests.

### R1 — The packaged macOS app is not distributable (release blocker, packaging)

The workflow-equivalent Release bundle builds, but inspection of the produced
`AsoBMaShow.app` shows three independent blockers:

- `vtool -show-build` reports `minos 26.0`. No deployment target is supplied by
  `scripts/macos_init.sh` or `.github/workflows/macos-build.yml`, and the generated
  Info.plist contains an empty `LSMinimumSystemVersion`. The artifact therefore
  only runs on macOS 26 even though the source comments say a release target must
  be chosen explicitly.
- The bundle is only linker/ad-hoc signed. `codesign --verify --deep --strict`
  fails (`code has no resources but signature indicates they must be present`),
  there is no Developer ID signature, hardened-runtime configuration, or
  notarization step, and `spctl --assess` rejects it.
- `otool -L` reports an unbundled absolute dependency on
  `/opt/homebrew/opt/zlib/lib/libz.1.dylib`. The bundle has no Frameworks/lib
  directory, so it depends on the build machine's Homebrew layout and will fail
  to launch on a normal destination Mac. The current shell's global
  `LDFLAGS=-L/opt/homebrew/opt/zlib/lib`, combined with the bare `z` link in
  `CMakeLists.txt:410-415`, wins over the already-linked vcpkg static zlib.

The locally built app is 58 MiB (50 MiB executable), arm64-only, version 1.0,
and has no macOS application icon. These are secondary release-scope decisions after the
launch/signing blockers.

Recommendation: choose and consistently build every dependency for a supported
`CMAKE_OSX_DEPLOYMENT_TARGET`; eliminate host path leakage and validate every
non-system `otool -L` entry; configure an icon; then Developer-ID sign with the
hardened runtime, notarize, staple, and gate packaging on `codesign` and `spctl`
verification. Build a universal artifact if Intel support is a requirement.

### R2 — Android lint failures are allowed through the release build (high, bug/release engineering)

Evidence:

- `android/app/build.gradle:162-164` sets `lint.abortOnError false`.
- Both `lintPlayDebug` and `lintFirebaseDebug` complete while reporting 28 errors.
- The app targets Android 35 but
  `android/app/src/main/AndroidManifest.xml` does not declare
  `POST_NOTIFICATIONS`.
- `AsoBMaShowMusicService.java:179` posts a paused-playback notification without
  checking/requesting that runtime permission. Android lint reports
  `NotificationPermission`.
- `SDL/.../HIDDeviceManager.java:196` registers a receiver containing the custom
  `org.libsdl.app.USB_PERMISSION` broadcast without an exported/not-exported flag.
  Lint reports `UnspecifiedRegisterReceiverFlag`. The app initializes every SDL
  subsystem (`src/main.cpp:576`), which initializes HIDAPI/USB; on target 35 this
  path throws and is cleared by SDL JNI, leaving USB HID discovery unavailable.
- `AsoBMaShowActivity.java:457,502` can pass zero to
  `takePersistableUriPermission`; the exception is swallowed, so a selected tree
  can silently lose durable access. Lint reports `WrongConstant`.
- The manifest has no application icon (`MissingApplicationIcon`).

Impact: notification behavior is broken or degraded on Android 13+, USB HID
controller discovery is degraded on Android 14+/target 35, folder access can
fail after restart, and the shipping launcher can have a generic/missing icon.

Recommendation: fix the first-party permission flows, update/backport SDL's
receiver registration, configure the icon, triage or suppress only proven
false positives, then make lint fatal in CI.

### R3 — Release/deploy CI does not execute the test suites (high, technical debt)

Evidence:

- `.github/workflows/mobile-beta-deploy.yml` intentionally lets the iOS Firebase
  PR iteration lane and Android Firebase deploy without executing CTest,
  platform unit tests, or Android lint. The TestFlight lane has a separate
  required iOS verifier.
- `.github/workflows/macos-build.yml` builds and packages without running CTest.
- `ASOBMASHOW_BUILD_TESTS` is on by default, so the macOS workflow compiles the
  large test graph but never runs it. The local all-target build had 1,097 Ninja
  steps; 178 tests then completed in about 61 seconds. The 3,997-line CMake file
  defines 165 executables and recompiles the 250,816-line SQLite amalgamation in
  22 targets and the BMS amalgamation in 55 targets. The debug build tree reached
  4.4 GiB.

Impact: regressions can be uploaded to the accepted fast-iteration Firebase
track, Android Firebase, or a GitHub release despite a broad, currently healthy
test suite. TestFlight is protected by the iOS release verifier. macOS CI also
pays the test compilation cost without receiving its validation benefit.

Recommendation: retain the required TestFlight verifier, treat Firebase builds
as non-release iteration artifacts, add required Android test/lint jobs before
release deployment, and run CTest after building on macOS. Decide explicitly
whether packaging jobs should compile tests or depend on a separate test job.
Extract common object/static test-support libraries so large amalgamations and
shared production sources compile once.

### R4 — Decoded artwork caches and stale async work are unbounded (high, performance)

Evidence:

- `src/view/ImageView.cpp:655` holds decoded images in a process-wide map with no
  byte/count limit or eviction policy.
- `ImageView.cpp:690-691` retains the full decoded RGBA allocation after the GPU
  upload.
- `ImageView.cpp:518` retains completed async images until a matching view takes
  them; rebinding a recycled view at `ImageView.cpp:770-802` does not cancel or
  discard the old queued/in-flight request.
- Production cache clearing occurs only at a few library-wide invalidation
  points in `MainMenuScene.cpp`, not during ordinary scrolling/navigation.

Impact: browsing many unique full-resolution artworks can grow CPU memory for
the life of the library session and stale work can consume decode CPU/I/O. A
1920×1080 RGBA image is about 7.9 MiB; 100 unique images are about 791 MiB before
container, GPU, and decoder overhead.

Recommendation: add a byte-budgeted LRU, downsample to rendered thumbnail size,
and add request ownership/cancellation or discard-on-completion for recycled
views. Add a stress test asserting a bounded cache/work queue.

### R5 — IR credentials are exposed at rest and may be sent over HTTP (high, security)

Evidence:

- `src/context.h:161-164` uses `Utils::GetDocumentsPath()` as application data.
- On Android, `src/Utils.cpp:127-133` resolves that to the external-files path;
  `AndroidNatives.cpp:748-761` already provides an internal-files helper.
- On iOS, the same function resolves to `NSDocumentDirectory`
  (`iOSNatives.mm:3324-3327`), while `UIFileSharingEnabled` is true and the
  Xcode settings enable opening documents in place/document browsing.
- `PlayerProfileManager.cpp:396-405` stores `ir-credentials.json` beneath that
  root, and `ir/IrCredentialStore.cpp:57-63` serializes each API key directly
  into plaintext JSON. No Keychain, Keystore, or equivalent protected store is
  used.
- `IrProfileSettings.cpp:61-150` accepts both HTTP and HTTPS origins. The Tachi
  driver sends the key as `Authorization: Bearer ...` and score data to that
  origin (`ir/tachi/TachiDriver.cpp:169-180`). The settings screen warns that an
  HTTP origin is insecure, but still permits the authenticated request.
- Android enables backups without data-extraction/full-backup rules, so the
  intended backup treatment of profile credentials is unspecified.

Impact: API keys are plaintext in a user-file-facing iOS Documents hierarchy and
in Android emulated external storage. App-specific external storage is better
isolated on current Android versions, but remains the wrong trust boundary for
credentials, especially with the supported API 28 floor. Selecting HTTP exposes
the bearer token and score payload to network interception by design.

Recommendation: store secrets in iOS Keychain and Android Keystore-backed
internal storage, keep shareable profile data separate from secrets, and define
backup/export behavior explicitly. Require HTTPS for authenticated production
origins; if localhost/development HTTP is necessary, gate it behind a non-release
developer mode rather than a warning alone.

### R6 — Android release compilation exposes first-party portability warnings (low)

Evidence: the `firebaseRelease` native build warns at `src/audio/decoder.cpp:114`
that `%lld` is used for `sf_count_t`, which is `long` on Android arm64.

Impact: diagnostic output is undefined by the C varargs contract and can be
wrong on affected ABIs.

Recommendation: use a type-correct format/cast or C++ formatting and promote
first-party warnings to errors while excluding third-party code.

The same build also reports a non-standard designated-initializer order at
`src/replay/ReplayFileStore.cpp:422-427`: `compressedSize` is initialized before
the earlier-declared `sha256` field. Apple Clang accepts this as an extension,
but the code violates standard C++ declaration order and reduces compiler
portability.

### R7 — A failed chart-migration commit is reported as success (medium, bug)

Evidence: `src/repositories/ChartRepository.cpp:401-415` assigns the result of
`RELEASE chart_metadata_rebuild_migration` to `ok`, but never checks it. Even
when SQLite reports that the savepoint commit failed, the function logs success,
bumps the library revision, sets `completed = true`, and returns true.

Impact: an I/O/SQLite failure at the transaction boundary can be hidden and the
caller can continue under the false assumption that cache invalidation and its
durable rebuild marker committed.

Recommendation: handle a failed `RELEASE` as failure, perform best-effort
rollback/cleanup, and add an injected commit-failure test.

### R8 — Deployment versioning and concurrency are not release-safe (medium, release engineering)

Evidence:

- `mobile-beta-deploy.yml` has no concurrency group. Every qualifying iOS job
  outside a pull request to `develop` reads the latest TestFlight build number
  and adds one (`fastlane/Fastfile:102-105`). Concurrent jobs can therefore
  select the same build number.
- Android defaults to a minute-resolution UTC version code
  (`scripts/android_firebase_deploy.sh:269-272`), so two builds in one minute
  receive the same identity.
- Public-facing versions disagree: CMake/macOS is `1.0`, iOS is `0.0.1`, and
  Android generates `1.0.<timestamp>`.

Impact: overlapping workflows can collide or publish ambiguously identified
artifacts, and support/rollback decisions cannot reliably map one product
release across platforms.

Recommendation: derive all platforms from one release version plus a unique,
monotonic CI build number, and serialize or otherwise make publishing atomic.

### R9 — Every referenced BGA video gets its own decoder thread and frame buffer (high, performance)

Evidence:

- `Jukebox::reconcileVisualResources` iterates every unresolved visual ID and
  calls `loadVideoPath`/`loadMaterializedVideoPath` independently. Multiple IDs
  for the same file also create independent players on an initial load.
- `loadMaterializedVideoPath` constructs one `VideoPlayer` per ID.
- `VideoPlayer::loadVideo` immediately starts a predecode thread, regardless of
  whether that visual is active. Each player fills a ten-frame YUV420 buffer and
  then keeps the frames and decoder alive until chart unload.
- Static images are similarly decoded/uploaded per visual ID rather than shared
  by canonical asset path during the initial load.

Impact: a chart containing many video IDs can create many simultaneous decoder
threads and retain all of their predecoded frames. Ten 1920×1080 YUV420 frames
are roughly 30 MiB per player before alignment and decoder overhead; ten unique
players are roughly 300 MiB. Duplicate IDs multiply the cost without adding
unique content. This is an OOM/startup-stall risk on mobile.

Recommendation: preserve the invariant that every scheduled visual is ready
before playback starts and that event activation never performs file/archive
I/O or decoder initialization. Future memory work may share immutable sources,
reduce per-player buffers, and prepare a bounded decoder pool during chart
loading, but it must prove all event targets are ready (including after seek)
before replacing eager preparation. Add a many-ID stress fixture that checks
both memory and exact audio-clock alignment.

### R10 — Video decoding mishandles valid edge cases and has no focused tests (medium, bug)

Evidence:

- `VideoPlayer.cpp:268-283,430-436` sizes YUV420 chroma planes with integer
  `width / 2` and `height / 2`. FFmpeg's local pixel-format contract explicitly
  defines chroma dimensions with `AV_CEIL_RSHIFT`, so odd frame dimensions need
  ceiling division. The current code drops the last chroma row/column.
- At demux EOF (`VideoPlayer.cpp:606-614`), the decoder is never flushed with a
  null packet, so delayed codec frames can be omitted.
- There are no focused `VideoPlayer` tests; the only small video fixture is used
  by a higher-level jukebox restore test and has even 2×2 dimensions.

Impact: odd-dimension video can render corrupted edges, and videos using delayed
frames can lose their ending frames.

Recommendation: calculate plane dimensions from the FFmpeg pixel descriptor,
drain the decoder at EOF, and add odd-dimension plus B-frame end-of-stream tests.

## Additional technical debt

- The workspace still has ambiguous same-named SDL framework products. A clean,
  unconstrained simulator scheme build can select SDL's macOS `Framework`
  target and fail on `Cocoa/Cocoa.h`; the successful arm64 simulator audit used
  the already-built iOS dependency products with implicit dependency discovery
  disabled temporarily. Generic device release verification selects the iOS
  products and passes. Add explicit cross-project target dependencies so
  simulator builds do not depend on product-name inference.
- The static-analyzer pass completed. It found the ignored SQLite commit result
  described above and several dead stores. Its possible replay-UI null access,
  jukebox image-dimension uninitialized access, and moved-`std::function` leak
  paths do not reproduce through surrounding lifecycle/return invariants and
  are not classified as confirmed defects.
- `CMakeLists.txt:448-453` uses unsupported `DEPENDS` syntax with the TARGET form
  of `add_custom_command`; modern CMake warns and the intended incremental Metal
  shader dependency is not represented.
- `CMakeLists.txt:92` adds `-g` globally, including Release. The final linker does
  not retain DWARF in the executable, but every release compile/cache still pays
  the debug-info generation and object-size cost.
- `MainMenuScene.cpp` is 12,369 lines, `ArchiveFile.cpp` 8,660, and the root CMake
  file 3,997. These concentration points make lifecycle invariants harder for
  tools and reviewers to establish; the analyzer's replay-modal null warning is
  currently protected only by construction/teardown ordering.
- Performance telemetry is hard-coded on in Release (`src/main.cpp:826`) and
  logs every five seconds. It is low overhead, but should be an explicit runtime
  diagnostic option rather than permanent production behavior.
- The iOS 26 simulator logs SDL UIKit-controller appearance-transition warnings
  at launch and UIKit's forward-looking requirement to adopt the `UIScene`
  lifecycle. Moving fullscreen selection into window creation did not change
  the warnings, so that ineffective workaround was discarded. They do not
  currently stop launch or rendering, but the SDL/UIKit lifecycle integration
  needs a focused upgrade before a future iOS SDK turns the warning into an
  assertion.
- Simulator-only prebuilt archive members report iOS-simulator 26.0 minimums
  while linking the app at 14.0. They are not included in the shipping device
  artifact, whose complete Mach-O closure passes the iOS 14 audit, but they
  prevent this Xcode 26 environment from proving runtime behavior on an iOS 14
  simulator.
- The macOS workflow runs `git pull` on a persistent, unpinned vcpkg checkout.
  The manifest baseline pins port resolution, but the package-manager executable
  itself can change between identical source builds. Pin the tool revision and
  avoid mutating shared runner state during a release job.
- No automated dependency vulnerability, SBOM, or bundled-license completeness
  check is present. This audit found no tracked private-key material; the two
  key-like tracked files are the expected Firebase client configuration files.

## Recommended release order

1. Produce the signed iOS 0.0.1 archive and run the artifact audit with
   signature enforcement enabled; do not distribute an artifact that only
   passed the unsigned path.
2. Complete the checklist's physical iPhone and iPad smoke tests, including
   chart import, difficulty-table HTTP loading, authenticated IR over HTTPS,
   playback/BGA memory pressure, background/foreground, and a representative
   performance trace.
3. Complete App Store screenshots, URLs, disclosures, export-compliance answers,
   age rating, and the final release-owner approval, then submit the exact
   audited archive.
4. After the iOS stage, fix the Android writable-temporary-root/lint/runtime
   blockers and the macOS portability/signing/notarization blockers before
   enabling either platform's release.

## Verification log

| Check | Result |
| --- | --- |
| Branch created from clean `develop` | pass |
| `cmake --build cmake-build-debug --target main -j 6` | pass |
| Full debug all-target build after remediation | pass |
| `ctest --test-dir cmake-build-debug --output-on-failure -j 6` | pass, 186/186 |
| `python3 tests/ios_build_setup_tests.py` | pass, 24/24 (12.21 s) |
| iOS release workflow tests | pass, 11/11 |
| iOS artifact-audit fixtures | pass, 8/8 |
| iOS release-documentation tests | pass, 3/3 |
| `scripts/ios_release_verify.sh` | pass; 10 native release tests, unsigned device build, and app audit |
| Unsigned device `AsoBMaShow.app` audit | pass; signature check correctly skipped only for unsigned build |
| iPhone 17 Pro Release simulator launch | pass; Metal, Main Thread Checker crash-on-report, no fatal issue |
| iPad Pro 11-inch Release simulator launch | pass; Metal, Main Thread Checker crash-on-report, no fatal issue |
| iPad idle Time Profiler before/after | zero hangs both; `FindActiveWindow` 46 ms → 26 ms, `connectedScenes` 11 ms → 0 ms |
| Android Firebase debug unit tests | pass |
| Android Play debug unit tests | pass |
| Android Play/Firebase lint | command succeeds, reports 28 errors and 57/58 warnings |
| Android `firebaseRelease` build-only | pass (2m 12s; 52 MiB APK) |
| Android Firebase APK signature verification | pass (v2; one signer) |
| Android 10/API-29 signed-APK launch | renderer starts with Vulkan; profile migration fails reproducibly |
| iOS build-only | pass (`BUILD SUCCEEDED`) |
| macOS Release bundle compilation | pass (372 Ninja steps) |
| macOS `codesign --verify --deep --strict` | fail |
| macOS `spctl --assess --type execute` | fail |
| macOS deployment/load-command inspection | fail: empty plist minimum, binary min macOS 26.0 |
| macOS dependency inspection | fail: unbundled Homebrew zlib |
| First-party static-analyzer sample | completes; confirms ignored SQLite release result |
| Tracked credential-pattern/name review | no private-key material found; expected Firebase client configs present |
