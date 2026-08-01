# No-Disk Ordinary Artwork Loading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore fast, independently scheduled ordinary jacket and banner loading without persisting ordinary artwork previews.

**Architecture:** Keep archive virtual-path thumbnails in the existing temporary archive cache, while ordinary files use only the process-local decoded-image cache. Make speculative background work newest-first and add phase timing to identify source access separately from pixel and archive-preview work.

**Tech Stack:** C++20, stb_image, bgfx, SDL, CMake/CTest

## Global Constraints

- Ordinary filesystem artwork creates and reads no persisted preview.
- Archive-entry preview behavior and format remain unchanged.
- No UI-thread source metadata probes are introduced.
- Selected artwork retains a separate priority worker.
- Use real FIFO and image fixtures; do not mock worker behavior.

---

### Task 1: Restore the archive-only disk-cache boundary

**Files:**
- Modify: `tests/image_view_fade_tests.cpp`
- Modify: `src/view/ImageView.cpp`
- Modify: `src/ArchiveFile.h`
- Modify: `src/ArchiveFile.cpp`

**Interfaces:**
- Consumes: `ImageView::setImageAsync`, `ImageView::dropAllCache`, `archive_file::materializeFileBytes`
- Produces: archive-only `readCachedArchivedThumbnail` and `writeCachedArchivedThumbnail`; `materializeFileBytes(path, bytes, errorMessage, cancelled)`

- [ ] **Step 1: Replace the ordinary persistence assertion with a failing no-disk cold-reload assertion**

  Decode a 512x256 ordinary PPM, clear the process cache, bind the same path,
  and assert its dimensions remain zero until the worker decodes it again.
  Retain the final assertion that the second worker load reaches 512x256.

- [ ] **Step 2: Run the focused test and verify RED**

  Run: `cmake --build cmake-build-debug --target image_view_fade_tests -j 6 && ./cmake-build-debug/image_view_fade_tests`

  Expected: FAIL because the persisted 256x128 ordinary preview is applied immediately.

- [ ] **Step 3: Remove ordinary preview reads/writes and replacement materialization**

  Restore archive-only thumbnail helpers, call them only for virtual archive
  paths, stop passing an ordinary-persistence flag through the worker, and
  remove `replaceExisting` from `materializeFileBytes`.

- [ ] **Step 4: Add phase timings without changing ordinary cache ownership**

  Record queue, source-open, source-load/decode, RGBA-copy, and archive-preview
  durations. Include them in the existing thresholded slow-load diagnostic.

- [ ] **Step 5: Run the focused test and verify GREEN**

  Run: `cmake --build cmake-build-debug --target image_view_fade_tests -j 6 && ./cmake-build-debug/image_view_fade_tests`

  Expected: PASS.

- [ ] **Step 6: Commit**

  ```bash
  git add src/ArchiveFile.cpp src/ArchiveFile.h src/view/ImageView.cpp tests/image_view_fade_tests.cpp
  git commit -m "perf: keep ordinary artwork cache in memory"
  ```

### Task 2: Prefer newly visible background artwork

**Files:**
- Modify: `tests/image_view_fade_tests.cpp`
- Modify: `src/view/ImageView.cpp`

**Interfaces:**
- Consumes: `ImageDecodeWorker::request(path, key, false)`
- Produces: newest-first scheduling for queued non-priority artwork; unchanged dedicated priority scheduling

- [ ] **Step 1: Write a failing real-worker scheduling regression**

  Occupy all four background workers with FIFO fixtures, enqueue a fifth stale
  FIFO, enqueue a valid newly visible PPM, release one worker, and assert the
  valid PPM finishes before the stale FIFO begins reading.

- [ ] **Step 2: Run the focused test and verify RED**

  Run: `cmake --build cmake-build-debug --target image_view_fade_tests -j 6 && ./cmake-build-debug/image_view_fade_tests`

  Expected: FAIL because the FIFO queue starts the older stale task first.

- [ ] **Step 3: Make speculative background dispatch newest-first**

  Append normal tasks to the deque and continue popping from its back. Leave
  the priority queue and in-flight deduplication unchanged.

- [ ] **Step 4: Run the focused test and verify GREEN**

  Run: `cmake --build cmake-build-debug --target image_view_fade_tests -j 6 && ./cmake-build-debug/image_view_fade_tests`

  Expected: PASS.

- [ ] **Step 5: Commit**

  ```bash
  git add src/view/ImageView.cpp tests/image_view_fade_tests.cpp
  git commit -m "perf: prefer newly visible artwork"
  ```

### Task 3: Verify and publish

**Files:**
- Review: complete diff against `origin/develop`

**Interfaces:**
- Consumes: Tasks 1 and 2
- Produces: pushed ready PR head and successful Firebase check

- [ ] **Step 1: Run focused integration checks**

  Run the image test plus `find_bms_download_tests`,
  `foundation_av_jukebox_restore`, and `foundation_profile_archive`.

- [ ] **Step 2: Run full desktop verification**

  Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 1`

  Run: `cmake --build cmake-build-debug --target main -j 6`

- [ ] **Step 3: Run iOS compile verification**

  Run: `scripts/ios_firebase_deploy.sh --build-only`

- [ ] **Step 4: Review and publish**

  Confirm `git diff --check`, review cache/queue authorities against
  `origin/develop`, push `feature/file-based-replays-v2`, and monitor PR #83's
  initial checks and actionable review feedback.
