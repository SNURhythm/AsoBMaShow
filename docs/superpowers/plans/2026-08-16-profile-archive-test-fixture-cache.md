# Profile Archive Test Fixture Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce `foundation_profile_archive` runtime by creating its fully initialized profile seed once per test process and cloning it for isolated cases.

**Architecture:** Keep all behavior in `tests/profile_archive_tests.cpp`. A static seed owns a production-created profile tree and immutable fixture IDs; each `Fixture` copies its contents into a fresh `TempDirectory` before initializing its own `PlayerProfileManager`. The test suite continues to exercise real archive and profile operations against on-disk, isolated profiles.

**Tech Stack:** C++23, `std::filesystem`, CTest, SQLite-backed profile repositories.

## Global Constraints

- Work in the current `chore/repo-cleanup` checkout; do not create a worktree.
- Do not change production source, replay-schema creation, migration, or durable-write behavior as part of this archive-fixture task. Later independent production optimizations are authorized by the user's explicit expanded request and are verified separately.
- Preserve deterministic fixture IDs, per-fixture UUID sequences, and per-fixture exchange directories.
- Do not change fault-injection behavior in the archive test suite; any cached fault seed must reproduce the prior matrix setup exactly.
- The cached seed must be built once and never mutated by a test case.

---

### Task 1: Cache the profile-archive fixture seed

**Files:**
- Modify: `tests/profile_archive_tests.cpp:70-710`
- Test: `tests/profile_archive_tests.cpp:800-840`, `tests/profile_archive_tests.cpp:3138-3180`

**Interfaces:**
- Consumes: the current `Fixture` setup, `TempDirectory`, `writeFile`, and `PlayerProfileManager` test dependencies.
- Produces: `Fixture::seedBuildCountForTesting()` and a `Fixture` that always owns a private, initialized profile tree.

- [ ] **Step 1: Add the failing fixture-cache regression test before other archive cases**

```cpp
void testFixtureSeedIsBuiltOnceAndClonesAreIsolated() {
  Fixture first;
  Fixture second;

  expect(Fixture::seedBuildCountForTesting() == 1,
         "profile archive fixture seed builds exactly once");
  const auto firstSettings = first.manager.pathsFor(first.sourceId).settingsJson;
  const auto secondSettings = second.manager.pathsFor(second.sourceId).settingsJson;
  writeFile(firstSettings, "{\"modified\":true}\n");
  expect(readFile(secondSettings).find("\"modified\":true") == std::string::npos,
         "profile archive fixture copies remain isolated");
}
```

Call it first from `main()`. At this point `seedBuildCountForTesting` is not defined, so the target must fail to compile for the intended missing cache interface.

- [ ] **Step 2: Build and verify the regression test is red**

Run: `cmake --build cmake-build-debug --target profile_archive_tests -j 6`

Expected: build failure stating that `Fixture::seedBuildCountForTesting` is not a member of `Fixture`.

- [ ] **Step 3: Implement the immutable seed and isolated clone path**

Extract the current fixture setup into a seed object that is initialized once and retains the existing active, source, and target profile IDs and payload setup. Copy the seed directory contents entry-by-entry, rather than copying the seed root itself, into each fixture's already-created temporary directory.

```cpp
struct ProfileArchiveFixtureSeed {
  TempDirectory root{"profile-archive-seed"};
  std::string sourceId;
  std::string targetId;

  ProfileArchiveFixtureSeed(); // Existing production-manager setup and seeding.
};

const ProfileArchiveFixtureSeed &profileArchiveFixtureSeed();

void copyDirectoryContents(const std::filesystem::path &from,
                           const std::filesystem::path &to) {
  for (const auto &entry : std::filesystem::directory_iterator(from)) {
    std::filesystem::copy(entry.path(), to / entry.path().filename(),
                          std::filesystem::copy_options::recursive);
  }
}
```

Have `Fixture` clone the seed before `manager.Initialize()`, then set its `sourceId` and `targetId` from the seed. Preserve `Fixture`'s local UUID generator so imports still use the same test-controlled IDs. Expose a read-only seed-build counter for the regression test. Handle `std::filesystem` errors through existing `expect` failure reporting, leaving the fixture invalid only when cloning or initialization fails.

- [ ] **Step 4: Build and verify the regression is green**

Run: `cmake --build cmake-build-debug --target profile_archive_tests -j 6 && ctest --test-dir cmake-build-debug -R '^foundation_profile_archive$' --output-on-failure -j 1`

Expected: build succeeds and the focused test passes with no profile archive failures.

- [ ] **Step 5: Commit the focused implementation**

```bash
git add tests/profile_archive_tests.cpp
git commit -m "test: cache profile archive fixture setup"
```

### Task 2: Measure and verify suite-wide behavior

**Files:**
- Verify: `tests/profile_archive_tests.cpp`
- Verify: `cmake-build-debug/CTestTestfile.cmake`

**Interfaces:**
- Consumes: the fixture cache produced in Task 1.
- Produces: fresh timing evidence for the focused test and full serial test suite.

- [ ] **Step 1: Capture focused runtime**

Run: `/usr/bin/time -lp ctest --test-dir cmake-build-debug -R '^foundation_profile_archive$' --output-on-failure -j 1`

Expected: 1/1 test passes and the reported CTest time is materially below the 30.76-second baseline.

- [ ] **Step 2: Run the full serial CTest regression suite**

Run: `/usr/bin/time -lp ctest --test-dir cmake-build-debug --output-on-failure -j 1`

Expected: 255/255 tests pass. Compare the fresh total with the 127.79-second baseline and report both times.

- [ ] **Step 3: Inspect the final change set**

Run: `git diff --check && git status --short`

Expected: no whitespace errors; only the intended fixture-cache implementation and approved design/plan documents are modified or untracked.
