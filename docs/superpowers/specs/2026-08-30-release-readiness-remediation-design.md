# Release Readiness Remediation Design

**Date:** 2026-08-30

**Status:** Approved for execution by the user's instruction to audit, fix, verify, and open a ready-for-review pull request without approval pauses.

## Goal

Bring the current `main` branch to a defensible release-candidate state by finding and fixing reproducible security, memory-safety, dependency, platform-policy, and release-engineering defects. Preserve one isolated worktree and produce reviewable commits rather than a broad cleanup commit.

## Evidence and risk model

The audit treats user-selected chart folders, imported archives, profile archives and databases, remote ranking responses, stored credentials, and release credentials as untrusted or sensitive boundaries. It combines:

- full desktop build and CTest baselines;
- address/undefined-behavior sanitizer execution for native lifetime defects;
- focused inspection of SQL construction, archive paths, asynchronous lifetimes, allocation ownership, JNI entry points, and exported Android components;
- secret-history and dependency-advisory scanners;
- release-policy and artifact/build-only checks for Android, iOS, and macOS where local signing or store access is not required.

Generated or third-party code is changed only through an upstream release artifact. Large first-party files are not refactored merely because they are large; a release branch should not absorb speculative churn.

## Confirmed remediation areas

### Android Play compliance and native compatibility

The Play build still compiles and targets API 35. Google Play requires new apps and updates to target API 36 beginning 2026-08-31. API 36 requires a newer Android Gradle Plugin than the current 8.6.0, and the SDL submodule-owned Gradle 8.7 wrapper cannot be updated atomically in this repository.

Add a repository-owned Android wrapper pinned to Gradle 8.11.1 with the official distribution checksum, update to Android Gradle Plugin 8.10.1, and compile/target API 36. Pin NDK 28.2.13676358, which produces 16 KiB-aligned native libraries by default, instead of silently selecting the lexically newest installed NDK. Build scripts should fail with a useful message when that exact NDK is absent. CI, documentation, and local helpers should invoke the repository-owned wrapper.

### Android credential backup boundary

Android IR credentials are kept in owner-only internal storage under `files/profiles`, but `android:allowBackup="true"` currently includes that directory in Auto Backup. Preserve backup for non-secret app data while excluding `files/profiles` from cloud backup and device-to-device transfer on both the pre-31 and API 31+ rule formats. The credential backend is the only production use of this internal `profiles` path.

### Native SQLite security maintenance

The vendored SQLite 3.43.1 predates multiple upstream memory-safety fixes, including a 3.43.2 JSON parser use-after-free fix. SQL inspection found bound parameters for user values and validated or constant identifiers for dynamic table/column names, so there is no confirmed SQL-injection path. Nevertheless, imported profile databases cross a trust boundary and the obsolete amalgamation is avoidable risk.

Replace only the generated `sqlite3.c` and `sqlite3.h` files with the official SQLite 3.53.4 amalgamation after verifying its published archive and source hashes. Run the complete database and native suites against it.

### iOS release-tool dependencies

The committed Ruby lockfile contains advisory-affected ActiveSupport, concurrent-ruby, Excon, Faraday, json, and jwt versions. Relax the obsolete compatibility pins only as far as needed for fixed versions, re-resolve with the repository Ruby, and validate Fastlane/CocoaPods loading plus the no-upload iOS verifier. These gems do not ship in the app, but they process release credentials and network traffic.

## Non-findings and bounded cleanup

- Dynamic SQL values are parameter-bound; dynamic identifiers inspected in repository helpers are constants or pass identifier validation.
- Archive extraction has lexical/canonical traversal checks and size/count limits.
- The self-deleting archive COM adapter follows reference-count ownership; detached process-reaper threads capture only process IDs; document-handoff work owns shared state across destruction.
- Secret scanning findings consist of public Firebase client configuration, test placeholders, generated dependency strings, and keyword false positives. No private signing key, token, or service credential is committed on the audited history.
- No identical first-party source files or clearly unreachable translation units were found. Shell warnings will be fixed only where the change is semantic and low risk; intentional portability idioms receive narrowly documented suppressions.

## Verification and commit structure

Each behavioral/configuration fix begins with a focused failing contract test, then the minimal implementation. Commits remain independently reviewable:

1. audit design and implementation plan;
2. Android API/toolchain and 16 KiB compatibility;
3. Android credential-backup hardening;
4. SQLite security update;
5. iOS release-tool dependency update;
6. bounded release-script cleanup, audit results, and verification evidence.

No Firebase, TestFlight, Play, notarization, or other distribution upload is authorized by this audit. Missing signing identities, physical-device access, store metadata, Windows hardware, or notarization service access remain explicit external release gates rather than silently skipped successes.
