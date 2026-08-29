# Beatoraja Result Skins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Support selectable Beatoraja music-result (7) and course-result (15) skins in ResultScene with the same lifecycle as gameplay skins.

**Architecture:** Generalize selection and catalog admission around neutral Beatoraja screen-target traits, retaining the shared document/resource/renderer stack. Add a result-specific state bridge and session that feed the shared renderer from ResultScene.

**Tech Stack:** C++23, CMake/CTest, SDL, bgfx, Lua, nlohmann/json; compatibility authority: /Users/xf/workspace/SNURhythm/beatoraja at c2ed5db1.

**Spec:** docs/superpowers/specs/2026-08-23-beatoraja-result-skins-design.md

## Global Constraints

- Beatoraja types 7 and 15 plus their property/event factories and LR2/JSON/Lua loaders are the compatibility authority.
- Retain structural, package-path, resource, and configured-Lua safety checks; never apply gameplay-keymode admission to type 7 or 15.
- Support .luaskin, .json, and .lr2skin for both result types.
- Built-in selection renders the native layout. A selected activation failure is visible and never becomes a silent fallback.
- Preserve existing gameplay selections during profile migration.
- Do not whole-file format. Final verification: cmake --build cmake-build-debug --target main -j 6 and ctest --test-dir cmake-build-debug --output-on-failure -j 6.

---

### Task 1: Define neutral skin targets and migrate profile selection

**Files:**

- Create: src/skin/SkinTargetTraits.h
- Modify: src/skin/SkinProfileSettings.h
- Modify: src/skin/SkinProfileSettings.cpp
- Modify: src/AppSettingsStore.cpp
- Create: tests/skin_target_traits_tests.cpp
- Modify: tests/app_settings_store_tests.cpp
- Modify: tests/profile_settings_persistence_tests.cpp
- Modify: CMakeLists.txt

**Interfaces:**

- Produces SkinTargetTrait { int skinType; SkinTargetKind kind; int keyMode; std::string_view label; }.
- Produces skinTargetTraits(), skinTargetTraitForType(int), and gameplaySkinTargetForKeyMode(int).
- Produces SkinProfileSettings::selectedSkinEntries keyed by Beatoraja type.

- [ ] **Step 1: Write failing trait and migration tests.**

~~~cpp
TEST_CASE("result traits use exact Beatoraja result type ids") {
  CHECK(skinTargetTraitForType(7)->kind == SkinTargetKind::Result);
  CHECK(skinTargetTraitForType(15)->kind == SkinTargetKind::CourseResult);
  CHECK(gameplaySkinTargetForKeyMode(7)->skinType == 0);
}
TEST_CASE("legacy gameplay selection migrates without overwriting result") {
  SkinProfileSettings settings;
  settings.selectedGameplayEntries.emplace(0, entry("legacy-7k"));
  settings.selectedSkinEntries.emplace(7, entry("result"));
  settings.sanitize();
  CHECK(settings.selectedSkinEntries.at(0) == entry("legacy-7k"));
  CHECK(settings.selectedSkinEntries.at(7) == entry("result"));
}
~~~

- [ ] **Step 2: Run the focused test and verify RED.**

Run: cmake --build cmake-build-debug --target skin_target_traits_tests -j 6 && ctest --test-dir cmake-build-debug -R '^skin_target_traits_tests$' --output-on-failure

Expected: compilation fails because neutral target traits and selectedSkinEntries do not exist.

- [ ] **Step 3: Implement traits and lossless profile migration.**

~~~cpp
enum class SkinTargetKind : std::uint8_t { Gameplay, Result, CourseResult };
inline constexpr auto kSkinTargetTraits = std::to_array<SkinTargetTrait>({
    {0, SkinTargetKind::Gameplay, 7, "7K"},
    {1, SkinTargetKind::Gameplay, 5, "5K"},
    {2, SkinTargetKind::Gameplay, 14, "14K"},
    {3, SkinTargetKind::Gameplay, 10, "10K"},
    {4, SkinTargetKind::Gameplay, 9, "9K"},
    {16, SkinTargetKind::Gameplay, 24, "24K"},
    {17, SkinTargetKind::Gameplay, 48, "24K Double"},
    {7, SkinTargetKind::Result, 0, "Result"},
    {15, SkinTargetKind::CourseResult, 0, "Course Result"},
});
~~~

Make sanitize() copy a valid legacy type only when the neutral map has no value for that type, then remove invalid type keys. Decode legacy selectedGameplayEntries only when the new field is absent. Write only selectedSkinEntries.

- [ ] **Step 4: Run trait and profile regressions.**

Run: cmake --build cmake-build-debug --target skin_target_traits_tests profile_switch_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(skin_target_traits_tests|profile_switch_tests)$' --output-on-failure

Expected: both tests pass.

- [ ] **Step 5: Commit.**

~~~bash
git add src/skin/SkinTargetTraits.h src/skin/SkinProfileSettings.* tests/skin_target_traits_tests.cpp CMakeLists.txt
git commit -m "feat: generalize skin selection targets"
~~~

### Task 2: Make catalog validation and activation target-aware

**Files:**

- Create: src/skin/SkinActivationRequest.h
- Modify: src/skin/GameplaySkinActivationRequest.h and consumers
- Modify: src/skin/GameplaySkinLifecycle.*
- Modify: src/skin/beatoraja/GameplaySkinValidator.*
- Modify: src/skin/package/SkinPackageTypes.h
- Create: tests/skin_catalog_target_validation_tests.cpp
- Modify: tests/gameplay_skin_lifecycle_tests.cpp
- Modify: CMakeLists.txt

**Interfaces:**

- Produces SkinAcquisition with a target plus BuiltIn, Ready, and Failed dispositions.
- Produces neutral catalog disposition Selectable.
- Consumes selectedSkinEntries.

- [ ] **Step 1: Write failing catalog and acquisition tests.**

~~~cpp
TEST_CASE("result header is selectable for its matching target") {
  auto result = validateHeader(7, *skinTargetTraitForType(7));
  CHECK(result.disposition == SkinValidationDisposition::Selectable);
}
TEST_CASE("course request does not use music-result selection") {
  profile.selectedSkinEntries[7] = entry("music-result");
  CHECK(acquireForTarget(*skinTargetTraitForType(15)).disposition ==
        SkinAcquisitionDisposition::BuiltIn);
}
~~~

- [ ] **Step 2: Run focused tests and verify RED.**

Run: cmake --build cmake-build-debug --target skin_catalog_target_validation_tests gameplay_skin_lifecycle_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(skin_catalog_target_validation_tests|gameplay_skin_lifecycle_tests)$' --output-on-failure

Expected: type 7 is unavailable because validation is gameplay-only.

- [ ] **Step 3: Implement neutral selection while retaining validator safety.**

~~~cpp
[[nodiscard]] bool skinTypeMatchesTarget(int type, SkinTargetTrait target) {
  return type == target.skinType;
}
[[nodiscard]] SkinAcquisition acquireForTarget(SkinTargetTrait target);
~~~

Rename serialized SelectableGameplay to Selectable with backward-compatible catalog decoding. Continue to create restricted document filesystems, reconcile configuration, and retain metadata before admitting only a type contained in skinTargetTraits().

- [ ] **Step 4: Run target-aware lifecycle regressions.**

Run: cmake --build cmake-build-debug --target skin_catalog_target_validation_tests gameplay_skin_lifecycle_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(skin_catalog_target_validation_tests|gameplay_skin_lifecycle_tests)$' --output-on-failure

Expected: all focused tests pass, including current gameplay cases.

- [ ] **Step 5: Commit.**

~~~bash
git add src/skin src/scene/play tests/gameplay_skin_lifecycle_tests.cpp tests/skin_catalog_target_validation_tests.cpp CMakeLists.txt
git commit -m "feat: make skin lifecycle target-aware"
~~~

### Task 3: Decode types 7 and 15 in JSON, Lua, and LR2

**Files:**

- Modify: src/skin/beatoraja/GameplaySkinDocumentLoader.*
- Modify: src/skin/beatoraja/JsonGameplaySkinDecoder.*
- Modify: src/skin/beatoraja/Lr2GameplaySkinDecoder.*
- Modify: src/skin/beatoraja/LuaSkinTableDecoder.*
- Modify: src/skin/beatoraja/SkinModelValidator.*
- Modify: src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
- Create: tests/fixtures/beatoraja_skin/result/json/
- Create: tests/fixtures/beatoraja_skin/result/lr2/
- Create: tests/fixtures/beatoraja_skin/result/lua/
- Create: tests/beatoraja_result_skin_cross_format_tests.cpp
- Modify: CMakeLists.txt

**Interfaces:**

- Produces a validated canonical BeatorajaSkinModel for type 7/15 documents.
- Consumes neutral target traits and retains source/destination graph records.

- [ ] **Step 1: Write failing source-backed cross-format tests.**

~~~cpp
TEST_CASE("type 7 documents decode in each Beatoraja format") {
  for (auto decoded : decodeResultFixtures(7)) {
    REQUIRE(decoded.model);
    CHECK(decoded.model->header.type == 7);
    CHECK(hasObject<SkinGaugeGraphObject>(*decoded.model));
    CHECK(hasObject<SkinTimingDistributionGraphObject>(*decoded.model));
  }
}
TEST_CASE("type 15 LR2 retains its gauge graph") {
  auto decoded = decodeLr2ResultFixture(15);
  REQUIRE(decoded.model);
  CHECK(hasObject<SkinGaugeGraphObject>(*decoded.model));
}
~~~

Base fixtures on beatoraja/skin/default/result/result.luaskin, result.json, and graderesult.json. Retain authored result-graph fields.

- [ ] **Step 2: Run the target and verify RED.**

Run: cmake --build cmake-build-debug --target beatoraja_result_skin_cross_format_tests -j 6 && ctest --test-dir cmake-build-debug -R '^beatoraja_result_skin_cross_format_tests$' --output-on-failure

Expected: type 7/15 fails at the gameplay-type gate.

- [ ] **Step 3: Split generic document rules from gameplay-only rules.**

~~~cpp
[[nodiscard]] bool isSupportedSkinDocumentType(int type) noexcept {
  return skinTargetTraitForType(type).has_value();
}
[[nodiscard]] bool requiresGameplayModelRules(int type) noexcept {
  const auto trait = skinTargetTraitForType(type);
  return trait && trait->kind == SkinTargetKind::Gameplay;
}
~~~

Keep resource identity, dimensions, path policy, and structural checks for every target. Apply lane/note/projection rules only when requiresGameplayModelRules() is true. Implement upstream LR2ResultSkinLoader and LR2CourseResultSkinLoader commands by lowering to existing graph objects, never a parallel result renderer.

- [ ] **Step 4: Run result and gameplay cross-format tests.**

Run: cmake --build cmake-build-debug --target beatoraja_result_skin_cross_format_tests beatoraja_gameplay_cross_format_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(beatoraja_result_skin_cross_format_tests|beatoraja_gameplay_cross_format_tests)$' --output-on-failure

Expected: both targets pass.

- [ ] **Step 5: Commit.**

~~~bash
git add src/skin/beatoraja tests/fixtures/beatoraja_skin/result tests/beatoraja_result_skin_cross_format_tests.cpp CMakeLists.txt
git commit -m "feat: decode beatoraja result skin documents"
~~~

### Task 4: Build the result property, graph, and event bridge

**Files:**

- Create: src/skin/beatoraja/ResultSkinBuiltinCatalog.*
- Create: src/skin/beatoraja/ResultSkinStateBridge.*
- Modify: src/skin/beatoraja/Skin2DRenderer.* only to neutralize frame inputs
- Modify: src/scene/ResultScene.*
- Create: tests/result_skin_state_bridge_tests.cpp
- Modify: tests/skin_renderer_golden_tests.cpp
- Modify: CMakeLists.txt

**Interfaces:**

- Produces ResultSkinFrameSnapshot and ResultSkinStateBridge : ISkinFrameState.
- Produces ResultSkinAction { Back, Retry, Replay, Rankings, ContinueCourse } with explicit unavailable outcome.
- Consumes exact result selectors from Beatoraja property and event factories.

- [ ] **Step 1: Write failing selector/action tests.**

~~~cpp
TEST_CASE("result bridge exposes upstream score and judgement selectors") {
  ResultSkinStateBridge bridge(localSnapshot({.score = 1234, .maxScore = 2000,
                                              .pGreat = 321, .fast = 12}));
  CHECK(bridge.integerProperty({150}).value == 1234);
  CHECK(bridge.integerProperty({110}).value == 321);
  CHECK(bridge.integerProperty({423}).value == 12);
}
TEST_CASE("continue-course action is unavailable outside a course stage") {
  CHECK(bridgeForCourseStage().dispatch(eventForContinueCourse()).available);
  CHECK_FALSE(bridgeForFinalCourseResult().dispatch(eventForContinueCourse()).available);
}
~~~

- [ ] **Step 2: Run the bridge test and verify RED.**

Run: cmake --build cmake-build-debug --target result_skin_state_bridge_tests -j 6 && ctest --test-dir cmake-build-debug -R '^result_skin_state_bridge_tests$' --output-on-failure

Expected: compilation fails because the result bridge/catalog is absent.

- [ ] **Step 3: Implement direct result-state mapping.**

~~~cpp
struct ResultSkinFrameSnapshot {
  const ResultSkinData *data = nullptr;
  const ResultPresentationModel *presentation = nullptr;
  ResultCourseMode courseMode = ResultCourseMode::None;
  std::int64_t sceneElapsedMillis = 0;
};
class ResultSkinStateBridge final : public ISkinFrameState {
 public:
  explicit ResultSkinStateBridge(ResultSkinFrameSnapshot);
  SkinPropertyLookup<std::int64_t> integerProperty(
      const SkinBuiltinPropertySelector&, SkinIntegerPropertyDomain) override;
  SkinPropertyLookup<std::string_view> stringProperty(
      const SkinBuiltinPropertySelector&) override;
};
~~~

Transcribe result-relevant upstream factories into the catalog. Read RhythmState, ChartMeta, ResultPresentationModel, timing analytics, gauge history, and course session directly. For missing local/remote/course data return supported = false, never invented values. Reuse graph paths. Map only equivalent app actions and report an unavailable action at runtime without invalidating the entry.

- [ ] **Step 4: Run bridge and renderer regressions.**

Run: cmake --build cmake-build-debug --target result_skin_state_bridge_tests skin_renderer_golden_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(result_skin_state_bridge_tests|skin_renderer_golden_tests)$' --output-on-failure

Expected: bridge mappings and existing renderer goldens pass.

- [ ] **Step 5: Commit.**

~~~bash
git add src/skin/beatoraja/ResultSkin* src/skin/beatoraja/Skin2DRenderer.* src/scene/ResultScene.* tests/result_skin_state_bridge_tests.cpp tests/skin_renderer_golden_tests.cpp CMakeLists.txt
git commit -m "feat: bridge result data into beatoraja skins"
~~~

### Task 5: Create the result session and wire ResultScene

**Files:**

- Create: src/skin/beatoraja/ResultSkinSession.*
- Create: src/scene/ResultSkinSessionFactory.*
- Modify: src/scene/ResultScene.*
- Modify: src/skin/CMakeLists.txt
- Modify: src/scene/CMakeLists.txt
- Create: tests/result_skin_session_factory_tests.cpp
- Modify: tests/result_contract_tests.cpp
- Modify: CMakeLists.txt

**Interfaces:**

- Produces ResultSkinSession::create(ValidatedSkinActivation, ResultSkinSessionContext).
- Produces ResultSkinSession::render(RenderContext&, ResultSkinFrameSnapshot).
- Produces ResultSkinSessionResult { BuiltIn, Ready, Failed }.

- [ ] **Step 1: Write failing selection/failure tests.**

~~~cpp
TEST_CASE("music result requests type 7") {
  auto result = createResultSkinSession(services, {.target = *skinTargetTraitForType(7)});
  CHECK(result.disposition == ResultSkinSessionDisposition::Ready);
}
TEST_CASE("selected activation failure does not build DefaultSkin") {
  auto result = createResultSkinSession(failingServices, {.target = *skinTargetTraitForType(7)});
  CHECK(result.disposition == ResultSkinSessionDisposition::Failed);
  CHECK_FALSE(result.session);
}
~~~

- [ ] **Step 2: Run the session test and verify RED.**

Run: cmake --build cmake-build-debug --target result_skin_session_factory_tests -j 6 && ctest --test-dir cmake-build-debug -R '^result_skin_session_factory_tests$' --output-on-failure

Expected: compilation fails because the session/factory does not exist.

- [ ] **Step 3: Implement session ownership and scene dispatch.**

~~~cpp
const auto target = skinTargetTraitForType(isCourseFinalResult() ? 15 : 7);
auto selected = createResultSkinSession(
    std::move(services), {.target = *target, .initialData = makeResultSkinData()});
~~~

Mirror the gameplay factory's revision-lease, preparation, diagnostics, configuration-write, and stop-token ownership. ResultScene builds DefaultSkin only for deliberate BuiltIn. It renders Ready sessions with fresh bridge snapshots, retains Failed selected-skin state, and destroys the session before clearing result state.

- [ ] **Step 4: Run session and result-contract tests.**

Run: cmake --build cmake-build-debug --target result_skin_session_factory_tests result_contract_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(result_skin_session_factory_tests|result_contract_tests)$' --output-on-failure

Expected: built-in, ready, and selected-failure behavior passes.

- [ ] **Step 5: Commit.**

~~~bash
git add src/skin/beatoraja/ResultSkinSession.* src/scene/ResultSkinSessionFactory.* src/scene/ResultScene.* src/skin/CMakeLists.txt src/scene/CMakeLists.txt tests/result_skin_session_factory_tests.cpp tests/result_contract_tests.cpp CMakeLists.txt
git commit -m "feat: render selected beatoraja result skins"
~~~

### Task 6: Rename Gameplay Skins to Skins and include result traits

**Files:**

- Rename: src/scene/GameplaySkinSettingsController.* to src/scene/SkinSettingsController.*
- Rename: src/scene/GameplaySkinSettingsPresentation.* to src/scene/SkinSettingsPresentation.*
- Modify: src/scene/SettingsScene.h
- Modify: src/scene/SettingsSceneSkins.cpp
- Modify: src/scene/SettingsSceneSkinsUnavailable.cpp
- Modify: src/scene/CMakeLists.txt
- Modify: tests/gameplay_skin_settings_ui_contract_tests.py
- Create: tests/skin_settings_controller_tests.cpp

**Interfaces:**

- Produces user-facing Skins tab and traits card sourced from skinTargetTraits().
- Consumes target-aware catalog metadata from Tasks 1-2.

- [ ] **Step 1: Write failing UI/controller tests.**

~~~python
def test_skins_tab_includes_result_targets():
    source = (ROOT / "src/scene/SettingsSceneSkins.cpp").read_text()
    assert '"Skins"' in source
    assert "skinTargetTraits()" in source
    assert '"Result"' in source and '"Course Result"' in source
~~~

~~~cpp
TEST_CASE("traits card projects a selection row for type 15") {
  const auto snapshot = controller.snapshot();
  CHECK(std::ranges::any_of(snapshot.targets, [](const auto &row) {
    return row.skinType == 15 && row.label == "Course Result";
  }));
}
~~~

- [ ] **Step 2: Run UI/controller tests and verify RED.**

Run: cmake --build cmake-build-debug --target skin_settings_controller_tests -j 6 && ctest --test-dir cmake-build-debug -R '^skin_settings_controller_tests$' --output-on-failure && python3 tests/gameplay_skin_settings_ui_contract_tests.py

Expected: no type-15 row and the old Gameplay Skins tab contract remains.

- [ ] **Step 3: Rename the public settings surface and project all traits.**

~~~cpp
for (const auto &target : skinTargetTraits()) {
  snapshot.targets.push_back(projectTargetRow(target, catalog, profile));
}
~~~

Update user-visible text from Gameplay Skins to Skins. Preserve configuration controls. Show a selectable entry only when catalog metadata has the exact target type; retain invalid/unavailable entries in revalidation/removal views.

- [ ] **Step 4: Run settings and lifecycle regressions.**

Run: cmake --build cmake-build-debug --target skin_settings_controller_tests gameplay_skin_lifecycle_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(skin_settings_controller_tests|gameplay_skin_lifecycle_tests)$' --output-on-failure && python3 tests/gameplay_skin_settings_ui_contract_tests.py

Expected: settings tests pass and observe the renamed tab.

- [ ] **Step 5: Commit.**

~~~bash
git add src/scene tests/gameplay_skin_settings_ui_contract_tests.py tests/skin_settings_controller_tests.cpp
git commit -m "feat: show result targets in skins settings"
~~~

### Task 7: Extend the Beatoraja reference contract and verify fully

**Files:**

- Modify: tests/beatoraja_skin_reference_tests.py
- Modify: tests/fixtures/beatoraja_skin/reference_manifest.json
- Modify: docs/skin-compat/beatoraja-lua-gameplay-contract.md to use target-neutral scope wording
- Modify: CMakeLists.txt

**Interfaces:**

- Produces reference coverage for upstream default result and course-result paths without a second compatibility authority.

- [ ] **Step 1: Write the failing reference check.**

~~~python
def test_reference_manifest_covers_result_skins(self):
    manifest = self.require_manifest()
    self.assertIn("skin/default/result/result.luaskin", manifest["requiredPaths"])
    self.assertIn("skin/default/graderesult.json", manifest["requiredPaths"])
~~~

- [ ] **Step 2: Run the reference test and verify RED.**

Run: python3 tests/beatoraja_skin_reference_tests.py

Expected: failure because the manifest does not yet require result paths.

- [ ] **Step 3: Update the existing authority artifacts.**

Add the two source paths plus result loader/property-factory paths to the existing manifest and target-neutral documentation. Retain the current commit/version check; do not create another source of truth.

- [ ] **Step 4: Run full verification.**

~~~bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
python3 tests/beatoraja_skin_reference_tests.py
~~~

Expected: every command exits 0. If CTest has an unrelated baseline failure, record its target and output before requesting direction; do not claim full verification.

- [ ] **Step 5: Commit.**

~~~bash
git add tests/beatoraja_skin_reference_tests.py tests/fixtures/beatoraja_skin/reference_manifest.json docs/skin-compat CMakeLists.txt
git commit -m "test: cover beatoraja result skin reference"
~~~
