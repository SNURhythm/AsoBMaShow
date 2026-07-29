# Parsing Log Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a cross-platform `Export Log` button that saves or shares the current parsing performance log as a text file.

**Architecture:** `PlatformDocumentHandoff` gains a text request and a tested private staging helper whose opaque lifetime cleans the temporary source only after detached export work exits. `MainMenuScene` snapshots the archive debug log, starts that operation, and renders pending/completion status in the existing modal footer.

**Tech Stack:** C++23, custom View UI, `PlatformDocumentHandoff`, native Android/iOS document bridges, tinyfiledialogs, CMake/CTest.

## Global Constraints

- Suggested filename is `AsoBMaShow-performance-log.txt` and MIME type is `text/plain`.
- Maximum staged text size is 4 MiB.
- Temporary source cleanup must outlive nonblocking operation cancellation and scene teardown.
- Existing profile archive export behavior must remain unchanged.
- Do not benchmark or deploy.

---

### Task 1: Add owned text-document staging

**Files:**
- Modify: `src/PlatformDocumentHandoff.h`
- Modify: `src/PlatformDocumentHandoff.cpp`
- Modify: `tests/platform_document_handoff_tests.cpp`

**Interfaces:**
- Produces: `PlatformTextDocumentExportRequest { text, suggestedName, maxBytes }`
- Produces: `platform_document_handoff::ExportTextDocumentAsync(PlatformTextDocumentExportRequest)`
- Produces for tests: `detail::PrepareTextDocumentExportUnder(const PlatformTextDocumentExportRequest &, const std::filesystem::path &)`

- [ ] **Step 1: Write failing staging tests**

Verify exact file bytes and request metadata, then release `sourceLifetime` and verify the issued file and directory disappear. Verify text exceeding `maxBytes` and a filename containing `/` fail without producing a request.

- [ ] **Step 2: Run the focused test and verify RED**

Run `cmake --build cmake-build-debug --target platform_document_handoff_tests -j 6 && ./cmake-build-debug/platform_document_handoff_tests`.

Expected: compilation fails because the text request and preparation API do not exist.

- [ ] **Step 3: Implement staging and text export**

Create a unique private directory with `CreatePrivateImportDirectoryUnder`, create `export-document.txt` exclusively, write exact bytes, secure it, and attach a cleanup owner to `PlatformDocumentExportRequest::sourceLifetime`. Resolve Android cache storage with `GetAndroidCacheDir()` and the filesystem temporary directory elsewhere. For desktop `text/plain`, show a text-document save dialog/filter; retain the profile dialog for other MIME types.

- [ ] **Step 4: Verify GREEN**

Run `cmake --build cmake-build-debug --target platform_document_handoff_tests -j 6 && ./cmake-build-debug/platform_document_handoff_tests`.

---

### Task 2: Wire the Parsing Logs modal

**Files:**
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`

**Interfaces:**
- Consumes: `archive_file::debugLogText()`
- Consumes: `platform_document_handoff::ExportTextDocumentAsync(...)`
- Produces: `startParseLogExport()`, `applyParseLogDocumentHandoff()`, and `refreshParseLogExportControls()`

- [ ] **Step 1: Add modal controls and state**

Add a footer status label, `Export Log` button/text pointers, and a `PlatformDocumentHandoffOperation`. Disable the button and show `Exporting...` while the operation is active.

- [ ] **Step 2: Add snapshot/export/result flow**

Start export with the exact name, MIME, and 4 MiB limit. Poll from `update()`, map success/cancel/failure to the specified status messages, and close the operation during `cleanupScene()`.

- [ ] **Step 3: Compile the desktop app**

Run `cmake --build cmake-build-debug --target main -j 6`.

---

### Task 3: Regression verification and publication

**Files:**
- Review: all files changed by Tasks 1 and 2

**Interfaces:**
- Verifies: platform handoff, scheduler/scanner behavior, desktop compile, and clean Git scope

- [ ] **Step 1: Run focused tests**

Run `cmake --build cmake-build-debug --target platform_document_handoff_tests chart_scan_work_scheduler_tests archive_file_concurrency_tests chart_library_scanner_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(platform_document_handoff_tests|chart_scan_work_scheduler_tests|archive_file_concurrency_tests|chart_library_scanner_tests)$' --output-on-failure`.

- [ ] **Step 2: Run final checks**

Run `cmake --build cmake-build-debug --target main -j 6` and `git diff --check`.

- [ ] **Step 3: Commit and push**

Commit only the spec, plan, handoff, modal, and test files; push `perf/dynamic-chart-scan-scheduling`; update PR #84 with the new log-export capability and validation.
