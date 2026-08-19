# IR Account Skin Property Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Beatoraja string property `1021` by capturing the first successfully authenticated IR account name and supplying it to every gameplay-skin presentation.

**Architecture:** Beatoraja constructs `MainController.IRStatus` only after `IRConnection.login` succeeds, and `StringPropertyFactory.irUserName` returns `ir[0].player.name` or an empty string. Aso will add an authenticated-account read to its existing IR driver boundary; the Tachi driver will request the authoritative `GET /api/v1/users/me` user document using the active API key. `ApplicationContext` will refresh the first successful account snapshot whenever it starts or activates the profile's IR services, and live play/replay-export authorities will capture the resulting name. A failed or absent authenticated response leaves the source empty string.

**Tech Stack:** C++23, existing `ir::IrDriver`/`TachiDriver`, `IrHttpClient`, nlohmann JSON, gameplay skin state bridge, CMake/CTest.

**Spec:** [`docs/todo.md`](../../todo.md), pinned Beatoraja `MainController.java` and `StringPropertyFactory.java` at `c2ed5db1a46145ed10790c3872f717e95b59db9d`; Tachi's maintained `GET /api/v1/users/me` implementation from the `zkldi/Tachi` source checkout.

## Global Constraints

- Keep the property source-faithful: use the first successful authenticated IR account, never the local profile name, provider id, API credential, remote numeric receipt ID, or a fabricated fallback.
- Preserve Beatoraja's empty-string branch when no authenticated account exists or the authenticated account request fails.
- Use the existing normalized provider origin and encrypted API-key lookup; account display names stay runtime presentation state and are not persisted in settings or replay payloads.
- Do not add a skin-specific input validation or normalization rule to a source account name.
- Keep `vcpkg_installed/` untracked and unstaged.
- Follow red/green TDD and commit the independently usable feature before its documentation update.

---

### Task 1: Authenticated IR account driver contract

**Files:**

- Modify: `src/ir/IrDriver.h`
- Modify: `src/ir/IrDriver.cpp`
- Modify: `src/ir/tachi/TachiDriver.h`
- Modify: `src/ir/tachi/TachiDriver.cpp`
- Modify: `tests/tachi_driver_tests.cpp`

**Interfaces:**

- Consumes: `IrProviderRuntimeConfig` and `IrHttpClient`.
- Produces: `IrAuthenticatedAccountOutcome` through `IrDriverRegistry::fetchAuthenticatedAccount(providerId, config, http, stopToken)`, with an account name only after a successful authenticated response.

- [x] **Step 1: Write the failing authenticated-account driver tests**

  Add a `TachiDriver` test with the existing fake HTTP client that supplies a complete `GET https://boku.tachi.ac/api/v1/users/me` response containing literal `id` and `username` values. Assert the driver outcome is successful, returns the literal username, and reports no credential. Add an unauthenticated response fixture and assert it has no account name.

- [x] **Step 2: Run the focused driver test to verify it fails**

  Run: `cmake --build cmake-build-debug --target tachi_driver_tests -j 6 && ./cmake-build-debug/tachi_driver_tests`

  Expected: failure because the authenticated-account driver/registry API does not exist.

- [x] **Step 3: Implement the driver contract and Tachi request**

  Add the outcome/status/value types and the registry forwarding method beside the existing ranking and reconciliation driver operations. Make the default driver return the existing-style unsupported outcome. In `TachiDriver`, request `GET <normalized-origin>/api/v1/users/me` with `Authorization: Bearer <apiKey>` and parse the response object's `username` field without substituting any local value. Map the established HTTP/transport conditions to the new outcome and return no name for all non-success cases.

- [x] **Step 4: Run the focused driver test to verify it passes**

  Run: `cmake --build cmake-build-debug --target tachi_driver_tests -j 6 && ./cmake-build-debug/tachi_driver_tests`

  Expected: the literal authenticated username is returned and an unauthenticated result contains no account name.

### Task 2: First-connected runtime authority and skin projection

**Files:**

- Modify: `src/context.h`
- Modify: `src/scene/play/PlayfieldVisualState.h`
- Modify: `src/scene/play/PlayfieldVisualState.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Modify: `tests/play_skin_state_bridge_tests.cpp`
- Modify: `tests/playfield_visual_state_tests.cpp`

**Interfaces:**

- Consumes: current profile settings, `ApplicationContext` secure credential lookup, registered IR drivers, and the authenticated-account outcome.
- Produces: `ApplicationContext`'s first connected account display name, copied to `PlayfieldAuthorityUpdate::irAccountName` and returned for skin string property `1021`.

- [x] **Step 1: Write the failing skin-authority tests**

  Add a bridge test that captures a literal `irAccountName` and verifies selector `1021` returns it. Add a second captured state with no account name and verify selector `1021` stays the empty source branch. The mutation these tests must catch is replacing `1021` with the local profile name, provider id, or an unconditional empty string.

- [x] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target play_skin_state_bridge_tests playfield_visual_state_tests -j 6 && ./cmake-build-debug/play_skin_state_bridge_tests && ./cmake-build-debug/playfield_visual_state_tests`

  Expected: the bridge currently falls through to its generic empty-string branch for `1021`.

- [x] **Step 3: Capture the first successful connected account**

  Add runtime-only account-name state to `ApplicationContext`. After activating the HTTP transport/cache and whenever a profile's IR services are started or activated, iterate the profile's configured providers in their existing order, read each active credential, invoke the driver authenticated-account operation, and retain the first success. Clear the snapshot before each refresh so a failed new login has the same empty `IRStatus[]` effect as the source. Do not persist the returned account name or derive it from receipt/cache IDs.

- [x] **Step 4: Project the captured name through every gameplay presenter**

  Add `irAccountName` to the immutable playfield authority/state equality. Set it in live `GamePlayScene` capture and every normal/course replay-export authority construction. Make `PlaySkinStateBridge::stringProperty(1021)` return that field directly.

- [x] **Step 5: Run focused tests to verify they pass**

  Run: `cmake --build cmake-build-debug --target tachi_driver_tests play_skin_state_bridge_tests playfield_visual_state_tests main -j 6 && ./cmake-build-debug/tachi_driver_tests && ./cmake-build-debug/play_skin_state_bridge_tests && ./cmake-build-debug/playfield_visual_state_tests`

  Expected: `1021` returns the connected account name only when captured, and the desktop main target links.

- [x] **Step 6: Commit the IR account property feature**

  Run: `git add src/ir/IrDriver.h src/ir/IrDriver.cpp src/ir/tachi/TachiDriver.h src/ir/tachi/TachiDriver.cpp src/context.h src/scene/play/PlayfieldVisualState.h src/scene/play/PlayfieldVisualState.cpp src/scene/play/GamePlayScene.cpp src/ReplayVideoExporter.cpp src/skin/beatoraja/PlaySkinStateBridge.cpp tests/tachi_driver_tests.cpp tests/play_skin_state_bridge_tests.cpp && git commit -m "feat: expose connected IR account to gameplay skins"`

### Task 3: Record the completed compatibility property and verify platforms

**Files:**

- Modify: `docs/todo.md`
- Modify: `docs/progress.md`
- Modify: `docs/superpowers/plans/2026-08-20-ir-account-skin-property.md`

- [x] **Step 1: Update compatibility documentation**

  Replace the unresolved `1021` entry with completed behavior: the first successfully authenticated IR account name is captured from the source-equivalent login path; absent/failed authentication remains empty; local profile and provider text are never substituted.

- [x] **Step 2: Run desktop verification**

  Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 1`

  Expected: all tests pass serially; the suite uses serial execution because the headless renderer characterization test shares the Metal resource.

- [x] **Step 3: Run iOS build-only verification**

  Run: `scripts/ios_firebase_deploy.sh --build-only`

  Expected: `** BUILD SUCCEEDED **`; no Firebase upload occurs.

- [x] **Step 4: Commit the documentation result and push**

  Run: `git add docs/todo.md docs/progress.md docs/superpowers/plans/2026-08-20-ir-account-skin-property.md && git commit -m "docs: record IR account skin compatibility" && git push origin feature/skin-compat`

## Self-review

- Spec coverage: Task 1 makes the existing authenticated IR driver own the server request; Task 2 keeps the source's first-successful-account and empty branches through live/replay presentation; Task 3 proves and records completion.
- Placeholder scan: no omitted source field, fallback account source, or unspecified test command remains.
- Type consistency: the driver returns an account name, `ApplicationContext` owns its runtime snapshot, and the playfield authority transports the snapshot to selector `1021`.
