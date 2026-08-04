#include "LuaSkinRuntime.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "LuaSkinFileSystem.h"
#include "LuaSkinHostModules.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <luajit.h>
}

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skin {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kHookInstructionInterval = 1'000;
constexpr std::size_t kMaximumReturnedTableDepth = 64;
constexpr std::uint64_t kMaximumReturnedTableEntries = 200'000;
constexpr std::size_t kMaximumReturnedStringBytes = 16ULL * 1024 * 1024;
constexpr std::size_t kMaximumCallbacks = 20'000;
constexpr std::string_view kHostErrorPrefix = "@ASOBMSKIN:";
char kRuntimeRegistryKey;

SkinDiagnostic makeDiagnostic(std::string code, std::string message,
                              std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

const char *runtimeFileFailureCode(SkinFileError error) noexcept {
  switch (error) {
  case SkinFileError::RenderPhase:
    return "skin_file_render_phase_denied";
  case SkinFileError::BinaryChunk:
    return "skin_lua_binary_chunk_denied";
  case SkinFileError::LimitExceeded:
  case SkinFileError::QuotaExceeded:
    return "skin_lua_host_limit_exceeded";
  case SkinFileError::InvalidPath:
  case SkinFileError::EscapesPackage:
  case SkinFileError::WrongUse:
  case SkinFileError::Missing:
  case SkinFileError::NonRegular:
  case SkinFileError::IoError:
    return "skin_lua_file_operation_failed";
  }
  return "skin_lua_file_operation_failed";
}

struct LuaRuntimeShared {
  lua_State *state = nullptr;
  std::size_t allocatedBytes = 0;
  std::size_t maximumAllocatorBytes = 0;
  std::uint32_t generation = 0;
  std::vector<int> callbackReferences;

  bool executionActive = false;
  bool callbackActive = false;
  bool budgetViolated = false;
  const char *budgetViolationCode = nullptr;
  std::uint64_t executionInstructions = 0;
  std::uint64_t executionInstructionLimit = 0;
  Clock::time_point executionDeadline{};

  bool frameBegun = false;
  bool frameExhausted = false;
  std::uint64_t frameSequence = 0;
  std::uint64_t frameInstructions = 0;
  std::chrono::nanoseconds frameWallUsed{0};
  Clock::time_point callbackStarted{};
};

void *quotaAllocator(void *userData, void *pointer, std::size_t oldSize,
                     std::size_t newSize) noexcept {
  auto *shared = static_cast<LuaRuntimeShared *>(userData);
  // Lua 5.1/LuaJIT passes an object type tag in osize for a fresh allocation;
  // it is an allocation size only when pointer is non-null.
  const std::size_t accountedOldSize = pointer == nullptr ? 0 : oldSize;
  if (newSize == 0) {
    if (pointer != nullptr) {
      shared->allocatedBytes = accountedOldSize <= shared->allocatedBytes
                                   ? shared->allocatedBytes - accountedOldSize
                                   : 0;
      std::free(pointer);
    }
    return nullptr;
  }
  if (newSize > accountedOldSize) {
    const std::size_t increase = newSize - accountedOldSize;
    if (increase >
        shared->maximumAllocatorBytes -
            std::min(shared->allocatedBytes, shared->maximumAllocatorBytes)) {
      return nullptr;
    }
  }
  void *replacement = std::realloc(pointer, newSize);
  if (replacement == nullptr) {
    return nullptr;
  }
  if (newSize >= accountedOldSize) {
    shared->allocatedBytes += newSize - accountedOldSize;
  } else {
    shared->allocatedBytes -=
        std::min(shared->allocatedBytes, accountedOldSize - newSize);
  }
  return replacement;
}

LuaRuntimeShared *runtimeShared(lua_State *state) noexcept {
  lua_pushlightuserdata(state, &kRuntimeRegistryKey);
  lua_rawget(state, LUA_REGISTRYINDEX);
  auto *shared = static_cast<LuaRuntimeShared *>(lua_touserdata(state, -1));
  lua_pop(state, 1);
  return shared;
}

void budgetHook(lua_State *state, lua_Debug *) {
  LuaRuntimeShared *shared = runtimeShared(state);
  if (shared == nullptr || !shared->executionActive) {
    return;
  }
  shared->executionInstructions += kHookInstructionInterval;
  if (shared->callbackActive) {
    shared->frameInstructions += kHookInstructionInterval;
  }
  const Clock::time_point now = Clock::now();
  const bool instructionExceeded =
      shared->executionInstructions > shared->executionInstructionLimit;
  const bool deadlineExceeded = now > shared->executionDeadline;
  const bool frameInstructionExceeded =
      shared->callbackActive &&
      shared->frameInstructions >
          LuaRuntimePolicy::gameplayFrame.maxInstructions;
  const bool frameDeadlineExceeded =
      shared->callbackActive &&
      shared->frameWallUsed + (now - shared->callbackStarted) >
          LuaRuntimePolicy::gameplayFrame.maxWallTime;
  if (!instructionExceeded && !deadlineExceeded && !frameInstructionExceeded &&
      !frameDeadlineExceeded) {
    return;
  }
  shared->budgetViolated = true;
  shared->frameExhausted = shared->frameExhausted || frameInstructionExceeded ||
                           frameDeadlineExceeded;
  shared->budgetViolationCode = deadlineExceeded || frameDeadlineExceeded
                                    ? "skin_lua_wall_time_limit_exceeded"
                                    : "skin_lua_instruction_limit_exceeded";
  luaL_error(state, "%s%s:Lua execution budget exceeded",
             kHostErrorPrefix.data(), shared->budgetViolationCode);
}

void installHook(void *context, lua_State *state) {
  auto *shared = static_cast<LuaRuntimeShared *>(context);
  if (state == nullptr || shared == nullptr) {
    return;
  }
  lua_sethook(state, budgetHook, LUA_MASKCOUNT,
              static_cast<int>(kHookInstructionInterval));
}

int absoluteIndex(lua_State *state, int index) {
  return index > 0 || index <= LUA_REGISTRYINDEX
             ? index
             : lua_gettop(state) + index + 1;
}

struct ReturnValidation {
  LuaRuntimeShared *shared = nullptr;
  std::uint64_t entries = 0;
  std::unordered_set<const void *> visitedTables;
  std::optional<SkinDiagnostic> failure;
};

bool validateReturnedValue(lua_State *state, int index, std::size_t depth,
                           ReturnValidation &validation) {
  if (Clock::now() > validation.shared->executionDeadline) {
    validation.failure =
        makeDiagnostic("skin_lua_wall_time_limit_exceeded",
                       "Lua return-value validation exceeded its deadline");
    return false;
  }
  const int type = lua_type(state, index);
  if (type == LUA_TSTRING) {
    std::size_t size = 0;
    lua_tolstring(state, index, &size);
    if (size > kMaximumReturnedStringBytes) {
      validation.failure =
          makeDiagnostic("skin_lua_return_limit_exceeded",
                         "Lua returned string exceeds the fixed limit");
      return false;
    }
    return true;
  }
  if (type != LUA_TTABLE) {
    return true;
  }
  if (depth > kMaximumReturnedTableDepth) {
    validation.failure =
        makeDiagnostic("skin_lua_return_limit_exceeded",
                       "Lua returned table exceeds the fixed depth limit");
    return false;
  }
  const void *identity = lua_topointer(state, index);
  if (!validation.visitedTables.insert(identity).second) {
    return true;
  }
  if (!lua_checkstack(state, 4)) {
    validation.failure =
        makeDiagnostic("skin_lua_stack_limit_exceeded",
                       "Lua returned table exceeds the fixed stack limit");
    return false;
  }
  const int tableIndex = absoluteIndex(state, index);
  const int savedTop = lua_gettop(state);
  lua_pushnil(state);
  while (lua_next(state, tableIndex) != 0) {
    ++validation.entries;
    if (validation.entries > kMaximumReturnedTableEntries ||
        !validateReturnedValue(state, -2, depth + 1, validation) ||
        !validateReturnedValue(state, -1, depth + 1, validation)) {
      lua_settop(state, savedTop);
      if (!validation.failure) {
        validation.failure =
            makeDiagnostic("skin_lua_return_limit_exceeded",
                           "Lua returned table exceeds the fixed entry limit");
      }
      return false;
    }
    lua_pop(state, 1);
  }
  return true;
}

SkinDiagnostic luaFailure(lua_State *state, int status,
                          const LuaRuntimeShared &shared) {
  if (shared.budgetViolated && shared.budgetViolationCode != nullptr) {
    return makeDiagnostic(shared.budgetViolationCode,
                          "Lua execution budget exceeded");
  }
  const char *message = lua_tostring(state, -1);
  const std::string text =
      message != nullptr ? message : "Lua execution failed without a message";
  if (text.starts_with(kHostErrorPrefix)) {
    const std::size_t codeStart = kHostErrorPrefix.size();
    const std::size_t separator = text.find(':', codeStart);
    if (separator != std::string::npos) {
      return makeDiagnostic(text.substr(codeStart, separator - codeStart),
                            text.substr(separator + 1));
    }
  }
  if (status == LUA_ERRMEM) {
    return makeDiagnostic("skin_lua_allocator_limit_exceeded",
                          "Lua allocator quota was exhausted");
  }
  return makeDiagnostic("skin_lua_execution_failed", text);
}

void beginLoadBudget(LuaRuntimeShared &shared, LuaLoadBudget budget) {
  shared.executionActive = true;
  shared.callbackActive = false;
  shared.budgetViolated = false;
  shared.budgetViolationCode = nullptr;
  shared.executionInstructions = 0;
  shared.executionInstructionLimit = budget.maxInstructions;
  shared.executionDeadline = Clock::now() + budget.maxWallTime;
}

void beginCallbackBudget(LuaRuntimeShared &shared) {
  shared.executionActive = true;
  shared.callbackActive = true;
  shared.budgetViolated = false;
  shared.budgetViolationCode = nullptr;
  shared.executionInstructions = 0;
  shared.executionInstructionLimit =
      LuaRuntimePolicy::gameplayCallback.maxInstructions;
  shared.callbackStarted = Clock::now();
  shared.executionDeadline =
      shared.callbackStarted + LuaRuntimePolicy::gameplayCallback.maxWallTime;
}

void endExecutionBudget(LuaRuntimeShared &shared) {
  if (shared.callbackActive) {
    shared.frameWallUsed += Clock::now() - shared.callbackStarted;
    if (shared.frameWallUsed > LuaRuntimePolicy::gameplayFrame.maxWallTime) {
      shared.frameExhausted = true;
    }
  }
  shared.executionActive = false;
  shared.callbackActive = false;
}

void pushScalar(lua_State *state, const LuaScalar &scalar) {
  std::visit(
      [state](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
          lua_pushnil(state);
        } else if constexpr (std::is_same_v<T, bool>) {
          lua_pushboolean(state, value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          lua_pushnumber(state, static_cast<lua_Number>(value));
        } else if constexpr (std::is_same_v<T, double>) {
          lua_pushnumber(state, static_cast<lua_Number>(value));
        } else {
          lua_pushlstring(state, value.data(), value.size());
        }
      },
      scalar);
}

std::optional<LuaScalar> readScalar(lua_State *state, int index) {
  switch (lua_type(state, index)) {
  case LUA_TNIL:
    return LuaScalar{nullptr};
  case LUA_TBOOLEAN:
    return LuaScalar{lua_toboolean(state, index) != 0};
  case LUA_TNUMBER: {
    const double value = static_cast<double>(lua_tonumber(state, index));
    if (std::isfinite(value) && std::trunc(value) == value &&
        value >=
            static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        value <=
            static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
      return LuaScalar{static_cast<std::int64_t>(value)};
    }
    return LuaScalar{value};
  }
  case LUA_TSTRING: {
    std::size_t size = 0;
    const char *text = lua_tolstring(state, index, &size);
    if (size > kMaximumReturnedStringBytes) {
      return std::nullopt;
    }
    return LuaScalar{std::string(text, size)};
  }
  default:
    return std::nullopt;
  }
}

} // namespace

struct LuaValueHandle::Impl {
  std::shared_ptr<LuaRuntimeShared> shared;
  int reference = LUA_NOREF;

  ~Impl() {
    if (shared && shared->state != nullptr && reference >= 0) {
      luaL_unref(shared->state, LUA_REGISTRYINDEX, reference);
    }
  }
};

struct LuaSkinRuntime::Impl {
  LuaRuntimePurpose purpose = LuaRuntimePurpose::Catalog;
  LuaRuntimePhase phase = LuaRuntimePhase::Created;
  std::shared_ptr<LuaRuntimeShared> shared;
  lua_State *state = nullptr;
  std::unique_ptr<LuaSkinFileSystem> fileSystem;
  std::unique_ptr<LuaSkinHostModules> hostModules;

  ~Impl() {
    if (state != nullptr) {
      shared->state = nullptr;
      lua_close(state);
      state = nullptr;
    }
  }

  LuaValueResult runEntry() {
    lua_settop(state, 0);
    beginLoadBudget(*shared, LuaRuntimePolicy::loadBudget(purpose));

    int loadStatus = LUA_ERRFILE;
    std::optional<SkinDiagnostic> readFailure;
    {
      auto entry = fileSystem->readEntry(LuaSkinHostPolicy::maxTextChunkBytes);
      if (entry.failure) {
        readFailure =
            makeDiagnostic(runtimeFileFailureCode(entry.failure->code),
                           entry.failure->message, entry.failure->virtualPath);
      } else {
        loadStatus = luaL_loadbuffer(
            state, reinterpret_cast<const char *>(entry.bytes.data()),
            entry.bytes.size(), "@skin-entry");
      }
    }
    if (readFailure) {
      endExecutionBudget(*shared);
      return {.failure = std::move(readFailure)};
    }
    if (Clock::now() > shared->executionDeadline) {
      endExecutionBudget(*shared);
      return {.failure = makeDiagnostic(
                  "skin_lua_wall_time_limit_exceeded",
                  "Lua entry compilation exceeded its deadline")};
    }
    if (loadStatus != 0) {
      auto failure = luaFailure(state, loadStatus, *shared);
      lua_settop(state, 0);
      endExecutionBudget(*shared);
      return {.failure = std::move(failure)};
    }

    const int status = lua_pcall(state, 0, LUA_MULTRET, 0);
    if (status != 0 || shared->budgetViolated) {
      auto failure = luaFailure(state, status, *shared);
      lua_settop(state, 0);
      endExecutionBudget(*shared);
      return {.failure = std::move(failure)};
    }
    if (lua_gettop(state) == 0) {
      lua_pushnil(state);
    } else if (lua_gettop(state) > 1) {
      lua_settop(state, 1);
    }

    ReturnValidation validation{.shared = shared.get()};
    bool returnValueValid = false;
    try {
      returnValueValid = validateReturnedValue(state, 1, 1, validation);
    } catch (...) {
      validation.failure = makeDiagnostic(
          "skin_lua_return_limit_exceeded",
          "Lua return-value validation could not allocate bounded state");
    }
    if (!returnValueValid) {
      lua_settop(state, 0);
      endExecutionBudget(*shared);
      return {.failure = std::move(validation.failure)};
    }
    int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    endExecutionBudget(*shared);
    std::unique_ptr<LuaValueHandle::Impl> handle;
    try {
      handle = std::unique_ptr<LuaValueHandle::Impl>(
          new LuaValueHandle::Impl{.shared = shared, .reference = reference});
    } catch (...) {
      if (reference >= 0) {
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
      }
      return {.failure = makeDiagnostic("skin_lua_allocator_limit_exceeded",
                                        "Lua value handle allocation failed")};
    }
    return {.value = LuaValueHandle(std::move(handle))};
  }
};

LuaValueHandle::LuaValueHandle(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LuaValueHandle::LuaValueHandle(LuaValueHandle &&) noexcept = default;
LuaValueHandle &LuaValueHandle::operator=(LuaValueHandle &&) noexcept = default;

LuaValueHandle::~LuaValueHandle() = default;

std::optional<LuaCallbackId>
LuaValueHandle::callbackNamed(std::string_view name) const {
  if (!impl_ || !impl_->shared || impl_->shared->state == nullptr ||
      impl_->reference == LUA_NOREF || name.empty() ||
      impl_->shared->callbackReferences.size() >= kMaximumCallbacks) {
    return std::nullopt;
  }
  lua_State *state = impl_->shared->state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, impl_->reference);
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return std::nullopt;
  }
  lua_pushlstring(state, name.data(), name.size());
  lua_rawget(state, -2);
  lua_remove(state, -2);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    return std::nullopt;
  }
  const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
  try {
    impl_->shared->callbackReferences.push_back(reference);
  } catch (...) {
    luaL_unref(state, LUA_REGISTRYINDEX, reference);
    return std::nullopt;
  }
  return LuaCallbackId{.slot = static_cast<std::uint32_t>(
                           impl_->shared->callbackReferences.size()),
                       .generation = impl_->shared->generation};
}

LuaSkinRuntime::LuaSkinRuntime(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LuaSkinRuntime::~LuaSkinRuntime() = default;

LuaRuntimeCreateResult LuaSkinRuntime::create(LuaSkinRuntimeOptions options) {
  if (!options.fileSystem) {
    return {.failure = makeDiagnostic("skin_lua_runtime_create_failed",
                                      "Lua runtime requires a filesystem")};
  }
  std::shared_ptr<LuaRuntimeShared> shared;
  std::unique_ptr<Impl> impl;
  try {
    shared = std::make_shared<LuaRuntimeShared>();
    static std::atomic_uint32_t nextGeneration{0};
    shared->generation = ++nextGeneration;
    if (shared->generation == 0) {
      shared->generation = ++nextGeneration;
    }
    shared->maximumAllocatorBytes =
        LuaRuntimePolicy::loadBudget(options.purpose).maxAllocatorBytes;
    impl = std::make_unique<Impl>();
  } catch (...) {
    return {.failure = makeDiagnostic("skin_lua_runtime_create_failed",
                                      "Lua runtime allocation failed")};
  }

  lua_State *state = lua_newstate(quotaAllocator, shared.get());
  if (state == nullptr) {
    return {.failure = makeDiagnostic("skin_lua_allocator_limit_exceeded",
                                      "Lua state allocation failed")};
  }
  shared->state = state;
  impl->purpose = options.purpose;
  impl->shared = shared;
  impl->state = state;
  impl->fileSystem = std::move(options.fileSystem);

  lua_pushlightuserdata(state, &kRuntimeRegistryKey);
  lua_pushlightuserdata(state, shared.get());
  lua_rawset(state, LUA_REGISTRYINDEX);
  installHook(shared.get(), state);
  if (luaJIT_setmode(state, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF) == 0) {
    return {.failure = makeDiagnostic("skin_lua_jit_disable_failed",
                                      "LuaJIT could not be disabled")};
  }

  auto installed = LuaSkinHostModules::create(
      state,
      {.fileSystem = impl->fileSystem.get(),
       .allowOverlayWrites = options.purpose != LuaRuntimePurpose::Catalog,
       .coroutineContext = shared.get(),
       .coroutineCreated = installHook});
  if (!installed.modules) {
    return {.failure = std::move(installed.failure)};
  }
  impl->hostModules = std::move(installed.modules);
  try {
    return {.runtime = std::unique_ptr<LuaSkinRuntime>(
                new LuaSkinRuntime(std::move(impl)))};
  } catch (...) {
    return {.failure = makeDiagnostic("skin_lua_runtime_create_failed",
                                      "Lua runtime wrapper allocation failed")};
  }
}

LuaValueResult LuaSkinRuntime::loadHeader() {
  if (!impl_ || impl_->phase != LuaRuntimePhase::Created) {
    return {.failure = makeDiagnostic(
                "skin_lua_phase_invalid",
                "Lua header execution is invalid in the current phase")};
  }
  lua_pushnil(impl_->state);
  lua_setglobal(impl_->state, "skin_config");
  auto result = impl_->runEntry();
  if (result.value) {
    impl_->phase = LuaRuntimePhase::HeaderLoaded;
  }
  return result;
}

LuaValueResult LuaSkinRuntime::loadConfigured(
    const BeatorajaSkinConfiguration &configuration) {
  if (!impl_ || impl_->phase != LuaRuntimePhase::HeaderLoaded) {
    return {.failure = makeDiagnostic(
                "skin_lua_phase_invalid",
                "Lua configured execution requires a loaded header")};
  }
  if (auto failure = impl_->hostModules->installConfiguration(configuration)) {
    return {.failure = std::move(failure)};
  }
  auto result = impl_->runEntry();
  if (result.value) {
    impl_->phase = LuaRuntimePhase::Configured;
  }
  return result;
}

LuaOperationResult LuaSkinRuntime::enterRenderPhase() {
  if (!impl_ || impl_->purpose != LuaRuntimePurpose::Gameplay ||
      impl_->phase != LuaRuntimePhase::Configured) {
    return {.failure = makeDiagnostic(
                "skin_lua_phase_invalid",
                "Lua render transition requires configured gameplay")};
  }
  const auto handles = impl_->hostModules->invalidateFileHandles();
  const auto transition = impl_->fileSystem->enterRenderPhase();
  if (!transition.ok) {
    return {.failure = makeDiagnostic(
                "skin_file_render_phase_denied",
                transition.failure ? transition.failure->message
                                   : "Lua filesystem transition failed")};
  }
  impl_->phase = LuaRuntimePhase::Render;
  if (handles.hadDirtyWrite) {
    return {.failure = makeDiagnostic(
                "skin_file_render_phase_denied",
                "dirty Lua file handle was discarded at render transition")};
  }
  return {.ok = true};
}

LuaOperationResult
LuaSkinRuntime::beginFrame(std::uint64_t visualStateSequence) {
  if (!impl_ || impl_->purpose != LuaRuntimePurpose::Gameplay ||
      impl_->phase != LuaRuntimePhase::Render || visualStateSequence == 0 ||
      (impl_->shared->frameBegun &&
       visualStateSequence <= impl_->shared->frameSequence)) {
    return {.failure = makeDiagnostic(
                "skin_lua_frame_invalid",
                "Lua callback frame requires a new nonzero visual sequence")};
  }
  impl_->shared->frameBegun = true;
  impl_->shared->frameExhausted = false;
  impl_->shared->frameSequence = visualStateSequence;
  impl_->shared->frameInstructions = 0;
  impl_->shared->frameWallUsed = std::chrono::nanoseconds{0};
  return {.ok = true};
}

LuaCallbackResult LuaSkinRuntime::invoke(LuaCallbackId callback,
                                         std::span<const LuaScalar> arguments) {
  if (!impl_ || impl_->phase != LuaRuntimePhase::Render ||
      !impl_->shared->frameBegun) {
    return {.failure =
                makeDiagnostic("skin_lua_callback_phase_invalid",
                               "Lua callback requires an active render frame")};
  }
  if (impl_->shared->frameExhausted) {
    return {.failure =
                makeDiagnostic("skin_lua_frame_budget_exceeded",
                               "Lua callback frame budget is exhausted")};
  }
  if (callback.generation != impl_->shared->generation || callback.slot == 0 ||
      callback.slot > impl_->shared->callbackReferences.size()) {
    return {.failure = makeDiagnostic("skin_lua_callback_invalid",
                                      "Lua callback handle is invalid")};
  }

  lua_settop(impl_->state, 0);
  lua_rawgeti(impl_->state, LUA_REGISTRYINDEX,
              impl_->shared->callbackReferences[callback.slot - 1]);
  for (const LuaScalar &argument : arguments) {
    pushScalar(impl_->state, argument);
  }
  beginCallbackBudget(*impl_->shared);
  const int status =
      lua_pcall(impl_->state, static_cast<int>(arguments.size()), 1, 0);
  if (!impl_->shared->budgetViolated &&
      Clock::now() > impl_->shared->executionDeadline) {
    impl_->shared->budgetViolated = true;
    impl_->shared->budgetViolationCode = "skin_lua_wall_time_limit_exceeded";
  }
  endExecutionBudget(*impl_->shared);
  if (!impl_->shared->budgetViolated && impl_->shared->frameExhausted) {
    impl_->shared->budgetViolated = true;
    impl_->shared->budgetViolationCode = "skin_lua_wall_time_limit_exceeded";
  }
  if (status != 0 || impl_->shared->budgetViolated) {
    auto failure = luaFailure(impl_->state, status, *impl_->shared);
    lua_settop(impl_->state, 0);
    return {.failure = std::move(failure)};
  }
  std::optional<LuaScalar> value;
  try {
    value = readScalar(impl_->state, -1);
  } catch (...) {
    lua_settop(impl_->state, 0);
    return {.failure = makeDiagnostic(
                "skin_lua_callback_result_invalid",
                "Lua callback result could not be copied within host limits")};
  }
  lua_settop(impl_->state, 0);
  if (!value) {
    return {.failure = makeDiagnostic(
                "skin_lua_callback_result_invalid",
                "Lua callback did not return a supported scalar")};
  }
  return {.value = std::move(value)};
}

LuaRuntimePhase LuaSkinRuntime::phase() const noexcept {
  return impl_ ? impl_->phase : LuaRuntimePhase::Created;
}

std::span<const SkinCompatibilityDiagnostic>
LuaSkinRuntime::compatibilityDiagnostics() const noexcept {
  return impl_ ? impl_->hostModules->diagnostics()
               : std::span<const SkinCompatibilityDiagnostic>{};
}

} // namespace skin

#endif
