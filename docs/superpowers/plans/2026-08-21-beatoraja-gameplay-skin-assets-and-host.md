# Beatoraja Gameplay-Skin Assets and Host Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete gameplay-skin fonts, movies, PM characters, practice/text interactions, and the pinned finite Lua host facade.

**Architecture:** Extend the canonical resource plan with typed bitmap-font and movie resources, retain all ownership in the chart-lifetime skin session, and keep renderer commands value-owned. Expose Beatoraja's Lua capabilities through injected file, HTTP, audio, display/input, and controller adapters rather than a general Java or operating-system bridge.

**Tech Stack:** C++23, existing image/font atlas pipeline, FFmpeg `VideoPlayer`, SDL text input/controllers, Lua 5.4, CMake/CTest.

**Spec:** [`docs/superpowers/specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md`](../specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md)

## Global Constraints

- Reproduce only the APIs and members allowed by pinned `SkinLuaAccessor` and `LegacySkinLuaApi`.
- Keep unsafe globals, native library loading, package-path mutation, process spawning, and `popen` unavailable.
- Match Beatoraja argument, return, error, timeout, size, and lifecycle behavior.
- Resource decoding/preparation is cancellable and stays outside the frame loop.
- JSON/LR2 decoders consume the typed payloads after these tasks; Lua decoding is extended in each object task.
- Keep `vcpkg_installed/` untracked and do not run a whole-file formatter.
- Use a red/green test cycle and one independently reviewable feature commit per task.

---

### Task 1: Bitmap `.fnt` and LR2FONT resources

**Files:**

- Create: `src/skin/beatoraja/SkinBitmapFontParser.h`
- Create: `src/skin/beatoraja/SkinBitmapFontParser.cpp`
- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.cpp`
- Modify: `src/skin/beatoraja/SkinTextAtlas.h`
- Modify: `src/skin/beatoraja/SkinTextAtlas.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/skin_resource_catalog_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/resources/bitmap-font/fixture.fnt`
- Create: `tests/fixtures/beatoraja_skin/resources/bitmap-font/page.png`

**Interfaces:**

- Consumes: Beatoraja/libGDX `.fnt` bytes, page images, font type, original size, fallback declarations, and LR2FONT glyph mappings.
- Produces: `SkinBitmapFontResource` and prepared glyph/page metrics through `SkinPreparedResourceView`.

- [ ] **Step 1: Write failing bitmap-font tests**

  Cover text/binary-invalid `.fnt`, multiple pages, kerning, supplementary remapping, ordinary/distance-field/colored-distance-field types, fallback glyphs, scale, alignment, wrapping, shrink overflow, outline/shadow uniforms, and LR2FONT glyph rectangles. Assert missing optional glyph/page behavior matches `SkinTextBitmap` and `SkinTextBitmapSource`.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests skin_draw_command_tests -j 6`

  Expected: resource preparation reports `skin.resource.font_format_unsupported` for `.fnt`.

- [ ] **Step 3: Implement bitmap-font parsing and preparation**

  ```cpp
  struct SkinBitmapFontResource {
    SkinResourceId id = 0;
    std::string virtualPath;
    int type = 0;
    int originalSize = 0;
    std::uint32_t authoredOrdinal = 0;
  };

  struct SkinBitmapGlyph {
    char32_t codepoint = 0;
    int page = 0;
    SkinSourceRect region;
    int xOffset = 0;
    int yOffset = 0;
    int xAdvance = 0;
  };
  ```

  Parse bounded descriptor bytes and resolve page paths through `LuaSkinFileSystem`. Prepare page textures/metrics once, preserve distance-field type in the draw state, and remove the hardcoded `.fnt` rejection. LR2FONT decoding supplies the same prepared representation.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/skin_resource_catalog_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass for TTF/OTF and bitmap font fixtures.

- [ ] **Step 5: Commit bitmap fonts**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/SkinBitmapFontParser.h src/skin/beatoraja/SkinBitmapFontParser.cpp src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinResourceCatalog.h src/skin/beatoraja/SkinResourceCatalog.cpp src/skin/beatoraja/SkinTextAtlas.h src/skin/beatoraja/SkinTextAtlas.cpp tests/skin_resource_catalog_tests.cpp tests/skin_draw_command_tests.cpp tests/fixtures/beatoraja_skin/resources/bitmap-font && git commit -m "feat: render Beatoraja bitmap fonts"`

### Task 2: Skin source movies

**Files:**

- Create: `src/skin/beatoraja/SkinMovieCatalog.h`
- Create: `src/skin/beatoraja/SkinMovieCatalog.cpp`
- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/SkinDrawCommand.h`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/beatoraja/Skin2DRendererSubmit.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/skin_resource_catalog_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`
- Modify: `tests/play_skin_session_tests.cpp`

**Interfaces:**

- Consumes: JSON/LR2 image sources whose resolved extension is a pinned movie extension, source timer/cycle, and visual frame time.
- Produces: `SkinMovieResource`, `SkinMovieCommand`, and a session-owned `SkinMovieCatalog` backed by testable movie devices.

- [ ] **Step 1: Write failing movie lifecycle/draw tests**

  Use an injected fake movie device. Assert one load per deduplicated path, cancellation before publication, source timer seek/reset, destination crop/tint/blend/stretch, adjacent image/movie ordering, no decode in `evaluateFrame`, and exact-once teardown on session destruction and failed creation.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests skin_draw_command_tests play_skin_session_tests -j 6`

  Expected: movie paths are treated as unsupported image resources.

- [ ] **Step 3: Implement the session-owned movie path**

  ```cpp
  struct SkinMovieResource {
    SkinResourceId id = 0;
    std::string virtualPath;
    std::optional<SkinTimerPropertyId> timer;
    std::uint32_t authoredOrdinal = 0;
  };

  struct SkinMovieCommand {
    SkinResourceId resource = 0;
    std::int64_t sourceTimeMillis = 0;
    AuthoredDestinationGeometry geometry;
    SkinRenderState state;
  };
  ```

  Materialize package movie bytes to a stable session path during preparation, load `VideoPlayer` off the frame path, prepare the current frame before command submission, and release players/materialized files with `OwnedActivation`. Keep BGA media ownership separate.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests skin_draw_command_tests play_skin_session_tests -j 6 && ./cmake-build-debug/skin_resource_catalog_tests && ./cmake-build-debug/skin_draw_command_tests && ./cmake-build-debug/play_skin_session_tests`

  Expected: all pass with deterministic movie ownership.

- [ ] **Step 5: Commit skin movies**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/SkinMovieCatalog.h src/skin/beatoraja/SkinMovieCatalog.cpp src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/SkinDrawCommand.h src/skin/beatoraja/SkinResourceCatalog.h src/skin/beatoraja/SkinResourceCatalog.cpp src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp src/skin/beatoraja/Skin2DRendererSubmit.cpp src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp tests/skin_resource_catalog_tests.cpp tests/skin_draw_command_tests.cpp tests/play_skin_session_tests.cpp && git commit -m "feat: render Beatoraja skin source movies"`

### Task 3: Complete Pomyu/PM-character rendering

**Files:**

- Create: `src/skin/beatoraja/PomyuCharaResource.h`
- Create: `src/skin/beatoraja/PomyuCharaResource.cpp`
- Modify: `src/skin/beatoraja/PomyuCharaCycles.h`
- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h`
- Modify: `src/skin/beatoraja/SkinResourceCatalog.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/play_skin_session_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`

**Interfaces:**

- Consumes: `.chp` metadata, `CHARBMP`/`CHARBMP2P`, optional texture definitions, color/type/side, motion timers, and existing cycle metadata.
- Produces: prepared PM-character sprite resources and source-selected animated frames matching `PomyuCharaLoader`.

- [ ] **Step 1: Write failing PM-character tests**

  Add CP932 `.chp` fixtures for primary/2P colors, texture/no-texture modes, missing optional images, all eight motion cycles, type/side defaults, and timer changes. Assert decoded regions, selected resource, cycle time, horizontal orientation, draw order, and fallback rules.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target play_skin_session_tests skin_draw_command_tests -j 6`

  Expected: only cycle metadata is prepared and no PM-character draw command is emitted.

- [ ] **Step 3: Implement full PM-character preparation/rendering**

  Move the current session-local `.chp` parsing/cache into `PomyuCharaResource`; retain cycle results and add decoded source rectangles/texture identity. Expand `SkinPmCharaObject` with the source fields required by pinned `PomyuCharaLoader` and emit the selected textured quad without file access during frames.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target play_skin_session_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/play_skin_session_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass for every color/type/side/motion fixture.

- [ ] **Step 5: Commit PM-character rendering**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/PomyuCharaResource.h src/skin/beatoraja/PomyuCharaResource.cpp src/skin/beatoraja/PomyuCharaCycles.h src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinResourceCatalog.h src/skin/beatoraja/SkinResourceCatalog.cpp src/skin/beatoraja/Skin2DRenderer.cpp src/skin/beatoraja/PlaySkinSession.cpp tests/play_skin_session_tests.cpp tests/skin_draw_command_tests.cpp && git commit -m "feat: render Beatoraja PM characters"`

### Task 4: SkinPractice object

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.h`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Modify: `src/practice/PracticeConfiguration.h`
- Modify: `src/practice/PracticeConfiguration.cpp`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`
- Modify: `tests/practice_configuration_tests.cpp`

**Interfaces:**

- Consumes: `visibleItems`, retained practice configuration, media-ready state, mode, judge counts, and note-distribution authority.
- Produces: visible-row-count mutation for `visibleItems > 0`; exact legacy text/help/count/graph fallback for `visibleItems == 0`.

- [ ] **Step 1: Write failing practice-object tests**

  Assert clamping to `0..16`; a positive value updates the retained menu viewport and draws nothing; zero uses the system-font fallback, source colors/turbo state, mode-specific help text, media-ready play hint, six judge-count rows, and the selected one of three note graphs in the BGA destination region.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests practice_configuration_tests -j 6`

  Expected: `sk.practice` is not decoded or rendered.

- [ ] **Step 3: Implement `SkinPracticeObject`**

  ```cpp
  struct SkinPracticeObject {
    int visibleItems = 10;
  };
  ```

  Decode the top-level object, apply the positive visible count transactionally to the existing practice controller, and use prepared system-font glyphs plus Task 2's note-distribution renderer for the zero-value legacy fallback. Follow `SkinPractice.java` text, coordinates, and colors exactly.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests practice_configuration_tests -j 6 && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests && ./cmake-build-debug/practice_configuration_tests`

  Expected: all pass for modern and legacy practice modes.

- [ ] **Step 5: Commit the practice object**

  Run: `git add src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/Skin2DRenderer.cpp src/skin/beatoraja/PlaySkinStateBridge.h src/skin/beatoraja/PlaySkinStateBridge.cpp src/practice/PracticeConfiguration.h src/practice/PracticeConfiguration.cpp tests/beatoraja_skin_model_tests.cpp tests/skin_draw_command_tests.cpp tests/practice_configuration_tests.cpp && git commit -m "feat: support Beatoraja practice skin objects"`

### Task 5: Editable skin text

**Files:**

- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/scene/play/CoordinatedPlaySkinSession.h`
- Modify: `src/scene/play/PlayfieldPresentationCoordinator.h`
- Modify: `src/scene/play/PlayfieldPresentationCoordinator.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `tests/play_skin_touch_geometry_tests.cpp`
- Modify: `tests/play_skin_session_tests.cpp`

**Interfaces:**

- Consumes: a visible editable `SkinTextObject`, its input bounds/current value/writer, pointer focus, SDL text events, Return, outside click, and session teardown.
- Produces: `SkinTextInteractionGeometry`, one focused editor, and a transactional `SkinStringWriterId` commit.

- [ ] **Step 1: Write failing text-input tests**

  Assert topmost editable-text hit testing, initial cursor at end, UTF-8 input, backspace, Return commit, outside-click commit, focus transfer, cancellation/session teardown, noneditable rejection, and no writer call until the matching skin frame successfully submits.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target play_skin_touch_geometry_tests play_skin_session_tests -j 6`

  Expected: interaction layout contains only sliders and images.

- [ ] **Step 3: Implement focused text editing**

  Add `SkinTextInteractionGeometry` to the heterogeneous topmost interaction sequence. The coordinator forwards text events only while a skin field owns focus; `PlaySkinSession` retains the edited UTF-8 value and queues a string-writer mutation on Return/outside focus loss. Rendering continues to use the captured frame value until commit, matching `SkinTextInput`'s transaction boundary.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target play_skin_touch_geometry_tests play_skin_session_tests -j 6 && ./cmake-build-debug/play_skin_touch_geometry_tests && ./cmake-build-debug/play_skin_session_tests`

  Expected: both pass with exact-once writer commits.

- [ ] **Step 5: Commit editable text**

  Run: `git add src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp src/scene/play/CoordinatedPlaySkinSession.h src/scene/play/PlayfieldPresentationCoordinator.h src/scene/play/PlayfieldPresentationCoordinator.cpp src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp tests/play_skin_touch_geometry_tests.cpp tests/play_skin_session_tests.cpp && git commit -m "feat: edit Beatoraja skin text fields"`

### Task 6: `main_state` file functions and legacy File facade

**Files:**

- Modify: `src/skin/beatoraja/LuaSkinFileSystem.h`
- Modify: `src/skin/beatoraja/LuaSkinFileSystem.cpp`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Modify: `tests/lua_skin_file_system_tests.cpp`
- Modify: `tests/lua_skin_host_modules_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/packages/runtime_contract/skin/main_state_file_surface.luaskin`

**Interfaces:**

- Consumes: pinned `SkinLuaPathResolver` semantics and the session's package/data filesystem.
- Produces: `file_exists`, `file_mkdir`, `file_list`, `file_read_lines`, `file_write`, `file_append`, `file_clear`, `file_count_lines`, and matching legacy `java.io.File` construction/list/mkdir.

- [ ] **Step 1: Write failing file-surface tests**

  Cover relative/absolute-form resolution accepted by the pinned resolver, missing paths, recursive versus single-directory creation, unsorted directory iteration and `file_list` pattern-group output, UTF-8 lines, overwrite/append/clear/count, render-phase behavior, and exact booleans/nil/error returns. Assert every nonallowlisted File member and constructor is denied.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target lua_skin_file_system_tests lua_skin_host_modules_tests -j 6`

  Expected: named `main_state.file_*` functions are absent or partial.

- [ ] **Step 3: Implement the pinned file API**

  Register exactly the eight functions and complete the existing File facade. Route all paths through one source-shaped resolver and preserve current handle invalidation/accounting. Do not expose arbitrary File members, process APIs, native library access, or a general filesystem object.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target lua_skin_file_system_tests lua_skin_host_modules_tests -j 6 && ./cmake-build-debug/lua_skin_file_system_tests && ./cmake-build-debug/lua_skin_host_modules_tests`

  Expected: both pass, including denial probes.

- [ ] **Step 5: Commit file host parity**

  Run: `git add src/skin/beatoraja/LuaSkinFileSystem.h src/skin/beatoraja/LuaSkinFileSystem.cpp src/skin/beatoraja/LuaSkinHostModules.cpp tests/lua_skin_file_system_tests.cpp tests/lua_skin_host_modules_tests.cpp tests/fixtures/beatoraja_skin/packages/runtime_contract/skin/main_state_file_surface.luaskin && git commit -m "feat: match Beatoraja Lua file APIs"`

### Task 7: Bounded HTTP and legacy URL/reader facade

**Files:**

- Create: `src/skin/beatoraja/LuaSkinHttpClient.h`
- Create: `src/skin/beatoraja/LuaSkinHttpClient.cpp`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.h`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.h`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/lua_skin_host_modules_tests.cpp`

**Interfaces:**

- Consumes: injected HTTP transport, URL, optional timeout, and GET-only legacy connection calls.
- Produces: `http_get`, `http_get_lines`, and legacy URL/InputStreamReader/BufferedReader facades with pinned bounds.

- [ ] **Step 1: Write failing deterministic HTTP tests**

  Use a fake transport to assert: only `http`/`https`; GET only; default 1,000 ms; timeout clamped to `1..5,000` ms; at most 1,024 lines and 65,536 characters; UTF-8 line joining; response code; connect idempotence; `readLine` then nil; and exact nil/error or Lua-error behavior for each modern/legacy path.

- [ ] **Step 2: Run the host test to verify it fails**

  Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 6`

  Expected: HTTP functions and URL constructors are denied.

- [ ] **Step 3: Implement the finite HTTP facade**

  Define an injected `LuaSkinHttpTransport::get(url, timeout, limits)` interface. Register only the two `main_state` functions and the exact `newInstance`/connection/reader members from `LegacySkinLuaApi`; reject every other scheme, method, class, or member.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 6 && ./cmake-build-debug/lua_skin_host_modules_tests`

  Expected: host tests pass with no real network dependency.

- [ ] **Step 5: Commit HTTP host parity**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/LuaSkinHttpClient.h src/skin/beatoraja/LuaSkinHttpClient.cpp src/skin/beatoraja/LuaSkinHostModules.h src/skin/beatoraja/LuaSkinHostModules.cpp src/skin/beatoraja/LuaSkinRuntime.h src/skin/beatoraja/LuaSkinRuntime.cpp tests/lua_skin_host_modules_tests.cpp && git commit -m "feat: match Beatoraja Lua HTTP APIs"`

### Task 8: Skin audio host lifecycle

**Files:**

- Create: `src/skin/beatoraja/LuaSkinAudioHost.h`
- Create: `src/skin/beatoraja/LuaSkinAudioHost.cpp`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.h`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/scene/play/GameplaySkinSessionFactory.h`
- Modify: `src/scene/play/GameplaySkinSessionFactory.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/lua_skin_host_modules_tests.cpp`
- Modify: `tests/play_skin_session_tests.cpp`

**Interfaces:**

- Consumes: package-resolved audio paths, volume, loop flag, and the application audio backend.
- Produces: `audio_play`, `audio_loop`, `audio_preload`, `audio_stop`, and `audio_dispose` with session-owned resource cleanup.

- [ ] **Step 1: Write failing audio-host tests**

  Inject a recording backend and assert exact path resolution, volume forwarding, loop flag, zero-volume preload, stop/dispose identity, repeated calls, missing audio behavior, teardown disposal, and no post-destruction callback. Verify all five Lua functions return the pinned values.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests play_skin_session_tests -j 6`

  Expected: all five functions are absent.

- [ ] **Step 3: Implement session-scoped audio commands**

  Add a narrow `LuaSkinAudioHost` adapter to `PlaySkinSessionContext`; register only the five functions. Resolve through the same skin path resolver, retain preloaded/playing identities in the session, and dispose them deterministically without exposing the general mixer.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests play_skin_session_tests -j 6 && ./cmake-build-debug/lua_skin_host_modules_tests && ./cmake-build-debug/play_skin_session_tests`

  Expected: both pass with exact-once cleanup.

- [ ] **Step 5: Commit audio host parity**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/LuaSkinAudioHost.h src/skin/beatoraja/LuaSkinAudioHost.cpp src/skin/beatoraja/LuaSkinHostModules.h src/skin/beatoraja/LuaSkinHostModules.cpp src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp src/scene/play/GameplaySkinSessionFactory.h src/scene/play/GameplaySkinSessionFactory.cpp tests/lua_skin_host_modules_tests.cpp tests/play_skin_session_tests.cpp && git commit -m "feat: match Beatoraja Lua audio APIs"`

### Task 9: Gdx input and controller legacy facade

**Files:**

- Create: `src/skin/beatoraja/LuaSkinLegacyInputHost.h`
- Create: `src/skin/beatoraja/LuaSkinLegacyInputHost.cpp`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.h`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`
- Modify: `src/scene/play/GameplaySkinSessionFactory.h`
- Modify: `src/scene/play/GameplaySkinSessionFactory.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/lua_skin_host_modules_tests.cpp`

**Interfaces:**

- Consumes: current drawable width/height, key state, SDL key-name mapping, and the ordered controller snapshot.
- Produces: exact allowlisted `Gdx`, `Input.Keys`, `Controllers`, and `Controller` facade values.

- [ ] **Step 1: Write failing legacy-input tests**

  Assert graphics width/height; known/unknown `Input.Keys.valueOf`; pressed/unpressed keys; zero/one/multiple controller `size`; `first`; controller name; pressed buttons; nil when absent; and denial of all classes, constructors, static fields, and members outside the pinned allowlist.

- [ ] **Step 2: Run the host test to verify it fails**

  Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 6`

  Expected: current facade exposes only partial File/Gdx behavior and denies controllers.

- [ ] **Step 3: Implement the snapshot-backed facade**

  Capture display/input/controller state at the same frame boundary as `main_state`. Implement closed Lua tables for the exact pinned class names and members; `luajava.bindClass`, `new`, and `newInstance` continue rejecting every unrecognized target. Do not expose SDL pointers or a generic reflection bridge.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 6 && ./cmake-build-debug/lua_skin_host_modules_tests`

  Expected: all allowlist and denial probes pass.

- [ ] **Step 5: Commit legacy input parity**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/LuaSkinLegacyInputHost.h src/skin/beatoraja/LuaSkinLegacyInputHost.cpp src/skin/beatoraja/LuaSkinHostModules.h src/skin/beatoraja/LuaSkinHostModules.cpp src/skin/beatoraja/PlaySkinSession.h src/scene/play/GameplaySkinSessionFactory.h src/scene/play/GameplaySkinSessionFactory.cpp tests/lua_skin_host_modules_tests.cpp && git commit -m "feat: match Beatoraja legacy input facade"`
