# Gameplay skin decode cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the ~2.9 s gameplay-skin re-decode that runs on every chart attempt. Persist decoded skin-owned images and bitmap-font pages keyed by skin revision so a subsequent attempt with the same skin takes ~130 ms. Evict the cache only when the user changes the skin.

**Architecture:** `PlaySkinSession::create`'s `decodeAndPlan` (2.96 s) is the dominant cost. Its expensive inputs are (a) skin-owned image decodes and (b) bitmap-font page decodes. Both depend only on the skin's revision + package paths — never on the chart's title/artist (those only affect cheap glyph-metric rebuilds). Today both are re-decoded per chart: `BitmapFontPreparationCache` is scoped per `decodeAndPlan` call, and `ImageDecodeCoordinator` is a live queue that erases completed work. `SkinResourcePreparationService` is a single app-level instance (`ApplicationContext::skinResourcePreparationService`), so we add a **`SkinDecodeCache` member on the service**, keyed by skin revision, and consult it in `decodeAndPlan` so page/image decodes are skipped on cache hit. The per-chart atlas (title glyphs) is still rebuilt from cached pages, preserving correctness. The cache is evicted only when the selected skin's revision changes.

**Tech Stack:** C++23, existing `SkinResourceUploadPlan`, `BitmapFontPreparationCache`, `ImageDecodeCoordinator`, CTest, `gameplay_skin_loading_benchmark_tests` for timing verification.

**Spec:** performance requirement from the branch review: selector→gameplay must not hang on every chart attempt.

## Global Constraints

- The gameplay skin must be fully loaded before the first gameplay frame. No built-in fallback or placeholder skin is shown while loading.
- Cached decoded resources must be keyed by skin revision (and per-package path) so a different skin or an edited skin never reuses stale pixels.
- The per-chart atlas corpus (chart title/artist) must still be produced correctly; only the expensive page/image *decode* is cached, never the glyph metrics.
- Do not change the pinned Beatoraja compatibility behaviors or the music-select skin surface.
- No whole-file formatters — format only changed lines.
- Verify with the benchmark: LITONE12 `PlaySkinSession::create` must drop from ~3.4 s to near ~130 ms on a cache hit, and remain correct on a cold miss.

---

### Task 1: Skin decode cache on the resource-preparation service

**Files:**
- Create: `src/skin/beatoraja/SkinDecodeCache.h`
- Create: `src/skin/beatoraja/SkinDecodeCache.cpp`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h` (member + `decodeAndPlan` input)
- Modify: `src/skin/beatoraja/SkinResourceCatalog.cpp` (use cache in `readBitmapFontFaces` + image decode)
- Modify: `src/skin/CMakeLists.txt` (register new sources)

**Interfaces:**
- Consumes: `SkinRevisionLease` (revision identity), `image_decode::DecodedImageData`, `SkinParsedBitmapFont`.
- Produces: `class skin::SkinDecodeCache` with:
  - `[[nodiscard]] const SkinDecodeCacheEntry *entry(std::string_view revisionSha256) const;`
  - `SkinDecodeCacheEntry &mutableEntry(std::string_view revisionSha256);`
  - `void dropAll();`
  where `SkinDecodeCacheEntry` holds:
  - `std::map<std::string, image_decode::DecodedImageData, std::less<>> fontPages;` (key = normalized page path)
  - `std::map<std::string, image_decode::DecodedImageData, std::less<>> skinImages;` (key = normalized image path)
  - `std::size_t decodedBytes = 0;`

- [ ] **Step 1: Write the failing test**

```cpp
void testDecodeCacheKeepsEntriesByRevisionAndEvictsOnChange() {
  skin::SkinDecodeCache cache;
  auto &entryA = cache.mutableEntry("aaaa");
  entryA.fontPages.emplace("font/page1.cim", decodedImage());
  auto &entryB = cache.mutableEntry("bbbb");
  expect(entryA.fontPages.size() == 1,
         "cache retains the first revision entry after a second revision");
  expect(cache.entry("aaaa") == &entryA &&
             cache.entry("bbbb") == &entryB,
         "entry lookup returns the same entry for the same revision");
  cache.dropAll();
  expect(cache.entry("aaaa") == nullptr,
         "dropAll clears every revision entry");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build cmake-build-debug --target skin_decode_cache_tests -j 6 && ./cmake-build-debug/skin_decode_cache_tests`
Expected: FAIL — `SkinDecodeCache` does not exist yet.

- [ ] **Step 3: Implement `SkinDecodeCache`**

```cpp
class SkinDecodeCache {
public:
  using Key = std::string; // lowercased revision sha256
  [[nodiscard]] const SkinDecodeCacheEntry *entry(std::string_view key) const;
  SkinDecodeCacheEntry &mutableEntry(std::string_view key);
  void dropAll();
  [[nodiscard]] std::size_t decodedBytes() const noexcept;
private:
  std::map<Key, SkinDecodeCacheEntry> entries_;
};
```

`mutableEntry` creates the entry on first use; keys are lowercased revision sha256.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build cmake-build-debug --target skin_decode_cache_tests -j 6 && ./cmake-build-debug/skin_decode_cache_tests`
Expected: PASS.

- [ ] **Step 5: Register target in `CMakeLists.txt` and commit**

Add `add_executable(skin_decode_cache_tests ...)` next to the other skin tests. Commit: `Add skin decode cache`.

### Task 2: Route font-page decodes through the cache

**Files:**
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h` — `decodeAndPlan` gains an optional `const SkinDecodeCache*` and the cache member is added to `SkinResourcePreparationService`.
- Modify: `src/skin/beatoraja/SkinResourceCatalog.cpp` — `readBitmapFontFaces` reads font pages from the cache before the coordinator.

**Interfaces:**
- Consumes: `SkinDecodeCache` from Task 1, exposed as `SkinResourcePreparationService::decodeCache()`.
- Produces: `decodeAndPlan` no longer re-reads/re-decodes a cached font page; `readBitmapFontFaces` consults `decodeCache().entry(revisionKey)->fontPages` first and stores fresh decodes in `decodeCache().mutableEntry(revisionKey)->fontPages`. The cache is a member of the service, so no new parameter is threaded through `PlaySkinSessionContext`.

- [ ] **Step 1: Write the failing test**

In `tests/skin_resource_catalog_tests.cpp`, add a test that runs `decodeAndPlan` twice with the same skin and a shared `SkinDecodeCache`, and asserts the second run's `resourcePreparationMicros` (via `SkinLoadingTelemetry`) is substantially lower, OR that a fake device's `create` count doesn't double for font pages. Simpler: assert the second `decodeAndPlan` produces identical `plan->atlases` while the coordinator receives no new page-decode tickets (instrument via `readBitmapFontFaces`'s cache hit path returning the cached page).

```cpp
void testBitmapFontPagesAreCachedAcrossDecodeRuns() {
  skin::SkinDecodeCache cache;
  auto plan1 = service.decodeAndPlan({...skin A..., .decodeCache = &cache});
  auto plan2 = service.decodeAndPlan({...skin A..., .decodeCache = &cache});
  expect(plan1.plan && plan2.plan &&
             plan1.plan->atlases.size() == plan2.plan->atlases.size() &&
             plan1.plan->atlases.front().glyphs.size() ==
                 plan2.plan->atlases.front().glyphs.size(),
         "second decode run reuses cached font pages and produces the same atlas");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests -j 6 && ./cmake-build-debug/skin_resource_catalog_tests`
Expected: FAIL — the second run re-decodes, and the atlas is identical anyway, so add an assertion that the coordinator's decode work count stayed flat (use `ImageDecodeCoordinator::workerCount`/a test hook or a counting fake). If a counting hook is needed, add `pendingCount()`/`readyBytes()` instrumentation under `ASOBMASHOW_SKIN_RESOURCE_TESTING`.

- [ ] **Step 3: Implement the cache lookup in `readBitmapFontFaces`**

In the per-page loop, before `coordinator.request`, check the app cache:

```cpp
const std::string revisionKey = files.revision().lowercaseSha256;
if (const auto cached =
        decodeCache != nullptr ? decodeCache->entry(revisionKey) : nullptr;
    cached != nullptr) {
  const auto found = cached->fontPages.find(combined);
  if (found != cached->fontPages.end()) {
    preparedPage.pixels = found->second;
    continue;
  }
}
```

On a successful page decode (after `coordinator.waitTake`), store it:

```cpp
if (decodeCache != nullptr) {
  decodeCache->mutableEntry(revisionKey)
      .fontPages.emplace(pending.path, *waited.image);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests -j 6 && ./cmake-build-debug/skin_resource_catalog_tests`
Expected: PASS — the second run reuses cached pages.

- [ ] **Step 5: Commit**

`Wire bitmap font pages through the app-level decode cache`

### Task 3: Route skin image decodes through the cache

**Files:**
- Modify: `src/skin/beatoraja/SkinResourceCatalog.cpp` — the image-decode loop in `decodeAndPlan` consults `cache->skinImages` before `coordinator.request`.

**Interfaces:**
- Consumes: `SkinDecodeCache` from Task 1.
- Produces: skin-owned images (those whose `candidate.normalizedVirtualPath` is a package path) are cached and skipped on cache hit; chart-owned builtin images (stage/back/banner) are never cached (per-chart).

- [ ] **Step 1: Write the failing test**

Extend `testBitmapFontPagesAreCachedAcrossDecodeRuns` (or add a sibling) to assert the image-decode count stays flat on the second run, using the same counting mechanism.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests -j 6 && ./cmake-build-debug/skin_resource_catalog_tests`
Expected: FAIL — images re-decode on the second run.

- [ ] **Step 3: Implement the image cache lookup**

In the image-decode loop, before `coordinator_.request`, check `cache->skinImages` keyed by `*candidate.normalizedVirtualPath`; on hit, use the cached `DecodedImageData` directly. On a fresh decode, store it in `cache->skinImages`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests -j 6 && ./cmake-build-debug/skin_resource_catalog_tests`
Expected: PASS.

- [ ] **Step 5: Commit**

`Wire skin image decodes through the app-level decode cache`

### Task 4: Cache eviction on skin change

**Files:**
- Modify: `src/skin/GameplaySkinLifecycle.cpp` — when `acquireForSkinType` yields an activation whose revision differs from the previously active skin, call the service's `decodeCache().dropAll()`.
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h` — expose `SkinDecodeCache& decodeCache();` on `SkinResourcePreparationService`.

**Interfaces:**
- Consumes: `SkinDecodeCache::dropAll()` from Task 1.
- Produces: switching the selected skin (or editing it) evicts the stale cached decodes; playing the same skin reuses them.

- [ ] **Step 1: Write the failing test**

In `tests/gameplay_skin_lifecycle_tests.cpp`, assert that acquiring skin B after skin A calls `dropAll` on the decode cache, and acquiring skin A again does not (no eviction for the same revision). Add a test seam: the lifecycle dependency gains `std::function<void()> dropDecodeCache`, defaulting to a no-op, and the test wires it to a counter.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build cmake-build-debug --target gameplay_skin_lifecycle_tests -j 6 && ./cmake-build-debug/gameplay_skin_lifecycle_tests`
Expected: FAIL — no eviction hook exists.

- [ ] **Step 3: Implement the eviction hook**

In `GameplaySkinLifecycle`, compare the new activation's `revision.revision().lowercaseSha256` with the previous `currentIdentity.revisionDigest`; if different (or no prior identity), invoke `dropDecodeCache` (bound in production to `service.decodeCache().dropAll()`).

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build cmake-build-debug --target gameplay_skin_lifecycle_tests -j 6 && ./cmake-build-debug/gameplay_skin_lifecycle_tests`
Expected: PASS.

- [ ] **Step 5: Commit**

`Evict the skin decode cache when the selected skin changes`

### Task 5: Verify timing and full suite

**Files:**
- Verify only.

- [ ] **Step 1: Benchmark the cache hit**

Run: `./cmake-build-debug/gameplay_skin_loading_benchmark_tests --acceptance-report --skin /Users/xf/Downloads/Skins/LITONE12 --entry Play/play7.luaskin --entry-identity LITONE12-play7 --format lua`
Then re-run the same command (now a cache hit). Confirm `loading.resourcePreparationMicros` collapses from ~2.96 s toward ~50 ms on the second run.

- [ ] **Step 2: Run the full suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 6`
Expected: all 313 tests pass.

- [ ] **Step 3: Run acceptance skins**

Run: `ASOBMASHOW_SKIN_ACCEPTANCE_ROOT=/Users/xf/Downloads/Skins ./cmake-build-debug/play_skin_session_tests`
Expected: PASS.

- [ ] **Step 4: Commit and push**

Commit any remaining test/adjustment, push the branch, and report timing before/after.