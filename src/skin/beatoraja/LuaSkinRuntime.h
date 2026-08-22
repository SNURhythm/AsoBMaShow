#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "LuaSkinHttpClient.h"
#include "SkinCompatibilityDiagnostics.h"
#include "../SkinSafetyPolicy.h"
#include "../package/SkinPackageTypes.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

struct lua_State;

namespace skin {

class LuaSkinFileSystem;
class LuaSkinRuntime;
class LuaSkinTableDecoder;
class ISkinFrameState;
struct LuaSkinEventExecutor;
struct LuaSkinEventExecutionResult;

enum class LuaRuntimePurpose : std::uint8_t {
  Catalog,
  Validation,
  Gameplay,
};

enum class LuaRuntimePhase : std::uint8_t {
  Created,
  HeaderLoaded,
  Configured,
  Render,
};

struct LuaLoadBudget {
  std::size_t maxAllocatorBytes = 0;
  std::uint64_t maxInstructions = 0;
  std::chrono::milliseconds maxWallTime{0};
};

struct LuaCallbackBudget {
  std::uint64_t maxInstructions = 0;
  std::chrono::milliseconds maxWallTime{0};
};

struct LuaRuntimePolicy {
  inline static constexpr std::size_t maxBindingPathDepth = 64;
  inline static constexpr std::size_t maxModuleSearchTemplates = 1024;
  inline static constexpr LuaLoadBudget catalogLoad{
      .maxAllocatorBytes = 32ULL * 1024ULL * 1024ULL,
      .maxInstructions = 2'000'000,
      .maxWallTime = std::chrono::milliseconds{2'000},
  };
  inline static constexpr LuaLoadBudget validationAndGameplayLoad{
      .maxAllocatorBytes = 128ULL * 1024ULL * 1024ULL,
      .maxInstructions = 20'000'000,
      .maxWallTime = std::chrono::milliseconds{10'000},
  };
  inline static constexpr LuaCallbackBudget gameplayCallback{
      .maxInstructions = 250'000,
      .maxWallTime = std::chrono::milliseconds{4},
  };
  inline static constexpr LuaCallbackBudget gameplayFrame{
      .maxInstructions = 1'000'000,
      .maxWallTime = std::chrono::milliseconds{6},
  };

  inline static constexpr LuaLoadBudget unrestrictedLoad{
      .maxAllocatorBytes = std::numeric_limits<std::size_t>::max(),
      .maxInstructions = std::numeric_limits<std::uint64_t>::max(),
      // Zero is the internal unlimited-deadline sentinel.
      .maxWallTime = std::chrono::milliseconds{0},
  };

  [[nodiscard]] static constexpr LuaLoadBudget
  loadBudget(LuaRuntimePurpose purpose,
             SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{}) noexcept {
    if (!safetyPolicy.enforces(SkinSafetyGuard::LuaResourceBudget)) {
      return unrestrictedLoad;
    }
    return purpose == LuaRuntimePurpose::Catalog ? catalogLoad
                                                 : validationAndGameplayLoad;
  }
};

using LuaScalar =
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;

struct LuaCallbackId {
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;
  explicit operator bool() const noexcept {
    return slot != 0 && generation != 0;
  }
  auto operator<=>(const LuaCallbackId &) const = default;
};

struct LuaCallbackLookupResult {
  std::optional<LuaCallbackId> callback;
  std::optional<SkinDiagnostic> failure;
};

struct LuaValuePathElement {
  std::variant<std::string, std::uint32_t> key;

  static LuaValuePathElement field(std::string_view name) {
    return {.key = std::string(name)};
  }
  static LuaValuePathElement index(std::uint32_t value) {
    return {.key = value};
  }
};

using LuaValuePath = std::vector<LuaValuePathElement>;
using LuaBindingSourceValue = std::variant<int, std::string, LuaCallbackId>;

struct LuaBindingSourceLookupLimits {
  std::size_t maxStringBytes = std::numeric_limits<std::size_t>::max();
  std::size_t remainingWorkBytes = std::numeric_limits<std::size_t>::max();
  bool numericFactoryAvailable = true;
};

struct LuaBindingSourceLookupResult {
  std::optional<LuaBindingSourceValue> source;
  std::optional<SkinDiagnostic> failure;
  std::size_t workBytes = 0;
};

enum class LuaCallbackScriptKind : std::uint8_t {
  ReturnExpression,
  Timer,
  Statement,
};

struct LuaCallbackCompileResult {
  std::optional<LuaCallbackId> callback;
  std::optional<SkinDiagnostic> failure;
};

class LuaCallbackLivenessView final {
public:
  LuaCallbackLivenessView() noexcept = default;

  [[nodiscard]] bool contains(LuaCallbackId callback) const noexcept {
    return generation_ != 0 && callback.generation == generation_ &&
           callback.slot != 0 && callback.slot <= retainedCount_;
  }

  [[nodiscard]] bool
  containsAll(std::span<const LuaCallbackId> callbacks) const noexcept {
    for (const auto callback : callbacks) {
      if (!contains(callback)) {
        return false;
      }
    }
    return true;
  }

private:
  std::uint32_t generation_ = 0;
  std::uint32_t retainedCount_ = 0;

  LuaCallbackLivenessView(std::uint32_t generation,
                          std::uint32_t retainedCount) noexcept
      : generation_(generation), retainedCount_(retainedCount) {}
  friend class LuaSkinRuntime;
};

class LuaValueHandle {
public:
  LuaValueHandle(LuaValueHandle &&) noexcept;
  LuaValueHandle &operator=(LuaValueHandle &&) noexcept;
  LuaValueHandle(const LuaValueHandle &) = delete;
  LuaValueHandle &operator=(const LuaValueHandle &) = delete;
  ~LuaValueHandle();

  [[nodiscard]] std::optional<LuaCallbackId>
  callbackNamed(std::string_view name) const;
  [[nodiscard]] LuaCallbackLookupResult
  lookupCallbackNamed(std::string_view name) const;
  [[nodiscard]] LuaBindingSourceLookupResult
  lookupBindingSource(const LuaValuePath &path,
                      LuaBindingSourceLookupLimits = {}) const;

private:
  using ProtectedValueOperation = void (*)(lua_State *, int, void *) noexcept;
  [[nodiscard]] std::optional<SkinDiagnostic>
  withValueProtected(void *, ProtectedValueOperation) const;

  struct Impl;
  explicit LuaValueHandle(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class LuaSkinRuntime;
  friend class LuaSkinTableDecoder;
};

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
  LuaRuntimePurpose purpose = LuaRuntimePurpose::Catalog;
  std::unique_ptr<LuaSkinFileSystem> fileSystem;
  SkinSafetyPolicy safetyPolicy{};
  std::unique_ptr<LuaSkinHttpTransport> httpTransport;
};

class LuaSkinRuntime final {
public:
  static LuaRuntimeCreateResult create(LuaSkinRuntimeOptions);

  LuaSkinRuntime(const LuaSkinRuntime &) = delete;
  LuaSkinRuntime &operator=(const LuaSkinRuntime &) = delete;
  ~LuaSkinRuntime();

  LuaValueResult loadHeader();
  LuaValueResult
  loadConfigured(const BeatorajaSkinConfiguration &configuration);
  LuaOperationResult enterRenderPhase();
  void setFrameState(ISkinFrameState *) noexcept;
  void setEventExecutor(LuaSkinEventExecutor) noexcept;
  LuaOperationResult beginFrame(std::uint64_t visualStateSequence);
  LuaCallbackResult invoke(LuaCallbackId, std::span<const LuaScalar> arguments);
  LuaCallbackCompileResult compileCallbackScript(std::string_view,
                                                 LuaCallbackScriptKind);
  [[nodiscard]] LuaCallbackLivenessView callbackLiveness() const noexcept;
  [[nodiscard]] LuaRuntimePhase phase() const noexcept;
  [[nodiscard]] std::span<const SkinCompatibilityDiagnostic>
  compatibilityDiagnostics() const noexcept;

private:
  struct Impl;
  explicit LuaSkinRuntime(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
// Test-only deterministic quota failures at otherwise hard-to-reach Lua API
// allocation boundaries.  These are unavailable from production builds.
enum class LuaRuntimeTestAllocationPoint : std::uint8_t {
  ValueReference,
  CallbackName,
  CallbackReference,
  InvokeArgument,
};

class LuaRuntimeTestHooks final {
public:
  static void failNextAllocationAt(LuaRuntimeTestAllocationPoint) noexcept;
};
#endif

} // namespace skin
