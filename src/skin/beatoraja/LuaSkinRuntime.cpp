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
#include <unordered_map>
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
constexpr std::size_t kMaximumCallbackScriptBytes = 64ULL * 1024;
constexpr std::size_t kMaximumCallbacks = 20'000;
constexpr std::size_t kMaximumDiagnosticBytes = 4ULL * 1024;
constexpr std::string_view kHostErrorPrefix = "@ASOBMSKIN:";
char kRuntimeRegistryKey;

#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
std::atomic<int> gTestAllocationPoint{-1};
#endif

SkinDiagnostic makeDiagnostic(std::string code, std::string message,
                              std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

std::string boundedLuaErrorText(lua_State *state, int index) {
  if (lua_type(state, index) != LUA_TSTRING) {
    return "Lua execution failed without a message";
  }
  std::size_t size = 0;
  const char *message = lua_tolstring(state, index, &size);
  if (message == nullptr) {
    return "Lua execution failed without a message";
  }
  return {message, std::min(size, kMaximumDiagnosticBytes)};
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
  std::unordered_map<const void *, std::uint32_t> callbackSlotsByIdentity;
  std::uint64_t callbackCompilationInstructions = 0;
  std::chrono::nanoseconds callbackCompilationWallUsed{0};
  Clock::time_point callbackCompilationStarted{};

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
#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
  std::atomic_bool failNextAllocationForTest = false;
#endif
};

void *quotaAllocator(void *userData, void *pointer, std::size_t oldSize,
                     std::size_t newSize) noexcept {
  auto *shared = static_cast<LuaRuntimeShared *>(userData);
#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
  if (newSize != 0 && shared->failNextAllocationForTest.exchange(false)) {
    return nullptr;
  }
#endif
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

#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
void failNextAllocationAt(LuaRuntimeShared &shared,
                          LuaRuntimeTestAllocationPoint point) noexcept {
  int expected = static_cast<int>(point);
  if (gTestAllocationPoint.compare_exchange_strong(expected, -1)) {
    shared.failNextAllocationForTest.store(true);
  }
}
#else
template <typename Point>
void failNextAllocationAt(LuaRuntimeShared &, Point) noexcept {}
#endif

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
  const std::string text = boundedLuaErrorText(state, -1);
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

SkinDiagnostic callbackFailureFromText(int status, const std::string &text) {
  if (status == LUA_ERRMEM) {
    return makeDiagnostic("skin_lua_allocator_limit_exceeded",
                          "Lua allocator quota was exhausted");
  }
  if (text.starts_with(kHostErrorPrefix)) {
    const std::size_t codeStart = kHostErrorPrefix.size();
    const std::size_t separator = text.find(':', codeStart);
    if (separator != std::string::npos) {
      return makeDiagnostic(text.substr(codeStart, separator - codeStart),
                            text.substr(separator + 1));
    }
  }
  return makeDiagnostic("skin_lua_execution_failed",
                        text.empty() ? "Lua callback failed" : text);
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

bool beginCallbackCompilationBudget(LuaRuntimeShared &shared,
                                    LuaLoadBudget budget) {
  shared.callbackActive = false;
  shared.budgetViolated = false;
  shared.budgetViolationCode = nullptr;
  const auto wallLimit =
      std::chrono::duration_cast<std::chrono::nanoseconds>(budget.maxWallTime);
  if (shared.callbackCompilationWallUsed >= wallLimit) {
    shared.executionActive = false;
    shared.budgetViolated = true;
    shared.budgetViolationCode = "skin_lua_wall_time_limit_exceeded";
    return false;
  }
  if (shared.callbackCompilationInstructions >= budget.maxInstructions) {
    shared.executionActive = false;
    shared.budgetViolated = true;
    shared.budgetViolationCode = "skin_lua_instruction_limit_exceeded";
    return false;
  }
  shared.executionActive = true;
  shared.executionInstructions = 0;
  shared.executionInstructionLimit =
      budget.maxInstructions - shared.callbackCompilationInstructions;
  shared.callbackCompilationStarted = Clock::now();
  shared.executionDeadline = shared.callbackCompilationStarted +
                             (wallLimit - shared.callbackCompilationWallUsed);
  return true;
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

void endCallbackCompilationBudget(LuaRuntimeShared &shared,
                                  LuaLoadBudget budget) {
  const auto wallLimit =
      std::chrono::duration_cast<std::chrono::nanoseconds>(budget.maxWallTime);
  const auto wallRemaining =
      wallLimit - std::min(shared.callbackCompilationWallUsed, wallLimit);
  shared.callbackCompilationWallUsed +=
      std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   Clock::now() - shared.callbackCompilationStarted),
               wallRemaining);
  if (shared.budgetViolated && shared.budgetViolationCode != nullptr &&
      std::string_view(shared.budgetViolationCode) ==
          "skin_lua_wall_time_limit_exceeded") {
    shared.callbackCompilationWallUsed = wallLimit;
  }
  const std::uint64_t remaining =
      budget.maxInstructions -
      std::min(shared.callbackCompilationInstructions, budget.maxInstructions);
  shared.callbackCompilationInstructions +=
      std::min(shared.executionInstructions, remaining);
  if (shared.budgetViolated && shared.budgetViolationCode != nullptr &&
      std::string_view(shared.budgetViolationCode) ==
          "skin_lua_instruction_limit_exceeded") {
    shared.callbackCompilationInstructions = budget.maxInstructions;
  }
  endExecutionBudget(shared);
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

int referenceArgument(lua_State *state) {
  lua_pushvalue(state, 1);
#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
  // Make the deterministic quota probe allocate inside this protected thunk.
  // Production never takes this branch.
  lua_newtable(state);
  lua_pop(state, 1);
#endif
  const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
  lua_pushinteger(state, reference);
  return 1;
}

enum class BindingLookupFailure : std::uint8_t {
  None,
  Missing,
  InvalidType,
  InvalidNumber,
  SourceTooLarge,
  WorkLimit,
  CallbackLimit,
  HostAllocation,
};

bool retainCallbackValue(lua_State *state, int index, LuaRuntimeShared &shared,
                         LuaCallbackId &callback,
                         BindingLookupFailure &failure) noexcept {
  const int candidate = absoluteIndex(state, index);
  const void *identity = lua_topointer(state, candidate);
  if (identity == nullptr) {
    failure = BindingLookupFailure::InvalidType;
    return false;
  }
  if (const auto existing = shared.callbackSlotsByIdentity.find(identity);
      existing != shared.callbackSlotsByIdentity.end()) {
    callback = {.slot = existing->second, .generation = shared.generation};
    return true;
  }
  if (shared.callbackReferences.size() >= kMaximumCallbacks) {
    failure = BindingLookupFailure::CallbackLimit;
    return false;
  }
  lua_pushvalue(state, candidate);
  // The registry reference keeps this exact function alive, so Lua cannot
  // recycle its identity pointer while the O(1) identity index contains it.
  const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
  try {
    shared.callbackReferences.push_back(reference);
    const auto slot =
        static_cast<std::uint32_t>(shared.callbackReferences.size());
    try {
      const auto [existing, inserted] =
          shared.callbackSlotsByIdentity.emplace(identity, slot);
      if (!inserted) {
        shared.callbackReferences.pop_back();
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        callback = {.slot = existing->second,
                    .generation = shared.generation};
        return true;
      }
    } catch (...) {
      shared.callbackReferences.pop_back();
      throw;
    }
  } catch (...) {
    luaL_unref(state, LUA_REGISTRYINDEX, reference);
    failure = BindingLookupFailure::HostAllocation;
    return false;
  }
  callback = {
      .slot = static_cast<std::uint32_t>(shared.callbackReferences.size()),
      .generation = shared.generation,
  };
  return true;
}

struct CallbackLookupRequest {
  LuaRuntimeShared *shared = nullptr;
  int valueReference = LUA_NOREF;
  std::string_view name;
  bool found = false;
  LuaCallbackId callback;
  BindingLookupFailure failure = BindingLookupFailure::None;
};

struct BindingSourceLookupRequest {
  LuaRuntimeShared *shared = nullptr;
  int valueReference = LUA_NOREF;
  const LuaValuePath *path = nullptr;
  LuaBindingSourceLookupLimits limits;
  std::optional<LuaBindingSourceValue> source;
  BindingLookupFailure failure = BindingLookupFailure::None;
  std::size_t workBytes = 0;
};

bool chargeBindingLookupWork(BindingSourceLookupRequest &request,
                             std::size_t bytes) noexcept {
  const std::size_t charge = std::max<std::size_t>(bytes, 1);
  if (charge > request.limits.remainingWorkBytes) {
    request.workBytes = request.limits.remainingWorkBytes;
    request.failure = BindingLookupFailure::WorkLimit;
    return false;
  }
  request.workBytes = charge;
  return true;
}

bool luaJNumericStringSyntax(std::string_view text) noexcept {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return false;
  }
  // LuaJ's custom parser truncates its decimal fallback at 64 bytes. Keep the
  // shared safe subset instead of reproducing trailing-garbage acceptance.
  if (text.size() > 64) {
    return false;
  }
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    return std::ranges::all_of(text.substr(2), [](char value) noexcept {
      return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
             (value >= 'A' && value <= 'F');
    });
  }
  return std::ranges::all_of(text, [](char value) noexcept {
    return (value >= '0' && value <= '9') || value == '+' || value == '-' ||
           value == '.' || value == 'e' || value == 'E';
  });
}

void storeNumericBinding(lua_State *state, int index,
                         BindingSourceLookupRequest &request) noexcept {
  const double numeric = static_cast<double>(lua_tonumber(state, index));
  // LuaJ wraps some overflows during integer conversion. Keep the portable
  // LuaJ/LuaJIT subset by rejecting non-finite and out-of-range values first.
  if (!std::isfinite(numeric) ||
      numeric < static_cast<double>(std::numeric_limits<int>::min()) ||
      numeric > static_cast<double>(std::numeric_limits<int>::max())) {
    request.failure = BindingLookupFailure::InvalidNumber;
    return;
  }
  request.source = static_cast<int>(numeric);
}

int lookupBindingSourceArgument(lua_State *state) {
  auto *request =
      static_cast<BindingSourceLookupRequest *>(lua_touserdata(state, 1));
  if (request == nullptr || request->shared == nullptr ||
      request->path == nullptr) {
    return 0;
  }
  lua_rawgeti(state, LUA_REGISTRYINDEX, request->valueReference);
  for (const auto &element : *request->path) {
    if (!lua_istable(state, -1)) {
      request->failure = BindingLookupFailure::Missing;
      return 0;
    }
    if (const auto *field = std::get_if<std::string>(&element.key)) {
      lua_pushlstring(state, field->data(), field->size());
    } else {
      lua_pushinteger(state, static_cast<lua_Integer>(
                                 std::get<std::uint32_t>(element.key)));
    }
    lua_rawget(state, -2);
    lua_remove(state, -2);
  }

  const int type = lua_type(state, -1);
  if (type == LUA_TFUNCTION) {
    if (!chargeBindingLookupWork(*request, 1)) {
      return 0;
    }
    LuaCallbackId callback;
    if (retainCallbackValue(state, -1, *request->shared, callback,
                            request->failure)) {
      request->source = callback;
    }
    return 0;
  }

  if (type == LUA_TNUMBER && request->limits.numericFactoryAvailable) {
    if (chargeBindingLookupWork(*request, 1)) {
      storeNumericBinding(state, -1, *request);
    }
    return 0;
  }

  if (type == LUA_TSTRING || type == LUA_TNUMBER) {
    if (type == LUA_TNUMBER && !chargeBindingLookupWork(*request, 1)) {
      return 0;
    }
    std::size_t size = 0;
    const char *text = lua_tolstring(state, -1, &size);
    if (text == nullptr) {
      request->failure = BindingLookupFailure::InvalidType;
      return 0;
    }
    if (size > request->limits.maxStringBytes) {
      request->workBytes =
          std::min<std::size_t>(request->limits.remainingWorkBytes, 1);
      request->failure = BindingLookupFailure::SourceTooLarge;
      return 0;
    }
    if (!chargeBindingLookupWork(*request, size)) {
      return 0;
    }
    if (request->limits.numericFactoryAvailable &&
        luaJNumericStringSyntax(std::string_view(text, size)) &&
        lua_isnumber(state, -1)) {
      storeNumericBinding(state, -1, *request);
      return 0;
    }
    try {
      request->source = std::string(text, size);
    } catch (...) {
      request->source.reset();
      request->failure = BindingLookupFailure::HostAllocation;
    }
    return 0;
  }

  chargeBindingLookupWork(*request, 1);
  if (type == LUA_TNIL) {
    request->failure = BindingLookupFailure::Missing;
  } else if (request->failure == BindingLookupFailure::None) {
    request->failure = BindingLookupFailure::InvalidType;
  }
  return 0;
}

enum class CallbackCompileFailure : std::uint8_t {
  None,
  InvalidScript,
  ScriptExecution,
  CallbackLimit,
  HostAllocation,
};

struct CallbackCompileRequest {
  LuaRuntimeShared *shared = nullptr;
  std::string_view source;
  LuaCallbackScriptKind kind = LuaCallbackScriptKind::ReturnExpression;
  LuaCallbackId callback;
  CallbackCompileFailure failure = CallbackCompileFailure::None;
  std::string error;
};

int compileCallbackArgument(lua_State *state) {
  auto *request =
      static_cast<CallbackCompileRequest *>(lua_touserdata(state, 1));
  if (request == nullptr || request->shared == nullptr) {
    return 0;
  }
  const int loadStatus =
      luaL_loadbuffer(state, request->source.data(), request->source.size(),
                      "@skin-binding-script");
  if (loadStatus != 0) {
    request->failure = CallbackCompileFailure::InvalidScript;
    try {
      request->error = boundedLuaErrorText(state, -1);
    } catch (...) {
      request->error = "Lua callback script is invalid";
    }
    return 0;
  }

  int candidate = lua_gettop(state);
  if (request->kind == LuaCallbackScriptKind::Timer) {
    lua_pushvalue(state, candidate);
    const int trialStatus = lua_pcall(state, 0, 1, 0);
    if (trialStatus != 0) {
      request->failure = CallbackCompileFailure::ScriptExecution;
      try {
        request->error = boundedLuaErrorText(state, -1);
      } catch (...) {
        request->error = "Lua timer callback trial failed";
      }
      return 0;
    }
    if (lua_isfunction(state, -1)) {
      candidate = lua_gettop(state);
    } else {
      lua_pop(state, 1);
    }
  }

  BindingLookupFailure retainFailure = BindingLookupFailure::None;
  if (!retainCallbackValue(state, candidate, *request->shared,
                           request->callback, retainFailure)) {
    request->failure = retainFailure == BindingLookupFailure::CallbackLimit
                           ? CallbackCompileFailure::CallbackLimit
                           : CallbackCompileFailure::HostAllocation;
  }
  return 0;
}

struct ProtectedValueRequest {
  int valueReference = LUA_NOREF;
  void *context = nullptr;
  void (*operation)(lua_State *, int, void *) noexcept = nullptr;
};

int accessProtectedValue(lua_State *state) {
  auto *request =
      static_cast<ProtectedValueRequest *>(lua_touserdata(state, 1));
  if (request == nullptr || request->operation == nullptr) {
    return 0;
  }
  lua_rawgeti(state, LUA_REGISTRYINDEX, request->valueReference);
  request->operation(state, lua_gettop(state), request->context);
  return 0;
}

int lookupCallbackArgument(lua_State *state) {
  auto *request = static_cast<CallbackLookupRequest *>(lua_touserdata(state, 1));
  if (request == nullptr || request->shared == nullptr) {
    return 0;
  }
  auto &shared = *request->shared;
  lua_rawgeti(state, LUA_REGISTRYINDEX, request->valueReference);
  if (!lua_istable(state, -1)) {
    return 0;
  }
#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
  failNextAllocationAt(shared, LuaRuntimeTestAllocationPoint::CallbackName);
  lua_newtable(state);
  lua_pop(state, 1);
#endif
  lua_pushlstring(state, request->name.data(), request->name.size());
  lua_rawget(state, -2);
  if (!lua_isfunction(state, -1)) {
    return 0;
  }
#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
  failNextAllocationAt(shared, LuaRuntimeTestAllocationPoint::CallbackReference);
  lua_newtable(state);
  lua_pop(state, 1);
#endif
  request->found = true;
  retainCallbackValue(state, -1, shared, request->callback, request->failure);
  return 0;
}

struct InvokeRequest {
  LuaRuntimeShared *shared = nullptr;
  int callbackReference = LUA_NOREF;
  std::span<const LuaScalar> arguments;
  int status = 0;
  std::optional<LuaScalar> value;
  std::string error;
  bool resultInvalid = false;
};

int invokeArgument(lua_State *state) {
  auto *request = static_cast<InvokeRequest *>(lua_touserdata(state, 1));
  if (request == nullptr) {
    return 0;
  }
  lua_rawgeti(state, LUA_REGISTRYINDEX, request->callbackReference);
  for (const LuaScalar &argument : request->arguments) {
#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
    failNextAllocationAt(*request->shared,
                         LuaRuntimeTestAllocationPoint::InvokeArgument);
    lua_newtable(state);
    lua_pop(state, 1);
#endif
    pushScalar(state, argument);
  }
  request->status = lua_pcall(state, static_cast<int>(request->arguments.size()),
                               1, 0);
  if (request->status != 0) {
    try {
      request->error = boundedLuaErrorText(state, -1);
    } catch (...) {
      request->error = "Lua callback failed";
    }
    return 0;
  }
  try {
    request->value = readScalar(state, -1);
    request->resultInvalid = !request->value.has_value();
  } catch (...) {
    request->resultInvalid = true;
  }
  return 0;
}

struct LuaSkinRuntime::Impl {
  LuaRuntimePurpose purpose = LuaRuntimePurpose::Catalog;
  LuaRuntimePhase phase = LuaRuntimePhase::Created;
  bool renderTransitionFailed = false;
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
    // luaL_ref may grow the registry table.  Keep that allocation within a
    // Lua protected call so quota exhaustion cannot reach the panic handler.
    if (!lua_checkstack(state, 1)) {
      lua_settop(state, 0);
      endExecutionBudget(*shared);
      return {.failure = makeDiagnostic("skin_lua_allocator_limit_exceeded",
                                        "Lua value reference stack allocation failed")};
    }
    lua_pushcfunction(state, referenceArgument);
    lua_insert(state, 1);
#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
    failNextAllocationAt(*shared, LuaRuntimeTestAllocationPoint::ValueReference);
#endif
    const int referenceStatus = lua_pcall(state, 1, 1, 0);
    if (referenceStatus != 0) {
      auto failure = luaFailure(state, referenceStatus, *shared);
      lua_settop(state, 0);
      endExecutionBudget(*shared);
      return {.failure = std::move(failure)};
    }
    const int reference = static_cast<int>(lua_tointeger(state, -1));
    lua_settop(state, 0);
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

std::optional<SkinDiagnostic>
LuaValueHandle::withValueProtected(void *context,
                                   ProtectedValueOperation operation) const {
  if (!impl_ || !impl_->shared || impl_->shared->state == nullptr ||
      impl_->reference == LUA_NOREF || operation == nullptr) {
    return makeDiagnostic("skin_lua_value_invalid",
                          "Lua value handle is no longer valid");
  }
  lua_State *state = impl_->shared->state;
  const int savedTop = lua_gettop(state);
  ProtectedValueRequest request{.valueReference = impl_->reference,
                                .context = context,
                                .operation = operation};
  const int status = lua_cpcall(state, accessProtectedValue, &request);
  if (status != 0) {
    auto failure = luaFailure(state, status, *impl_->shared);
    lua_settop(state, savedTop);
    return failure;
  }
  lua_settop(state, savedTop);
  return std::nullopt;
}

std::optional<LuaCallbackId>
LuaValueHandle::callbackNamed(std::string_view name) const {
  return lookupCallbackNamed(name).callback;
}

LuaCallbackLookupResult
LuaValueHandle::lookupCallbackNamed(std::string_view name) const {
  if (!impl_ || !impl_->shared || impl_->shared->state == nullptr ||
      impl_->reference == LUA_NOREF || name.empty()) {
    return {};
  }
  lua_State *state = impl_->shared->state;
  CallbackLookupRequest request{.shared = impl_->shared.get(),
                                .valueReference = impl_->reference,
                                .name = name};
  const int status = lua_cpcall(state, lookupCallbackArgument, &request);
  if (status != 0) {
    auto failure = luaFailure(state, status, *impl_->shared);
    lua_settop(state, 0);
    return {.failure = std::move(failure)};
  }
  if (!request.found) {
    return {};
  }
  if (request.failure == BindingLookupFailure::CallbackLimit) {
    return {.failure = makeDiagnostic("skin_lua_callback_limit_exceeded",
                                      "Lua callback limit is exhausted")};
  }
  if (request.failure != BindingLookupFailure::None || !request.callback) {
    return {.failure = makeDiagnostic("skin_lua_allocator_limit_exceeded",
                                      "Lua callback handle allocation failed")};
  }
  return {.callback = request.callback};
}

LuaBindingSourceLookupResult
LuaValueHandle::lookupBindingSource(const LuaValuePath &path,
                                    LuaBindingSourceLookupLimits limits) const {
  if (path.size() > LuaRuntimePolicy::maxBindingPathDepth) {
    return {.failure = makeDiagnostic(
                "skin_lua_binding_path_too_deep",
                "Lua binding path exceeds the maximum supported depth")};
  }
  if (!impl_ || !impl_->shared || impl_->shared->state == nullptr ||
      impl_->reference == LUA_NOREF || path.empty()) {
    return {.failure = makeDiagnostic("skin_lua_binding_missing",
                                      "Lua binding path is unavailable")};
  }
  lua_State *state = impl_->shared->state;
  const int savedTop = lua_gettop(state);
  BindingSourceLookupRequest request{.shared = impl_->shared.get(),
                                     .valueReference = impl_->reference,
                                     .path = &path,
                                     .limits = limits};
  const int status = lua_cpcall(state, lookupBindingSourceArgument, &request);
  if (status != 0) {
    auto failure = luaFailure(state, status, *impl_->shared);
    lua_settop(state, savedTop);
    return {.failure = std::move(failure), .workBytes = request.workBytes};
  }
  lua_settop(state, savedTop);
  if (request.source) {
    return {.source = std::move(request.source),
            .workBytes = request.workBytes};
  }
  switch (request.failure) {
  case BindingLookupFailure::Missing:
    return {.failure = makeDiagnostic("skin_lua_binding_missing",
                                      "Lua binding path is nil or missing"),
            .workBytes = request.workBytes};
  case BindingLookupFailure::InvalidNumber:
    return {.failure = makeDiagnostic(
                "skin_lua_binding_number_invalid",
                "Lua binding number is outside the supported integer range"),
            .workBytes = request.workBytes};
  case BindingLookupFailure::SourceTooLarge:
    return {.failure = makeDiagnostic(
                "skin_lua_binding_source_too_large",
                "Lua binding name or script exceeds the fixed byte limit"),
            .workBytes = request.workBytes};
  case BindingLookupFailure::WorkLimit:
    return {.failure = makeDiagnostic(
                "skin_lua_binding_work_limit_exceeded",
                "Lua binding source work exceeds the cumulative byte limit"),
            .workBytes = request.workBytes};
  case BindingLookupFailure::CallbackLimit:
    return {.failure = makeDiagnostic("skin_lua_callback_limit_exceeded",
                                      "Lua callback limit is exhausted"),
            .workBytes = request.workBytes};
  case BindingLookupFailure::HostAllocation:
    return {.failure =
                makeDiagnostic("skin_lua_allocator_limit_exceeded",
                               "Lua binding source could not be retained"),
            .workBytes = request.workBytes};
  case BindingLookupFailure::InvalidType:
  case BindingLookupFailure::None:
    return {.failure = makeDiagnostic(
                "skin_lua_binding_type_invalid",
                "Lua binding source is not a number, string, or function"),
            .workBytes = request.workBytes};
  }
  return {.failure = makeDiagnostic("skin_lua_binding_type_invalid",
                                    "Lua binding source is invalid"),
          .workBytes = request.workBytes};
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
  if (auto failure = impl_->hostModules->enableStateAccessors()) {
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
      impl_->phase != LuaRuntimePhase::Configured ||
      impl_->renderTransitionFailed) {
    return {.failure = makeDiagnostic(
                "skin_lua_phase_invalid",
                "Lua render transition requires configured gameplay")};
  }
  const auto handles = impl_->hostModules->invalidateFileHandles();
  const auto transition = impl_->fileSystem->enterRenderPhase();
  if (!transition.ok) {
    impl_->renderTransitionFailed = true;
    return {.failure = makeDiagnostic(
                "skin_file_render_phase_denied",
                transition.failure ? transition.failure->message
                                   : "Lua filesystem transition failed")};
  }
  if (handles.hadDirtyWrite) {
    impl_->renderTransitionFailed = true;
    return {.failure = makeDiagnostic(
                "skin_file_render_phase_denied",
                "dirty Lua file handle was discarded at render transition")};
  }
  impl_->phase = LuaRuntimePhase::Render;
  return {.ok = true};
}

void LuaSkinRuntime::setFrameState(ISkinFrameState *state) noexcept {
  if (impl_ && impl_->hostModules) {
    impl_->hostModules->setFrameState(state);
  }
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

  beginCallbackBudget(*impl_->shared);
  InvokeRequest request{.shared = impl_->shared.get(),
                        .callbackReference =
                            impl_->shared->callbackReferences[callback.slot - 1],
                        .arguments = arguments};
  const int protectedStatus =
      lua_cpcall(impl_->state, invokeArgument, &request);
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
  if (protectedStatus != 0 || request.status != 0 ||
      impl_->shared->budgetViolated) {
    SkinDiagnostic failure = impl_->shared->budgetViolated &&
                                     impl_->shared->budgetViolationCode != nullptr
                                 ? makeDiagnostic(impl_->shared->budgetViolationCode,
                                                  "Lua execution budget exceeded")
                                 : protectedStatus != 0
                                       ? luaFailure(impl_->state, protectedStatus,
                                                    *impl_->shared)
                                       : callbackFailureFromText(request.status,
                                                                 request.error);
    lua_settop(impl_->state, 0);
    return {.failure = std::move(failure)};
  }
  if (request.resultInvalid) {
    lua_settop(impl_->state, 0);
    return {.failure = makeDiagnostic(
                "skin_lua_callback_result_invalid",
                "Lua callback result could not be copied within host limits")};
  }
  lua_settop(impl_->state, 0);
  if (!request.value) {
    return {.failure = makeDiagnostic(
                "skin_lua_callback_result_invalid",
                "Lua callback did not return a supported scalar")};
  }
  return {.value = std::move(request.value)};
}

LuaCallbackCompileResult
LuaSkinRuntime::compileCallbackScript(std::string_view script,
                                      LuaCallbackScriptKind kind) {
  if (!impl_ || impl_->phase != LuaRuntimePhase::Configured || script.empty() ||
      script.size() > kMaximumCallbackScriptBytes) {
    return {.failure = makeDiagnostic("skin_lua_callback_script_invalid",
                                      "Lua callback script requires a "
                                      "configured runtime and bounded source")};
  }

  const LuaLoadBudget budget = LuaRuntimePolicy::loadBudget(impl_->purpose);
  if (!beginCallbackCompilationBudget(*impl_->shared, budget)) {
    return {.failure =
                makeDiagnostic(impl_->shared->budgetViolationCode,
                               "Lua callback compilation budget is exhausted")};
  }

  std::string source;
  try {
    if (kind == LuaCallbackScriptKind::Statement) {
      source.assign(script);
    } else {
      source.reserve(script.size() + 7);
      source.append("return ");
      source.append(script);
    }
  } catch (...) {
    endCallbackCompilationBudget(*impl_->shared, budget);
    return {.failure = makeDiagnostic("skin_lua_allocator_limit_exceeded",
                                      "Lua callback script allocation failed")};
  }

  lua_State *state = impl_->state;
  const int savedTop = lua_gettop(state);
  CallbackCompileRequest request{
      .shared = impl_->shared.get(), .source = source, .kind = kind};
  const int status = lua_cpcall(state, compileCallbackArgument, &request);
  if (!impl_->shared->budgetViolated &&
      Clock::now() > impl_->shared->executionDeadline) {
    impl_->shared->budgetViolated = true;
    impl_->shared->budgetViolationCode = "skin_lua_wall_time_limit_exceeded";
  }
  endCallbackCompilationBudget(*impl_->shared, budget);
  if (status != 0) {
    auto failure = luaFailure(state, status, *impl_->shared);
    lua_settop(state, savedTop);
    return {.failure = std::move(failure)};
  }
  lua_settop(state, savedTop);
  if (impl_->shared->budgetViolated &&
      impl_->shared->budgetViolationCode != nullptr) {
    return {.failure = makeDiagnostic(impl_->shared->budgetViolationCode,
                                      "Lua callback script budget exceeded")};
  }
  switch (request.failure) {
  case CallbackCompileFailure::None:
    if (request.callback.slot != 0 && request.callback.generation != 0) {
      return {.callback = request.callback};
    }
    break;
  case CallbackCompileFailure::InvalidScript:
    return {.failure = makeDiagnostic("skin_lua_callback_script_invalid",
                                      request.error.empty()
                                          ? "Lua callback script is invalid"
                                          : std::move(request.error))};
  case CallbackCompileFailure::ScriptExecution:
    return {.failure = makeDiagnostic("skin_lua_callback_script_failed",
                                      request.error.empty()
                                          ? "Lua callback script trial failed"
                                          : std::move(request.error))};
  case CallbackCompileFailure::CallbackLimit:
    return {.failure = makeDiagnostic("skin_lua_callback_limit_exceeded",
                                      "Lua callback limit is exhausted")};
  case CallbackCompileFailure::HostAllocation:
    return {.failure =
                makeDiagnostic("skin_lua_allocator_limit_exceeded",
                               "Lua callback script could not be retained")};
  }
  return {.failure =
              makeDiagnostic("skin_lua_callback_script_invalid",
                             "Lua callback script produced no callback")};
}

LuaCallbackLivenessView LuaSkinRuntime::callbackLiveness() const noexcept {
  if (!impl_ || !impl_->shared || impl_->shared->state == nullptr ||
      impl_->shared->callbackReferences.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return {0, 0};
  }
  return {impl_->shared->generation,
          static_cast<std::uint32_t>(impl_->shared->callbackReferences.size())};
}

#if defined(ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS)
void LuaRuntimeTestHooks::failNextAllocationAt(
    LuaRuntimeTestAllocationPoint point) noexcept {
  gTestAllocationPoint.store(static_cast<int>(point));
}
#endif

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
