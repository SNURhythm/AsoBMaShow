# Task 25 Compatibility Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct the one archive-ingress race, restore authored touch precedence, and realign the remaining Task 25 contracts and tests with the pinned Beatoraja runtime rather than obsolete safety assumptions.

**Architecture:** The normal Lua skin runtime remains a live, Files-visible implementation of Beatoraja's `RestrictedIoLib`: it permits normal selected-skin-directory file I/O and has no render-phase filesystem freeze. Archive import is separate ingress code, so it may reject a symlink before reading an untrusted ZIP without changing runtime compatibility. Test fixtures must state the current intentional behavior—`io.lines`, live visible sources, shader-ready BGA preflight, and authenticated built-in selector catalogs—rather than preserving superseded contracts.

**Tech Stack:** C++23, LuaJ-compatible C API, POSIX file descriptors, CMake/CTest, Python 3 contract tests, pinned Beatoraja source tree.

## Global Constraints

- The only compatibility authority is `/Users/xf/workspace/SNURhythm/beatoraja` at `c2ed5db1a46145ed10790c3872f717e95b59db9d`; begin every compatibility-sensitive task with `python3 scripts/check_beatoraja_reference.py --root ../beatoraja --require-clean`.
- Do not reintroduce a runtime overlay, private copy, render-phase I/O denial, file-I/O quota, profile-I/O isolation, or automatic rescan. Pinned `SkinLuaAccessor.RestrictedIoLib.openFile` uses ordinary selected-root I/O and `LuaSkinLoader.load` re-executes live source files.
- Keep `Documents/Skins` as the single source for installed skins. Archive ingress no-follow protection applies only before a ZIP is accepted for import.
- Preserve unrelated untracked UTF8proc files and the privacy-policy worktree. Do not deploy, upload, or run a clean 60-target iOS rebuild for these desktop/documentation changes.
- Redirect verbose build/test output to `/tmp` and print only failures or a short tail. Use `cmake --build cmake-build-debug --target main -j 12` for the desktop compile loop.

---

### Task 1: Reject symlink archive sources at POSIX import ingress

**Files:**
- Modify: `src/skin/package/SkinArchiveImporter.cpp:273-285`
- Modify: `tests/skin_archive_importer_tests.cpp`

**Interfaces:**
- Consumes: `copyArchiveSource(const std::filesystem::path &, std::stop_token, bool &, SkinImportIoObserver *, std::vector<SkinDiagnostic> &)`, `O_NOFOLLOW`, and POSIX `ELOOP`.
- Produces: `skin_archive_input_invalid` without reading or publishing an archive whose supplied path is a symbolic link; a regular ZIP still follows the existing `open`/`fstat`/copy stability protocol.

- [x] **Step 1: Add the focused failing symlink-input test**

  In `tests/skin_archive_importer_tests.cpp`, create a regular ZIP with `makeZip`, create `archive-link.zip` with `std::filesystem::create_symlink(regularZip, archiveLink)`, call `prepareZip(archiveLink, roots)`, and require all three facts:

  ```cpp
  expect(!result.package, "archive source symlink is rejected before copy");
  expect(hasDiagnosticCode(result, "skin_archive_input_invalid"),
         "archive source symlink uses the stable invalid-input diagnostic");
  expectRejectedAndClean(result, roots,
                         "archive source symlink leaves no issued import state");
  ```

- [x] **Step 2: Run the test to prove RED**

  Run:

  ```sh
  cmake --build cmake-build-debug --target skin_archive_importer_tests -j 12 >/tmp/archive-build.log 2>&1 && \
  cmake-build-debug/skin_archive_importer_tests
  ```

  Expected: the new symbolic-link assertion fails because `open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)` follows the link.

- [x] **Step 3: Add the minimal POSIX no-follow flag**

  Change only the POSIX input open at `SkinArchiveImporter.cpp:273-285` to:

  ```cpp
  const int source = ::open(path.c_str(),
                            O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  ```

  Keep the existing `source < 0` failure branch and its `skin_archive_input_invalid` diagnostic. `O_NOFOLLOW` makes a final-component symlink fail with `ELOOP`; do not add a runtime skin-file restriction or path policy here.

- [x] **Step 4: Run GREEN and the importer race suite**

  Run:

  ```sh
  cmake --build cmake-build-debug --target skin_archive_importer_tests -j 12 >/tmp/archive-build.log 2>&1 && \
  cmake-build-debug/skin_archive_importer_tests
  ```

  Expected: the new symlink case, existing source-change case, and existing staging-root race cases pass.

- [x] **Step 5: Commit the isolated ingress fix**

  ```sh
  git add src/skin/package/SkinArchiveImporter.cpp tests/skin_archive_importer_tests.cpp
  git commit -m "fix: reject linked skin archive sources"
  ```

### Task 2: Restore first-authored touch ownership for skin lane overlaps

**Files:**
- Modify: `src/scene/play/RealtimeTouchInputRouter.cpp:438-451`
- Modify: `src/scene/play/VirtualControllerLayout.cpp:302-312`
- Test: `tests/realtime_touch_input_router_tests.cpp:769-814`

**Interfaces:**
- Consumes: `RealtimeTouchLayout::laneRegions`, whose header declares vector order as the owner of shared edges and overlaps.
- Produces: `RealtimeTouchInputRouter::laneIndexAt` returns the first containing authored skin lane after its existing vertical clamp; overlapping virtual controls retain nearest-control priority and require an actual hit. The legacy uniform adapter keeps its separate reversed insertion behavior.

- [x] **Step 1: Preserve the failing authored-overlap regression**

  Keep `testAuthoredLaneRegionsUseFirstMatchForEdgesAndOverlaps` as the regression. Its observed sequence is `31, 31, 33` at current HEAD; its required sequence is `31, 31, 31`.

- [x] **Step 2: Run RED**

  Run:

  ```sh
  cmake --build cmake-build-debug --target realtime_touch_input_router_tests -j 12 >/tmp/touch-build.log 2>&1 && \
  cmake-build-debug/realtime_touch_input_router_tests
  ```

  Expected: fail with `the first authored region owns shared edges and overlap priority`.

- [x] **Step 3: Separate authored and virtual overlap selection**

  Preserve the existing nearest-center comparison only for `requiresInside` virtual controls. Retain the first matching `!requiresInside` authored skin region, return a virtual control when present, otherwise return the first authored region. Set every virtual-controller region—including scratch—to `requiresInside = true` so it cannot use skin vertical-clamp fallback.

  ```cpp
  std::optional<std::size_t> firstAuthoredSkin;
  std::optional<std::size_t> nearestVirtualControl;
  // Direct hits select nearest `requiresInside` region, then the first
  // authored region. The existing clamped pass returns its first authored hit.
  ```

  Do not reverse skin-authored order or apply vertical clamping to a virtual controller.

- [x] **Step 4: Run GREEN**

  Run the target from Step 2. Expected: it passes, including skin geometry, virtual-controller circle, flick, spin, and cancellation tests.

- [x] **Step 5: Commit the interaction correction**

  ```sh
  git add src/scene/play/RealtimeTouchInputRouter.cpp tests/realtime_touch_input_router_tests.cpp
  git commit -m "fix: preserve authored skin lane touch precedence"
  ```

### Task 3: Reconcile direct-runtime test fixtures with established compatibility

**Files:**
- Modify: `tests/lua_skin_file_system_windows_contract_tests.py`
- Modify: `scripts/check_judgement_indicator_range_flow.py`
- Modify: `tests/skin_resource_catalog_tests.cpp`
- Modify: `tests/fixtures/beatoraja_skin/packages/runtime_contract/skin/io_contract.luaskin`
- Modify: `tests/lua_skin_runtime_tests.cpp`
- Modify: `tests/gameplay_bga_target_tests.cpp`

**Interfaces:**
- Consumes: pinned `SkinLuaAccessor.RestrictedIoLib` (`openFile`, selected-directory lexical path handling), live visible source behavior in `SkinTreeSnapshotter`, `LuaSkinHostModules::installRuntimeModules`, and `Jukebox::preflight`.
- Produces: contract tests that reject only a violation of current intentional behavior; they no longer require removed Windows private-overlay APIs, a removed Settings layout label, a private revision for a visible source, or shader-unavailability as the healthy BGA result.

- [x] **Step 1: Update the Windows source contract to test the actual boundary**

  Replace the static assertions for `UniqueWindowsHandle`, `PrivateWindowsSecurity`, `validatePrivateOverlayRoot`, and retained private overlay handles with assertions that the source contains both `normalizeAtSkinDirectory` and the explicit `skin/` root branch, while it does not contain `enterRenderPhase` denial logic. Keep the `advapi32` target-link assertion only if the corresponding Windows-only package implementation still requires it; otherwise remove that assertion and the stale link requirement together.

- [x] **Step 2: Update the judgment-range flow audit for the current text-input UI**

  In `scripts/check_judgement_indicator_range_flow.py`, replace the removed `"Indicator Range"` / `"Range (ms)"` layout-text requirements with these live contracts:

  ```python
  require(controls, "syncJudgementIndicatorRangeInputText", "range input synchronization")
  require(controls, "commitJudgementIndicatorRangeInput", "range input commit")
  require(controls, "judgementIndicatorRangeMilliseconds", "range persistence")
  require(settings_scene, "summaryJudgementIndicatorRangeValueText", "summary range display")
  require(renderer_header, "int rangeMilliseconds", "required renderer range parameter")
  require(gameplay, "judgementIndicatorRangeMilliseconds", "live gameplay propagation")
  ```

- [x] **Step 3: Make the catalog fixture model a live Files-visible source**

  In `tests/skin_resource_catalog_tests.cpp`, construct the source under `roots.visiblePackages` and set `roots.liveSources = true`. The fixture then verifies its first and subsequently modified selected-root revisions without creating a private revision payload; the resource preparer still rejects a filesystem whose digest belongs to the later live revision.

- [x] **Step 4: Correct the Lua I/O fixture to include the deliberately added `io.lines`**

  Replace fixture lines 6-10 with:

  ```lua
  local io_exports = 0
  for _ in pairs(io) do io_exports = io_exports + 1 end
  assert(io_exports == 2)
  assert(io.stdin == nil and io.stdout == nil and io.stderr == nil)
  assert(type(io.open) == "function" and type(io.lines) == "function")
  assert(io.popen == nil and io.tmpfile == nil)
  ```

  Add one `io.lines("input.txt")` assertion that consumes both fixture lines and reaches `nil`, then rerun `lua_skin_runtime_tests`.

- [x] **Step 5: Make the BGA readiness assertion test both prepared role surfaces and successful preflight**

  At `tests/gameplay_bga_target_tests.cpp:933-939`, require `baseAndLayerFrame.base`, `baseAndLayerFrame.layer`, `basePreflight.ready`, and `layerPreflight.ready`. Then call the existing submission path and assert its submission statistic changes. Do not expect `gameplay_bga.image.preflight`: the observed current state has both role surfaces sharing token `3` and both preflights ready.

- [x] **Step 6: Run the six narrow RED-to-GREEN targets**

  Run:

  ```sh
  python3 scripts/check_judgement_indicator_range_flow.py . && \
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.lua_skin_file_system_windows_contract_tests -v && \
  cmake --build cmake-build-debug --target skin_resource_catalog_tests lua_skin_runtime_tests gameplay_bga_target_tests -j 12 >/tmp/runtime-contract-build.log 2>&1 && \
  ctest --test-dir cmake-build-debug -R '^(skin_resource_catalog_tests|lua_skin_runtime_tests|gameplay_bga_target_tests)$' --output-on-failure
  ```

  Expected: each test passes; no production live-I/O behavior changes.

- [x] **Step 7: Commit the contract fixture corrections**

  ```sh
  git add tests/lua_skin_file_system_windows_contract_tests.py scripts/check_judgement_indicator_range_flow.py tests/skin_resource_catalog_tests.cpp tests/fixtures/beatoraja_skin/packages/runtime_contract/skin/io_contract.luaskin tests/lua_skin_runtime_tests.cpp tests/gameplay_bga_target_tests.cpp
  git commit -m "test: align skin runtime contracts with live source semantics"
  ```

  Completed in `ed41be1e`.

### Task 4: Restore the full ImageSet binding test environment and prevent its crash

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/fixtures/beatoraja_skin/model/all_v1_objects.expected.json` only if its observed fields change after the test catalog is corrected
- Test: `tests/beatoraja_skin_model_tests.cpp`

**Interfaces:**
- Consumes: Beatoraja `JsonSkinObjectLoader.java:74-92`, which constructs `SkinImage(sources, imgs.value)` or `SkinImage(sources, imgs.ref)`; AsoBMaShow `LuaSkinTableDecoder::bindGameplayDefinitions` binds ImageSet `ref` in `SkinIntegerPropertyDomain::ImageIndex`.
- Produces: the model fixture supplies its numeric built-in selectors (`700`, `701`, `800`) to `LuaSkinBindingDecoder`; `integerProperties`, `floatProperties`, and ImageSet state vectors are validated without out-of-range construction or a process crash.

- [x] **Step 1: Preserve the current RED reproduction**

  Run:

  ```sh
  cmake --build cmake-build-debug --target beatoraja_skin_model_tests -j 12 >/tmp/model-build.log 2>&1 && \
  cmake-build-debug/beatoraja_skin_model_tests
  ```

  Expected: five expectation failures followed by `EXC_BAD_ACCESS`; LLDB shows zero integer and float bindings in the fixture before the failing assertions.

- [x] **Step 2: Supply a fixture built-in catalog for the selectors the fixture authors**

  In `ModelFixture::decode`, replace the judge-only `SkinBuiltinBindingCatalogView(judgeBindingBuiltins)` with a test-local catalog containing the existing judge entries plus numeric integer/image-index selectors `700` and `701`, and rate/float selectors `800` in the domains asserted by `all_v1_objects.expected.json`. Use the same `SkinBuiltinBinding` structures used by normal built-in catalog tests; do not add a fallback for an unknown selector.

- [x] **Step 3: Make cardinality mismatch non-crashing before inspecting payloads**

  After each `expect(actual.size() == expected.size(), ...)`, return from that local test when sizes differ before indexing or constructing expected IDs. This preserves the diagnostic assertion while preventing a fixture mismatch from becoming a segmentation fault.

- [x] **Step 4: Re-run and update only observed expected fields**

  Run the Step 1 command. If it reports a field-value mismatch after the catalog correction, update only that field in `all_v1_objects.expected.json` to the decoder's observed, source-derived value and explain the corresponding `JsonSkinObjectLoader` behavior in the commit message. Do not change an expectation solely to silence a failure.

- [x] **Step 5: Commit the model-contract repair**

  ```sh
  git add CMakeLists.txt tests/beatoraja_skin_model_tests.cpp tests/fixtures/beatoraja_skin/model/all_v1_objects.expected.json
  git commit -m "test: restore ImageSet binding model coverage"
  ```

  Completed in `6c60939d`; the observed expected JSON remains unchanged.

### Task 5: Rebase acceptance documents and schema on ordinary Beatoraja runtime I/O

**Files:**
- Modify: `docs/skin-compat/beatoraja-lua-gameplay-contract.md:161-182`
- Modify: `docs/skin-compat/modernchic-scuro-4.6-acceptance.md:78-95,115-123`
- Modify: `tests/fixtures/beatoraja_skin/reference_manifest.json`
- Delete: `tests/fixtures/beatoraja_skin/policies/lua_sandbox_v1.json`
- Modify: `scripts/audit_beatoraja_skin.py`
- Modify: `scripts/run_skin_acceptance.py`
- Modify: `tests/beatoraja_skin_reference_tests.py`
- Modify: `tests/lua_skin_runtime_tests.cpp`
- Modify: `tests/skin_acceptance_contract_tests.py`
- Modify: `docs/superpowers/plans/2026-08-03-beatoraja-lua-gameplay-skins.md:5158,5233`

**Interfaces:**
- Consumes: pinned `SkinLuaAccessor.java:46-53,340-391` and `LuaSkinLoader.java:50-56,69-90`.
- Produces: acceptance schema v2, with normal selected-root read/write/directory observation allowed; no fictional negative render-I/O denial scenario, no required denied counters, no same-frame built-in fallback asserted solely for file I/O, and no overlay before/after digest requirement. The target remains ModernChic 4.6 everywhere.

- [x] **Step 1: Add schema RED tests**

  In `tests/skin_acceptance_contract_tests.py`, add a contract fixture with schema version 2 and a normal configured/render file-I/O observation record. Assert validation succeeds. Add a schema-v2 fixture that contains legacy `negativeScenarios`, `passingGuardVectorSha256`, `overlayDigestBefore`, `overlayDigestAfter`, or `deniedCountersExpected`; assert validation fails with the exact legacy-field rejection.

- [x] **Step 2: Run the schema test to prove RED**

  Run:

  ```sh
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.skin_acceptance_contract_tests -v
  ```

  Expected: the current schema-v1 validator rejects schema version 2 or admits a legacy negative scenario.

- [x] **Step 3: Implement schema v2 validation**

  In `scripts/run_skin_acceptance.py`, make `schemaVersion == 2` the only accepted format. Replace the negative-scenario and guard-vector validator with an `ordinaryRuntimeIo` record that permits read, write, and directory-scan observations under the selected root and records only whether the manual run observed them. Reject the legacy keys named in Step 1. Preserve archive, revision, entry, configuration, screenshot, performance, resource-lifecycle, and external-evidence privacy validation.

- [x] **Step 4: Rewrite the two compatibility documents and manifest without the fictional transition**

  Replace the render-transition paragraphs with a concise source-cited statement: selected-skin-root I/O remains ordinary and live across configured load and render; compatibility does not claim a filesystem freeze or hidden overlay. Remove frozen guard digests, negative fallback action, denied-operation requirements, and before/after overlay digest from `reference_manifest.json`. Keep physical evidence status `pending` until real metadata is available, and record the user's manual functional confirmation only as non-threshold narrative evidence.

- [x] **Step 5: Retarget stale Task 25/26 identifiers**

  In the original plan's Task 25/26 shell snippets, replace `SCURO` variable names and the `modernchic-scuro-4.02-acceptance.md` path with neutral `SKIN_ACCEPTANCE_*` names and `modernchic-scuro-4.6-acceptance.md`. Do not represent the old SCURO 4.02 archive as an active acceptance target.

- [x] **Step 6: Run GREEN on contract and external target audit**

  Run:

  ```sh
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.beatoraja_skin_reference_tests tests.skin_acceptance_contract_tests -v && \
  python3 scripts/audit_beatoraja_skin.py \
    --beatoraja-root ../beatoraja \
    --archive-path "$HOME/Downloads/Skins/ModernChic460.zip" \
    --archive-package-prefix ModernChic \
    --skin-root "$HOME/Downloads/Skins/ModernChic" \
    --expected-archive-sha256 3e36eef69d2f5f3b117696e22936347d9c8f5e379f8912d763a074ca9bcbbe4c \
    --verify tests/fixtures/beatoraja_skin/reference_manifest.json
  ```

- [x] **Step 7: Commit the honest acceptance contract**

  ```sh
  git add docs/skin-compat/beatoraja-lua-gameplay-contract.md docs/skin-compat/modernchic-scuro-4.6-acceptance.md tests/fixtures/beatoraja_skin/reference_manifest.json scripts/run_skin_acceptance.py tests/skin_acceptance_contract_tests.py docs/superpowers/plans/2026-08-03-beatoraja-lua-gameplay-skins.md
  git commit -m "docs: align skin acceptance with Beatoraja runtime I/O"
  ```

  Completed in `364739f2`.

### Task 6: Re-run the desktop milestone gate and restart final review

**Files:**
- Modify: `docs/skin-compat/beatoraja-lua-gameplay-final-review.md`
- Modify: `docs/skin-compat/modernchic-scuro-4.6-acceptance.md`

**Interfaces:**
- Consumes: Tasks 1-5 commits, the fixed ModernChic 4.6 archive/root, and the pinned clean Beatoraja reference.
- Produces: a new `REVIEWED_COMMIT` record whose source and desktop suites pass. Physical evidence remains explicitly pending unless a connected device produces the privacy-preserving metadata file.

- [ ] **Step 1: Run the complete desktop suite with a bounded report**

  Run:

  ```sh
  cmake --build cmake-build-debug --target main -j 12 >/tmp/task25-main-build.log 2>&1 && \
  ctest --test-dir cmake-build-debug --output-on-failure >/tmp/task25-ctest.log 2>&1
  ```

  On failure, print only `rg -n 'Failed|Exception|tests passed|The following tests FAILED' /tmp/task25-ctest.log`; on success, print its final twenty lines.

- [ ] **Step 2: Run focused Python and artifact-contract tests**

  Run:

  ```sh
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
    tests.beatoraja_skin_reference_tests \
    tests.skin_acceptance_contract_tests \
    tests.ios_build_setup_tests \
    tests.ios_artifact_audit_tests -v
  ```

- [ ] **Step 3: Record only verified review facts**

  Create `docs/skin-compat/beatoraja-lua-gameplay-final-review.md` with the reviewed commit, pinned source SHA, commands/results, the accepted archive race fix, the authored-touch fix, and the schema-v2 direct-I/O decision. State that the user's manual functionality confirmation is accepted product feedback but does not fill physical performance/screenshot evidence fields.

- [ ] **Step 4: Commit the review record only after all desktop gates are green**

  ```sh
  git add docs/skin-compat/beatoraja-lua-gameplay-final-review.md docs/skin-compat/modernchic-scuro-4.6-acceptance.md
  git commit -m "docs: record gameplay skin final review"
  ```

## Self-Review

- Source truth is preserved: the plan removes only invented runtime restrictions and retains an ingress-only no-follow ZIP check.
- Each of the eight reproduced CTest failures has a concrete owner: Windows static contract and judgment audit (Task 3), archive patcher regression (Task 1), live-source catalog assumption and `io.lines` fixture (Task 3), ImageSet binding fixture/crash (Task 4), shader-ready BGA assertion (Task 3), and overlap precedence (Task 2).
- The plan does not copy third-party skins, commit third-party assets, deploy builds, or modify unrelated user files.
- Task 6 deliberately keeps physical evidence pending; it does not convert a manual observation into fabricated thresholds or screenshots.
