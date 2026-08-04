#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "SkinCompatibilityDiagnostics.h"
#include "../package/SkinPackageTypes.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace skin {

class LuaSkinFileSystem;
class LuaSkinRuntime;

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

  [[nodiscard]] static constexpr LuaLoadBudget
  loadBudget(LuaRuntimePurpose purpose) noexcept {
    return purpose == LuaRuntimePurpose::Catalog ? catalogLoad
                                                 : validationAndGameplayLoad;
  }
};

using LuaScalar =
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;

struct LuaCallbackId {
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;
  auto operator<=>(const LuaCallbackId &) const = default;
};

struct LuaCallbackLookupResult {
  std::optional<LuaCallbackId> callback;
  std::optional<SkinDiagnostic> failure;
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

private:
  struct Impl;
  explicit LuaValueHandle(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class LuaSkinRuntime;
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
  LuaOperationResult beginFrame(std::uint64_t visualStateSequence);
  LuaCallbackResult invoke(LuaCallbackId, std::span<const LuaScalar> arguments);
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
