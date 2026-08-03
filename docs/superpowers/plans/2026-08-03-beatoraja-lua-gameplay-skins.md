# Beatoraja Lua Gameplay Skins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Install, configure, and run an unmodified ModernChicPlay (SCURO) 4.02 7-key Beatoraja Lua gameplay skin on iPad, with Files-app editing, Fit/Stretch/Custom layouts, a package-scoped sandbox, authoritative AsoBMaShow gameplay, and immediate built-in-renderer fallback.

**Architecture:** Treat `Documents/Skins` as a mutable user source and activate only immutable, validated private revisions. Decode Beatoraja's two-phase Lua return tables into a typed source-neutral model, map one immutable AsoBMaShow gameplay snapshot into Beatoraja properties/timers/events, evaluate a whole ordered command buffer before submitting it, and keep the current `BMSRenderer` initialized as the same-frame fallback. Portable C++ owns package, Lua, model, projection, and rendering logic; Objective-C++ is limited to cancellable folder handoff, Application Support path resolution, and no-follow Apple alias classification.

**Tech Stack:** C++23, LuaJIT with JIT disabled, sol2, utf8proc, libarchive, nlohmann/json, SDL2/SDL_ttf, bgfx, Objective-C++/UIKit document pickers, CMake/CTest, Python contract tests, and the existing iOS unsigned-release verification script.

## Global Constraints

- The approved design at `docs/superpowers/specs/2026-08-03-beatoraja-lua-gameplay-skin-design.md` is authoritative. If implementation evidence conflicts with it, stop and amend the design with user approval before changing product behavior.
- Do not edit generated `src/bms_parser.hpp` or `src/bms_parser.cpp` directly. Parser behavior changes must be made and tested in `../bms-parser-cpp`, amalgamated there, committed/pushed in that sibling repository, and only then copied into this repository exactly as required by `AGENTS.md`.
- Do not distribute or commit ModernChic/SCURO files. Commit only manifests, hashes, provenance, synthetic fixtures, traces, and screenshots whose contents are AsoBMaShow-generated and redistributable.
- Do not add a network API, arbitrary/reflective Java access, `ffi`, `jit`, unrestricted `io`/`os`, process execution, native loading, or filesystem access outside an activated package revision and its private data overlay. The only module named `luajava` permitted by v1 is the Task 1-audited closed Lua table implemented in Task 9: exact `bindClass` support for `java.io.File` and `com.badlogic.gdx.Gdx`, exact `new(File, virtualPath)`, virtual-only load-time `listFiles`, overlay-only `mkdir`, and no `Gdx.app`; every other class, constructor, member, Java object, URL/HTTP/controller/input/reflection/native capability is rejected.
- Keep LuaJIT disabled for every skin state on desktop and iOS. A passing desktop test may not depend on a LuaJIT-only extension hidden on iOS.
- Keep the built-in renderer initialized throughout gameplay. A critical skin failure must discard the unsubmitted skin command buffer and render the built-in presentation in that same frame.
- Never read package files, scan directories, decode images, rasterize new glyphs, or create textures in the active render path. Do those operations during revision validation or session preparation.
- Packages are shared. Entry selection, options, file choices, offsets, and viewport layout are profile-owned through `settings.json`.
- `Documents/Skins` is the canonical user-editable source. Runtime revisions live in iOS Application Support and are never exposed as package roots to Lua.
- Run `scripts/ios_release_verify.sh` after any Objective-C++, Xcode, native dependency, bundled shader/resource, LuaJIT packaging, Files, or picker change. It is an unsigned verification path and must not upload a build.
- Do not run `scripts/ios_firebase_deploy.sh` without an explicit deployment request.

---

## Mandatory Beatoraja Reference Refresh

At the start of every numbered task, and as the first action after context compaction or a resumed implementation session:

1. Read this plan's current task and the approved design again.
2. Resolve the immutable reference root and run:

   ```sh
   aso_root="$(git rev-parse --show-toplevel)"
   beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
   if test -f scripts/check_beatoraja_reference.py; then
     python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean
   else
     test "$(git -C "$beatoraja_ref_root" rev-parse HEAD)" = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
     test -z "$(git -C "$beatoraja_ref_root" status --porcelain)"
   fi
   ```

   Expected: the checker exits 0 and reports `c2ed5db1a46145ed10790c3872f717e95b59db9d`. Task 1 uses the direct fallback because it creates the checker; Task 2 onward must use the checker. Do not pull, rebase, edit, or silently advance the reference clone. `ASOBMASHOW_BEATORAJA_ROOT` is the only supported override, so the gate also works from an isolated AsoBMaShow worktree.
3. Reopen every Beatoraja file listed under the current task's **Reference refresh** line. Read the relevant methods directly even if a prior agent summarized them.
4. Record the pinned commit, file path, class/method or Lua symbol, and captured behavior in the task's fixture provenance or compatibility contract.

The baseline files reopened from `beatoraja_ref_root` for every delivery slice are:

- `src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java`
- `src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java`
- `src/bms/player/beatoraja/skin/json/JsonSkin.java`
- `src/bms/player/beatoraja/skin/Skin.java`
- `skin/default/play/play7.luaskin`

If a future task intentionally changes the Beatoraja baseline, it must first commit a reviewed reference diff and regenerate all affected traces. No implementation task follows upstream HEAD implicitly.

This reference refresh is an implementation-process gate, not a default build dependency. Default CTest runs consume only committed manifests, traces, and redistributable fixtures. Live source/package audits are opt-in and require explicit `--beatoraja-root` and `--skin-root` arguments.

## Fixed Policy Limits

Put these values in typed policy structs and assert them in tests; do not scatter literals:

- Package input: 2 GiB maximum archive, 512 MiB maximum regular file, 4 GiB maximum expanded package, 20,000 regular files, 65,535 total ZIP members (regular files plus explicit directories), 1,024 UTF-8 bytes per normalized relative path, 64 path components, and 128 UTF-8 bytes for an app-chosen package directory name.
- Package identity: valid UTF-8, slash separators, no empty/dot components, NFC storage spelling, and a full Unicode case-folded collision key.
- Profile settings: at most 64 remembered entries; 256 options, 256 file choices, and 256 offsets per entry; 128-byte configuration keys; 1,024-byte entry/file values; offset components clamped to `[-32768, 32767]`; custom scale clamped to `[0.1, 10.0]`; custom translation clamped to `[-8192, 8192]` authored pixels.
- Lua catalog state: 32 MiB allocator limit, 2,000,000 VM instructions, and 2 seconds wall time per execution.
- Lua validation/gameplay load state: 128 MiB allocator limit, 20,000,000 instructions, and 10 seconds wall time for each header or configured phase.
- Lua gameplay callbacks: 250,000 instructions and 4 milliseconds wall time per callback; 1,000,000 instructions and 6 milliseconds wall time for all callbacks in one frame. Frame totals reset once per captured `PlayfieldVisualState`.
- Model conversion: 64 table levels, 200,000 table entries, 20,000 skin objects, 20,000 resources, 8,192 pixels on either decoded-image axis, and 512 MiB decoded/pinned image bytes per session.
- Private data overlay: 16 MiB and 1,024 regular files per profile/entry, with atomic replacement and no executable/resource lookup through the overlay.

If the pinned SCURO package exceeds a fixed limit, change the limit once in the policy type, document the measured requirement in the acceptance manifest, and rerun hostile-input tests before proceeding.

The package constants live in one compile-time contract:

```cpp
struct SkinPackagePolicy {
  static constexpr std::uint64_t maxArchiveBytes = 2ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t maxRegularFileBytes = 512ULL * 1024 * 1024;
  static constexpr std::uint64_t maxExpandedBytes = 4ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t maxFiles = 20'000;
  static constexpr std::uint32_t maxPathBytes = 1'024;
  static constexpr std::uint32_t maxPathComponents = 64;
  static constexpr std::uint32_t maxPackageNameBytes = 128;
};
```

## Canonical Digest Grammars

All integer fields below are unsigned big-endian unless explicitly signed. Text is valid NFC UTF-8 with `/` separators and no terminator.

- `SkinTreeDigestV1` is SHA-256 over the exact byte stream: ASCII bytes `ASOBMSKIN-TREE-V1`, one `0x00`, `u64 fileCount`, then each regular file sorted by normalized relative-path UTF-8 bytes as `u32 pathByteCount`, path bytes, `u64 fileByteCount`, and file bytes. Directories contribute nothing. The root itself contributes no name. A ZIP first safely normalizes and inventories regular-file and explicit-directory entries, then strips exactly one common first path component iff every regular file is below that component and no regular file is at archive root; otherwise it strips nothing. Wrapper inference uses regular files only. In the strip case every explicit directory must be the wrapper itself or below it; the wrapper-root directory becomes empty after stripping and is ignored, while every other directory must retain a nonempty safe path. In the no-strip case every explicit directory retains its normalized path. Explicit directories are structural only, but still participate in duplicate, normalization, and file/directory collision checks. Reject links and all other special nodes, encrypted entries, an empty regular-file post-strip path, a regular-file/directory collision, duplicate/colliding normalized paths, or any policy violation. The archive audit requires `--archive-package-prefix RELATIVE_OR_DOT`; `.` means the no-strip case, otherwise the value must equal the uniquely inferred wrapper. `--skin-root` names the corresponding extracted package root after that strip. Reject a declared/inferred mismatch or any payload outside the declared prefix. Task 5 snapshots, Task 1's Python audit, ZIP import, folder import, manual rescan, and activated revision IDs use these identical bytes.
- `SkinConfigurationDigestV1` is SHA-256 over ASCII bytes `ASOBMSKIN-CONFIG-V1`, one `0x00`, followed by three sections in fixed order. Each section starts with its tag and `u32 recordCount`; records sort by NFC-key UTF-8 bytes. Option section tag `0x01`: `u32 keyBytes`, key, signed two's-complement `i32 value`. File section tag `0x02`: `u32 keyBytes`, key, `u32 valueBytes`, declared virtual choice value. Offset section tag `0x03`: `u32 keyBytes`, key, then signed two's-complement `i32 x,y,w,h,r,a` in that order. No host path, callback, header declaration, viewport, category, or synthesized table ordering is hashed. Task 10 reconciliation emits this digest; Tasks 7, 21, 24, and acceptance compare it byte-for-byte.

## File Structure

### Compatibility evidence

- `docs/skin-compat/beatoraja-lua-gameplay-contract.md`: pinned source symbols, Lua/model/property semantics, intentional divergences, and criticality decisions.
- `docs/skin-compat/modernchic-scuro-4.02-acceptance.md`: external package provenance, local digest, entry inventory, iPad/chart/performance matrix, and final results without assets.
- `tests/fixtures/beatoraja_skin/reference_manifest.json`: machine-readable reference commit, trace versions, object/property/timer/event surface, and fixture provenance.
- `tests/fixtures/beatoraja_skin/`: small synthetic Lua, package, trace, and expected-command fixtures only.
- `scripts/check_beatoraja_reference.py`: read-only pinned-SHA/clean-tree implementation gate with an explicit root.
- `scripts/audit_beatoraja_skin.py`: local, read-only audit of an external package and the pinned adjacent Beatoraja clone.
- `scripts/capture_beatoraja_skin_traces.py`: deterministic developer tool that refreshes numeric/property/timer/event traces without making the production build depend on the clone.

### Package and profile core

- `src/skin/package/SkinPackageTypes.h`: limits, identifiers, diagnostics, progress, revision metadata, and result types.
- `src/skin/package/SkinPathPolicy.{h,cpp}`: UTF-8 validation, NFC, full case-fold collision keys, relative-path normalization, and containment.
- `src/skin/package/SkinAliasDetector.{h,cpp}` and `src/skin/package/SkinAliasDetectorApple.mm`: injected no-follow link/alias/reparse classification, including Apple's `NSURLIsAliasFileKey`.
- `src/skin/package/SkinTreeSnapshotter.{h,cpp}`: no-follow stable tree inventory/copy, framed SHA-256, immutable revision publication, and leases.
- `src/skin/package/SkinArchiveImporter.{h,cpp}`: strict ZIP streaming into package staging.
- `src/skin/package/SkinPackageCatalog.{h,cpp}`: versioned private metadata, store-internal metadata recovery, validation records, and activation pointers.
- `src/skin/package/SkinPackageStore.{h,cpp}`: exclusive pre-service cross-root journal recovery, archive/folder prepare, visible publication, manual rescan, activation, retention, and garbage collection.
- `src/skin/SkinStoragePaths.{h,cpp}`: injectable visible, private-revision, private-catalog, and profile-overlay roots.
- `src/skin/SkinProfileSettings.{h,cpp}`: selected 7-key entry, per-entry Beatoraja configuration, and viewport layout.
- `src/ProfileSettingsPersistenceCoordinator.{h,cpp}`: the single serialized owner for active-profile `AppSettings` saves and versioned skin-section commits; its worker performs every active `settings.json` write.

### Lua and source model

- `src/skin/beatoraja/SkinCompatibilityDiagnostics.{h,cpp}`: structured deduplicated diagnostics and critical/optional attribution.
- `src/skin/beatoraja/SkinDiagnosticHistory.{h,cpp}`: bounded catalog-backed diagnostic history exposed after a chart ends.
- `src/skin/beatoraja/LuaSkinFileSystem.{h,cpp}`: package-local virtual paths, controlled `require`, overlay access, and render-phase denial.
- `src/skin/beatoraja/LuaSkinRuntime.{h,cpp}`: quota allocator, JIT shutdown, selected libraries, instruction hooks, and phase state machine.
- `src/skin/beatoraja/LuaSkinHostModules.{h,cpp}`: `skin_config`, controlled main-state/timer/event modules, and presentation-only writers.
- `src/skin/beatoraja/LuaSkinTableDecoder.{h,cpp}`: ordered table conversion and retained callback registry.
- `src/skin/beatoraja/BeatorajaSkinModel.h`: source-neutral header, resource, object, destination, note, gauge, judge, BGA, timer, and event types.
- `src/skin/beatoraja/SkinModelValidator.{h,cpp}`: reference resolution, criticality, limits, and unsupported-surface diagnostics.
- `src/skin/beatoraja/SkinResourceCatalog.{h,cpp}`: prepared package images/fonts/sprite regions and render-thread texture ownership.

### Evaluation and rendering

- `src/skin/beatoraja/PlaySkinViewport.{h,cpp}`: Fit/Stretch/Custom matrices and exact inverse input transform.
- `src/skin/beatoraja/SkinDestinationEvaluator.{h,cpp}`: timer, condition, loop, interpolation, offsets, color, rotation, clip, filter, blend, and stretch evaluation.
- `src/skin/beatoraja/SkinDrawCommand.h`: authored-order command variants and immutable frame buffers.
- `src/skin/beatoraja/Skin2DRenderer.{h,cpp}`: evaluate-whole-frame then submit-contiguous-batches workflow.
- `src/rendering/SkinQuadBatchRenderer.{h,cpp}`: per-vertex position/UV/color, rotation, sampler, scissor, and blend submission.
- `src/skin/beatoraja/SkinTextAtlas.{h,cpp}`: bounded prebuilt glyph atlases and dynamic-string glyph validation.
- `shader_src/vs_skin_quad.sc`, `shader_src/fs_skin_quad.sc`: portable skin quad shaders.

### Gameplay integration

- `src/scene/play/PlayfieldPresentationEvents.h`: press/release/judge event seam.
- `src/scene/play/PlayfieldChartVisualModel.{h,cpp}`: pointer-free immutable chart/lane/note identity and scroll-prefix data.
- `src/scene/play/PlayfieldVisualState.{h,cpp}`: mutable event store plus immutable per-frame gameplay snapshot.
- `src/scene/play/PlayfieldProjection.{h,cpp}`: deterministic visible note/LN/measure descriptors.
- `src/skin/beatoraja/PlaySkinStateBridge.{h,cpp}`: Beatoraja properties/timers/events over a captured snapshot.
- `src/skin/beatoraja/PlaySkinSession.{h,cpp}`: Lua state, model, resources, evaluation budget, and diagnostics for one chart.
- `src/skin/GameplaySkinLifecycle.{h,cpp}`: startup/profile/rescan service and next-chart activation owner.
- `src/scene/play/PlayfieldPresentation.h`: render/touch control interface.
- `src/scene/play/PlayfieldPresentationCoordinator.{h,cpp}`: warmed built-in renderer, optional skin session, and atomic fallback.
- `src/audio/GameplayBgaFrame.h`: render-neutral embedded-BGA target.

### Settings and iOS

- `src/scene/GameplaySkinSettingsController.{h,cpp}`: pure catalog/import/rescan/config/layout presentation state.
- `src/scene/SettingsSceneSkins.cpp`: Gameplay Skins tab and action wiring.
- `PlatformDocumentHandoff` and iOS native files: cancellable temporary-directory handoff.
- `scripts/ios_artifact_audit.sh`: verifies final Files/document-browser keys and packaged native dependencies/shaders.

## Slice 1 — Compatibility Corpus

### Task 1: Pin the external acceptance package and source contract

**Reference refresh:** baseline files plus `SkinLoader.java`, `JSONSkinLoader.java`, `SkinHeader.java`, `play/PlaySkin.java`, `play/SkinNote.java`, `play/SkinBGA.java`, all files in `skin/property/` referenced by the target, and every `.lua`/`.luaskin` loaded by the target entry.

**Files:**

- Create: `docs/skin-compat/beatoraja-lua-gameplay-contract.md`
- Create: `docs/skin-compat/modernchic-scuro-4.02-acceptance.md`
- Create: `tests/fixtures/beatoraja_skin/reference_manifest.json`
- Create: `tests/fixtures/beatoraja_skin/README.md`
- Create: `scripts/check_beatoraja_reference.py`
- Create: `scripts/audit_beatoraja_skin.py`
- Test: `tests/beatoraja_skin_reference_tests.py`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: pinned commit `c2ed5db1a46145ed10790c3872f717e95b59db9d`; live inputs only through explicit `--beatoraja-root`, `--skin-root`, and archive-digest arguments.
- Produces: `check_beatoraja_reference.py --root PATH [--require-clean] -> exit 0|nonzero`; `audit_beatoraja_skin.py --beatoraja-root PATH --archive-path PATH --archive-package-prefix RELATIVE_OR_DOT --skin-root PATH [--expected-archive-sha256 HEX64] (--output PATH | --verify PATH) -> exit 0|nonzero`; `ReferenceManifestV1 {schemaVersion, beatorajaCommit, targetVersion, archiveSha256, archivePackagePrefix, archivePayloadTreeSha256, auditedSourceTreeSha256, acceptanceContract, entries, surface, externalPayloadDigests, traceVersions}`; `SurfaceEvidence {kind, id, criticality, provenance[]}`; and `SourceProvenance {commit, path, symbol, behavior}`. The audit computes the archive digest itself and hashes both normalized regular-file trees with `SkinTreeDigestV1`; a mismatch fails, so the audited tree is cryptographically bound to the pinned archive.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```python
def test_committed_contract_is_clone_independent(self):
    self.assertEqual(self.manifest["beatorajaCommit"], PINNED_COMMIT)
    self.assertEqual(self.manifest["acceptanceContract"]["schemaVersion"], 1)
```

- [ ] **Step 2: Write `tests/beatoraja_skin_reference_tests.py` first**. Make the default test clone-independent: validate the committed pinned SHA string, manifest schema, nonempty source-symbol provenance, exact official target version `4.02`, lowercase 64-character archive/archive-tree/audited-tree SHA-256 values with the two tree digests equal, at least one 7-key Lua entry, complete criticality for every audited object/property/timer/event/module/file API, and absence of external payload hashes anywhere in `git ls-files` (not only images or one fixture directory). Check all audited file kinds, including Lua, fonts, audio, video, archives, and images. Do not inspect a live clone or external package from default CTest.
- [ ] **Step 3: Register only that clone-independent Python test with CTest, then run**:

  ```sh
  python3 -m unittest tests/beatoraja_skin_reference_tests.py -v
  ```

  Expected RED: missing contract, acceptance document, manifest, and audit tool assertions fail.
- [ ] **Step 4: Implement `scripts/check_beatoraja_reference.py` and `scripts/audit_beatoraja_skin.py`**. The checker accepts `--root` and `--require-clean`, verifies the exact pinned commit, and never mutates Git. The opt-in audit accepts `--beatoraja-root`, `--archive-path`, required `--archive-package-prefix`, `--skin-root`, optional `--expected-archive-sha256`, `--output`, and `--verify`; it parses Lua source conservatively, inventories modules/resources and numeric/string callback identifiers, computes the archive SHA-256, applies the canonical deterministic wrapper rule and requires the CLI prefix to match it, computes exact `SkinTreeDigestV1` streams directly from safe regular ZIP entries after the inferred strip and from the extracted root, and fails on a digest mismatch or a dependency without a critical/optional disposition. Allow safe explicit directories only under the canonical structural-directory rule (including an ignored wrapper-root directory); reject links/special nodes, payload outside the prefix, empty regular-file post-strip paths, duplicate/colliding normalized paths, file/directory collisions, traversal, encrypted entries, unsupported compression, or policy-limit violations while reading the archive. Neither tool downloads nor copies the package.
- [ ] **Step 5: Obtain SCURO 4.02 from the official KasaBlog Google Drive folder linked at `https://www.kasacontent.com/musicgame/beatoraja/4226/`, store it outside the repository, compute the archive and payload-tree SHA-256 values, and record the exact archive filename, byte count, digests, acquisition date, official source URL, exact archive package prefix, corresponding extracted package root identity, and selected 7-key `.luaskin` entry identity/path in the acceptance document and manifest**. Also record the author's published license/usage/screenshot terms URL, access date, and whether local testing plus private screenshots are permitted; unresolved or prohibitive terms block physical screenshot capture. Source/terms URLs and the selected entry path are allowed provenance; other proprietary resource/module paths remain opaque. Digests are evidence, not bundled dependencies.
- [ ] **Step 6: Freeze acceptance schema version 1 before renderer work**. Require fields for non-unique iPad hardware model, exact iPadOS, drawable size, safe insets, configured Hz, measurement build/commit, external archive/entry/configuration digests, synthetic chart hashes, fixed autoplay scripts, screenshot timestamps, 30-second warm-up, three 180-second repetitions per scenario/layout, all six 16:9 and 4:3 Fit/Stretch/Custom layout cases, p99/missed-presentation/memory/resource limits from this plan, and a `pending|pass|fail` status plus evidence reference for every completion criterion. Task 1 accepts `pending`; Task 25 requires only `pass`.
- [ ] **Step 7: Fill the source contract with observed two-phase loading, table conversion, missing property behavior, actual libGDX `IntMap` timer/event ordering, destination interpolation, note/LN phases, BGA ordering, the no-network boundary, and the exact audited legacy-module surface that must be resolved before runtime acceptance**. Every claim must name a pinned Java method or Lua symbol.
- [ ] **Step 8: Run the audit against the external package and commit its machine-readable surface in `reference_manifest.json`**. Preserve only the selected entry identity/path plus allowed official source/terms provenance URLs; represent all other proprietary modules/resources as opaque stable IDs, kind, byte count, and digest, with their real package-relative names solely in the external audit report/evidence root. Do not commit extracted source, assets, absolute local paths, account/device names, or public physical-evidence URLs.

  ```sh
  aso_root="$(git rev-parse --show-toplevel)"
  beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
  python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean
  : "${SCURO_ARCHIVE_PATH:?set external SCURO archive path}"
  : "${SCURO_ARCHIVE_PACKAGE_PREFIX:?set . or the inferred wrapper}"
  : "${SCURO_ARCHIVE_SHA256:?set computed archive digest}"
  : "${SCURO_SKIN_ROOT:?set corresponding extracted package root}"
  python3 scripts/audit_beatoraja_skin.py \
    --beatoraja-root "$beatoraja_ref_root" \
    --archive-path "$SCURO_ARCHIVE_PATH" \
    --archive-package-prefix "$SCURO_ARCHIVE_PACKAGE_PREFIX" \
    --skin-root "$SCURO_SKIN_ROOT" \
    --expected-archive-sha256 "$SCURO_ARCHIVE_SHA256" \
    --output tests/fixtures/beatoraja_skin/reference_manifest.json
  ```
- [ ] **Step 9: Run the GREEN check**

  ```sh
  python3 -m unittest tests/beatoraja_skin_reference_tests.py -v
  ```

  Expected GREEN: all reference/provenance/asset-exclusion assertions pass.
- [ ] **Step 10: Commit the task**

  ```sh
  git add CMakeLists.txt docs/skin-compat scripts/check_beatoraja_reference.py scripts/audit_beatoraja_skin.py tests/beatoraja_skin_reference_tests.py tests/fixtures/beatoraja_skin
  git commit -m "test: pin Beatoraja gameplay skin contract"
  ```

### Task 1a: Freeze audited file-I/O and render-I/O acceptance policy

**Reference refresh:** Task 1's selected external closure plus pinned `SkinLuaAccessor.execFile`, LuaJ `JseIoLib` call/handle behavior, `LegacySkinLuaApi.fileFacade`, and `Skin.updateCustomObjects`.

**Files:**

- Modify: `scripts/audit_beatoraja_skin.py`
- Modify: `tests/beatoraja_skin_reference_tests.py`
- Modify: `tests/fixtures/beatoraja_skin/reference_manifest.json`
- Modify: `tests/fixtures/beatoraja_skin/README.md`
- Modify: `docs/skin-compat/beatoraja-lua-gameplay-contract.md`
- Modify: `docs/skin-compat/modernchic-scuro-4.02-acceptance.md`

**Interfaces:**

- Extends `ReferenceManifestV1` with opaque `SelectedFileIoSurfaceV1`, explicit empty selected custom-object map counts, four zero render-I/O limits, canonical opaque passing/negative guard-vector digests bound to the audited configuration, and a frozen negative render-I/O scenario. The tracked aggregate contains counts, call shapes, reachability/guard disposition, opaque guard IDs/canonical values, expected diagnostics/actions, the session-critical sandbox-integrity policy, and digests only—never non-selected external paths, module names, option labels, or source text.

- [ ] **Step 1: Refresh the pinned and external references** — Run the checker, reopen every reference above, and rerun the opt-in audit from explicit external inputs before writing tests. Do not copy package data into the repository.

**RED test anchor:**

```python
def test_render_io_policy_is_frozen(self):
    self.assertEqual(self.contract["limits"]["activeRenderFilesystemWrites"], 0)
    self.assertEqual(self.contract["limits"]["activeRenderFilesystemDirectoryScans"], 0)
    self.assertEqual(self.contract["limits"]["activeRenderResourceUploads"], 0)
    self.assertEqual(self.contract["negativeScenarios"][0]["overlayDigestBefore"], "pending")
    self.assertEqual(self.contract["negativeScenarios"][0]["overlayDigestAfter"], "pending")
```

- [ ] **Step 2: Extend the clone/package-independent test first**. Require an opaque aggregate for package `dofile`, `io.open` default/`r`/`w`/`a`, `lines`, zero-or-more-argument chainable `write`, `close`, call counts, nested-write parent creation, load/configured/render-callback reachability, and every selected option guard affecting render-time reads/writes/scans. Require `selectedCustomObjectMaps == {customTimers: 0, customEvents: 0}`. Extend `acceptanceContract.limits`—do not add a competing threshold object—with `activeRenderFilesystemReads`, `activeRenderFilesystemWrites`, `activeRenderFilesystemDirectoryScans`, and `activeRenderResourceUploads`, all zero; migrate the old `activeRenderUploads` key to `activeRenderResourceUploads` and reject the legacy key so both cannot coexist. Freeze canonical opaque guard-vector digests for every passing configuration and the negative scenario, each bound to the audited configuration digest. Freeze the negative scenario as a session-critical sandbox-integrity probe regardless of the triggering object's ordinary criticality, with opaque scenario/guard IDs and canonical values, diagnostic `skin_file_render_phase_denied`, action `discard_frame_disable_session_same_frame_builtin`, and pending before/after overlay digests that must become equal at Task 25. Reject external names/paths in all new fields.
- [ ] **Step 3: Run the RED check**

  ```sh
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests/beatoraja_skin_reference_tests.py -v
  ```

  Expected RED: the aggregate, empty-map proof, thresholds, guards, and negative scenario do not exist.
- [ ] **Step 4: Extend the bounded token-aware audit**. Derive the selected file-I/O call shapes and phase/guard graph from the explicit external closure, then replace every proprietary identity with a stable opaque ID before serialization. Record all render-I/O guards, not just lane-cover rotation; record configured-load listing separately from render callbacks. Canonicalize sorted opaque `(guardId, value)` vectors and bind their SHA-256 digests to the audited configuration digest. Record the AsoBMaShow rule that every post-transition filesystem/upload attempt is session-critical as a policy field, not as an upstream Beatoraja observation. Preserve all upstream facts separately from AsoBMaShow policy fields. Emit the selected custom-map counts from configured-model evidence or fail closed if they cannot be proven empty. Never download, copy, extract, or retain source in the tracked output.
- [ ] **Step 5: Extend schema documentation and acceptance policy**. Document safe automatic overlay-parent creation for nested writes, handle invalidation/discard at render transition, zero render read/write/scan/upload limits, separate performed/denied evidence, the session-critical post-transition I/O rule, exact negative diagnostic/fallback, asynchronously measured unchanged overlay digest, and the canonical opaque guard-vector digest required by each passing configuration.
- [ ] **Step 6: Regenerate and verify the real manifest, then run GREEN**

  ```sh
  aso_root="$(git rev-parse --show-toplevel)"
  beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
  python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean
  : "${SCURO_ARCHIVE_PATH:?set external SCURO archive path}"
  : "${SCURO_ARCHIVE_PACKAGE_PREFIX:?set . or the inferred wrapper}"
  : "${SCURO_ARCHIVE_SHA256:?set pinned SCURO archive digest}"
  : "${SCURO_SKIN_ROOT:?set corresponding extracted package root}"
  python3 scripts/audit_beatoraja_skin.py --beatoraja-root "$beatoraja_ref_root" --archive-path "$SCURO_ARCHIVE_PATH" --archive-package-prefix "$SCURO_ARCHIVE_PACKAGE_PREFIX" --skin-root "$SCURO_SKIN_ROOT" --expected-archive-sha256 "$SCURO_ARCHIVE_SHA256" --verify tests/fixtures/beatoraja_skin/reference_manifest.json
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests/beatoraja_skin_reference_tests.py -v
  ```

  Expected GREEN: real audit verification and all clone-independent schema/opacity tests pass.
- [ ] **Step 7: Commit the task**

  ```sh
  git add docs/skin-compat scripts/audit_beatoraja_skin.py tests/beatoraja_skin_reference_tests.py tests/fixtures/beatoraja_skin
  git commit -m "test: freeze gameplay skin render I/O policy"
  ```

### Task 2: Capture differential Lua, destination, property, timer, and event traces

**Reference refresh:** `LuaSkinLoader.java`, `SkinLuaAccessor.java`, `LegacySkinLuaApi.java`, `SkinLuaPathResolver.java`, `MainStateAccessor.java`, `MainStatePropertyLuaApiExporter.java`, `SkinFileLuaApiExporter.java`, `TimerUtility.java`, `EventUtility.java`, `CustomTimer.java`, `CustomEvent.java`, `SkinObject.java`, `Skin.java`, `TimerProperty.java`, `TimerPropertyFactory.java`, and `com.badlogic.gdx.utils.IntMap` from the pinned clone's `lib/gdx.jar`.

**Files:**

- Create: `scripts/capture_beatoraja_skin_traces.py`
- Create: `tests/fixtures/beatoraja_skin/lua/two_phase/entry.luaskin`
- Create: `tests/fixtures/beatoraja_skin/lua/two_phase/shared.lua`
- Create: `tests/fixtures/beatoraja_skin/lua/sandbox_probe.luaskin`
- Create: `tests/fixtures/beatoraja_skin/traces/lua_language_v1.json`
- Create: `tests/fixtures/beatoraja_skin/traces/destination_v1.json`
- Create: `tests/fixtures/beatoraja_skin/traces/properties_v1.json`
- Create: `tests/fixtures/beatoraja_skin/traces/timers_events_v1.json`
- Create: `tests/fixtures/beatoraja_skin/traces/legacy_lua_upstream_v1.json`
- Create: `tests/fixtures/beatoraja_skin/policies/lua_sandbox_v1.json`
- Modify: `tests/beatoraja_skin_reference_tests.py`

**Interfaces:**

- Consumes: `ReferenceManifestV1`, redistributable synthetic Lua fixtures, and an explicit live reference root only when refreshing upstream evidence.
- Produces: `capture_beatoraja_skin_traces.py --beatoraja-root PATH --output-dir PATH [--verify] -> exit 0|nonzero`; five upstream `TraceEnvelopeV1 {schemaVersion, kind, referenceCommit, provenance[], cases[]}` files whose `TraceCase` records contain `name`, `input`, `expected`, optional `callOrder`/`callCount`, and numeric `precision`; and one separately authored `SandboxPolicyV1 {schemaVersion, authority: "AsoBMaShow", selectedSurfaceDigest, allowed[], denied[], phaseRules[]}` fixture. The capture tool never creates or rewrites the policy fixture.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```python
def test_two_phase_trace_is_stable(self):
    self.assertEqual(self.trace["referenceCommit"], PINNED_COMMIT)
    self.assertEqual(self.trace["cases"][0]["expected"]["packageLoaded"], True)
```

- [ ] **Step 2: Extend the reference test first to require deterministic upstream trace schemas, pinned source provenance, input vectors, expected output values, callback call counts/order, numeric precision, timer OFF sentinel behavior, zero/one/two-argument events, and same-state header/configured `package.loaded` persistence. Require the AsoBMaShow sandbox policy as a distinct schema/authority whose allowed surface digest equals Task 1a's audit; reject any claim that virtual paths, overlay writes, denied upstream legacy branches, or render-phase denial were captured from Beatoraja.**
- [ ] **Step 3: Run the Python test**

  ```sh
  python3 -m unittest tests/beatoraja_skin_reference_tests.py -v
  ```

  Expected RED: the trace files and capture tool are missing.
- [ ] **Step 4: Implement the capture tool as a developer-only harness with required `--beatoraja-root`**. It may compile/run a small Java driver against that root's `lib` and pinned source tree, but must write only normalized JSON traces under `tests/fixtures`; production targets and default CI consume committed traces without Java or a reference clone.
- [ ] **Step 5: Capture exact upstream vectors for Lua table/number/string conversions used by SCURO, `bit32`, destination timer/loop/easing/color/angle/clip behavior, supported property dispatch, custom timer once-per-frame caching, and timer-before-event updates. For synthetic nonempty `IntMap` vectors, record initial capacity, load factor, complete insertion/replacement sequence, controlled `MathUtils.random` seed/state, and resulting order; label that seed as trace setup, not a universal Beatoraja order. Record that Task 1a proves the selected configured custom timer/event maps empty. Capture only upstream file/legacy facts: standard `dofile`/`io.open` return and handle-call shapes, repeated `require("luajava")` table identity, File/Gdx binds, File construction/list shape normalized without host paths, and absent `Gdx.app`. Keep Beatoraja's broader allowed legacy branches as upstream provenance facts, not AsoBMaShow requirements. Put virtual paths, overlay-only writes, class/member denial, automatic overlay parents, handle invalidation, and render-phase denial only in `lua_sandbox_v1.json` and Tasks 8–9 local tests.**
- [ ] **Step 6: Make the synthetic two-phase entry `require("shared")` before its `if skin_config then` branch, mutate module/global state during header evaluation, and assert configured evaluation sees the mutation in the same state while a new catalog state does not.**
- [ ] **Step 7: Run the GREEN check**

  ```sh
  aso_root="$(git rev-parse --show-toplevel)"
  beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
  python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean
  fixture_before="$(find tests/fixtures/beatoraja_skin/traces tests/fixtures/beatoraja_skin/policies -type f -print0 | sort -z | xargs -0 shasum -a 256)"
  python3 scripts/capture_beatoraja_skin_traces.py \
    --beatoraja-root "$beatoraja_ref_root" \
    --output-dir tests/fixtures/beatoraja_skin/traces \
    --verify
  python3 -m unittest tests/beatoraja_skin_reference_tests.py -v
  python3 -m unittest tests/beatoraja_skin_reference_tests.py -v
  fixture_after="$(find tests/fixtures/beatoraja_skin/traces tests/fixtures/beatoraja_skin/policies -type f -print0 | sort -z | xargs -0 shasum -a 256)"
  test "$fixture_before" = "$fixture_after"
  ```

  Expected GREEN: the live pinned-reference verifier and both default test runs pass, and committed trace output is byte-identical.
- [ ] **Step 8: Commit the task**

  ```sh
  git add scripts/capture_beatoraja_skin_traces.py tests/beatoraja_skin_reference_tests.py tests/fixtures/beatoraja_skin
  git commit -m "test: capture Beatoraja skin reference traces"
  ```

## Slice 2 — Portable Package and Lua Core

### Task 3: Add Unicode-safe skin identity and its native dependency

**Reference refresh:** `LuaSkinLoader.sandboxed`, `SkinLuaAccessor.setDirectory`, `SkinLuaAccessor.execFile`, `SkinFileLuaApiExporter`, and the target's relative `require`/resource references.

**Files:**

- Create: `src/skin/SkinPresentationTypes.h`
- Create: `src/skin/package/SkinPackageTypes.h`
- Create: `src/skin/package/SkinPathPolicy.h`
- Create: `src/skin/package/SkinPathPolicy.cpp`
- Test: `tests/skin_path_policy_tests.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `vcpkg.json`
- Modify: `scripts/get_ios_libs.py`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Create: `assets/legal/utf8proc.txt`
- Modify: `tests/ios_build_setup_tests.py`

**Interfaces:**

- Consumes: an authored UTF-8 direct-child package directory name plus a package-relative virtual path.
- Produces: `SkinPackageIdResult normalizePackageId(std::string_view)`, `SkinEntryIdResult normalizeEntryPath(const SkinPackageId &, std::string_view)`, `std::string installedRelativePath(const SkinEntryId &)`, shared `SkinDiagnostic`/progress types in `SkinPackageTypes.h`, and dependency-free render-neutral skin enums/tokens in `SkinPresentationTypes.h`. Public identities contain normalized virtual names only, never host paths.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("entry identity stays package-relative") {
  const auto package = normalizePackageId("ModernChic").package.value();
  CHECK(normalizeEntryPath(package, "play/play7.luaskin").entry->package == package);
}
```

- [ ] **Step 2: Make the RED test reference this required core identity shape; do not create the production headers yet**:

  ```cpp
  struct SkinPackageId {
    std::string directoryName;
    std::string collisionKey;
    auto operator<=>(const SkinPackageId &) const = default;
  };

  struct SkinEntryId {
    SkinPackageId package;
    std::string packageRelativePath;
    std::string collisionKey;
    auto operator<=>(const SkinEntryId &) const = default;
  };

  // These live in SkinPresentationTypes.h and remain available to
  // unconditional presentation/audio DTOs when Lua skins are compiled out.
  struct SkinFloatWriterId {
    std::uint32_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const SkinFloatWriterId &) const = default;
  };
  enum class SkinBlendMode : std::uint8_t {
    Normal, Additive, Subtractive, Multiply, Inverse
  };
  enum class SkinFilterMode : std::uint8_t { Nearest, Linear };
  enum class SkinStretchMode : std::uint8_t {
    Stretch = 0,
    KeepAspectRatioFitInner = 1,
    KeepAspectRatioFitOuter = 2,
    KeepAspectRatioFitOuterTrimmed = 3,
    KeepAspectRatioFitWidth = 4,
    KeepAspectRatioFitWidthTrimmed = 5,
    KeepAspectRatioFitHeight = 6,
    KeepAspectRatioFitHeightTrimmed = 7,
    KeepAspectRatioNoExpanding = 8,
    NoResize = 9,
    NoResizeTrimmed = 10
  };

  enum class DiagnosticSeverity : std::uint8_t {
    Info,
    Warning,
    Error
  };

  enum class SkinProgressPhase : std::uint8_t {
    Inspecting,
    Copying,
    Validating,
    Publishing
  };

  struct SkinSourceLocation {
    std::string virtualPath;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
  };

  struct SkinDiagnostic {
    std::string code;
    std::string message;
    std::string virtualPath;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::optional<SkinSourceLocation> source;
  };

  struct SkinProgress {
    SkinProgressPhase phase;
    std::uint64_t completedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t completedFiles = 0;
  };

  using SkinProgressCallback = std::function<void(const SkinProgress &)>;

  struct SkinRevision {
    SkinPackageId package;
    std::string lowercaseSha256;
    std::uint64_t fileCount = 0;
    std::uint64_t totalBytes = 0;
  };

  struct SkinPackageIdResult {
    std::optional<SkinPackageId> package;
    std::string error;
  };

  struct SkinEntryIdResult {
    std::optional<SkinEntryId> entry;
    std::string error;
  };

  struct SkinUtf8NfcResult {
    std::optional<std::string> value;
    std::string error;
  };

  SkinUtf8NfcResult normalizeSkinSourceNameNfc(std::string_view);
  SkinPackageIdResult normalizePackageId(std::string_view directoryName);
  SkinEntryIdResult normalizeEntryPath(const SkinPackageId &package,
                                       std::string_view packageRelativePath);
  std::string installedRelativePath(const SkinEntryId &);
  ```

- [ ] **Step 3: Add `tests/skin_path_policy_tests.cpp` and CMake registration first**. Cover the shared filename-only `normalizeSkinSourceNameNfc` helper; invalid UTF-8, absolute paths, drive/UNC paths, `.`/`..`, repeated separators, NUL, package-name and path depth/byte limits, composed/decomposed NFC equality, full-casefold collisions such as `Straße`/`STRASSE`, preserved authored spelling, `installedRelativePath(package/entry)`, containment of only `packageRelativePath` under an injected package-revision root, and the dependency-free `SkinFloatWriterId` strong-token semantics.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_path_policy_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_path_policy_tests$' --output-on-failure
  ```

  Expected RED: normalization and collision assertions fail.
- [ ] **Step 5: Extend the iOS build-setup test before editing the project, then run it to prove the dependency contract is RED**:

  ```sh
  python3 -m unittest tests/ios_build_setup_tests.py -v
  ```

  Expected RED: at least one required utf8proc device/simulator slice, header, link entry, or license assertion fails. Then add `utf8proc` to vcpkg for desktop/Android, link `utf8proc::utf8proc`, and package `libutf8proc.xcframework`, `utf8proc.h`, and its license through the existing iOS dependency script/project.
- [ ] **Step 6: Implement the Task 3 identity/result/diagnostic interfaces and normalization with utf8proc NFC plus full case folding**. Resolve paths lexically first, then verify canonical/no-follow containment at open time; never turn an absolute host path back into a virtual path.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target skin_path_policy_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_path_policy_tests$' --output-on-failure
  python3 -m unittest tests/ios_build_setup_tests.py -v
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: Unicode cases pass and both desktop/iOS link the same implementation.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt vcpkg.json src/skin/SkinPresentationTypes.h src/skin/package src/skin/CMakeLists.txt tests/skin_path_policy_tests.cpp scripts/get_ios_libs.py ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj assets/legal/utf8proc.txt tests/ios_build_setup_tests.py
  git commit -m "feat: add Unicode-safe skin path identity"
  ```

### Task 4: Persist bounded per-profile skin configuration and viewport layout

**Reference refresh:** `SkinHeader.CustomOption`, `SkinHeader.CustomFile`, `SkinHeader.CustomOffset`, `SkinLuaAccessor.exportSkinProperty`, `SkinConfiguration.java`, and the four synthesized play offsets in Beatoraja's skin configuration flow.

**Files:**

- Create: `src/skin/SkinProfileSettings.h`
- Create: `src/skin/SkinProfileSettings.cpp`
- Create: `src/ProfileSettingsPersistenceCoordinator.h`
- Create: `src/ProfileSettingsPersistenceCoordinator.cpp`
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/AppSettingsStore.h`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/ProfileSessionCoordinator.h`
- Modify: `src/ProfileSessionCoordinator.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/SettingsSceneIr.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/app_settings_store_tests.cpp`
- Test: `tests/profile_switch_tests.cpp`
- Test: `tests/player_profile_manager_tests.cpp`
- Test: `tests/profile_archive_tests.cpp`
- Test: `tests/profile_settings_persistence_tests.cpp`
- Test: `tests/profile_settings_persistence_contract_tests.py`

**Interfaces:**

- Consumes: typed `SkinEntryId` values and profile JSON schema 3.
- Produces: schema 4 plus `SkinProfileSettings {gameplayCompatibilityEnabled, optional<SkinEntryId> selected7KeyEntry, map<SkinEntryId, EntryProfileSettings> entries, sanitize()}`. JSON stores each ID as `{package, path}` and stores the map as an array of `{entry, settings}`; collision keys are derived again after load and never trusted from JSON.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("schema 3 migration disables compatibility") {
  const auto loaded = loadSettingsFixture("schema3.json");
  CHECK_FALSE(loaded.skin.gameplayCompatibilityEnabled);
  CHECK_FALSE(loaded.skin.selected7KeyEntry.has_value());
}
```

- [ ] **Step 2: Make the RED settings tests reference these required profile-owned types and defaults; do not implement serialization yet**:

  ```cpp
  enum class ViewportMode : std::uint8_t { Fit, Stretch, Custom };
  enum class CustomViewportBase : std::uint8_t { Fit, Stretch };

  struct ConfigOffset {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int r = 0;
    int a = 0;
  };

  struct ViewportSettings {
    ViewportMode mode = ViewportMode::Fit;
    CustomViewportBase customBase = CustomViewportBase::Fit;
    float scaleX = 1.0F;
    float scaleY = 1.0F;
    float translateX = 0.0F;
    float translateY = 0.0F;
  };

  struct EntryProfileSettings {
    std::map<std::string, int> options;
    std::map<std::string, std::string> filePaths;
    std::map<std::string, ConfigOffset> offsets;
    ViewportSettings viewport;
  };

  struct SkinProfileId {
    std::string opaque;
    auto operator<=>(const SkinProfileId &) const = default;
  };

  std::optional<SkinProfileId> makeSkinProfileId(
      std::string_view existingPlayerProfileId);

  struct SkinProfileSettings {
    bool gameplayCompatibilityEnabled = false;
    std::optional<SkinEntryId> selected7KeyEntry;
    std::map<SkinEntryId, EntryProfileSettings> entries;
    void sanitize();
  };

  struct VersionedSkinProfileSettings {
    SkinProfileId profileId;
    std::uint64_t generation = 0;
    SkinProfileSettings settings;
  };
  struct SkinProfileCommitResult {
    enum class Status : std::uint8_t {
      Pending,
      Persisted,
      RetryableFailure,
      GenerationChanged
    } status = Status::Pending;
    std::uint64_t ticket = 0;
    bool generationChanged = false;
    std::optional<VersionedSkinProfileSettings> snapshot;
    std::optional<SkinDiagnostic> failure;
  };
  struct ProfileInventorySnapshot {
    std::uint64_t inventoryGeneration = 0;
    std::vector<VersionedSkinProfileSettings> profiles;
  };
  struct AllSkinProfileSnapshotsResult {
    bool complete = false;
    bool cancelled = false;
    std::optional<ProfileInventorySnapshot> inventory;
    std::vector<SkinDiagnostic> diagnostics;
  };
  class ProfileInventoryCommitFence {
  public:
    ProfileInventoryCommitFence(ProfileInventoryCommitFence &&) noexcept;
    ProfileInventoryCommitFence(const ProfileInventoryCommitFence &) = delete;
    ~ProfileInventoryCommitFence();
  };
  class ProfileInventoryMutationBarrier {
  public:
    ProfileInventoryMutationBarrier(ProfileInventoryMutationBarrier &&) noexcept;
    ProfileInventoryMutationBarrier(const ProfileInventoryMutationBarrier &) = delete;
    ~ProfileInventoryMutationBarrier();
  };
  class ISkinProfileSnapshotProvider {
  public:
    virtual ~ISkinProfileSnapshotProvider() = default;
    // Main-thread/no-I/O request; the implementation captures validated typed
    // IDs/settings paths and queues inactive-profile loads on its worker.
    virtual std::uint64_t beginSnapshotAllProfiles() = 0;
    virtual std::optional<AllSkinProfileSnapshotsResult>
    pollSnapshotAllProfiles(std::uint64_t ticket) = 0;
    virtual void cancelSnapshotAllProfiles(std::uint64_t ticket) noexcept = 0;
    // Thread-safe: called only after package validation and before its first
    // visible/revision/catalog mutation. Validates inventory epoch and every
    // per-profile generation, then prevents membership and every profile's
    // skin-generation mutation until release.
    virtual std::optional<ProfileInventoryCommitFence>
    tryAcquireInventoryCommitFence(const ProfileInventorySnapshot &) = 0;
    // Main-thread profile-management gate. Beginning invalidates captured
    // inventories before waiting for any short commit fence.
    virtual ProfileInventoryMutationBarrier
    beginInventoryMutation() = 0;
    virtual void finishInventoryMutation(
        ProfileInventoryMutationBarrier &&) noexcept = 0;
  };
  class ISkinProfileSettingsOwner {
  public:
    virtual ~ISkinProfileSettingsOwner() = default;
    virtual VersionedSkinProfileSettings snapshot(
        const SkinProfileId &) const = 0;
    // Main-thread only: reserve/publish one generation and queue a serialized
    // durable profile save; no filesystem I/O occurs in this call.
    virtual SkinProfileCommitResult beginCommit(
        const SkinProfileId &, std::uint64_t expectedGeneration,
        SkinProfileSettings candidate) = 0;
    // Main-thread only: poll the owner's worker completion for this ticket.
    virtual SkinProfileCommitResult pollCommit(std::uint64_t ticket) = 0;
    // Main-thread only. Completion remains idempotently pollable until the
    // app-owned commit coordinator acknowledges the terminal transaction.
    virtual void acknowledgeCommit(std::uint64_t ticket) noexcept = 0;
  };

  struct ProfileSettingsPersistenceDependencies {
    std::function<bool(const std::filesystem::path &, const AppSettings &,
                       std::string &)> saveAtomic = AppSettingsStore::Save;
  };

  class ProfileSettingsPersistenceCoordinator final
      : public ISkinProfileSettingsOwner,
        public ISkinProfileSnapshotProvider {
  public:
    ProfileSettingsPersistenceCoordinator(
        PlayerProfileManager &, AppSettings &activeSettings,
        ProfileSettingsPersistenceDependencies = {});
    ~ProfileSettingsPersistenceCoordinator(); // idempotent shutdown/join
    VersionedSkinProfileSettings snapshot(
        const SkinProfileId &) const override;
    SkinProfileCommitResult beginCommit(
        const SkinProfileId &, std::uint64_t expectedGeneration,
        SkinProfileSettings candidate) override;
    SkinProfileCommitResult pollCommit(std::uint64_t ticket) override;
    void acknowledgeCommit(std::uint64_t ticket) noexcept override;
    std::uint64_t beginSnapshotAllProfiles() override;
    std::optional<AllSkinProfileSnapshotsResult>
    pollSnapshotAllProfiles(std::uint64_t ticket) override;
    void cancelSnapshotAllProfiles(std::uint64_t ticket) noexcept override;
    std::optional<ProfileInventoryCommitFence>
    tryAcquireInventoryCommitFence(
        const ProfileInventorySnapshot &) override;
    ProfileInventoryMutationBarrier beginInventoryMutation() override;
    void finishInventoryMutation(
        ProfileInventoryMutationBarrier &&) noexcept override;
    // Used by ApplicationContext::saveSettings and profile switching. The
    // caller may wait, but only the owned worker calls saveAtomic.
    bool saveActiveSettingsAndWait(const SkinProfileId &, AppSettings &,
                                   std::string &error);
    bool flushProfileAndWait(const SkinProfileId &, std::string &error);
    // Main-thread profile-switch hook after PlayerProfileManager commits.
    void bindCommittedActiveProfile(SkinProfileId, AppSettings &);
    void shutdown() noexcept;
  };
  ```
- [ ] **Step 3: Extend settings tests first for schema-3 migration, schema-4 round trip, future-version rejection, invalid enum fallback, nonfinite/clamped custom transforms, map/string/count limits, selection/config/layout preservation across profile switch, duplication, archive export, and archive import, plus the concrete coordinator above and a fake owner**. Prove `beginCommit` is main-thread/no-I/O, rejects a stale generation or a second unresolved commit for the same profile, and reports worker completion only through `pollCommit`; a terminal result remains idempotently pollable until `acknowledgeCommit`. Generations are strictly monotonic per profile for the whole process: accepting a commit reserves a newer generation, save failure rolls settings back without reusing that generation, and every successful active-profile bind advances again. Cover A→B→A, an old-A prepare, and an old pending ticket; no ABA value may be accepted. Interleave a full ordinary candidate (including an IR provider edit) while a skin save is pending: `saveActiveSettingsAndWait` must wait for the earlier worker item, merge the resulting durable skin generation into the latest full `AppSettings`, and never overwrite either change. Every queued work item captures its typed profile ID and validated `settings.json` path at enqueue; switch profiles before worker execution and prove it still writes only the captured old path. Test `beginSnapshotAllProfiles` with active plus inactive profiles, ordering behind a pending save, sanitized typed IDs, cancellation, and one corrupt/future/missing inactive settings file: the result must be `complete=false` with no silently omitted profile. Test inventory fencing: a fence succeeds only for the exact epoch/profile generations; `beginInventoryMutation` invalidates old snapshots before waiting; mutation waits for an existing fence; abandoned RAII barriers release; and active skin generation changes invalidate a captured inventory even without membership change. With polling paused, wait/save and then switch profiles behind a completed skin save; neither operation may consume/erase the old owner ticket, which remains pollable until explicit acknowledgment. Task 7 adds the full later-activation-CAS regression after that coordinator exists. Add a lightweight Python source-contract test requiring `SettingsSceneIr.cpp` to call the context full-candidate adapter and contain no `AppSettingsStore::Save`, rather than linking the whole scene/application graph into the unit target. Cover crash-after-save/before-poll recovery from schema-4 settings, switch rollback retaining the old binding/advanced epoch, and shutdown draining or reporting every accepted ticket. Header-declaration reconciliation belongs to Task 10, after headers exist.
- [ ] **Step 3a: Add coordinator unwind and snapshot-ticket ABA regressions**. Snapshot tickets are nonzero, process-monotonic, and never reused across cancellation/profile switch; a late old completion/cancel cannot target a newer request. Destroy the coordinator with a pending skin save and pending all-profile snapshot and prove its destructor's idempotent shutdown cancels/drains and joins before borrowed manager/settings references or ticket/queue state are destroyed. Declare the worker last so reverse member destruction joins it first if construction unwinds.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target app_settings_store_tests profile_switch_tests player_profile_manager_tests profile_archive_tests profile_settings_persistence_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(app_settings_store_tests|profile_switch_tests|player_profile_manager_tests|profile_archive_tests|profile_settings_persistence_tests)$' --output-on-failure
  python3 -m unittest tests/profile_settings_persistence_contract_tests.py -v
  ```

  Expected RED: schema version and missing skin fields fail.
- [ ] **Step 5: Implement the Task 4 profile types and the new concrete `ProfileSettingsPersistenceCoordinator`, add `skin::SkinProfileSettings skin;` to `AppSettings`, and bump `AppSettingsStore::kCurrentSchemaVersion` from 3 to 4**. `ApplicationContext` owns one coordinator after `settings` is loaded and routes `saveSettings()` through `saveActiveSettingsAndWait`. Preserve the existing durable bool contract: `saveSettings()` returns true only after the worker atomically persisted the merged full settings, because existing callers report/roll back on false. Route `SettingsSceneIr`'s real `.storeSettings` callback through a context method that merges/saves the supplied full candidate through this same owner; remove its direct active-path `AppSettingsStore::Save`. Extend `ProfileSessionDependencies` with the owner-backed current-profile full-save plus a no-throw `activeProfileCommitted(profileId, AppSettings &)` hook; remove the direct-store default, full-save the latest ordinary+skin state before database rebinding, and call the bind hook only after every fallible switch step including `activateProfileServices` succeeds. The hook advances the target's retained process-lifetime generation and then notifies skin lifecycle/controller consumers; any rollback keeps the old binding but still never reuses a reserved epoch. Each worker item owns the profile ID, already-validated path, and full candidate captured on the main thread—never resolve `activePaths()` on the worker. The all-profile provider captures the current profile inventory/paths and inventory epoch on the main thread, serializes inactive `AppSettingsStore::Load` calls on its worker, uses the active owner's latest snapshot, and fails the whole result on any unreadable profile. Its commit fence validates the epoch plus every profile generation under one mutex and, until release, blocks `beginCommit`, ordinary full saves that alter skin state, active-profile binding, and inventory membership mutation across the post-validation publication commit. `beginInventoryMutation` increments the epoch before waiting for a fence, so captured inventories fail rather than silently omitting a new/changed profile; finish increments again. Staging writes used to create/duplicate/import an inactive profile may continue to call `AppSettingsStore::Save` directly only while Task 24's inventory mutation barrier is held. For the active profile, `beginCommit` is short/main-thread-only, the worker is the only code that performs durable I/O, and ordinary saves serialize behind a pending skin save and merge its success/failure result. Explicitly add `SkinProfileSettings.cpp` and `SkinPathPolicy.cpp` to every existing root CMake target that compiles `AppSettings.cpp`/`AppSettingsStore.cpp`, link those targets to `utf8proc::utf8proc`, and add `ProfileSettingsPersistenceCoordinator.cpp` to the switch/persistence targets that exercise it. Task 24 owns final shutdown order; do not close this coordinator while skin commit tickets remain. Serialize typed IDs and stable viewport-enum strings, migrate version 3 with sanitized defaults, and keep revision IDs out of settings.
- [ ] **Step 6: Implement `sanitize()` using the fixed limits, retaining valid matching entry keys and deterministically dropping excess keys in sorted order**. Never persist host filesystem paths in the profile object.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target app_settings_store_tests profile_switch_tests player_profile_manager_tests profile_archive_tests profile_settings_persistence_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(app_settings_store_tests|profile_switch_tests|player_profile_manager_tests|profile_archive_tests|profile_settings_persistence_tests)$' --output-on-failure
  python3 -m unittest tests/profile_settings_persistence_contract_tests.py -v
  ```

  Expected GREEN: settings/profile lifecycle tests pass without structural changes to `PlayerProfilePaths` or `ProfileArchive`, and no active-profile `settings.json` write occurs outside the coordinator worker.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/CMakeLists.txt src/skin/SkinProfileSettings.h src/skin/SkinProfileSettings.cpp src/ProfileSettingsPersistenceCoordinator.h src/ProfileSettingsPersistenceCoordinator.cpp src/skin/CMakeLists.txt src/AppSettings.h src/AppSettings.cpp src/AppSettingsStore.h src/AppSettingsStore.cpp src/ProfileSessionCoordinator.h src/ProfileSessionCoordinator.cpp src/context.h src/scene/SettingsSceneIr.cpp tests/app_settings_store_tests.cpp tests/profile_switch_tests.cpp tests/player_profile_manager_tests.cpp tests/profile_archive_tests.cpp tests/profile_settings_persistence_tests.cpp tests/profile_settings_persistence_contract_tests.py
  git commit -m "feat: persist gameplay skin profile settings"
  ```

### Task 5: Create private storage roots, stable tree snapshots, and revision leases

**Reference refresh:** `SkinLoader` loader selection, `LuaSkinLoader.sandboxed`, `LuaSkinAccessor.setDirectory`, and target package parent-sharing behavior.

**Files:**

- Create: `src/skin/SkinStoragePaths.h`
- Create: `src/skin/SkinStoragePaths.cpp`
- Create: `src/skin/package/SkinTreeSnapshotter.h`
- Create: `src/skin/package/SkinTreeSnapshotter.cpp`
- Create: `src/skin/package/SkinAliasDetector.h`
- Create: `src/skin/package/SkinAliasDetector.cpp`
- Create: `src/skin/package/SkinAliasDetectorApple.mm`
- Modify: `src/iOSNatives.hpp`
- Modify: `src/iOSNatives.mm`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/skin_tree_snapshotter_tests.cpp`
- Test: `tests/ios_build_setup_tests.py`

**Interfaces:**

- Consumes: `SkinPackageId`, injected `SkinStorageRoots`, a source folder, `SkinAliasDetector`, `std::stop_token`, and `SkinProgressCallback`.
- Produces: `SnapshotTreeResult SkinTreeSnapshotter::snapshot(const path &sourceRoot, const SkinPackageId &, stop_token, SkinProgressCallback)`, `std::unique_ptr<SkinAliasDetector> createPlatformSkinAliasDetector()`, a move-only `PreparedSkinRevision` that deletes unpublished staging in its destructor, and a move-only `SkinRevisionLease` that pins an immutable published package root.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("snapshot rejects an injected Finder alias") {
  FakeSkinAliasDetector aliases{SkinRejectedLinkKind::AppleFinderAlias};
  CHECK_FALSE(snapshotFixtureTree(aliases).prepared.has_value());
}
```

- [ ] **Step 2: Make the RED snapshot tests compile against these required injectable roots, leases, and result signatures**:

  ```cpp
  struct SkinStorageRoots {
    std::filesystem::path visiblePackages;
    std::filesystem::path privateRevisions;
    std::filesystem::path privateCatalog;
    std::filesystem::path profileOverlays;
  };

  SkinStorageRoots defaultSkinStorageRoots();

  struct SkinPrivateOverlayPathResult {
    std::optional<std::filesystem::path> root;
    std::optional<SkinDiagnostic> failure;
  };
  SkinPrivateOverlayPathResult deriveSkinPrivateOverlayRoot(
      const SkinStorageRoots &, const SkinProfileId &,
      const SkinEntryId &);

  // Non-owning, read-only access. The producing PreparedPackage or lease must
  // outlive every synchronous consumer; this type is never stored in a job.
  class SkinRevisionReadView {
  public:
    const SkinRevision &revision() const noexcept;
    const std::filesystem::path &root() const noexcept;
  };

  class SkinRevisionLease {
  public:
    SkinRevisionLease(SkinRevisionLease &&) noexcept;
    SkinRevisionLease &operator=(SkinRevisionLease &&) noexcept;
    SkinRevisionLease(const SkinRevisionLease &) = delete;
    const SkinRevision &revision() const noexcept;
    const std::filesystem::path &root() const noexcept;
    SkinRevisionReadView readView() const noexcept;
    // Returns another move-only handle over the same shared pin record.
    SkinRevisionLease clone() const;
  };

  class PreparedSkinRevision {
  public:
    PreparedSkinRevision(PreparedSkinRevision &&) noexcept;
    ~PreparedSkinRevision();
    const SkinRevision &revision() const noexcept;
    const std::filesystem::path &stagingRoot() const noexcept;
  };

  struct SnapshotTreeResult {
    std::optional<PreparedSkinRevision> prepared;
    bool cancelled = false;
    std::vector<SkinDiagnostic> diagnostics;
  };

  enum class SkinRejectedLinkKind : std::uint8_t {
    None,
    SymbolicLink,
    HardLink,
    AppleFinderAlias,
    WindowsReparsePoint,
    NonRegular
  };

  class SkinAliasDetector {
  public:
    virtual ~SkinAliasDetector() = default;
    virtual SkinRejectedLinkKind inspectNoFollow(
        const std::filesystem::path &) const = 0;
  };

  std::unique_ptr<SkinAliasDetector> createPlatformSkinAliasDetector();
  ```

- [ ] **Step 3: Write snapshotter tests first for no-follow regular files; symbolic-link, multi-link regular file, FIFO/device/socket, injected Finder-alias, and injected Windows-reparse rejection; deterministic sorting and framed SHA-256; NFC/casefold collision rejection; source mutation before/during/after copy; cancellation; incomplete-staging cleanup; immutable publication; simultaneous leases; and `clone()` producing independently releasable move-only handles that keep one shared revision pin alive until the final handle is destroyed.** Add path-policy tests for `deriveSkinPrivateOverlayRoot`: frame the normalized opaque profile ID plus typed package/entry identity with a version tag, use a lowercase SHA-256 directory name below `profileOverlays`, and prove two profiles × two entries produce four contained roots while traversal, invalid UTF-8, NFC/casefold aliases, and framing-collision attempts are rejected or canonicalized to the same identity. Extend `tests/ios_build_setup_tests.py` first to require the `GetIOSApplicationSupportPath()` native contract and `NSApplicationSupportDirectory`/backup-exclusion implementation. Task 7 owns garbage collection and its leased-revision refusal test.
- [ ] **Step 4: Implement exactly `SkinTreeDigestV1` from the Canonical Digest Grammars section, including its magic, `u64` file count, `u32` path length, `u64` file length, and normalized package-root-relative sorting**. Assert Python-audit/C++ snapshot parity and that two different path/file boundaries cannot produce the same framed byte stream.
- [ ] **Step 5: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_tree_snapshotter_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_tree_snapshotter_tests$' --output-on-failure
  python3 -m unittest tests/ios_build_setup_tests.py -v
  ```

  Expected RED: no snapshotter/private root implementation exists and the iOS Application Support contract is absent.
- [ ] **Step 6: Implement the Task 5 roots/lease/prepared-revision/alias interfaces and iOS `GetIOSApplicationSupportPath()`**. Use `NSApplicationSupportDirectory`, directory creation, and `NSURLIsExcludedFromBackupKey`; use `GetAndroidInternalFilesDir()` on Android and a `.asobmashow-private` child of the existing desktop application root elsewhere. `visiblePackages` remains exactly `Utils::GetDocumentsPath("Skins")`. Derive overlay directories only with `deriveSkinPrivateOverlayRoot`'s versioned length-framed identity hash; never join raw profile/entry text or accept a caller-supplied overlay leaf. Because Xcode's file-system-synchronized group discovers both normal source files, put the generic `createPlatformSkinAliasDetector()` definition behind `#if !defined(__APPLE__)` and the Objective-C++ definition behind `#if defined(__APPLE__)`; do not add normal files to Xcode membership exceptions.
- [ ] **Step 7: Implement a two-pass stable copy: open every source with no-follow semantics, capture type/size/mtime/file identity/link count, stream/hash/copy to private staging, fsync files and parent, then repeat the complete no-follow inventory and content hash**. Reject any metadata or digest difference before revision rename. On Apple, the concrete detector must query `NSURLIsAliasFileKey` without resolving the URL; on Windows, reject reparse points; on all platforms, reject symbolic links, hard links, and non-regular nodes. Do not trust `canonical()` alone as the link boundary.
- [ ] **Step 8: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target skin_tree_snapshotter_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_tree_snapshotter_tests$' --output-on-failure
  python3 -m unittest tests/ios_build_setup_tests.py -v
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: revisions are immutable and iOS uses Application Support while Files sees only `Documents/Skins`.
- [ ] **Step 9: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/SkinStoragePaths.h src/skin/SkinStoragePaths.cpp src/skin/package/SkinTreeSnapshotter.h src/skin/package/SkinTreeSnapshotter.cpp src/skin/package/SkinAliasDetector.h src/skin/package/SkinAliasDetector.cpp src/skin/package/SkinAliasDetectorApple.mm src/skin/CMakeLists.txt src/iOSNatives.hpp src/iOSNatives.mm tests/skin_tree_snapshotter_tests.cpp tests/ios_build_setup_tests.py
  git commit -m "feat: snapshot skins into private revisions"
  ```

### Task 6: Enforce one strict ZIP/folder preparation policy

**Reference refresh:** `LuaSkinLoader.loadHeader`, `LuaSkinLoader.load`, `JSONSkinLoader.loadJsonSkinHeader`, and every target package path/module pattern recorded in Task 1.

**Files:**

- Create: `src/skin/package/SkinArchiveImporter.h`
- Create: `src/skin/package/SkinArchiveImporter.cpp`
- Modify: `src/skin/package/SkinPackageTypes.h`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/skin_archive_importer_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/packages/minimal/skin/play.luaskin`
- Create: `tests/fixtures/beatoraja_skin/packages/minimal/skin/module.lua`

**Interfaces:**

- Consumes: a normalized `SkinPackageId`, Task 5 snapshot/link policy, a ZIP or folder root, `std::stop_token`, and `SkinProgressCallback`.
- Produces: `PreparePackageResult SkinArchiveImporter::prepareArchive(const path &, const SkinPackageId &, stop_token, SkinProgressCallback)` and `PreparePackageResult SkinArchiveImporter::prepareFolder(const path &, const SkinPackageId &, stop_token, SkinProgressCallback)`; the result owns a move-only `PreparedPackage` containing the candidate revision, entry inventory, and unpublished visible/revision staging roots.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("ZIP and folder produce one revision identity") {
  CHECK(prepareZipFixture().prepared->candidateRevision().lowercaseSha256 ==
        prepareFolderFixture().prepared->candidateRevision().lowercaseSha256);
}
```

- [ ] **Step 2: Make the RED importer tests reference these required move-only preparation results**:

  ```cpp
  enum class PackageCollisionPolicy : std::uint8_t { Reject, Replace };

  class PreparedPackage {
  public:
    PreparedPackage(PreparedPackage &&) noexcept;
    PreparedPackage &operator=(PreparedPackage &&) noexcept;
    ~PreparedPackage();
    const SkinPackageId &packageId() const noexcept;
    const SkinRevision &candidateRevision() const noexcept;
    std::span<const SkinEntryId> entries() const noexcept;
    const std::filesystem::path &visibleStagingRoot() const noexcept;
    SkinRevisionReadView readView() const noexcept;
  };

  struct PreparePackageResult {
    std::optional<PreparedPackage> prepared;
    bool cancelled = false;
    std::vector<SkinDiagnostic> diagnostics;
  };
  ```

- [ ] **Step 3: Add archive/folder parity tests first**. Build hostile ZIPs in the test itself and cover absolute/traversal names, duplicate normalized names, casefold/NFC collisions, directory/file collisions, symlink/hardlink/sparse/encrypted/nonregular entries, conservative rejection of every `__MACOSX/._*` or other `._*` AppleDouble sidecar, truncation, CRC/read errors, unsupported compression, every fixed limit, cancellation, and cleanup. Standard ZIP has no portable hardlink-target or sparse-map representation, and libarchive's ZIP writer does not serialize those attributes; therefore the representable raw special-node hostile fixtures are symlink entries plus central-directory Unix-mode-patched FIFO, socket, and character-device entries. Keep runtime rejection of `archive_entry_hardlink()` and any nonzero `archive_entry_sparse_count()` so a reader that does surface either metadata still fails closed rather than treating it as an untested regular file. Cover the canonical wrapper rule with a single-wrapper ZIP, root-file ZIP, multiple-top-level ZIP, an accepted explicit wrapper-root directory, a rejected regular/special entry masquerading as that directory, an explicit directory outside the inferred wrapper, and a post-strip normalization collision. Assert the same logical package installed from inferred-wrapper ZIP, picked folder contents, and a manual direct-child `Documents/Skins/<package>` tree yields the same revision digest and entry inventory.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_archive_importer_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_archive_importer_tests$' --output-on-failure
  ```

  Expected RED: permissive archive extraction cannot satisfy whole-package rejection.
- [ ] **Step 5: Implement the Task 6 prepared-package/result interfaces and ZIP-only streaming with libarchive**. Borrow defensive checks from `ProfileArchive.cpp::validateArchive` while keeping skin limits/types independent. Inventory and normalize every regular-file and explicit-directory entry before extraction, infer zero-or-one wrapper from regular files exactly by the canonical rule, ignore only the safe wrapper-root directory after stripping, then stream only nonempty post-strip regular-file paths and create directories from their safe parents. Fail the entire import on the first unsafe entry, special node, duplicate, or file/directory collision; never strip a leading slash, heuristically search for a skin subdirectory, skip an unsafe member, or accept a wrapper decision that differs between inventory and extraction.
- [ ] **Step 6: Implement folder preparation through `SkinTreeSnapshotter`, using the picked folder itself as one package root**. Both paths must discover `.luaskin` recursively only after a stable private candidate exists, and must diagnose loose files at the canonical `Skins` root later rather than widening package boundaries.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target skin_archive_importer_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_archive_importer_tests$' --output-on-failure
  ```

  Expected GREEN: ZIP and folder manifests match and every hostile case leaves no published package or revision.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/package/SkinArchiveImporter.h src/skin/package/SkinArchiveImporter.cpp src/skin/package/SkinPackageTypes.h src/skin/CMakeLists.txt tests/skin_archive_importer_tests.cpp tests/fixtures/beatoraja_skin/packages/minimal
  git commit -m "feat: validate skin archives and folders"
  ```

### Task 7: Publish, rescan, recover, activate, and collect packages atomically

**Reference refresh:** `SkinLoader.load`, `LuaSkinLoader.loadHeader`, `LuaSkinLoader.load`, and `SkinHeader.setSkinConfigProperty`.

**Files:**

- Create: `src/skin/package/SkinPackageCatalog.h`
- Create: `src/skin/package/SkinPackageCatalog.cpp`
- Create: `src/skin/package/SkinPackageStore.h`
- Create: `src/skin/package/SkinPackageStore.cpp`
- Create: `src/skin/package/SkinPackageOperationService.h`
- Create: `src/skin/package/SkinPackageOperationService.cpp`
- Create: `src/skin/SkinCommitCoordinator.h`
- Create: `src/skin/SkinCommitCoordinator.cpp`
- Modify: `src/skin/package/SkinPackageTypes.h`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/skin_package_store_tests.cpp`

**Interfaces:**

- Consumes: `PreparedPackage`, profile ID/settings, Task 5 roots/leases, one app-owned `SkinPackageCatalog`, catalog generation, and an injected `SkinEntryValidator`.
- Produces: typed recovery/prepare/publish/scan/GC results; one synchronous exclusive `SkinPackageStore::recoverBeforeServiceStart()` bootstrap; one app-owned serialized `SkinPackageOperationService` for every later filesystem/validation operation and discarded staging cleanup; store-level main-thread begin/poll operations; one scene-independent app-owned `SkinCommitCoordinator` for activation and profile-only skin commits; and `AcquireActivationResult acquireValidatedActivation(profileId, entry, configurationDigest)`. Validation never mutates profile/catalog state; the main-thread commit serializes the durable profile update before the activation CAS.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("invalid requested configuration retains activation") {
  const auto result = prepareInvalidReplacementFixture();
  CHECK_FALSE(result.prepared.has_value());
  CHECK(result.previousActivation->configurationDigest == previousDigest());
}
```

- [ ] **Step 2: Define test doubles and make the RED store tests reference these concrete lifecycle/validator contracts**:

  ```cpp
  enum class SkinValidationDisposition : std::uint8_t {
    Selectable7Key,
    UnavailableType,
    Invalid
  };

  struct SkinCatalogCategoryDeclaration {
    std::string name;
    std::vector<std::string> items;
  };
  struct SkinCatalogOptionChoice {
    std::string label;
    int value = 0;
  };
  struct SkinCatalogOptionDeclaration {
    std::string category;
    std::string name;
    std::vector<SkinCatalogOptionChoice> choices;
    std::string defaultLabel;
  };
  struct SkinCatalogFileDeclaration {
    std::string category;
    std::string name;
    std::string pattern;
    std::string defaultValue;
    std::vector<std::string> choices;
  };
  struct SkinCatalogOffsetDeclaration {
    std::string category;
    std::string name;
    int id = 0;
    std::uint8_t permissions = 0;
  };
  struct SkinEntryMetadataSnapshot {
    std::string displayName;
    std::string author;
    int skinType = -1;
    int authoredWidth = 0;
    int authoredHeight = 0;
    std::vector<SkinCatalogCategoryDeclaration> categories;
    std::vector<SkinCatalogOptionDeclaration> options;
    std::vector<SkinCatalogFileDeclaration> files;
    std::vector<SkinCatalogOffsetDeclaration> offsets;
  };

  struct SkinValidationResult {
    SkinValidationDisposition disposition =
        SkinValidationDisposition::Invalid;
    bool cancelled = false;
    std::optional<EntryProfileSettings> reconciledSettings;
    std::optional<SkinEntryMetadataSnapshot> metadata;
    std::string configurationDigest;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct PublishPackageResult {
    bool published = false;
    bool retryableInventoryRace = false;
    // Present only when a post-validation inventory-fence race prevented the
    // first mutation. The independently owned staging is returned intact so
    // the caller can reload inventory and revalidate without picker access.
    std::optional<PreparedPackage> retryPrepared;
    SkinPackageId package;
    std::vector<SkinEntryId> entries;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct ScanPackagesResult {
    bool cancelled = false;
    bool retryableInventoryRace = false;
    std::uint64_t sourceGeneration = 0;
    std::vector<SkinEntryId> discoveredEntries;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct GarbageCollectionResult {
    std::uint64_t revisionsRemoved = 0;
    std::uint64_t bytesRemoved = 0;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct RemovePackageResult {
    bool removed = false;
    bool cancelled = false;
    SkinPackageId package;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct ValidatedSkinActivation {
    SkinRevisionLease revision;
    SkinEntryId entry;
    EntryProfileSettings reconciledSettings;
    std::string configurationDigest;
  };

  struct PreparedSkinActivation {
    std::uint64_t sourceGeneration = 0;
    std::uint64_t catalogGeneration = 0;
    std::uint64_t expectedProfileGeneration = 0;
    SkinProfileId profileId;
    ValidatedSkinActivation activation;
    SkinProfileSettings candidateProfileSettings;
  };

  struct PrepareActivationResult {
    std::optional<PreparedSkinActivation> prepared;
    std::optional<ValidatedSkinActivation> previousActivation;
    bool cancelled = false;
    std::vector<SkinDiagnostic> diagnostics;
  };

  enum class ActivationCommitDisposition : std::uint8_t {
    PendingProfileSave,
    ActivatedRequested,
    RetainedPrevious,
    ProfileGenerationChanged,
    SourceGenerationChanged,
    ProfileCommittedNeedsRevalidation
  };
  struct CommitActivationResult {
    ActivationCommitDisposition disposition =
        ActivationCommitDisposition::RetainedPrevious;
    std::uint64_t ticket = 0;
    std::optional<ValidatedSkinActivation> activation;
    std::optional<VersionedSkinProfileSettings> profileSnapshot;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct AcquireActivationResult {
    std::optional<ValidatedSkinActivation> activation;
    std::vector<SkinDiagnostic> diagnostics;
  };

  using SkinActivationClientId = std::uint64_t;
  struct SkinActivationSubmissionResult {
    bool accepted = false;
    std::uint64_t ticket = 0;
    std::vector<SkinDiagnostic> diagnostics;
  };
  struct SkinActivationCompletion {
    SkinActivationClientId client = 0;
    std::uint64_t ticket = 0;
    CommitActivationResult result;
  };
  struct SkinProfileCommitSubmissionResult {
    bool accepted = false;
    std::uint64_t ticket = 0;
    std::vector<SkinDiagnostic> diagnostics;
  };
  struct SkinProfileCommitCompletion {
    SkinActivationClientId client = 0;
    std::uint64_t ticket = 0;
    SkinProfileCommitResult result;
  };
  class SkinProfileMutationBarrier {
  public:
    SkinProfileMutationBarrier(SkinProfileMutationBarrier &&) noexcept;
    SkinProfileMutationBarrier(const SkinProfileMutationBarrier &) = delete;
    // An unfinished barrier resumes submissions without deleting activation.
    ~SkinProfileMutationBarrier();
    const SkinProfileId &profileId() const noexcept;
  };
  struct BeginSkinProfileMutationResult {
    std::optional<SkinProfileMutationBarrier> barrier;
    std::string error;
  };

  struct SkinCatalogEntrySnapshot {
    SkinEntryId entry;
    std::string revisionDigest;
    SkinValidationDisposition validation =
        SkinValidationDisposition::Invalid;
    std::optional<SkinEntryMetadataSnapshot> metadata;
    std::vector<std::string> validatedConfigurationDigests;
    std::vector<SkinDiagnostic> diagnostics;
  };
  struct SkinPackageCatalogSnapshot {
    std::uint64_t catalogGeneration = 0;
    std::uint64_t sourceGeneration = 0;
    std::vector<SkinPackageId> packages;
    std::vector<SkinCatalogEntrySnapshot> entries;
  };

  class SkinEntryValidator {
  public:
    virtual ~SkinEntryValidator() = default;
    virtual SkinValidationResult validate(
        SkinRevisionReadView revision, const SkinEntryId &entry,
        const EntryProfileSettings *desiredSettings,
        std::stop_token stop) = 0;
  };

  struct SkinDiagnosticHistoryRecord; // completed by Task 21
  class SkinPackageCatalog {
  public:
    explicit SkinPackageCatalog(
        std::filesystem::path privateCatalogRoot);
    ~SkinPackageCatalog(); // calls idempotent no-throw shutdown
    std::shared_ptr<const SkinPackageCatalogSnapshot>
    snapshot() const noexcept;
    std::vector<SkinDiagnosticHistoryRecord>
    loadDiagnosticHistory() const;
    bool replaceDiagnosticHistory(
        std::span<const SkinDiagnosticHistoryRecord>);
    // Waits for every already-enqueued deep-copied snapshot write.
    void flush();
    // Idempotently closes submissions, flushes, and joins the catalog worker.
    void shutdown() noexcept;
  private:
    friend class SkinPackageStore;
    // Catalog metadata recovery is part of the store-owned cross-root replay.
    void recover();
  };

  enum class SkinRecoveryDisposition : std::uint8_t {
    Recovered,
    AlreadyRecovered,
    ConcurrentCallRejected,
    Failed
  };
  struct SkinRecoveryResult {
    SkinRecoveryDisposition disposition = SkinRecoveryDisposition::Failed;
    std::vector<SkinDiagnostic> diagnostics;
  };

  class SkinPackageStore {
  public:
    SkinPackageStore(SkinStorageRoots, SkinPackageCatalog &,
                     SkinAliasDetector &,
                     ISkinProfileSnapshotProvider &);
    // Synchronous exclusive bootstrap, completed before service construction.
    SkinRecoveryResult recoverBeforeServiceStart();
    PreparePackageResult prepareArchive(const std::filesystem::path &zip,
                                        const SkinPackageId &package,
                                        std::stop_token,
                                        SkinProgressCallback);
    PreparePackageResult prepareFolder(const std::filesystem::path &folder,
                                       const SkinPackageId &package,
                                       std::stop_token,
                                       SkinProgressCallback);
    PublishPackageResult publish(PreparedPackage &&,
                                 PackageCollisionPolicy,
                                 ProfileInventorySnapshot,
                                 SkinEntryValidator &,
                                 std::stop_token,
                                 SkinProgressCallback);
    ScanPackagesResult rescanVisibleSources(std::stop_token,
                                            SkinProgressCallback,
                                            ProfileInventorySnapshot,
                                            SkinEntryValidator &);
    PrepareActivationResult prepareActivation(
        const VersionedSkinProfileSettings &, const SkinEntryId &,
        SkinProfileSettings candidateProfileSettings,
        SkinEntryValidator &, std::stop_token);
    // Main-thread only; begins no-I/O owner commit and retains the prepared
    // activation under the returned ticket.
    CommitActivationResult beginPreparedActivationCommit(
        PreparedSkinActivation &&, ISkinProfileSettingsOwner &);
    // Main-thread only; polls owner persistence and performs the short catalog
    // generation-CAS after persistence succeeds.
    CommitActivationResult pollPreparedActivationCommit(
        std::uint64_t ticket, ISkinProfileSettingsOwner &);
    AcquireActivationResult acquireValidatedActivation(
        const SkinProfileId &, const SkinEntryId &,
        std::string_view configurationDigest);
    std::shared_ptr<const SkinPackageCatalogSnapshot>
    catalogSnapshot() const noexcept;
    RemovePackageResult removePackage(const SkinPackageId &,
                                      std::stop_token);
    void removeProfileActivations(const SkinProfileId &);
    void reconcileProfileActivations(
        std::span<const SkinProfileId> existingProfiles);
    GarbageCollectionResult collectGarbage();
  private:
    ISkinProfileSnapshotProvider &profileSnapshots_;
    // Protects source/catalog generations and activation maps against the
    // operation worker versus main-thread activation CAS. Filesystem/journal
    // operations themselves have the one service FIFO as their owner.
    std::mutex stateMutex_;
  };

  class SkinDeferredCleanup {
  public:
    SkinDeferredCleanup() noexcept = default;
    // The action is value-owned, invoked at most once on the operation worker,
    // and exceptions are swallowed/reported. A caller can move an otherwise
    // move-only cleanup payload into a shared state captured by this action.
    explicit SkinDeferredCleanup(std::function<void()> action);
    SkinDeferredCleanup(SkinDeferredCleanup &&) noexcept;
    SkinDeferredCleanup &operator=(SkinDeferredCleanup &&) noexcept;
    SkinDeferredCleanup(const SkinDeferredCleanup &) = delete;
    SkinDeferredCleanup &operator=(const SkinDeferredCleanup &) = delete;
    ~SkinDeferredCleanup();
    void run() noexcept;
  private:
    std::function<void()> action_;
  };

  struct ReconcileProfileActivationsResult {
    bool completed = false;
    std::vector<SkinDiagnostic> diagnostics;
  };
  using SkinPackageOperationPayload = std::variant<
      PreparePackageResult, PublishPackageResult, ScanPackagesResult,
      RemovePackageResult, PrepareActivationResult,
      GarbageCollectionResult, ReconcileProfileActivationsResult>;
  struct SkinPackageOperationCompletion {
    std::uint64_t ticket = 0;
    SkinPackageOperationPayload payload;
  };
  class SkinPackageProgressMailbox {
  public:
    // Thread-safe snapshots only. Worker publication never calls user code or
    // synchronously dispatches to the main thread.
    SkinProgress snapshot() const noexcept;
  private:
    friend class SkinPackageOperationService;
    void publish(SkinProgress) noexcept;
  };
  struct SkinPackageOperationHandle {
    std::uint64_t ticket = 0;
    std::shared_ptr<const SkinPackageProgressMailbox> progress;
  };
  class SkinPackageOperationService {
  public:
    // Requires store.recoverBeforeServiceStart() to have completed.
    SkinPackageOperationService(SkinPackageStore &, SkinEntryValidator &);
    ~SkinPackageOperationService();
    // Tickets are nonzero, process-monotonic, and never reused. Each request
    // owns its immutable progress mailbox; discarding it detaches UI safely.
    SkinPackageOperationHandle submitPrepareArchive(
        std::filesystem::path, SkinPackageId, SkinDeferredCleanup);
    SkinPackageOperationHandle submitPrepareFolder(
        std::filesystem::path, SkinPackageId, SkinDeferredCleanup);
    SkinPackageOperationHandle submitPublish(
        PreparedPackage, PackageCollisionPolicy, ProfileInventorySnapshot);
    SkinPackageOperationHandle submitRescan(ProfileInventorySnapshot);
    SkinPackageOperationHandle submitRemove(SkinPackageId);
    SkinPackageOperationHandle submitPrepareActivation(
        VersionedSkinProfileSettings, SkinEntryId,
        SkinProfileSettings candidate);
    SkinPackageOperationHandle submitGarbageCollection();
    SkinPackageOperationHandle submitReconcileProfileActivations(
        std::vector<SkinProfileId> existingProfiles);
    // Main-thread, read/lease-only facades; these perform no filesystem work.
    AcquireActivationResult acquireValidatedActivation(
        const SkinProfileId &, const SkinEntryId &,
        std::string_view configurationDigest);
    std::shared_ptr<const SkinPackageCatalogSnapshot>
    catalogSnapshot() const noexcept;
    std::optional<SkinPackageOperationCompletion> poll(std::uint64_t);
    // Requests stop and transfers result/staging/cleanup disposal to the
    // service worker; the caller may immediately forget the ticket.
    void cancelAndDetach(std::uint64_t) noexcept;
    void discardPrepared(PreparedPackage, SkinDeferredCleanup = {});
    void shutdown() noexcept;
  };

  class SkinCommitCoordinator {
  public:
    SkinCommitCoordinator(SkinPackageStore &,
                          ISkinProfileSettingsOwner &);
    ~SkinCommitCoordinator(); // calls idempotent no-throw shutdown
    // Main-thread only; 0 is invalid and IDs monotonically increase without
    // process-lifetime reuse, including after detachClient.
    SkinActivationClientId createClient();
    SkinActivationSubmissionResult submitActivation(
        SkinActivationClientId, PreparedSkinActivation &&);
    // Compatibility toggles and viewport-only changes do not require Lua
    // validation or an activation CAS, but use the same durable owner/tickets.
    SkinProfileCommitSubmissionResult submitProfileSettings(
        SkinActivationClientId, const VersionedSkinProfileSettings &base,
        SkinProfileSettings candidate);
    // Main-thread application-lifecycle polling; never owned by Settings.
    void poll();
    std::vector<SkinActivationCompletion> takeCompletions(
        SkinActivationClientId);
    std::vector<SkinProfileCommitCompletion> takeProfileCompletions(
        SkinActivationClientId);
    // Stops delivery to a closed scene but deliberately does not abandon the
    // accepted durable transaction or its revision lease.
    void detachClient(SkinActivationClientId) noexcept;
    std::vector<VersionedSkinProfileSettings> takeRevalidationRequests();
    // Profile-management barrier: stop submissions for this ID, wait for its
    // owner work, and poll every activation/profile-only ticket to terminal.
    BeginSkinProfileMutationResult beginProfileMutation(
        const SkinProfileId &);
    // On success removes activation keys; if profileStillExists (archive
    // replacement), submissions resume. Failure always resumes without remove.
    void finishProfileMutation(SkinProfileMutationBarrier &&,
                               bool mutationSucceeded,
                               bool profileStillExists) noexcept;
    void shutdown() noexcept;
  };
  ```

- [ ] **Step 3: Write store/service tests first for new import with default configuration validation for every discovered 7-key entry plus configured-main/model/resource validation for every profile snapshot whose selected entry belongs to the candidate package; callback-free metadata/declaration persistence, bounded normalized custom-file choices, and restart reload; reject/replace collision without merging; cancellation during publish validation leaving the old package whole; and a replacement whose defaults pass but one selected profile's option/file configuration fails, proving the prior visible source, revision, activation, and all profile settings remain eligible/unchanged.** Validate unpublished candidates only through `PreparedPackage::readView()` while the package owner remains alive; prove no validator can obtain or retain a published lease to staging. Submit rescan↔publish, rescan↔remove, GC↔publish, activation-prepare↔manual edit, and shutdown races through the single service FIFO; no two filesystem/journal operations overlap, main-thread activation CAS uses only the short state mutex, and no worker holds that mutex while waiting on a profile fence/callback. Inject blocking cleanup/deletion and prove `cancelAndDetach`, controller-style close, and `discardPrepared` return immediately while the worker eventually destroys exactly the staging/source capability; shutdown drains or reports every ticket. Every request/result owns paths, IDs, settings, inventory, stop state, and cleanup by value—never a controller `this`, stack reference, or picker-scoped URL.

  Race create, duplicate, import-create, overwrite, and delete both during all-profile load/validation and immediately before publication. The store must acquire the injected provider's inventory commit fence after validation and before its first journal/visible/revision/catalog mutation; stale epoch or any profile generation returns `retryableInventoryRace=true`, makes zero mutation, returns the still-owned `PreparedPackage` in `retryPrepared`, and forces a fresh inventory/full validation without reopening the picker. Only success, explicit cancellation, or a permanent validation/publication failure cleans staging, always on the operation worker. Also cover crash recovery through the exact `recoverBeforeServiceStart()` API after every staging/visible/revision/catalog rename and parent fsync boundary. Require the first call to return `Recovered`, a later call to return `AlreadyRecovered` without replay, and an overlapping call to return `ConcurrentCallRejected` without touching files. A corrupt journal-owned final `privateRevisions/<newDigest>` must be removed or quarantined after its digest fails, while a valid unreferenced revision survives. Continue with direct-child package discovery, nested entry discovery, loose-root diagnostic, manual valid edit, unstable edit retry, invalid manual edit preserving last valid activation while the user-edited visible tree remains diagnosed; explicit cancellable whole-package removal with leased revision survival; manual package deletion fallback; per-entry failure isolation; configuration digest separation; worker prepare with zero mutation; main-thread profile-save failure retaining prior settings/activation; stale profile generation rejected; crash/source race after profile save but before activation yielding `ProfileCommittedNeedsRevalidation`; startup revalidation repair; invalid changed configuration retaining the prior activation; active lease survival; cloned revision pins retaining GC eligibility until the final clone is released; and unreferenced revision GC. Add coordinator tests where client IDs start above zero, increase monotonically, and are never reused: detach an old controller with an accepted transaction, allocate a new client for another profile, finish the old transaction, and prove its completion cannot be observed under the new ID. Settings closing while the owner save is pending must still let app polling complete the activation; a detached client cannot leak a completion or lease; shutdown drains a completed save/CAS or leaves durable desired settings for startup revalidation; and profile deletion/import-overwrite removes orphaned activation keys after pending work is quiesced. Require an abandoned `SkinProfileMutationBarrier` to resume submissions without deleting activation. Pause app polling, let an accepted owner save complete, perform an ordinary full-save and profile switch, then resume polling: the old profile's idempotent result must still complete exactly one activation CAS, release its retained lease, and only then call `acknowledgeCommit`.
  Require nonzero process-monotonic never-reused operation tickets and value-owned progress mailboxes. Detach an old controller, create a new operation, then publish late progress/completion from the old operation and prove neither can target or reenter the new controller; progress publication is a nonblocking snapshot update even while an inventory fence is held and never invokes user code. Race close/profile-switch against progress publication and every operation phase.

- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_package_store_red_tests -j 6
  ./cmake-build-debug/skin_package_store_red_tests
  ```

  Expected RED: the executable exits nonzero because no journal/catalog/store/coordinator behavior exists. This opt-in contract target is not registered with CTest; Task 7 GREEN promotes it to `skin_package_store_tests` and registers it.
- [ ] **Step 5: Implement the Task 7 result/validator/catalog/store interfaces, the app-owned `SkinPackageOperationService`, the separately owned `SkinCommitCoordinator.{h,cpp}`, and private catalog JSON with `VersionedJson` plus `AtomicFile`**. Add service/coordinator/package/profile sources explicitly to `src/skin/CMakeLists.txt` and to the root CMake source list of every focused store/settings/lifecycle test (including `SkinProfileSettings.cpp`, `ProfileSettingsPersistenceCoordinator.cpp`, `SkinPathPolicy.cpp`, and utf8proc where IDs are decoded); root CMake test targets do not inherit the app target's source graph. Application context owns exactly one catalog and store. Immediately after constructing them, it calls `store.recoverBeforeServiceStart()` synchronously and exactly once; only a completed `Recovered` result permits construction of Task 21's one operation service after its concrete validator. Recovery holds exclusive bootstrap ownership before that FIFO exists. A repeated call returns `AlreadyRecovered`, an overlapping call returns `ConcurrentCallRejected`, neither performs filesystem work, and failure keeps the skin subsystem unavailable rather than allowing the service to race partial replay. `SkinPackageCatalog::recover()` is private, friend-only store implementation detail, so no lifecycle, worker, or caller can replay catalog metadata independently of visible/revision state. Task 21 shares the service with Settings/lifecycle. Inject the same catalog plus all-profile provider into `SkinPackageStore`, retain the provider by reference as `profileSnapshots_`, and inject the catalog into `SkinDiagnosticHistory` in Task 21 so mutable history/activation state cannot diverge. After bootstrap, the operation service is the only application caller of archive/folder prepare, publish, rescan, remove, activation preparation, and GC; it serializes them on one owned worker and owns deferred cleanup/result disposal. Callers supply one complete `ProfileInventorySnapshot`: publish validates defaults plus every selected configuration targeting the candidate package, then uses `profileSnapshots_.tryAcquireInventoryCommitFence`; only a valid fence permits the first publication mutation and remains held through the short journal/rename/catalog commit. A fence race moves the candidate back into `PublishPackageResult::retryPrepared`; the controller resubmits it through the service after fresh inventory/validation. Any selected/permanent failure or fence failure preserves the old visible package. Inactive-profile activation generations are not mutated by validation and are rechecked when that profile becomes active. A manual Files edit cannot be rolled back at the visible-source layer, so scan retains the prior validated revision/activation per affected profile and records the invalid current-source diagnostic. Journal each cross-root publication phase before mutation, make replay idempotent, remove or quarantine a journal-owned final revision whose physical tree does not match its typed digest without sweeping valid unreferenced revisions, and never expose a selectable activation until visible publication, immutable revision, entry validation, fenced catalog commit all succeed.
- [ ] **Step 6: Key catalog identity by `installedRelativePath(entry)`, which joins the direct-child `SkinPackageId::directoryName` and `SkinEntryId::packageRelativePath`**. A revision lease root is always that package's private root, so all opens resolve only `packageRelativePath` beneath it. Key validation by entry, revision digest, and canonical configuration digest; key the selected activation additionally by opaque profile ID.
- [ ] **Step 7: Make `pollPreparedActivationCommit` the only writer of a profile activation and require both store commit methods on the main thread**. `prepareActivation` validates/reconciles a whole candidate profile copy on a worker and captures source/catalog/profile generations without mutation. `beginPreparedActivationCommit` rechecks source/catalog generations, calls the owner's no-I/O `beginCommit(expectedProfileGeneration, candidate)`, retains the prepared activation by ticket, and returns `PendingProfileSave`; stale generation leaves old settings/activation. The app-owned `SkinCommitCoordinator`, not a scene/controller, owns every accepted owner/store ticket and lease and polls them from application-lifecycle polling. `submitProfileSettings` uses the same owner begin/poll flow but deliberately performs no activation CAS. Ordinary saves/waits never consume a terminal owner result; the coordinator acknowledges it only after the activation CAS or profile-only terminal completion is recorded. Persistence success performs only an in-memory generation CAS/activation-pointer swap on the main thread. `SkinPackageCatalog` then deep-copies the complete private-catalog snapshot before enqueue and coalesces its JSON write on its own worker; no borrowed span/reference crosses the call, and no `AtomicFile`, fsync, or other filesystem call occurs in `pollPreparedActivationCommit`. A crash before that catalog snapshot is harmless because durable profile settings are the source of truth and startup revalidation reconstructs the activation. Retryable failure retains the old activation. A source/catalog race after profile save returns `ProfileCommittedNeedsRevalidation`, publishes no requested activation, and emits a revalidation request from the new owner snapshot. Detaching a UI client only discards its bounded delivery record; the transaction still reaches a terminal state. Coordinator shutdown stops submissions and resolves its own tickets but does not flush or close the separately owned catalog; Task 24 owns catalog flush/shutdown after diagnostic history has enqueued its final deep-copied state. Import/rescan validate defaults; selection and every option/file/offset edit use prepare→coordinator submit→application poll. Compatibility toggles and viewport-only changes use `submitProfileSettings`. Viewport is excluded from Lua validation/digest and carried separately at chart start.
- [ ] **Step 8: On manual delete, remove the entry from discovery immediately, retain leased revisions, and return no activation for new sessions**. On invalid/partial edit, retain the prior validated activation and attach diagnostics to the current source generation. `beginProfileMutation` rejects new submissions for that ID, waits for its owner work, and polls even a save-completed/CAS-pending activation to terminal; the move-only barrier is mandatory before delete/overwrite. `finishProfileMutation(..., false, ...)` preserves activations and resumes submissions after a failed operation. A successful delete removes activation keys and keeps the absent ID blocked; a successful archive overwrite removes old activation keys then resumes the still-existing ID for revalidation. At startup, `reconcileProfileActivations` removes catalog keys for profile IDs absent from `PlayerProfileManager::listProfiles()`.
- [ ] **Step 9: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target skin_package_store_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_package_store_tests$' --output-on-failure
  ```

  Expected GREEN: the exclusive pre-service call recovers fault injection at every boundary to exactly the old or new whole package, never a merge, rejects repeated/overlapping ownership, quarantines corrupt journal-owned revisions without sweeping valid unrelated revisions, and leaves no detached or deleted profile with a live ticket/lease/catalog activation.
- [ ] **Step 10: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/package/SkinPackageCatalog.h src/skin/package/SkinPackageCatalog.cpp src/skin/package/SkinPackageStore.h src/skin/package/SkinPackageStore.cpp src/skin/package/SkinPackageOperationService.h src/skin/package/SkinPackageOperationService.cpp src/skin/SkinCommitCoordinator.h src/skin/SkinCommitCoordinator.cpp src/skin/package/SkinPackageTypes.h src/skin/CMakeLists.txt tests/skin_package_store_tests.cpp
  git commit -m "feat: add transactional skin package store"
  ```

### Task 8: Build the package-local Lua filesystem and private data overlay

**Reference refresh:** `SkinLuaAccessor.createSandboxGlobals`, `SkinLuaAccessor.restrictPackageLoaders`, `SkinLuaAccessor.setDirectory`, `SkinLuaAccessor.execFile`, `SkinFileLuaApiExporter`, `SkinLuaPathResolver`, `LegacySkinLuaApi.NewFunction`, and `LegacySkinLuaApi.fileFacade`.

**Files:**

- Create: `src/skin/beatoraja/SkinCompatibilityDiagnostics.h`
- Create: `src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp`
- Create: `src/skin/beatoraja/LuaSkinFileSystem.h`
- Create: `src/skin/beatoraja/LuaSkinFileSystem.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/lua_skin_file_system_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/lua/filesystem/entry/play.luaskin`
- Create: `tests/fixtures/beatoraja_skin/lua/filesystem/shared.lua`

**Interfaces:**

- Consumes: a `ValidatedSkinActivation`, typed profile/entry identity, `SkinStorageRoots`, file use, byte/file limits, and current runtime phase. It derives the private overlay internally; no caller supplies an arbitrary overlay path.
- Produces: virtual-path-only `SkinFileResolveResult`, `SkinFileReadResult`, `SkinFileListResult`, and `SkinFileWriteResult`; each carries a typed `SkinFileFailure {code, virtualPath, message}` and never a host path.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("captured closure cannot read after render phase") {
  auto fixture = makeFileSystemClosureFixture();
  fixture.fileSystem.enterRenderPhase();
  CHECK(fixture.readClosure().failure->code == SkinFileError::RenderPhase);
}
```

- [ ] **Step 2: Make the RED filesystem tests reference these explicit uses, failures, and phase-sensitive operations**:

  ```cpp
  enum class SkinFileUse : std::uint8_t {
    LuaEntry,
    LuaModule,
    Resource,
    DataRead,
    DataWrite
  };

  enum class SkinFileError : std::uint8_t {
    InvalidPath,
    EscapesPackage,
    WrongUse,
    Missing,
    NonRegular,
    BinaryChunk,
    LimitExceeded,
    QuotaExceeded,
    RenderPhase,
    IoError
  };

  struct SkinFileFailure {
    SkinFileError code;
    std::string virtualPath;
    std::string message;
  };

  struct SkinFileResolveResult {
    std::optional<std::string> normalizedVirtualPath;
    std::optional<SkinFileFailure> failure;
  };

  struct SkinFileReadResult {
    std::vector<std::byte> bytes;
    std::optional<SkinFileFailure> failure;
  };

  struct SkinFileListResult {
    std::vector<std::string> entries;
    std::optional<SkinFileFailure> failure;
  };

  struct SkinFileWriteResult {
    std::uint64_t resultingBytes = 0;
    std::uint64_t resultingFiles = 0;
    std::optional<SkinFileFailure> failure;
  };

  struct SkinFileActivityCounters {
    std::uint64_t renderReadsPerformed = 0;
    std::uint64_t renderReadsDenied = 0;
    std::uint64_t renderWritesPerformed = 0;
    std::uint64_t renderWritesDenied = 0;
    std::uint64_t renderDirectoryScansPerformed = 0;
    std::uint64_t renderDirectoryScansDenied = 0;
  };

  struct SkinFileRenderTransitionResult {
    bool ok = false;
    std::optional<SkinFileFailure> failure;
  };

  class LuaSkinFileSystem {
  public:
    SkinFileResolveResult resolve(std::string_view virtualPath,
                                 SkinFileUse) const;
    SkinFileReadResult read(std::string_view virtualPath, SkinFileUse,
                            std::uint64_t maximumBytes) const;
    SkinFileListResult list(std::string_view virtualDirectory,
                            std::string_view luaPattern,
                            std::size_t maximumEntries) const;
    SkinFileWriteResult writeData(std::string_view virtualPath,
                                  std::span<const std::byte>,
                                  bool append);
    SkinFileRenderTransitionResult enterRenderPhase();
    SkinFileActivityCounters activityCounters() const noexcept;
  };

  struct LuaSkinFileSystemOptions {
    SkinRevisionReadView revision;
    SkinEntryId entry;
    SkinStorageRoots storageRoots;
    std::optional<SkinProfileId> profileId;
    bool allowDataWrites = false;
  };
  ```

- [ ] **Step 3: Write filesystem tests first for package ceiling versus entry-parent working directory, `?.lua` and `?/init.lua`, shared-parent modules inside one package, sibling-package denial, absolute/parent/symlink escape denial, regular-file only reads, binary Lua rejection, overlay-first data reads, resource/module bypass of overlay, atomic append/truncate/clear semantics, safe automatic parent creation for a nested write in a fresh overlay, line/list limits and Lua-pattern traces, quota exhaustion, and denial of every operation after `enterRenderPhase()`, including through a previously captured closure. Add the exact primitives needed by Task 9's closed legacy File object: bounded deterministic direct-child listing that returns virtual paths only, overlay-only single-directory `mkdir`, false/nil-compatible failure shapes, and denial through a captured legacy object after render phase. Prove the phase transition itself performs no overlay mutation and the before/after overlay digest is identical. Assert denied render attempts increment their exact read/write/scan `Denied` counters while all matching `Performed` counters remain zero.** Exercise two profiles and two entries and prove no overlay read/list/write crosses any of the four Task 5-derived roots. A missing profile with data writes enabled is construction failure; read-only catalog/default validation gets no overlay at all. Exercise both a prepared-package view whose owner encloses the entire synchronous validation call and a published lease view whose owning session outlives the filesystem; no view may be captured by an asynchronous job.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target lua_skin_file_system_tests -j 6
  ctest --test-dir cmake-build-debug -R '^lua_skin_file_system_tests$' --output-on-failure
  ```

  Expected RED: the virtual filesystem and overlay do not exist.
- [ ] **Step 5: Implement the Task 8 failure/result/filesystem interfaces from a `SkinRevisionReadView`**. For pre-publication validation the `PreparedPackage` owner encloses construction, Lua execution, and destruction synchronously. For a chart session, `PlaySkinSession` first retains the owning activation lease and constructs the filesystem from that lease's `readView()` plus roots/profile/entry; member/destruction order keeps the lease alive until after the filesystem closes. Assert `entry.package == revision.revision().package`, use the view root as the security ceiling, derive the working directory internally from only `entry.packageRelativePath`'s parent, and obtain data storage only from `deriveSkinPrivateOverlayRoot`. Return virtual normalized paths only; never disclose host paths.
- [ ] **Step 6: Recheck the current phase and normalized/opened target in every host call**. Use no-follow reads and `AtomicFile` for overlay replacement, create missing overlay parents component-by-component under the already validated root without following links, and never mutate the package. Implement both `file_mkdir` and the legacy File object's `mkdir` as overlay-only directory creation. The legacy object's `listFiles` uses the same bounded direct-child listing, returns normalized virtual path strings rather than host paths, and never consults the overlay for resource/module replacement. Count every render-phase read, write, and directory-scan attempt in its `Denied` field before returning `RenderPhase`; increment a `Performed` field only at the corresponding operation boundary, which the render-phase guard must make unreachable. The transition itself performs no filesystem I/O.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target lua_skin_file_system_tests -j 6
  ctest --test-dir cmake-build-debug -R '^lua_skin_file_system_tests$' --output-on-failure
  ```

  Expected GREEN: data persistence works within quota while Lua/modules/resources remain immutable and render-time I/O is impossible.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/beatoraja/SkinCompatibilityDiagnostics.h src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp src/skin/beatoraja/LuaSkinFileSystem.h src/skin/beatoraja/LuaSkinFileSystem.cpp src/skin/CMakeLists.txt tests/lua_skin_file_system_tests.cpp tests/fixtures/beatoraja_skin/lua/filesystem
  git commit -m "feat: sandbox gameplay skin file access"
  ```

### Task 9: Create quota-enforced, JIT-disabled, two-phase Lua states

**Reference refresh:** `LuaSkinLoader.loadHeader`, `LuaSkinLoader.load`, `SkinLuaAccessor` constructors, `createStandardGlobals`, `createSandboxGlobals`, `initializeModules`, `execFile`, `setDirectory`, `LegacySkinLuaApi.install`, `BindClassFunction`, `NewFunction`, `fileFacade`, `gdxFacade`, and default `play7.luaskin`'s require-before-branch behavior.

**Files:**

- Create: `src/skin/beatoraja/LuaSkinRuntime.h`
- Create: `src/skin/beatoraja/LuaSkinRuntime.cpp`
- Create: `src/skin/beatoraja/BeatorajaSkinConfiguration.h`
- Create: `src/skin/beatoraja/LuaSkinHostModules.h`
- Create: `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Create: `src/skin/LuaGameplaySkinFeature.h`
- Modify: `src/skin/beatoraja/SkinCompatibilityDiagnostics.h`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/lua_skin_runtime_tests.cpp`
- Test: `tests/lua_skin_feature_gate_tests.py`

**Interfaces:**

- Consumes: a move-owned `LuaSkinFileSystem`, `LuaRuntimePurpose`, the fixed budgets, and committed Lua-language traces.
- Produces: `LuaRuntimeCreateResult create(LuaSkinRuntimeOptions)`, `LuaValueResult loadHeader()`, `LuaValueResult loadConfigured(const BeatorajaSkinConfiguration &)`, `LuaOperationResult enterRenderPhase()/beginFrame(frameSerial)`, and `LuaCallbackResult invoke(LuaCallbackId, span<const LuaScalar>)`. `LuaCallbackId {slot, generation}` is runtime-owned and invalid after close.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("coroutine loops share the frame budget") {
  auto runtime = makeGameplayRuntime();
  CHECK_FALSE(runInfiniteWrappedCoroutine(*runtime).value.has_value());
  CHECK(runInfiniteWrappedCoroutine(*runtime).failure.has_value());
}
```

- [ ] **Step 2: Make the RED runtime test fixture reference this strict state machine and the purpose-specific budgets**:

  ```cpp
  enum class LuaRuntimePurpose : std::uint8_t { Catalog, Validation, Gameplay };
  enum class LuaRuntimePhase : std::uint8_t {
    Created,
    HeaderLoaded,
    Configured,
    Render
  };

  using LuaScalar = std::variant<std::nullptr_t, bool, std::int64_t,
                                 double, std::string>;

  struct ConfiguredOption {
    std::string name;
    int value = 0;
  };

  using OffsetPermissionMask = std::uint8_t;
  inline constexpr OffsetPermissionMask kOffsetPermissionX = 1U << 0U;
  inline constexpr OffsetPermissionMask kOffsetPermissionY = 1U << 1U;
  inline constexpr OffsetPermissionMask kOffsetPermissionW = 1U << 2U;
  inline constexpr OffsetPermissionMask kOffsetPermissionH = 1U << 3U;
  inline constexpr OffsetPermissionMask kOffsetPermissionR = 1U << 4U;
  inline constexpr OffsetPermissionMask kOffsetPermissionA = 1U << 5U;

  struct BeatorajaSkinConfiguration {
    std::vector<ConfiguredOption> orderedOptions;
    std::map<std::string, int> options;
    std::set<int> enabledOptionIds;
    std::map<std::string, std::string> filePaths;
    std::map<std::string, ConfigOffset> offsets;
    std::map<std::string, OffsetPermissionMask> offsetPermissions;
    std::map<int, ConfigOffset> offsetsById;
    std::string lowercaseSha256;
  };

  struct LuaCallbackId {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    auto operator<=>(const LuaCallbackId &) const = default;
  };

  class LuaValueHandle {
  public:
    LuaValueHandle(LuaValueHandle &&) noexcept;
    LuaValueHandle(const LuaValueHandle &) = delete;
    ~LuaValueHandle();
  };

  class LuaSkinRuntime;

  struct LuaRuntimeCreateResult {
    std::unique_ptr<LuaSkinRuntime> runtime;
    std::optional<SkinDiagnostic> failure;
  };

  struct LuaValueResult {
    std::optional<LuaValueHandle> value;
    std::optional<SkinDiagnostic> failure;
  };

  struct LuaOperationResult {
    bool ok = false;
    std::optional<SkinDiagnostic> failure;
  };

  struct LuaCallbackResult {
    std::optional<LuaScalar> value;
    std::optional<SkinDiagnostic> failure;
  };

  struct LuaSkinRuntimeOptions {
    LuaRuntimePurpose purpose;
    std::unique_ptr<LuaSkinFileSystem> fileSystem;
  };

  class LuaSkinRuntime final {
  public:
    static LuaRuntimeCreateResult create(LuaSkinRuntimeOptions);
    LuaValueResult loadHeader();
    LuaValueResult
    loadConfigured(const BeatorajaSkinConfiguration &configuration);
    LuaOperationResult enterRenderPhase();
    LuaOperationResult beginFrame(std::uint64_t visualStateSequence);
    LuaCallbackResult invoke(LuaCallbackId,
                             std::span<const LuaScalar> arguments);
    LuaRuntimePhase phase() const noexcept;
  };
  ```

- [ ] **Step 3: Make the RED tests reference canonical `BeatorajaSkinConfiguration`, runtime-owned callbacks/values, and create/value/operation/callback results**. The expected digest excludes `get_path`; the test host exposes empty `main_state`, `timer_util`, and `event_util` modules during header execution. Load Task 2's `legacy_lua_upstream_v1.json` only for selected upstream return/call-shape parity and `lua_sandbox_v1.json` as the AsoBMaShow authority for allowed/denied capabilities and phase rules; assert the policy's selected-surface digest equals Task 1a's manifest. Production and default tests must not inspect the external skin. Add `tests/lua_skin_feature_gate_tests.py` first to require `ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=ON` for desktop/iOS, `OFF` by default on Android, all Lua/model/renderer/integration sources behind that gate, and an always-available `skin::luaGameplaySkinsAvailable()` compile-time query. The test must inspect source guards as well as CMake: Xcode auto-discovers supported files under `src`, so enabled-only and unavailable-only translation units must compile to complementary empty/nonempty units and never define the same symbol together.

- [ ] **Step 4: Add runtime tests first for nil `skin_config` header execution, configured execution in the identical `lua_State`, persistent globals/closures/`package.loaded`, fresh catalog/validation/gameplay isolation, text-only chunks, exact visible standard-library subset, `bit32` trace parity, no `ffi`/`jit`/`debug`/network/process/native loaders, deterministic stack/table limits, allocator exhaustion, instruction and wall-time interruption for header/configured/callback loops, per-frame callback totals, host-call byte/time limits, and infinite loops created through both `coroutine.create` and `coroutine.wrap`. Replace, never expose, the standard filesystem functions: test bounded package-local text `dofile`; an `io` table containing only `open`; Task 1a-audited default/`r`/`w`/`a` modes; bounded `lines`; zero-or-more-argument chainable `write`; idempotent `close`; atomic overlay commit with safe automatic parents in a fresh overlay; missing/wrong-mode results; handle quota; and no host paths/standard streams/seek/temp members. Before render transition, leave both read and dirty write handles open; `enterRenderPhase` must invalidate all handles, release read buffers/quota, discard dirty buffers, return `skin_file_render_phase_denied`, and preserve the overlay digest. Captured functions/handles then fail, increment the exact read/write/scan `Denied` counters, and leave all matching `Performed` counters zero. Test the closed legacy table exhaustively from the Aso policy: it is installed as the global and the already-loaded module; repeated `require("luajava")` returns the same table; only `bindClass` and `new` exist; File/Gdx are identity-checked Lua-side tokens/tables, no host pointer or Java userdata escapes, `new` accepts only the File token, `listFiles` is bounded/virtual/load-phase-only, `mkdir` is overlay-only, `Gdx.app` is nil, and every other class/constructor/member plus `newInstance`, URL/HTTP, controllers/input, reflection, native access, and post-render file call fails with one deduplicated diagnostic.**
- [ ] **Step 5: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target lua_skin_runtime_tests -j 6
  ctest --test-dir cmake-build-debug -R '^lua_skin_runtime_tests$' --output-on-failure
  python3 -m unittest tests/lua_skin_feature_gate_tests.py -v
  ```

  Expected RED: unrestricted existing LuaJIT setup and unconditional sources cannot satisfy the sandbox/budget/platform-gate assertions.
- [ ] **Step 6: Implement the Task 9 configuration/callback/result/runtime interfaces, the exact closed legacy table in `LuaSkinHostModules`, and the explicit platform feature gate**. In `LuaGameplaySkinFeature.h`, define `ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS` only when the build did not: `0` on `__ANDROID__`, `1` elsewhere, then expose `skin::luaGameplaySkinsAvailable()`. CMake explicitly sets the same value. Default ON therefore covers Xcode's independently discovered desktop/iOS sources, while Android remains OFF because this repository does not package/link LuaJIT there. Put every enabled-only implementation behind `#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS`, give unavailable adapters the complementary `#if !ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS`, keep package/profile core compilable either way, and make Settings report unavailable without referring to Lua types. Do not add normal source files to the Xcode synchronized group's membership exceptions. On enabled platforms, create each state with `lua_newstate` plus a quota allocator. Disable JIT before user code, install hooks, and use `sol::state_view` only internally. Open controlled base/package/table/string/math plus `bit32`; wrapped coroutine create/wrap/resume installs the shared accounting/deadline/count hook on every child before user bytecode runs. Install the legacy table before any entry/module code, route its File object only through the move-owned `LuaSkinFileSystem`, return no host path, and model absent `Gdx.app` without a no-op audio success.
- [ ] **Step 7: Replace LuaJIT 5.1's `package.loaders` table with `LuaSkinFileSystem` loaders only and do not create a permissive `package.searchers` alias**. Prepopulate `package.loaded["luajava"]` with the exact host table so package files cannot shadow or replace its authority. Remove the standard `dofile`, `loadfile`, `io`, `string.dump`, `package.loadlib`, `package.cpath`, native loaders, and `os`; reinstall only the Task 2-traced text-only VFS `dofile` plus restricted `io.open`/handle wrappers described above. Replace both `load` and `loadstring` with text-only wrappers and reject the binary-chunk signature in entries, modules, `dofile`, and dynamic chunks. Do not implement Beatoraja's `newInstance`, URL/HTTP/reader, controller/input, `debug.getmetatable`, or any other unaudited `LegacySkinLuaApi` branch.
- [ ] **Step 8: Enforce `Created -> HeaderLoaded -> Configured -> Render`; `loadConfigured` must fail if called on a fresh/different state, and a catalog state must not expose event or overlay-write APIs. Before delegating phase transition to the filesystem, invalidate every runtime-owned Lua file handle; any live dirty handle discards its buffer, releases all handle quota, preserves the overlay digest, and makes `enterRenderPhase` fail with the frozen diagnostic so validation cannot start gameplay with ambiguous persistence.**
- [ ] **Step 9: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target lua_skin_runtime_tests -j 6
  ctest --test-dir cmake-build-debug -R '^lua_skin_runtime_tests$' --output-on-failure
  python3 -m unittest tests/lua_skin_feature_gate_tests.py -v
  ```

  Expected GREEN: trace-compatible Lua and the exact audited legacy table execute under budgets, all prohibited capabilities remain absent, and Android remains LuaJIT-free and compilable.
- [ ] **Step 10: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/LuaGameplaySkinFeature.h src/skin/beatoraja/BeatorajaSkinConfiguration.h src/skin/beatoraja/LuaSkinHostModules.h src/skin/beatoraja/LuaSkinHostModules.cpp src/skin/beatoraja/LuaSkinRuntime.h src/skin/beatoraja/LuaSkinRuntime.cpp src/skin/beatoraja/SkinCompatibilityDiagnostics.h src/skin/CMakeLists.txt tests/lua_skin_runtime_tests.cpp tests/lua_skin_feature_gate_tests.py
  git commit -m "feat: add bounded two-phase Lua skin runtime"
  ```

### Task 10: Decode headers and export exact `skin_config`

**Reference refresh:** `LuaSkinLoader.fromLuaValue`, `JSONSkinLoader.loadJsonSkinHeader`, `SkinHeader`, `SkinHeader.setSkinConfigProperty`, `SkinLuaAccessor.exportSkinProperty`, `SkinConfig`, and synthesized gameplay offsets from `SkinConfiguration.java`.

**Files:**

- Create: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Create: `src/skin/beatoraja/LuaSkinTableDecoder.h`
- Create: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Modify: `src/skin/beatoraja/BeatorajaSkinConfiguration.h`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.h`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.h`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/lua_skin_table_decoder_tests.cpp`
- Test: `tests/lua_skin_host_modules_tests.cpp`

**Interfaces:**

- Consumes: a runtime-owned `LuaValueHandle`, optional saved `EntryProfileSettings`, `LuaSkinFileSystem`, and Task 9 callback/configuration types.
- Produces: `HeaderDecodeResult LuaSkinTableDecoder::decodeHeader(const LuaValueHandle &)`, typed `BeatorajaSkinHeader`, and `ConfigurationReconcileResult reconcileSkinConfiguration(const BeatorajaSkinHeader &, const EntryProfileSettings *, LuaSkinFileSystem &)`, including reconciled settings, canonical configuration, and diagnostics.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("configured skin table matches Beatoraja shape") {
  const auto table = decodeConfiguredTableFixture();
  CHECK(table.enabledOptions.front() == table.options.at("Lane type"));
  CHECK(table.offsets.at("Notes offset").permissions == kOffsetPermissionH);
}
```

- [ ] **Step 2: Make the RED decoder tests reference these typed header and reconciliation records; `LuaCallbackId` remains runtime-owned**:

  ```cpp
  struct SkinHeaderCategory {
    std::string name;
    std::vector<std::string> items;
  };

  struct SkinHeaderOptionChoice {
    std::string label;
    int value = 0;
  };

  struct SkinHeaderOption {
    std::string category;
    std::string name;
    std::vector<SkinHeaderOptionChoice> choices;
    std::string defaultLabel;
  };

  struct SkinHeaderFile {
    std::string category;
    std::string name;
    std::string pattern;
    std::string defaultValue;
  };

  struct SkinHeaderOffset {
    std::string category;
    std::string name;
    int id = 0;
    OffsetPermissionMask permissions = 0;
  };

  struct BeatorajaSkinHeader {
    int type = -1;
    int width = 1280;
    int height = 720;
    std::string name;
    std::string author;
    std::vector<SkinHeaderCategory> categories;
    std::vector<SkinHeaderOption> options;
    std::vector<SkinHeaderFile> files;
    std::vector<SkinHeaderOffset> offsets;
  };

  struct HeaderDecodeResult {
    std::optional<BeatorajaSkinHeader> header;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct ConfigurationReconcileResult {
    std::optional<BeatorajaSkinConfiguration> configuration;
    EntryProfileSettings reconciledSettings;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct BeatorajaSkinModelDecodeResult; // completed by Task 11
  class LuaSkinTableDecoder {
  public:
    HeaderDecodeResult decodeHeader(const LuaValueHandle &);
    BeatorajaSkinModelDecodeResult decodeGameplay(
        const LuaValueHandle &);
  };
  ConfigurationReconcileResult reconcileSkinConfiguration(
      const BeatorajaSkinHeader &, const EntryProfileSettings *,
      LuaSkinFileSystem &);
  ```
- [ ] **Step 3: Write decoder/host tests first for authored array order, Lua numeric indexing, holes/mixed keys, unknown fields, defaults, integer/float/string coercions used by the traces, depth/entry/object limits, malformed headers, callback lifetime, and exact configured table shape**:

  ```text
  skin_config.file_path[name]
  skin_config.get_path(relative_path)
  skin_config.option[name]
  skin_config.enabled_options[1..n]
  skin_config.offset[name] = {x, y, w, h, r, a}
  ```

- [ ] **Step 4: Assert the configured table contains declared offsets plus Beatoraja's four 7-key synthesized controls with exact upstream permissions: `All offset(%)` allows x/y/w/h; `Notes offset` allows h only; `Judge offset` and `Judge Detail offset` allow x/y/w/h/a**. Disallowed components remain zero and are not persisted.
- [ ] **Step 5: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target lua_skin_table_decoder_tests lua_skin_host_modules_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(lua_skin_table_decoder_tests|lua_skin_host_modules_tests)$' --output-on-failure
  ```

  Expected RED: no typed header/config export exists.
- [ ] **Step 6: Implement the Task 10 header/reconciliation interfaces and decode the nil-configuration return value**. Reconcile saved settings by declared key/value, preserve matching values, and reset removed/invalid values to defaults. Populate `enabledOptionIds` from the selected declared choice values and `offsetsById` from the declared/synthesized offset IDs after permission sanitization; reject ambiguous duplicate IDs instead of allowing map overwrite. Compute the lowercase SHA-256 over the specified versioned length-framed option/file/offset stream; exclude viewport, host paths, and `get_path`.
- [ ] **Step 7: Make `get_path` return a virtual normalized path resolved within the immutable revision; do not expose its host path**. Keep catalog header execution read-only and destroy that state after conversion.
- [ ] **Step 8: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target lua_skin_table_decoder_tests lua_skin_host_modules_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(lua_skin_table_decoder_tests|lua_skin_host_modules_tests)$' --output-on-failure
  ```

  Expected GREEN: default and saved configurations exactly match reference traces, including same-state configured execution.
- [ ] **Step 9: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/beatoraja/BeatorajaSkinConfiguration.h src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/LuaSkinHostModules.h src/skin/beatoraja/LuaSkinHostModules.cpp src/skin/beatoraja/LuaSkinRuntime.h src/skin/beatoraja/LuaSkinRuntime.cpp src/skin/CMakeLists.txt tests/lua_skin_table_decoder_tests.cpp tests/lua_skin_host_modules_tests.cpp
  git commit -m "feat: decode Beatoraja skin headers"
  ```

## Slice 3 — Portable Model and Renderer

### Task 11: Decode and validate the complete v1 gameplay model

**Reference refresh:** `JsonSkin.java`, `JSONSkinLoader.loadJsonSkin`, `JsonSkinObjectLoader`, `JsonPlaySkinObjectLoader`, `PlaySkin.java`, `SkinNote.java`, `SkinGauge.java`, `SkinJudge.java`, and `SkinBGA.java`.

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Create: `src/skin/beatoraja/SkinModelValidator.h`
- Create: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/beatoraja_skin_model_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/lua/model/all_v1_objects.luaskin`
- Create: `tests/fixtures/beatoraja_skin/model/all_v1_objects.expected.json`

**Interfaces:**

- Consumes: the configured-phase `LuaValueHandle`, header/configuration, runtime-owned callbacks, and compatibility diagnostics.
- Produces: `BeatorajaSkinModelDecodeResult LuaSkinTableDecoder::decodeGameplay(const LuaValueHandle &)`, followed by `SkinModelValidationResult SkinModelValidator::validate(BeatorajaSkinModel)`. The validated result contains the model plus stable IDs for every resource/object and the exact list of disabled optional objects; critical failure returns no model.

  ```cpp
  using SkinObjectId = std::uint32_t;
  using SkinResourceId = std::uint32_t;

  template <typename Tag> struct SkinBindingId {
    std::uint32_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const SkinBindingId &) const = default;
  };
  struct SkinBooleanPropertyTag;
  struct SkinIntegerPropertyTag;
  struct SkinFloatPropertyTag;
  struct SkinStringPropertyTag;
  struct SkinTimerPropertyTag;
  struct SkinStringWriterTag;
  struct SkinEventBindingTag;
  using SkinBooleanPropertyId = SkinBindingId<SkinBooleanPropertyTag>;
  using SkinIntegerPropertyId = SkinBindingId<SkinIntegerPropertyTag>;
  using SkinFloatPropertyId = SkinBindingId<SkinFloatPropertyTag>;
  using SkinStringPropertyId = SkinBindingId<SkinStringPropertyTag>;
  using SkinTimerPropertyId = SkinBindingId<SkinTimerPropertyTag>;
  using SkinStringWriterId = SkinBindingId<SkinStringWriterTag>;
  using SkinEventBindingId = SkinBindingId<SkinEventBindingTag>;

  // LuaSkinLoader resolves a numeric ID or a recognized string name through
  // the built-in factory. A Lua function, or an unrecognized string script,
  // remains a runtime-owned callback. Never collapse the latter to ID 0.
  struct SkinBuiltinPropertySelector {
    std::variant<int, std::string> value;
  };
  template <typename Id> struct SkinRuntimeBinding {
    Id id{};
    std::variant<SkinBuiltinPropertySelector, LuaCallbackId> source;
    std::uint32_t authoredOrdinal = 0;
  };
  using SkinBooleanPropertyBinding = SkinRuntimeBinding<SkinBooleanPropertyId>;
  using SkinIntegerPropertyBinding = SkinRuntimeBinding<SkinIntegerPropertyId>;
  using SkinFloatPropertyBinding = SkinRuntimeBinding<SkinFloatPropertyId>;
  using SkinStringPropertyBinding = SkinRuntimeBinding<SkinStringPropertyId>;
  using SkinTimerPropertyBinding = SkinRuntimeBinding<SkinTimerPropertyId>;
  using SkinFloatWriterBinding = SkinRuntimeBinding<SkinFloatWriterId>;
  using SkinStringWriterBinding = SkinRuntimeBinding<SkinStringWriterId>;
  using SkinEventBinding = SkinRuntimeBinding<SkinEventBindingId>;

  // SkinBlendMode, SkinFilterMode, and SkinStretchMode come from Task 3's
  // unconditional SkinPresentationTypes.h.
  enum class SkinValueKind : std::uint8_t { Integer, Float, String };

  struct SkinSourceRect { int x = 0; int y = 0; int w = 0; int h = 0; };
  struct SkinImageResource {
    SkinResourceId id = 0;
    std::string virtualPath;
    int divisionX = 1;
    int divisionY = 1;
    std::uint32_t authoredOrdinal = 0;
  };
  struct SkinFontFallbackResource {
    std::string virtualPath;
    int type = 0;
  };
  struct SkinFontResource {
    SkinResourceId id = 0;
    std::string virtualPath;
    int type = 0;
    std::vector<SkinFontFallbackResource> fallbacks;
    std::uint32_t authoredOrdinal = 0;
  };
  using SkinResourceDefinition =
      std::variant<SkinImageResource, SkinFontResource>;

  struct SkinSpriteFrames {
    SkinResourceId resource = 0;
    std::vector<SkinSourceRect> frames;
    int cycleMillis = 0;
    std::optional<SkinTimerPropertyId> timer;
  };
  struct SkinImageObject {
    // A plain Image has one state. Image.len partitions its row-major regions
    // into ordered states selected by ref. ImageSet preserves one complete
    // independently timed/cropped/resource-backed state per images[] entry.
    std::vector<SkinSpriteFrames> orderedStates;
    std::optional<SkinIntegerPropertyId> stateIndex;
    std::optional<SkinEventBindingId> clickEvent;
    int clickMode = 0;
  };
  enum class SkinZeroPaddingMode : std::uint8_t {
    None = 0, Zero = 1, AlternateZero = 2
  };
  struct SkinDigitOffset {
    double x = 0.0; double y = 0.0;
    double width = 0.0; double height = 0.0;
  };
  struct SkinDigitSpriteSet {
    SkinSpriteFrames positive;
    std::optional<SkinSpriteFrames> negative;
    int glyphsPerAnimationFrame = 0;
  };
  struct SkinNumberObject {
    SkinDigitSpriteSet digits;
    SkinIntegerPropertyId value{};
    int digitCount = 0;
    int spacing = 0;
    int alignment = 0;
    SkinZeroPaddingMode zeroPadding = SkinZeroPaddingMode::None;
    std::vector<SkinDigitOffset> perDigitOffsets;
  };
  struct SkinFloatObject {
    SkinDigitSpriteSet digits;
    SkinFloatPropertyId value{};
    int integerDigits = 0;
    int fractionalDigits = 0;
    int spacing = 0;
    int alignment = 0;
    SkinZeroPaddingMode zeroPadding = SkinZeroPaddingMode::None;
    bool signVisible = false;
    double gain = 1.0;
    std::vector<SkinDigitOffset> perDigitOffsets;
  };
  struct SkinTextObject {
    SkinResourceId font = 0;
    std::optional<SkinStringPropertyId> value;
    std::optional<SkinStringWriterId> writer;
    std::string literal;
    int pointSize = 0;
    int alignment = 0;
    bool wrapping = false;
    int overflow = 0;
    std::array<std::uint8_t, 4> outlineRgba{255, 255, 255, 0};
    double outlineWidth = 0.0;
    std::array<std::uint8_t, 4> shadowRgba{255, 255, 255, 0};
    double shadowOffsetX = 0.0;
    double shadowOffsetY = 0.0;
    double shadowSmoothness = 0.0;
    bool editable = false;
  };
  struct SkinSliderObject {
    SkinSpriteFrames knob;
    struct IntegerRangeSource {
      SkinIntegerPropertyId value{};
      int minimum = 0;
      int maximum = 0;
    };
    std::variant<SkinFloatPropertyId, IntegerRangeSource> value;
    std::optional<SkinFloatWriterId> writer;
    std::uint8_t direction = 0; // 0 up, 1 right, 2 down, 3 left
    double range = 0.0;
    bool changeable = true;
  };
  struct SkinGraphObject {
    SkinSpriteFrames fill;
    std::variant<SkinFloatPropertyId,
                 SkinSliderObject::IntegerRangeSource> value;
    int direction = 0; // pinned SkinGraph: 1 grows down, all others right
  };
  enum class SkinGaugeAnimationType : std::uint8_t {
    Random = 0, Increase = 1, Decrease = 2, Flicker = 3
  };
  struct SkinGaugeObject {
    // Pinned node ordering is six roles per gauge family; retain it exactly.
    std::vector<SkinSpriteFrames> orderedNodes;
    int parts = 50;
    SkinGaugeAnimationType animation = SkinGaugeAnimationType::Random;
    int animationRange = 3;
    int animationCycleMillis = 33;
    int resultStartMillis = 0;
    int resultEndMillis = 500;
  };
  struct SkinAuthoredRect {
    double x = 0.0; double y = 0.0;
    double width = 0.0; double height = 0.0;
  };
  struct SkinDestinationFrame {
    int timeMillis = 0;
    double x = 0.0; double y = 0.0;
    double width = 0.0; double height = 0.0;
    double angleDegrees = 0.0;
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
    int acceleration = 0;
    std::optional<SkinSourceRect> clip;
  };
  struct SkinDestinationBody {
    std::optional<SkinTimerPropertyId> timer;
    int loop = -1;
    std::vector<std::variant<int, SkinBooleanPropertyId>> conditions;
    std::vector<int> offsetIds;
    std::optional<SkinBooleanPropertyId> drawCondition;
    int center = 0;
    std::optional<SkinAuthoredRect> mouseRect;
    SkinBlendMode blend = SkinBlendMode::Normal;
    SkinFilterMode filter = SkinFilterMode::Nearest;
    SkinStretchMode stretch = SkinStretchMode::Stretch;
    std::vector<SkinDestinationFrame> frames;
    std::uint32_t authoredOrdinal = 0;
  };
  enum class SkinNoteVisualKind : std::uint8_t {
    Normal, Mine, Hidden, Processed,
    LnEnd, LnStart, LnBodyActive, LnBodyInactive,
    HcnEnd, HcnStart, HcnBodyActive, HcnBodyInactive,
    HcnDamage, HcnReactive
  };
  struct SkinLaneNotePresentation {
    int authoredLane = -1;
    SkinAuthoredRect laneDestination;
    double noteHeight = 0.0;
    std::optional<double> secondaryDestinationY;
    std::map<SkinNoteVisualKind, SkinSpriteFrames> visuals;
  };
  enum class SkinNoteLineKind : std::uint8_t {
    Group, Bpm, Stop, Time
  };
  struct SkinNoteLinePresentation {
    SkinNoteLineKind kind = SkinNoteLineKind::Group;
    SkinSpriteFrames sprite;
    SkinAuthoredRect laneGroupDestination;
    SkinDestinationBody destination;
  };
  struct SkinNoteObject {
    std::vector<SkinLaneNotePresentation> lanes;
    std::vector<SkinNoteLinePresentation> lines;
    std::array<int, 2> expansionRatePercent{100, 100};
  };
  enum class SkinCoverKind : std::uint8_t { Hidden, Lift };
  struct SkinCoverObject {
    SkinCoverKind kind = SkinCoverKind::Hidden;
    SkinSpriteFrames sprite;
    double disappearLine = -1.0;
    bool disappearLineLinksLift = true;
  };
  struct SkinNestedObjectPresentation {
    SkinObjectId object = 0;
    SkinDestinationBody destination;
  };
  struct SkinJudgeGradePresentation {
    std::optional<SkinNestedObjectPresentation> image;
    std::optional<SkinNestedObjectPresentation> detailNumber;
  };
  struct SkinJudgeObject {
    std::vector<SkinJudgeGradePresentation> grades;
    int player = 0;
    bool shiftImageByHalfDetailWidth = false;
  };
  // One authored SkinBGA compositor marker. Base/layer/miss roles belong only
  // to Task 19's dynamically expanded prepared targets.
  struct SkinBgaObject {};

  using SkinObjectPayload = std::variant<
      SkinImageObject, SkinNumberObject, SkinFloatObject, SkinTextObject,
      SkinSliderObject, SkinGraphObject, SkinGaugeObject, SkinNoteObject,
      SkinCoverObject, SkinJudgeObject, SkinBgaObject>;

  struct SkinObjectDefinition {
    SkinObjectId id = 0;
    std::string authoredName;
    SkinObjectPayload payload;
    std::uint32_t authoredOrdinal = 0;
    bool critical = false;
  };

  struct SkinDestination {
    SkinObjectId object = 0;
    SkinDestinationBody presentation;
  };
  struct SkinCustomTimer {
    int id = 0;
    SkinTimerPropertyId timer{};
  };
  struct SkinCustomEvent {
    int id = 0;
    SkinEventBindingId action{};
    std::optional<SkinBooleanPropertyId> condition;
    int minimumIntervalMillis = 0;
  };
  struct BeatorajaSkinModel {
    BeatorajaSkinHeader header;
    std::vector<SkinBooleanPropertyBinding> booleanProperties;
    std::vector<SkinIntegerPropertyBinding> integerProperties;
    std::vector<SkinFloatPropertyBinding> floatProperties;
    std::vector<SkinStringPropertyBinding> stringProperties;
    std::vector<SkinTimerPropertyBinding> timerProperties;
    std::vector<SkinFloatWriterBinding> floatWriters;
    std::vector<SkinStringWriterBinding> stringWriters;
    std::vector<SkinEventBinding> events;
    std::vector<SkinResourceDefinition> resources;
    std::vector<SkinObjectDefinition> objects;
    std::vector<SkinDestination> destinations;
    std::vector<SkinCustomTimer> customTimers;
    std::vector<SkinCustomEvent> customEvents;
  };
  struct BeatorajaSkinModelDecodeResult {
    std::optional<BeatorajaSkinModel> model;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct ValidatedBeatorajaSkinModel {
    BeatorajaSkinModel model;
    std::map<std::string, SkinResourceId, std::less<>> resourceIds;
    std::map<std::string, SkinObjectId, std::less<>> objectIds;
    std::vector<SkinObjectId> disabledOptionalObjects;
  };

  struct SkinModelValidationResult {
    std::optional<ValidatedBeatorajaSkinModel> model;
    std::vector<SkinDiagnostic> diagnostics;
    bool criticalFailure = false;
  };
  class SkinModelValidator {
  public:
    SkinModelValidationResult validate(BeatorajaSkinModel);
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("critical note dependency rejects the model") {
  const auto result = validateModelFixture("missing-critical-note");
  CHECK(result.criticalFailure);
  CHECK_FALSE(result.model.has_value());
}
```

- [ ] **Step 2: Make the RED model fixture reference source-neutral variants for every v1 target surface**. Require stable binding/resource/object IDs and authored ordinals; exact custom-timer/event ID, typed action/timer/condition binding, interval, and authored declaration order; and explicit unsupported-field diagnostics. Exercise every Boolean/Integer/Float/String/Timer/FloatWriter/StringWriter/Event input spelling accepted by pinned `LuaSkinLoader`: numeric built-in ID, recognized built-in name where that factory permits it, Lua function, and Lua script string. A script string must remain a runtime callback rather than silently becoming built-in ID zero.
- [ ] **Step 3: Write model tests first for defaults, sprite divisions/regions, negative dimensions, destination references, duplicate IDs, missing sources/images/fonts, plain single-state images, `Image.len > 1` row-major state partitioning selected by `ref`, and ordered `ImageSet.images[]` choices whose resource/crop/timer/cycle may each differ; note-array lane counts and authored lane order; all normal/mine/hidden/processed and LN/CN/HCN visual phases; per-lane `dst`/`size`/`dst2`; expansion rate; group/BPM/stop/time line presentations; hidden/lift cover timer/cycle/disappear-line/link-lift semantics; gauge node/parts/range; judge indices; BGA identity; destination numeric-sign versus Boolean-binding conditions; draw bindings; center/pivot; mouse rectangle; timer/event/property callback variants; slider directions 0–3, `changeable`, custom float writers, numeric rate properties, and `isRefNum` integer min/max mapping; regular graph numeric/custom/`isRefNum` rate sources and direction; authored ordinals; conversion limits; critical/optional dependency propagation; and malformed critical note paths. Add nonempty synthetic custom timer/event vectors and assert deterministic authored order plus one compatibility-divergence diagnostic; Task 2's seeded `IntMap` collision/resize cases remain upstream evidence only. Assert Task 1a's selected SCURO map counts are both zero and the decoded external acceptance model must match or fail closed.** Preserve `Image.act/click` and `Text.event/editable` as typed model fields so the validator can issue exact diagnostics; v1 deliberately rejects a critical use or disables an optional use because Task 20 implements only audited gameplay float sliders/lane cover. The SCURO manifest must prove those direct click/text-input fields absent or Task 25 opens a gap-remediation plan. Decode the pinned Beatoraja `StretchType` IDs 0 through 10 to the exact enum values above and reject any other value with the audited critical/optional disposition; do not collapse trimmed and non-trimmed modes.
- [ ] **Step 3a: Add lossless HUD model regressions from the pinned loader**. Cover positive/negative number and float sprite-set partitioning, three-state zero padding, per-digit x/y/w/h offsets, float gain/sign visibility, sparse judge grades with independently optional image/detail nested full destinations and half-detail-width shift, and gauge ordered node roles/parts/animation type/range/cycle/result interval. Task 14 reads current gauge value/type/min/max/border only through `SkinGaugeStateView`; the model must not fake gauge state as a generic float property.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target beatoraja_skin_model_tests -j 6
  ctest --test-dir cmake-build-debug -R '^beatoraja_skin_model_tests$' --output-on-failure
  ```

  Expected RED: only header records exist.
- [ ] **Step 5: Implement every Task 11 source-neutral model/result type and extend `LuaSkinTableDecoder`**. Intern every typed binding once in its model registry, retain Lua functions/scripts by runtime-owned `LuaCallbackId`, dispatch traced numeric/name/function/script forms exactly like pinned `serializeLuaScript`, preserve authored order, and attach available Lua file/line information. Preserve custom timer/event declaration vectors exactly and attach one deduplicated `custom_object_order_authored_divergence` diagnostic when either is nonempty; do not claim or simulate pinned `IntMap` RNG state. Decode note line destinations and covers into their typed nested presentations; synthesize Beatoraja's cover offset lists (`Lift+Hidden Cover` for hidden and `Lift` for lift) without conflating either with the separate interactive lane-cover slider.
- [ ] **Step 6: Implement `SkinModelValidator` as a separate pass**. Reject the entry when canvas/type, note presentation, required lane geometry, binding kind/reference, slider direction, cover, or another critical dependency is invalid; disable only optional objects with one deduplicated diagnostic. Require exactly the audited v1 surface and report every unimplemented object/property/timer/writer/event identifier. Validate each typed binding against the corresponding built-in host registry or live runtime callback; never reinterpret a missing or wrong-kind binding as ID zero.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target beatoraja_skin_model_tests -j 6
  ctest --test-dir cmake-build-debug -R '^beatoraja_skin_model_tests$' --output-on-failure
  aso_root="$(git rev-parse --show-toplevel)"
  beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
  python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean
  : "${SCURO_ARCHIVE_PATH:?set external SCURO archive path}"
  : "${SCURO_ARCHIVE_PACKAGE_PREFIX:?set . or the inferred wrapper}"
  : "${SCURO_ARCHIVE_SHA256:?set pinned SCURO archive digest}"
  : "${SCURO_SKIN_ROOT:?set corresponding extracted package root}"
  python3 scripts/audit_beatoraja_skin.py --beatoraja-root "$beatoraja_ref_root" --archive-path "$SCURO_ARCHIVE_PATH" --archive-package-prefix "$SCURO_ARCHIVE_PACKAGE_PREFIX" --skin-root "$SCURO_SKIN_ROOT" --expected-archive-sha256 "$SCURO_ARCHIVE_SHA256" --verify tests/fixtures/beatoraja_skin/reference_manifest.json
  ```

  Expected GREEN: the synthetic complete fixture decodes exactly, package/tree digests match, and the SCURO manifest has a disposition for every dependency.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinModelValidator.h src/skin/beatoraja/SkinModelValidator.cpp src/skin/CMakeLists.txt tests/beatoraja_skin_model_tests.cpp tests/fixtures/beatoraja_skin/lua/model tests/fixtures/beatoraja_skin/model
  git commit -m "feat: decode Beatoraja gameplay skin models"
  ```

### Task 12: Implement viewport and destination evaluation from reference traces

**Reference refresh:** `SkinObject.prepare`, `SkinObject.draw`, destination interpolation helpers in `SkinObject.java`, `Skin.getOffsetAll`, `SkinOffset`, `SkinProperty` offset constants, and the target's destination tables.

**Files:**

- Create: `src/skin/beatoraja/PlaySkinViewport.h`
- Create: `src/skin/beatoraja/PlaySkinViewport.cpp`
- Create: `src/skin/beatoraja/SkinDestinationEvaluator.h`
- Create: `src/skin/beatoraja/SkinDestinationEvaluator.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/play_skin_viewport_tests.cpp`
- Test: `tests/skin_destination_evaluator_tests.cpp`

**Interfaces:**

- Consumes: `ViewportSettings`, authored canvas size, top-left UI-logical safe rect, destinations, already-resolved timer/conditions, and ordered authored `ConfigOffset` values.
- Produces: `PlaySkinViewport evaluatePlaySkinViewport(AuthoredSize, UiLogicalRect, const ViewportSettings &)`, `SkinDestinationEvaluationResult evaluateSkinDestinationAuthored(const SkinDestinationBody &, const SkinDestinationEvaluationInputs &)`, and `UiDestinationGeometry projectSkinDestinationToUi(const AuthoredDestinationGeometry &, const SkinSourceRegionGeometry &, const PlaySkinViewport &)`. Top-level objects and nested group/BPM/stop/time line presentations use the same complete destination body. `INT64_MIN` is the exact timer-OFF sentinel.

  ```cpp
  struct PlaySkinViewport {
    Affine2D authoredToUi;
    Affine2D uiToAuthored;
    AuthoredRect drawableAuthoredBounds;
    UiLogicalRect safeUiBounds;
    bool valid = false;
  };

  struct AuthoredSize { double width = 0.0; double height = 0.0; };
  struct AuthoredPoint { double x = 0.0; double y = 0.0; };
  using AuthoredRect = SkinAuthoredRect;
  struct UiLogicalRect {
    double x = 0.0; double y = 0.0;
    double width = 0.0; double height = 0.0;
  };
  struct Affine2D {
    double m00 = 1.0; double m01 = 0.0; double tx = 0.0;
    double m10 = 0.0; double m11 = 1.0; double ty = 0.0;
  };

  struct SkinDestinationEvaluationInputs {
    std::int64_t nowMicros = 0;
    std::int64_t timerStartMicros = INT64_MIN;
    std::span<const bool> optionConditions;
    std::span<const ConfigOffset> orderedOffsets;
  };

  struct AuthoredDestinationGeometry {
    AuthoredRect rect;
    std::optional<AuthoredRect> clip;
    double centerX = 0.0;
    double centerY = 0.0;
    double angleDegrees = 0.0;
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
    SkinBlendMode blend = SkinBlendMode::Normal;
    SkinFilterMode filter = SkinFilterMode::Nearest;
    SkinStretchMode stretch = SkinStretchMode::Stretch;
  };

  struct SkinSourceRegionGeometry {
    int textureWidth = 0;
    int textureHeight = 0;
    SkinSourceRect region;
  };

  struct SkinDestinationEvaluationResult {
    std::optional<AuthoredDestinationGeometry> geometry;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct UiDestinationGeometry {
    std::array<std::array<double, 2>, 4> vertices{};
    std::array<std::array<double, 2>, 4> normalizedUvs{};
    std::optional<UiLogicalRect> clip;
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
    SkinBlendMode blend = SkinBlendMode::Normal;
    SkinFilterMode filter = SkinFilterMode::Nearest;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("authored offsets precede the viewport") {
  const auto ui = evaluateOffsetThenViewportFixture(10.0, 2.0);
  CHECK(ui.xDelta == Catch::Approx(20.0));
}
```

- [ ] **Step 2: Make the RED viewport tests reference the authored/UI rectangles, affine matrix, destination inputs, source texture/region geometry, and `PlaySkinViewport` contract above**. The exact projection signature is `projectSkinDestinationToUi(const AuthoredDestinationGeometry &, const SkinSourceRegionGeometry &, const PlaySkinViewport &)`. Require UI-logical safe bounds derived through `rendering::screenToUi`; the viewport signature must not accept Beatoraja offsets.
- [ ] **Step 3: Write viewport tests first for 16:9 skin on landscape 4:3 iPad Fit bars, Stretch fill, Custom-over-Fit, Custom-over-Stretch, safe-area origin, 1x and 2x-HiDPI inputs yielding identical UI-logical geometry, positive/clamped scales, bounded translation, invalid persisted reset to Fit, inverse round trips, `rendering::normalizedToUi` touch conversion, bottom-left authored coordinates to top-left UI coordinates, rotation handedness, clips, UI-to-drawable scissor conversion, and noninvertible interaction denial.**
- [ ] **Step 4: Write destination tests first from `destination_v1.json`: timer OFF suppression, time relative to timer start, pre-first-frame behavior, `loop=-1` stop-at-end, loop-point modulo, linear/ease-in/ease-out/step acceleration, missing animation components carrying forward, integer/color/alpha interpolation, pivot/center/angle, clip interpolation, filter/blend, option/draw conditions, one offset and offset-array composition, and Beatoraja's non-relative x/y adjustment.** Add source-region cases for every pinned `StretchType` ID 0–10, including the exact integer truncation points in trimmed UV cropping, centering behavior, no-expansion, source region inside a larger texture, invalid/zero source dimensions, and rotation after stretch adjustment. Assert both adjusted destination vertices and normalized UVs; unsupported IDs never reach this function.
- [ ] **Step 5: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target play_skin_viewport_tests skin_destination_evaluator_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(play_skin_viewport_tests|skin_destination_evaluator_tests)$' --output-on-failure
  ```

  Expected RED: geometry and trace comparisons fail.
- [ ] **Step 6: Implement the Task 12 viewport/destination interfaces as pure functions only**. Port the pinned `StretchType.stretchRect` behavior exactly: adjust the authored destination rectangle and copied source region first, derive normalized UVs from the full texture dimensions, then apply rotation and viewport projection. Use no Lua, filesystem, bgfx, or mutable gameplay pointers. Compose authored offsets before stretch/viewport, use double-precision intermediates except at the traced Java integer crop points, and apply traced rounding/clamping points.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target play_skin_viewport_tests skin_destination_evaluator_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(play_skin_viewport_tests|skin_destination_evaluator_tests)$' --output-on-failure
  ```

  Expected GREEN: every numeric trace and inverse-transform assertion matches.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/beatoraja/PlaySkinViewport.h src/skin/beatoraja/PlaySkinViewport.cpp src/skin/beatoraja/SkinDestinationEvaluator.h src/skin/beatoraja/SkinDestinationEvaluator.cpp src/skin/CMakeLists.txt tests/play_skin_viewport_tests.cpp tests/skin_destination_evaluator_tests.cpp
  git commit -m "feat: evaluate Beatoraja skin destinations"
  ```

### Task 13: Prepare bounded image, sprite, and font resources before rendering

**Reference refresh:** `JSONSkinLoader` source/file resolution, `SkinImage`, `SkinNumber`, `SkinFloat`, `SkinText`, `SkinSourceImage`, `SkinHeader.CustomFile`, and the target's file globs/font definitions.

**Files:**

- Create: `src/skin/beatoraja/SkinResourceCatalog.h`
- Create: `src/skin/beatoraja/SkinResourceCatalog.cpp`
- Create: `src/skin/beatoraja/SkinTextAtlas.h`
- Create: `src/skin/beatoraja/SkinTextAtlas.cpp`
- Create: `src/view/ImageFileDecoder.h`
- Create: `src/view/ImageFileDecoder.cpp`
- Modify: `src/view/ImageView.cpp`
- Modify: `src/view/CMakeLists.txt`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/skin_resource_catalog_tests.cpp`
- Modify: `tests/image_decode_coordinator_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/resources/fixture.png`
- Create: `tests/fixtures/beatoraja_skin/resources/fixture.ttf`
- Create: `tests/fixtures/beatoraja_skin/resources/README.md`

**Interfaces:**

- Consumes: `SkinRevisionLease`, `ValidatedBeatorajaSkinModel`, configuration, one app-owned `SkinResourcePreparationService`, and `std::stop_token`.
- Produces: worker-safe `SkinResourcePlanResult SkinResourcePreparationService::decodeAndPlan(SkinResourcePreparationInputs)`, render-thread `SkinResourceUploadResult SkinResourceCatalog::upload(SkinResourceUploadPlan &&)`, and immutable lookup by `SkinResourceId`. The catalog retains the lease; commands never retain paths, decoded buffers, or Lua values.

  ```cpp
  struct SkinDecodedImage {
    SkinResourceId id = 0;
    image_decode::DecodedImageData pixels;
    std::vector<SkinSourceRect> regions;
  };
  using SkinTextAtlasId = std::uint32_t;
  struct SkinTextAtlasKey {
    SkinResourceId font = 0;
    int pointSize = 0;
    std::array<std::uint8_t, 4> outlineRgba{255, 255, 255, 0};
    double outlineWidth = 0.0;
    std::array<std::uint8_t, 4> shadowRgba{255, 255, 255, 0};
    double shadowOffsetX = 0.0;
    double shadowOffsetY = 0.0;
    double shadowSmoothness = 0.0;
    std::string fallbackChainDigest;
    auto operator<=>(const SkinTextAtlasKey &) const = default;
  };
  struct SkinPreparedGlyphMetrics {
    SkinSourceRect region;
    int bearingX = 0;
    int bearingY = 0;
    int advance = 0;
  };
  struct SkinPreparedGlyphAtlas {
    SkinTextAtlasId id = 0;
    SkinTextAtlasKey key;
    image_decode::DecodedImageData pixels;
    std::map<char32_t, SkinPreparedGlyphMetrics> glyphs;
    std::map<std::pair<char32_t, char32_t>, int> kerning;
    int ascent = 0;
    int descent = 0;
    int lineHeight = 0;
  };
  struct SkinResourceUploadPlan {
    SkinRevisionLease revision;
    std::vector<SkinDecodedImage> images;
    std::vector<SkinPreparedGlyphAtlas> atlases;
    std::size_t decodedBytes = 0;
  };
  struct SkinResourceValidationInputs {
    SkinRevisionReadView revision;
    const ValidatedBeatorajaSkinModel &model;
    const BeatorajaSkinConfiguration &configuration;
    std::span<const std::string> requiredRuntimeStrings;
    std::stop_token stop;
  };
  struct SkinResourceValidationResult {
    bool valid = false;
    bool cancelled = false;
    std::vector<SkinDiagnostic> diagnostics;
  };
  struct SkinResourcePreparationInputs {
    SkinRevisionLease revision;
    const ValidatedBeatorajaSkinModel &model;
    const BeatorajaSkinConfiguration &configuration;
    std::span<const std::string> requiredRuntimeStrings;
    std::stop_token stop;
  };
  struct SkinResourcePlanResult {
    std::optional<SkinResourceUploadPlan> plan;
    bool cancelled = false;
    std::vector<SkinDiagnostic> diagnostics;
  };
  struct PreparedSkinResource {
    SkinResourceId id = 0;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    int width = 0;
    int height = 0;
    std::vector<SkinSourceRect> regions;
  };
  struct PreparedSkinTextAtlas {
    SkinTextAtlasId id = 0;
    SkinTextAtlasKey key;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    int width = 0;
    int height = 0;
    std::map<char32_t, SkinPreparedGlyphMetrics> glyphs;
    std::map<std::pair<char32_t, char32_t>, int> kerning;
    int ascent = 0;
    int descent = 0;
    int lineHeight = 0;
  };
  class SkinResourceCatalog;
  struct SkinResourceUploadResult {
    std::unique_ptr<SkinResourceCatalog> catalog;
    std::vector<SkinDiagnostic> diagnostics;
  };

  class SkinResourcePreparationService {
  public:
    SkinResourcePreparationService();
    ~SkinResourcePreparationService();
    // Synchronous and non-retaining; valid for unpublished PreparedPackage
    // staging as long as its owner encloses this whole call.
    SkinResourceValidationResult validateResources(
        SkinResourceValidationInputs);
    SkinResourcePlanResult decodeAndPlan(SkinResourcePreparationInputs);
    void shutdown() noexcept;
  private:
    // Cache outlives the coordinator: reverse destruction joins/stops workers
    // before cache teardown even if explicit shutdown was missed.
    std::mutex serviceMutex_;
    image_decode::DecodedImageCache cache_;
    image_decode::ImageDecodeCoordinator coordinator_;
  };

  class SkinResourceCatalog {
  public:
    static SkinResourceUploadResult upload(SkinResourceUploadPlan &&);
    ~SkinResourceCatalog(); // render-thread destruction before bgfx shutdown
    SkinResourceCatalog(const SkinResourceCatalog &) = delete;
    SkinResourceCatalog &operator=(const SkinResourceCatalog &) = delete;
    const PreparedSkinResource *find(SkinResourceId) const noexcept;
    const PreparedSkinTextAtlas *findTextAtlas(
        SkinTextAtlasId) const noexcept;
    const PreparedSkinTextAtlas *findTextAtlas(
        const SkinTextAtlasKey &) const noexcept;
    void enterRenderPhase() noexcept;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("render phase performs no resource IO") {
  auto fixture = prepareResourceFixture();
  fixture.simulateFrames(120);
  CHECK(fixture.renderPhaseReads() == 0);
  CHECK(fixture.renderPhaseUploads() == 0);
}
```

- [ ] **Step 2: Make the RED resource fixture reference synchronous non-retaining `validateResources`, the owning `decodeAndPlan`/render-thread `upload` lifecycle, and immutable ID-only lookups.** Validation must accept a prepared staging read view and finish before that owner can move; gameplay preparation accepts a cloned owning lease and transfers that pin into the uploaded catalog.
- [ ] **Step 3: Write resource tests first for selected file globs and defaults, source/reference resolution, sprite division and crop bounds, PNG/JPEG formats used by the target, invalid/oversized dimensions, decoded-byte/session counts, duplicate-resource reuse, missing critical versus optional assets, prepared-view validation lifetime, cloned owning-lease lifetime through upload/catalog teardown, cancellation, no overlay lookup, and zero reads/uploads after `enterRenderPhase()`.** Run concurrent `validateResources`, concurrent `decodeAndPlan`, validation/decode overlap, and shutdown-during-decode races against the one app-owned service. Prove cache/coordinator ticket state is serialized without holding `serviceMutex_` across a join or client callback, every waiter completes/cancels, the destructor's idempotent shutdown joins workers before cache teardown, and staged validation retains no path/view/decoded buffer or render upload after return. Destroy a populated catalog on the render thread before bgfx shutdown and require every image/atlas texture destroyed exactly once with live texture/resource counts returning to baseline; copy is forbidden.
- [ ] **Step 4: Add font tests for package TTF/OTF files used by the target, primary font type plus ordered fallback path/type declarations, the same font used at multiple text sizes/styles, digits/ASCII, all glyphs present in chart title/artist and static model text at preparation time, bounded atlas/cache keys, unknown runtime glyph diagnostics, and no active-frame glyph rasterization or texture creation**. Key atlases by font resource, point size, audited outline/shadow style, and the ordered fallback chain rather than storing one size on the font resource; assign stable atlas IDs and retain atlas width/height, glyph region/bearing/advance, ascent/descent/line-height, and the bounded set of used codepoint-pair kerning values so normalized UVs, alignment, wrapping, and shrink/overflow behavior require no font access at render time. Include a nontrivial region-to-normalized-UV assertion and an `AV` pair whose kerned width differs from the sum of advances. The committed font fixture must have a redistribution-compatible license recorded in its fixture README.
- [ ] **Step 5: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_resource_catalog_tests image_decode_coordinator_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(skin_resource_catalog_tests|image_decode_coordinator_tests)$' --output-on-failure
  ```

  Expected RED: resources are still only string paths and the shared decoder contract is not extracted.
- [ ] **Step 6: Extract the existing file decoder from `ImageView.cpp` into `image_decode::decodeImageFile(const ImageDecodeRequest &)` in `ImageFileDecoder.{h,cpp}`, leave `ImageView` behavior covered by its existing tests, and implement the Task 13 validation/plan/upload/catalog interfaces with a dedicated app-owned `SkinResourcePreparationService` using correctly namespaced `image_decode::ImageDecodeCoordinator` and `image_decode::DecodedImageCache`**. `validateResources` performs the same containment, decode, glyph, and limit checks synchronously through `SkinRevisionReadView`, discards all prepared bytes before return, and never queues/stores the view. `decodeAndPlan` accepts a cloned owning lease and transfers it through the plan into `SkinResourceCatalog`. Serialize shared cache/ticket bookkeeping inside the service; release the service mutex before waiting, joining, invoking decoders, or returning callbacks. Declare cache before coordinator and make the destructor call idempotent `shutdown`, which stops/joins the coordinator before cache destruction. The service is not the function-local `ImageView` coordinator/cache. Keep containment/session limits in `SkinResourceCatalog`; use SDL_ttf only during preparation and never route arbitrary skin text through `TextView`.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target skin_resource_catalog_tests image_decode_coordinator_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(skin_resource_catalog_tests|image_decode_coordinator_tests)$' --output-on-failure
  ```

  Expected GREEN: prepared catalogs perform no file or upload work during simulated frames and remain valid while the revision lease lives.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/beatoraja/SkinResourceCatalog.h src/skin/beatoraja/SkinResourceCatalog.cpp src/skin/beatoraja/SkinTextAtlas.h src/skin/beatoraja/SkinTextAtlas.cpp src/skin/CMakeLists.txt src/view/ImageFileDecoder.h src/view/ImageFileDecoder.cpp src/view/ImageView.cpp src/view/CMakeLists.txt tests/skin_resource_catalog_tests.cpp tests/image_decode_coordinator_tests.cpp tests/fixtures/beatoraja_skin/resources
  git commit -m "feat: prepare gameplay skin resources"
  ```

### Task 14: Evaluate whole frames into authored-order command buffers

**Reference refresh:** `Skin.prepare`, `Skin.draw`, `SkinObject.prepare`, `SkinObject.draw`, `JsonSkin.Destination`, each target object class's prepare/draw method, and `Skin.updateCustomObjects`.

**Files:**

- Create: `src/skin/beatoraja/SkinDrawCommand.h`
- Create: `src/skin/beatoraja/Skin2DRenderer.h`
- Create: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/skin_draw_command_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/commands/all_v1_objects_1280x720.json`
- Create: `tests/fixtures/beatoraja_skin/commands/all_v1_objects_ipad_fit.json`

**Interfaces:**

- Consumes: `ValidatedBeatorajaSkinModel`, `SkinResourceCatalog`, `PlaySkinViewport`, `LuaSkinRuntime`, and fakeable `ISkinFrameState` property/timer/note projections.
- Produces: `SkinFrameEvaluationResult Skin2DRenderer::evaluateFrame(const SkinFrameInputs &)`, where `submitReady` is optional. Critical failure structurally returns no command buffer, so partial evaluation cannot be submitted.

  ```cpp
  struct SkinVertex {
    float x = 0.0F; float y = 0.0F;
    float u = 0.0F; float v = 0.0F;
    std::uint32_t rgba = 0xffffffffU;
  };
  struct SkinRenderState {
    SkinBlendMode blend = SkinBlendMode::Normal;
    SkinFilterMode filter = SkinFilterMode::Nearest;
    std::optional<UiLogicalRect> scissor;
  };
  struct SkinTexturedQuadCommand {
    SkinResourceId resource = 0;
    std::array<SkinVertex, 4> vertices{};
    SkinRenderState state;
  };
  struct SkinGlyphInstance {
    char32_t codepoint = 0;
    std::array<SkinVertex, 4> vertices{};
  };
  struct SkinGlyphRunCommand {
    SkinTextAtlasId atlas = 0;
    std::vector<SkinGlyphInstance> glyphs;
    SkinRenderState state;
  };
  enum class SkinPrimitiveKind : std::uint8_t {
    SolidQuad, LineStrip, TriangleStrip
  };
  struct SkinPrimitiveCommand {
    SkinPrimitiveKind kind = SkinPrimitiveKind::SolidQuad;
    std::vector<SkinVertex> vertices;
    SkinRenderState state;
  };
  struct SkinBgaCommand {
    // Source dimensions are unknown until BGA preparation. Preserve the
    // evaluated pre-stretch authored geometry/mode and viewport so Task 19 can
    // derive final vertices/UVs separately for each prepared surface.
    AuthoredDestinationGeometry authoredGeometry;
    PlaySkinViewport viewport;
    std::uint32_t authoredOrdinal = 0;
  };
  using SkinDrawPayload = std::variant<
      SkinTexturedQuadCommand, SkinGlyphRunCommand,
      SkinPrimitiveCommand, SkinBgaCommand>;
  struct SkinDrawCommand {
    std::uint32_t authoredOrdinal = 0;
    SkinObjectId sourceObject = 0;
    SkinDrawPayload payload;
  };
  struct SkinBatchRange {
    std::size_t firstCommand = 0;
    std::size_t commandCount = 0;
  };
  struct SkinCommandBuffer {
    std::uint64_t frameSerial = 0;
    std::vector<SkinDrawCommand> commands;
    std::vector<SkinBatchRange> adjacentBatches;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("critical callback cannot expose a partial buffer") {
  const auto result = evaluateCriticalFailureAfterFirstObject();
  CHECK_FALSE(result.submitReady.has_value());
}
```

- [ ] **Step 2: Make the RED command fixture reference the exact immutable quad/glyph/primitive/BGA variants above and prove how numbers, sliders, graphs/gauges, covers, normal/invisible/mine notes, every distinct LN/CN/HCN source role, group/BPM/stop/time lines, and judge objects lower into them without losing authored order.** Carry both long-note endpoints/mode/state and line kind/position through the source-neutral views; do not flatten a long note into one point or make the renderer reconstruct data discarded by projection.
- [ ] **Step 3: Make the RED test reference this evaluate-then-submit API**:

  ```cpp
  template <typename T> struct SkinPropertyLookup {
    T value;
    bool supported = false;
  };

  enum class SkinProjectedNoteKind : std::uint8_t {
    Normal,
    Invisible,
    Mine
  };

  struct SkinProjectedNoteView {
    std::uint32_t visualId = 0;
    int lane = -1;
    SkinProjectedNoteKind kind;
    double scrollDelta = 0.0;
    bool judged = false;
    std::uint32_t submissionOrdinal = 0;
  };

  enum class SkinProjectedLongNoteMode : std::uint8_t { LN, CN, HCN };
  struct SkinProjectedLongNoteView {
    std::uint32_t headVisualId = 0;
    std::uint32_t tailVisualId = 0;
    int lane = -1;
    SkinProjectedLongNoteMode mode = SkinProjectedLongNoteMode::LN;
    double headScrollDelta = 0.0;
    double tailScrollDelta = 0.0;
    bool active = false;
    bool damaged = false;
    bool reactive = false;
    bool headJudged = false;
    bool tailJudged = false;
    std::uint32_t submissionOrdinal = 0;
  };

  enum class SkinProjectedLineKind : std::uint8_t {
    Group, Bpm, Stop, Time
  };
  struct SkinProjectedLineView {
    std::uint32_t timelineVisualId = 0;
    SkinProjectedLineKind kind = SkinProjectedLineKind::Time;
    double scrollDelta = 0.0;
    std::uint32_t submissionOrdinal = 0;
  };

  struct SkinGaugeStateView {
    bool supported = false;
    double value = 0.0;
    int gaugeType = 0;
    double minimum = 0.0;
    double maximum = 100.0;
    double border = 0.0;
  };

  class ISkinFrameState {
  public:
    virtual ~ISkinFrameState() = default;
    virtual SkinPropertyLookup<bool> booleanProperty(
        const SkinBuiltinPropertySelector &) = 0;
    virtual SkinPropertyLookup<std::int64_t> integerProperty(
        const SkinBuiltinPropertySelector &) = 0;
    virtual SkinPropertyLookup<double> floatProperty(
        const SkinBuiltinPropertySelector &) = 0;
    virtual SkinPropertyLookup<std::string_view> stringProperty(
        const SkinBuiltinPropertySelector &) = 0;
    virtual SkinPropertyLookup<ConfigOffset> offsetProperty(int) = 0;
    virtual std::int64_t timerProperty(
        const SkinBuiltinPropertySelector &) = 0;
    virtual std::span<const SkinProjectedNoteView>
    projectedNotes() const noexcept = 0;
    virtual std::span<const SkinProjectedLongNoteView>
    projectedLongNotes() const noexcept = 0;
    virtual std::span<const SkinProjectedLineView>
    projectedLines() const noexcept = 0;
    virtual SkinGaugeStateView gaugeState() const noexcept = 0;
  };

  struct SkinFrameInputs {
    std::uint64_t frameSerial = 0;
    std::int64_t visualTimeMicros = 0;
    const ValidatedBeatorajaSkinModel &model;
    const BeatorajaSkinConfiguration &configuration;
    const SkinResourceCatalog &resources;
    const PlaySkinViewport &viewport;
    LuaSkinRuntime &runtime;
    ISkinFrameState &state;
  };

  struct SkinFrameEvaluationResult {
    std::optional<SkinCommandBuffer> submitReady;
    std::vector<SkinDiagnostic> diagnostics;
  };

  class Skin2DRenderer {
  public:
    SkinFrameEvaluationResult evaluateFrame(const SkinFrameInputs &);
  };
  ```

- [ ] **Step 4: Write command tests first for every v1 object variant, built-in-name/ID and Lua-backed property bindings, configured signed option conditions and offset-ID composition, destination conditions, timers, animations/sprite cycles, plain/`Image.len`/cross-resource `ImageSet` state selection with out-of-range fallback to state zero, values/text, numeric/custom/`isRefNum` sliders and graphs, gauge/judge state, note/LN/line/cover variants, exact vertices/UV/color/clip/resource IDs, all 11 Task 12 stretch results (including trimmed UVs), authored order, adjacent-compatible batch ranges, and noncontiguous identical-resource commands remaining separated.** Include a nontrivial text-atlas pixel-region→normalized-UV case and an `AV` run whose alignment/wrap vertices use the prepared pair kerning. Resolve each model binding through its typed registry: built-ins call the matching `ISkinFrameState` selector lookup, while Lua-backed properties execute through `LuaSkinRuntime` under the same per-frame callback/instruction/time budget and rollback boundary. Resolve numeric option signs from `configuration.enabledOptionIds` and offsets from `configuration.offsetsById`; no configured state is inferred from Lua globals. Use only the matched snapshot's `visualTimeMicros` for destination timers/cycles and assert its frame serial matches the projection—never substitute render wall clock. A `SkinBgaObject` emits one role-free compositor command carrying evaluated pre-stretch authored geometry, `SkinStretchMode`, and viewport; it emits no guessed Base/Layer/Miss role and performs no source-dependent projection.
- [ ] **Step 5: Add failure tests where an optional callback disables only dependent commands and a critical callback fails after earlier objects have evaluated**. Assert critical failure returns no submit-ready buffer so a caller cannot draw a partial hybrid frame.
- [ ] **Step 6: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_draw_command_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_draw_command_tests$' --output-on-failure
  ```

  Expected RED: no evaluator/command types exist.
- [ ] **Step 7: Implement the Task 14 command/state/evaluate-submit interfaces with allocation-reusing storage**. Evaluate bindings/properties/callbacks in observed order; memoize only classified pure snapshot-backed built-ins, never arbitrary Lua callbacks. Match Task 17's projection result into the three source-neutral spans without losing long-note endpoints/mode or timeline line descriptors.
- [ ] **Step 8: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target skin_draw_command_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_draw_command_tests$' --output-on-failure
  ```

  Expected GREEN: both 1280x720 and iPad Fit expected-command fixtures match and batching never changes authored order.
- [ ] **Step 9: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/beatoraja/SkinDrawCommand.h src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp src/skin/CMakeLists.txt tests/skin_draw_command_tests.cpp tests/fixtures/beatoraja_skin/commands
  git commit -m "feat: evaluate ordered skin command buffers"
  ```

### Task 15: Submit ordered skin quads, text, and primitives through bgfx

**Reference refresh:** `Skin.java::SkinObjectRenderer` blend/type/filter behavior, `SkinObject.draw`, `SkinText.draw`, and target destinations using non-default blend/filter/clip/rotation.

**Files:**

- Create: `src/rendering/SkinQuadBatchRenderer.h`
- Create: `src/rendering/SkinQuadBatchRenderer.cpp`
- Create: `shader_src/vs_skin_quad.sc`
- Create: `shader_src/fs_skin_quad.sc`
- Generate: `shaders/metal/vs_skin_quad.bin`
- Generate: `shaders/metal/fs_skin_quad.bin`
- Generate: `shaders/spirv/vs_skin_quad.bin`
- Generate: `shaders/spirv/fs_skin_quad.bin`
- Generate: `shaders/essl/vs_skin_quad.bin`
- Generate: `shaders/essl/fs_skin_quad.bin`
- Generate on Windows verification: `shaders/dx11/vs_skin_quad.bin`
- Generate on Windows verification: `shaders/dx11/fs_skin_quad.bin`
- Create: `scripts/verify_skin_shader_outputs.py`
- Create: `tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json`
- Modify: `src/rendering/CMakeLists.txt`
- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/ios_artifact_audit_tests.py`
- Test: `tests/skin_quad_batch_renderer_tests.cpp`
- Test: `tests/skin_renderer_golden_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/golden/fit_16x9.png`
- Create: `tests/fixtures/beatoraja_skin/golden/stretch_16x9.png`
- Create: `tests/fixtures/beatoraja_skin/golden/custom_16x9.png`
- Create: `tests/fixtures/beatoraja_skin/golden/fit_4x3.png`
- Create: `tests/fixtures/beatoraja_skin/golden/stretch_4x3.png`
- Create: `tests/fixtures/beatoraja_skin/golden/custom_4x3.png`

**Interfaces:**

- Consumes: an immutable submit-ready `SkinCommandBuffer` whose vertices/scissors are UI-logical.
- Produces: `SkinQuadBatchRenderer::begin(RenderContext &, const SkinResourceCatalog &)`, `submit(span<const SkinDrawCommand>)`, and `flush()`; `Skin2DRenderer::submit(const SkinCommandBuffer &, const SkinResourceCatalog &, RenderContext &, SkinQuadBatchRenderer &) const` preserves authored order. The session-owned catalog remains alive through flush and is the only ID→bgfx-handle resolver; there is no hidden/global texture table. Scissors pass through `rendering::setScissorUI`, so drawable scaling happens exactly once.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("all six viewport goldens are covered") {
  CHECK(goldenCaseNames() == std::vector<std::string>{
      "fit_16x9", "stretch_16x9", "custom_16x9",
      "fit_4x3", "stretch_4x3", "custom_4x3"});
}
```

- [ ] **Step 2: Write backend-state tests first for explicit catalog resolution of image and composite-key text-atlas IDs, glyph metrics, per-vertex position/UV/color, rotated quads, nearest/linear sampler selection, UI-logical scissor changes at 1x/2x drawable scale, normal/additive/subtractive/multiply/inverse blend mapping, sequential submit order, and batch flush on any state/resource/scissor change.** A missing/stale ID is caught during whole-buffer preflight and produces fallback before submission; submit never consults a hidden global or performs a fallible late lookup after drawing begins.
- [ ] **Step 3: Add an offscreen deterministic fixture scene and six readback goldens: Fit, Stretch, and Custom at fixed 16:9 and iPad-style 4:3 targets**. Compare dimensions exactly and pixels with a fixed one-channel tolerance of 2 to allow backend rounding; fail if authored command order changes visible overlap. All committed pixels must come from redistributable synthetic fixtures.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_quad_batch_renderer_tests skin_renderer_golden_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(skin_quad_batch_renderer_tests|skin_renderer_golden_tests)$' --output-on-failure
  python3 -m unittest tests/ios_artifact_audit_tests.py -v
  ```

  Expected RED: shader/backend/golden submissions are absent.
- [ ] **Step 5: Implement `SkinQuadBatchRenderer` separately from `TexBatchRenderer` so built-in behavior cannot change**. Bind the explicit session catalog for exactly `begin`→`flush`, preflight every command ID before the first draw, and submit skin content sequentially on `rendering::ui_view`; submit native pause/reset overlays afterward so they remain reachable above the skin. Do not use depth sorting to reconstruct authored order.
- [ ] **Step 6: Compile shaders on macOS and verify the exact six locally generated outputs**:

  ```sh
  cd shader_src && SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py
  cd ..
  python3 scripts/verify_skin_shader_outputs.py \
    --shader skin_quad --require-backends metal,spirv,essl
  ```

  Expected on this macOS host: Metal, SPIR-V, and ESSL pairs are present, nonempty, and listed by the verifier; `shader_src/make.py` intentionally cannot compile HLSL/DX11 on macOS. Before the task commit, run this executable gate in a clean Windows checkout with the repository's Windows `shadercRelease.exe`:

  ```powershell
  $env:SHADERC = (Resolve-Path 'bgfx\bgfx\.build\win64_mingw-gcc\bin\shadercRelease.exe')
  Push-Location shader_src
  py -3 make.py
  Pop-Location
  py -3 scripts\verify_skin_shader_outputs.py --shader skin_quad --require-backends metal,spirv,essl,dx11 --write-manifest tests\fixtures\beatoraja_skin\shaders\skin_shader_manifest.json
  py -3 scripts\verify_skin_shader_outputs.py --shader skin_quad --require-backends metal,spirv,essl,dx11 --manifest tests\fixtures\beatoraja_skin\shaders\skin_shader_manifest.json
  git diff --exit-code -- shader_src
  ```

  Preserve and commit the resulting DX11 pair together with the six macOS-generated outputs. The manifest records the exact shader-source and eight output SHA-256 values; normal `--manifest` verification is read-only. `verify_skin_shader_outputs.py` rejects missing/empty/hash-mismatched outputs and unexpected broad shader-tree changes; its Python unit coverage is part of `ios_artifact_audit_tests.py`. The script already recursively discovers `vs_*`/`fs_*`, so this task does not modify `make.py`.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target skin_quad_batch_renderer_tests skin_renderer_golden_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(skin_quad_batch_renderer_tests|skin_renderer_golden_tests)$' --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  python3 scripts/verify_skin_shader_outputs.py --shader skin_quad --require-backends metal,spirv,essl,dx11 --manifest tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json
  python3 -m unittest tests/ios_artifact_audit_tests.py -v
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: backend state, offscreen goldens, packaged shaders, and Metal linking pass.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/rendering/SkinQuadBatchRenderer.h src/rendering/SkinQuadBatchRenderer.cpp src/rendering/CMakeLists.txt src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp shader_src/vs_skin_quad.sc shader_src/fs_skin_quad.sc shaders/metal/vs_skin_quad.bin shaders/metal/fs_skin_quad.bin shaders/spirv/vs_skin_quad.bin shaders/spirv/fs_skin_quad.bin shaders/essl/vs_skin_quad.bin shaders/essl/fs_skin_quad.bin shaders/dx11/vs_skin_quad.bin shaders/dx11/fs_skin_quad.bin scripts/verify_skin_shader_outputs.py tests/ios_artifact_audit_tests.py tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json tests/skin_quad_batch_renderer_tests.cpp tests/skin_renderer_golden_tests.cpp tests/fixtures/beatoraja_skin/golden
  git commit -m "feat: render ordered gameplay skin commands"
  ```

## Slice 4 — Gameplay Integration

### Task 16: Establish pointer-free chart and per-frame presentation state

**Reference refresh:** `BMSPlayer.java`, `JudgeManager.java`, `KeyInputProccessor.java`, `PlaySkin.java`, `CustomTimer.java`, `CustomEvent.java`, and `LaneRenderer.java` state read during one draw.

**Files:**

- Create: `src/scene/play/PlayfieldPresentationEvents.h`
- Create: `src/scene/play/PlayfieldChartVisualModel.h`
- Create: `src/scene/play/PlayfieldChartVisualModel.cpp`
- Create: `src/scene/play/PlayfieldVisualState.h`
- Create: `src/scene/play/PlayfieldVisualState.cpp`
- Create: `src/scene/play/PlayfieldPresentation.h`
- Modify: `src/scene/play/RhythmLaneInputController.h`
- Modify: `src/scene/play/RhythmLaneInputController.cpp`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/scene/SettingsScenePreview.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/playfield_visual_state_tests.cpp`
- Modify: `tests/gameplay_simulation_tests.cpp`
- Modify: `tests/realtime_gameplay_worker_tests.cpp`

**Interfaces:**

- Consumes: `bms_parser::Chart` only during one-time conversion, pointer-free gameplay-authority updates, profile presentation configuration, copied replay visuals, and actual event timestamps.
- Produces: `PlayfieldChartVisualModel buildPlayfieldChartVisualModel(const bms_parser::Chart &, int longNoteModeOverride)` and a `PlayfieldVisualStateStore final : IPlayfieldPresentationEvents` with `setConfiguration`, `applyAuthorityUpdate`, live-touch methods, and `capture(PlayfieldFrameClock)`. Captured DTOs contain no parser/gameplay pointers, atomics, bgfx handles, or `ReplayData *`.

  ```cpp
  struct PlayfieldFrameClock {
    std::uint64_t serial = 0;
    long long visualTimeMicros = 0;
    long long gameplayTimeMicros = 0;
    long long replayTouchTimeMicros = 0;
    long long bgaTimeMicros = 0;
  };

  struct PlayfieldPresentationConfig {
    int visibleTimeGreenNumber = 0;
    bool visibleTimeUseMilliseconds = false;
    AppSettings::VisibleTimeBpmStrategy visibleTimeBpmStrategy;
    float playAreaWidth = 0.0F;
    bool laneBeamsEnabled = true;
    bool laneCoverFloatingEnabled = true;
    int laneBeamLengthPercent = 100;
    int noteStartPositionPercent = 0;
    bool laneBeamClockUsesRenderTime = false;
    bool showInvisibleNotes = false;
    bool judgementIndicatorEnabled = true;
    float judgementIndicatorY = 0.0F;
    float judgementIndicatorWidthScale = 1.0F;
    bool judgementIndicatorHudMode = false;
    int judgementIndicatorRangeMilliseconds = 0;
    float judgementTextY = 0.0F;
    bool judgementCounterEnabled = false;
    AppSettings::JudgementCounterPosition judgementCounterPosition;
    AppSettings::JudgementTimingDisplayCriteria fastSlowCriteria;
    AppSettings::JudgementTimingDisplayCriteria millisecondsCriteria;
    AppSettings::GaugeBarPosition gaugeBarPosition;
    bool touchVisualizationEnabled = true;
    bool replayGhostRenderingEnabled = true;
  };

  struct PlayfieldAuthorityUpdate {
    double currentBpm = 0.0;
    std::map<Judgement, int> judgementCounters;
    int comboBreak = 0;
    GaugeType gaugeType;
    GaugeAutoShiftMode gaugeAutoShift;
    float currentGauge = 0.0F;
    GameplayGaugeRules gaugeRules;
    pacemaker::Target pacemakerTarget;
    pacemaker::Snapshot pacemakerStatus;
    std::string playOptionLabel;
    bool autoPlayMarkVisible = false;
    std::vector<int> startLaneIndicators;
    bool startLaneIndicatorsVisible = false;
    int laneCoverPercent = 0;
    bool resetLaneCoverVisibleTimeReference = false;
  };

  using ChartVisualId = std::uint32_t;

  enum class ChartVisualNoteKind : std::uint8_t {
    Normal, Invisible, Mine, LongHead, LongBody, LongTail
  };

  enum class ChartLongNoteMode : std::uint8_t { LN, CN, HCN };

  struct ChartVisualTimeline {
    ChartVisualId id = 0;
    long long timeMicros = 0;
    double beat = 0.0;
    double scrollPosition = 0.0;
    double bpm = 0.0;
    double scrollRate = 1.0;
    long long stopMicros = 0;
    bool sectionLine = false;
    bool bgaOnly = false;
    std::uint32_t authoredOrdinal = 0;
  };

  struct ChartVisualNote {
    ChartVisualId id = 0;
    ChartVisualId timelineId = 0;
    ChartVisualId pairId = 0;
    int lane = -1;
    ChartVisualNoteKind kind = ChartVisualNoteKind::Normal;
    ChartLongNoteMode longNoteMode = ChartLongNoteMode::LN;
    int mineDamage = 0;
    std::uint32_t authoredOrdinal = 0;
  };

  struct PlayfieldChartTextMetadata {
    std::string title;
    std::string subtitle;
    std::string artist;
    std::string subartist;
    std::string genre;
    // Stable value copies for any additional immutable chart-string property
    // admitted by Task 1's audited surface, keyed by property ID.
    std::map<int, std::string> auditedStringProperties;
  };

  struct PlayfieldChartVisualModel {
    std::string chartSha256;
    int keyCount = 0;
    PlayfieldChartTextMetadata text;
    std::vector<int> laneOrder;
    std::vector<ChartVisualTimeline> timelines;
    std::vector<ChartVisualNote> notes;
    std::vector<double> scrollPrefix;
  };

  struct LanePresentationState {
    bool pressed = false;
    // Zero is a valid first-frame event time; OFF is the shared sentinel.
    long long pressMicros = INT64_MIN;
    long long releaseMicros = INT64_MIN;
    long long bombMicros = INT64_MIN;
  };

  struct NotePresentationState {
    ChartVisualId id = 0;
    bool judged = false;
    bool dead = false;
    bool longActive = false;
    bool longDamaged = false;
    bool longReactive = false;
  };

  struct PresentationTouchPoint {
    long long fingerId = 0;
    ReplayTouchAction action;
    float normalizedX = 0.0F;
    float normalizedY = 0.0F;
    long long songTimeMicros = 0;
  };

  struct PlayfieldVisualState {
    PlayfieldFrameClock clock;
    PlayfieldPresentationConfig configuration;
    PlayfieldAuthorityUpdate authority;
    std::vector<LanePresentationState> lanes;
    std::vector<NotePresentationState> notes;
    std::vector<PresentationTouchPoint> touches;
    JudgeResult lastJudge = JudgeResult(None, 0);
    long long lastJudgeVisualMicros = INT64_MIN;
    int combo = 0;
    int score = 0;
    int fastSlowMicros = 0;
    long long sceneStartMicros = 0;
    long long playStartMicros = 0;
  };

  struct PlayfieldJudgeEventClock {
    long long songTimeMicros = 0;
    long long visualTimeMicros = 0;
    long long bgaTimeMicros = 0;
  };

  class PlayfieldVisualStateStore final
      : public IPlayfieldPresentationEvents {
  public:
    void setConfiguration(const PlayfieldPresentationConfig &);
    void applyAuthorityUpdate(const PlayfieldAuthorityUpdate &);
    void setLiveTouchPoint(long long fingerId, ReplayTouchAction,
                           float x, float y, long long songTimeMicros);
    void clearLiveTouchPoints();
    PlayfieldVisualState capture(PlayfieldFrameClock) const;
    void onLanePressed(int, JudgeResult, long long) override;
    void onLaneReleased(int, long long) override;
    void onJudge(JudgeResult, int, int,
                 PlayfieldJudgeEventClock, bool) override;
  };

  // GamePlayScene gives this one sink to RhythmLaneInputController and every
  // judge producer. It updates the captured-state store first, then forwards
  // the same event exactly once to the current presentation sink.
  class PlayfieldPresentationEventFanout final
      : public IPlayfieldPresentationEvents {
  public:
    PlayfieldPresentationEventFanout(
        PlayfieldVisualStateStore &, IPlayfieldPresentationEvents &);
    void setPresentationSink(IPlayfieldPresentationEvents &) noexcept;
    void onLanePressed(int, JudgeResult, long long) override;
    void onLaneReleased(int, long long) override;
    void onJudge(JudgeResult, int, int,
                 PlayfieldJudgeEventClock, bool) override;
  private:
    PlayfieldVisualStateStore &state_;
    IPlayfieldPresentationEvents *presentation_;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("lane controller emits no judge event") {
  RecordingPresentationEvents events;
  exerciseLanePressAndRelease(events);
  CHECK(events.pressCount == 1);
  CHECK(events.releaseCount == 1);
  CHECK(events.judgeCount == 0);
}
```

- [ ] **Step 2: Make the RED input/event tests reference this narrow event seam and require `BMSRenderer`'s existing by-value signatures**:

  ```cpp
  class IPlayfieldPresentationEvents {
  public:
    virtual ~IPlayfieldPresentationEvents() = default;
    virtual void onLanePressed(int lane, JudgeResult,
                               long long eventMicros) = 0;
    virtual void onLaneReleased(int lane, long long eventMicros) = 0;
    virtual void onJudge(JudgeResult, int combo, int score,
                         PlayfieldJudgeEventClock,
                         bool recordTimingSample) = 0;
  };
  ```

- [ ] **Step 3: Make the RED visual-state tests reference `PlayfieldPresentationConfig` for every static renderer input and `PlayfieldVisualState`/chart model for every listed runtime or replay value.**
- [ ] **Step 4: Make the RED tests reference `PlayfieldPresentation::configure`, the three event methods, `reset`, and `refreshGeometry`; later tasks add render/touch DTOs**. Assert `BMSRenderer` will mark the existing signatures `override`.
- [ ] **Step 5: Extend `tests/gameplay_simulation_tests.cpp` to require an `IPlayfieldPresentationEvents *` sink, the same press/release calls, and exact transaction timestamps; extend `tests/realtime_gameplay_worker_tests.cpp` plus the new store test for live, catch-up, replay, and judge sources.** Every judge source supplies one `PlayfieldJudgeEventClock`: preserve recorded song time, derive visual and BGA clocks from it exactly once, and never substitute render `nowMicros()`. Cover nonzero visual/BGA offsets, catch-up, replay, and exporter paths; Task 19 uses the BGA member for miss-layer triggering. Adapt direct production callers too: `ReplayVideoExporter.cpp` constructs song/visual/BGA members from its exported transaction time and the same configured offsets as gameplay, while `SettingsScenePreview.cpp` uses its deterministic preview clock for all three fields. Test `PlayfieldPresentationEventFanout` with distinct recording store/presentation sinks: each press, release, and judge reaches each sink once in store-then-presentation order, `setPresentationSink` redirects subsequent events without replay or duplication, and the lane controller has exactly one fan-out pointer.
- [ ] **Step 6: Write `playfield_visual_state_tests` first**. Require stable value IDs instead of `Note *`, immutable copied title/subtitle/artist/subartist/genre plus every Task 1-audited chart string and lane ordering, one coherent frame serial/time, lane press/release/bomb timestamps with `INT64_MIN` as OFF so time zero remains valid, normal/invisible/mine and LN/CN/HCN lifecycle values, judge/combo/score/counters/timing plus the last judge's exact `PlayfieldJudgeEventClock::visualTimeMicros`, gauge/rules, pacemaker, play-option, lane-cover, start indicators, replay/touch visuals, BGA time, scene/timer starts, and no mutation after capture. Exercise live, catch-up, replay, preview, and export judge clocks without substituting capture/render time. Build the exact deduplicated string list used both for Task 13 glyph preparation and Task 18 string-property lookup; it contains no chart/parser pointer or host path.
- [ ] **Step 7: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target playfield_visual_state_tests gameplay_simulation_tests realtime_gameplay_worker_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(playfield_visual_state_tests|gameplay_simulation_tests|realtime_gameplay_worker_tests)$' --output-on-failure
  ```

  Expected RED: pointer-free chart/state stores and the event-sink/clock propagation are missing.
- [ ] **Step 8: Implement the Task 16 event/config/state interfaces, mark `BMSRenderer` overrides, and change `RhythmLaneInputController` to the event seam**. `GamePlayScene` owns `PlayfieldVisualStateStore`, then the current `PlayfieldPresentation`, then one `PlayfieldPresentationEventFanout(stateStore, *presentation)` and passes only the fan-out to `RhythmLaneInputController` and judge producers. No producer calls the store or renderer separately. `onJudge` stores the supplied visual-clock member verbatim for judge-object timers while Task 19 consumes the supplied BGA-clock member for miss-layer timing; neither derives an event time from render/capture time. Build `PlayfieldChartVisualModel` once with stable copied values; update/capture `PlayfieldVisualStateStore` on the gameplay thread only after realtime synchronization.
- [ ] **Step 9: Keep `Chart *`, `RhythmState *`, mutable `Note *`, atomics, and renderer resources out of both public DTOs.**
- [ ] **Step 10: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target playfield_visual_state_tests gameplay_simulation_tests realtime_gameplay_worker_tests logical_gameplay_input_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(playfield_visual_state_tests|gameplay_simulation_tests|realtime_gameplay_worker_tests|logical_gameplay_input_tests)$' --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  ```

  Expected GREEN: event behavior remains unchanged and snapshots are coherent.
- [ ] **Step 11: Commit the task**

  ```sh
  git add CMakeLists.txt src/scene/play/PlayfieldPresentationEvents.h src/scene/play/PlayfieldPresentation.h src/scene/play/PlayfieldChartVisualModel.h src/scene/play/PlayfieldChartVisualModel.cpp src/scene/play/PlayfieldVisualState.h src/scene/play/PlayfieldVisualState.cpp src/scene/play/RhythmLaneInputController.h src/scene/play/RhythmLaneInputController.cpp src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp src/ReplayVideoExporter.cpp src/scene/SettingsScenePreview.cpp src/scene/play/CMakeLists.txt tests/playfield_visual_state_tests.cpp tests/gameplay_simulation_tests.cpp tests/realtime_gameplay_worker_tests.cpp
  git commit -m "refactor: add gameplay presentation snapshots"
  ```

### Task 17: Extract shared note/LN/scroll projection and preserve built-in rendering

**Reference refresh:** `LaneRenderer.init`, `LaneRenderer.getCurrentSpeed`, `LaneRenderer.drawLane`, `SkinNote`, `PlaySkin.getLaneRegion`, and `SkinObject` note destination handling.

**Files:**

- Create: `src/scene/play/PlayfieldProjection.h`
- Create: `src/scene/play/PlayfieldProjection.cpp`
- Modify: `src/scene/play/PlayfieldChartVisualModel.h`
- Modify: `src/scene/play/PlayfieldChartVisualModel.cpp`
- Modify: `src/scene/play/PlayfieldPresentation.h`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/playfield_chart_visual_model_tests.cpp`
- Test: `tests/playfield_projection_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/projection/gimmick_chart_v1.json`
- Create: `tests/fixtures/gameplay_presentation/builtin_7k_16x9.png`
- Create: `tests/fixtures/gameplay_presentation/builtin_timing_v1.json`

**Interfaces:**

- Consumes: one `PlayfieldChartVisualModel`, one matching `PlayfieldVisualState`, and the existing scroll/order/budget helpers.
- Produces: value-only `PlayfieldProjectionResult PlayfieldProjection::project(const PlayfieldChartVisualModel &, const PlayfieldVisualState &, const PlayfieldProjectionRequest &)` plus `reset()`, and `BMSRenderer::render(RenderContext &, const PlayfieldVisualState &, const PlayfieldProjectionResult &)`. Preserve both legacy `render(context, micro)` overloads as adapters for preview/export callers.

  ```cpp
  enum class ProjectedLineKind : std::uint8_t {
    Section, BpmChange, Stop, Time
  };

  struct PlayfieldProjectionRequest {
    double visibleScrollBefore = 0.0;
    double visibleScrollAfter = 0.0;
    std::size_t maxTimelines = 0;
    std::size_t maxNotes = 0;
    bool includeInvisibleNotes = false;
  };

  struct ProjectedTimelineDescriptor {
    ChartVisualId timelineId = 0;
    double scrollDelta = 0.0;
    long long timeMicros = 0;
    std::uint32_t submissionOrdinal = 0;
  };

  struct ProjectedPlayfieldNote {
    ChartVisualId noteId = 0;
    int lane = -1;
    ChartVisualNoteKind kind = ChartVisualNoteKind::Normal;
    double scrollDelta = 0.0;
    bool judged = false;
    std::uint32_t submissionOrdinal = 0;
  };

  struct ProjectedLongNoteDescriptor {
    ChartVisualId headId = 0;
    ChartVisualId tailId = 0;
    int lane = -1;
    ChartLongNoteMode mode = ChartLongNoteMode::LN;
    double headScrollDelta = 0.0;
    double tailScrollDelta = 0.0;
    bool active = false;
    bool damaged = false;
    bool reactive = false;
    bool headJudged = false;
    bool tailJudged = false;
    std::uint32_t submissionOrdinal = 0;
  };

  struct ProjectedLineDescriptor {
    ChartVisualId timelineId = 0;
    ProjectedLineKind kind = ProjectedLineKind::Time;
    double scrollDelta = 0.0;
    std::uint32_t submissionOrdinal = 0;
  };

  struct PlayfieldProjectionResult {
    std::uint64_t frameSerial = 0;
    double currentScrollPosition = 0.0;
    std::vector<ProjectedTimelineDescriptor> timelines;
    std::vector<ProjectedPlayfieldNote> notes;
    std::vector<ProjectedLongNoteDescriptor> longNotes;
    std::vector<ProjectedLineDescriptor> lines;
    bool budgetExceeded = false;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("projection expires invisible notes without mutation") {
  auto fixture = makeInvisibleNoteProjectionFixture();
  const auto before = fixture.chartBytes();
  CHECK(fixture.project().notes.empty());
  CHECK(fixture.chartBytes() == before);
}
```

- [ ] **Step 2: Write model/projection tests first for retained timeline filtering, BGA-only rows, lane order/map, BPM/stop/scroll/speed prefix data, `scrollPositionAtTime`, visible-window boundaries, section/BPM/stop/time lines, normal/invisible/mine notes, LN/CN/HCN head/body/tail/active/damage/reactive phases, orphan heads, dead tails, deterministic authored/submission ordering, and render budget limits.** Add an exact adapter test from `PlayfieldProjectionResult` to Task 14's source-neutral note/long-note/line spans: both LN endpoints, mode, state flags, line kind, scroll delta, and submission ordinal must survive unchanged.
- [ ] **Step 3: Reuse `GameplayScrollGeometry.h`, `GameplayNoteSubmissionOrder.h`, and `GameplayChartEntityRenderBudget.h` as the low-level authority**. Do not clone their formulas.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target playfield_chart_visual_model_tests playfield_projection_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(playfield_chart_visual_model_tests|playfield_projection_tests)$' --output-on-failure
  ```

  Expected RED: the current projection remains embedded in `BMSRenderer`.
- [ ] **Step 5: Before moving code, capture a redistributable synthetic built-in 7-key visual golden and timing characterization covering submission order, event timestamps, projection boundaries, HUD snapshot, touch bounds, and lane-cover response.** Freeze it as the pre-refactor baseline.
- [ ] **Step 6: Move retained-timeline construction, lane mapping, timeline scroll positions, visible traversal, and note/LN descriptor generation out of `BMSRenderer`**. Remove render-time mutation of `timeline->InvisibleNotes`/`note->IsDead`; projection suppresses past invisible notes from captured frame time (or a pointer-free expired-ID cursor). Copy `IsDead` only when gameplay, rather than presentation code, owns that lifecycle. Implement the one lossless Task 14 view adapter here and share it between built-in/skin paths rather than rebuilding long-note bodies or line descriptors in the renderer.
- [ ] **Step 7: Add the state/projection render method to `PlayfieldPresentation`, implement it in `BMSRenderer`, and preserve both existing render overloads as internal-snapshot adapters**. Keep current settings preview/replay exporter behavior built-in-only.
- [ ] **Step 8: Wire `GamePlayScene` to update `PlayfieldVisualStateStore`, capture once after realtime synchronization, project once, and feed the same immutable values to the built-in renderer**. Preserve actual replay/catch-up transaction timestamps rather than substituting render `nowMicros()`. Assert the post-refactor pixels remain within the frozen tolerance and the timing/projection records remain identical when compatibility mode is disabled.
- [ ] **Step 9: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target playfield_chart_visual_model_tests playfield_projection_tests gameplay_scroll_geometry_tests gameplay_chart_entity_render_budget_tests gameplay_simulation_tests realtime_gameplay_worker_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(playfield_chart_visual_model_tests|playfield_projection_tests|gameplay_scroll_geometry_tests|gameplay_chart_entity_render_budget_tests|gameplay_simulation_tests|realtime_gameplay_worker_tests)$' --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  ```

  Expected GREEN: built-in mode is behaviorally equivalent and the projection has one owner.
- [ ] **Step 10: Commit the task**

  ```sh
  git add CMakeLists.txt src/scene/play/PlayfieldProjection.h src/scene/play/PlayfieldProjection.cpp src/scene/play/PlayfieldChartVisualModel.h src/scene/play/PlayfieldChartVisualModel.cpp src/scene/play/PlayfieldPresentation.h src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp src/scene/play/CMakeLists.txt tests/playfield_chart_visual_model_tests.cpp tests/playfield_projection_tests.cpp tests/fixtures/beatoraja_skin/projection tests/fixtures/gameplay_presentation
  git commit -m "refactor: share gameplay chart projection"
  ```

### Task 18: Map gameplay snapshots to Beatoraja properties, timers, and safe events

**Reference refresh:** all target IDs in `BooleanPropertyFactory.java`, `IntegerPropertyFactory.java`, `FloatPropertyFactory.java`, `StringPropertyFactory.java`, `TimerPropertyFactory.java`, `EventFactory.java`, `MainStatePropertyLuaApiExporter.java`, `CustomTimer.java`, `CustomEvent.java`, and `Skin.updateCustomObjects`.

**Files:**

- Create: `src/skin/beatoraja/PlaySkinStateBridge.h`
- Create: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.h`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.h`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/play_skin_state_bridge_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/traces/scuro_property_frames_v1.json`
- Create: `tests/fixtures/beatoraja_skin/event_mutation_table_v1.json`

**Interfaces:**

- Consumes: one state/projection pair with the same frame serial, model timers/events, runtime callback APIs, and the frozen mutation table.
- Produces: `PlaySkinStateBridge final : ISkinFrameState`, `SkinHostCallResult updateCustomObjects()`, zero-to-two-argument `executeEvent`, a typed frozen `SkinEventMutationTable`, and a rollbackable `PlaySkinFrameCommit {frameSerial, orderedMutations[]}`. Timer properties return their start timestamp or exactly `INT64_MIN`.

  ```cpp
  struct SetSkinOption {
    std::string key;
    int value = 0;
  };

  struct SetSkinFilePath {
    std::string key;
    std::string declaredValue;
  };

  struct SetSkinOffset {
    std::string key;
    ConfigOffset value;
  };

  struct SessionPresentationWrite {
    int eventId = 0;
    std::array<int, 2> arguments{};
    std::uint8_t argumentCount = 0;
  };

  enum class SkinEventMutationKind : std::uint8_t {
    SessionPresentation,
    SetOption,
    SetFilePath,
    SetOffset,
    ReadOnly,
    Unsupported
  };
  struct SkinEventMutationRule {
    int builtInEventId = 0;
    SkinEventMutationKind kind = SkinEventMutationKind::Unsupported;
    std::uint8_t minimumArguments = 0;
    std::uint8_t maximumArguments = 0;
    std::string configurationKey;
  };
  class SkinEventMutationTable {
  public:
    static constexpr std::uint32_t schemaVersion = 1;
    const SkinEventMutationRule *find(int builtInEventId) const noexcept;
  };
  SkinEventMutationTable makePinnedSkinEventMutationTableV1();

  using PersistedSkinConfigurationWrite =
      std::variant<SetSkinOption, SetSkinFilePath, SetSkinOffset>;
  using SkinFrameMutation = std::variant<
      SessionPresentationWrite, PersistedSkinConfigurationWrite>;

  enum class SkinHostCallStatus : std::uint8_t {
    Completed,
    Unsupported,
    BudgetExceeded,
    CriticalFailure
  };

  struct SkinHostCallResult {
    SkinHostCallStatus status = SkinHostCallStatus::Completed;
    std::uint32_t callbacksInvoked = 0;
    std::vector<SkinDiagnostic> diagnostics;
    bool ok() const noexcept {
      return status == SkinHostCallStatus::Completed ||
             status == SkinHostCallStatus::Unsupported;
    }
  };

  struct PlaySkinFrameCommit {
    std::uint64_t frameSerial = 0;
    std::vector<SkinFrameMutation> orderedMutations;
  };

  struct PlaySkinStateBridgeContext {
    const PlayfieldChartVisualModel &chartModel;
    const ValidatedBeatorajaSkinModel &model;
    const BeatorajaSkinConfiguration &configuration;
    LuaSkinRuntime &runtime;
    const SkinEventMutationTable &mutationTable;
  };

  class PlaySkinStateBridge final : public ISkinFrameState {
  public:
    explicit PlaySkinStateBridge(PlaySkinStateBridgeContext);
    void beginFrame(const PlayfieldVisualState &,
                    const PlayfieldProjectionResult &);
    SkinHostCallResult updateCustomObjects();
    SkinHostCallResult executeEvent(int, std::span<const int> arguments);
    PlaySkinFrameCommit commitFrame();
    void discardFrame() noexcept;
    SkinPropertyLookup<bool> booleanProperty(
        const SkinBuiltinPropertySelector &) override;
    SkinPropertyLookup<std::int64_t> integerProperty(
        const SkinBuiltinPropertySelector &) override;
    SkinPropertyLookup<double> floatProperty(
        const SkinBuiltinPropertySelector &) override;
    SkinPropertyLookup<std::string_view> stringProperty(
        const SkinBuiltinPropertySelector &) override;
    SkinPropertyLookup<ConfigOffset> offsetProperty(int) override;
    std::int64_t timerProperty(
        const SkinBuiltinPropertySelector &) override;
    std::span<const SkinProjectedNoteView>
    projectedNotes() const noexcept override;
    std::span<const SkinProjectedLongNoteView>
    projectedLongNotes() const noexcept override;
    std::span<const SkinProjectedLineView>
    projectedLines() const noexcept override;
    SkinGaugeStateView gaugeState() const noexcept override;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("custom timers precede automatic events") {
  const auto calls = updateCustomObjectsFixture();
  CHECK(calls == expectedAuthoredTimersThenAuthoredEvents());
}
```

- [ ] **Step 2: Make the RED bridge tests reference the complete `PlaySkinStateBridgeContext`, `beginFrame`, built-in selector lookups for every typed property/timer kind, all three note/long-note/line spans, and the rollbackable commit API**. Exercise equivalent recognized ID/name selectors and require caching only for classified pure host properties, never arbitrary Lua callbacks. Construct with a validated model/runtime/configuration/frozen mutation table and prove custom timers/events are reachable without global state.
- [ ] **Step 3: Write bridge tests first for every SCURO manifest property/timer/event ID or recognized name, the complete version-1 mutation table and argument bounds, missing-selector type-specific semantics, timer start/OFF values, lane press/release/bomb/judge timers, score/combo/gauge/pacemaker/title/subtitle/artist/subartist/genre/audited-string/option fields, normal/invisible/mine notes, complete LN/CN/HCN endpoints/mode/state, group/BPM/stop/time line descriptors, and one deduplicated diagnostic per unsupported lookup.** Judge timer lookup must return Task 16's stored `lastJudgeVisualMicros` verbatim, including a valid event at zero and `INT64_MIN` before any event; it never substitutes the current frame clock. Construct the bridge from the same immutable `PlayfieldChartVisualModel` whose deduplicated strings were supplied to Task 13 resource preparation, and assert exact equality. Unknown/mismatched mutation rules stage nothing; the table is value-owned by `PlaySkinSession` and cannot be replaced by skin code.
- [ ] **Step 4: Add custom-object tests proving custom timers evaluate and cache once per frame in deterministic authored order, automatic custom events follow in authored order, the entire timer phase precedes the event phase, nonempty maps carry the frozen divergence diagnostic, the selected SCURO maps are empty, manual events accept zero/one/two integers, minimum interval uses the captured clock, and frame/callback instruction totals stop further skin work deterministically. Never sort custom objects by ID or claim to reconstruct upstream RNG state.**
- [ ] **Step 5: Freeze `event_mutation_table_v1.json`**. Permit only bounded session-local presentation changes and explicitly declared skin-configuration writes. During evaluation, presentation writes update a staged in-frame view visible to later skin callbacks/objects; a critical failure discards it. After a successful frame, queue persistence off the render thread. Option/file/resource/model-affecting changes must pass Task 7's worker `prepareActivation`, `SkinCommitCoordinator::submitActivation`, and app-lifetime main-thread polling, and affect only a future session; they never rebuild the current session. Reject score, gauge authority, note state, judgment, filesystem path, general profile, audio, replay, and scene-transition mutation.
- [ ] **Step 6: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target play_skin_state_bridge_tests -j 6
  ctest --test-dir cmake-build-debug -R '^play_skin_state_bridge_tests$' --output-on-failure
  ```

  Expected RED: host modules lack authoritative snapshot mappings.
- [ ] **Step 7: Implement the Task 18 bridge/commit interfaces and traced function/number/string dispatch plus every selector-based `ISkinFrameState` lookup and projection span**. The bridge resolves only built-in selectors; Task 14 resolves typed model registries and invokes Lua-backed property bindings through the runtime. It reads custom timers/events from the validated model and invokes them through the exact runtime/mutation table supplied by its context; configured option/offset lookups come from the frozen configuration. In `PlaySkinSession`, declare model/configuration/runtime before the bridge so reverse destruction releases the bridge first. Accumulate writes in the staged transaction, expose them within the current evaluation, commit only after whole-frame success, discard on failure, and hand persistence intents to a non-render-thread queue.
- [ ] **Step 8: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target play_skin_state_bridge_tests -j 6
  ctest --test-dir cmake-build-debug -R '^play_skin_state_bridge_tests$' --output-on-failure
  ```

  Expected GREEN: values/call counts/order match the committed reference traces and gameplay snapshots remain byte-for-byte unchanged by events.
- [ ] **Step 9: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/beatoraja/PlaySkinStateBridge.h src/skin/beatoraja/PlaySkinStateBridge.cpp src/skin/beatoraja/LuaSkinHostModules.h src/skin/beatoraja/LuaSkinHostModules.cpp src/skin/beatoraja/LuaSkinRuntime.h src/skin/beatoraja/LuaSkinRuntime.cpp src/skin/CMakeLists.txt tests/play_skin_state_bridge_tests.cpp tests/fixtures/beatoraja_skin/traces/scuro_property_frames_v1.json tests/fixtures/beatoraja_skin/event_mutation_table_v1.json
  git commit -m "feat: bridge gameplay state to Beatoraja skins"
  ```

### Task 19: Render BGA directly into the authored skin destination

**Reference refresh:** `SkinBGA.java`, `BGAProcessor.java`, `PlaySkin` BGA registration, destination stretch handling, and target BGA destinations.

**Files:**

- Modify: `../bms-parser-cpp/src/TimeLine.h`
- Modify: `../bms-parser-cpp/src/Parser.cpp`
- Modify: `../bms-parser-cpp/test/main.cpp`
- Generate: `../bms-parser-cpp/build/bms_parser.hpp`
- Generate: `../bms-parser-cpp/build/bms_parser.cpp`
- Replace from generated sibling output: `src/bms_parser.hpp`
- Replace from generated sibling output: `src/bms_parser.cpp`
- Create: `src/audio/GameplayBgaFrame.h`
- Create: `src/audio/GameplayBgaMissStateTracker.h`
- Create: `src/audio/GameplayBgaMissStateTracker.cpp`
- Modify: `src/audio/CMakeLists.txt`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `src/video/VideoPlayer.h`
- Modify: `src/video/VideoPlayer.cpp`
- Create: `shader_src/vs_skin_yuvrgb.sc`
- Create: `shader_src/fs_skin_yuvrgb.sc`
- Generate: `shaders/metal/vs_skin_yuvrgb.bin`
- Generate: `shaders/metal/fs_skin_yuvrgb.bin`
- Generate: `shaders/spirv/vs_skin_yuvrgb.bin`
- Generate: `shaders/spirv/fs_skin_yuvrgb.bin`
- Generate: `shaders/essl/vs_skin_yuvrgb.bin`
- Generate: `shaders/essl/fs_skin_yuvrgb.bin`
- Generate on Windows verification: `shaders/dx11/vs_skin_yuvrgb.bin`
- Generate on Windows verification: `shaders/dx11/fs_skin_yuvrgb.bin`
- Modify: `scripts/verify_skin_shader_outputs.py`
- Modify: `tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json`
- Modify: `tests/ios_artifact_audit_tests.py`
- Modify: `src/skin/beatoraja/SkinDrawCommand.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/scene/play/PlayfieldVisualState.h`
- Modify: `src/scene/play/PlayfieldVisualState.cpp`
- Modify: `src/scene/play/PlayfieldPresentationEvents.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/gameplay_bga_target_tests.cpp`
- Test: `tests/video_frame_layout_tests.cpp`

**Interfaces:**

- Consumes: a submit-ready `SkinCommandBuffer`, `RenderContext`, captured BGA timeline time, and the active Jukebox visual state through injection.
- Produces: `IGameplayBgaSubmitter`, explicit-role `BgaDrawTarget`, one `PreparedGameplayBgaFrame`, and `Skin2DRenderer::preflight/submit` overloads. Evaluation never submits BGA or reaches through global context.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("base and layer roles do not depend on view id") {
  const auto submissions = submitEmbeddedBgaFixture(rendering::ui_view);
  CHECK(submissions[0].role == GameplayBgaRole::Base);
  CHECK(submissions[1].role == GameplayBgaRole::Layer);
}
```

- [ ] **Step 2: Make the RED BGA tests reference explicit roles and this injected preparation/preflight/submission seam**:

  ```cpp
  enum class GameplayBgaRole : std::uint8_t { Base, Layer, Miss };
  enum class GameplayBgaComposition : std::uint8_t {
    Blank,
    MissOnly,
    BaseThenLayer
  };
  struct GameplayBgaPoint {
    float x = 0.0F;
    float y = 0.0F;
  };

  enum class GameplayBgaMediaKind : std::uint8_t { Image, Video };

  struct PreparedGameplayBgaSurface {
    GameplayBgaRole role = GameplayBgaRole::Base;
    GameplayBgaMediaKind mediaKind = GameplayBgaMediaKind::Image;
    std::uint64_t surfaceToken = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
  };

  struct PreparedGameplayBgaFrame {
    std::uint64_t sequence = 0;
    GameplayBgaComposition composition = GameplayBgaComposition::Blank;
    std::optional<PreparedGameplayBgaSurface> base;
    std::optional<PreparedGameplayBgaSurface> layer;
    std::optional<PreparedGameplayBgaSurface> miss;
  };

  struct GameplayBgaMissState {
    bool active = false;
    std::int64_t startedBgaMicros = 0;
    std::int64_t durationMicros = 500'000;
    std::uint64_t triggerSerial = 0;
  };

  class GameplayBgaMissStateTracker {
  public:
    void onJudge(JudgeResult, int resultingCombo,
                 PlayfieldJudgeEventClock) noexcept;
    GameplayBgaMissState snapshot() const noexcept;
    void reset() noexcept;
  };

  // Task 19 appends this exact value member to Task 16's captured DTO; it is
  // not held out-of-band and contains no tracker pointer:
  //   GameplayBgaMissState PlayfieldVisualState::bgaMiss;

  inline constexpr std::int64_t kDefaultMissLayerDurationMicros = 500'000;

  // Added upstream in ../bms-parser-cpp, then copied only through its
  // amalgamation output. One value is retained for every authored cell.
  namespace bms_parser {
  inline constexpr int BgaSequenceBlank = -1;
  struct BgaPoorSequence { std::vector<int> Frames; };
  // TimeLine::BgaPoor becomes optional<BgaPoorSequence> and is present only
  // on that measure's position-zero timeline.
  }

  struct BgaPreflightResult {
    bool ready = false;
    std::optional<SkinDiagnostic> failure;
  };

  struct BgaDrawTarget {
    GameplayBgaRole role;
    std::uint16_t viewId = 0;
    std::array<GameplayBgaPoint, 4> destination;
    SkinStretchMode stretch = SkinStretchMode::Stretch;
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
    SkinBlendMode blend;
    std::optional<UiLogicalRect> clip;
    std::uint32_t authoredOrdinal = 0;
  };

  class IGameplayBgaSubmitter {
  public:
    virtual ~IGameplayBgaSubmitter() = default;
    virtual PreparedGameplayBgaFrame prepareVisualFrameAt(
        std::uint64_t frameSerial, std::int64_t micro,
        const GameplayBgaMissState &) = 0;
    virtual BgaPreflightResult preflight(
        const PreparedGameplayBgaFrame &,
        std::span<const BgaDrawTarget>) = 0;
    virtual void submitPrepared(const PreparedGameplayBgaFrame &,
                                const BgaDrawTarget &) noexcept = 0;
    virtual void submitFullscreen(const PreparedGameplayBgaFrame &) noexcept = 0;
  };
  ```

  Make `Jukebox` implement the interface. Replace `Skin2DRenderer::submit` with `preflight` plus a no-fail submission plan that receives `IGameplayBgaSubmitter &`; retain `Jukebox::render()` as the built-in fullscreen delegate. Shader/type selection comes from `GameplayBgaRole` plus `GameplayBgaMediaKind`, never from the bgfx view ID.
- [ ] **Step 3: In the sibling parser, write RED tests before production changes**. Require `#00106:01000200` without `#BMP00` to produce `[1,-1,2,-1]`, and with defined `#BMP00` to produce `[1,0,2,0]`; retain repeated rows such as `01000100`; make the last active channel-06 row win within a measure; keep successive measures separate at their position-zero timelines; turn undefined nonzero IDs into `BgaSequenceBlank` without registering them; register every defined referenced BMP including `#BMP00`; and create no fractional timelines for channel 06. Direct and amalgamated tests must assert identical results.
- [ ] **Step 4: Run the parser RED check**

  ```sh
  cd ../bms-parser-cpp
  make clean
  make test
  ```

  Expected RED: scalar `TimeLine::BgaPoor` and skipped `00` cells cannot satisfy the new sequence assertions.
- [ ] **Step 5: Write Aso BGA tests first for pinned composition/playback**. This milestone deliberately uses Beatoraja's `500'000`-microsecond default as one fixed compatibility value; no setting advertises a configurable miss-layer duration, and the compatibility contract records that limitation. `GameplayBgaMissStateTracker` is the single reusable owner of trigger state: match pinned `BMSPlayer.update` after a real committed judgment (exclude the no-judgment/None sentinel), trigger whenever the resulting `combo` argument is zero, including repeated KPOOR-at-zero cases, and do not substitute a generic `isComboBreak()` predicate. It copies `PlayfieldJudgeEventClock::bgaTimeMicros`, stores the fixed default duration, and increments `triggerSerial`; visual offset must not affect it. `PlayfieldVisualStateStore` owns one tracker and copies its snapshot into each frame; `ReplayVideoExporter` owns another and feeds it the exact exported judge clocks. Preserve pinned `misslayertime != 0` behavior: a trigger at BGA time zero does not activate the overlay, and record this sentinel quirk in the compatibility contract/manifest. The active interval is otherwise start-inclusive/end-exclusive. Match pinned integer indexing exactly: for `N > 0`, choose `floor((N - 1) * elapsed / duration)`; `N == 0` draws nothing and `N == 1` always chooses zero. Do not clamp into a last frame that pinned Beatoraja never reaches before the end-exclusive boundary. For four frames, duration `500000`, and start `1`, assert absolute times/indices `1→0`, `166667→0`, `166668→1`, `333334→1`, `333335→2`, `500000→2`, and `500001→inactive`; index 3 is unreachable. The active chart sequence is the latest measure-start sequence at or before current BGA time; recompute on backward seek, and switch to a newly crossed sequence even while an overlay is active. Replay/catch-up use recorded event time. No current sequence uses `BaseThenLayer`; a current sequence whose selected frame is blank/unmaterializable stays `MissOnly` with zero draws and no base/layer fallback.
- [ ] **Step 6: Add pure target/submission tests** for unprepared/blank, exclusive miss, and normal base/layer composition. Preflight expands one role-free `SkinBgaCommand` atomically into zero, one, or two explicit-role `BgaDrawTarget`s at the command's exact authored ordinal. `BaseThenLayer` with no base draws the black base placeholder before its optional layer. The built-in 1×1 black surface used for `Blank` or a missing base bypasses media aspect/stretch calculation and always fills the command's entire evaluated authored destination (including rotation/viewport); Fit/NoExpand must never shrink it to a square or pixel. Materialized base/layer/miss surfaces apply the preserved `SkinStretchMode` only after their independent dimensions are known, using Task 12 to produce final vertices/trimmed UVs. Assert image base uses linear type, image layer uses layer type, video base/layer both use video/FFmpeg type, and miss uses LINEAR even when its resource is a movie, matching the pinned processor. The embedded YUV path must accept arbitrary four-point destinations, one authored uniform RGBA tint/brightness replicated identically to all four vertices, clip, and blend without changing the legacy fullscreen shader/output; keep `BgaDrawTarget::tint` as `std::array<float, 4>` and test shader selection plus one known YUV×tint conversion. Also cover mixed image/video roles, all audited stretch modes, negative/zero dimensions, explicit authored order, reset/unload, one video update across multiple submissions, and blank/miss/video preflight failure handed to Task 21 fullscreen fallback with the same prepared sequence/timestamp.
- [ ] **Step 6a: Add the captured-state propagation regression**: update the store's tracker before capture, copy the snapshot into `PlayfieldVisualState::bgaMiss`, and prove event fan-out → store → capture → presentation coordinator carries the exact start/duration/serial with no out-of-band tracker lookup.
- [ ] **Step 7: Run the Aso RED check**

  ```sh
  cmake --build cmake-build-debug --target gameplay_bga_target_tests video_frame_layout_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(gameplay_bga_target_tests|video_frame_layout_tests)$' --output-on-failure
  python3 -m unittest tests/ios_artifact_audit_tests.py -v
  ```

  Expected RED: scalar parser data and mutable fullscreen destinations cannot satisfy sequence/target assertions.
- [ ] **Step 8: Implement the parser change only in `../bms-parser-cpp`**. Replace scalar `TimeLine::BgaPoor` with `optional<BgaPoorSequence>`. Treat each active channel-06 row as one sequence on measure position zero, preserve every cell/order, make last row win, map defined `#BMP00` to resource `0`, map unresolved cells to `BgaSequenceBlank`, and do not collapse repeated IDs. Then run the mandated workflow, review generated diffs, and commit/push that sibling repository before copying outputs:

  ```sh
  cd ../bms-parser-cpp
  make clean
  make test
  make test_amalgamation
  git diff --check
  git add src/TimeLine.h src/Parser.cpp test/main.cpp
  git commit -m "feat: preserve BMS poor BGA sequences"
  git push
  cp build/bms_parser.hpp ../AsoBMaShow/src/bms_parser.hpp
  cp build/bms_parser.cpp ../AsoBMaShow/src/bms_parser.cpp
  ```

  Expected GREEN: direct and amalgamated parser tests pass, and the copied Aso files exactly match the generated siblings.
- [ ] **Step 8b: Wire the render-neutral production surface**. Add `GameplayBgaMissStateTracker.cpp` to `src/audio/CMakeLists.txt`; root CMake additions alone own only focused tests. Make `GameplayBgaFrame.h` include Task 3's unconditional `SkinPresentationTypes.h`, not the enabled-only model header, and extend the Android-OFF feature-gate check to compile the BGA DTO and `Jukebox` interface.
- [ ] **Step 9: Implement the Task 19 tracker/BGA interfaces and playback contract**. Keep the current chart sequence schedule separate from miss trigger state; a judge updates only the tracker's start/serial. `PlayfieldVisualStateStore::onJudge` updates its tracker before capture. Make `Jukebox` choose the active sequence by captured BGA time (including backward seek), select the duration frame, then choose blank/miss/base-layer composition. Refactor `VideoPlayer` to accept an explicit target at submission. During preflight, expand each role-free command using prepared per-role dimensions: materialized media goes through Task 12's source-aware stretch/UV projection, while black placeholders use the raw evaluated destination fill path. `ReplayVideoExporter` remains built-in-only but replaces legacy `renderVisualsAt` BGA submission with `prepareVisualFrameAt(frameSerial, exportedBgaTime, exporterTracker.snapshot())` plus `submitFullscreen` exactly once; gameplay and exporter tests require identical miss behavior. Add separate `vs_skin_yuvrgb.sc`/`fs_skin_yuvrgb.sc` variants using a position/UV/color layout so embedded YUV frames multiply tint and honor authored quad geometry; construct all four vertex colors from the one uniform `BgaDrawTarget::tint`, and retain the current shaders/layout for fullscreen output. Remove public mutable destination fields while preserving decode/update lifecycle and actual per-role source dimensions.
- [ ] **Step 10: Make embedded skin mode submit the chosen composition contiguously at the BGA command's exact authored ordinal: a built-in 1×1 black surface for `Blank`; only the miss target for `MissOnly`, with an unavailable miss producing no draw; or a real base or black base placeholder followed by the optional layer for `BaseThenLayer`**. `prepareVisualFrameAt(frameSerial, capturedBgaTime, missState)` updates video at most once and returns a value object; `submitPrepared` and `submitFullscreen` never update/decode and may reuse that same object after preflight failure. Do not sample `BlurPass::outputTexture()` because that texture has already been fitted to the screen and would double-transform authored aspect.
- [ ] **Step 11: Do not switch the global compositor in this task; Task 21 owns the per-frame decision after coordinator evaluation**. Ensure BGA preparation/preflight happens after whole-buffer evaluation but before the first skin submit, and make successful planned submission no-fail. Apply `bgaBrightnessPercent` as embedded tint; embedded-mode global blur remains a non-error compatibility status.
- [ ] **Step 12: Compile the new shaders, then run the GREEN check**

  ```sh
  cd shader_src && SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py
  cd ..
  python3 scripts/verify_skin_shader_outputs.py --shader skin_yuvrgb --require-backends metal,spirv,essl
  cd ../bms-parser-cpp && make test && make test_amalgamation
  cmp build/bms_parser.hpp ../AsoBMaShow/src/bms_parser.hpp
  cmp build/bms_parser.cpp ../AsoBMaShow/src/bms_parser.cpp
  cd ../AsoBMaShow
  cmake --build cmake-build-debug --target gameplay_bga_target_tests video_frame_layout_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(gameplay_bga_target_tests|video_frame_layout_tests)$' --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  python3 scripts/verify_skin_shader_outputs.py --shader skin_quad --shader skin_yuvrgb --require-backends metal,spirv,essl,dx11 --manifest tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json
  python3 -m unittest tests/ios_artifact_audit_tests.py -v
  scripts/ios_release_verify.sh
  ```

  Before this GREEN gate, generate the DX11 pair in the same clean Windows checkout/procedure specified by Task 15, then rewrite the single manifest with both `--shader skin_quad --shader skin_yuvrgb --require-backends metal,spirv,essl,dx11 --write-manifest ...`; rerun read-only `--manifest` verification on both Windows and macOS. Expected GREEN: all sixteen exact skin shader binaries match their source/output hashes, and parser sequence, source aspect/layer order, miss timing, and built-in fullscreen behavior all pass.
- [ ] **Step 13: Commit the AsoBMaShow task**

  ```sh
  git add CMakeLists.txt src/bms_parser.hpp src/bms_parser.cpp src/audio/GameplayBgaFrame.h src/audio/GameplayBgaMissStateTracker.h src/audio/GameplayBgaMissStateTracker.cpp src/audio/Jukebox.h src/audio/Jukebox.cpp src/video/VideoPlayer.h src/video/VideoPlayer.cpp shader_src/vs_skin_yuvrgb.sc shader_src/fs_skin_yuvrgb.sc shaders/metal/vs_skin_yuvrgb.bin shaders/metal/fs_skin_yuvrgb.bin shaders/spirv/vs_skin_yuvrgb.bin shaders/spirv/fs_skin_yuvrgb.bin shaders/essl/vs_skin_yuvrgb.bin shaders/essl/fs_skin_yuvrgb.bin shaders/dx11/vs_skin_yuvrgb.bin shaders/dx11/fs_skin_yuvrgb.bin scripts/verify_skin_shader_outputs.py tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json tests/ios_artifact_audit_tests.py src/skin/beatoraja/SkinDrawCommand.h src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp src/scene/play/PlayfieldPresentationEvents.h src/scene/play/PlayfieldVisualState.h src/scene/play/PlayfieldVisualState.cpp src/scene/play/GamePlayScene.cpp src/ReplayVideoExporter.cpp tests/gameplay_bga_target_tests.cpp tests/video_frame_layout_tests.cpp
  git add src/audio/CMakeLists.txt
  git commit -m "feat: embed BGA in gameplay skins"
  ```

### Task 20: Generalize realtime touch and lane-cover interaction to authored geometry

**Reference refresh:** `PlaySkin.getLaneRegion`, `PlaySkin.getLaneGroupRegion`, JSON play skin lane-region construction, `SkinSlider.java`, `Skin.mousePressed`, `Skin.mouseDragged`, `FloatWriter`, lane-cover slider behavior, and target lane/note/slider destinations.

**Files:**

- Modify: `src/scene/play/RealtimeTouchInputRouter.h`
- Modify: `src/scene/play/RealtimeTouchInputRouter.cpp`
- Modify: `src/scene/play/PlayfieldPresentation.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `CMakeLists.txt`
- Test: `tests/realtime_touch_input_router_tests.cpp`
- Test: `tests/play_skin_touch_geometry_tests.cpp`

**Interfaces:**

- Consumes: `PlaySkinViewport::uiToAuthored`, ordered authored destination geometry, slider/writer bindings, and the existing realtime touch sink.
- Produces: ordered `RealtimeTouchLayout`, `PresentationUiHit hitTestUiControl(UiLogicalPoint)`, begin/update/end/cancel presentation-touch methods, and `SkinWriterInvocation {writer, normalizedValue, eventMicros}`. Skin controls hit-test topmost-first by reverse authored order.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("topmost slider captures and invokes its writer") {
  const auto result = dragOverlappingSliderFixture();
  CHECK(result.capturedOrdinal == 9);
  CHECK(result.invocations == 1);
}
```

- [ ] **Step 2: Make the RED touch tests reference these ordered authored regions, slider/writer targets, and result types while preserving the built-in helper**:

  ```cpp
  struct RealtimeTouchLaneRegion {
    int lane = -1;
    bool scratch = false;
    RealtimeTouchPoint bottomLeft;
    RealtimeTouchPoint bottomRight;
    RealtimeTouchPoint topLeft;
    RealtimeTouchPoint topRight;
  };

  struct RealtimeTouchLayout {
    std::vector<RealtimeTouchLaneRegion> regions;
    int keyMode = 7;
    bool dragMode = false;
  };

  enum class SkinInteractionKind : std::uint8_t {
    Lane,
    LaneCover,
    Slider
  };

  struct SkinInteractionRegion {
    SkinInteractionKind kind;
    std::uint32_t authoredOrdinal = 0;
    AuthoredRect bounds;
    std::optional<SkinFloatWriterId> writer;
    std::uint8_t direction = 0; // 0 up, 1 right, 2 down, 3 left
    double minimum = 0.0;
    double maximum = 1.0;
  };

  struct SkinWriterInvocation {
    SkinFloatWriterId writer{};
    float normalizedValue = 0.0F;
    long long eventMicros = 0;
  };

  enum class PresentationUiControlKind : std::uint8_t {
    None,
    LaneCover,
    Slider,
    NativeOverlay
  };

  struct PresentationUiHit {
    PresentationUiControlKind kind = PresentationUiControlKind::None;
    std::uint32_t authoredOrdinal = 0;
    std::optional<SkinFloatWriterId> writer;
  };

  struct UiLogicalPoint {
    float x = 0.0F;
    float y = 0.0F;
  };

  struct PresentationTouchEvent {
    long long pointerId = 0;
    UiLogicalPoint uiPoint;
    long long eventMicros = 0;
  };

  struct PresentationTouchResult {
    bool consumed = false;
    bool excludeFromGameplay = false;
  };
  ```

- [ ] **Step 3: Extend router tests first for unequal widths, left/right scratch placement, perspective quads, gaps, overlap priority by authored region order, edge ownership, pointer cancellation during layout switch, and unchanged built-in uniform mapping.** Instantiate concrete `BMSRenderer` through `PlayfieldPresentation &` after adding the pure touch surface: it returns the existing uniform `RealtimeTouchLayout`, reports no skin UI hit, returns unconsumed for presentation-control begin/update/end, and accepts cancel as a no-op, so built-in gameplay touch remains owned by the router.
- [ ] **Step 4: Add skin geometry tests for authored lane destinations transformed by Fit/Stretch/Custom, normalized-screen to UI-logical to authored inverse round trip, 2x HiDPI and safe-area offsets, failed matrix inversion disabling interaction, lane-cover hit/grab/drag, slider directions 0–3, endpoint snapping, non-changeable sliders, pointer capture, typed float-writer invocation/failure under the callback budget with transaction discard, reverse-authored-order overlap priority, cancellation on layout/session switch, and native UI regions excluded from gameplay.** Assert there is no generic standalone Writer interaction: Task 11 diagnoses direct image click/text-edit input outside the audited v1 gameplay scope.
- [ ] **Step 5: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target realtime_touch_input_router_tests play_skin_touch_geometry_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(realtime_touch_input_router_tests|play_skin_touch_geometry_tests)$' --output-on-failure
  ```

  Expected RED: the router assumes one trapezoid divided evenly.
- [ ] **Step 6: Implement the Task 20 authored-region/writer/result interfaces and exact `PlayfieldPresentation` touch methods**. Keep `SkinFloatWriterId` in Task 3's unconditional render-neutral package types header; `PlayfieldPresentation.h`/`BMSRenderer` must compile in Android-OFF builds without including `BeatorajaSkinModel.h`, enforced by the feature-gate source test. Add every new pure virtual override to `BMSRenderer.h/.cpp` using the existing built-in uniform layout and the no-skin-control behavior tested above, keeping it concrete. Convert normalized input with `rendering::normalizedToUi`, apply `PlaySkinViewport::uiToAuthored`, and queue bounded `SkinWriterInvocation` records for Task 21 to process at the next frame boundary through Task 18's rollbackable transaction. Remove GamePlayScene access to renderer-private touch/lane-cover geometry; writer callbacks never mutate authority or settings directly.
- [ ] **Step 7: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target realtime_touch_input_router_tests play_skin_touch_geometry_tests gameplay_simulation_tests realtime_gameplay_worker_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(realtime_touch_input_router_tests|play_skin_touch_geometry_tests|gameplay_simulation_tests|realtime_gameplay_worker_tests)$' --output-on-failure
  ```

  Expected GREEN: current built-in touch remains identical and skin input follows authored geometry.
- [ ] **Step 8: Commit the task**

  ```sh
  git add CMakeLists.txt src/scene/play/RealtimeTouchInputRouter.h src/scene/play/RealtimeTouchInputRouter.cpp src/scene/play/PlayfieldPresentation.h src/scene/play/GamePlayScene.cpp src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp src/skin/beatoraja/Skin2DRenderer.h tests/realtime_touch_input_router_tests.cpp tests/play_skin_touch_geometry_tests.cpp
  git commit -m "feat: route touch through skin lane geometry"
  ```

### Task 21: Integrate sessions and an atomic built-in/skin presentation coordinator

**Reference refresh:** `LuaSkinLoader.load`, `Skin.updateCustomObjects`, `Skin.prepare`, `Skin.draw`, `BMSPlayer` scene timing, `LaneRenderer`, and default `play7.luaskin`/`play7main.lua`.

**Files:**

- Create: `src/skin/beatoraja/PlaySkinSession.h`
- Create: `src/skin/beatoraja/PlaySkinSession.cpp`
- Create: `src/skin/beatoraja/GameplaySkinValidator.h`
- Create: `src/skin/beatoraja/GameplaySkinValidator.cpp`
- Create: `src/skin/beatoraja/SkinDiagnosticHistory.h`
- Create: `src/skin/beatoraja/SkinDiagnosticHistory.cpp`
- Create: `src/skin/SkinConfigurationWriteQueue.h`
- Create: `src/skin/SkinConfigurationWriteQueue.cpp`
- Create: `src/scene/play/PlayfieldPresentationCoordinator.h`
- Create: `src/scene/play/PlayfieldPresentationCoordinator.cpp`
- Modify: `src/scene/play/PlayfieldPresentation.h`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `src/skin/package/SkinPackageCatalog.h`
- Modify: `src/skin/package/SkinPackageCatalog.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/SceneManager.h`
- Modify: `src/scene/SceneManager.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/play_skin_session_tests.cpp`
- Test: `tests/playfield_presentation_coordinator_tests.cpp`
- Test: `tests/gameplay_skin_integration_tests.cpp`

**Interfaces:**

- Consumes: typed presentation config/state/projection, a `ValidatedSkinActivation` carrying reconciled profile settings plus digest, one app-owned `SkinResourcePreparationService`, `IGameplayBgaSubmitter`, writer invocations, a scene-independent `SkinCommitCoordinator`, and exact gameplay-event timestamps.
- Produces: `PlaySkinSession::create(ValidatedSkinActivation, PlaySkinSessionContext)`, `SkinWriterResult invokeWriter(const SkinWriterInvocation &)`, and `PlayfieldPresentationCoordinator final : PlayfieldPresentation`. Its per-frame result—not selected settings or `activeMode()`—is the sole input to global BGA compositing.

  ```cpp
  enum class PresentationMode : std::uint8_t { BuiltIn, Skin };
  enum class PresentationFrameOutcome : std::uint8_t {
    Ready,
    RecoverableFailure,
    CriticalFailure
  };
  enum class GameplayBgaCompositeMode : std::uint8_t {
    FullscreenBuiltIn,
    EmbeddedSkin
  };

  struct PresentationFailure {
    SkinEntryId entry;
    std::string revisionDigest;
    std::string configurationDigest;
    SkinDiagnostic diagnostic;
    std::uint64_t frameSerial = 0;
  };

  class PlaySkinSession;
  class SkinDiagnosticHistory;
  class SkinConfigurationWriteQueue;
  struct PresentationFrameResult;

  struct PlaySkinSessionContext {
    std::uint64_t sessionSerial = 0;
    SkinProfileId profileId;
    const PlayfieldChartVisualModel &chartModel;
    ViewportSettings viewport;
    SkinStorageRoots storageRoots;
    SkinResourcePreparationService &resourcePreparation;
    SkinDiagnosticHistory &diagnosticHistory;
    SkinConfigurationWriteQueue &configurationWrites;
    std::stop_token stop;
  };

  struct GameplaySkinActivationRequest {
    std::uint64_t sessionSerial = 0;
    SkinProfileId profileId;
    ValidatedSkinActivation activation;
    ViewportSettings viewport;
  };
  using AcquireGameplaySkinForNextChart =
      std::function<std::optional<GameplaySkinActivationRequest>()>;

  struct PlaySkinSessionCreateResult {
    std::unique_ptr<PlaySkinSession> session;
    EntryProfileSettings reconciledSettings;
    std::string configurationDigest;
    bool cancelled = false;
    std::vector<SkinDiagnostic> diagnostics;
  };

  struct PlaySkinSessionIdentity {
    std::uint64_t sessionSerial = 0;
    SkinProfileId profileId;
    SkinEntryId entry;
    std::string revisionDigest;
    std::string configurationDigest;
  };

  enum class SkinWriterDisposition : std::uint8_t {
    Applied,
    Rejected
  };
  struct SkinWriterResult {
    SkinWriterDisposition disposition = SkinWriterDisposition::Rejected;
    std::vector<SkinFrameMutation> orderedMutations;
    std::optional<SkinDiagnostic> diagnostic;
  };

  struct SkinConfigurationWriteRequest {
    std::uint64_t sessionSerial = 0;
    SkinProfileId profileId;
    SkinEntryId entry;
    std::string expectedRevisionDigest;
    std::string expectedConfigurationDigest;
    std::uint64_t frameSerial = 0;
    std::vector<PersistedSkinConfigurationWrite> orderedWrites;
  };
  enum class SkinConfigurationEnqueueResult : std::uint8_t {
    Enqueued, QueueFull, Closed
  };
  class SkinConfigurationWriteQueue {
  public:
    static constexpr std::size_t maxPending = 256;
    SkinConfigurationEnqueueResult enqueue(
        SkinConfigurationWriteRequest) noexcept;
    std::vector<SkinConfigurationWriteRequest> drain();
    void close() noexcept;
  };

  enum class SkinDiagnosticPhase : std::uint8_t {
    Import, Scan, Validation, Session, FrameFallback
  };
  struct SkinDiagnosticHistoryRecord {
    std::uint64_t recordSerial = 0;
    SkinEntryId entry;
    std::string revisionDigest;
    std::string configurationDigest;
    SkinDiagnosticPhase phase = SkinDiagnosticPhase::Validation;
    SkinDiagnostic diagnostic;
    std::optional<std::uint32_t> luaLine;
    std::optional<std::uint64_t> frameSerial;
  };
  class SkinDiagnosticHistory {
  public:
    explicit SkinDiagnosticHistory(SkinPackageCatalog &);
    ~SkinDiagnosticHistory();
    void append(SkinDiagnosticHistoryRecord);
    std::vector<SkinDiagnosticHistoryRecord> records() const;
    std::vector<SkinDiagnosticHistoryRecord>
    recordsFor(const SkinEntryId &) const;
    void flush();
    static constexpr std::size_t maxGlobalRecords = 256;
    static constexpr std::size_t maxRecordsPerEntry = 32;
  };

  // Task 7 predeclares the catalog's diagnostic-history members using the
  // forward-declared record; Task 21 completes the record and implements them.

  class GameplaySkinValidator final : public SkinEntryValidator {
  public:
    explicit GameplaySkinValidator(SkinResourcePreparationService &);
    SkinValidationResult validate(
        SkinRevisionReadView, const SkinEntryId &,
        const EntryProfileSettings *, std::stop_token) override;
  };

  class PlaySkinSession {
  public:
    static PlaySkinSessionCreateResult create(
        ValidatedSkinActivation, PlaySkinSessionContext);
    PresentationFrameOutcome prepareFrame(
        const PlayfieldVisualState &, const PlayfieldProjectionResult &);
    PresentationFrameResult render(RenderContext &,
                                   const PreparedGameplayBgaFrame &,
                                   IGameplayBgaSubmitter &);
    void setViewport(ViewportSettings);
    const PlaySkinSessionIdentity &identity() const noexcept;
    SkinWriterResult invokeWriter(const SkinWriterInvocation &);
    void onLanePressed(int, JudgeResult, long long);
    void onLaneReleased(int, long long);
    void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock, bool);
    RealtimeTouchLayout touchLayout() const;
    PresentationUiHit hitTestUiControl(UiLogicalPoint) const;
    PresentationTouchResult beginPresentationTouch(
        const PresentationTouchEvent &);
    PresentationTouchResult updatePresentationTouch(
        const PresentationTouchEvent &);
    PresentationTouchResult endPresentationTouch(
        const PresentationTouchEvent &, bool cancelled);
    void cancelPresentationTouches(long long eventMicros);
  };

  struct PresentationFrameResult {
    std::uint64_t frameSerial = 0;
    PresentationFrameOutcome outcome = PresentationFrameOutcome::Ready;
    PresentationMode submittedMode = PresentationMode::BuiltIn;
    GameplayBgaCompositeMode bgaCompositeMode =
        GameplayBgaCompositeMode::FullscreenBuiltIn;
    std::optional<PreparedGameplayBgaFrame> preparedBga;
    std::optional<PresentationFailure> failure;
  };

  struct GameplayBgaCompositeState {
    std::uint64_t frameSerial = 0;
    GameplayBgaCompositeMode mode =
        GameplayBgaCompositeMode::FullscreenBuiltIn;
    // Prepared exactly once by the gameplay coordinator. main.cpp reuses this
    // value for fullscreen fallback; submit never updates video a second time.
    std::optional<PreparedGameplayBgaFrame> prepared;
  };

  class PlayfieldPresentation : public IPlayfieldPresentationEvents {
  public:
    virtual ~PlayfieldPresentation() = default;
    virtual void configure(const PlayfieldPresentationConfig &) = 0;
    virtual PresentationFrameOutcome prepareFrame(
        const PlayfieldVisualState &,
        const PlayfieldProjectionResult &) = 0;
    virtual PresentationFrameResult render(RenderContext &) = 0;
    virtual RealtimeTouchLayout touchLayout() const = 0;
    virtual PresentationUiHit hitTestUiControl(UiLogicalPoint) const = 0;
    virtual PresentationTouchResult beginPresentationTouch(
        const PresentationTouchEvent &) = 0;
    virtual PresentationTouchResult updatePresentationTouch(
        const PresentationTouchEvent &) = 0;
    virtual PresentationTouchResult endPresentationTouch(
        const PresentationTouchEvent &, bool cancelled) = 0;
    virtual void cancelPresentationTouches(long long eventMicros) = 0;
    virtual void reset() = 0;
    virtual void refreshGeometry() = 0;
    virtual PresentationMode activeMode() const noexcept = 0;
    virtual std::optional<PresentationFailure> lastFailure() const = 0;
  };

  enum class GameplayViewportPersistenceDisposition : std::uint8_t {
    Queued,
    Deferred,
    Rejected
  };
  struct GameplayViewportPersistenceResult {
    GameplayViewportPersistenceDisposition disposition =
        GameplayViewportPersistenceDisposition::Rejected;
    std::optional<SkinDiagnostic> diagnostic;
  };
  using PersistGameplayViewport = std::function<
      GameplayViewportPersistenceResult(
          const PlaySkinSessionIdentity &, ViewportSettings)>;

  struct PlayfieldPresentationCoordinatorDependencies {
    std::unique_ptr<PlayfieldPresentation> builtIn;
    std::unique_ptr<PlaySkinSession> skin;
    IGameplayBgaSubmitter &bga;
    PersistGameplayViewport persistViewport;
    std::function<void(const PresentationFailure &)> recordFailure;
  };

  class PlayfieldPresentationCoordinator final
      : public PlayfieldPresentation {
  public:
    explicit PlayfieldPresentationCoordinator(
        PlayfieldPresentationCoordinatorDependencies);
    ~PlayfieldPresentationCoordinator() override;
    void installSkinSession(std::unique_ptr<PlaySkinSession>);
    void clearSkinSession() noexcept;
    // Native overlay action: immediately changes current render/touch geometry
    // to Fit and queues a profile-only durable change for future charts.
    bool resetLayoutToFit();
    void configure(const PlayfieldPresentationConfig &) override;
    PresentationFrameOutcome prepareFrame(
        const PlayfieldVisualState &,
        const PlayfieldProjectionResult &) override;
    PresentationFrameResult render(RenderContext &) override;
    RealtimeTouchLayout touchLayout() const override;
    PresentationUiHit hitTestUiControl(UiLogicalPoint) const override;
    PresentationTouchResult beginPresentationTouch(
        const PresentationTouchEvent &) override;
    PresentationTouchResult updatePresentationTouch(
        const PresentationTouchEvent &) override;
    PresentationTouchResult endPresentationTouch(
        const PresentationTouchEvent &, bool cancelled) override;
    void cancelPresentationTouches(long long eventMicros) override;
    void onLanePressed(int, JudgeResult, long long) override;
    void onLaneReleased(int, long long) override;
    void onJudge(JudgeResult, int, int,
                 PlayfieldJudgeEventClock, bool) override;
    void reset() override;
    void refreshGeometry() override;
    PresentationMode activeMode() const noexcept override;
    std::optional<PresentationFailure> lastFailure() const override;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("critical skin failure renders one built-in frame") {
  const auto frame = renderCriticalFailureFixture();
  CHECK(frame.submittedMode == PresentationMode::BuiltIn);
  CHECK(frame.bgaCompositeMode == GameplayBgaCompositeMode::FullscreenBuiltIn);
}
```

- [ ] **Step 2: Make the RED session tests reference the exact `GameplaySkinValidator`, `PlaySkinSessionCreateResult`, `PlaySkinSession::create`, vector-valued writer result, and diagnostic-history APIs above.** One float-writer callback may stage multiple session-local and persisted mutations in interleaved authored order: `Applied` returns one ordered discriminated `SkinFrameMutation` vector after the callback transaction succeeds, while any failure returns `Rejected` with it empty. Validator calls run synchronously against either a prepared or published `SkinRevisionReadView` and retain nothing from it. Session creation must run header and configured phases in one fresh state, reconcile `activation.reconciledSettings`, and reject a resulting digest that differs from `activation.configurationDigest`; no borrowed `BeatorajaSkinConfiguration` crosses the store/session boundary.
- [ ] **Step 3: Make the RED coordinator tests reference the three outcomes and exact `PlayfieldPresentation` surface, including the Task 16 fan-out sink transition and shared config/state/projection consumption. Require concrete coordinator implementations of all three inherited event methods and concrete `BMSRenderer` overrides for `configure`, state/projection `prepareFrame`, result-returning `render`, touch methods, `reset`, `refreshGeometry`, `activeMode`, and `lastFailure`; the built-in returns Ready/BuiltIn/fullscreen/no failure through Task 17's adapter and must not remain abstract. Every render result, including early critical-failure paths, must carry the exact matched state/projection `frameSerial`.**
- [ ] **Step 4: Make the RED history tests require a `SkinPackageCatalog &`-bound history that reloads after a reconstructed app/catalog, asynchronously coalesces `replaceDiagnosticHistory` writes off the render thread, flushes on orderly shutdown, and preserves a ring of 256 records globally and 32 per entry with the exact safe fields and post-session visibility.** Mutate/free the caller vector immediately after `replaceDiagnosticHistory(span)` and prove the catalog worker persisted its deep copy. Require `history.flush()` to finish its producer queue, followed by separately owned `catalog.flush()`/`catalog.shutdown()` durability and join; no coordinator may close the catalog first.
- [ ] **Step 5: Write validator/session tests first for callback-free header metadata/declaration conversion into `SkinValidationResult`, prepared-staging validation with no retained owner/path, fresh-state activation, a nonzero process-monotonic `sessionSerial` retained in identity and every writer batch, internally derived and isolated profile/entry overlay roots (no raw-path injection), independent cloned revision pins for session and uploaded resource catalog across Files edits/deletion, final-clone GC release, resource/session teardown thread, no hot reload, prebuilt glyph coverage from the chart model's exact runtime-string list, frame callback budgets, optional object disable, critical note/callback failure, pending event discard, one bounded nonblocking configuration-write batch enqueued only after whole-frame success, two same-frame writes preserving authored order under one expected digest, queue-full diagnostics, and deduplicated file/line diagnostics.**
- [ ] **Step 6: Write coordinator/integration tests first for built-in-only default, valid selected 7-key activation, full static-config/runtime-state forwarding, press/release/judge fan-out, skin command-buffer submission, failure before first command, failure after earlier command evaluation, buffer discard, built-in render and global BGA in the same frame, failed session disabled for the rest of the chart, next-chart retry only after validated activation, embedded/fullscreen BGA mode switch, native overlay ordering, no gameplay state mutation, touch layout switching/cancellation, and durable bounded diagnostics after teardown.** Install the coordinator as the Task 16 fan-out's presentation sink and prove one live/catch-up/replay event updates the state store exactly once, the warmed built-in presentation exactly once, and the active skin session exactly once; after skin failure, the built-in remains current without rebinding or duplicate delivery. Exercise the exact `PlaySkinSession` touch layout/hit/begin/update/end/cancel methods: coordinator forwards to the skin only while skin mode is live, forwards to built-in otherwise, and cancels every captured skin pointer before failure fallback, `clearSkinSession`, replacement, reset, or its destructor tears the coordinator down. No event is delivered to both touch targets. On a successful skin frame assert exactly zero built-in lane/playfield/HUD submissions; “native overlays” means only app/system chrome such as pause and Reset Layout. Assert `PlaySkinSession::identity()` is immutable and supplies the exact profile/entry/revision/configuration identity to Reset Layout. Assert a current activation whose stored viewport is Fit but whose `PlaySkinSessionContext.viewport` is Custom renders/touch-maps with Custom: the context value is the sole chart viewport input and must overwrite any stale viewport in `activation.reconciledSettings`. Tap native Reset Layout and prove the current session changes to Fit immediately and the persistence callback receives that identity/Fit exactly once. `Deferred` retains a bounded lifecycle-owned retry and reports status; `Rejected` appends a diagnostic, but neither outcome reverts current Fit, hides the overlay, nor corrupts gameplay authority. Also assert `SceneManager` resets `ApplicationContext::gameplayBgaCompositeState` to fullscreen/no-prepared-frame before every scene render, including non-gameplay and replay-export frames, and `GamePlayScene` replaces it only with the current coordinator result. On blank, miss-only, and video preflight failure, the same `PreparedGameplayBgaFrame` reaches post-scene `main.cpp` fullscreen submission with one video update even when the pre-scene Jukebox active check was false.
- [ ] **Step 7: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target play_skin_session_tests playfield_presentation_coordinator_tests gameplay_skin_integration_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(play_skin_session_tests|playfield_presentation_coordinator_tests|gameplay_skin_integration_tests)$' --output-on-failure
  ```

  Expected RED: GamePlayScene directly owns `BMSRenderer` and has no session coordinator.
- [ ] **Step 8: Implement the Task 21 validator/session/history/coordinator interfaces and, only inside the enabled Task 9 CMake branch, construct the shared application services in dependency order with Task 4's already-constructed `ProfileSettingsPersistenceCoordinator` profile owner/all-profile snapshot provider alive first: roots; `createPlatformSkinAliasDetector()` owner; one catalog; store bound to that catalog, detector, and snapshot provider; resource-preparation service; validator bound to that service; history bound to the same catalog; configuration-write queue; `SkinCommitCoordinator` bound to the same profile owner/store**. The validator runs Lua/model checks and Task 13's synchronous `validateResources` entirely inside the supplied read-view lifetime. `PlaySkinSession::create` moves the activation's owning lease into the session, builds its Lua filesystem from that lease's view, clones one owning pin for `decodeAndPlan`, supplies the chart model's exact deduplicated runtime strings, and transfers the clone into the uploaded resource catalog; member/destruction order releases non-owning consumers before the master pin. `ApplicationContext` exposes an unconditional dependency-free `pollGameplaySkinCommits()` method: enabled builds call the coordinator, disabled builds inline/no-op without naming a skin service type. `main.cpp` calls it exactly once on the main thread at the top of every application-loop iteration before background/Android early-continue branches and before scene/controller consumers. Polling therefore remains active when Settings closes, scenes change, or rendering is skipped, and Android OFF still compiles. Context exposes immutable catalog snapshots plus a default-empty `AcquireGameplaySkinForNextChart` callback only inside guarded code; Android's disabled build contains none of these Lua-skin service types. Replace every direct GamePlayScene renderer call with the exact coordinator above plus state store on enabled builds; inject warmed built-in presentation, optional chart-lifetime session, Jukebox BGA submitter, failure recorder, and a default diagnostic viewport-persistence callback. Repoint the already-owned Task 16 `PlayfieldPresentationEventFanout` from the former built-in sink to the coordinator exactly once; coordinator event methods always forward to the warmed built-in and additionally to the live skin session, so fallback animation state remains current. Capture/project once, evaluate/commit/submit one presentation, and add a source-contract assertion that `rg -n 'ownedRenderer|\brenderer->|BMSRenderer' src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp` returns no matches.
- [ ] **Step 8a: Add exactly one `SkinPackageOperationService` to that construction order immediately after the concrete validator and before history/controller/lifecycle consumers**. Settings and Task 24 share it and may not invoke raw store filesystem or validation operations; partial-construction unwind calls its idempotent destructor shutdown while validator/store/catalog/provider still live.
- [ ] **Step 9: At chart construction call only the injected `AcquireGameplaySkinForNextChart`; session/integration tests inject a validated request, while the production callback remains empty until Task 24 wires lifecycle**. Build a fresh session from the request or use warmed built-in mode on absence/create failure. `GamePlayScene` never calls package activation methods directly; Settings in Task 23 and lifecycle in Task 24 own worker prepare/main-thread commit transactions.
- [ ] **Step 10: Keep Settings preview and replay-video export built-in-only for v1**. Keep AsoBMaShow authoritative for audio, input, judge, gauge, score, replay, fail, result, and scene transition.
- [ ] **Step 11: Enforce exact frame order: `SceneManager` resets `ApplicationContext::gameplayBgaCompositeState` to fullscreen with no prepared frame before scene render; capture/project once; process queued writers, custom timers, then automatic events; evaluate the whole skin buffer; call `prepareVisualFrameAt` exactly once; on critical evaluation/preflight failure discard commands/writes, disable the session for this chart, render the warmed built-in presentation, and return fullscreen mode plus that prepared value in `PresentationFrameResult::preparedBga`; on success submit authored skin commands and embedded BGA from that same prepared frame with zero built-in lane/playfield/HUD draws, commit allowed session-local presentation writes, enqueue one persisted-write batch for the frame with profile/entry/expected revision/configuration and all option/file/offset intents in authored order, render only pause/Reset Layout/system chrome last, and return embedded mode plus the prepared value**. Set `PresentationFrameResult::frameSerial` from the sole matched capture/projection pair on every return path, including failures before BGA preparation; `GamePlayScene` copies exactly that result field, mode, and prepared value into `ApplicationContext::gameplayBgaCompositeState`. There is no hidden coordinator→context reach-through. If failure occurs before gameplay BGA preparation, the coordinator prepares once for fallback. The current session remains unchanged except native Reset Layout's viewport-only transform; Task 24 drains each configuration batch, applies all writes to one candidate, and validates/saves once off the render thread. `main.cpp` reads the context state after `sceneManager.render()`: fullscreen with `prepared` calls `submitFullscreen(*prepared)`; fullscreen without it uses the unchanged legacy Jukebox path; embedded suppresses global submission. Thus stale state cannot suppress same-frame fallback or update video twice.
- [ ] **Step 12: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target play_skin_session_tests playfield_presentation_coordinator_tests gameplay_skin_integration_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(play_skin_session_tests|playfield_presentation_coordinator_tests|gameplay_skin_integration_tests)$' --output-on-failure
  cmake --build cmake-build-debug -j 6
  ctest --test-dir cmake-build-debug --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: valid skins own the whole play surface; every failure produces one complete built-in frame, never a hybrid.
- [ ] **Step 13: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/SkinConfigurationWriteQueue.h src/skin/SkinConfigurationWriteQueue.cpp src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp src/skin/beatoraja/GameplaySkinValidator.h src/skin/beatoraja/GameplaySkinValidator.cpp src/skin/beatoraja/SkinDiagnosticHistory.h src/skin/beatoraja/SkinDiagnosticHistory.cpp src/skin/package/SkinPackageCatalog.h src/skin/package/SkinPackageCatalog.cpp src/scene/play/PlayfieldPresentation.h src/scene/play/PlayfieldPresentationCoordinator.h src/scene/play/PlayfieldPresentationCoordinator.cpp src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp src/scene/play/CMakeLists.txt src/scene/SceneManager.h src/scene/SceneManager.cpp src/skin/CMakeLists.txt src/context.h src/main.cpp tests/play_skin_session_tests.cpp tests/playfield_presentation_coordinator_tests.cpp tests/gameplay_skin_integration_tests.cpp
  git commit -m "feat: activate Lua gameplay skin sessions"
  ```

## Slice 5 — iPad Integration and Closure

### Task 22: Add cancellable ZIP and folder picker handoff

**Reference refresh:** `LuaSkinLoader.sandboxed`, target package-root layout, and target file/module access that proves the picked folder itself must remain the package boundary.

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `src/PlatformDocumentHandoff.h`
- Modify: `src/PlatformDocumentHandoff.cpp`
- Modify: `src/iOSNatives.hpp`
- Modify: `src/iOSNatives.mm`
- Modify: `src/context.h`
- Modify: `src/main.cpp`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Test: `tests/platform_document_handoff_tests.cpp`
- Modify: `tests/ios_build_setup_tests.py`
- Modify: `scripts/ios_artifact_audit.sh`
- Modify: `tests/ios_artifact_audit_tests.py`

**Interfaces:**

- Consumes: the existing operation-token/commit-gate machinery, legacy file handoff API, and Task 3's shared `normalizeSkinSourceNameNfc` helper.
- Produces: injected-cleanup overloads of `ImportDocumentAsync`/`ImportDirectoryAsync`, `CleanupTemporaryPath(PlatformDocumentHandoffResult &)`, the one app-owned `PlatformTemporaryPathCleanupService` constructed in `ApplicationContext`, and the file-only compatibility wrapper `CleanupTemporaryDocument`. A `TemporaryPathCleanupCapability` owns one issued private root and kind; cleanup never trusts a boolean or caller-provided path. No UI/operation-state destructor recursively deletes a temporary tree.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("directory cleanup cannot be retargeted") {
  auto result = importedDirectoryFixture();
  result.localPath = result.localPath.parent_path();
  CHECK_FALSE(CleanupTemporaryPath(result));
}
```

- [ ] **Step 2: Make the RED handoff tests reference this generalized exact-root cleanup capability while preserving file-call compatibility**:

  ```cpp
  enum class PlatformTemporaryPathKind : std::uint8_t {
    None,
    File,
    Directory
  };

  struct PlatformDirectoryImportRequest {
    std::uint64_t maxBytes = 0;
    std::uint64_t maxFiles = 0;
    std::uint32_t maxDepth = 0;
    std::uint32_t maxPathBytes = 0;
  };

  namespace platform_document_handoff::detail {
  class TemporaryPathCleanupCapability;
  }

  struct PlatformDocumentHandoffResult {
    PlatformDocumentHandoffStatus status =
        PlatformDocumentHandoffStatus::Failed;
    std::string message;
    std::filesystem::path localPath;
    std::string originalSourceName;
    PlatformTemporaryPathKind temporaryPathKind =
        PlatformTemporaryPathKind::None;
    std::shared_ptr<platform_document_handoff::detail::
                        TemporaryPathCleanupCapability>
        temporaryCleanup;
    bool temporaryLocalFile = false; // legacy file wrapper only
    bool ok() const noexcept {
      return status == PlatformDocumentHandoffStatus::Succeeded;
    }
    bool cancelled() const noexcept {
      return status == PlatformDocumentHandoffStatus::Cancelled;
    }
  };

  namespace platform_document_handoff {
    class PlatformTemporaryPathCleanupService {
    public:
      PlatformTemporaryPathCleanupService();
      ~PlatformTemporaryPathCleanupService();
      // Moves path/kind/capability by value to one cleanup worker and returns
      // immediately. Only that worker invokes CleanupTemporaryPath.
      void schedule(PlatformDocumentHandoffResult &&) noexcept;
      void shutdown() noexcept;
    };
    using PlatformTemporaryPathCleanupHandle =
        std::shared_ptr<PlatformTemporaryPathCleanupService>;

    class PlatformDocumentHandoffOperation {
    public:
      PlatformDocumentHandoffOperation() = default;
      PlatformDocumentHandoffOperation(
          PlatformDocumentHandoffOperation &&) noexcept;
      PlatformDocumentHandoffOperation &operator=(
          PlatformDocumentHandoffOperation &&) noexcept;
      PlatformDocumentHandoffOperation(
          const PlatformDocumentHandoffOperation &) = delete;
      PlatformDocumentHandoffOperation &operator=(
          const PlatformDocumentHandoffOperation &) = delete;
      ~PlatformDocumentHandoffOperation();
      bool ready() const noexcept;
      bool poll() const noexcept;
      std::optional<PlatformDocumentHandoffResult> takeResult();
      void cancel() noexcept;
      // Signals/detaches only. Completion owns cleanup of an unconsumed result.
      void abandon() noexcept;
      void close() noexcept; // compatibility alias for abandon
      explicit operator bool() const noexcept;
    };

    // Preserve the existing profile-import call surface.
    PlatformDocumentHandoffOperation ImportDocumentAsync(
        PlatformDocumentImportRequest);
    PlatformDocumentHandoffOperation ImportDocumentAsync(
        PlatformDocumentImportRequest,
        PlatformTemporaryPathCleanupHandle);
    PlatformDocumentHandoffOperation ImportDirectoryAsync(
        PlatformDirectoryImportRequest,
        PlatformTemporaryPathCleanupHandle);
    bool CleanupTemporaryPath(PlatformDocumentHandoffResult &) noexcept;
    bool CleanupTemporaryDocument(
        PlatformDocumentHandoffResult &) noexcept;
  }
  ```

- [ ] **Step 3: Extend handoff tests first for temporary-directory ownership, recursive capability cleanup of only the exact issued root, the legacy `CleanupTemporaryDocument` file wrapper, refusal to clean a child/parent/substituted path, directory cancellation before picker/during copy/before commit, file/count/byte/depth/path limits, symlink/alias/nonregular rejection, normalization of the picker URL's UTF-8 `lastPathComponent` through Task 3's shared filename-only NFC helper as `originalSourceName`, invalid-UTF-8 rejection, and nonblocking `cancel()`/`close()` during Settings teardown.** Inject a blocking cleaner and prove `schedule`, operation `abandon`, and controller-style close return immediately while exactly the issued root is eventually removed. Race a result becoming ready between `ready()`/`takeResult()` and `abandon()`: `OperationState::abandon` only signals/detaches, and the completion worker—not the abandoning caller or result destructor—moves any ready/unconsumed result to the cleanup service. Cover operation and cleanup-service destruction with pending/ready work; idempotent shutdown drains or explicitly reports every capability.
- [ ] **Step 4: Extend iOS build/artifact tests first to require source and final built Info.plist values for `UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`, and `UISupportsDocumentBrowser`, plus the folder-import native symbols**. Expected RED: directory handoff and the final artifact audit are incomplete.
- [ ] **Step 5: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target platform_document_handoff_tests -j 6
  ctest --test-dir cmake-build-debug -R '^platform_document_handoff_tests$' --output-on-failure
  python3 -m unittest tests/ios_build_setup_tests.py tests/ios_artifact_audit_tests.py -v
  ```

  Expected RED: directory handoff, exact-root cleanup, and final artifact declarations are absent.
- [ ] **Step 6: Implement the Task 22 result/capability/directory interfaces on the existing cancellable `AsoDocumentHandoffDelegate` machinery, not bookmarks**. Use `UTTypeFolder`, scoped/coordinated no-follow copying, bounded progress, explicit scope release, and recursive cleanup only after exact issued-root identity/kind validation. Construct exactly one cleanup-service handle in `ApplicationContext` before any Settings handoff/controller can be created, inject that handle into every import operation, and retain it through application teardown; its idempotent destructor covers partial-construction unwind, while Task 24 owns the final explicit shutdown order. `OperationState` retains the service until its completion path has transferred or delivered the result, so a ready-vs-abandon race cannot destroy a directory on the caller thread. After native extraction, normalize every file/folder `originalSourceName` only through Task 3's shared helper in `PlatformDocumentHandoff.cpp`; do not duplicate Unicode normalization in Objective-C++. Update CMake so every focused target that compiles `PlatformDocumentHandoff.cpp`—currently `platform_document_handoff_tests` and `profile_export_staging_tests`—also compiles `src/skin/package/SkinPathPolicy.cpp` and links `utf8proc::utf8proc`.
- [ ] **Step 7: Pass `skin::SkinPackagePolicy::maxArchiveBytes` to the cleanup-service overload of `ImportDocumentAsync` for ZIPs and the typed package-policy byte/file/depth/path limits to `ImportDirectoryAsync`; do not repeat a `2 GiB` literal**. Both results preserve `originalSourceName`, feed the same operation-service preparation path with an editable sanitized package name, and transfer their exact cleanup capability into `SkinDeferredCleanup`/the cleanup service after success, rejection, cancellation, or scene teardown. No caller uses raw `remove_all` or performs synchronous recursive cleanup.
- [ ] **Step 7a: Implement the exact cleanup adapter through Task 7's callable `SkinDeferredCleanup` constructor**. Move the handoff result into a shared one-shot state captured by the callable; running it schedules that result on the retained cleanup-service handle and clears the state. This supports the move-only logical capability without making `SkinPackageOperationService` depend on platform headers, and tests prove empty/moved/run-twice cases cannot delete twice.
- [ ] **Step 8: On desktop, use the folder dialog and copy to a private temporary directory through the same bounded implementation**. On Android v1, return a clear unsupported-directory-picker result while preserving compilation; direct portable `prepareFolder` remains testable and Android-specific UX is outside this iPad milestone.
- [ ] **Step 9: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target platform_document_handoff_tests -j 6
  ctest --test-dir cmake-build-debug -R '^platform_document_handoff_tests$' --output-on-failure
  python3 -m unittest tests/ios_build_setup_tests.py tests/ios_artifact_audit_tests.py -v
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: cancellation/cleanup are race-safe and the unsigned app artifact advertises Files access.
- [ ] **Step 10: Commit the task**

  ```sh
  git add CMakeLists.txt src/PlatformDocumentHandoff.h src/PlatformDocumentHandoff.cpp src/iOSNatives.hpp src/iOSNatives.mm src/context.h src/main.cpp ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj tests/platform_document_handoff_tests.cpp tests/ios_build_setup_tests.py scripts/ios_artifact_audit.sh tests/ios_artifact_audit_tests.py
  git commit -m "feat: import skin folders on iOS"
  ```

### Task 23: Build the Gameplay Skins settings controller and native UI

**Reference refresh:** `SkinHeader` custom categories/options/files/offsets, `SkinConfiguration.java` selection behavior, `SkinLuaAccessor.exportSkinProperty`, and the target's declared configuration.

**Files:**

- Create: `src/scene/GameplaySkinSettingsController.h`
- Create: `src/scene/GameplaySkinSettingsController.cpp`
- Create: `src/scene/SettingsSceneSkins.cpp`
- Create: `src/scene/SettingsSceneSkinsUnavailable.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/gameplay_skin_settings_tests.cpp`
- Modify: `tests/lua_skin_feature_gate_tests.py`

**Interfaces:**

- Consumes: `originalSourceName`, temporary cleanup capability, package store/catalog snapshots, per-profile settings, and generation/configuration-aware diagnostic history.
- Produces: `suggestSkinPackageName(originalSourceName, pathKind)`, immutable `GameplaySkinSettingsSnapshot`, and controller actions for import/name/confirm/rescan/revalidate/select/remove/layout/cancel. Import strips one case-insensitive `.zip`, preserves a folder basename, NFC-normalizes, and enforces the typed package-name policy.

  ```cpp
  struct SkinPackageNameSuggestion {
    std::string originalSourceName;
    std::string suggestedPackageName;
    std::string validationError;
    bool ok() const noexcept { return validationError.empty(); }
  };

  enum class GameplaySkinSettingsState : std::uint8_t {
    Empty, Ready, Busy, Error
  };
  struct GameplaySkinEntryRow {
    SkinEntryId entry;
    SkinEntryMetadataSnapshot metadata;
    std::string revisionDigest;
    std::string configurationDigest;
    SkinValidationDisposition validation =
        SkinValidationDisposition::Invalid;
    EntryProfileSettings settings;
    std::vector<SkinDiagnostic> diagnostics;
  };
  struct GameplaySkinSettingsSnapshot {
    GameplaySkinSettingsState state = GameplaySkinSettingsState::Empty;
    bool featureAvailable = true;
    bool compatibilityEnabled = false;
    std::optional<SkinEntryId> selected7KeyEntry;
    std::vector<GameplaySkinEntryRow> entries;
    std::optional<SkinPackageNameSuggestion> preparedName;
    std::optional<SkinPackageId> collisionPackage;
    SkinProgress progress;
    std::vector<SkinDiagnosticHistoryRecord> history;
    std::string statusMessage;
    bool canCancel = false;
  };
  struct ControllerActionResult {
    bool accepted = false;
    bool asynchronous = false;
    std::string message;
    std::vector<SkinDiagnostic> diagnostics;
  };
  struct GameplaySkinSettingsControllerDependencies {
    SkinPackageOperationService &operations;
    SkinDiagnosticHistory &history;
    SkinProfileId profileId;
    ISkinProfileSettingsOwner &profileOwner;
    ISkinProfileSnapshotProvider &profileSnapshots;
    SkinCommitCoordinator &commits;
    SkinActivationClientId clientId;
    // Task 24 wires both callbacks to the one app-owned lifecycle. Task 23
    // tests inject fakes; no controller ever starts a second scanner.
    std::function<void()> requestRescan;
    std::function<void(const SkinEntryId &)> requestRevalidation;
    std::function<std::shared_ptr<const SkinPackageCatalogSnapshot>()>
        catalogSnapshot;
    std::function<platform_document_handoff::
                      PlatformDocumentHandoffOperation()>
        beginArchiveHandoff;
    std::function<platform_document_handoff::
                      PlatformDocumentHandoffOperation(
        PlatformDirectoryImportRequest)> beginFolderHandoff;
  };

  class GameplaySkinSettingsController {
  public:
    explicit GameplaySkinSettingsController(
        GameplaySkinSettingsControllerDependencies);
    ~GameplaySkinSettingsController(); // calls idempotent no-throw close
    const GameplaySkinSettingsSnapshot &snapshot() const noexcept;
    void poll();
    void profileChanged(SkinProfileId, SkinActivationClientId);
    ControllerActionResult beginArchiveImport();
    ControllerActionResult beginFolderImport();
    ControllerActionResult setSuggestedPackageName(std::string);
    ControllerActionResult confirmPreparedImport(PackageCollisionPolicy);
    ControllerActionResult requestRescan();
    ControllerActionResult requestRevalidation(const SkinEntryId &);
    ControllerActionResult select(const SkinEntryId &);
    ControllerActionResult setCompatibilityEnabled(bool);
    ControllerActionResult setOption(const SkinEntryId &, std::string, int);
    ControllerActionResult setFileChoice(const SkinEntryId &, std::string,
                                         std::string);
    ControllerActionResult setOffset(const SkinEntryId &, std::string,
                                     ConfigOffset);
    ControllerActionResult setViewport(const SkinEntryId &,
                                       ViewportSettings);
    ControllerActionResult requestRemoval(const SkinPackageId &);
    ControllerActionResult resetLayout(const SkinEntryId &);
    void cancelOperation() noexcept;
    void close() noexcept;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("import name comes from the selected source") {
  const auto suggestion = suggestSkinPackageName("ModernChic.zip", PlatformTemporaryPathKind::File);
  CHECK(suggestion.suggestedPackageName == "ModernChic");
}
```

- [ ] **Step 2: Make the RED controller tests reference the pure snapshot/action interface, including editable suggested package name, current/history diagnostics, collision prompt, and enabled actions.**
- [ ] **Step 3: Write controller tests first for empty/busy/error/ready states; archive and folder original-name preservation, sanitized/editable package naming, completion and capability cleanup; replace versus reject without merge; select only validated 7-key entries; explicit compatibility enable/disable; operation-service prepare followed by app-owned commit-coordinator submission on selection and every option/file/offset edit; stale profile generation and invalid configuration retaining prior settings/activation; Fit/Stretch/Custom edits/reset; rescan/revalidate/remove; manual invalid edit preserving last valid revision; package delete fallback; tab close cancellation/detachment while an accepted commit completes independently; and deduplicated current/history diagnostics with entry/virtual Lua file/line.** Before app-driven publication, request the complete all-profile inventory and submit it with the prepared package to the one app-owned operation service; a corrupt/missing inactive profile aborts replacement and preserves the old package. If publication returns `retryableInventoryRace`, retain exactly its returned `retryPrepared`, reload the complete inventory, and resubmit through the service without picker access; test multiple bounded races followed by success, explicit cancellation cleanup, and permanent failure cleanup. The controller owns no worker/thread and captures no `this` in asynchronous work: every pre-submit/inventory/operation job is a process-monotonic ticket, `close`/destructor/profile switch call `cancelAndDetach` on all of them, discard their progress mailbox, and move any collision-held `PreparedPackage` into `operations.discardPrepared` before UI state dies. Accepted commit-coordinator transactions remain durable and independent. Race close/destruction at every operation phase, including a completion becoming ready concurrently and a blocking cleanup/deletion; close must return without recursive filesystem deletion on the main thread and the service eventually disposes exactly once. The controller's first and subsequent settings snapshots must come from `profileOwner.snapshot(profileId)`, never a copied constructor value. `profileChanged` cancels old-profile preparation/inventory tickets, detaches the old client ID without cancelling accepted commits, installs a new ID, and refreshes the newly bound owner snapshot. Cover switching profiles from the Profiles tab while Settings stays alive, then editing Gameplay Skins without touching the old profile. Enabling requires a selected validated activation; disabling remembers selection/configuration but forces built-in presentation for subsequent charts. `requestRescan`, `requestRevalidation`, and `catalogSnapshot` must use only the injected lifecycle callbacks, proving startup/tab/manual scans have one owner. Extend the feature-gate test to require the real controller/`SettingsSceneSkins.cpp` only when enabled; Android instead compiles `SettingsSceneSkinsUnavailable.cpp`, whose dependency-free tab reads only `luaGameplaySkinsAvailable()==false`, explains desktop/iOS availability, and exposes no import/select actions or validator/history types.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target gameplay_skin_settings_tests -j 6
  ctest --test-dir cmake-build-debug -R '^gameplay_skin_settings_tests$' --output-on-failure
  python3 -m unittest tests/lua_skin_feature_gate_tests.py -v
  ```

  Expected RED: no controller/tab exists and its enabled/unavailable source gate is absent.
- [ ] **Step 5: Implement the Task 23 suggestion/snapshot/dependency/controller interfaces for feature-enabled builds, add `GameplaySkins` to `SettingsScene::SettingsTab`, and implement/register the enabled and dependency-free unavailable versions of `buildGameplaySkinsTab()` under the Task 9 feature gate**. `SettingsSceneSkins.cpp` must wrap all definitions in `#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS`; `SettingsSceneSkinsUnavailable.cpp` uses the exact complement. This is required in addition to CMake selection because the iOS synchronized group discovers both files. Construct the real controller from Task 21's one app-owned `SkinPackageOperationService`, history/profile owner/snapshot provider/`SkinCommitCoordinator`; it receives no raw store or validator and obtains its `clientId` only from `commits.createClient()`. Inject lifecycle callbacks that are explicitly unavailable until Task 24 wires them. On each profile change allocate a fresh never-reused ID and pass it to `profileChanged`, which alone detaches its previous ID. Wire tab selection, layout, reset, cleanup, and update polling. `requestRemoval` calls `operations.submitRemove`, never calls the store or deletes paths directly.
- [ ] **Step 6: Trigger rescan on the transition into the tab, not inside its layout builder**. Provide a `Use Beatoraja Gameplay Skin` toggle plus `Import Skin Archive`, `Import Skin Folder`, editable `Package Name`, `Rescan/Reload`, `Revalidate`, `Select`, `Replace`, `Remove`, and `Reset Layout` actions with confirmations for whole-package replacement/removal. Normalize and validate the package name before enabling import. Display the Files location as `On My iPad/AsoBMaShow/Skins`.
- [ ] **Step 7: Populate controls only from the callback-free `SkinEntryMetadataSnapshot` persisted by the validator/catalog; never rerun Lua from the UI. Display name, author, entry path, source canvas, revision, validation state, declared categories/options/files/offsets, four synthesized offsets, Fit/Stretch/Custom controls, and compatibility diagnostics**. Keep the controls native and usable even when a selected Lua skin is broken.
- [ ] **Step 8: For selection/option/file/offset changes, snapshot the shared `ISkinProfileSettingsOwner`, submit the reconciled candidate to `operations.submitPrepareActivation`, then pass the owned prepared result to the app-owned `SkinCommitCoordinator` with this controller's current client ID**. `ApplicationContext` polls the coordinator independently of Settings; controller `poll()` only consumes its client completions and refreshes from the returned/current owner snapshot. Persist compatibility-toggle and sanitized viewport-only changes through `submitProfileSettings`, because they do not require Lua validation. `close()`/`profileChanged()` cancel-and-detach every pre-submit operation/all-profile inventory ticket and detach the old commit client without cancelling accepted durable work. Release the picker capability as soon as archive/folder preparation yields an independently owned `PreparedPackage`; collision confirmation retains that prepared object only until confirm/cancel/close, then transfers it into publish or deferred service disposal. Package mutation, inventory load, validation, cleanup, and diagnostic-history persistence run asynchronously; main-thread owner/catalog commits are short and the scene never blocks on validation or I/O.
- [ ] **Step 9: Run the GREEN check**

  ```sh
  cmake --build cmake-build-debug --target gameplay_skin_settings_tests -j 6
  ctest --test-dir cmake-build-debug -R '^gameplay_skin_settings_tests$' --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  python3 -m unittest tests/lua_skin_feature_gate_tests.py -v
  ```

  Expected GREEN: all controller transitions pass and Settings remains usable with no packages or a malformed package.
- [ ] **Step 10: Commit the task**

  ```sh
  git add CMakeLists.txt src/scene/GameplaySkinSettingsController.h src/scene/GameplaySkinSettingsController.cpp src/scene/SettingsSceneSkins.cpp src/scene/SettingsSceneSkinsUnavailable.cpp src/scene/SettingsScene.h src/scene/SettingsScene.cpp src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneControls.cpp src/scene/CMakeLists.txt tests/gameplay_skin_settings_tests.cpp tests/lua_skin_feature_gate_tests.py
  git commit -m "feat: add gameplay skin settings"
  ```

### Task 24: Wire startup rescans, profile activation, and release-critical verification

**Reference refresh:** `SkinLoader.load`, `LuaSkinLoader.load`, default `play7.luaskin`, and the target entry/configuration chosen in the acceptance manifest.

**Files:**

- Create: `src/skin/GameplaySkinLifecycle.h`
- Create: `src/skin/GameplaySkinLifecycle.cpp`
- Modify: `src/context.h`
- Modify: `src/main.cpp`
- Modify: `src/scene/SceneManager.cpp`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneSkins.cpp`
- Modify: `src/scene/ProfileSettingsController.h`
- Modify: `src/scene/ProfileSettingsController.cpp`
- Modify: `src/scene/ProfileSettingsControllerContext.cpp`
- Modify: `src/scene/SettingsSceneProfiles.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `scripts/ios_release_verify.sh`
- Modify: `tests/ios_release_workflow_tests.py`
- Test: `tests/gameplay_skin_lifecycle_tests.cpp`
- Test: `tests/profile_settings_controller_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: default storage roots, active profile ID/settings, the one app-owned package operation service, immutable catalog snapshots, and coordinator failures. It receives no raw package store/validator and cannot bypass the serialized operation FIFO.
- Produces: one cancellable `GameplaySkinLifecycle` service shared by startup, tab-open, and explicit rescans. Profile changes alter only the desired next-chart activation; a running session remains pinned.

  ```cpp
  enum class SkinRescanReason : std::uint8_t {
    Startup,
    SettingsOpened,
    Explicit
  };

  class GameplaySkinLifecycle {
  public:
    GameplaySkinLifecycle(SkinStorageRoots,
                          SkinPackageOperationService &,
                          SkinDiagnosticHistory &,
                          SkinConfigurationWriteQueue &,
                          ISkinProfileSettingsOwner &,
                          ISkinProfileSnapshotProvider &,
                          SkinCommitCoordinator &,
                          SkinActivationClientId lifecycleClientId);
    ~GameplaySkinLifecycle(); // calls idempotent no-throw shutdown
    void startAfterProfileInitialization(SkinProfileId);
    void profileChanged(SkinProfileId);
    void requestRescan(SkinRescanReason);
    void requestRevalidation(const SkinEntryId &);
    GameplayViewportPersistenceResult requestViewportReset(
        const PlaySkinSessionIdentity &, ViewportSettings);
    void poll();
    std::shared_ptr<const SkinPackageCatalogSnapshot>
    catalogSnapshot() const noexcept;
    std::optional<GameplaySkinActivationRequest> acquireForNextChart();
    void recordPresentationFailure(const PresentationFailure &);
    void shutdown() noexcept;
  };
  ```

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```cpp
TEST_CASE("profile switch affects only the next chart") {
  auto fixture = runningLifecycleFixture();
  fixture.switchProfile("second");
  CHECK(fixture.runningRevision() == fixture.originalRevision());
}
```

- [ ] **Step 2: Add lifecycle tests first for creating `Documents/Skins`, the exact startup order `catalog/store construction → synchronous recoverBeforeServiceStart → operation-service construction → asynchronous startup scan`, and default-config validation of new 7-key entries, typed profile ID/config selection, explicit compatibility disable yielding no next-chart activation while remembering selection, activation on selection/options/files/offsets through worker `prepareActivation` → app-owned `submitActivation` → application `SkinCommitCoordinator::poll`, sanitized current-profile viewport carried separately on every next-chart request without revalidating Lua, chart-start revalidation of a changed desired config, profile-save failure retaining prior settings/activation, crash-after-save recovery diagnostics, and a source race producing a lifecycle-consumed revalidation request.** Require `Failed`, `ConcurrentCallRejected`, or `AlreadyRecovered` to prevent a second service/lifecycle construction and expose built-in gameplay only; the latter two dispositions perform no filesystem replay. Also cover a process-monotonic nonzero session serial on each acquired chart and a lifecycle-owned bounded per-session writer FIFO. Drain render batches into that FIFO without blocking; keep at most one prepare/owner-commit/activation-CAS in flight per session. If B arrives while A is pending, retain B in frame/authored order; after A succeeds, rebase B only onto A's exact returned successor profile snapshot/configuration digest and commit B, rather than rejecting it for the session's original digest. Multiple pending batches may be folded into the next single candidate only in frame then authored order. Any external owner generation, selected entry, source/catalog generation, revision, profile switch, or different session serial invalidates the chain and diagnoses/discards its remaining batches. Cover applying two same-frame writes in authored order to one candidate with one validation/save; A-pending/B-arrives/A-success/B-success; owner single-in-flight never rejecting internal B; rejecting truly stale/external profile/entry/revision/configuration/session batches; sanitize → validation → profile-save → activation-CAS ordering; invalid writer batches retaining old activation/settings with history diagnostics; invalid changed config retaining the prior activation while the chart uses built-in fallback; profile-switch activation separation; one shared startup/Settings-tab/explicit rescan service; active-session revision pinning; no mid-chart reload; post-chart changed-revision eligibility; selected-package valid replacement withheld until its selected configuration is prepared/committed; invalid replacement remaining diagnosed/visible while the old activation/settings stay eligible; deleted-package fallback; invalid edit last-known-good behavior; native current-chart Reset Layout immediately applying Fit and `requestViewportReset` submitting exactly one profile-only commit for future charts; a reset from stale revision/configuration/session identity after next-chart activation moved to another identity rejecting without altering the newer desired state; graceful queue/shutdown cancellation; and startup journal recovery. Add profile-management regressions for save-completed/CAS-pending → switch → each of create, duplicate, delete, import-create, and import-overwrite racing both all-profile loading and post-validation pre-publication. Every membership mutation must invalidate the captured inventory before touching the manager/files; delete/overwrite must additionally make `beginProfileMutation` reach terminal first. Successful deletion removes keys/leases and stays blocked; successful overwrite removes then resumes; failed mutation retains keys and resumes acceptance; an abandoned combined barrier releases both gates.
- [ ] **Step 3: Extend release-workflow contract tests first to require all release-critical skin targets, shader/artifact audits, and the unsigned iOS build**. Expected RED: new tests are not in the verification script.
- [ ] **Step 4: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target gameplay_skin_lifecycle_tests profile_settings_controller_tests -j 6
  ctest --test-dir cmake-build-debug -R '^(gameplay_skin_lifecycle_tests|profile_settings_controller_tests)$' --output-on-failure
  python3 -m unittest tests/ios_release_workflow_tests.py -v
  ```

  Expected RED: the lifecycle service/target is absent and the release script omits its required skin gates.
- [ ] **Step 5: Construct the skin subsystem in this exact startup order: create roots/catalog/store; synchronously call `store.recoverBeforeServiceStart()` exactly once while no service, lifecycle, or filesystem worker exists; only on `Recovered` create the validator and one `SkinPackageOperationService`; then construct only `GameplaySkinLifecycle` from Task 21's app-owned roots/service/history/write queue/profile owner/all-profile snapshot provider/`SkinCommitCoordinator`, allocating one dedicated never-reused lifecycle client with `commits.createClient()`; finally wire `ApplicationContext::AcquireGameplaySkinForNextChart` to `acquireForNextChart()` and clear it before lifecycle shutdown**. Any other disposition constructs neither operation service nor lifecycle and leaves built-in gameplay available with a sanitized startup diagnostic; `AlreadyRecovered` specifically prevents accidentally constructing a second service. No code calls private catalog recovery directly. The lifecycle submits all rescans, revalidation/activation preparation, and GC through the service and uses only its read/lease facade for catalog snapshots and chart acquisition; it never calls raw store/validator filesystem or validation methods. Every scan first awaits a complete all-profile inventory; any load failure preserves the old catalog/revisions and reports a diagnostic instead of skipping selected configuration validation. Start a cancellable scan after active profile initialization and keep UI/gameplay consumers on immutable catalog snapshots. `acquireForNextChart()` assigns a nonzero process-monotonic session serial and returns it in the request.

  Wire Task 23's rescan/revalidation/snapshot callbacks to this one lifecycle, and wire the gameplay coordinator's native viewport callback to `requestViewportReset`. That method receives all five fields of the immutable `PlaySkinSessionIdentity`, requires the tracked current session serial plus current owner selection/catalog activation to match profile, entry, revision digest, and configuration digest, changes only that entry's viewport, and calls `submitProfileSettings`; a stale identity returns `Rejected` and cannot touch a newer desired activation. If another owner ticket for that complete identity is pending, coalesce one latest viewport request keyed by all five fields and return `Deferred`; retry only while every field still matches after terminal completion.

  `poll()` runs after app-owned commit polling, consumes completions/revalidation requests, retries bounded viewport changes, and drains writer batches into a bounded FIFO keyed by the five-field session identity. It permits at most one prepare/save/CAS for that chain. Later batches remain ordered while one is in flight; on success, rebase them only onto that transaction's exact successor owner snapshot and configuration digest before the next prepare. An external generation/catalog/source/selection/revision change or another session serial invalidates the remaining chain. Each submitted candidate applies all eligible writes in frame/authored order, sanitizes once, validates once, and saves/activates once. Validation/save failure retains old settings/activation and appends a diagnostic; a post-save source race is re-prepared only from durable desired settings. `recordPresentationFailure` appends runtime/fallback history without overwriting earlier phases.

  Task 4's final no-throw `activeProfileCommitted` hook first calls `bindCommittedActiveProfile`, then lifecycle `profileChanged(newId)` and, if Settings remains open, allocates `newClientId = commits.createClient()` and calls controller `profileChanged(newId,newClientId)`; that controller method alone detaches exactly its stored old ID and installs the new one. Never notify from the earlier fallible `activateProfileServices`. A running chart session remains pinned. Encode teardown in this exact order: clear scene/controller/acquisition/viewport/profile-mutation producers and call controller `close`; `GameplaySkinLifecycle::shutdown`; `SkinPackageOperationService::shutdown` to cancel/drain every preparation/filesystem/deferred-cleanup ticket; `PlatformTemporaryPathCleanupService::shutdown` after no operation can enqueue another capability; `SkinCommitCoordinator::shutdown` while store/catalog/profile owner still live; `SkinDiagnosticHistory::flush`; `SkinPackageCatalog::flush` then `shutdown`; `SkinResourcePreparationService::shutdown` after every validation/session producer is gone; `ProfileSettingsPersistenceCoordinator::shutdown`; destroy history before catalog, then validator/store/catalog/resource objects; destroy every `SkinResourceCatalog` on the render thread before bgfx shutdown. No earlier destructor/error path may close the cleanup service, profile owner, catalog, resource service, cache, or store first. Every owner destructor repeats its idempotent shutdown/close contract for partial construction and exception unwind.
- [ ] **Step 6: Route every profile-inventory mutation through one combined main-thread barrier without making unconditional profile UI sources depend on enabled-only skin classes**. Add dependency-free callbacks to `ProfileSettingsControllerDependencies`/`ApplicationContext` taking only opaque strings/tokens: `beginSkinProfileCatalogMutation(optional<string_view> existingTargetId, string &error) -> optional<uint64_t>` (the disabled/default implementation returns a token) and `finishSkinProfileCatalogMutation(token, succeeded, profileStillExists)` (disabled/default no-op). Enabled wiring owns a token map whose value contains a mandatory Task 4 `ProfileInventoryMutationBarrier` plus, for delete/import-overwrite, an optional Task 7 `SkinProfileMutationBarrier`. For an existing target, acquire/drain `SkinCommitCoordinator::beginProfileMutation(id)` first; then call `ISkinProfileSnapshotProvider::beginInventoryMutation()`, which invalidates the inventory epoch before it waits for a short publication fence. If either acquisition fails or the token is abandoned, RAII resumes the per-profile gate and releases the inventory gate. On finish, call `SkinCommitCoordinator::finishProfileMutation` first when present, then `finishInventoryMutation`; do both on the main thread and exactly once.

  Wrap synchronous `create`, `duplicate`, and `remove` immediately around their manager/file mutations: create/duplicate pass no existing target, while remove passes its target ID. Both success and every returned failure/exception finish the opaque token. `rename`, export, and ordinary activation do not change membership; active binding and every skin/full-settings generation change are already serialized by Task 4 against the commit fence. For import-create and import-overwrite, `ProfileSettingsController::beginImport` acquires the combined token on the main thread before `SettingsSceneProfiles` starts its `std::jthread`; create passes no target and overwrite passes `options.overwriteProfileId`. The worker `ProfileArchiveTask::execute` never calls a skin/provider callback. Retain the opaque token in controller state keyed by archive generation, then finish it on the main thread in `completeArchive`. Also finish failure on picker cancellation after acquisition, thread-start failure, stale completion, controller/scene teardown, and every exception/abandon path. A successful delete passes `profileStillExists=false`; a successful overwrite passes true; create/duplicate/import-create have no per-profile gate and ignore that flag. Any failure resumes and preserves activation. Staging writes may use the existing direct `AppSettingsStore::Save` only while the combined token is held. Android uses defaults and includes no coordinator/store/provider types. After profile initialization/import, enabled wiring calls `operations.submitReconcileProfileActivations` with a value-owned vector of typed IDs from `PlayerProfileManager::listProfiles()` and consumes its completion before exposing chart acquisition; it never mutates the store directly. Add no observer or skin dependency to `PlayerProfileManager` itself, and extend `lua_skin_feature_gate_tests.py` to reject enabled-only type names in the unguarded controller/context sources.
- [ ] **Step 7: Add focused native targets to `scripts/ios_release_verify.sh`: path policy, snapshotter, archive/store, Lua filesystem/runtime/decoder/host, viewport/destination/model/commands, visual state/projection/bridge/session/coordinator, touch, BGA, settings, lifecycle, and profile-management integration**. Keep the script free of signing/distribution/upload actions.
- [ ] **Step 8: Run the GREEN check**

  ```sh
  python3 -m unittest tests/ios_release_workflow_tests.py tests/ios_build_setup_tests.py tests/ios_artifact_audit_tests.py -v
  cmake --build cmake-build-debug -j 6
  ctest --test-dir cmake-build-debug --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: all portable tests, desktop target, artifact audits, release-critical native tests, and unsigned iOS build pass without upload.
- [ ] **Step 9: Commit the task**

  ```sh
  git add CMakeLists.txt src/skin/GameplaySkinLifecycle.h src/skin/GameplaySkinLifecycle.cpp src/skin/CMakeLists.txt src/context.h src/main.cpp src/scene/SceneManager.cpp src/scene/SettingsScene.cpp src/scene/SettingsSceneSkins.cpp src/scene/ProfileSettingsController.h src/scene/ProfileSettingsController.cpp src/scene/ProfileSettingsControllerContext.cpp src/scene/SettingsSceneProfiles.cpp scripts/ios_release_verify.sh tests/ios_release_workflow_tests.py tests/gameplay_skin_lifecycle_tests.cpp tests/profile_settings_controller_tests.cpp
  git commit -m "feat: wire gameplay skin lifecycle"
  ```

### Task 25: Close SCURO compatibility and physical-iPad acceptance

**Reference refresh:** reread every baseline file, then every task-specific Beatoraja file referenced by a remaining manifest gap. Rerun the source audit before changing code for a gap.

**Files:**

- Modify: `docs/skin-compat/beatoraja-lua-gameplay-contract.md`
- Modify: `docs/skin-compat/modernchic-scuro-4.02-acceptance.md`
- Modify: `tests/fixtures/beatoraja_skin/reference_manifest.json`
- Create: `src/skin/beatoraja/SkinPerformanceTelemetry.h`
- Create: `src/skin/beatoraja/SkinPerformanceTelemetry.cpp`
- Create: `src/skin/beatoraja/SkinAcceptanceRecorder.h`
- Create: `src/skin/beatoraja/SkinAcceptanceRecorder.cpp`
- Create: `src/skin/beatoraja/SkinOverlayDigestProvider.h`
- Create: `src/skin/beatoraja/SkinOverlayDigestProvider.cpp`
- Create: `src/BuildIdentity.h`
- Create: `src/BuildIdentity.cpp`
- Modify: `src/skin/beatoraja/LuaSkinFileSystem.h`
- Modify: `src/skin/beatoraja/LuaSkinFileSystem.cpp`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.h`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.cpp`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/scene/play/PlayfieldPresentation.h`
- Modify: `src/scene/play/PlayfieldPresentationCoordinator.h`
- Modify: `src/scene/play/PlayfieldPresentationCoordinator.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/GameplaySkinSettingsController.h`
- Modify: `src/scene/GameplaySkinSettingsController.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneSkins.cpp`
- Modify: `src/context.h`
- Modify: `src/main.cpp`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Modify: `scripts/ios_artifact_audit.sh`
- Create: `scripts/ios_build_install_for_skin_acceptance.sh`
- Create: `scripts/run_skin_acceptance.py`
- Test: `tests/skin_acceptance_contract_tests.py`
- Modify: `tests/ios_artifact_audit_tests.py`
- Modify: `tests/ios_build_setup_tests.py`
- Test: `tests/skin_performance_telemetry_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/charts/acceptance_7k.bms`
- Create: `tests/fixtures/beatoraja_skin/charts/acceptance_bga_base.png`
- Create: `tests/fixtures/beatoraja_skin/charts/acceptance_bga_layer.png`
- Create: `tests/fixtures/beatoraja_skin/charts/acceptance_bga_miss.png`
- Create: `tests/fixtures/beatoraja_skin/charts/acceptance_bga_video.mp4`
- Create: `tests/fixtures/beatoraja_skin/charts/README.md`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: Task 1's frozen `AcceptanceContractV1`, the external SCURO archive/root, final diagnostic history, and a physical-iPad evidence root outside Git.
- Produces: bounded `SkinPerformanceTelemetry`, `SkinPerformanceSummary`, and `scripts/run_skin_acceptance.py verify --contract PATH --evidence-root EXTERNAL_PATH --expected-app-commit HEX40`. The verifier rejects an evidence root inside the repository and verifies hashes/metadata without copying third-party images.

  ```cpp
  struct SkinRenderIoCounters {
    std::uint64_t filesystemReadsPerformed = 0;
    std::uint64_t filesystemReadsDenied = 0;
    std::uint64_t filesystemWritesPerformed = 0;
    std::uint64_t filesystemWritesDenied = 0;
    std::uint64_t filesystemDirectoryScansPerformed = 0;
    std::uint64_t filesystemDirectoryScansDenied = 0;
    std::uint64_t resourceUploadsPerformed = 0;
    std::uint64_t resourceUploadsDenied = 0;
  };

  enum class SkinRenderIoOperation : std::uint8_t {
    FilesystemRead,
    FilesystemWrite,
    FilesystemDirectoryScan,
    ResourceUpload
  };

  struct SkinFrameTelemetrySample {
    std::uint64_t frameSerial = 0;
    std::int64_t visualTimeMicros = 0;
    std::uint64_t evaluationMicros = 0;
    std::uint64_t submissionMicros = 0;
    std::uint64_t luaInstructions = 0;
    std::uint64_t callbackMicros = 0;
    std::uint32_t commandCount = 0;
    std::uint32_t batchCount = 0;
    SkinRenderIoCounters renderIo;
    std::uint64_t liveTextures = 0;
    std::uint64_t liveResources = 0;
    std::uint64_t luaAllocatorBytes = 0;
    std::uint64_t residentBytes = 0;
    bool missedPresentation = false;
    bool fallback = false;
  };

  enum class SkinTelemetryContribution : std::uint32_t {
    Runtime = 1U << 0U,
    FileSystem = 1U << 1U,
    Resources = 1U << 2U,
    Renderer = 1U << 3U,
    Session = 1U << 4U,
    Coordinator = 1U << 5U,
    MainLoop = 1U << 6U
  };
  inline constexpr std::uint32_t kCompleteSkinTelemetryContributions =
      0x7fU;
  struct SkinFrameTelemetryEnvelope {
    PlaySkinSessionIdentity identity;
    SkinFrameTelemetrySample sample;
    std::uint32_t contributions = 0;
  };

  struct SkinPerformanceSummary {
    std::uint64_t receivedSampleCount = 0;
    std::uint64_t retainedSampleCount = 0;
    std::uint64_t overflowSampleCount = 0;
    std::uint64_t p99SkinCpuMicros = 0;
    double missedPresentationPercent = 0.0;
    std::uint64_t peakLuaAllocatorBytes = 0;
    std::uint64_t peakResidentBytes = 0;
    std::int64_t residentDriftBytes = 0;
    SkinRenderIoCounters renderIo;
    std::int64_t liveTextureDrift = 0;
    std::int64_t liveResourceDrift = 0;
    std::uint64_t fallbackCount = 0;
  };

  class SkinPerformanceTelemetry {
  public:
    static constexpr std::size_t maxSamples = 65'536;
    void record(const SkinFrameTelemetrySample &) noexcept;
    SkinPerformanceSummary summarize() const;
  };

  struct SkinAcceptanceActivationKey {
    SkinProfileId profileId;
    SkinEntryId entry;
    std::string revisionDigest;
    std::string configurationDigest;
  };
  enum class SkinAcceptanceRunKind : std::uint8_t {
    Performance,
    ResourceLifecycle,
    RenderIoNegative
  };
  struct SkinAcceptanceScenarioContract {
    std::string scenarioId;
    SkinAcceptanceRunKind kind = SkinAcceptanceRunKind::Performance;
    std::string expectedChartSha256;
    std::string expectedLayoutId;
    std::int64_t warmupMicros = 30'000'000;
    std::int64_t measurementMicros = 180'000'000;
    std::uint32_t requiredExitCycles = 10;
    std::uint32_t maximumRefreshHz = 240;
    std::string expectedOpaqueGuardVectorSha256;
    std::optional<std::string> expectedDiagnosticCode;
    std::optional<std::string> expectedFallbackAction;
    std::optional<SkinRenderIoOperation> expectedDeniedOperation;
  };
  struct SkinAcceptanceSessionFacts {
    PlaySkinSessionIdentity identity;
    std::string chartSha256;
    std::string layoutId;
    std::uint32_t actualRefreshHz = 0;
    // Derived from the loaded, validated model's canonical sorted opaque
    // (guard ID, value) vector; never supplied by Settings or editable UI.
    std::string observedOpaqueGuardVectorSha256;
  };
  enum class SkinResourceLifecyclePhase : std::uint8_t {
    BeforeFirstEntry,
    AfterExit
  };
  struct SkinResourceLifecycleSample {
    SkinResourceLifecyclePhase phase =
        SkinResourceLifecyclePhase::BeforeFirstEntry;
    std::uint32_t cycleIndex = 0;
    std::uint64_t liveTextures = 0;
    std::uint64_t liveResources = 0;
    std::uint64_t residentBytes = 0;
  };
  struct SkinLiveResourceSnapshot {
    std::uint64_t liveTextures = 0;
    std::uint64_t liveResources = 0;
  };
  class SkinLiveResourceCounters {
  public:
    void textureCreated() noexcept;
    void textureDestroyed() noexcept;
    void resourceCreated() noexcept;
    void resourceDestroyed() noexcept;
    SkinLiveResourceSnapshot snapshot() const noexcept;
  };

  struct SkinAcceptanceScenarioMetadata {
    std::string opaqueRunId;
    std::string scenarioId;
    std::string layoutId;
    std::string chartSha256;
    SkinEntryId entry;
    std::string revisionDigest;
    std::string configurationDigest;
    std::uint32_t configuredRefreshHz = 0;
    std::int64_t warmupMicros = 30'000'000;
    std::int64_t measurementMicros = 180'000'000;
    std::optional<std::string> overlayDigestBefore;
    std::optional<std::string> overlayDigestAfter;
    std::string expectedOpaqueGuardVectorSha256;
    std::string observedOpaqueGuardVectorSha256;
    std::vector<std::string> observedDiagnosticCodes;
    std::optional<std::string> observedFallbackAction;
  };
  struct SkinAcceptanceExportResult {
    bool exported = false;
    std::filesystem::path documentsRelativePath;
    std::string lowercaseSha256;
    std::optional<SkinDiagnostic> failure;
  };
  struct SkinBuildIdentity {
    std::string commit;
    std::string configuration;
    bool cleanSource = false;
    bool validForAcceptance() const noexcept;
  };
  SkinBuildIdentity compiledSkinBuildIdentity();

  enum class SkinAcceptanceCaptureState : std::uint8_t;

  struct SkinAcceptanceExportTicket {
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const SkinAcceptanceExportTicket &) const = default;
  };
  enum class SkinAcceptanceExportPollState : std::uint8_t {
    Unknown,
    Pending,
    Ready
  };
  struct SkinAcceptanceExportPollResult {
    SkinAcceptanceExportPollState state =
        SkinAcceptanceExportPollState::Unknown;
    std::optional<SkinAcceptanceExportResult> result;
  };

  struct SkinOverlayDigestTicket {
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const SkinOverlayDigestTicket &) const = default;
  };
  enum class SkinOverlayDigestPollState : std::uint8_t {
    Unknown,
    Pending,
    Ready
  };
  struct SkinOverlayDigestPollResult {
    SkinOverlayDigestPollState state = SkinOverlayDigestPollState::Unknown;
    std::string lowercaseSha256;
    std::optional<SkinDiagnostic> failure;
  };
  class IAsyncSkinOverlayDigestProvider {
  public:
    virtual ~IAsyncSkinOverlayDigestProvider() = default;
    // Queues a bounded no-follow digest of the private overlay. The work runs
    // outside render; polling is memory-only and performs no filesystem I/O.
    virtual SkinOverlayDigestTicket
    beginDigest(const SkinAcceptanceActivationKey &) = 0;
    virtual SkinOverlayDigestPollResult
    pollDigest(SkinOverlayDigestTicket) const noexcept = 0;
    virtual void cancelDigest(SkinOverlayDigestTicket) noexcept = 0;
    virtual void shutdown() noexcept = 0; // cancel, drain, join; idempotent
  };

  struct SkinAcceptanceRecorderDependencies {
    std::filesystem::path documentsRoot;
    SkinBuildIdentity buildIdentity;
    std::function<std::optional<SkinAcceptanceScenarioContract>(
        std::string_view)> resolveScenario;
    // Non-owning; ApplicationContext owns this provider after the recorder and
    // drains it during shutdown. It never executes filesystem work on render.
    IAsyncSkinOverlayDigestProvider *overlayDigests = nullptr;
    std::function<bool(const std::filesystem::path &,
                       std::span<const std::byte>, std::string &)>
        writeAtomic;
    // Called synchronously on the main thread at export linearization. The
    // bounded sanitized value copy, never this callback/history reference,
    // crosses to the worker.
    std::function<std::vector<SkinDiagnostic>()> snapshotDiagnostics;
  };
  class SkinAcceptanceRecorder final {
  public:
    explicit SkinAcceptanceRecorder(SkinAcceptanceRecorderDependencies);
    ~SkinAcceptanceRecorder();
    bool arm(std::string opaqueRunId, std::string scenarioId,
             SkinAcceptanceActivationKey);
    bool bindSession(const SkinAcceptanceSessionFacts &);
    void record(SkinFrameTelemetryEnvelope &&) noexcept;
    void recordResourceLifecycle(SkinResourceLifecycleSample) noexcept;
    void sessionEnded(const PlaySkinSessionIdentity &) noexcept;
    // Called only after the session catalog/resources have been destroyed.
    // For the negative scenario this queues the after-overlay digest.
    void sessionTeardownComplete(const PlaySkinSessionIdentity &) noexcept;
    // Polls provider-owned completion state only; safe from the main loop but
    // never performs hashing or filesystem access itself.
    void pollAsyncDependencies() noexcept;
    SkinAcceptanceExportTicket beginStopAndExport();
    SkinAcceptanceExportPollResult
    pollExport(SkinAcceptanceExportTicket) const;
    bool acknowledgeExport(SkinAcceptanceExportTicket) noexcept;
    std::optional<SkinAcceptanceExportTicket>
    currentExportTicket() const noexcept;
    SkinAcceptanceCaptureState state() const noexcept;
    void shutdown() noexcept;
  };

  enum class SkinAcceptanceCaptureState : std::uint8_t {
    Idle, Armed, WarmingUp, Recording, Exporting, Exported, Failed
  };
  struct SkinAcceptanceStartRequest {
    std::string opaqueRunId;
    std::string scenarioId;
  };

  struct GameplaySkinAcceptanceSnapshot {
    SkinAcceptanceCaptureState state = SkinAcceptanceCaptureState::Idle;
    std::optional<SkinAcceptanceExportTicket> exportTicket;
    std::optional<SkinAcceptanceExportResult> lastExport;
    std::string statusMessage;
  };
  using CurrentAcceptanceActivation =
      std::function<std::optional<SkinAcceptanceActivationKey>()>;
  class GameplaySkinAcceptanceController {
  public:
    GameplaySkinAcceptanceController(
        SkinAcceptanceRecorder &, CurrentAcceptanceActivation);
    const GameplaySkinAcceptanceSnapshot &snapshot() const noexcept;
    ControllerActionResult start(SkinAcceptanceStartRequest);
    ControllerActionResult stopAndExport();
    ControllerActionResult acknowledgeLastExport();
    void poll();
    void close() noexcept;
  };
  ```

  `ApplicationContext` owns one `SkinLiveResourceCounters`, one worker-backed `IAsyncSkinOverlayDigestProvider`, and one recorder constructed with the Documents root, `compiledSkinBuildIdentity()`, the frozen Task 1a scenario resolver, the provider, an atomic worker writer, and a bounded sanitized diagnostic-history snapshot callback. The provider outlives the recorder and is drained during context shutdown. Pressing Start in Settings does not claim that a gameplay session already exists: the controller obtains a validated four-field `SkinAcceptanceActivationKey`, calls `arm`, and may then be destroyed when the user leaves Settings. For a negative run, `arm` immediately queues the before-overlay digest; `bindSession` is rejected until a memory-only poll has produced that digest, ensuring it precedes chart startup. The matching `GamePlayScene` derives chart SHA-256, effective layout, actual display refresh, the canonical opaque guard-vector SHA-256 from the loaded validated model, and the new nonzero session serial from real chart/session/display state and calls `bindSession`; none of those threshold-critical facts is editable UI input. The observed guard-vector digest must exactly equal the scenario's expected digest and both are retained in metadata. Refresh must be in `1..240` and match the frozen scenario. A performance scenario binds exactly one five-field identity, discards the first 30 seconds by the bound session's visual clock, retains the next exact 180 seconds, records trusted first/last timestamps and duration, and automatically begins export; any identity/session change, incomplete frame, early chart end, clock reversal, sample overflow, wrong chart/layout/refresh/guard vector, or nonzero performed or denied render read/write/scan/upload counter makes the evidence fail. A resource-lifecycle scenario takes one baseline before the first entry, permits exactly ten explicitly bound sequential session serials whose other four fields match the armed key, and takes each post-exit sample only after the session catalog/textures have been destroyed; the tenth completed teardown triggers export. A render-I/O-negative scenario binds the frozen guard-vector digest, snapshots the overlay asynchronously before chart start, requires the exact `skin_file_render_phase_denied` diagnostic and `discard_frame_disable_session_same_frame_builtin` action, and treats the attempt as session-critical regardless of the caller's ordinary object criticality. After the matching session catalog/resources are destroyed, `sessionTeardownComplete` queues the after-overlay digest. Export is invalid unless memory-only polling obtains equal before/after digests, every performed counter is zero, the contract-selected denied-operation counter is nonzero, and no unrelated denied counter is nonzero.

  Enabled frame components receive a nullable pointer to one value-owned, allocation-reusing `SkinFrameTelemetryEnvelope`: `LuaSkinRuntime`, `LuaSkinFileSystem`, `SkinResourceCatalog`, `Skin2DRenderer`, `PlaySkinSession`, and `PlayfieldPresentationCoordinator` fill their distinct contribution bits against one frame serial/identity. `LuaSkinFileSystem` contributes separate denied/performed render read, write, and directory-scan deltas; the resource/renderer layers contribute separate denied/performed upload deltas. `PresentationFrameResult` carries the optional envelope to `GamePlayScene`; `SceneManager` resets the context's optional pending envelope before every render, and gameplay installs it once. The post-scene main loop adds missed-presentation/process-resident facts and the final bit, verifies the complete mask, then moves it into `record` exactly once. The main loop also calls `pollAsyncDependencies`, which only consumes provider-owned memory results and never hashes or touches the filesystem. Disabled recording constructs no envelope and does no sample allocation or merging. The app-owned live counters are incremented/decremented at actual catalog texture/resource creation/destruction and are independently sampled after teardown; per-frame counts are not used to fake the ten-cycle leak result. No instrumentation path performs file I/O while recording a frame.

  The recorder rejects an invalid/non-clean/placeholder build identity and injects its trusted compile-time commit/configuration into output. It retains at most 65,536 samples, treats any additional received sample as acceptance-fatal overflow, and owns at most one export work item plus one terminal result. Tickets are nonzero process-monotonic and never reused; polling distinguishes unknown/pending/ready and is idempotent until explicit acknowledgement. A new arm is rejected while an export or unacknowledged result exists, while a reopened Settings scene recovers the retained ticket through `currentExportTicket()`. At stop linearization, the main thread snapshots samples, lifecycle facts, resolved overlay digests, expected/observed guard-vector digests, and a bounded sanitized value-owned diagnostic vector before queuing the sole worker write to `Documents/SkinAcceptance/<opaqueRunId>.json`; the worker retains no service reference. The recorder cancels any outstanding digest ticket before shutdown; then the context drains the longer-lived digest provider. The destructor calls `shutdown`, which closes arm/bind/record, resolves the accepted export, joins the writer, and performs no render-thread I/O. Exports contain no pixels, third-party resource names, raw profile ID, or unique device identifier.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```python
def test_physical_evidence_stays_external(self):
    self.assertFalse(self.evidence_root.is_relative_to(self.repository_root))
    self.assertEqual(self.contract["criteriaStatus"], "pass")
```

- [ ] **Step 2: Write the telemetry, recorder, controller, build-identity, and acceptance-verifier tests first.** `skin_performance_telemetry_tests` requires the exact bounded summary fields/limit math above and drives fakes through runtime → filesystem/resource → renderer → session → coordinator → post-scene main-loop finalization, proving one real frame has one full five-field identity/frame serial, every contribution bit exactly once, and disabled recording does negligible/no allocation work. Test the fixed 65,536 capacity, received/retained/overflow counts, acceptance-fatal overflow rather than silent eviction, `1..240` refresh validation, trusted first/last visual timestamps and exact duration, incomplete/duplicate/out-of-order envelopes, wrong-session/profile/revision/configuration frames, an early session end, and separate performed/denied render read/write/directory-scan/upload counters. Passing runs require all eight counters zero. Test Idle→Armed→WarmingUp→Recording→Exporting→Exported/Failed, actual session binding from chart/layout/display facts, one-session performance rules, the separate baseline plus exactly ten post-destruction lifecycle samples with app-global counters returning to baseline, and Task 1a's session-critical negative scenario with exact expected/observed opaque guard-vector digests, diagnostic, fallback action, all performed counters zero, exactly the selected denied-operation counter nonzero, and equal before/after overlay digests. Use a fake asynchronous overlay provider to prove `arm` requests the before digest, binding is impossible while pending/failed, polling is memory-only, `sessionTeardownComplete` requests the after digest only after destruction, export waits for it, mismatch/failure is fatal, and shutdown cancels/drains safely. Exercise injected Documents root/build identity/scenario resolver/overlay provider/diagnostic snapshot, safe opaque filenames, a single in-flight plus single terminal export, never-reused typed tickets, unknown/pending/ready polling, explicit acknowledgement before a repeat, worker-only export, Settings close/reopen recovery, and shutdown/destructor draining. Reject arm without a validated activation key, and derive profile/entry/revision/configuration only from the callback; derive chart/layout/refresh/guard-vector digest/session serial only from `GamePlayScene`, never the start request. Extend iOS build/artifact tests first to require `AsoBMaShowBuildCommit`, `AsoBMaShowBuildConfiguration`, and `AsoBMaShowSourceClean` in the app Info.plist, matching compile definitions, plus a direct-install script that rejects dirty/wrong-commit checkouts and contains no Firebase/TestFlight/upload lane. The default clone/device-independent Python test validates every schema-v1 field, proves `verify` rejects `pending` criteria or missing external evidence, and checks every screenshot reference is a SHA-256/dimensions/timestamp record with no image payload in Git; it does not require a physical evidence root during default CTest. Final `verify` requires every criterion to be `pass`, the non-unique iPad hardware model identifier, exact iPadOS version, drawable resolution/safe area, actual configured Hz, app measurement commit/build configuration, SCURO archive/entry/configuration and payload-tree/activated-revision digests, chart hashes, autoplay scripts, trusted warm-up/measurement timestamps for three complete 180-second runs per layout/scenario, the exact negative render-I/O record, every expected/observed passing guard-vector digest, and the exact ten-cycle teardown samples. Reject an evidence root inside the repository and any public URL/absolute path/account name/device name/UDID in physical-evidence records; Task 1's official source and terms URLs remain allowed only in the dedicated provenance fields. Reject any tracked file whose digest matches Task 1's audited SCURO payload digest set regardless of extension. Require an access-controlled local evidence identifier, redaction status, retention-until date, and deletion procedure; never record a unique device identifier.
- [ ] **Step 3: Run the RED check**

  ```sh
  cmake --build cmake-build-debug --target skin_performance_telemetry_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_performance_telemetry_tests$' --output-on-failure
  python3 -m unittest tests/skin_acceptance_contract_tests.py tests/ios_build_setup_tests.py tests/ios_artifact_audit_tests.py -v
  ```

  Expected RED: telemetry/runner/schema enforcement is absent; the committed acceptance record remains `pending` until physical evidence exists.
- [ ] **Step 4: Use redistributable synthetic 7-key charts/assets to cover normal notes, every supported LN/CN/HCN phase, mines/invisible notes when audited, BPM, stops, scroll/speed changes, chords, every judgment grade, combo breaks, gauge thresholds/failure, lane cover, BGA base/layer/image/video plus channel-06 miss-sequence transitions/gaps, retry, and song end**. Record their hashes in the acceptance document. The poor/combo-break scenario must exercise Task 19's separate BGA clock and blank miss frame.
- [ ] **Step 5: Implement and wire build identity plus telemetry/recording end to end, then add native `Arm Next Gameplay Capture`, `Stop & Export Metrics`, and terminal-result acknowledgement controls to the gameplay-skins tab**. CMake obtains HEAD/configuration/clean status at configure time and supplies the three fixed `ASOBMASHOW_BUILD_*` definitions to `BuildIdentity.cpp`; tests inject their own values. Xcode defines the same values from `ASOBMASHOW_BUILD_COMMIT`, `CONFIGURATION`, and `ASOBMASHOW_SOURCE_CLEAN`, and mirrors them into the three Info.plist keys. `scripts/ios_artifact_audit.sh` requires the compiled/plist identity to equal its expected environment. No editable UI field supplies build identity, chart hash, layout, refresh rate, guard values, or session serial. Construct the narrow Settings controller with the app-owned recorder and Task 23 selection callback returning only the current validated four-field `SkinAcceptanceActivationKey`; `start` arms that key and resolves the chosen scenario ID through the frozen contract. Settings polls/shows Idle/Armed/WarmingUp/Recording/Exporting/Exported/Failed and displays the Files-visible relative path/digest; closing it never cancels an arm or accepted export, and reopening reconstructs state from the recorder's retained ticket. Implement the bounded no-follow overlay digest provider on its own worker; it derives the private overlay from the typed activation, accepts no host path, returns only a digest/failure, and is never invoked synchronously by a render call. `GamePlayScene` binds actual `SkinAcceptanceSessionFacts` including the loaded model's canonical opaque guard-vector digest, reports explicit post-destruction lifecycle/teardown samples, and moves the coordinator's identity-bearing envelope into the context. `SceneManager` resets that slot every frame and post-scene `main.cpp` alone finalizes/records it and polls only provider-owned memory completion. The runtime/filesystem/resource/renderer/session/coordinator hooks fill their distinct bits, while the app-global catalog counters measure actual teardown. The Python verifier consumes those JSON reports from the external evidence root and cross-checks their metadata/hashes against the manifest. Passing acceptance requires p99 total skin CPU time at or below 90% of the actual configured refresh interval, missed presentations at or below 0.5%, all performed and denied active-render filesystem read/write/directory-scan/resource-upload counters zero, matching expected/observed guard-vector digests, no overflow/incomplete/mismatched samples, no growth in live skin textures/resources after the tenth completed exit, and no more than 32 MiB resident-memory drift after warm-up. The negative scenario alone requires every performed counter zero and exactly its frozen denied-operation counter nonzero while proving the exact session-critical diagnostic/action, matching guard-vector digest, and asynchronously captured equal pre/post overlay digests; it is not a passing performance run.
- [ ] **Step 6: Run the opt-in source/package audit against the external SCURO package and pinned Beatoraja root**:

  ```sh
  aso_root="$(git rev-parse --show-toplevel)"
  beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
  python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean
  : "${SCURO_ARCHIVE_PATH:?set external SCURO archive path}"
  : "${SCURO_ARCHIVE_PACKAGE_PREFIX:?set . or the inferred wrapper}"
  : "${SCURO_ARCHIVE_SHA256:?set pinned SCURO archive digest}"
  : "${SCURO_SKIN_ROOT:?set corresponding extracted package root}"
  python3 scripts/audit_beatoraja_skin.py \
    --beatoraja-root "$beatoraja_ref_root" \
    --archive-path "$SCURO_ARCHIVE_PATH" \
    --archive-package-prefix "$SCURO_ARCHIVE_PACKAGE_PREFIX" \
    --skin-root "$SCURO_SKIN_ROOT" \
    --expected-archive-sha256 "$SCURO_ARCHIVE_SHA256" \
    --verify tests/fixtures/beatoraja_skin/reference_manifest.json
  ```

  Require archive SHA-256, archive payload-tree SHA-256, audited source-tree SHA-256, and inferred wrapper to match the frozen manifest. Confirm Task 1's recorded usage/private-screenshot terms still permit this acceptance work. Reconfirm the package's `legacyLuaApiSurface` is exactly the Task 9 closed facade: the two imports, File/Gdx class binds, configured-load `listFiles`, latent `mkdir`, and guarded absent-`Gdx.app` audio path are present, while `newInstance`, URL/HTTP, controllers/input, reflection, native access, or another class/member remain absent. Any remaining compatibility or permission gap blocks Task 25: create `docs/superpowers/plans/2026-08-03-beatoraja-lua-gameplay-skin-gap-remediation.md` with exact failing fixtures/files/interfaces via `superpowers:writing-plans`, implement it separately with RED/GREEN commits, then restart this task. Do not modify an unspecified production file or add an unaudited API inside acceptance work.
- [ ] **Step 7: Run the automation GREEN check before device measurement**

  ```sh
  cmake --build cmake-build-debug --target skin_performance_telemetry_tests -j 6
  ctest --test-dir cmake-build-debug -R '^skin_performance_telemetry_tests$' --output-on-failure
  python3 -m unittest tests/skin_acceptance_contract_tests.py tests/ios_build_setup_tests.py tests/ios_artifact_audit_tests.py -v
  cmake --build cmake-build-debug -j 6
  ctest --test-dir cmake-build-debug --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: telemetry, runtime capture/export, and verifier behavior pass while the manifest explicitly remains `pending`; no external device evidence is required by default tests.
- [ ] **Step 8: Commit the clean measurement candidate, then set `MEASUREMENT_COMMIT="$(git rev-parse HEAD)"`**. This commit contains every executable/native/script/test/fixture change used by the measured app; no production or acceptance-runner file may change after it without discarding evidence and restarting Task 25.

  ```sh
  git add CMakeLists.txt src/skin/CMakeLists.txt docs/skin-compat/beatoraja-lua-gameplay-contract.md docs/skin-compat/modernchic-scuro-4.02-acceptance.md tests/fixtures/beatoraja_skin/reference_manifest.json tests/fixtures/beatoraja_skin/charts src/BuildIdentity.h src/BuildIdentity.cpp src/skin/beatoraja/SkinPerformanceTelemetry.h src/skin/beatoraja/SkinPerformanceTelemetry.cpp src/skin/beatoraja/SkinAcceptanceRecorder.h src/skin/beatoraja/SkinAcceptanceRecorder.cpp src/skin/beatoraja/SkinOverlayDigestProvider.h src/skin/beatoraja/SkinOverlayDigestProvider.cpp src/skin/beatoraja/LuaSkinFileSystem.h src/skin/beatoraja/LuaSkinFileSystem.cpp src/skin/beatoraja/LuaSkinRuntime.h src/skin/beatoraja/LuaSkinRuntime.cpp src/skin/beatoraja/SkinResourceCatalog.h src/skin/beatoraja/SkinResourceCatalog.cpp src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp src/scene/play/PlayfieldPresentation.h src/scene/play/PlayfieldPresentationCoordinator.h src/scene/play/PlayfieldPresentationCoordinator.cpp src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp src/scene/GameplaySkinSettingsController.h src/scene/GameplaySkinSettingsController.cpp src/scene/SettingsScene.h src/scene/SettingsSceneSkins.cpp src/context.h src/main.cpp ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj scripts/ios_artifact_audit.sh scripts/ios_build_install_for_skin_acceptance.sh scripts/run_skin_acceptance.py tests/skin_acceptance_contract_tests.py tests/ios_artifact_audit_tests.py tests/ios_build_setup_tests.py tests/skin_performance_telemetry_tests.cpp
  git commit -m "feat: add gameplay skin acceptance telemetry"
  MEASUREMENT_COMMIT="$(git rev-parse HEAD)"
  test -z "$(git status --porcelain)"
  ```

- [ ] **Step 9: Build/install exactly `MEASUREMENT_COMMIT` as a development-signed build directly from Xcode to the connected physical iPad; do not use Firebase or TestFlight**. Use a private ephemeral device identifier only for installation; never write it to metrics/evidence:

  ```sh
  : "${IOS_ACCEPTANCE_DEVICE_ID:?set connected iPad CoreDevice identifier privately}"
  : "${IOS_DEVELOPMENT_TEAM:?set development team privately}"
  scripts/ios_build_install_for_skin_acceptance.sh \
    --commit "$MEASUREMENT_COMMIT" \
    --configuration Release \
    --device-id "$IOS_ACCEPTANCE_DEVICE_ID" \
    --development-team "$IOS_DEVELOPMENT_TEAM"
  ```

  The script requires `HEAD == --commit`, a clean checkout, runs `scripts/ios_init.sh`, builds scheme `AsoBMaShow` with `xcodebuild -project ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj -scheme AsoBMaShow -configuration Release -destination id=... -derivedDataPath <checkout-specific path> ASOBMASHOW_BUILD_COMMIT=... ASOBMASHOW_SOURCE_CLEAN=1 DEVELOPMENT_TEAM=... CODE_SIGN_STYLE=Automatic build`, verifies the resulting app's three Info.plist identity keys and artifact audit, then installs only with `xcrun devicectl device install app --device ...`. It prints no device ID and performs no distribution/upload. Verify all three install paths for the same unmodified package: ZIP picker, folder picker, and manual unarchived copy into `Files > On My iPad > AsoBMaShow > Skins/ModernChic`. For each path, record the activated revision digest and require it to equal the frozen archive payload-tree digest before any manual edit; confirm app-imported packages are visible/editable there.
- [ ] **Step 10: Verify startup/tab/manual rescans; option/file/offset/layout persistence across restart, profile switch, reimport, and valid edit; whole-package collision behavior; manual partial/invalid edit preserving the active revision; no mid-chart hot reload; package deletion allowing the active chart to finish but forcing built-in fallback next chart; and malformed/runtime-failing same-frame fallback. In a validation-only load/configuration run, exercise every Task 1a audited file-I/O shape—including the configured-load legacy `File.listFiles` path—and prove only virtual package paths and overlay effects are observable. In the separate frozen negative gameplay run, apply the configuration whose canonical opaque guard-vector digest matches the scenario and reach the audited render-time operation: regardless of ordinary object criticality, require diagnostic `skin_file_render_phase_denied`, action `discard_frame_disable_session_same_frame_builtin`, every performed counter zero, exactly the frozen denied-operation counter nonzero, no render-time filesystem mutation, and asynchronously captured identical before/after overlay digests. Every passing performance/layout run must derive the loaded model's canonical guard-vector digest and match Task 1a's expected passing digest so no selected render-I/O branch is reachable. Do not rely on a named option as the complete guard set.**
- [ ] **Step 11: Capture 4:3 Fit, Stretch, and Custom screenshots at the manifest timestamps and compare object placement, BGA, notes/LNs, judge, combo, gauge, HUD, lane cover, and touch regions to the six synthetic renderer goldens/expected-command evidence**. Keep every physical SCURO screenshot in the access-controlled external evidence root because it contains third-party assets; redact Files/account/device UI before retention, never publish it, and commit only its SHA-256, pixel dimensions, timestamp, measured landmarks/deltas, opaque evidence identifier, redaction status, retention-until date, and deletion procedure. Exercise native Reset Layout and verify it remains reachable.
- [ ] **Step 12: Fill only the tracked acceptance document/manifest evidence records, then run the final GREEN check against `MEASUREMENT_COMMIT`**:

  ```sh
  aso_root="$(git rev-parse --show-toplevel)"
  beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
  python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean
  : "${SCURO_ARCHIVE_PATH:?set external SCURO archive path}"
  : "${SCURO_ARCHIVE_PACKAGE_PREFIX:?set . or the inferred wrapper}"
  : "${SCURO_ARCHIVE_SHA256:?set pinned SCURO archive digest}"
  : "${SCURO_SKIN_ROOT:?set corresponding extracted package root}"
  : "${SCURO_ACCEPTANCE_EVIDENCE_ROOT:?set external physical-iPad evidence root}"
  : "${MEASUREMENT_COMMIT:?set Task 25 measurement commit}"
  test -z "$(git diff --name-only "$MEASUREMENT_COMMIT" | rg -v '^(docs/skin-compat/modernchic-scuro-4\.02-acceptance\.md|tests/fixtures/beatoraja_skin/reference_manifest\.json)$')"
  python3 scripts/run_skin_acceptance.py verify \
    --contract tests/fixtures/beatoraja_skin/reference_manifest.json \
    --evidence-root "$SCURO_ACCEPTANCE_EVIDENCE_ROOT" \
    --expected-app-commit "$MEASUREMENT_COMMIT"
  python3 scripts/audit_beatoraja_skin.py \
    --beatoraja-root "$beatoraja_ref_root" \
    --archive-path "$SCURO_ARCHIVE_PATH" \
    --archive-package-prefix "$SCURO_ARCHIVE_PACKAGE_PREFIX" \
    --skin-root "$SCURO_SKIN_ROOT" \
    --expected-archive-sha256 "$SCURO_ARCHIVE_SHA256" \
    --verify tests/fixtures/beatoraja_skin/reference_manifest.json
  python3 -m unittest tests/skin_acceptance_contract_tests.py tests/ios_release_workflow_tests.py tests/ios_build_setup_tests.py tests/ios_artifact_audit_tests.py -v
  cmake --build cmake-build-debug -j 6
  ctest --test-dir cmake-build-debug --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  ```

  Expected GREEN: no unknown critical manifest dependencies, all optional gaps are explicit, every status is `pass`, all automated tests pass, the unsigned iOS build passes, and physical-iPad evidence meets the fixed thresholds.
- [ ] **Step 13: Commit only the evidence summary**

  ```sh
  git add docs/skin-compat/modernchic-scuro-4.02-acceptance.md tests/fixtures/beatoraja_skin/reference_manifest.json
  test -z "$(git diff --cached --name-only | rg -v '^(docs/skin-compat/modernchic-scuro-4\.02-acceptance\.md|tests/fixtures/beatoraja_skin/reference_manifest\.json)$')"
  git commit -m "test: close SCURO gameplay skin acceptance"
  ```

### Task 26: Final review and milestone verification

**Reference refresh:** baseline files and every Beatoraja file named in the completed compatibility contract.

**Files:**

- Create: `docs/skin-compat/beatoraja-lua-gameplay-final-review.md`
- Modify: `docs/skin-compat/modernchic-scuro-4.02-acceptance.md`

**Interfaces:**

- Consumes: explicit `SCURO_ARCHIVE_PATH`, `SCURO_ARCHIVE_PACKAGE_PREFIX`, `SCURO_ARCHIVE_SHA256`, `SCURO_SKIN_ROOT`, `SCURO_ACCEPTANCE_EVIDENCE_ROOT`, `SCURO_MEASUREMENT_COMMIT`, pinned Beatoraja root, Task 25's clean evidence-closure commit, and the measurement commit recorded in the manifest.
- Produces: a code-reviewed `REVIEWED_COMMIT` plus one docs-only descendant `FINAL_COMMIT` for which the opt-in external audit, physical-evidence verifier bound to `SCURO_MEASUREMENT_COMMIT`, Python contracts, full CTest suite, desktop target, and unsigned iOS verification all exit 0. The final diff may contain only the review record and its acceptance-document link; no command rewrites evidence, deploys, or uploads.

- [ ] **Step 1: Refresh the pinned Beatoraja reference** — Run the mandatory checker (or Task 1 bootstrap), reopen every file in this task's **Reference refresh** from `beatoraja_ref_root`, and record the pinned path/symbol/behavior before writing the test or editing production code.

**RED test anchor:**

```sh
REVIEWED_COMMIT="$(git rev-parse HEAD)"
test -z "$(git status --porcelain)"
test "$(git rev-parse HEAD)" = "$REVIEWED_COMMIT"
```

- [ ] **Step 2: Set `REVIEWED_COMMIT="$(git rev-parse HEAD)"`, require a clean tree, and use `superpowers:requesting-code-review` for an independent review of that exact commit focused on sandbox escapes, archive/path races, publication recovery, Lua quotas, gameplay mutation, render-time work, command ordering, fallback atomicity, BGA ordering, touch geometry, profile isolation, and iOS cancellation/lifetime.**
- [ ] **Step 3: Record `REVIEWED_COMMIT`, reviewer, scope, and findings in the final-review document**. Any actionable finding blocks Task 26: use `superpowers:writing-plans` to create an exact remediation plan, implement each finding with a RED regression and narrow GREEN fix in separate scoped commits, rerun Task 25 scenarios affected by the fix, and then restart Task 26 with a new measurement/review when executable behavior changed. Do not edit an unspecified implementation file inside this final-verification task.
- [ ] **Step 4: Review `git diff` and `git status` for third-party assets, absolute local paths, secrets, private package contents, generated temporary files, unrelated user changes, parser edits, deployment commands, and accidental network/native Lua capabilities**. The ModernChic/SCURO archive and extracted files must remain outside Git.
- [ ] **Step 5: Commit the final review record and its link from the acceptance document, then set `FINAL_COMMIT="$(git rev-parse HEAD)"`**. Require `FINAL_COMMIT^ == REVIEWED_COMMIT` and require `git diff --name-only "$REVIEWED_COMMIT" "$FINAL_COMMIT"` to contain only the two files listed for this task. No tracked file may change afterward.

  ```sh
  git add docs/skin-compat/beatoraja-lua-gameplay-final-review.md docs/skin-compat/modernchic-scuro-4.02-acceptance.md
  git commit -m "docs: record gameplay skin milestone verification"
  ```

- [ ] **Step 6: Use `superpowers:verification-before-completion` and run exactly**:

  ```sh
  aso_root="$(git rev-parse --show-toplevel)"
  beatoraja_ref_root="${ASOBMASHOW_BEATORAJA_ROOT:-$(cd "$aso_root/.." && pwd)/beatoraja}"
  python3 scripts/check_beatoraja_reference.py --root "$beatoraja_ref_root" --require-clean

  : "${SCURO_ARCHIVE_PATH:?set external SCURO archive path}"
  : "${SCURO_ARCHIVE_PACKAGE_PREFIX:?set . or the inferred wrapper}"
  : "${SCURO_ARCHIVE_SHA256:?set pinned SCURO archive digest}"
  : "${SCURO_SKIN_ROOT:?set external extracted SCURO root}"
  : "${SCURO_ACCEPTANCE_EVIDENCE_ROOT:?set external evidence root}"
  : "${SCURO_MEASUREMENT_COMMIT:?set Task 25 measured app commit}"

  FINAL_COMMIT="$(git rev-parse HEAD)"
  REVIEWED_COMMIT="$(git rev-parse HEAD^)"
  test -z "$(git status --porcelain)"
  test -z "$(git diff --name-only "$REVIEWED_COMMIT" "$FINAL_COMMIT" | rg -v '^(docs/skin-compat/beatoraja-lua-gameplay-final-review\.md|docs/skin-compat/modernchic-scuro-4\.02-acceptance\.md)$')"
  test "$(shasum -a 256 "$SCURO_ARCHIVE_PATH" | awk '{print $1}')" = "$SCURO_ARCHIVE_SHA256"

  python3 scripts/audit_beatoraja_skin.py \
    --beatoraja-root "$beatoraja_ref_root" \
    --archive-path "$SCURO_ARCHIVE_PATH" \
    --archive-package-prefix "$SCURO_ARCHIVE_PACKAGE_PREFIX" \
    --skin-root "$SCURO_SKIN_ROOT" \
    --expected-archive-sha256 "$SCURO_ARCHIVE_SHA256" \
    --verify tests/fixtures/beatoraja_skin/reference_manifest.json

  python3 scripts/run_skin_acceptance.py verify \
    --contract tests/fixtures/beatoraja_skin/reference_manifest.json \
    --evidence-root "$SCURO_ACCEPTANCE_EVIDENCE_ROOT" \
    --expected-app-commit "$SCURO_MEASUREMENT_COMMIT"

  python3 -m unittest \
    tests/beatoraja_skin_reference_tests.py \
    tests/skin_acceptance_contract_tests.py \
    tests/ios_release_workflow_tests.py \
    tests/ios_build_setup_tests.py \
    tests/ios_artifact_audit_tests.py -v
  cmake --build cmake-build-debug -j 6
  ctest --test-dir cmake-build-debug --output-on-failure
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_release_verify.sh
  git diff --check
  test -z "$(git status --porcelain)"
  test "$(git rev-parse HEAD)" = "$FINAL_COMMIT"
  ```

  Expected GREEN: every command exits 0, the working tree and `FINAL_COMMIT` remain unchanged, every approved completion criterion is represented in verified evidence, and no distribution upload occurs. Do not push, merge, deploy, or upload without a separate explicit request.

## Plan Self-Review Checklist

- [x] Every approved product decision maps to at least one numbered task and one test/acceptance assertion.
- [x] Every implementation task begins with a direct Beatoraja reference refresh and has explicit RED, implementation, GREEN, and commit steps; Task 26 applies the same regression discipline conditionally to review findings.
- [x] All new production files have one focused responsibility and a named CMake owner.
- [x] Interfaces use consistent names: `SkinEntryId`, `SkinRevisionLease`, `SkinProfileSettings`, `LuaSkinRuntime`, `BeatorajaSkinModel`, `PlayfieldVisualState`, `PlaySkinStateBridge`, `PlaySkinSession`, and `PlayfieldPresentationCoordinator`.
- [x] Every value and path needed to begin a task is concrete, and later measured evidence is produced by a named command or script before dependent work proceeds.
- [x] ModernChic/SCURO remains external; committed fixtures and traces are redistributable.
- [x] iOS verification uses `scripts/ios_release_verify.sh`; no task uploads a build.
