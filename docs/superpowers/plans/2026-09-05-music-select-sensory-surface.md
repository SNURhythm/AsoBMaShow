# Beatoraja Music-Select Sensory Surface Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. Execute
> inline in the current checkout.

**Goal:** Close the confirmed sensory-surface gaps in the Beatoraja type-5
music-select skin path: play select sound effects, default select BGM and
preview parity with pinned `PreviewMusicProcessor`, the small unmapped input
and event branches, and prove (rather than implement) that movie sources
already promote through the shared catalog.

**Context:** The type-5 spine is complete and ledger-covered
(`docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`). The plan
`docs/skin-compat/beatoraja-music-select-gaps.md` inventories the sensory
surface. That report's claim #1 (JSON/Lua never emit movies) is **superseded**:
`SkinMovieCatalog::resolveMovies` already promotes `SkinImageResource` to
`SkinMovieResource` by extension
(`src/skin/beatoraja/SkinMovieCatalog.cpp:154-157,185-187`), the Lua filesystem
imposes no image-extension gating (`LuaSkinFileSystem.cpp:959-969`), and the
select session and renderer both pass movies through
(`MusicSelectSkinSession.cpp:798,823`). Missing movie coverage is a **test**
gap, not an implementation gap.

**Spec:**
`docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md` —
the pinned source authority is Beatoraja commit
`c2ed5db1a46145ed10790c3872f717e95b59db9d`.

## Global Constraints

- Compatibility authority is exactly Beatoraja commit
  `c2ed5db1a46145ed10790c3872f717e95b59db9d` at
  `/Users/xf/workspace/SNURhythm/beatoraja`.
- Do not touch files owned by the concurrent chart-db rework review:
  `src/ArchiveFile.*`, `src/ChartLibraryScanner.cpp`,
  `src/library/ChartLibraryOperations.cpp`, `src/repositories/ChartRepository.*`,
  `src/repositories/ChartScanStore.*`, `src/scene/ChartPreloadWorker.*`,
  `tests/archive_file_concurrency_tests.cpp`,
  `tests/chart_library_scanner_tests.cpp`,
  `tests/chart_preload_worker_tests.cpp`, and the chart-scan checkpointing and
  archive-index-cache code paths. This plan's work lives in
  `src/music_select/`, `src/scene/MusicSelectScene.cpp`,
  `src/skin/beatoraja/`, `src/audio/` interfaces, and focused tests.
- Add no skin validation absent from the pinned source. Keep existing
  repository and gameplay invariants authoritative.
- Preserve source ordering, sentinel values, timer transitions, and exact
  source-defined no-ops.
- Make each commit a coherent production slice with its focused tests.
- Do not modify parser amalgamation files or run a whole-file formatter.
- The selected-skin failure policy stays: no fallback to Built-in, error
  screen preserves stored selection.
- Reuse existing media plumbing (`AudioWrapper` skin-sound APIs,
  `LuaSkinApplicationAudioBackend`, `SkinMovieCatalog`) instead of adding new
  subsystems.

## Task 1: Prove existing movie-source promotion with focused tests

**Files:**
- Create: `tests/skin_movie_catalog_types_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `docs/skin-compat/beatoraja-music-select-gaps.md` (mark claim #1
  verified/covered)

**Goal:** Show a JSON/Lua-shaped model that references a movie-extension
`SkinImageResource` through a used `SkinImageObject` promotes to a prepared
`SkinMovieResource` today. No production behavior change.

- [ ] **Step 1: Write failing promotion tests**

Model an `SkinImageResource` with `virtualPath = "resources/movie.mp4"` and a
`SkinImageObject` referencing it, reuse the `FakeMovieDevice` pattern from
`tests/skin_resource_catalog_tests.cpp`, and assert `SkinMovieCatalog::prepare`
loads one device path, deduplicates, and that `findMovie(id)` returns the
prepared resource. Add the negative assertion that a `.png` path stays an
image (no promotion).

- [ ] **Step 2: Run and observe the absent target failure**

Run: `cmake --build cmake-build-debug --target skin_movie_catalog_types_tests -j 6`

Expected: compilation fails because the target does not exist.

- [ ] **Step 3: Register the test and implement the assertions**

Add the executable to `CMakeLists.txt` next to `skin_resource_catalog_tests`
(which builds the shared `SkinMovieCatalog` machinery). Copy the fixture
setup from `skin_resource_catalog_tests.cpp` (leased filesystem, fake device).

- [ ] **Step 4: Mutation-check**

Temporarily point the object at a `.png` path and confirm the promotion
assertion fails, then restore.

- [ ] **Step 5: Run and commit**

Run: `cmake --build cmake-build-debug --target skin_movie_catalog_types_tests -j 6 && ./cmake-build-debug/skin_movie_catalog_types_tests`

Expected: PASS.

```bash
git add CMakeLists.txt tests/skin_movie_catalog_types_tests.cpp docs/skin-compat/beatoraja-music-select-gaps.md
git commit -m "test: prove music select movie source promotion"
```

## Task 2: Select sound-effect wiring

**Files:**
- Modify: `src/music_select/MusicSelectEventController.h`
- Modify: `src/scene/MusicSelectScene.cpp`
- Create: `src/audio/SkinSystemSoundService.h`
- Create: `src/audio/SkinSystemSoundService.cpp`
- Create: `tests/music_select_system_sound_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `docs/skin-compat/beatoraja-music-select-gaps.md`

**Goal:** Play the pinned select SEs. The controller already emits
`OptionChangeSound` (`src/music_select/MusicSelectEventController.cpp:55-64`)
and the input processor already emits `ScratchSound`
(`src/music_select/MusicSelectInputProcessor.cpp:233,238,350,356`); the scene
drops all of them (`MusicSelectScene::executeEvent` has no sound case,
`applyInputAction` has no scratch case). Folder open/close play nothing.

- [ ] **Step 1: Write failing sound tests**

Add `MusicSelectScene`-level tests: assert a consumed `OptionChangeSound`
effect invokes the sound service's `optionChange`; a `ScratchSound` input
action invokes `scratch`; `openDirectory`/`closeDirectory` invoke
`folderOpen`/`folderClose`. Use an injected fake sound service.

- [ ] **Step 2: Run and observe the absent service failure**

Run: `cmake --build cmake-build-debug --target music_select_system_sound_tests -j 6`

Expected: compilation fails because the service is absent.

- [ ] **Step 3: Implement the system-sound service**

`SkinSystemSoundService` owns the pinned `SystemSoundManager.SoundType` select
subset (`SCRATCH`, `FOLDER_OPEN`, `FOLDER_CLOSE`, `OPTION_CHANGE`,
`OPTION_OPEN`, `OPTION_CLOSE`, `SELECT`, `DECIDE`) with the resource paths
Beatoraja ships (`SystemSoundManager.java:129-151`), and plays them through
`AudioWrapper` skin-sound APIs (same path as
`MusicSelectPreviewAudioService.cpp:80-96`). It can be backed by
`defaultsound/` asset paths; make load failures warnings, not errors.

- [ ] **Step 4: Wire the scene**

`executeEvent` handles `OptionChangeSound`; `applyInputAction` handles
`ScratchSound`; `openDirectory`/`closeDirectory` play folder open/close. The
service instance is created in `init()` and torn down cleanly.

- [ ] **Step 5: Run and commit**

Run: `cmake --build cmake-build-debug --target music_select_system_sound_tests music_select_event_controller_tests music_select_input_processor_tests -j 6 && ./cmake-build-debug/music_select_system_sound_tests && ./cmake-build-debug/music_select_event_controller_tests && ./cmake-build-debug/music_select_input_processor_tests`

Expected: PASS.

```bash
git add src/CMakeLists.txt CMakeLists.txt src/music_select/MusicSelectEventController.h src/scene/MusicSelectScene.cpp src/audio/SkinSystemSoundService.h src/audio/SkinSystemSoundService.cpp tests/music_select_system_sound_tests.cpp docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json docs/skin-compat/beatoraja-music-select-gaps.md
git commit -m "feat: play music select sound effects"
```

## Task 3: Default select BGM and preview parity

**Files:**
- Modify: `src/music_select/MusicSelectPreview.h`
- Modify: `src/music_select/MusicSelectPreview.cpp`
- Modify: `src/music_select/MusicSelectPreviewAudioService.cpp`
- Modify: `src/scene/MusicSelectScene.cpp`
- Modify: `tests/music_select_preview_tests.cpp`
- Modify: `src/skin/beatoraja/MusicSelectSkinStateBridge.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-gaps.md`

**Goal:** Default select BGM + fade parity with pinned `PreviewMusicProcessor`
(`PreviewMusicProcessor.java:41-43,79-101,115-133`), `DECIDE` sound on launch,
and suppress-preview-when-silent handling.

- [ ] **Step 1: Write failing parity tests**

Extend `music_select_preview_tests.cpp`: a chart with no `#PREVIEW`
falls back to the default select BGM path. A selection move to an empty
preview routes back to the default. `launchSelected` requests a `DECIDE`
sound. The config `SongPreview` mode (`NONE`/`ONCE`/`LOOP`) and
`systemvolume` reach the worker.
/SongPreview is not yet surfaced in AsoBMaShow settings; if adding it is
beyond this slice, gate parity on the existing
`archiveChartPreviewEnabled` and note the remaining mode gap in the plan.

- [ ] **Step 2: Run and observe the failing parity**

Run: `cmake --build cmake-build-debug --target music_select_preview_tests -j 6 && ./cmake-build-debug/music_select_preview_tests`

Expected: the new assertions fail.

- [ ] **Step 3: Implement default-BGM parity**

`MusicSelectPreviewAudioService` accepts a default path (the select BGM
asset) and a fade-back; when a preview finishes it fades to the default
(`PreviewMusicProcessor.java:97-101`). Wire `DECIDE` on launch through the
system-sound service.

- [ ] **Step 4: Run and commit**

Run: `cmake --build cmake-build-debug --target music_select_preview_tests music_select_system_sound_tests -j 6 && ./cmake-build-debug/music_select_preview_tests && ./cmake-build-debug/music_select_system_sound_tests`

Expected: PASS.

```bash
git add src/music_select/MusicSelectPreview.h src/music_select/MusicSelectPreview.cpp src/music_select/MusicSelectPreviewAudioService.cpp src/scene/MusicSelectScene.cpp src/skin/beatoraja/MusicSelectSkinStateBridge.cpp tests/music_select_preview_tests.cpp docs/skin-compat/beatoraja-music-select-gaps.md
git commit -m "feat: default music select bgm and preview parity"
```

## Task 4: Input and event branch closure

**Files:**
- Modify: `src/music_select/MusicSelectInputProcessor.h`
- Modify: `src/music_select/MusicSelectInputProcessor.cpp`
- Modify: `src/scene/MusicSelectScene.cpp`
- Modify: `tests/music_select_input_processor_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-gaps.md`

**Goal:**
- Add `Num6` (`SDLK_6`) to the control-key set and route it to the same
  Settings entry the toolbar uses, matching pinned `MusicSelector.input()`
  (`NUM6` → CONFIG, `MusicSelector.java:296-300`).
- Distinguish `keyconfig` (13) from `skinconfig` (14): `keyconfig` opens the
  key-config surface if one exists, else Settings; `skinconfig` stays
  Settings. If no key-config surface exists, keep both on Settings and
  record that decision in the plan.

- [ ] **Step 1: Write failing input tests**

Cover `SDLK_6`/`Num6` mapping and the `keyconfig`/`skinconfig` event split.

- [ ] **Step 2: Run and observe the missing branch**

Run: `cmake --build cmake-build-debug --target music_select_input_processor_tests -j 6`

Expected: the new assertions fail.

- [ ] **Step 3: Implement the branches**

- [ ] **Step 4: Run and commit**

Run: `cmake --build cmake-build-debug --target music_select_input_processor_tests music_select_event_controller_tests -j 6 && ./cmake-build-debug/music_select_input_processor_tests && ./cmake-build-debug/music_select_event_controller_tests`

Expected: PASS.

```bash
git add src/music_select/MusicSelectInputProcessor.h src/music_select/MusicSelectInputProcessor.cpp src/scene/MusicSelectScene.cpp tests/music_select_input_processor_tests.cpp docs/skin-compat/beatoraja-music-select-gaps.md
git commit -m "feat: close music select input and event branches"
```

**Note (Task 4 rulings):** No key-config surface exists in this codebase
(checked: nothing under `src/scene/` or `src/view/` provides a key-config
screen). `keyconfig` (13) and `skinconfig` (14) therefore both route to
Settings, but they are kept as **structurally separate cases** in
`MusicSelectEventController::execute` so a future key-config surface can slot
into the `keyconfig` case. The pinned `OPEN_SKIN_CONFIGURATION` → `SKINCONFIG`
branch (gap #8) was deliberately NOT added; it needs a skin-config destination
and re-activation semantics and is out of scope.

## Task 5: Runtime evidence closure

**Files:**
- Modify only source/test/ledger/gap files from Tasks 1-4 when a focused
  failure proves a runtime defect.

- [ ] **Step 1: Refresh and inspect ledger assignments**

Run the ledger tests:

`python3 -m unittest tests/beatoraja_music_select_skin_ledger_tests.py tests/beatoraja_music_select_skin_ledger_evidence_tests.py -v`

- [ ] **Step 2: Run the focused suite**

`ctest --test-dir cmake-build-debug --output-on-failure -R 'music_select|preview' -j 6`

- [ ] **Step 3: Run desktop compile and diff checks**

Run: `cmake --build cmake-build-debug --target main -j 6 && git diff --check`

Expected: PASS and no whitespace errors.

- [ ] **Step 4: Keep corrections in their owning feature slice**

Return a defect to its focused failing test and amend the owning coherent
slice instead of creating a cleanup commit.

## Verification

- Focused tests for each task, plus the ledger and evidence suites in
  `cmake-build-debug`.
- Concurrent chart-db rework files remain untouched
  (`git diff --name-only` never lists them).
- No whole-file formatter used; formatting matches surrounding style.

## Task 6: User-configurable select sound set

**Files:**
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/audio/SkinSystemSoundService.h`
- Modify: `src/audio/SkinSystemSoundService.cpp`
- Modify: `src/scene/MusicSelectScene.cpp`
- Modify: `src/scene/SettingsSceneSkins.cpp`
- Modify: `tests/music_select_system_sound_tests.cpp`
- Modify: `tests/app_settings_store_tests.cpp`
- Modify: `tests/gameplay_skin_settings_tests.cpp` (if the default changes any assertion)
- Modify: `docs/skin-compat/beatoraja-music-select-gaps.md`

**Goal:** Make the select SEs + default BGM/decide actually audible by letting
the user point the app at a Beatoraja sound-set folder (ModernChic's `Sound/`
etc.), and resolve every sound across Beatoraja's full supported-extension
set.

[Rulings that bind this task]
- Extension authority is pinned Beatoraja `AudioDriver.getPaths`
  (`AudioDriver.java:158`): `.wav`, `.flac`, `.ogg`, `.mp3`, tried in that
  order. "All supported extensions" means exactly these four — they are
  Beatoraja's authorities and libsndfile (`decodeAudioToPCMBounded`) decodes
  each by content. Formats outside that list (m4a/aac/wma/opus) are NOT added.
- New setting `AppSettings::skinSelectSoundSetPath` (std::string, default
  empty = bundled `assets/` fallback unchanged). Persisted via
  `AppSettingsStore` like `skinSortId`/`skinModeFilterName`.
- Resolution order is: configured sound-set folder first, then the bundled
  `assets/` root; within a folder, Beatoraja ext order wins (`.wav` beats
  `.ogg`).
- The setting is edited as a typed path in the Music Select section of
  `SettingsSceneSkins` using the existing `appendText`-style input row. The
  app has no folder-picker widget; typed path is the consistent existing
  pattern. Note this limitation in the gap doc.
- Keep the existing missing-asset warning (warn, never fail).

- [ ] **Step 1: Write failing tests**
  - Service resolve helper: a temp sound-set dir with `f-open.ogg` resolves
    `FolderOpen` to that path; `scratch.wav` + `scratch.ogg` picks `.wav`;
    sound-set dir beats bundled; missing everywhere → warning + no crash; the
    combined search roots [`set` (if any), bundled] is honored per sound.
  - AppSettingsStore round-trip: `skinSelectSoundSetPath` persists and reads
    back, default empty.
  - Multi-extension: each of `.wav/.flac/.ogg/.mp3` resolves when it is the
    only match in the sound-set dir.

- [ ] **Step 2: Run and observe the failing resolve**

Run: `cmake --build cmake-build-debug --target music_select_system_sound_tests app_settings_store_tests -j 6 && ./cmake-build-debug/music_select_system_sound_tests && ./cmake-build-debug/app_settings_store_tests`

Expected: the extension/precedence/resolution assertions fail.

- [ ] **Step 3: Implement**
  - `SkinSystemSoundService` constructor takes
    `std::span<const std::filesystem::path> searchRoots` (configured set dir
    first, bundled root last). Add a free testable helper
    `musicSelectSystemSoundPath(searchRoots, MusicSelectSystemSound)` that
    returns the first existing `{name}.{ext}` across roots in Beatoraja ext
    order, or `std::nullopt` (warn when nothing matches).
  - `MusicSelectScene` builds search roots from
    `context.settings.skinSelectSoundSetPath` (when non-empty) plus
    `kSkinSoundAssetRoot`, passes them to `SkinSystemSoundService`, and uses
    the same helper for the default-BGM/decide path resolution (so a set with
    `select.ogg`/`decide.flac` is honored).
  - `SettingsSceneSkins`: a "Sound set folder" text-input row in the Music
    Select section editing `skinSelectSoundSetPath` and persisting.
  - `AppSettingsStore`: write/read `"skinSelectSoundSetPath"`.
  - Gap doc: update #4/#5 to say SEs + select/decide resolve across the user's
    sound-set folder in all four Beatoraja extensions; note the typed-path
    input (no folder picker).

- [ ] **Step 4: Run and commit**

Run: `cmake --build cmake-build-debug --target music_select_system_sound_tests app_settings_store_tests music_select_preview_tests main -j 6 && ./cmake-build-debug/music_select_system_sound_tests && ./cmake-build-debug/app_settings_store_tests && ./cmake-build-debug/music_select_preview_tests`

Expected: PASS.

```bash
git add src/AppSettings.h src/AppSettings.cpp src/AppSettingsStore.cpp src/audio/SkinSystemSoundService.h src/audio/SkinSystemSoundService.cpp src/scene/MusicSelectScene.cpp src/scene/SettingsSceneSkins.cpp tests/music_select_system_sound_tests.cpp tests/app_settings_store_tests.cpp docs/skin-compat/beatoraja-music-select-gaps.md
git commit -m "feat: configurable music select sound set with full extension support"
```

## Task 7: Bundle-aware skin-sound loading + platform folder picker for the sound set

**Files:**
- Modify: `src/audio/AudioWrapper.cpp`
- Modify: `src/scene/MusicSelectScene.cpp`
- Modify: `src/scene/SettingsSceneSkins.cpp`
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/library/ChartLibraryPlatform.h`
- Modify: `src/library/ChartLibraryPlatform.cpp`
- Modify: `src/iOSNatives.mm` (only if a bookmark resumption helper is needed)
- Modify: `src/AndroidNatives.h` (only if a wrapped SAF folder-pick for app-owned folders is needed)
- Modify: `tests/audio_wrapper_lifecycle_tests.cpp` (or a new focused test)
- Modify: `docs/skin-compat/beatoraja-music-select-gaps.md`

**Root cause (from debugging):** The bundled select sounds are silent on iPad because the audio load path (`AudioWrapper::loadSkinSound` → `asobmashow::audio::openSoundFile` → libsndfile `sf_open`) opens the literal filesystem path `assets/select.wav`. On iOS there is no chdir and no bundle resource-path resolution, so that relative path does not exist in the sandbox; the same is true inside a macOS `.app` bundle (app chdir's to `Contents/MacOS`, assets live in `Contents/Resources/assets`). Fonts and skin images render because they read via `SDL_RWFromFile` (bundle-aware: SDL maps to `[NSBundle mainBundle] pathForResource` on iOS), but libsndfile `sf_open` is a plain filesystem open.

**Established patterns to reuse (already in the codebase):**
- `SkinResourceCatalog::readPlatformAsset` (`src/skin/beatoraja/SkinResourceCatalog.cpp:90-127`) reads an asset via `SDL_RWFromFile`.
- `Jukebox` loads chart audio as `loadSoundFromMemory(path, bytes, isCancelled)` (`src/audio/Jukebox.cpp:2410` etc.) — a bytes path.
- `AudioWrapper::decodeAudioBytesToPCM` (`decoder.h:22`) decodes bytes via `sf_open_virtual`.
- iOS folder pick: `PickIOSFolder` (`src/iOSNatives.mm:2627`) + security-scoped bookmark; used by `chart_library_platform::FolderActionService::requestAddFolder` (`src/library/ChartLibraryPlatform.cpp:207`) on a background thread.
- Android folder pick: `PickAndroidChartFolder` (`src/AndroidNatives.cpp:850`) + tree URI; used at `ChartLibraryPlatform.cpp:232`.
- A `std::jthread` picker-active guard pattern in `ChartLibraryPlatform.cpp:203-244`.

**Root-cause fix (bundle-aware decode):** Make `AudioWrapper::loadSkinSound`/the skin-sound load in `MusicSelectScene` read the asset bytes via `SDL_RWFromFile` (bundle-aware) and decode from memory (`decodeAudioBytesToPCM`), instead of only `sf_open` on the literal path. Fall back to the existing `sf_open` path if the SDL read returns nothing (covers real user files with absolute paths). Keep the current warning behavior and the `loadSkinSound` return shape (`audio::SkinSoundLoadResult`).

**Folder-picker fix:** `AppSettings::skinSelectSoundSetPath` is currently a typed-path input (`SettingsSceneSkins.cpp`, "Sound set folder"). Replace the typed input with a button that launches the platform folder picker (reusing the `FolderActionService` picker pattern) and persists the picked path (and, on iOS, the security-scoped bookmark needed to re-access it after app relaunch; on Android, the SAF tree URI if the folder is not a plain absolute path). Keep the typed input as an advanced fallback (the app must still support entering a path on desktop, and iOS path fields are near-useless — the button is the primary affordance).

- [ ] **Step 1: Write failing tests**
  - `loadSkinSound`-decode: a fake/bytes-based decoder asserts a `.wav` byte buffer (matching our generated WAV format) decodes to PCM; the bundle-aware read path is exercised with a temp SDL-visible file (or an injected read callback).
  - A focused `SkinSystemSoundService` test that the resolution preflight no longer relies on `std::filesystem::is_regular_file` alone when the path is bundle-relative (the player must attempt the bundle read).
  - Settings-store: the `skinSelectSoundSetPath` round-trip continues to work; a new bookmarked-path field round-trips.
- [ ] **Step 2: Run and observe the failing decode**
  Run: `cmake --build cmake-build-debug --target audio_wrapper_lifecycle_tests -j 6` and the focused new test; expect the byte-decode or bundle-read assertion to fail.
- [ ] **Step 3: Implement bundle-aware load**
  Add a bytes-based skin-sound load (may reuse `decodeAudioBytesToPCM`) and route `MusicSelectSkinSoundPlayer`/`MusicSelectPreviewBgmPlayer` through it; keep `sf_open` as fallback. Do not break the existing `loadSoundFromMemory`/chart-audio paths.
- [ ] **Step 4: Implement the folder-picker setting**
  Add a "Sound set folder" button in `SettingsSceneSkins` that launches the platform picker on a background thread (reuse `pickFolderFor...` pattern), persists the path (+ iOS bookmark / Android tree URI as needed), and updates the resolution roots. Keep the typed-path input as fallback.
- [ ] **Step 5: Run and commit**
  Run the focused tests + `main` build + `music_select_*` suite; commit.

```bash
git add src/audio/AudioWrapper.cpp src/scene/MusicSelectScene.cpp src/scene/SettingsSceneSkins.cpp src/AppSettings.h src/AppSettingsStore.cpp src/library/ChartLibraryPlatform.h src/library/ChartLibraryPlatform.cpp src/iOSNatives.mm src/AndroidNatives.h tests/audio_wrapper_lifecycle_tests.cpp docs/skin-compat/beatoraja-music-select-gaps.md
git commit -m "feat: load bundled sounds via bundle-aware read and pick sound set folder"
```
