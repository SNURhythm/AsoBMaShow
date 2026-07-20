# Persistent Bokutachi Lookup Cache Implementation Plan

**Goal:** Persist profile-local Bokutachi chart and user identifiers so repeat
native ranking opens normally need only the PB request.

**Architecture:** A bounded, versioned `BokutachiCacheStore` owns the
`bokutachi-cache.json` representation and an in-memory synchronized lookup.
`TachiDriver` receives the shared store, skips resolved prerequisites on hits,
and repairs a stale cached chart ID once. `ApplicationContext` activates the
store with the current profile and clears user identities after credential
changes.

**Tech stack:** C++23, nlohmann JSON, existing `VersionedJson`/`AtomicFile`,
CMake/CTest.

## Task 1: Add the bounded profile-local cache store

**Files:**

- Create: `src/ir/tachi/BokutachiCacheStore.h`
- Create: `src/ir/tachi/BokutachiCacheStore.cpp`
- Create: `tests/bokutachi_cache_store_tests.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

1. Write tests for a missing-file activation, persistence/reload, normalized
   lookup keys, unchanged-value no-op, chart eviction, user-ID clearing,
   malformed and oversized replacement, future-version preservation, bounds,
   and absence of credential fields.
2. Run `cmake --build cmake-build-debug --target bokutachi_cache_store_tests -j 6`
   to demonstrate the new target or symbols are missing.
3. Implement the mutex-protected store with schema version 1, 1 MiB input,
   16 origins, 2,048 chart mappings, strict value validation, and atomic writes.
4. Re-run `./cmake-build-debug/bokutachi_cache_store_tests` and make it pass.

## Task 2: Use cached prerequisites in native ranking requests

**Files:**

- Modify: `src/ir/tachi/TachiDriver.h`
- Modify: `src/ir/tachi/TachiDriver.cpp`
- Modify: `tests/tachi_driver_tests.cpp`
- Modify: `CMakeLists.txt`

1. Add driver tests proving full hits issue only the PB request, partial hits
   issue only the missing prerequisite, successful cold lookups populate the
   store, and a cached chart 404 evicts/resolves/retries exactly once.
2. Run the focused driver target and confirm the tests fail for absent cache
   behavior.
3. Inject a shared optional cache store into `TachiDriver`, consult it after
   query normalization, remember successful prerequisites without turning
   cache failures into ranking failures, and add the bounded stale-chart retry.
4. Re-run `./cmake-build-debug/tachi_driver_tests` and the cache-store tests.

## Task 3: Wire cache lifetime to player profiles and credentials

**Files:**

- Modify: `src/PlayerProfile.h`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/SettingsSceneIr.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `tests/profile_archive_tests.cpp`

1. Add failing path/archive tests for the exact `bokutachi-cache.json` name and
   its exclusion from portable profile archives.
2. Add `bokutachiCacheJson` to `PlayerProfilePaths` and path construction.
3. Let `ApplicationContext` own the shared store, inject it into the driver,
   activate it before profile IR services start, and clear user IDs on both UI
   and observed credential-change callbacks.
4. Run the focused profile-manager and profile-archive tests.

## Task 4: Verify and commit

1. Run `git diff --check`.
2. Run the focused cache, driver, profile-manager, profile-archive, IR settings,
   ranking-service, and submission-service tests.
3. Run `ctest --test-dir cmake-build-debug --output-on-failure`.
4. Run `cmake --build cmake-build-debug --target main -j 6`.
5. Review the diff for cache secrecy, bounded retry behavior, and accidental
   profile-archive inclusion.
6. Commit the implementation locally without pushing or deploying.

## Verification Result

- [x] Cache-store, driver, profile, credential lifecycle, and archive tests
      pass, including encoded-size and failed-invalidation recovery cases.
- [x] All 105 CTest tests pass.
- [x] The desktop `main` target builds successfully.
- [x] Independent review found no remaining Important-or-higher issue.
