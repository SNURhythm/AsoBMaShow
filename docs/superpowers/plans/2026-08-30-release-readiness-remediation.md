# Release Readiness Remediation Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Follow superpowers:test-driven-development for every production behavior or policy change and superpowers:systematic-debugging for unexpected failures.

**Goal:** Fix confirmed release/security defects, verify native and mobile release paths without distribution, and publish a ready-for-review pull request from one isolated worktree.

**Architecture:** Keep changes at existing platform and dependency boundaries. Android release policy stays in Gradle, its repository-owned wrapper, manifest backup rules, and the existing release contract tests. SQLite remains an upstream amalgamation replacement. Ruby release dependencies remain Bundler-managed. Runtime application architecture is unchanged unless a sanitizer or focused regression proves a native defect.

**Tech Stack:** C++20/CMake/CTest, SQLite amalgamation, Android Gradle Plugin/Gradle/NDK, Android resource XML, Ruby/Bundler/Fastlane/CocoaPods, Python unittest release contracts, shell release helpers.

---

### Task 1: Preserve the baseline and record audit evidence

**Files:**
- Create: `docs/release-readiness-audit-2026-08-30.md`

**Steps:**
1. Complete the clean debug build and `ctest --test-dir cmake-build-debug --output-on-failure -j 6` baseline.
2. Record scanner versions and triage results without copying sensitive matches into the repository.
3. Inspect SQL construction, archive extraction, asynchronous ownership, manual allocation/deallocation, platform manifests, and release workflows.
4. Keep the audit document provisional until all remediation verification is complete.

### Task 2: Upgrade the Android release toolchain and target policy

**Files:**
- Modify: `tests/android_release_workflow_tests.py`
- Modify: `android/build.gradle`
- Modify: `android/app/build.gradle`
- Create: `android/gradlew`
- Create: `android/gradlew.bat`
- Create: `android/gradle/wrapper/gradle-wrapper.jar`
- Create: `android/gradle/wrapper/gradle-wrapper.properties`
- Modify: `scripts/android_firebase_deploy.sh`
- Modify: `.github/workflows/mobile-beta-deploy.yml`
- Modify: `android/README.md`
- Modify: `AGENTS.md`

**Steps:**
1. Add contract tests that require API 36, AGP 8.10.1, Gradle 8.11.1 with its official checksum, pinned NDK 28.2.13676358, and use of the repository wrapper.
2. Run `PYTHONDONTWRITEBYTECODE=1 python3 tests/android_release_workflow_tests.py` and confirm the new tests fail for the old API/toolchain/wrapper.
3. Generate the wrapper with the verified official Gradle distribution; update Gradle, NDK, scripts, workflow, and documentation minimally.
4. Re-run the contract tests and shell syntax checks.
5. Run the wrapper version check, Android unit tests, fatal lint for both flavors, and an unsigned debug native build.
6. Inspect packaged arm64 ELF load alignment with the pinned NDK tools and require at least `0x4000` alignment.
7. Commit as `fix(android): meet API 36 release requirements`.

### Task 3: Exclude Android credentials from backup and transfer

**Files:**
- Modify: `tests/android_release_workflow_tests.py`
- Modify: `android/app/src/main/AndroidManifest.xml`
- Create: `android/app/src/main/res/xml/backup_rules.xml`
- Create: `android/app/src/main/res/xml/data_extraction_rules.xml`

**Steps:**
1. Add an XML contract test proving `files/profiles` is excluded from both legacy cloud backup and API 31+ cloud/device transfer while backups remain enabled.
2. Run the focused test and confirm it fails because the manifest has no rule resources.
3. Add both rule resources and manifest references.
4. Re-run the focused contract and Android fatal lint/build checks.
5. Commit as `security(android): exclude credentials from backups`.

### Task 4: Update the vendored SQLite amalgamation

**Files:**
- Modify: `tests/cross_platform_release_contract_tests.py`
- Modify: `src/sqlite3.c`
- Modify: `src/sqlite3.h`

**Steps:**
1. Add a release contract asserting that the compiled amalgamation is at least SQLite 3.53.4, the audited upstream release.
2. Run the focused contract and confirm it fails against 3.43.1.
3. Download the official 3.53.4 amalgamation, verify its published SHA3-256 and `sqlite3.c` hash, and replace only generated upstream files.
4. Re-run the contract, rebuild affected native targets, and run all database/profile/import/archive tests.
5. Commit as `deps: update SQLite to 3.53.4`.

### Task 5: Resolve iOS release-tool advisories

**Files:**
- Modify: `ios/Xcode/AsoBMaShow/Gemfile`
- Modify: `ios/Xcode/AsoBMaShow/Gemfile.lock`
- Modify: `tests/ios_release_workflow_tests.py`

**Steps:**
1. Add a dependency-policy regression for the minimum fixed ActiveSupport and concurrent-ruby versions and the known fixed transitive versions reported by the audit.
2. Run the focused test and confirm it fails against the current lockfile.
3. Replace obsolete upper-bound compatibility pins with fixed compatible constraints and re-resolve using Ruby 3.4.9.
4. Re-run OSV Scanner against `Gemfile.lock` until it reports no known vulnerabilities.
5. Run `bundle check`, require Fastlane and CocoaPods, then execute the no-upload iOS release verifier.
6. Commit as `deps(ios): resolve release tool advisories`.

### Task 6: Run native memory-safety and release verification

**Files:**
- Modify if warranted: only files implicated by a reproducible sanitizer/static-analysis failure
- Modify: `docs/release-readiness-audit-2026-08-30.md`

**Steps:**
1. Configure a separate debug tree with AddressSanitizer and UndefinedBehaviorSanitizer plus frame pointers.
2. Build and run the complete 186-test CTest suite under sanitizers.
3. For any first-party failure, reproduce it in a focused test, fix it test-first, and commit by defect area. Do not edit amalgamated BMS parser files in this repository.
4. Run all Python release-policy suites and `shellcheck scripts/*.sh`; apply only narrow, behavior-preserving cleanup and keep it in a separate commit if needed.
5. Run the clean desktop build/tests, macOS release artifact checks, Android checks, and iOS no-upload verifier from the final tree.
6. Complete the audit report with fixed findings, non-findings, scanner evidence, commands/results, and remaining external gates.

### Task 7: Review, publish, and complete the goal

**Files:**
- Review: all branch changes relative to `origin/main`

**Steps:**
1. Use superpowers:verification-before-completion and run fresh final verification.
2. Use superpowers:requesting-code-review for a full diff/self-review; fix any confirmed issue with the same test-first discipline.
3. Confirm the original checkout is untouched and commits are logically separated.
4. Push `codex/release-readiness-audit` to `origin`.
5. Open a non-draft GitHub pull request targeting `main`, with findings, fixes, verification, and external gates in the body.
6. Mark the Codex goal complete only after the ready-for-review PR exists.
