# Gameplay Skin Trait Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users select and configure one Lua gameplay skin per supported
key-mode trait without triggering scans at app startup or Settings entry.

**Architecture:** Define Beatoraja-pinned gameplay traits once, persist selected
entries by header type, and resolve that mapping with the chart's key mode at
gameplay activation. The Settings UI renders that same trait list as a vertical
tab column and filters the selected panel's dropdown/configuration body from
the immutable catalog snapshot.

**Tech Stack:** C++23, CMake/Ninja, SDL/bgfx custom views, existing
`DropdownView`, profile JSON persistence, Beatoraja Lua gameplay runtime.

## Global Constraints

- Use Beatoraja commit `c2ed5db1a46145ed10790c3872f717e95b59db9d` as the
  canonical `SkinType` mapping: 5K=type 1, 7K=0, 9K=4, 10K=3, 14K=2,
  24K=16, 24K Double=17.
- Do not add Result, Decide, or battle skin traits.
- Startup and Settings-tab selection must not request a rescan; explicit
  Rescan and completed package mutations remain refresh paths.
- Preserve unrelated untracked utf8proc and Python-cache work.
- Use focused desktop tests first, redirect build logs to `/tmp`, then run
  `scripts/ios_release_verify.sh` after the lifecycle/context signature change.
- Never deploy or upload a build.

---

### Task 1: Define the canonical gameplay-trait contract and migrate profile settings

**Files:**
- Create: `src/skin/GameplaySkinTraits.h`
- Create: `src/skin/GameplaySkinTraits.cpp`
- Modify: `src/skin/SkinProfileSettings.h`
- Modify: `src/skin/SkinProfileSettings.cpp`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/gameplay_skin_traits_tests.cpp`
- Modify: `tests/app_settings_store_tests.cpp`
- Modify: `tests/profile_settings_persistence_tests.cpp`

**Interfaces:**
- Produces `skin::GameplaySkinTrait` with `skinType`, `keyMode`, and `label`.
- Produces `gameplaySkinTraits()`, `gameplaySkinTraitForSkinType(int)`, and
  `gameplaySkinTraitForKeyMode(int)`.
- Adds authoritative `std::map<int, SkinEntryId> selectedGameplayEntries`.
  `selected7KeyEntry` and `gameplayCompatibilityEnabled` remain derived,
  non-serialized compatibility aliases until their downstream consumers move.

- [ ] **Step 1: Write the failing trait/migration tests**

```cpp
require(gameplaySkinTraitForSkinType(3)->keyMode == 10,
        "Beatoraja play10 type maps to 10K");
require(gameplaySkinTraitForKeyMode(48)->skinType == 17,
        "24K Double maps to a 48-key chart");
require(loaded.settings.skin.selectedGameplayEntries.at(0) == legacyEntry,
        "legacy selected7KeyEntry migrates to the 7K trait");
require(!encoded.at("skin").contains("selected7KeyEntry"),
        "new profiles no longer emit the legacy 7K key");
```

- [ ] **Step 2: Run the new tests and confirm the expected compile/test failure**

Run: `cmake --build cmake-build-debug --target gameplay_skin_traits_tests app_settings_store_tests -j 6 > /tmp/gameplay-skin-traits-red.log 2>&1`

Expected: `gameplay_skin_traits_tests` is absent or fails because trait lookup
and `selectedGameplayEntries` do not exist.

- [ ] **Step 3: Implement the minimal canonical mapping and persistence migration**

```cpp
struct GameplaySkinTrait {
  int skinType = -1;
  int keyMode = 0;
  std::string_view label;
};

inline constexpr std::array kGameplaySkinTraits{
    GameplaySkinTrait{1, 5, "5K"}, GameplaySkinTrait{0, 7, "7K"},
    GameplaySkinTrait{4, 9, "9K"}, GameplaySkinTrait{3, 10, "10K"},
    GameplaySkinTrait{2, 14, "14K"}, GameplaySkinTrait{16, 24, "24K"},
    GameplaySkinTrait{17, 48, "24K Double"}};
```

`readSkinProfileSettings` reads `selectedGameplayEntries` first, then maps a
valid legacy `selected7KeyEntry` to type 0 only when no type-0 entry was
already present. Serialization emits only the new object keyed by decimal skin
type. `sanitize()` normalizes every selected entry, removes keys that are not
returned by `gameplaySkinTraitForSkinType`, then derives the old 7K aliases
from the type-0 map entry.

- [ ] **Step 4: Run the focused green tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_traits_tests|app_settings_store_tests|profile_settings_persistence_tests'`

Expected: all selected tests pass; migration retains only valid 7K legacy data
and independent type selections survive a save/load cycle.

- [ ] **Step 5: Commit the isolated settings-model slice**

```bash
git add src/skin/GameplaySkinTraits.* src/skin/SkinProfileSettings.* \
  src/AppSettingsStore.cpp src/skin/CMakeLists.txt CMakeLists.txt \
  tests/gameplay_skin_traits_tests.cpp tests/app_settings_store_tests.cpp \
  tests/profile_settings_persistence_tests.cpp
git commit -m "feat: persist gameplay skin selections by trait"
```

### Task 2: Make validation, catalog reconciliation, and activation trait-aware

**Files:**
- Modify: `src/skin/package/SkinPackageTypes.h`
- Modify: `src/skin/package/SkinPackageCatalog.cpp`
- Modify: `src/skin/beatoraja/GameplaySkinValidator.cpp`
- Modify: `src/skin/package/SkinPackageStore.cpp`
- Modify: `tests/gameplay_skin_validator_tests.cpp`
- Modify: `tests/skin_package_store_tests.cpp`

**Interfaces:**
- Produces `SkinValidationDisposition::SelectableGameplay` for a reconciled
  Lua entry whose metadata type is a supported `GameplaySkinTrait`.
- `submitPrepareActivation` validates an entry against the requested trait
  type and writes only `selectedGameplayEntries[skinType]`.

- [ ] **Step 1: Write failing multi-trait validation and package tests**

```cpp
expect(result.disposition == SkinValidationDisposition::SelectableGameplay &&
           result.metadata->skinType == 3,
       "a reconciled type-3 Lua header is a selectable 10K gameplay skin");
expect(profile.settings.selectedGameplayEntries.at(0) == seven &&
           profile.settings.selectedGameplayEntries.at(3) == ten,
       "rescanning a package preserves selections for distinct gameplay traits");
```

- [ ] **Step 2: Run the focused tests and confirm they fail for the old 7K-only disposition**

Run: `cmake --build cmake-build-debug --target gameplay_skin_validator_tests skin_package_store_tests -j 6 > /tmp/gameplay-skin-catalog-red.log 2>&1 && ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_validator_tests|skin_package_store_tests'`

Expected: assertions fail because every validated Lua header is named and
handled as `Selectable7Key` and package loops inspect only one selected entry.

- [ ] **Step 3: Implement the smallest trait-aware catalog change**

```cpp
if (!gameplaySkinTraitForSkinType(decodedHeader.header->type)) {
  result.disposition = SkinValidationDisposition::UnavailableType;
  return result;
}
result.disposition = SkinValidationDisposition::SelectableGameplay;
```

Update catalog encoding to decode legacy `"selectable7Key"` as
`SelectableGameplay`. Replace each selected-entry special case in publication,
rescan, package replacement, removal, and activation preparation with a loop
over `selectedGameplayEntries`; reject a selected entry whose validated header
type does not match its map key.

- [ ] **Step 4: Run focused green tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_validator_tests|skin_package_store_tests'`

Expected: all selected entries retain their independent validated
configuration digests and wrong-type selections are rejected.

- [ ] **Step 5: Commit the catalog/activation slice**

```bash
git add src/skin/package/SkinPackageTypes.h src/skin/package/SkinPackageCatalog.cpp \
  src/skin/beatoraja/GameplaySkinValidator.cpp src/skin/package/SkinPackageStore.cpp \
  tests/gameplay_skin_validator_tests.cpp tests/skin_package_store_tests.cpp
git commit -m "feat: validate gameplay skins by trait"
```

### Task 3: Project and persist selection per trait through the settings controller

**Files:**
- Modify: `src/scene/GameplaySkinSettingsController.h`
- Modify: `src/scene/GameplaySkinSettingsController.cpp`
- Modify: `src/scene/GameplaySkinSettingsPresentation.cpp`
- Modify: `tests/gameplay_skin_settings_tests.cpp`
- Modify: `tests/gameplay_skin_settings_presentation_tests.cpp`

**Interfaces:**
- `GameplaySkinSettingsSnapshot::selectedGameplayEntries` mirrors the profile
  mapping.
- `selectTrait(int skinType, const SkinEntryId&)` validates and saves one
  trait selection.
- `clearTrait(int skinType)` removes one selection and leaves all other traits
  and entry configuration intact.

- [ ] **Step 1: Write failing controller/presentation tests**

```cpp
expect(controller->selectTrait(3, tenEntry).accepted,
       "the 10K panel can select a matching 10K skin");
expect(controller->clearTrait(3).accepted,
       "Built-in presentation clears one trait");
expect(owner.snapshot(profile).settings.selectedGameplayEntries.at(0) == seven,
       "clearing 10K leaves 7K selected");
requirePresentationChange(base,
    [](auto &v) { v.selectedGameplayEntries[3] = entryId("-10"); },
    "trait selection changes rebuild the panel");
```

- [ ] **Step 2: Run the controller and presentation tests to prove red**

Run: `cmake --build cmake-build-debug --target gameplay_skin_settings_tests gameplay_skin_settings_presentation_tests -j 6 > /tmp/gameplay-skin-controller-red.log 2>&1 && ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_settings_tests|gameplay_skin_settings_presentation_tests'`

Expected: methods and snapshot field are unavailable; existing controller
selection remains 7K-only.

- [ ] **Step 3: Implement per-trait actions without changing entry-local controls**

```cpp
ControllerActionResult GameplaySkinSettingsController::selectTrait(
    int skinType, const SkinEntryId &entry) {
  const auto trait = gameplaySkinTraitForSkinType(skinType);
  const auto *catalog = impl_->findCatalogEntry(entry, impl_->catalog());
  if (!trait || !catalog || !catalog->metadata ||
      catalog->metadata->skinType != trait->skinType ||
      catalog->validation != SkinValidationDisposition::SelectableGameplay) {
    return rejected("The skin does not match this gameplay trait.");
  }
  return impl_->prepareActivation(entry, trait->skinType, candidate,
                                  "Validating selected skin…");
}
```

Retain `setOption`, `setFileChoice`, `setOffset`, `setViewport`, and
`resetLayout` as entry-local operations. Encode the full map in the
presentation key so switching a selected trait rebuilds the Settings surface.

- [ ] **Step 4: Run the focused green tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_settings_tests|gameplay_skin_settings_presentation_tests'`

Expected: selection/clearing are type-isolated, presentation keys change for
each trait mapping, and all existing entry configuration tests remain green.

- [ ] **Step 5: Commit the controller slice**

```bash
git add src/scene/GameplaySkinSettingsController.* \
  src/scene/GameplaySkinSettingsPresentation.cpp \
  tests/gameplay_skin_settings_tests.cpp \
  tests/gameplay_skin_settings_presentation_tests.cpp
git commit -m "feat: select gameplay skins per trait"
```

### Task 4: Resolve a trait by chart key mode and stop implicit rescans

**Files:**
- Modify: `src/skin/GameplaySkinActivationRequest.h`
- Modify: `src/skin/GameplaySkinLifecycle.h`
- Modify: `src/skin/GameplaySkinLifecycle.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `tests/gameplay_skin_lifecycle_tests.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `tests/play_skin_session_tests.cpp`

**Interfaces:**
- `AcquireGameplaySkinForNextChart` accepts `int keyMode`.
- `GameplaySkinLifecycle::acquireForNextChart(int keyMode)` chooses only the
  corresponding selected trait.

- [ ] **Step 1: Write failing lifecycle/startup tests**

```cpp
lifecycle.startAfterProfileInitialization(profile);
expect(fake.rescanTickets.empty(), "startup never schedules a skin rescan");
expect(lifecycle.acquireForNextChart(10)->activation.entry == tenEntry,
       "10K gameplay acquires the persisted 10K trait");
expect(!lifecycle.acquireForNextChart(14),
       "an unselected trait leaves the built-in presentation active");
```

- [ ] **Step 2: Run the affected tests and confirm red**

Run: `cmake --build cmake-build-debug --target gameplay_skin_lifecycle_tests gameplay_playback_startup_tests play_skin_session_tests -j 6 > /tmp/gameplay-skin-lifecycle-red.log 2>&1 && ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_lifecycle_tests|gameplay_playback_startup_tests|play_skin_session_tests'`

Expected: startup requests a rescan, the acquisition signature lacks a key
mode, and gameplay returns before anything except a 7K chart.

- [ ] **Step 3: Implement the minimal routing and scan-policy changes**

```cpp
impl_->initialized = true;
impl_->acquisitionReady = true; // catalog recovery completed before construction
impl_->activeProfile = std::move(profile);
// No requestRescan(SkinRescanReason::Startup).

const auto trait = gameplaySkinTraitForKeyMode(keyMode);
if (!trait) return std::nullopt;
const auto selected = base.settings.selectedGameplayEntries.find(trait->skinType);
if (selected == base.settings.selectedGameplayEntries.end()) return std::nullopt;
```

Pass `chart->Meta.KeyMode` through `ApplicationContext` to the lifecycle and
remove the `chart->Meta.KeyMode != 7` early return. Remove the
`requestRescan()` call from the Gameplay Skins Settings-tab click handler;
leave the explicit Rescan button intact.

- [ ] **Step 4: Run focused green tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_lifecycle_tests|gameplay_playback_startup_tests|play_skin_session_tests'`

Expected: no startup/Settings rescan, matching type acquisition for every
supported test key mode, and built-in fallback for missing mappings.

- [ ] **Step 5: Commit the runtime-policy slice**

```bash
git add src/skin/GameplaySkinActivationRequest.h src/skin/GameplaySkinLifecycle.* \
  src/context.h src/scene/play/GamePlayScene.cpp src/scene/SettingsSceneLayout.cpp \
  tests/gameplay_skin_lifecycle_tests.cpp tests/gameplay_playback_startup_tests.cpp \
  tests/play_skin_session_tests.cpp
git commit -m "feat: route gameplay skins by chart trait"
```

### Task 5: Render vertical trait tabs and a filtered skin dropdown

**Files:**
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneSkins.cpp`
- Modify: `src/scene/SettingsSceneSkinsUnavailable.cpp`
- Modify: `tests/gameplay_skin_settings_presentation_tests.cpp`

**Interfaces:**
- `SettingsScene::gameplaySkinActiveTraitType` is the active vertical trait
  tab and defaults to type 0 (7K).
- The selected trait panel uses `DropdownView::State` options whose values are
  stable indices into its filtered entry list; index 0 means Built-in.

- [ ] **Step 1: Add a failing presentation-layout contract**

```cpp
require(gameplaySkinTraitForSkinType(1)->label == "5K" &&
            gameplaySkinTraitForSkinType(2)->label == "14K",
        "the UI consumes canonical vertical-tab labels");
require(filteredDropdownEntries(snapshot, 3).size() == 1,
        "the 10K dropdown excludes skins for every other trait");
```

- [ ] **Step 2: Run the presentation test and confirm red**

Run: `cmake --build cmake-build-debug --target gameplay_skin_settings_presentation_tests -j 6 > /tmp/gameplay-skin-ui-red.log 2>&1 && ctest --test-dir cmake-build-debug --output-on-failure -R gameplay_skin_settings_presentation_tests`

Expected: the filtering helper and trait-tab state do not exist.

- [ ] **Step 3: Replace the per-entry card loop with the trait panel**

```cpp
for (const auto &trait : skin::gameplaySkinTraits()) {
  tabs->addView(makeGameplaySkinAction(metrics, trait.label,
      ordinaryActionsEnabled, [this, type = trait.skinType] {
        gameplaySkinActiveTraitType = type;
        lastLayoutWidth = -1;
      }));
}
```

Build one right-side panel for `gameplaySkinActiveTraitType`. Its dropdown
contains Built-in plus valid entries with matching `metadata.skinType`; its
selection invokes `clearTrait` or `selectTrait`. Render configuration and
entry actions only for the dropdown-selected entry. Keep Install, explicit
Rescan, cancellation, availability, and diagnostic-history cards. Remove the
old flat loop that renders every entry's full configuration simultaneously.

- [ ] **Step 4: Run focused UI/model tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_settings_presentation_tests|gameplay_skin_settings_tests'`

Expected: trait labels/filtering and independent selection persistence pass.

- [ ] **Step 5: Build the desktop executable and commit**

Run: `cmake --build cmake-build-debug --target main -j 6 > /tmp/gameplay-skin-trait-ui-main.log 2>&1`

```bash
git add src/scene/SettingsScene.h src/scene/SettingsSceneSkins.cpp \
  src/scene/SettingsSceneSkinsUnavailable.cpp \
  tests/gameplay_skin_settings_presentation_tests.cpp
git commit -m "feat: organize gameplay skin settings by trait"
```

### Task 6: Verify the integrated behavior and native checkpoint

**Files:**
- Modify: `.superpowers/sdd/progress.md`

**Interfaces:**
- Consumes all trait selection, lifecycle, and Settings UI contracts above.
- Produces one ledger row recording verification and the pinned Beatoraja SHA.

- [ ] **Step 1: Run the integrated desktop suites**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'gameplay_skin_traits_tests|app_settings_store_tests|profile_settings_persistence_tests|gameplay_skin_validator_tests|skin_package_store_tests|gameplay_skin_settings_tests|gameplay_skin_settings_presentation_tests|gameplay_skin_lifecycle_tests|gameplay_playback_startup_tests|play_skin_session_tests'`

Expected: every selected suite passes.

- [ ] **Step 2: Verify a real installed skin per available trait**

Run the existing external-skin header/validation path against
`~/Downloads/Skins`; confirm a 7K skin appears only in the 7K candidate list
and any installed 5K/10K/14K header appears only in its matching list.

- [ ] **Step 3: Run the iOS-sensitive verification without deployment**

Run: `scripts/ios_release_verify.sh > /tmp/gameplay-skin-trait-ios-verify.log 2>&1`

Expected: native test suite passes, unsigned arm64 build reports
`BUILD SUCCEEDED`, and artifact audit passes.

- [ ] **Step 4: Record the result and commit the ledger**

```bash
git add .superpowers/sdd/progress.md
git commit -m "docs: record gameplay skin trait verification"
```
