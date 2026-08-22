# Beatoraja Gameplay-Skin Contract and Formats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish the pinned gameplay compatibility ledger and load Lua, JSON, and LR2 gameplay skins into the shared validated model.

**Architecture:** Keep one canonical `BeatorajaSkinModel`. Add format-specific JSON and LR2 decoders beside the existing Lua decoder, dispatch by a closed source-format enum, and make the Lua callback runtime optional for static formats. Package discovery is enabled for JSON and LR2 only after their validators and session loaders are complete.

**Tech Stack:** C++23, Lua 5.4, nlohmann JSON, CP932 conversion through `cp932_to_utf8`, CMake/CTest, Python 3 contract checks.

**Spec:** [`docs/superpowers/specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md`](../specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md)

## Global Constraints

- Compatibility is pinned to Beatoraja commit `c2ed5db1a46145ed10790c3872f717e95b59db9d`.
- Include only gameplay skin types and loader paths used by Beatoraja's gameplay player.
- Preserve authored order, source provenance, loader defaults, and format-specific observable behavior.
- A valid gameplay construct must not silently become `SkinBlankObject`.
- Keep `vcpkg_installed/` untracked and unstaged.
- Do not run a whole-file formatter.
- Use a red/green test cycle and one intentional commit per task.

## Coordinated plan-set order

Execute the four plans in this dependency order:

1. This plan, Tasks 1–3: ledger, format classification, and static-runtime seam.
2. `2026-08-21-beatoraja-gameplay-skin-visualizers.md`, Tasks 1–7.
3. `2026-08-21-beatoraja-gameplay-skin-assets-and-host.md`, Tasks 1–5.
4. This plan, Tasks 4–7: JSON/LR2 decoders and production discovery, now that
   every typed payload exists.
5. `2026-08-21-beatoraja-gameplay-skin-assets-and-host.md`, Tasks 6–9.
6. `2026-08-21-beatoraja-gameplay-skin-conformance.md`, Tasks 1–5.

The Task 1 ledger gate may add a feature-sized task before production edits if
the pinned extractor finds a valid gameplay surface absent from this audited
plan set. It may not weaken, ignore, or reclassify that surface to preserve the
published order.

---

### Task 1: Machine-readable gameplay parity contract

**Files:**

- Create: `docs/skin-compat/beatoraja-gameplay-source-surface-v1.json`
- Create: `docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json`
- Create: `scripts/extract_beatoraja_gameplay_skin_surface.py`
- Create: `tests/beatoraja_gameplay_skin_ledger_tests.py`
- Modify: `CMakeLists.txt`
- Modify: `docs/todo.md`

**Interfaces:**

- Consumes: pinned Java sources under `../beatoraja/src/bms/player/beatoraja/skin`, existing property traces, and the Lua compatibility contract.
- Produces: a stable source-surface ID set and a one-to-one classification ledger with statuses `implemented`, `missing`, or `source-defined-noop`.

- [ ] **Step 1: Write the failing ledger test**

  Add a Python test that loads both JSON files, asserts the pinned commit and schema version, rejects duplicate IDs and the words `unclassified`, `TBD`, and `TODO`, and requires exact set equality between source-surface IDs and ledger IDs. For `implemented`, require nonempty `implementation` and `tests`; for `missing`, require an owning plan/task; for `source-defined-noop`, require a pinned source path and symbol.

  ```python
  VALID_STATUS = {"implemented", "missing", "source-defined-noop"}
  assert source_ids == ledger_ids
  for row in ledger["features"]:
      assert row["status"] in VALID_STATUS
      if row["status"] == "implemented":
          assert row["implementation"] and row["tests"]
      elif row["status"] == "missing":
          assert row["plan"].startswith("docs/superpowers/plans/")
          assert row["task"].startswith("Task ")
      else:
          assert row["source"]["path"] and row["source"]["symbol"]
  ```

- [ ] **Step 2: Run the contract test to verify it fails**

  Run: `python3 tests/beatoraja_gameplay_skin_ledger_tests.py`

  Expected: failure because the two manifests and extractor do not exist.

- [ ] **Step 3: Implement the extractor, snapshots, and ledger**

  The extractor accepts `--beatoraja-root` and `--check`. It first verifies `git rev-parse HEAD` equals the pinned commit, then inventories these exact gameplay surfaces:

  - public fields in `JsonSkin.Skin` and every object class reachable from `JsonPlaySkinObjectLoader`;
  - command registrations in `LR2SkinCSVLoader` and `LR2PlaySkinLoader`, plus header declarations from `LR2SkinHeaderLoader`;
  - Lua object table fields decoded by `LuaSkinTableDecoder` and exported functions from `SkinLuaAccessor`, `SkinFileLuaApiExporter`, `SkinHttpLuaApiExporter`, `SkinAudioLuaApiExporter`, and `LegacySkinLuaApi`;
  - gameplay properties, timers, events, writers, and offsets referenced by those loader paths.

  Emit IDs as `<format>.<kind>.<normalized-name>`, retain `{path, symbol}`, and sort by ID. Populate the ledger by migrating current implemented coverage and assigning every audited gap to a concrete task in this four-plan set. Add the Python test to CTest and replace `docs/todo.md`'s claim of completeness with a link to the ledger.

  ```json
  {
    "schemaVersion": 1,
    "pinnedCommit": "c2ed5db1a46145ed10790c3872f717e95b59db9d",
    "features": [
      {
        "id": "json.object.judgegraph",
        "status": "missing",
        "plan": "docs/superpowers/plans/2026-08-21-beatoraja-gameplay-skin-visualizers.md",
        "task": "Task 2: Judgement and note-distribution graph"
      }
    ]
  }
  ```

- [ ] **Step 4: Verify the committed inventory against the pinned source**

  Run: `python3 scripts/extract_beatoraja_gameplay_skin_surface.py --beatoraja-root ../beatoraja --check && python3 tests/beatoraja_gameplay_skin_ledger_tests.py && ctest --test-dir cmake-build-debug -R beatoraja_gameplay_skin_ledger --output-on-failure`

  Expected: the regenerated ID/source set equals the committed snapshot and every entry is classified.

- [ ] **Step 5: Commit the compatibility contract**

  Run: `git add CMakeLists.txt docs/todo.md docs/skin-compat/beatoraja-gameplay-source-surface-v1.json docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json scripts/extract_beatoraja_gameplay_skin_surface.py tests/beatoraja_gameplay_skin_ledger_tests.py && git commit -m "test: inventory Beatoraja gameplay skin parity"`

### Task 2: Closed source-format classification

**Files:**

- Create: `src/skin/beatoraja/GameplaySkinSourceFormat.h`
- Create: `src/skin/beatoraja/GameplaySkinSourceFormat.cpp`
- Create: `tests/gameplay_skin_source_format_tests.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: a package-relative entry path.
- Produces: `std::optional<GameplaySkinSourceFormat> gameplaySkinSourceFormatForPath(std::string_view)` for `.luaskin`, `.json`, and `.lr2skin`, using ASCII case-insensitive extension matching.

- [ ] **Step 1: Write the failing format-classification test**

  Assert the following exact mapping and rejection behavior:

  ```cpp
  expect(gameplaySkinSourceFormatForPath("Play/main.luaskin") ==
             GameplaySkinSourceFormat::Lua,
         "luaskin must classify as Lua");
  expect(gameplaySkinSourceFormatForPath("Play/main.JSON") ==
             GameplaySkinSourceFormat::Json,
         "extension matching must be ASCII case-insensitive");
  expect(gameplaySkinSourceFormatForPath("Play/main.lr2skin") ==
             GameplaySkinSourceFormat::Lr2,
         "lr2skin must classify as LR2");
  expect(!gameplaySkinSourceFormatForPath("config.json.bak"),
         "suffix-like names must not classify");
  ```

- [ ] **Step 2: Run the focused test to verify it fails**

  Run: `cmake --build cmake-build-debug --target gameplay_skin_source_format_tests -j 6`

  Expected: failure because the enum and classifier do not exist.

- [ ] **Step 3: Implement the closed classifier**

  Define only these values and do not infer format from file contents:

  ```cpp
  enum class GameplaySkinSourceFormat : std::uint8_t { Lua, Json, Lr2 };

  [[nodiscard]] std::optional<GameplaySkinSourceFormat>
  gameplaySkinSourceFormatForPath(std::string_view packageRelativePath) noexcept;
  ```

  Keep gameplay-type admission in the validator; a generic `.json` file may classify as JSON but must not become selectable unless its decoded header is a gameplay type.

- [ ] **Step 4: Run the focused test**

  Run: `cmake --build cmake-build-debug --target gameplay_skin_source_format_tests -j 6 && ./cmake-build-debug/gameplay_skin_source_format_tests`

  Expected: `gameplay skin source format tests passed`.

- [ ] **Step 5: Commit the reusable classifier**

  Run: `git add CMakeLists.txt src/skin/CMakeLists.txt src/skin/beatoraja/GameplaySkinSourceFormat.h src/skin/beatoraja/GameplaySkinSourceFormat.cpp tests/gameplay_skin_source_format_tests.cpp && git commit -m "feat: classify gameplay skin source formats"`

### Task 3: Optional Lua callback runtime for static skin formats

**Files:**

- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.h`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/skin/beatoraja/LuaSkinBindingDecoder.h`
- Modify: `src/skin/beatoraja/SkinModelValidator.h`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`
- Modify: `tests/play_skin_state_bridge_tests.cpp`
- Modify: `tests/play_skin_session_tests.cpp`

**Interfaces:**

- Consumes: models whose bindings are either built-in selectors or Lua callback IDs.
- Produces: nullable `LuaSkinRuntime *runtime`; validation rejects a callback binding when it is null, while static bindings render without creating a Lua VM.

- [ ] **Step 1: Write failing static-runtime tests**

  Add a renderer/bridge fixture with only built-in selectors and `runtime = nullptr`; assert a complete frame evaluates. Add a model containing one `LuaCallbackId` and assert validation emits `skin.model.callback_runtime_missing`. Preserve the existing Lua-backed frame test unchanged.

- [ ] **Step 2: Run the focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target skin_draw_command_tests play_skin_state_bridge_tests play_skin_session_tests -j 6`

  Expected: compile failure because runtime fields are references and the validator cannot express absence.

- [ ] **Step 3: Make callback execution explicitly optional**

  Change runtime-bearing frame/session contexts to pointers. Guard `beginFrame`, `setFrameState`, `setEventExecutor`, and `invoke`; a callback source with no runtime is an error, never a default value. Store `std::unique_ptr<LuaSkinRuntime>` in `OwnedActivation`, allowing null only after static-model validation.

  ```cpp
  struct SkinBindingValidationContext {
    SkinBuiltinBindingCatalogView builtins;
    std::optional<LuaCallbackLivenessView> callbacks;
  };

  struct SkinFrameInputs {
    // unchanged fields omitted
    LuaSkinRuntime *runtime = nullptr;
  };
  ```

- [ ] **Step 4: Run the focused tests**

  Run: `cmake --build cmake-build-debug --target skin_draw_command_tests play_skin_state_bridge_tests play_skin_session_tests -j 6 && ./cmake-build-debug/skin_draw_command_tests && ./cmake-build-debug/play_skin_state_bridge_tests && ./cmake-build-debug/play_skin_session_tests`

  Expected: all three executables pass for both static and Lua-backed models.

- [ ] **Step 5: Commit the static-runtime seam**

  Run: `git add src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp src/skin/beatoraja/PlaySkinStateBridge.h src/skin/beatoraja/PlaySkinStateBridge.cpp src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp src/skin/beatoraja/LuaSkinBindingDecoder.h src/skin/beatoraja/SkinModelValidator.h src/skin/beatoraja/SkinModelValidator.cpp tests/skin_draw_command_tests.cpp tests/play_skin_state_bridge_tests.cpp tests/play_skin_session_tests.cpp && git commit -m "refactor: allow static gameplay skin bindings"`

### Task 4: JSON gameplay document decoder

**Files:**

- Create: `src/skin/beatoraja/JsonGameplaySkinDecoder.h`
- Create: `src/skin/beatoraja/JsonGameplaySkinDecoder.cpp`
- Create: `tests/json_gameplay_skin_decoder_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/json/all_gameplay_fields.json`
- Create: `tests/fixtures/beatoraja_skin/json/defaults.json`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: bounded UTF-8 JSON bytes, entry provenance, desired configuration, and `gameplaySkinBuiltinCatalog()`.
- Produces: `JsonGameplaySkinDecodeResult` containing header, reconciled configuration/settings, canonical model, and diagnostics; it never creates Lua callbacks.

- [ ] **Step 1: Write failing JSON decoder tests**

  Build fixtures covering every `JsonSkin.Skin` gameplay field, nested class field, omitted primitive default, explicit `Integer.MIN_VALUE` sentinel equivalent, destination inheritance, object-ID resolution, and custom property/timer/event binding. Assert `defaults.json` matches pinned Java defaults and `all_gameplay_fields.json` preserves destination order and source locations.

- [ ] **Step 2: Run the focused test to verify it fails**

  Run: `cmake --build cmake-build-debug --target json_gameplay_skin_decoder_tests -j 6`

  Expected: failure because `JsonGameplaySkinDecoder` is undefined.

- [ ] **Step 3: Implement bounded JSON-to-model decoding**

  Expose one value-owned result:

  ```cpp
  struct JsonGameplaySkinDecodeResult {
    std::optional<BeatorajaSkinHeader> header;
    std::optional<BeatorajaSkinConfiguration> configuration;
    std::optional<EntryProfileSettings> reconciledSettings;
    std::optional<BeatorajaSkinModel> model;
    std::vector<SkinDiagnostic> diagnostics;
  };

  class JsonGameplaySkinDecoder final {
  public:
    JsonGameplaySkinDecodeResult decode(
        std::span<const std::byte> bytes,
        const SkinEntryId &entry,
        const EntryProfileSettings *desired,
        SkinBuiltinBindingCatalogView builtins,
        SkinSafetyPolicy safetyPolicy = {}) const;
  };
  ```

  Use `nlohmann::json::parse` with exceptions caught at the decoder boundary. Apply defaults from pinned `JsonSkin.java`, object construction/order from `JsonSkinObjectLoader` and `JsonPlaySkinObjectLoader`, and the same ID-resolution precedence already tested for Lua. Decode all ledger fields even when a later rendering task is still marked missing; represent those objects with their typed canonical payload introduced by the owning visualizer/assets task, not a blank.

- [ ] **Step 4: Run JSON and shared model tests**

  Run: `cmake --build cmake-build-debug --target json_gameplay_skin_decoder_tests beatoraja_skin_model_tests -j 6 && ./cmake-build-debug/json_gameplay_skin_decoder_tests && ./cmake-build-debug/beatoraja_skin_model_tests`

  Expected: both executables pass and the JSON fixture has no unclassified-field diagnostic.

- [ ] **Step 5: Commit the JSON frontend**

  Run: `git add CMakeLists.txt src/skin/CMakeLists.txt src/skin/beatoraja/JsonGameplaySkinDecoder.h src/skin/beatoraja/JsonGameplaySkinDecoder.cpp tests/json_gameplay_skin_decoder_tests.cpp tests/fixtures/beatoraja_skin/json && git commit -m "feat: decode Beatoraja JSON gameplay skins"`

### Task 5: LR2 syntax, encoding, header, and include engine

**Files:**

- Create: `src/skin/beatoraja/Lr2SkinCsvParser.h`
- Create: `src/skin/beatoraja/Lr2SkinCsvParser.cpp`
- Create: `src/skin/beatoraja/Lr2SkinHeaderDecoder.h`
- Create: `src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp`
- Create: `tests/lr2_skin_csv_parser_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/lr2/header/main.lr2skin`
- Create: `tests/fixtures/beatoraja_skin/lr2/header/included.lr2skin`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `LuaSkinFileSystem` bounded reads, an LR2 entry path, CP932 bytes, and a cancellation token.
- Produces: ordered `Lr2SkinCommand` values with decoded fields/source locations and a `BeatorajaSkinHeader` matching `LR2SkinHeaderLoader`.

- [ ] **Step 1: Write failing CP932/include/header tests**

  Commit a small CP932 fixture containing Japanese header text, quoted commas, empty fields, comments, mixed command case, a nested include, and a duplicate declaration. Assert exact decoded text, include insertion order, source file/line, Beatoraja coordinate metadata, and the pinned duplicate/default behavior. Add a cycle fixture and assert a bounded diagnostic with the full include chain.

- [ ] **Step 2: Run the focused parser test to verify it fails**

  Run: `cmake --build cmake-build-debug --target lr2_skin_csv_parser_tests -j 6`

  Expected: failure because the parser and header decoder do not exist.

- [ ] **Step 3: Implement the LR2 command stream**

  ```cpp
  struct Lr2SkinCommand {
    std::string name;
    std::vector<std::string> fields;
    SkinSourceLocation source;
    std::vector<std::string> includeChain;
  };

  struct Lr2SkinParseResult {
    std::vector<Lr2SkinCommand> commands;
    std::vector<SkinDiagnostic> diagnostics;
    bool cancelled = false;
  };
  ```

  Decode with `cp932_to_utf8`, apply `LR2SkinCSVLoader` tokenization and `INCLUDE` insertion rules, resolve includes through the package filesystem, and preserve command order. The cycle/depth/byte guards must reject only invalid or unsafe input and must not reorder valid includes. Decode header/customization commands using `LR2SkinHeaderLoader` rules.

- [ ] **Step 4: Run parser and filesystem tests**

  Run: `cmake --build cmake-build-debug --target lr2_skin_csv_parser_tests lua_skin_file_system_tests -j 6 && ./cmake-build-debug/lr2_skin_csv_parser_tests && ./cmake-build-debug/lua_skin_file_system_tests`

  Expected: both executables pass, including CP932 and include provenance.

- [ ] **Step 5: Commit the LR2 syntax layer**

  Run: `git add CMakeLists.txt src/skin/CMakeLists.txt src/skin/beatoraja/Lr2SkinCsvParser.h src/skin/beatoraja/Lr2SkinCsvParser.cpp src/skin/beatoraja/Lr2SkinHeaderDecoder.h src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp tests/lr2_skin_csv_parser_tests.cpp tests/fixtures/beatoraja_skin/lr2/header && git commit -m "feat: parse LR2 gameplay skin documents"`

### Task 6: LR2 gameplay commands to canonical model

**Files:**

- Create: `src/skin/beatoraja/Lr2GameplaySkinDecoder.h`
- Create: `src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp`
- Create: `tests/lr2_gameplay_skin_decoder_tests.cpp`
- Create: `tests/fixtures/beatoraja_skin/lr2/all_play_commands.lr2skin`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: the ordered `Lr2SkinCommand` stream, decoded header, configured LR2 options/files/offsets, and built-in binding catalog.
- Produces: canonical model and configuration for every command registered by `LR2SkinCSVLoader` and `LR2PlaySkinLoader`.

- [ ] **Step 1: Write the failing all-command fixture test**

  Cover common timing/control commands and every gameplay command family: `IMAGE`, `LR2FONT`, source/destination image, image set, number, text, slider, bar graph, button, on-mouse, groove gauge, BGA, lines, NOTE/LN/HCN/MINE variants, NOTE2, expansion, NOWJUDGE/COMBO players, JUDGELINE, NOTECHART, BPMCHART, TIMING, HIDDEN/LIFT, PM character, and gameplay start/load/finish timing. Assert source defaults, 480-to-authored Y conversion, destination ordering, and selector IDs against the pinned loader.

- [ ] **Step 2: Run the focused decoder test to verify it fails**

  Run: `cmake --build cmake-build-debug --target lr2_gameplay_skin_decoder_tests -j 6`

  Expected: failure because the gameplay decoder does not exist.

- [ ] **Step 3: Implement command-state decoding**

  ```cpp
  struct Lr2GameplaySkinDecodeResult {
    std::optional<BeatorajaSkinConfiguration> configuration;
    std::optional<EntryProfileSettings> reconciledSettings;
    std::optional<BeatorajaSkinModel> model;
    std::vector<SkinDiagnostic> diagnostics;
  };

  class Lr2GameplaySkinDecoder final {
  public:
    Lr2GameplaySkinDecodeResult decode(
        const BeatorajaSkinHeader &,
        std::span<const Lr2SkinCommand>,
        const EntryProfileSettings *,
        SkinBuiltinBindingCatalogView,
        SkinSafetyPolicy = {}) const;
  };
  ```

  Mirror the pinned loaders' stateful source/destination association and fallback behavior. Unknown non-gameplay commands follow the source loader's ignore/diagnostic boundary; every registered gameplay command must materialize a typed model element or explicit timing/configuration value.

- [ ] **Step 4: Run LR2 and shared model tests**

  Run: `cmake --build cmake-build-debug --target lr2_gameplay_skin_decoder_tests beatoraja_skin_model_tests -j 6 && ./cmake-build-debug/lr2_gameplay_skin_decoder_tests && ./cmake-build-debug/beatoraja_skin_model_tests`

  Expected: both executables pass and the all-command fixture has no unsupported-command diagnostic.

- [ ] **Step 5: Commit the LR2 gameplay frontend**

  Run: `git add CMakeLists.txt src/skin/CMakeLists.txt src/skin/beatoraja/Lr2GameplaySkinDecoder.h src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp tests/lr2_gameplay_skin_decoder_tests.cpp tests/fixtures/beatoraja_skin/lr2/all_play_commands.lr2skin && git commit -m "feat: decode LR2 gameplay skin commands"`

### Task 7: Production dispatch, validation, and discovery

**Files:**

- Create: `src/skin/beatoraja/GameplaySkinDocumentLoader.h`
- Create: `src/skin/beatoraja/GameplaySkinDocumentLoader.cpp`
- Modify: `src/skin/beatoraja/GameplaySkinValidator.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/skin/package/SkinPackageStore.cpp`
- Modify: `src/skin/package/SkinArchiveImporter.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/gameplay_skin_validator_tests.cpp`
- Modify: `tests/play_skin_session_tests.cpp`
- Modify: `tests/skin_package_store_tests.cpp`
- Modify: `tests/skin_archive_importer_tests.cpp`

**Interfaces:**

- Consumes: a validated package revision/entry, desired settings, optional initial frame authority, and source format classification.
- Produces: one `LoadedGameplaySkinDocument` with validated model/configuration, optional Lua runtime, source metadata, and diagnostics.

- [ ] **Step 1: Write failing end-to-end format tests**

  Import a package containing one gameplay Lua entry, one gameplay JSON entry, one gameplay LR2 entry, one non-skin JSON file, and one non-gameplay LR2 skin. Assert only the three gameplay entries are selectable, each creates a session, static sessions own no Lua runtime, and all three emit equivalent commands for a shared image/number/text fixture.

- [ ] **Step 2: Run the focused integration targets to verify they fail**

  Run: `cmake --build cmake-build-debug --target gameplay_skin_validator_tests play_skin_session_tests skin_package_store_tests skin_archive_importer_tests -j 6`

  Expected: JSON/LR2 entries are not discovered and the validator/session remain Lua-only.

- [ ] **Step 3: Implement the document dispatcher and enable discovery**

  ```cpp
  struct LoadedGameplaySkinDocument {
    BeatorajaSkinHeader header;
    BeatorajaSkinConfiguration configuration;
    EntryProfileSettings reconciledSettings;
    ValidatedBeatorajaSkinModel model;
    std::unique_ptr<LuaSkinRuntime> luaRuntime;
    std::vector<SkinDiagnostic> diagnostics;
  };
  ```

  Dispatch Lua through the existing two-phase runtime/decoder and JSON/LR2 through their static decoders. Share `SkinModelValidator` and resource planning after decode. Update package scans to admit the three classified extensions, but rely on `gameplaySkinTraitForSkinType` before catalog publication. Keep non-gameplay JSON/LR2 entries unavailable rather than selectable or fatal to the package.

- [ ] **Step 4: Run the format integration suite**

  Run: `cmake --build cmake-build-debug --target gameplay_skin_validator_tests play_skin_session_tests skin_package_store_tests skin_archive_importer_tests gameplay_skin_integration_tests -j 6 && ./cmake-build-debug/gameplay_skin_validator_tests && ./cmake-build-debug/play_skin_session_tests && ./cmake-build-debug/skin_package_store_tests && ./cmake-build-debug/skin_archive_importer_tests && ./cmake-build-debug/gameplay_skin_integration_tests`

  Expected: all targets pass with all three gameplay formats selectable and session-capable.

- [ ] **Step 5: Commit production multi-format loading**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/GameplaySkinDocumentLoader.h src/skin/beatoraja/GameplaySkinDocumentLoader.cpp src/skin/beatoraja/GameplaySkinValidator.cpp src/skin/beatoraja/PlaySkinSession.cpp src/skin/package/SkinPackageStore.cpp src/skin/package/SkinArchiveImporter.cpp tests/gameplay_skin_validator_tests.cpp tests/play_skin_session_tests.cpp tests/skin_package_store_tests.cpp tests/skin_archive_importer_tests.cpp && git commit -m "feat: load JSON and LR2 gameplay skins"`

## Plan boundary check

After Task 1, compare every `missing` ledger row with the task ownership in this plan set. If an audited row has no owning task, add a feature-sized task to the appropriate plan before changing production code. This is a required completeness gate, not permission to classify the row as a no-op.
