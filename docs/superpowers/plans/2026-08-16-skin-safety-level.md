# Skin Safety Level Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist an owner-acknowledged skin safety level and route every gameplay-skin host safeguard through it.

**Architecture:** A profile setting drives one `SkinSafetyPolicy`. Every guard is classified as `Protective` or `Catastrophic`: Standard enforces both, BeatorajaCompatibility enforces Catastrophic only, and Unrestricted bypasses both. The policy is passed through package/catalog, Lua, and resource construction; the UI requires a modal acknowledgement before persisting Unrestricted.

**Tech Stack:** C++20, AppSettings JSON migration, SettingsScene, LuaJIT host, CMake/CTest.

## Global Constraints

- Default old and new profiles to `Standard`.
- Do not trigger a skin rescan from a safety-level change.
- Do not alter Standard behavior or diagnostics.
- Unrestricted may lift every security/resource guard after confirmation; malformed data and ordinary I/O errors still fail.
- Keep the existing legacy `gameplayCompatibilityEnabled` selection alias separate.

---

### Task 1: Add persisted safety level and warning acknowledgement

**Files:**
- Modify: `src/skin/SkinProfileSettings.h`, `src/skin/SkinProfileSettings.cpp`
- Modify: `src/AppSettingsStore.h`, `src/AppSettingsStore.cpp`
- Modify: `src/scene/GameplaySkinSettingsController.h`, `src/scene/GameplaySkinSettingsController.cpp`
- Modify: `src/scene/GameplaySkinSettingsPresentation.h`, `src/scene/GameplaySkinSettingsPresentation.cpp`, and the Gameplay Skins SettingsScene builder
- Test: `tests/app_settings_store_tests.cpp`, `tests/gameplay_skin_settings_controller_tests.cpp`, `tests/gameplay_skin_settings_presentation_tests.cpp`

**Interfaces:**
- Produce `enum class SkinSafetyLevel : std::uint8_t { Standard, BeatorajaCompatibility, Unrestricted };`.
- Produce `setSafetyLevel(SkinSafetyLevel)`, `confirmSafetyLevelChange()`, and `cancelSafetyLevelChange()` on the settings controller.

- [ ] **Step 1: Write failing persistence and modal tests**

```cpp
expect(loaded.settings.skin.safetyLevel == SkinSafetyLevel::Standard,
       "missing safety level defaults to Standard");
expect(controller.setSafetyLevel(SkinSafetyLevel::Unrestricted).requiresConfirmation,
       "Unrestricted requires acknowledgement");
```

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target app_settings_store_tests gameplay_skin_settings_controller_tests gameplay_skin_settings_presentation_tests -j 12 > /tmp/skin-safety-task1-red-build.log 2>&1 && ./cmake-build-debug/app_settings_store_tests && ./cmake-build-debug/gameplay_skin_settings_controller_tests && ./cmake-build-debug/gameplay_skin_settings_presentation_tests`

- [ ] **Step 3: Implement the enum, JSON migration, dropdown, and modal**

```cpp
ControllerActionResult setSafetyLevel(SkinSafetyLevel requested);
ControllerActionResult confirmSafetyLevelChange();
void cancelSafetyLevelChange() noexcept;
```

The modal must identify external filesystem access, process-wide side effects, and resource exhaustion. Its cancel path must leave the persisted snapshot unchanged.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 command. Then commit settings/controller/presentation/tests with `git commit -m "Add skin safety level setting"`.

### Task 2: Introduce one policy for package and virtual-file safeguards

**Files:**
- Create: `src/skin/SkinSafetyPolicy.h`, `src/skin/SkinSafetyPolicy.cpp`
- Modify: `src/skin/package/SkinArchiveImporter.h`, `src/skin/package/SkinArchiveImporter.cpp`
- Modify: `src/skin/package/SkinTreeSnapshotter.h`, `src/skin/package/SkinTreeSnapshotter.cpp`
- Modify: `src/skin/package/SkinPackageStore.h`, `src/skin/package/SkinPackageStore.cpp`
- Modify: `src/skin/beatoraja/LuaSkinFileSystem.h`, `src/skin/beatoraja/LuaSkinFileSystem.cpp`
- Test: `tests/skin_archive_importer_tests.cpp`, `tests/lua_skin_file_system_tests.cpp`

**Interfaces:**
- Produce `SkinSafetyPolicy::enforces(SkinSafetyGuard)` and `SkinSafetyGuardSeverity` for package resource limits, package path containment, virtual-file containment, and virtual-file write quota.

- [ ] **Step 1: Write failing policy cases**

```cpp
expect(standard.failure->code == "skin_archive_package_limit_exceeded",
       "Standard keeps the expansion limit");
expect(!unrestricted.failure,
       "Unrestricted accepts the same otherwise-valid package");
```

Add matching Standard/Unrestricted tests for an authored virtual write path outside its package.

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target skin_archive_importer_tests lua_skin_file_system_tests -j 12 > /tmp/skin-safety-task2-red-build.log 2>&1 && ./cmake-build-debug/skin_archive_importer_tests && ./cmake-build-debug/lua_skin_file_system_tests`

- [ ] **Step 3: Implement policy injection at existing branches**

```cpp
enum class SkinSafetyGuardSeverity : std::uint8_t { Protective, Catastrophic };
enum class SkinSafetyGuard : std::uint8_t {
  PackageResourceLimit, PackagePathContainment,
  VirtualFileContainment, VirtualFileWriteQuota,
};
```

Only existing protection checks consult the policy. Their severity is declared
alongside the policy guard, so BeatorajaCompatibility and Unrestricted have
distinct, testable behavior. Do not add scans or new Lua/model validation.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 command. Commit with `git commit -m "Route package safeguards through skin safety policy"`.

### Task 3: Route Lua runtime and decoder safeguards through the policy

**Files:**
- Modify: `src/skin/beatoraja/LuaSkinRuntime.h`, `src/skin/beatoraja/LuaSkinRuntime.cpp`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.h`, `src/skin/beatoraja/LuaSkinHostModules.cpp`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.h`, `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Modify: `src/skin/beatoraja/LuaSkinBindingDecoder.h`, `src/skin/beatoraja/LuaSkinBindingDecoder.cpp`
- Test: `tests/lua_skin_runtime_tests.cpp`, `tests/lua_skin_host_modules_tests.cpp`, `tests/lua_skin_table_decoder_tests.cpp`, `tests/lua_skin_binding_decoder_tests.cpp`

**Interfaces:**
- Consume `SkinSafetyPolicy` from filesystem/runtime construction.
- Produce policy-aware load/callback/table/binding limits with unchanged Standard constants.

- [ ] **Step 1: Write failing Standard/Unrestricted runtime tests**

```cpp
expect(catalogStandard.failure->code == "skin_lua_host_limit_exceeded",
       "Standard rejects oversized source before host allocation");
expect(catalogUnrestricted.value.has_value(),
       "Unrestricted loads the valid oversized source");
```

Cover allocator, instruction, wall-time, host file-read, table depth/entries, and binding work limits.

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target lua_skin_runtime_tests lua_skin_host_modules_tests lua_skin_table_decoder_tests lua_skin_binding_decoder_tests -j 12 > /tmp/skin-safety-task3-red-build.log 2>&1 && ./cmake-build-debug/lua_skin_runtime_tests && ./cmake-build-debug/lua_skin_host_modules_tests && ./cmake-build-debug/lua_skin_table_decoder_tests && ./cmake-build-debug/lua_skin_binding_decoder_tests`

- [ ] **Step 3: Use policy-aware existing limits**

```cpp
LuaLoadBudget loadBudget(LuaRuntimePurpose purpose,
                         const SkinSafetyPolicy &policy) noexcept;
```

Expose process-global Lua functions such as `os.setlocale` only in Unrestricted. Keep syntax/runtime/I/O failure semantics unchanged.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 command. Commit with `git commit -m "Make Lua skin safeguards policy-aware"`.

### Task 4: Route resource safeguards and verify end-to-end policy propagation

**Files:**
- Modify: `src/skin/beatoraja/SkinResourceCatalog.h`, `src/skin/beatoraja/SkinResourceCatalog.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`, `src/skin/beatoraja/PlaySkinSession.cpp`
- Test: `tests/skin_resource_catalog_tests.cpp`, `tests/play_skin_session_tests.cpp`

**Interfaces:**
- Consume the runtime policy from Task 3.
- Produce policy-aware resource session, decoded-image, glyph, atlas, and encoded-byte checks.

- [ ] **Step 1: Write failing resource policy tests**

```cpp
expect(standard.diagnostics.front().code == "skin.resource.session_limit",
       "Standard keeps the session allocation limit");
expect(unrestricted.resources.size() == authoredCount,
       "Unrestricted retains otherwise-valid resources");
```

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target skin_resource_catalog_tests play_skin_session_tests -j 12 > /tmp/skin-safety-task4-red-build.log 2>&1 && ./cmake-build-debug/skin_resource_catalog_tests && ./cmake-build-debug/play_skin_session_tests`

- [ ] **Step 3: Thread the policy through PlaySkinSession and resources**

Do not bypass device texture limits or actual decoder failures; those are platform capabilities, not host policy rejections.

- [ ] **Step 4: Verify GREEN, full desktop suite, and commit**

Run: `cmake --build cmake-build-debug -j 12 > /tmp/skin-safety-full-build.log 2>&1 && ctest --test-dir cmake-build-debug --output-on-failure -j 12 > /tmp/skin-safety-full-ctest.log 2>&1`

Commit with `git commit -m "Apply skin safety policy to resource decoding"`.
