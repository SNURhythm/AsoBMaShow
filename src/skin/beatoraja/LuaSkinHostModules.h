#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "SkinCompatibilityDiagnostics.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

struct lua_State;

namespace skin {

class LuaSkinFileSystem;
struct LuaSkinHostModulesImpl;

struct LuaSkinHostPolicy {
  static constexpr std::uint64_t maxTextChunkBytes = 8ULL * 1024 * 1024;
  static constexpr std::uint64_t maxDataReadBytes = 16ULL * 1024 * 1024;
  static constexpr std::size_t maxOpenHandles = 64;
  static constexpr std::uint64_t maxAggregateHandleBytes = 64ULL * 1024 * 1024;
  static constexpr std::size_t maxDirectoryEntries = 20'000;
};

using LuaCoroutineCreatedCallback = void (*)(void *, lua_State *);

struct LuaSkinHostModulesOptions {
  LuaSkinFileSystem *fileSystem = nullptr;
  bool allowOverlayWrites = false;
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
  LuaFileHandleInvalidationResult invalidateFileHandles() noexcept;
  std::span<const SkinCompatibilityDiagnostic> diagnostics() const noexcept;

private:
  explicit LuaSkinHostModules(std::unique_ptr<LuaSkinHostModulesImpl>) noexcept;
  std::unique_ptr<LuaSkinHostModulesImpl> impl_;
};

} // namespace skin
