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

- Base: `origin/develop` at `e018a47fad69`
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
| High | Android targeted API 35 immediately before Google Play's 2026-08-31 API 36 deadline and lacked a repository-owned, checksum-pinned Gradle wrapper. | Moved to compile/target API 36, AGP 8.10.1, Gradle 8.11.1 with verified checksums, and NDK r28. Gradle and build-mode deploys now validate the installed NDK revision from `source.properties`; AGP is explicitly bound to that validated path, including standalone NDK installations. Artifact-only uploads remain independent of the local build toolchain. The native libraries pass 16 KiB ELF alignment checks. |
| High | Vendored SQLite 3.43.1 predates SQLite's 3.43.2 JSON-parser use-after-free fix and later memory-safety fixes. | Updated the exact upstream amalgamation to SQLite 3.53.4 and pinned a release-policy minimum. |
| High | The iOS release bundle contained dependencies with current OSV advisories in ActiveSupport, concurrent-ruby, Excon, Faraday, JSON, and JWT. | Updated Fastlane and the affected dependency graph. OSV Scanner reports no known vulnerability in the resulting `Gemfile.lock`. |
| High | The manifest's 2023 LuaJIT revision crashed in optimized Apple Silicon builds when Lua skins called core built-ins such as `rawget` and `type`. | Pinned LuaJIT 2026-07-11 without advancing the rest of the vcpkg baseline. The macOS workflow now pins a registry commit containing that version while fetching the manifest baseline separately. Its Release interpreter smoke check and the complete optimized macOS test suite now pass. |
| Medium | Android's private IR API-key file was eligible for cloud backup and device transfer. | Kept general user-data backup enabled but excluded the entire `profiles` subtree in both legacy and Android 12+ backup rule formats. |
| Medium | Desktop/iOS downloads could follow an HTTPS response to HTTP; HTTPS difficulty-table metadata could also load an HTTP data document. | Redirect policy is now derived from the initial URL across curl, `NSURLSession`, and Android: HTTPS origins reject HTTP and non-HTTP downgrades, while direct legacy HTTP origins retain HTTP/HTTPS compatibility. Mixed-content difficulty-table references are rejected before a request is made, with URL schemes compared case-insensitively. |
| Medium | Fresh `develop` introduced an arm64 libc++ compile failure by mixing `optional<long long>` and `optional<int64_t>` in replay provenance selection. | Normalized the provenance branch to the replay seed type explicitly. Both Android product flavors now complete native compilation and packaging. |
| Medium | Fresh `develop` split an LR2 graph rectangle that the pinned Beatoraja loader intentionally shares, changing compatibility behavior. | Restored the shared graph dimensions and verified them against the pinned Java oracle. |
| Medium | The Java-pattern adapter used Java 19+ ASCII word-boundary behavior even though pinned Beatoraja requires Java 17, where the default boundary predicate includes Unicode letters/digits and contextually attached non-spacing marks. | Implemented Java 17's separate legacy and `UNICODE_CHARACTER_CLASS` boundary predicates, including its UTF-16 treatment of supplementary marks; pinned macOS CI to Zulu 17; and passed both native backends against alphabetic-symbol, connector-punctuation, join-control, number-category, combining-mark, `\b`, and `\B` oracle cases. |
| Low | Split settings translation units emitted duplicate internal-linkage helper bodies and unused-function warnings; other first-party functions and locals were dead. | Converted header-defined helpers to ODR-safe `inline` functions and removed unused wrappers, locals, and the obsolete course replay builder. |
| Low | Release scripts emitted actionable ShellCheck diagnostics and could inherit a hostile caller `CDPATH`. | Corrected the scripts, isolated directory changes from caller `CDPATH`, documented the one intentionally literal CocoaPods expression, and added a hostile-environment regression. ShellCheck is clean. |
| Low | The resource-catalog test passed only after the desktop app had copied runtime assets into the build directory. | Made its runtime asset working directory explicit so the clean iOS release verifier is independent of target build order. |
| Low | Four macOS Release tests assumed the Debug build directory or repository assets copied beside their binaries. | Injected the active target directory into the ledger contracts and made repository-asset working directories explicit. A clean Release CTest run now passes 282/282. |
| Low | New release-contract test wiring relied on transitive curl headers and referenced Lua-only test targets even when Lua gameplay skins were disabled. | Linked the curl contract to its imported dependency, guarded optional target paths, and completed a fresh test-enabled CMake generation with Lua gameplay skins disabled. |
| Low | The committed gameplay-oracle verification also tried to regenerate its trace from an unowned sibling Beatoraja checkout, making clean CI fail after all repository-owned artifacts had passed. | The regeneration check now runs when the exact pinned external checkout is available and otherwise reports an explicit skip; committed trace, provenance, fixture, and native-differential checks remain mandatory. |
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

Gitleaks 8.30.1 scanned the complete Git history at the original audit point
(3,194 commits). Twelve
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
- LuaJIT is pinned to the 2026-07-11 upstream revision; its optimized arm64
  interpreter executes `rawget`, `type`, and `tonumber` successfully. A clean
  workflow-style vcpkg checkout resolved both its registry version and the
  historical manifest baseline.
- Direct vcpkg packages and shipped mobile libraries were reviewed against the
  pinned manifest/baseline. Findings in nested third-party development-only
  package metadata were excluded when that package manager is not used to
  build or ship AsoBMaShow.
- Gradle and its wrapper JAR were checked against official published
  checksums.

## Verification evidence

| Area | Result |
| --- | --- |
| Desktop configure/build | Clean Debug `main` build and a clean macOS Release app/test build passed. |
| Desktop tests | Debug and clean optimized macOS Release runs each passed 282/282 CTest tests with six-way parallel scheduling; deadline-sensitive and exact fault-injection suites are explicitly isolated. |
| Security regression | SQL-injection-shaped playlist names and HTTPS mixed-content rejection passed. |
| Undefined behavior | Six ownership/storage-focused UBSan targets passed; address/leak sanitizer limitation documented above. |
| Allocator diagnostics | Six ownership/storage-focused targets passed with Apple malloc diagnostics enabled. |
| Android policy tests | 12/12 release workflow tests and 28/28 cross-platform contract tests passed. A native Gradle configuration probe also confirmed that a standalone-style `ANDROID_NDK_HOME` is passed through as AGP's NDK path and CMake's Android NDK. |
| Android builds | Play and Firebase debug unit tests, lint, assembly, and Java/native compilation passed after the rebase. Production signing material is not present in this worktree, so the signed release flavor remains an external gate. |
| Android native ABI | APKs passed 16 KiB zip alignment and v2 signature verification; `libmain.so` has GNU RELRO, immediate binding, and a non-executable stack. The audit's full native-library ELF `LOAD` alignment check passed at 0x4000. |
| iOS policy tests | 49/49 setup, 18/18 workflow, 16/16 artifact-audit, and 3/3 documentation tests passed. |
| iOS build | `scripts/ios_release_verify.sh` passed 66/66 native release-critical tests, an unsigned arm64 device build under Xcode 26, and the resulting `.app` artifact audit without distribution. |
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

## Rebase and review closure

This branch was rebased onto the updated `develop` tip before final
verification. The review findings were reproduced and resolved as focused
commits: safe legacy HTTP redirect behavior was preserved, URL-scheme security
checks now handle mixed case, the installed NDK revision is read from metadata
rather than inferred from its directory name, AGP uses the validated NDK path,
and release scripts are insulated from caller `CDPATH`. The rebase also exposed
and fixed the Android arm64 seed-type compile failure, the LR2 graph-rectangle
compatibility regression, stale skin/lifecycle test contracts, callback-budget
test coupling, the clean-build runtime-font dependency, the optimized arm64
LuaJIT crash, and Release test assumptions about Debug paths and copied assets
described above. The final review also found that macOS CI's older vcpkg tool
checkout could not resolve the newer LuaJIT override; the workflow now fetches
the pinned tool registry and manifest baseline explicitly, with a release
contract guarding that relationship.
The clean-runner verification then exposed two additional oracle assumptions:
the native regex adapter now follows Beatoraja's required Java 17 legacy and
`UNICODE_CHARACTER_CLASS` word boundaries, and trace regeneration is optional
when its separately maintained
pinned Beatoraja checkout is absent.
