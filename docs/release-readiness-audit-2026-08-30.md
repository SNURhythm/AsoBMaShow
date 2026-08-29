# Release Readiness Audit — 2026-08-30

## Decision

The audited source tree is a **go for release-candidate review** after the
changes in this branch. No known critical or high-severity source-code blocker
remains. The release is still conditional on the signed-build, physical-device,
store, notarization, Windows, and legal-review gates listed below; those require
credentials, hardware, platforms, or professional review outside this audit.

This is an evidence-based point-in-time assessment, not a guarantee that the
software is free of every defect or vulnerability.

## Scope

- Base: `origin/main` at `23f02213`
- Audited areas: first-party C, C++, Objective-C++, Java, shell, Ruby release
  tooling, Gradle/Xcode/CMake configuration, GitHub Actions, vendored SQLite,
  mobile backup behavior, network transports, archive handling, persistence,
  release artifacts, licenses, and repository history
- Platforms verified locally: macOS desktop tests/build, unsigned iOS device
  build, Android Play/Firebase debug builds
- No build was uploaded or distributed

## Remediated findings

| Severity | Finding | Resolution |
| --- | --- | --- |
| High | Android targeted API 35 immediately before Google Play's 2026-08-31 API 36 deadline and lacked a repository-owned, checksum-pinned Gradle wrapper. | Moved to compile/target API 36, AGP 8.10.1, Gradle 8.11.1 with verified checksums, and NDK r28. The native libraries pass 16 KiB ELF alignment checks. |
| High | Vendored SQLite 3.43.1 predates SQLite's 3.43.2 JSON-parser use-after-free fix and later memory-safety fixes. | Updated the exact upstream amalgamation to SQLite 3.53.4 and pinned a release-policy minimum. |
| High | The iOS release bundle contained dependencies with current OSV advisories in ActiveSupport, concurrent-ruby, Excon, Faraday, JSON, and JWT. | Updated Fastlane and the affected dependency graph. OSV Scanner reports no known vulnerability in the resulting `Gemfile.lock`. |
| Medium | Android's private IR API-key file was eligible for cloud backup and device transfer. | Kept general user-data backup enabled but excluded the entire `profiles` subtree in both legacy and Android 12+ backup rule formats. |
| Medium | Desktop/iOS downloads could follow an HTTPS response to HTTP; HTTPS difficulty-table metadata could also load an HTTP data document. | HTTPS requests now reject HTTP and non-HTTP redirects across curl, `NSURLSession`, and Android. Mixed-content difficulty-table references are rejected before a request is made. Direct legacy HTTP sources remain supported where the platform permits them. |
| Low | Split settings translation units emitted duplicate internal-linkage helper bodies and unused-function warnings; other first-party functions and locals were dead. | Converted header-defined helpers to ODR-safe `inline` functions and removed unused wrappers, locals, and the obsolete course replay builder. |
| Low | Release scripts emitted actionable ShellCheck diagnostics. | Corrected the scripts and documented the one intentionally literal CocoaPods expression. ShellCheck is clean. |
| Low | FFmpeg and x264 license files shipped, but the top-level notice did not describe them or their GPL release obligations. | Added explicit FFmpeg/x264 notice and release-checklist sections. |

Relevant policy and upstream references:

- [Google Play target API requirements](https://support.google.com/googleplay/android-developer/answer/11926878?hl=en)
- [Android Gradle Plugin/API compatibility](https://developer.android.com/build/releases/about-agp)
- [Android 16 KiB page-size guidance](https://developer.android.com/guide/practices/page-sizes)
- [Android backup controls](https://developer.android.com/identity/data/autobackup)
- [SQLite CVE assessment](https://www.sqlite.org/cves.html)
- [SQLite security guidance](https://www.sqlite.org/security.html)

## Security review results

### Injection and network handling

- No confirmed SQL injection path was found. User-controlled values are bound
  with SQLite parameters. Dynamic table/column fragments are fixed internal
  constants, except profile-inspection identifiers, which are restricted to
  SQL identifier characters before interpolation.
- Playlist create/rename now has a regression test using SQL-injection-shaped
  names and verifies that the value round-trips while the schema remains
  intact.
- Process launching uses argument vectors rather than shell concatenation.
- Archive import restricts supported inputs, canonicalizes extraction paths,
  rejects traversal/system entries, bounds extraction, and stages downloads
  before publishing them into the library.
- Authenticated IR requests do not follow redirects. API keys were not found in
  logging statements, and Android credentials remain in owner-private internal
  storage with backup/transfer exclusion; iOS uses Keychain storage.

### Secrets

Gitleaks 8.30.1 scanned the complete Git history (3,194 commits). Twelve
candidates were manually reviewed as generated-code keywords, test
placeholders, or public Firebase client configuration. No private signing key,
service credential, bearer token, or production password was confirmed.

Public Firebase client configuration is intentionally versioned and is not a
server credential. Release signing material and real `.env` files remain
gitignored and were not added by this branch.

### Memory safety and undefined behavior

- Manual lifetime review covered archive COM-style reference ownership,
  detached document-handoff work, replay video writer cleanup, replay file
  deletion/reconciliation, and IR worker shutdown. No confirmed first-party
  use-after-free, double-free, or unsafe detached capture remained.
- UBSan ran with fatal diagnostics over archive concurrency, replay lifecycle,
  replay deletion, asynchronous document handoff, profile export staging, and
  SQLite-backed playlist paths. All selected suites passed.
- The same ownership-heavy suites passed with Apple's allocator scribble,
  pre-scribble, guard-edge, and abort-on-error diagnostics enabled.
- AddressSanitizer could not execute on this host: both the app test binary and
  a minimal standalone probe deadlocked inside the macOS ASan runtime during
  shadow-memory initialization. LeakSanitizer also reports that it is not
  supported on this platform. This environment limitation is recorded rather
  than being presented as a passing ASan result.

### Dependencies

- SQLite is 3.53.4, verified against the upstream archive SHA3-256 checksum.
- OSV Scanner 2.5.1 reports no known vulnerability in the final iOS Ruby lock.
- Direct vcpkg packages and shipped mobile libraries were reviewed against the
  pinned manifest/baseline. Findings in nested third-party development-only
  package metadata were excluded when that package manager is not used to
  build or ship AsoBMaShow.
- Gradle and its wrapper JAR were checked against official published
  checksums.

## Verification evidence

| Area | Result |
| --- | --- |
| Desktop configure/build | Clean Debug configure and `main` build passed. |
| Desktop tests | 186/186 CTest tests passed in parallel. |
| Security regression | SQL-injection-shaped playlist names and HTTPS mixed-content rejection passed. |
| Undefined behavior | Six ownership/storage-focused UBSan targets passed; address/leak sanitizer limitation documented above. |
| Allocator diagnostics | Six ownership/storage-focused targets passed with Apple malloc diagnostics enabled. |
| Android policy tests | 10/10 release workflow tests and 15/15 cross-platform contract tests passed. |
| Android builds | Play and Firebase debug unit tests, lint, assembly, and Java/native compilation passed. |
| Android native ABI | Every shipped native library has a minimum ELF `LOAD` alignment of 0x4000; APK zip alignment passed `-P 16`. |
| iOS policy tests | 24/24 setup, 12/12 workflow, 9/9 artifact-audit, and 3/3 documentation tests passed. |
| iOS build | `scripts/ios_release_verify.sh` passed its native tests, unsigned device build, and artifact audit without distribution. |
| Release scripts | `shellcheck scripts/*.sh` passed. |
| Dependency advisories | Final iOS Ruby OSV scan passed with no known advisories. |
| Secret history | Full-history Gitleaks scan completed; all candidates reviewed as non-secret. |

## Required external release gates

These are release operations, not unresolved source defects:

- Build the final Android release flavor with production signing credentials,
  then install it on at least one physical arm64 device and run archive import,
  audio, graphics, storage, notification, and IR smoke tests.
- Archive/sign the final iOS build, run it on a physical device, then complete
  TestFlight/App Store validation, privacy manifest, export-compliance, and
  store-metadata review.
- Run the tagged macOS signing/notarization workflow and Gatekeeper validation
  on the exact candidate commit.
- Build and smoke-test Windows on a supported Windows runner; it was not
  possible from this macOS host.
- Have the release owner or counsel confirm GPL source-offer/distribution
  mechanics, third-party notices, media codec/patent obligations, privacy
  disclosures, and store terms. This audit is not legal advice.
- Confirm crash reporting/operational monitoring and rollback ownership for
  the production rollout.
