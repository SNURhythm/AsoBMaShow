#include "skin/beatoraja/LuaSkinRuntime.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

#ifndef ASOBMASHOW_SOURCE_DIR
#define ASOBMASHOW_SOURCE_DIR "."
#endif

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testPurposeSpecificBudgetsAreFixed() {
  using namespace skin;
  constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

  expect(LuaRuntimePolicy::catalogLoad.maxAllocatorBytes == 32 * mebibyte,
         "catalog allocator budget is 32 MiB");
  expect(LuaRuntimePolicy::catalogLoad.maxInstructions == 2'000'000,
         "catalog execution budget is 2,000,000 instructions");
  expect(LuaRuntimePolicy::catalogLoad.maxWallTime.count() == 2'000,
         "catalog execution deadline is 2 seconds");
  expect(LuaRuntimePolicy::validationAndGameplayLoad.maxAllocatorBytes ==
             128 * mebibyte,
         "validation/gameplay allocator budget is 128 MiB");
  expect(LuaRuntimePolicy::validationAndGameplayLoad.maxInstructions ==
             20'000'000,
         "validation/gameplay phase budget is 20,000,000 instructions");
  expect(LuaRuntimePolicy::validationAndGameplayLoad.maxWallTime.count() ==
             10'000,
         "validation/gameplay phase deadline is 10 seconds");
  expect(LuaRuntimePolicy::gameplayCallback.maxInstructions == 250'000,
         "one callback gets 250,000 instructions");
  expect(LuaRuntimePolicy::gameplayCallback.maxWallTime.count() == 4,
         "one callback gets 4 milliseconds");
  expect(LuaRuntimePolicy::gameplayFrame.maxInstructions == 1'000'000,
         "one frame gets 1,000,000 callback instructions");
  expect(LuaRuntimePolicy::gameplayFrame.maxWallTime.count() == 6,
         "one frame gets 6 milliseconds of callback wall time");
}

std::string readFixture(std::string_view relativePath) {
  std::ifstream input(std::string(ASOBMASHOW_SOURCE_DIR) + "/" +
                      std::string(relativePath));
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void testRuntimeContractsUseTheFrozenAuditAndSandboxAuthority() {
  const auto manifest = readFixture(
      "tests/fixtures/beatoraja_skin/reference_manifest.json");
  const auto policy = readFixture(
      "tests/fixtures/beatoraja_skin/policies/lua_sandbox_v1.json");
  const auto legacyTrace = readFixture(
      "tests/fixtures/beatoraja_skin/traces/legacy_lua_upstream_v1.json");

  expect(!manifest.empty(), "Task 1a manifest is available to runtime tests");
  expect(!policy.empty(), "Task 2 sandbox policy is available to runtime tests");
  expect(!legacyTrace.empty(),
         "Task 2 selected upstream call-shape trace is available");
  expect(policy.find(
             "ccfd3ed2e67b991815aefe000fbc221b37064366bd37c148ea2504c0e423a8ed") !=
             std::string::npos,
         "sandbox authority remains bound to the audited selected surface");
  for (const auto capability : {"package-text-dofile", "restricted-io-open",
                                "legacy-file-list", "legacy-overlay-mkdir"}) {
    expect(policy.find(capability) != std::string::npos,
           "runtime probe covers every allowed filesystem capability");
  }
  for (const auto capability : {"network", "reflection", "native-access",
                                "process-execution",
                                "unaudited-legacy-surface"}) {
    expect(policy.find(capability) != std::string::npos,
           "runtime probe covers every denied authority class");
  }
  for (const auto callShape : {"io.open:default", "io.open:w", "io.open:r",
                               "io.open:a", "write:zero", "write:multiple"}) {
    expect(legacyTrace.find(callShape) != std::string::npos,
           "runtime file facade retains selected upstream call-shape parity");
  }
  expect(manifest.find("\"customEvents\": 0") != std::string::npos,
         "selected custom event map remains audited empty");
  expect(manifest.find("\"customTimers\": 0") != std::string::npos,
         "selected custom timer map remains audited empty");
}

#if __has_include("skin/beatoraja/LuaSkinFileSystem.h")

// Task 8 owns construction of the move-only filesystem. These test-only
// adapters deliberately have no production counterpart; their definitions
// belong in this test once Task 8 freezes the real constructor/result API.
skin::LuaRuntimeCreateResult
makeRuntime(skin::LuaRuntimePurpose purpose, std::string_view entryFixture);
skin::LuaCallbackId callbackNamed(skin::LuaValueHandle &value,
                                  std::string_view name);

std::unique_ptr<skin::LuaSkinRuntime>
requireRuntime(skin::LuaRuntimeCreateResult result, std::string_view message) {
  expect(result.runtime != nullptr, message);
  expect(!result.failure.has_value(), "successful creation has no diagnostic");
  return std::move(result.runtime);
}

void testStrictTwoPhaseStateMachineUsesOneState() {
  using namespace skin;
  auto runtime = requireRuntime(
      makeRuntime(LuaRuntimePurpose::Gameplay, "two_phase.luaskin"),
      "gameplay runtime is created");
  if (!runtime) {
    return;
  }

  const auto prematureConfigured = runtime->loadConfigured({});
  expect(!prematureConfigured.value.has_value(),
         "configured phase cannot run before header phase");
  expect(prematureConfigured.failure.has_value(),
         "out-of-order configured phase is diagnosed");
  expect(runtime->phase() == LuaRuntimePhase::Created,
         "failed transition preserves Created phase");

  auto header = runtime->loadHeader();
  expect(header.value.has_value(), "nil-skin_config header succeeds");
  expect(runtime->phase() == LuaRuntimePhase::HeaderLoaded,
         "header success advances exactly one phase");
  auto configured = runtime->loadConfigured({});
  expect(configured.value.has_value(), "configured execution succeeds");
  expect(runtime->phase() == LuaRuntimePhase::Configured,
         "configured success advances exactly one phase");

  const auto render = runtime->enterRenderPhase();
  expect(render.ok, "configured gameplay runtime enters render phase");
  expect(runtime->phase() == LuaRuntimePhase::Render,
         "render transition is terminal for loading");
  expect(!runtime->loadConfigured({}).value.has_value(),
         "configured execution cannot repeat after render transition");
}

void testFreshPurposesDoNotShareLuaState() {
  using namespace skin;
  for (const auto purpose : {LuaRuntimePurpose::Catalog,
                             LuaRuntimePurpose::Validation,
                             LuaRuntimePurpose::Gameplay}) {
    auto runtime = requireRuntime(makeRuntime(purpose, "fresh_state.luaskin"),
                                  "purpose state is created");
    if (!runtime) {
      continue;
    }
    expect(runtime->loadHeader().value.has_value(),
           "fresh state observes no prior package/global mutation");
  }
}

void testForbiddenCapabilitiesAreAbsentBeforeUserCode() {
  using namespace skin;
  auto runtime = requireRuntime(
      makeRuntime(LuaRuntimePurpose::Validation,
                  "forbidden_capabilities.luaskin"),
      "security probe runtime is created");
  if (!runtime) {
    return;
  }
  expect(runtime->loadHeader().value.has_value(),
         "ffi, jit, debug, os, network, native loaders, and reflective legacy "
         "surface are unavailable");
}

void testLoadQuotasInterruptAllocatorAndPhaseLoops() {
  using namespace skin;
  for (const auto fixture : {"allocator_exhaustion.luaskin",
                             "infinite_header.luaskin"}) {
    auto runtime = requireRuntime(
        makeRuntime(LuaRuntimePurpose::Catalog, fixture),
        "catalog quota probe runtime is created");
    if (!runtime) {
      continue;
    }
    const auto result = runtime->loadHeader();
    expect(!result.value.has_value(), "catalog quota probe is interrupted");
    expect(result.failure.has_value(), "catalog quota failure is diagnosed");
  }

  auto configuredRuntime = requireRuntime(
      makeRuntime(LuaRuntimePurpose::Validation,
                  "infinite_configured.luaskin"),
      "configured-loop probe runtime is created");
  if (configuredRuntime) {
    expect(configuredRuntime->loadHeader().value.has_value(),
           "configured-loop probe header succeeds");
    const auto result = configuredRuntime->loadConfigured({});
    expect(!result.value.has_value(), "configured loop is interrupted");
    expect(result.failure.has_value(),
           "configured instruction/deadline failure is diagnosed");
  }
}

void testCoroutineLoopsShareHookedCallbackAndFrameBudgets() {
  using namespace skin;
  for (const auto callbackName : {"created_loop", "wrapped_loop"}) {
    auto runtime = requireRuntime(
        makeRuntime(LuaRuntimePurpose::Gameplay, "coroutine_loops.luaskin"),
        "coroutine probe runtime is created");
    if (!runtime) {
      continue;
    }
    auto header = runtime->loadHeader();
    expect(header.value.has_value(), "coroutine probe header succeeds");
    if (!header.value) {
      continue;
    }
    const auto callback = callbackNamed(*header.value, callbackName);
    expect(runtime->loadConfigured({}).value.has_value(),
           "coroutine probe configured phase succeeds");
    expect(runtime->enterRenderPhase().ok, "coroutine probe enters render");
    expect(runtime->beginFrame(1).ok, "first callback frame begins");
    const auto result = runtime->invoke(callback, std::span<const LuaScalar>{});
    expect(!result.value.has_value(), "child coroutine loop is interrupted");
    expect(result.failure.has_value(),
           "child coroutine consumes the shared hooked budget");
  }
}

void testFrameTotalsResetOnlyForANewVisualStateSequence() {
  using namespace skin;
  auto runtime = requireRuntime(
      makeRuntime(LuaRuntimePurpose::Gameplay, "frame_budget.luaskin"),
      "frame budget probe runtime is created");
  if (!runtime) {
    return;
  }
  auto header = runtime->loadHeader();
  expect(header.value.has_value(), "frame budget probe header succeeds");
  if (!header.value) {
    return;
  }
  const auto callback = callbackNamed(*header.value, "bounded_work");
  expect(runtime->loadConfigured({}).value.has_value(),
         "frame budget probe configured phase succeeds");
  expect(runtime->enterRenderPhase().ok, "frame budget probe enters render");
  expect(runtime->beginFrame(7).ok, "first visual-state frame begins");

  bool exhausted = false;
  for (int invocation = 0; invocation < 16; ++invocation) {
    const auto result = runtime->invoke(callback, std::span<const LuaScalar>{});
    if (result.failure) {
      exhausted = true;
      break;
    }
  }
  expect(exhausted, "callback totals exhaust the shared frame budget");
  expect(!runtime->beginFrame(7).ok,
         "repeating a visual-state sequence cannot reset exhausted totals");
  expect(runtime->beginFrame(8).ok,
         "a new visual-state sequence resets callback totals once");
  expect(!runtime->invoke(callback, std::span<const LuaScalar>{}).failure,
         "bounded callback can run after the next frame reset");
}

void testRenderPhaseRejectsCapturedFilesystemAuthority() {
  using namespace skin;
  auto runtime = requireRuntime(
      makeRuntime(LuaRuntimePurpose::Gameplay,
                  "captured_file_operation.luaskin"),
      "captured filesystem probe runtime is created");
  if (!runtime) {
    return;
  }
  auto header = runtime->loadHeader();
  expect(header.value.has_value(), "captured filesystem probe header succeeds");
  if (!header.value) {
    return;
  }
  const auto callback = callbackNamed(*header.value, "captured_read");
  expect(runtime->loadConfigured({}).value.has_value(),
         "captured filesystem probe configured phase succeeds");
  expect(runtime->enterRenderPhase().ok, "probe enters render phase");
  expect(runtime->beginFrame(1).ok, "render probe frame begins");
  const auto result = runtime->invoke(callback, std::span<const LuaScalar>{});
  expect(!result.value.has_value(), "captured load-time authority is revoked");
  expect(result.failure.has_value(), "render-phase filesystem denial is diagnosed");
  if (result.failure) {
    expect(result.failure->code == "skin_file_render_phase_denied",
           "render-phase denial uses the frozen sandbox diagnostic");
  }
}

#else

void testTask8FilesystemDependencyIsExplicit() {
  expect(false,
         "RED: Task 8 must provide skin/beatoraja/LuaSkinFileSystem.h before "
         "Lua runtime construction and sandbox behavior can be linked");
}

#endif

} // namespace

int main() {
  testPurposeSpecificBudgetsAreFixed();
  testRuntimeContractsUseTheFrozenAuditAndSandboxAuthority();
#if __has_include("skin/beatoraja/LuaSkinFileSystem.h")
  testStrictTwoPhaseStateMachineUsesOneState();
  testFreshPurposesDoNotShareLuaState();
  testForbiddenCapabilitiesAreAbsentBeforeUserCode();
  testLoadQuotasInterruptAllocatorAndPhaseLoops();
  testCoroutineLoopsShareHookedCallbackAndFrameBudgets();
  testFrameTotalsResetOnlyForANewVisualStateSequence();
  testRenderPhaseRejectsCapturedFilesystemAuthority();
#else
  testTask8FilesystemDependencyIsExplicit();
#endif
  return failures == 0 ? 0 : 1;
}
