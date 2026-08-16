#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "SkinCompatibilityDiagnostics.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>

struct lua_State;

namespace skin {

class LuaSkinFileSystem;
class ISkinFrameState;
struct LuaSkinHostModulesImpl;

using LuaCoroutineCreatedCallback = void (*)(void *, lua_State *);

struct LuaSkinEventExecutionResult {
  std::optional<SkinDiagnostic> failure;

  [[nodiscard]] bool ok() const noexcept { return !failure.has_value(); }
};

using LuaSkinEventExecutionCallback = LuaSkinEventExecutionResult (*)(
    void *, int, std::span<const int>) noexcept;

struct LuaSkinEventExecutor {
  void *context = nullptr;
  LuaSkinEventExecutionCallback execute = nullptr;

  explicit operator bool() const noexcept {
    return context != nullptr && execute != nullptr;
  }
};

struct LuaSkinHostModulesOptions {
  LuaSkinFileSystem *fileSystem = nullptr;
  // Lua source first enters host storage before luaL_loadbuffer can charge it
  // to the Lua allocator. The runtime supplies its existing load budget here
  // to prevent an arbitrarily large package file from bypassing that budget.
  std::size_t maximumSourceBytes = std::numeric_limits<std::size_t>::max();
  bool allowProcessGlobalOperations = false;
  void *coroutineContext = nullptr;
  LuaCoroutineCreatedCallback coroutineCreated = nullptr;
};

struct LuaSkinHostModulesCreateResult {
  std::unique_ptr<class LuaSkinHostModules> modules;
  std::optional<SkinDiagnostic> failure;
};

struct LuaFileHandleInvalidationResult {
  bool hadDirtyWrite = false;
};

class LuaSkinHostModules final {
public:
  static LuaSkinHostModulesCreateResult create(lua_State *,
                                               LuaSkinHostModulesOptions);

  LuaSkinHostModules(const LuaSkinHostModules &) = delete;
  LuaSkinHostModules &operator=(const LuaSkinHostModules &) = delete;
  ~LuaSkinHostModules();

  std::optional<SkinDiagnostic>
  installConfiguration(const BeatorajaSkinConfiguration &);
  std::optional<SkinDiagnostic> enableStateAccessors();
  void setFrameState(ISkinFrameState *) noexcept;
  void setEventExecutor(LuaSkinEventExecutor) noexcept;
  LuaFileHandleInvalidationResult invalidateFileHandles() noexcept;
  std::span<const SkinCompatibilityDiagnostic> diagnostics() const noexcept;

private:
  explicit LuaSkinHostModules(std::unique_ptr<LuaSkinHostModulesImpl>) noexcept;
  std::unique_ptr<LuaSkinHostModulesImpl> impl_;
};

} // namespace skin
