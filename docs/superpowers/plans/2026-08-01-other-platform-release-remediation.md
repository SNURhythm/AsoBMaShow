# Other-Platform Release Remediation Plan

**Date:** 2026-08-01

**Branch:** `agent/deferred-platform-release-fixes`

**Baseline:** `develop` at `82b74406`

**Goal:** Resolve the actionable Android, macOS, and shared desktop findings
deferred by the iOS-first release audit without changing the approved iOS
0.0.1 contract or moving work onto the timing-critical BGA activation path.

## Invariants

- Keep the public application version at `0.0.1` across platforms.
- Keep iOS/iPadOS 14, `NSAllowsArbitraryLoads = true`, and the intentional
  absence of privacy manifests.
- Keep every scheduled BGA resource materialized before playback. Event-time
  activation must remain lookup plus seek/play, with no file/archive I/O,
  decoder construction, or eviction recovery.
- Keep PR-to-`develop` Firebase distribution as the explicitly accepted fast
  iteration track. Do not add the full native test suite to that upload path.
- Do not deploy, upload, publish, notarize, or use release signing credentials
  during local verification.

## Reconciled Findings

| Audit item | Current state on baseline | This branch |
| --- | --- | --- |
| R0 Android writable temporary root | Fixed on `develop`; API-29 regression contract present | Preserve and recheck |
| R1 macOS deployment/signing/dependency closure | Open release blocker | Fix build target, dependency selection, bundle metadata/icon, artifact audit, signing/notarization workflow |
| R2 Android lint/runtime permissions/icon | Permissions, URI grants, SDL receiver, and fatal lint fixed; launcher icon still absent | Add shared product icon and preserve fatal lint |
| R3 CI does not execute tests | TestFlight protected; Firebase intentionally fast; macOS still packages without CTest | Run CTest and release contract checks before macOS packaging |
| R6 C++ portability warnings | Open | Correct the `sf_count_t` format and designated-initializer order; add warning regression checks |
| R8 release identity/concurrency | iOS serialized; Android code remains minute-granularity; desktop/Android report 1.0 | Align public version to 0.0.1 and use a finer monotonic Android build identity |
| R9 eager BGA decoder count | Frame/recycle buffers reduced, but one prepared decoder per scheduled ID remains | Keep open unless a chart-load-only pooling design can prove independent simultaneous timelines and zero activation work |
| CMake Metal dependency warning | Open | Replace unsupported TARGET/DEPENDS form with a real dependency target |
| Global Release `-g` cost | Open | Restrict debug information to Debug/RelWithDebInfo |
| Release performance telemetry | Hard-coded on | Make compile-time opt-in and default it off for Release |
| macOS vcpkg checkout mutation | Open | Pin the tool checkout and avoid `git pull` on shared mutable state |

## Implementation Order

1. Add static release-contract tests for version alignment, macOS deployment
   configuration, imported zlib usage, bundle metadata/icon, Release compiler
   flags, opt-in telemetry, workflow test/audit gates, pinned vcpkg, Android
   launcher icon, and unique build-number policy.
2. Fix the two first-party portable C++ diagnostics and compile their focused
   targets with the Android NDK frontend where practical.
3. Make macOS release configuration explicit and reproducible. Generate a
   bundle icon from the existing approved 1024px application artwork, use the
   vcpkg zlib target, and correct Metal shader dependency tracking.
4. Add a deterministic macOS artifact auditor for version/minimum OS,
   architecture, dependency closure, resources, debug metadata, and optional
   Developer ID signature/Gatekeeper enforcement.
5. Update macOS CI so pull requests build, run CTest, and audit an unsigned or
   ad-hoc verification artifact. Tag builds must require Developer ID signing,
   hardened runtime, notarization, stapling, and the strict audit before a
   draft release artifact is created.
6. Align Android/macOS public versions with 0.0.1, improve Android build-code
   uniqueness, and add the approved application icon to Android.
7. Run focused contracts, the full portable CTest suite, desktop compilation,
   a local unsigned macOS Release bundle audit, Android lint/contracts, and
   shell/static checks. Skip Firebase archive/upload verification.
8. Self-review the diff and update the original audit with a factual
   remediation addendum, leaving credential-backed and hardware-only gates
   explicitly pending.

## External Gates That Code Cannot Complete

- Developer ID certificate/private key availability and release-owner identity
  selection.
- Apple notarization credentials and the live notarization service response.
- Gatekeeper validation of the exact stapled distribution archive on a clean
  destination Mac.
- Physical Android/macOS device compatibility and performance coverage.
- Windows-native compilation, packaging, and runtime coverage on a Windows host.
- A future BGA pooling redesign with a stress fixture proving simultaneous
  base/layer correctness, seek correctness, eager readiness, and audio-clock
  alignment.

## Verification Outcomes

- macOS Release app plus native test tree: pass (1,824-step initial build;
  1,768-step assertion-enabled test rebuild).
- macOS Release CTest: pass, 186/186 in 64.57 seconds with six parallel workers.
- macOS synthetic artifact audit tests: pass, 5/5.
- Cross-platform release contracts: pass, 13/13.
- Android release workflow contracts: pass, 6/6.
- Android Firebase/Play debug lint under Java 17: pass, 54 Gradle tasks.
- Android NDK focused compilation: `decoder.cpp` and `ReplayFileStore.cpp` pass.
- Real macOS 0.0.1 bundle audit: pass after hardened-runtime ad-hoc signing;
  arm64, macOS 13.0, resources, signature, and dependency closure verified.
- Real macOS runtime smoke: Metal renderer and main menu reached; normal Cocoa
  quit completed.
- ShellCheck, shell syntax, plist, and workflow YAML parsing: pass.
- Firebase archive/build/upload: intentionally skipped for fast iteration, as
  requested. No deployment, release signing, notarization, or upload occurred.
