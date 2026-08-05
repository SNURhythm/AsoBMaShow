#include "LuaSkinHostModules.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "LuaSkinFileSystem.h"
#include "Skin2DRenderer.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skin {
namespace {

constexpr const char *kHandleMetatable = "AsoBMaShow.LuaSkinFileHandle";
constexpr const char *kErrorPrefix = "@ASOBMSKIN:";

enum class HandleMode : std::uint8_t { Read, Write, Append };

struct LuaFileHandle;
using SharedLuaFileHandle = std::shared_ptr<LuaFileHandle>;

SkinDiagnostic diagnostic(std::string code, std::string message,
                          std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

const char *fileFailureCode(SkinFileError error) noexcept {
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

std::uint32_t bitArgument(lua_State *state, int index) {
  const double value = static_cast<double>(luaL_checknumber(state, index));
  if (!std::isfinite(value)) {
    luaL_argerror(state, index, "finite integer expected");
  }
  constexpr double modulus = 4294967296.0;
  double normalized = std::fmod(std::trunc(value), modulus);
  if (normalized < 0) {
    normalized += modulus;
  }
  return static_cast<std::uint32_t>(normalized);
}

int boundedIntegerArgument(lua_State *state, int index, int fallback,
                           bool optional) {
  if (optional && lua_isnoneornil(state, index)) {
    return fallback;
  }
  const double value = static_cast<double>(luaL_checknumber(state, index));
  if (!std::isfinite(value)) {
    return luaL_argerror(state, index, "finite integer expected");
  }
  const double integer = std::trunc(value);
  if (integer > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (integer < static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(integer);
}

void pushUnsigned(lua_State *state, std::uint32_t value) {
  lua_pushnumber(state, static_cast<lua_Number>(value));
}

int bitBand(lua_State *state) {
  std::uint32_t value = 0xffffffffU;
  for (int index = 1; index <= lua_gettop(state); ++index) {
    value &= bitArgument(state, index);
  }
  pushUnsigned(state, value);
  return 1;
}

int bitBor(lua_State *state) {
  std::uint32_t value = 0;
  for (int index = 1; index <= lua_gettop(state); ++index) {
    value |= bitArgument(state, index);
  }
  pushUnsigned(state, value);
  return 1;
}

int bitBxor(lua_State *state) {
  std::uint32_t value = 0;
  for (int index = 1; index <= lua_gettop(state); ++index) {
    value ^= bitArgument(state, index);
  }
  pushUnsigned(state, value);
  return 1;
}

int bitBnot(lua_State *state) {
  pushUnsigned(state, ~bitArgument(state, 1));
  return 1;
}

int shiftLeft(lua_State *state) {
  const std::uint32_t value = bitArgument(state, 1);
  const int displacement = boundedIntegerArgument(state, 2, 0, false);
  if (displacement < 0) {
    const unsigned amount =
        static_cast<unsigned>(-static_cast<long long>(displacement));
    pushUnsigned(state, amount >= 32 ? 0 : value >> amount);
  } else {
    const unsigned amount = static_cast<unsigned>(displacement);
    pushUnsigned(state, amount >= 32 ? 0 : value << amount);
  }
  return 1;
}

int shiftRight(lua_State *state) {
  const std::uint32_t value = bitArgument(state, 1);
  const int displacement = boundedIntegerArgument(state, 2, 0, false);
  if (displacement < 0) {
    const unsigned amount =
        static_cast<unsigned>(-static_cast<long long>(displacement));
    pushUnsigned(state, amount >= 32 ? 0 : value << amount);
  } else {
    const unsigned amount = static_cast<unsigned>(displacement);
    pushUnsigned(state, amount >= 32 ? 0 : value >> amount);
  }
  return 1;
}

int arithmeticShiftRight(lua_State *state) {
  const std::uint32_t value = bitArgument(state, 1);
  const int displacement = boundedIntegerArgument(state, 2, 0, false);
  if (displacement < 0) {
    const unsigned amount =
        static_cast<unsigned>(-static_cast<long long>(displacement));
    pushUnsigned(state, amount >= 32 ? 0 : value << amount);
    return 1;
  }
  const unsigned amount = static_cast<unsigned>(displacement);
  const std::int32_t signedValue = std::bit_cast<std::int32_t>(value);
  if (amount >= 32) {
    pushUnsigned(state, signedValue < 0 ? 0xffffffffU : 0U);
  } else if (amount == 0) {
    pushUnsigned(state, value);
  } else if (signedValue < 0) {
    pushUnsigned(state, (value >> amount) | (0xffffffffU << (32 - amount)));
  } else {
    pushUnsigned(state, value >> amount);
  }
  return 1;
}

bool bitField(lua_State *state, unsigned &field, unsigned &width) {
  const int requestedField = boundedIntegerArgument(state, 2, 0, false);
  const int requestedWidth = boundedIntegerArgument(state, 3, 1, true);
  if (requestedField < 0 || requestedField > 31 || requestedWidth <= 0 ||
      requestedWidth > 32 - requestedField) {
    return false;
  }
  field = static_cast<unsigned>(requestedField);
  width = static_cast<unsigned>(requestedWidth);
  return true;
}

int bitExtract(lua_State *state) {
  unsigned field = 0;
  unsigned width = 0;
  if (!bitField(state, field, width)) {
    return luaL_error(state, "bit field is out of range");
  }
  const std::uint32_t mask = width == 32 ? 0xffffffffU : (1U << width) - 1U;
  pushUnsigned(state, (bitArgument(state, 1) >> field) & mask);
  return 1;
}

int bitReplace(lua_State *state) {
  const int requestedField = boundedIntegerArgument(state, 3, 0, false);
  const int requestedWidth = boundedIntegerArgument(state, 4, 1, true);
  if (requestedField < 0 || requestedField > 31 || requestedWidth <= 0 ||
      requestedWidth > 32 - requestedField) {
    return luaL_error(state, "bit field is out of range");
  }
  const unsigned field = static_cast<unsigned>(requestedField);
  const unsigned width = static_cast<unsigned>(requestedWidth);
  const std::uint32_t mask = width == 32 ? 0xffffffffU : (1U << width) - 1U;
  const std::uint32_t shiftedMask = mask << field;
  const std::uint32_t destination = bitArgument(state, 1);
  const std::uint32_t replacement = bitArgument(state, 2);
  pushUnsigned(state,
               (destination & ~shiftedMask) | ((replacement & mask) << field));
  return 1;
}

} // namespace

struct LuaSkinHostModulesImpl {
  lua_State *state = nullptr;
  LuaSkinFileSystem *fileSystem = nullptr;
  ISkinFrameState *frameState = nullptr;
  bool allowOverlayWrites = false;
  void *coroutineContext = nullptr;
  LuaCoroutineCreatedCallback coroutineCreated = nullptr;
  LuaSkinEventExecutor eventExecutor;
  SkinCompatibilityDiagnostics diagnostics;
  std::vector<std::weak_ptr<LuaFileHandle>> handles;
  std::size_t openHandleCount = 0;
  std::uint64_t openHandleBytes = 0;
  int fileTokenReference = LUA_NOREF;
  int gdxTokenReference = LUA_NOREF;
  const BeatorajaSkinConfiguration *pendingConfiguration = nullptr;
  std::vector<ConfiguredFile> configuredFiles;
  std::string resolvedConfigurationPath;
  std::string lastErrorCode;
  std::string lastErrorMessage;

  void storeError(std::string_view code, std::string_view message,
                  std::string_view virtualPath = {}) noexcept {
    try {
      lastErrorCode.assign(code);
      lastErrorMessage.assign(message);
      if (!virtualPath.empty()) {
        lastErrorMessage += " [";
        lastErrorMessage.append(virtualPath);
        lastErrorMessage += ']';
      }
    } catch (...) {
      lastErrorCode = "skin_lua_host_failure";
      lastErrorMessage = "Lua skin host operation failed";
    }
  }

  void storeFileError(const SkinFileFailure &failure) noexcept {
    storeError(fileFailureCode(failure.code), failure.message,
               failure.virtualPath);
  }

  void reportLegacyDenial(std::string_view authority) noexcept {
    const std::string_view diagnosticAuthority =
        authority == "java.net.URL" || authority == "java.io.File.member" ||
                authority == "java.io.File.constructor" || authority == "bindClass"
            ? authority
            : "unknown_legacy_authority";
    try {
      diagnostics.report(
          diagnostic("skin_legacy_lua_access_denied",
                     "legacy Lua authority is outside the audited facade"),
          true, diagnosticAuthority);
    } catch (...) {
    }
    storeError("skin_legacy_lua_access_denied",
               "legacy Lua authority is outside the audited facade");
  }

  void releaseHandle(LuaFileHandle &handle) noexcept;
  bool copyConfiguredFiles(const std::vector<ConfiguredFile> &) noexcept;
  bool resolveConfiguredPath(std::string_view) noexcept;
  bool reserveHandleBytes(std::uint64_t bytes) noexcept {
    return bytes <= LuaSkinHostPolicy::maxAggregateHandleBytes -
                        std::min(openHandleBytes,
                                 LuaSkinHostPolicy::maxAggregateHandleBytes);
  }
};

namespace {

struct LuaFileHandle {
  LuaSkinHostModulesImpl *owner = nullptr;
  std::string virtualPath;
  HandleMode mode = HandleMode::Read;
  std::vector<std::byte> bytes;
  std::size_t readOffset = 0;
  bool closed = false;
  bool invalidated = false;
  bool dirty = false;
  bool holdsQuota = false;
  std::uint64_t accountedBytes = 0;

  ~LuaFileHandle() {
    if (owner != nullptr && holdsQuota) {
      owner->releaseHandle(*this);
    }
  }
};

} // namespace

void LuaSkinHostModulesImpl::releaseHandle(LuaFileHandle &handle) noexcept {
  openHandleBytes = handle.accountedBytes <= openHandleBytes
                        ? openHandleBytes - handle.accountedBytes
                        : 0;
  handle.accountedBytes = 0;
  if (!handle.holdsQuota) {
    return;
  }
  handle.holdsQuota = false;
  if (openHandleCount > 0) {
    --openHandleCount;
  }
}

bool LuaSkinHostModulesImpl::copyConfiguredFiles(
    const std::vector<ConfiguredFile> &files) noexcept {
  if (files.size() > SkinProfileSettingsPolicy::maxFilesPerEntry) {
    storeError("skin_lua_configuration_export_failed",
               "Lua skin configured file list exceeds its fixed limit");
    return false;
  }
  for (const auto &file : files) {
    if (file.name.size() >
            SkinProfileSettingsPolicy::maxConfigurationKeyBytes ||
        file.pattern.size() >
            SkinProfileSettingsPolicy::maxConfigurationValueBytes ||
        file.selectedValue.size() >
            SkinProfileSettingsPolicy::maxConfigurationValueBytes) {
      storeError("skin_lua_configuration_export_failed",
                 "Lua skin configured file text exceeds its fixed limit");
      return false;
    }
  }
  try {
    configuredFiles = files;
    resolvedConfigurationPath.clear();
    return true;
  } catch (...) {
    storeError("skin_lua_configuration_export_failed",
               "Lua skin configured file copy failed");
    return false;
  }
}

bool LuaSkinHostModulesImpl::resolveConfiguredPath(
    std::string_view request) noexcept {
  try {
    const ConfiguredFile *match = nullptr;
    for (const auto &file : configuredFiles) {
      if (!request.starts_with(file.pattern)) {
        continue;
      }
      if (match != nullptr) {
        storeError("skin_lua_file_operation_failed",
                   "skin_config.get_path matched multiple configured files");
        return false;
      }
      match = &file;
    }
    if (match == nullptr) {
      if (request.find('*') == std::string_view::npos) {
        auto resolved = fileSystem->resolve(request, SkinFileUse::Resource);
        if (resolved.failure) {
          storeFileError(*resolved.failure);
          return false;
        }
        if (!resolved.normalizedVirtualPath) {
          storeError("skin_lua_file_operation_failed",
                     "skin_config.get_path did not resolve a resource");
          return false;
        }
        resolvedConfigurationPath = std::move(*resolved.normalizedVirtualPath);
        return true;
      }
      storeError("skin_lua_file_operation_failed",
                 "skin_config.get_path has no configured file match");
      return false;
    }

    const std::size_t wildcard = request.rfind('*');
    if (wildcard == std::string::npos ||
        request.size() < match->pattern.size()) {
      storeError("skin_lua_file_operation_failed",
                 "skin_config.get_path pattern cannot be resolved");
      return false;
    }
    const std::size_t requestSuffixSize =
        request.size() - match->pattern.size();
    if (wildcard > SkinPackagePolicy::maxPathBytes ||
        match->selectedValue.size() >
            SkinPackagePolicy::maxPathBytes - wildcard ||
        requestSuffixSize > SkinPackagePolicy::maxPathBytes - wildcard -
                                match->selectedValue.size()) {
      storeError("skin_lua_file_operation_failed",
                 "skin_config.get_path result exceeds its fixed limit");
      return false;
    }

    std::string candidate;
    candidate.reserve(wildcard + match->selectedValue.size() +
                      requestSuffixSize);
    candidate.append(request, 0, wildcard);
    candidate.append(match->selectedValue);
    candidate.append(request, match->pattern.size(), requestSuffixSize);

    auto resolved = fileSystem->resolve(candidate, SkinFileUse::Resource);
    if (resolved.failure) {
      storeFileError(*resolved.failure);
      return false;
    }
    if (!resolved.normalizedVirtualPath) {
      storeError("skin_lua_file_operation_failed",
                 "skin_config.get_path did not resolve a resource");
      return false;
    }
    resolvedConfigurationPath = std::move(*resolved.normalizedVirtualPath);
    return true;
  } catch (...) {
    storeError("skin_lua_file_operation_failed",
               "skin_config.get_path failed within host limits");
    return false;
  }
}

namespace {

LuaSkinHostModulesImpl *host(lua_State *state, int upvalue = 1) {
  return static_cast<LuaSkinHostModulesImpl *>(
      lua_touserdata(state, lua_upvalueindex(upvalue)));
}

int raiseStoredError(lua_State *state, LuaSkinHostModulesImpl *impl) {
  lua_pushfstring(state, "%s%s:%s", kErrorPrefix, impl->lastErrorCode.c_str(),
                  impl->lastErrorMessage.c_str());
  return lua_error(state);
}

int expectedFailure(lua_State *state, std::string_view message) {
  lua_pushnil(state);
  lua_pushlstring(state, message.data(), message.size());
  return 2;
}

void installClosure(lua_State *state, LuaSkinHostModulesImpl *impl,
                    lua_CFunction function) {
  lua_pushlightuserdata(state, impl);
  lua_pushcclosure(state, function, 1);
}

ISkinFrameState *frameState(lua_State *state) {
  return host(state)->frameState;
}

int mainStateOption(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  if (current == nullptr) {
    return luaL_error(state, "main_state.option has no configured state");
  }
  const auto result = current->booleanProperty({.value = id});
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.option id: %d", id);
  }
  lua_pushboolean(state, result.value);
  return 1;
}

int mainStateNumber(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  if (current == nullptr) {
    return luaL_error(state, "main_state.number has no configured state");
  }
  const auto result = current->integerProperty({.value = id});
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.number id: %d", id);
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int mainStateFloatNumber(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  if (current == nullptr) {
    return luaL_error(state, "main_state.float_number has no configured state");
  }
  const auto result = current->floatProperty({.value = id});
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.float_number id: %d", id);
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int mainStateText(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  if (current == nullptr) {
    return luaL_error(state, "main_state.text has no configured state");
  }
  const auto result = current->stringProperty({.value = id});
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.text id: %d", id);
  }
  lua_pushlstring(state, result.value.data(), result.value.size());
  return 1;
}

int mainStateOffset(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  if (current == nullptr) {
    return luaL_error(state, "main_state.offset has no configured state");
  }
  const auto result = current->offsetProperty(id);
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.offset id: %d", id);
  }
  lua_createtable(state, 0, 6);
  lua_pushinteger(state, result.value.x); lua_setfield(state, -2, "x");
  lua_pushinteger(state, result.value.y); lua_setfield(state, -2, "y");
  lua_pushinteger(state, result.value.w); lua_setfield(state, -2, "w");
  lua_pushinteger(state, result.value.h); lua_setfield(state, -2, "h");
  lua_pushinteger(state, result.value.r); lua_setfield(state, -2, "r");
  lua_pushinteger(state, result.value.a); lua_setfield(state, -2, "a");
  return 1;
}

int mainStateTimer(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  const std::int64_t value = current == nullptr
                                 ? std::numeric_limits<std::int64_t>::min()
                                 : current->timerProperty({.value = id});
  lua_pushnumber(state, static_cast<lua_Number>(value));
  return 1;
}

int mainStateEventExec(lua_State *state) {
  auto *impl = host(state);
  const int count = lua_gettop(state);
  if (count < 1 || count > 3) {
    return luaL_error(
        state,
        "main_state.event_exec expects an event ID and zero to two arguments");
  }
  const int eventId = boundedIntegerArgument(state, 1, 0, false);
  std::array<int, 2> arguments{};
  for (int index = 2; index <= count; ++index) {
    arguments[static_cast<std::size_t>(index - 2)] =
        boundedIntegerArgument(state, index, 0, false);
  }
  if (!impl->eventExecutor) {
    impl->storeError("skin_lua_event_executor_unavailable",
                     "main_state.event_exec has no active frame executor");
    return raiseStoredError(state, impl);
  }

  LuaSkinEventExecutionResult result;
  try {
    result = impl->eventExecutor.execute(
        impl->eventExecutor.context, eventId,
        std::span<const int>{arguments.data(),
                             static_cast<std::size_t>(count - 1)});
  } catch (...) {
    impl->storeError("skin_lua_event_execution_failed",
                     "main_state.event_exec failed within host limits");
    return raiseStoredError(state, impl);
  }
  if (!result.ok()) {
    impl->storeError(
        result.failure->code.empty() ? "skin_lua_event_execution_failed"
                                     : result.failure->code,
        result.failure->message.empty() ? "main_state.event_exec was rejected"
                                        : result.failure->message,
        result.failure->virtualPath);
    return raiseStoredError(state, impl);
  }
  lua_pushboolean(state, 1);
  return 1;
}

int pushNamedInteger(lua_State *state, std::string_view name) {
  auto *current = frameState(state);
  if (current == nullptr) {
    return luaL_error(state, "main_state.%.*s has no configured state",
                      static_cast<int>(name.size()), name.data());
  }
  const auto result = current->integerProperty(
      {.value = std::string{name}});
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.%.*s",
                      static_cast<int>(name.size()), name.data());
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int pushNamedFloat(lua_State *state, std::string_view name) {
  auto *current = frameState(state);
  if (current == nullptr) {
    return luaL_error(state, "main_state.%.*s has no configured state",
                      static_cast<int>(name.size()), name.data());
  }
  const auto result = current->floatProperty(
      {.value = std::string{name}});
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.%.*s",
                      static_cast<int>(name.size()), name.data());
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int mainStateEventIndex(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  if (current == nullptr) {
    return luaL_error(state, "main_state.event_index has no configured state");
  }
  const auto result = current->integerProperty({.value = id});
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.event_index id: %d", id);
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int mainStateExscore(lua_State *state) {
  return pushNamedInteger(state, "exscore");
}

int mainStateGauge(lua_State *state) {
  auto *current = frameState(state);
  const auto gauge = current == nullptr ? SkinGaugeStateView{}
                                         : current->gaugeState();
  if (!gauge.supported) {
    return luaL_error(state, "main_state.gauge has no configured state");
  }
  lua_pushnumber(state, static_cast<lua_Number>(gauge.value));
  return 1;
}

int mainStateGaugeType(lua_State *state) {
  auto *current = frameState(state);
  const auto gauge = current == nullptr ? SkinGaugeStateView{}
                                         : current->gaugeState();
  if (!gauge.supported) {
    return luaL_error(state, "main_state.gauge_type has no configured state");
  }
  lua_pushnumber(state, static_cast<lua_Number>(gauge.gaugeType));
  return 1;
}

int mainStateJudge(lua_State *state) {
  const int judge = boundedIntegerArgument(state, 1, 0, false);
  return pushNamedInteger(state, "judge:" + std::to_string(judge));
}

int mainStateRate(lua_State *state) {
  return pushNamedFloat(state, "rate");
}

int mainStateTime(lua_State *state) {
  return pushNamedInteger(state, "time");
}

int mainStateVolumeBg(lua_State *state) {
  return pushNamedFloat(state, "volume_bg");
}

int mainStateVolumeKey(lua_State *state) {
  return pushNamedFloat(state, "volume_key");
}

int mainStateVolumeSys(lua_State *state) {
  return pushNamedFloat(state, "volume_sys");
}

void populateMainState(lua_State *state, LuaSkinHostModulesImpl *impl) {
  lua_getglobal(state, "main_state");
  if (!lua_istable(state, -1)) {
    luaL_error(state, "main_state compatibility table is unavailable");
  }
  installClosure(state, impl, mainStateOption); lua_setfield(state, -2, "option");
  installClosure(state, impl, mainStateNumber); lua_setfield(state, -2, "number");
  installClosure(state, impl, mainStateFloatNumber); lua_setfield(state, -2, "float_number");
  installClosure(state, impl, mainStateText); lua_setfield(state, -2, "text");
  installClosure(state, impl, mainStateOffset); lua_setfield(state, -2, "offset");
  installClosure(state, impl, mainStateTimer); lua_setfield(state, -2, "timer");
  lua_pushnumber(state, static_cast<lua_Number>(std::numeric_limits<std::int64_t>::min()));
  lua_setfield(state, -2, "timer_off_value");
  installClosure(state, impl, mainStateEventExec); lua_setfield(state, -2, "event_exec");
  installClosure(state, impl, mainStateEventIndex); lua_setfield(state, -2, "event_index");
  installClosure(state, impl, mainStateExscore); lua_setfield(state, -2, "exscore");
  installClosure(state, impl, mainStateGauge); lua_setfield(state, -2, "gauge");
  installClosure(state, impl, mainStateGaugeType); lua_setfield(state, -2, "gauge_type");
  installClosure(state, impl, mainStateJudge); lua_setfield(state, -2, "judge");
  installClosure(state, impl, mainStateRate); lua_setfield(state, -2, "rate");
  installClosure(state, impl, mainStateTime); lua_setfield(state, -2, "time");
  installClosure(state, impl, mainStateVolumeBg); lua_setfield(state, -2, "volume_bg");
  installClosure(state, impl, mainStateVolumeKey); lua_setfield(state, -2, "volume_key");
  installClosure(state, impl, mainStateVolumeSys); lua_setfield(state, -2, "volume_sys");
  lua_pop(state, 1);
}

int installStateAccessors(lua_State *state) {
  auto *impl = static_cast<LuaSkinHostModulesImpl *>(lua_touserdata(state, 1));
  populateMainState(state, impl);
  return 0;
}

SharedLuaFileHandle &checkedHandle(lua_State *state, int index) {
  return *static_cast<SharedLuaFileHandle *>(
      luaL_checkudata(state, index, kHandleMetatable));
}

bool guardHandle(LuaFileHandle &handle, SkinFileUse use) {
  const auto resolved =
      handle.owner->fileSystem->resolve(handle.virtualPath, use);
  if (resolved.failure) {
    handle.owner->storeFileError(*resolved.failure);
    return false;
  }
  if (handle.invalidated || handle.closed) {
    handle.owner->storeError("skin_lua_file_handle_invalid",
                             "Lua skin file handle is closed or invalidated",
                             handle.virtualPath);
    return false;
  }
  return true;
}

int fileHandleGc(lua_State *state) {
  auto *handle = static_cast<SharedLuaFileHandle *>(
      luaL_checkudata(state, 1, kHandleMetatable));
  handle->~SharedLuaFileHandle();
  return 0;
}

int fileLineIterator(lua_State *state) {
  SharedLuaFileHandle &shared = checkedHandle(state, lua_upvalueindex(1));
  LuaFileHandle *handle = shared.get();
  if (!guardHandle(*handle, SkinFileUse::DataRead)) {
    return raiseStoredError(state, handle->owner);
  }
  if (handle->readOffset >= handle->bytes.size()) {
    return 0;
  }
  std::size_t end = handle->readOffset;
  while (end < handle->bytes.size() &&
         handle->bytes[end] != static_cast<std::byte>('\n')) {
    ++end;
  }
  std::size_t contentEnd = end;
  if (contentEnd > handle->readOffset &&
      handle->bytes[contentEnd - 1] == static_cast<std::byte>('\r')) {
    --contentEnd;
  }
  lua_pushlstring(
      state,
      reinterpret_cast<const char *>(handle->bytes.data() + handle->readOffset),
      contentEnd - handle->readOffset);
  handle->readOffset = end < handle->bytes.size() ? end + 1 : end;
  return 1;
}

int fileLines(lua_State *state) {
  luaL_argcheck(state, lua_gettop(state) == 1, 1, "lines accepts no arguments");
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (handle->mode != HandleMode::Read) {
    handle->owner->storeError("skin_lua_file_mode_denied",
                              "lines is available only on read handles",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  if (!guardHandle(*handle, SkinFileUse::DataRead)) {
    return raiseStoredError(state, handle->owner);
  }
  lua_pushvalue(state, 1);
  lua_pushcclosure(state, fileLineIterator, 1);
  return 1;
}

int fileWrite(lua_State *state) {
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (handle->mode == HandleMode::Read) {
    handle->owner->storeError("skin_lua_file_mode_denied",
                              "write is unavailable on a read handle",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  if (!guardHandle(*handle, SkinFileUse::DataWrite)) {
    return raiseStoredError(state, handle->owner);
  }

  bool accepted = true;
  {
    std::vector<std::byte> addition;
    try {
      for (int index = 2; index <= lua_gettop(state); ++index) {
        std::size_t size = 0;
        const char *text = lua_tolstring(state, index, &size);
        if (text == nullptr ||
            size > LuaSkinHostPolicy::maxDataReadBytes - addition.size()) {
          accepted = false;
          break;
        }
        const auto *begin = reinterpret_cast<const std::byte *>(text);
        addition.insert(addition.end(), begin, begin + size);
      }
      if (accepted && addition.size() <= LuaSkinHostPolicy::maxDataReadBytes -
                                             handle->bytes.size() &&
          handle->owner->reserveHandleBytes(addition.size())) {
        handle->bytes.insert(handle->bytes.end(), addition.begin(),
                             addition.end());
        handle->owner->openHandleBytes += addition.size();
        handle->accountedBytes += addition.size();
      } else {
        accepted = false;
      }
    } catch (...) {
      accepted = false;
    }
  }
  if (!accepted) {
    handle->owner->storeError("skin_lua_host_limit_exceeded",
                              "Lua skin write buffer exceeds its limit",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  handle->dirty = true;
  lua_pushvalue(state, 1);
  return 1;
}

bool commitHandle(LuaFileHandle &handle) {
  const auto result = handle.owner->fileSystem->writeData(
      handle.virtualPath, handle.bytes, handle.mode == HandleMode::Append);
  if (!result.failure) {
    return true;
  }
  handle.owner->storeFileError(*result.failure);
  return false;
}

int fileClose(lua_State *state) {
  luaL_argcheck(state, lua_gettop(state) == 1, 1, "close accepts no arguments");
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (handle->closed && !handle->invalidated) {
    lua_pushboolean(state, 1);
    return 1;
  }
  const SkinFileUse use = handle->mode == HandleMode::Read
                              ? SkinFileUse::DataRead
                              : SkinFileUse::DataWrite;
  if (!guardHandle(*handle, use)) {
    return raiseStoredError(state, handle->owner);
  }

  const bool committed =
      handle->mode == HandleMode::Read || commitHandle(*handle);
  if (!committed) {
    return raiseStoredError(state, handle->owner);
  }
  std::vector<std::byte>().swap(handle->bytes);
  handle->closed = true;
  handle->dirty = false;
  handle->owner->releaseHandle(*handle);
  lua_pushboolean(state, 1);
  return 1;
}

int ioOpen(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  std::size_t pathSize = 0;
  const char *path = luaL_checklstring(state, 1, &pathSize);
  std::size_t modeSize = 0;
  const char *mode = luaL_optlstring(state, 2, "r", &modeSize);
  if (lua_gettop(state) > 2) {
    return expectedFailure(state, "io.open accepts only path and mode");
  }
  HandleMode selectedMode = HandleMode::Read;
  if (modeSize == 1 && mode[0] == 'r') {
    selectedMode = HandleMode::Read;
  } else if (modeSize == 1 && mode[0] == 'w') {
    selectedMode = HandleMode::Write;
  } else if (modeSize == 1 && mode[0] == 'a') {
    selectedMode = HandleMode::Append;
  } else {
    return expectedFailure(state, "Lua skin file mode is not allowed");
  }
  if (selectedMode != HandleMode::Read && !impl->allowOverlayWrites) {
    return expectedFailure(state, "Lua skin overlay writes are not allowed");
  }
  std::erase_if(impl->handles, [](const auto &weak) {
    const auto handle = weak.lock();
    return !handle || handle->closed;
  });
  if (impl->openHandleCount >= LuaSkinHostPolicy::maxOpenHandles) {
    return expectedFailure(state, "Lua skin file handle quota is exhausted");
  }

  bool ready = false;
  std::vector<std::byte> bytes;
  if (selectedMode == HandleMode::Read) {
    SkinFileReadResult read;
    {
      read = impl->fileSystem->read(std::string_view(path, pathSize),
                                    SkinFileUse::DataRead,
                                    LuaSkinHostPolicy::maxDataReadBytes);
    }
    if (read.failure) {
      if (read.failure->code == SkinFileError::RenderPhase) {
        impl->storeFileError(*read.failure);
        return raiseStoredError(state, impl);
      }
      return expectedFailure(state, read.failure->message);
    }
    bytes = std::move(read.bytes);
    ready = true;
  } else {
    const auto resolved = impl->fileSystem->resolve(
        std::string_view(path, pathSize), SkinFileUse::DataWrite);
    if (resolved.failure) {
      if (resolved.failure->code == SkinFileError::RenderPhase) {
        impl->storeFileError(*resolved.failure);
        return raiseStoredError(state, impl);
      }
      return expectedFailure(state, resolved.failure->message);
    }
    ready = true;
  }
  if (!ready) {
    return expectedFailure(state, "Lua skin file could not be opened");
  }
  if (!impl->reserveHandleBytes(bytes.size())) {
    return expectedFailure(state, "Lua skin aggregate file buffer quota is exhausted");
  }

  try {
    auto handle = std::make_shared<LuaFileHandle>();
    handle->owner = impl;
    handle->virtualPath.assign(path, pathSize);
    handle->mode = selectedMode;
    handle->bytes = std::move(bytes);
    handle->accountedBytes = handle->bytes.size();
    handle->dirty = selectedMode == HandleMode::Write;
    void *storage = lua_newuserdata(state, sizeof(SharedLuaFileHandle));
    new (storage) SharedLuaFileHandle(handle);
    luaL_getmetatable(state, kHandleMetatable);
    lua_setmetatable(state, -2);
    impl->handles.push_back(handle);
    handle->holdsQuota = true;
    ++impl->openHandleCount;
    impl->openHandleBytes += handle->accountedBytes;
  } catch (...) {
    return expectedFailure(state, "Lua skin file handle could not allocate");
  }
  return 1;
}

int textLoader(lua_State *state) {
  std::size_t size = 0;
  const char *text = luaL_checklstring(state, 1, &size);
  const char *name = luaL_optstring(state, 2, "=(skin-load)");
  if (size > LuaSkinHostPolicy::maxTextChunkBytes ||
      (size > 0 && static_cast<unsigned char>(text[0]) == 0x1b)) {
    lua_pushnil(state);
    lua_pushliteral(state, "binary or oversized Lua chunk is not allowed");
    return 2;
  }
  const int status = luaL_loadbuffer(state, text, size, name);
  if (status != 0) {
    lua_pushnil(state);
    lua_insert(state, -2);
    return 2;
  }
  return 1;
}

int doFile(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  luaL_argcheck(state, lua_gettop(state) == 1, 1,
                "dofile accepts exactly one path");
  std::size_t pathSize = 0;
  const char *path = luaL_checklstring(state, 1, &pathSize);
  int loadStatus = LUA_ERRFILE;
  {
    const auto read = impl->fileSystem->read(
        std::string_view(path, pathSize), SkinFileUse::LuaModule,
        LuaSkinHostPolicy::maxTextChunkBytes);
    if (read.failure) {
      impl->storeFileError(*read.failure);
    } else {
      std::string chunkName = "@" + std::string(path, pathSize);
      loadStatus = luaL_loadbuffer(
          state, reinterpret_cast<const char *>(read.bytes.data()),
          read.bytes.size(), chunkName.c_str());
    }
  }
  if (loadStatus == LUA_ERRFILE) {
    return raiseStoredError(state, impl);
  }
  if (loadStatus != 0) {
    return lua_error(state);
  }
  lua_remove(state, 1);
  lua_call(state, 0, LUA_MULTRET);
  return lua_gettop(state);
}

int moduleLoader(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  std::size_t nameSize = 0;
  const char *name = luaL_checklstring(state, 1, &nameSize);
  int loadStatus = LUA_ERRFILE;
  bool missing = false;
  {
    const auto read = impl->fileSystem->readModule(
        std::string_view(name, nameSize), LuaSkinHostPolicy::maxTextChunkBytes);
    if (read.failure) {
      missing = read.failure->code == SkinFileError::Missing;
      if (!missing) {
        impl->storeFileError(*read.failure);
      }
    } else {
      std::string chunkName = "@module:" + std::string(name, nameSize);
      loadStatus = luaL_loadbuffer(
          state, reinterpret_cast<const char *>(read.bytes.data()),
          read.bytes.size(), chunkName.c_str());
    }
  }
  if (missing) {
    lua_pushfstring(state, "\n\tvirtual module '%s' was not found", name);
    return 1;
  }
  if (loadStatus == LUA_ERRFILE) {
    return raiseStoredError(state, impl);
  }
  if (loadStatus != 0) {
    return lua_error(state);
  }
  return 1;
}

bool sameUpvalueTable(lua_State *state, int argument, int upvalue) {
  return lua_istable(state, argument) &&
         lua_rawequal(state, argument, lua_upvalueindex(upvalue));
}

enum class LegacyListStatus : std::uint8_t {
  Success,
  OrdinaryFailure,
  RenderDenied,
};

LegacyListStatus pushLegacyList(lua_State *state, LuaSkinHostModulesImpl &impl,
                                std::string_view path) {
  const auto listed =
      impl.fileSystem->list(path, {}, LuaSkinHostPolicy::maxDirectoryEntries);
  if (listed.failure) {
    if (listed.failure->code == SkinFileError::RenderPhase) {
      impl.storeFileError(*listed.failure);
      return LegacyListStatus::RenderDenied;
    }
    return LegacyListStatus::OrdinaryFailure;
  }
  lua_createtable(state, static_cast<int>(listed.entries.size()), 0);
  int index = 1;
  for (const auto &entry : listed.entries) {
    lua_pushlstring(state, entry.data(), entry.size());
    lua_rawseti(state, -2, index++);
  }
  return LegacyListStatus::Success;
}

int legacyListFiles(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (lua_gettop(state) != 1 || !sameUpvalueTable(state, 1, 3)) {
    impl->reportLegacyDenial("java.io.File.listFiles");
    return raiseStoredError(state, impl);
  }
  std::size_t pathSize = 0;
  const char *path = lua_tolstring(state, lua_upvalueindex(2), &pathSize);
  const LegacyListStatus status =
      pushLegacyList(state, *impl, std::string_view(path, pathSize));
  if (status == LegacyListStatus::RenderDenied) {
    return raiseStoredError(state, impl);
  }
  if (status == LegacyListStatus::OrdinaryFailure) {
    return 0;
  }
  return 1;
}

std::pair<bool, bool> performLegacyMkdir(LuaSkinHostModulesImpl &impl,
                                         std::string_view path) {
  if (!impl.allowOverlayWrites) {
    return {false, false};
  }
  const auto result = impl.fileSystem->mkdirData(path);
  if (!result.failure) {
    return {true, false};
  }
  if (result.failure->code == SkinFileError::RenderPhase) {
    impl.storeFileError(*result.failure);
    return {false, true};
  }
  return {false, false};
}

int legacyMkdir(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (lua_gettop(state) != 1 || !sameUpvalueTable(state, 1, 3)) {
    impl->reportLegacyDenial("java.io.File.mkdir");
    return raiseStoredError(state, impl);
  }
  std::size_t pathSize = 0;
  const char *path = lua_tolstring(state, lua_upvalueindex(2), &pathSize);
  const auto [created, renderDenied] =
      performLegacyMkdir(*impl, std::string_view(path, pathSize));
  if (renderDenied) {
    return raiseStoredError(state, impl);
  }
  lua_pushboolean(state, created);
  return 1;
}

int legacyFileMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("java.io.File.member");
  return raiseStoredError(state, impl);
}

int legacyGdxMember(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (lua_type(state, 2) == LUA_TSTRING) {
    std::size_t keySize = 0;
    const char *key = lua_tolstring(state, 2, &keySize);
    if (std::string_view(key, keySize) == "app") {
      return 0;
    }
  }
  impl->reportLegacyDenial("com.badlogic.gdx.Gdx.member");
  return raiseStoredError(state, impl);
}

void installClosedMemberMetatable(lua_State *state,
                                  LuaSkinHostModulesImpl *impl,
                                  lua_CFunction missingMember) {
  lua_createtable(state, 0, 2);
  installClosure(state, impl, missingMember);
  lua_setfield(state, -2, "__index");
  lua_pushboolean(state, 0);
  lua_setfield(state, -2, "__metatable");
  lua_setmetatable(state, -2);
}

void pushLegacyFileObject(lua_State *state, LuaSkinHostModulesImpl *impl,
                          const char *path, std::size_t pathSize) {
  lua_createtable(state, 0, 2);
  const int tableIndex = lua_gettop(state);

  lua_pushlightuserdata(state, impl);
  lua_pushlstring(state, path, pathSize);
  lua_pushvalue(state, tableIndex);
  lua_pushcclosure(state, legacyListFiles, 3);
  lua_setfield(state, tableIndex, "listFiles");

  lua_pushlightuserdata(state, impl);
  lua_pushlstring(state, path, pathSize);
  lua_pushvalue(state, tableIndex);
  lua_pushcclosure(state, legacyMkdir, 3);
  lua_setfield(state, tableIndex, "mkdir");

  installClosedMemberMetatable(state, impl, legacyFileMemberDenied);
}

int legacyBindClass(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING) {
    impl->reportLegacyDenial("bindClass");
    return raiseStoredError(state, impl);
  }
  std::size_t size = 0;
  const char *name = lua_tolstring(state, 1, &size);
  const std::string_view requested(name, size);
  if (requested == "java.io.File") {
    lua_rawgeti(state, LUA_REGISTRYINDEX, impl->fileTokenReference);
    return 1;
  }
  if (requested == "com.badlogic.gdx.Gdx") {
    lua_rawgeti(state, LUA_REGISTRYINDEX, impl->gdxTokenReference);
    return 1;
  }
  impl->reportLegacyDenial(requested);
  return raiseStoredError(state, impl);
}

int legacyNew(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  bool authorized = lua_gettop(state) == 2 && lua_istable(state, 1) &&
                    lua_type(state, 2) == LUA_TSTRING;
  if (authorized) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, impl->fileTokenReference);
    authorized = lua_rawequal(state, 1, -1);
    lua_pop(state, 1);
  }
  if (!authorized) {
    impl->reportLegacyDenial("java.io.File.constructor");
    return raiseStoredError(state, impl);
  }
  std::size_t pathSize = 0;
  const char *path = lua_tolstring(state, 2, &pathSize);
  pushLegacyFileObject(state, impl, path, pathSize);
  return 1;
}

int coroutineCreate(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  luaL_checktype(state, 1, LUA_TFUNCTION);
  luaL_argcheck(state, lua_gettop(state) == 1, 1,
                "create accepts exactly one function");
  lua_State *thread = lua_newthread(state);
  lua_pushvalue(state, 1);
  lua_xmove(state, thread, 1);
  if (impl->coroutineCreated != nullptr) {
    impl->coroutineCreated(impl->coroutineContext, thread);
  }
  return 1;
}

int resumeThread(lua_State *state, lua_State *thread, int firstArgument) {
  const int count = lua_gettop(state) - firstArgument + 1;
  for (int index = firstArgument; index <= lua_gettop(state); ++index) {
    lua_pushvalue(state, index);
  }
  lua_xmove(state, thread, count);
  const int status = lua_resume(thread, count);
  const int results = lua_gettop(thread);
  if (status == 0 || status == LUA_YIELD) {
    lua_pushboolean(state, 1);
    lua_xmove(thread, state, results);
    return results + 1;
  }
  lua_pushboolean(state, 0);
  lua_xmove(thread, state, 1);
  return 2;
}

int coroutineResume(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  lua_State *thread = lua_tothread(state, 1);
  if (thread == nullptr) {
    return luaL_argerror(state, 1, "coroutine expected");
  }
  if (impl->coroutineCreated != nullptr) {
    impl->coroutineCreated(impl->coroutineContext, thread);
  }
  return resumeThread(state, thread, 2);
}

int wrappedCoroutine(lua_State *state) {
  lua_State *thread = lua_tothread(state, lua_upvalueindex(1));
  const int results = resumeThread(state, thread, 1);
  if (!lua_toboolean(state, -results)) {
    lua_remove(state, -results);
    return lua_error(state);
  }
  lua_remove(state, -results);
  return results - 1;
}

int coroutineWrap(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  luaL_checktype(state, 1, LUA_TFUNCTION);
  luaL_argcheck(state, lua_gettop(state) == 1, 1,
                "wrap accepts exactly one function");
  lua_State *thread = lua_newthread(state);
  lua_pushvalue(state, 1);
  lua_xmove(state, thread, 1);
  if (impl->coroutineCreated != nullptr) {
    impl->coroutineCreated(impl->coroutineContext, thread);
  }
  lua_pushcclosure(state, wrappedCoroutine, 1);
  return 1;
}

void openLibrary(lua_State *state, const char *name, lua_CFunction function) {
  lua_pushcfunction(state, function);
  lua_pushstring(state, name);
  lua_call(state, 1, 0);
}

void setNilGlobal(lua_State *state, const char *name) {
  lua_pushnil(state);
  lua_setglobal(state, name);
}

void installSafeOsLibrary(lua_State *state) {
  openLibrary(state, LUA_OSLIBNAME, luaopen_os);
  lua_getglobal(state, LUA_OSLIBNAME);
  for (const char *name :
       {"execute", "exit", "getenv", "remove", "rename", "tmpname"}) {
    lua_pushnil(state);
    lua_setfield(state, -2, name);
  }
  lua_pop(state, 1);
}

void installBit32(lua_State *state) {
  lua_createtable(state, 0, 9);
  const luaL_Reg functions[] = {{"band", bitBand},
                                {"bor", bitBor},
                                {"bxor", bitBxor},
                                {"bnot", bitBnot},
                                {"lshift", shiftLeft},
                                {"rshift", shiftRight},
                                {"arshift", arithmeticShiftRight},
                                {"extract", bitExtract},
                                {"replace", bitReplace},
                                {nullptr, nullptr}};
  luaL_register(state, nullptr, functions);
  lua_setglobal(state, "bit32");
}

void setLoaded(lua_State *state, const char *name) {
  lua_getglobal(state, "package");
  lua_getfield(state, -1, "loaded");
  lua_getglobal(state, name);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    lua_newtable(state);
  }
  lua_setfield(state, -2, name);
  lua_pop(state, 2);
}

void installFileMetatable(lua_State *state) {
  luaL_newmetatable(state, kHandleMetatable);
  lua_pushcfunction(state, fileHandleGc);
  lua_setfield(state, -2, "__gc");
  lua_pushboolean(state, 0);
  lua_setfield(state, -2, "__metatable");
  lua_createtable(state, 0, 3);
  lua_pushcfunction(state, fileLines);
  lua_setfield(state, -2, "lines");
  lua_pushcfunction(state, fileWrite);
  lua_setfield(state, -2, "write");
  lua_pushcfunction(state, fileClose);
  lua_setfield(state, -2, "close");
  lua_setfield(state, -2, "__index");
  lua_pop(state, 1);
}

int installHost(lua_State *state) {
  auto *impl = static_cast<LuaSkinHostModulesImpl *>(lua_touserdata(state, 1));
  openLibrary(state, "", luaopen_base);
  openLibrary(state, LUA_LOADLIBNAME, luaopen_package);
  openLibrary(state, LUA_TABLIBNAME, luaopen_table);
  openLibrary(state, LUA_STRLIBNAME, luaopen_string);
  openLibrary(state, LUA_MATHLIBNAME, luaopen_math);
  installSafeOsLibrary(state);

  for (const char *name : {"ffi", "jit", "debug", "bit"}) {
    setNilGlobal(state, name);
  }
  for (const char *name :
       {"collectgarbage", "gcinfo", "newproxy", "module", "loadfile"}) {
    setNilGlobal(state, name);
  }

  installClosure(state, impl, doFile);
  lua_setglobal(state, "dofile");
  installClosure(state, impl, textLoader);
  lua_setglobal(state, "load");
  installClosure(state, impl, textLoader);
  lua_setglobal(state, "loadstring");

  lua_getglobal(state, "string");
  lua_pushnil(state);
  lua_setfield(state, -2, "dump");
  lua_pop(state, 1);

  lua_getglobal(state, "package");
  lua_pushnil(state);
  lua_setfield(state, -2, "searchers");
  lua_pushnil(state);
  lua_setfield(state, -2, "loadlib");
  lua_pushnil(state);
  lua_setfield(state, -2, "cpath");
  lua_pushliteral(state, "?.lua;?/init.lua");
  lua_setfield(state, -2, "path");
  lua_createtable(state, 1, 0);
  installClosure(state, impl, moduleLoader);
  lua_rawseti(state, -2, 1);
  lua_setfield(state, -2, "loaders");
  lua_pop(state, 1);

  installBit32(state);
  installFileMetatable(state);
  lua_newtable(state);
  lua_setglobal(state, "main_state");

  lua_createtable(state, 0, 1);
  installClosure(state, impl, ioOpen);
  lua_setfield(state, -2, "open");
  lua_setglobal(state, "io");

  lua_getglobal(state, "coroutine");
  installClosure(state, impl, coroutineCreate);
  lua_setfield(state, -2, "create");
  installClosure(state, impl, coroutineResume);
  lua_setfield(state, -2, "resume");
  installClosure(state, impl, coroutineWrap);
  lua_setfield(state, -2, "wrap");
  lua_pop(state, 1);

  lua_newtable(state);
  installClosedMemberMetatable(state, impl, legacyFileMemberDenied);
  impl->fileTokenReference = luaL_ref(state, LUA_REGISTRYINDEX);
  lua_newtable(state);
  installClosedMemberMetatable(state, impl, legacyGdxMember);
  impl->gdxTokenReference = luaL_ref(state, LUA_REGISTRYINDEX);

  lua_createtable(state, 0, 2);
  installClosure(state, impl, legacyBindClass);
  lua_setfield(state, -2, "bindClass");
  installClosure(state, impl, legacyNew);
  lua_setfield(state, -2, "new");
  lua_pushvalue(state, -1);
  lua_setglobal(state, "luajava");
  lua_getglobal(state, "package");
  lua_getfield(state, -1, "loaded");
  lua_pushvalue(state, -3);
  lua_setfield(state, -2, "luajava");
  lua_pop(state, 3);

  setLoaded(state, "main_state");
  setLoaded(state, "timer_util");
  setLoaded(state, "event_util");
  return 0;
}

void pushOffset(lua_State *state, const ConfigOffset &offset) {
  lua_createtable(state, 0, 6);
  lua_pushinteger(state, offset.x);
  lua_setfield(state, -2, "x");
  lua_pushinteger(state, offset.y);
  lua_setfield(state, -2, "y");
  lua_pushinteger(state, offset.w);
  lua_setfield(state, -2, "w");
  lua_pushinteger(state, offset.h);
  lua_setfield(state, -2, "h");
  lua_pushinteger(state, offset.r);
  lua_setfield(state, -2, "r");
  lua_pushinteger(state, offset.a);
  lua_setfield(state, -2, "a");
}

int configuredGetPath(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (lua_gettop(state) != 1) {
    impl->storeError("skin_lua_file_operation_failed",
                     "skin_config.get_path accepts exactly one path");
    return raiseStoredError(state, impl);
  }
  std::size_t requestSize = 0;
  const char *request = luaL_checklstring(state, 1, &requestSize);
  if (!impl->resolveConfiguredPath(std::string_view(request, requestSize))) {
    return raiseStoredError(state, impl);
  }
  lua_pushlstring(state, impl->resolvedConfigurationPath.data(),
                  impl->resolvedConfigurationPath.size());
  return 1;
}

int installPendingConfiguration(lua_State *state) {
  auto *impl = static_cast<LuaSkinHostModulesImpl *>(lua_touserdata(state, 1));
  const auto &configuration = *impl->pendingConfiguration;
  lua_createtable(state, 0, 5);

  installClosure(state, impl, configuredGetPath);
  lua_setfield(state, -2, "get_path");

  lua_createtable(state, 0, static_cast<int>(configuration.filePaths.size()));
  for (const auto &[name, path] : configuration.filePaths) {
    lua_pushlstring(state, path.data(), path.size());
    lua_setfield(state, -2, name.c_str());
  }
  lua_setfield(state, -2, "file_path");

  lua_createtable(state, 0, static_cast<int>(configuration.options.size()));
  for (const auto &option : configuration.orderedOptions) {
    lua_pushinteger(state, option.value);
    lua_setfield(state, -2, option.name.c_str());
  }
  for (const auto &[name, value] : configuration.options) {
    lua_pushinteger(state, value);
    lua_setfield(state, -2, name.c_str());
  }
  lua_setfield(state, -2, "option");

  lua_createtable(state, static_cast<int>(configuration.orderedOptions.size()),
                  0);
  int enabledIndex = 1;
  for (const auto &option : configuration.orderedOptions) {
    lua_pushinteger(state, option.value);
    lua_rawseti(state, -2, enabledIndex++);
  }
  lua_setfield(state, -2, "enabled_options");

  lua_createtable(state, 0, static_cast<int>(configuration.offsets.size()));
  for (const auto &[name, offset] : configuration.offsets) {
    pushOffset(state, offset);
    lua_setfield(state, -2, name.c_str());
  }
  lua_setfield(state, -2, "offset");
  lua_setglobal(state, "skin_config");
  return 0;
}

} // namespace

LuaSkinHostModulesCreateResult
LuaSkinHostModules::create(lua_State *state,
                           LuaSkinHostModulesOptions options) {
  if (state == nullptr || options.fileSystem == nullptr) {
    return {.failure = diagnostic("skin_lua_runtime_create_failed",
                                  "Lua host requires a state and filesystem")};
  }
  std::unique_ptr<LuaSkinHostModulesImpl> impl;
  try {
    impl = std::make_unique<LuaSkinHostModulesImpl>();
  } catch (...) {
    return {.failure = diagnostic("skin_lua_runtime_create_failed",
                                  "Lua host allocation failed")};
  }
  impl->state = state;
  impl->fileSystem = options.fileSystem;
  impl->allowOverlayWrites = options.allowOverlayWrites;
  impl->coroutineContext = options.coroutineContext;
  impl->coroutineCreated = options.coroutineCreated;
  if (lua_cpcall(state, installHost, impl.get()) != 0) {
    const char *message = lua_tostring(state, -1);
    auto failure =
        diagnostic("skin_lua_runtime_create_failed",
                   message != nullptr ? message : "Lua host install failed");
    lua_pop(state, 1);
    return {.failure = std::move(failure)};
  }
  try {
    return {.modules = std::unique_ptr<LuaSkinHostModules>(
                new LuaSkinHostModules(std::move(impl)))};
  } catch (...) {
    return {.failure = diagnostic("skin_lua_runtime_create_failed",
                                  "Lua host wrapper allocation failed")};
  }
}

LuaSkinHostModules::LuaSkinHostModules(
    std::unique_ptr<LuaSkinHostModulesImpl> impl) noexcept
    : impl_(std::move(impl)) {}

LuaSkinHostModules::~LuaSkinHostModules() = default;

std::optional<SkinDiagnostic> LuaSkinHostModules::installConfiguration(
    const BeatorajaSkinConfiguration &configuration) {
  if (!impl_->copyConfiguredFiles(configuration.orderedFiles)) {
    return diagnostic("skin_lua_configuration_export_failed",
                      impl_->lastErrorMessage);
  }
  impl_->pendingConfiguration = &configuration;
  const int status =
      lua_cpcall(impl_->state, installPendingConfiguration, impl_.get());
  impl_->pendingConfiguration = nullptr;
  if (status == 0) {
    return std::nullopt;
  }
  const char *message = lua_tostring(impl_->state, -1);
  auto failure =
      diagnostic("skin_lua_configuration_export_failed",
                 message != nullptr ? message : "Lua configuration failed");
  lua_pop(impl_->state, 1);
  return failure;
}

std::optional<SkinDiagnostic> LuaSkinHostModules::enableStateAccessors() {
  if (lua_cpcall(impl_->state, installStateAccessors, impl_.get()) == 0) {
    return std::nullopt;
  }
  const char *message = lua_tostring(impl_->state, -1);
  auto failure = diagnostic(
      "skin_lua_state_install_failed",
      message != nullptr ? message : "Lua state-accessor installation failed");
  lua_pop(impl_->state, 1);
  return failure;
}

void LuaSkinHostModules::setFrameState(ISkinFrameState *state) noexcept {
  if (impl_) {
    impl_->frameState = state;
  }
}

void LuaSkinHostModules::setEventExecutor(
    LuaSkinEventExecutor executor) noexcept {
  if (impl_) {
    impl_->eventExecutor = executor;
  }
}

LuaFileHandleInvalidationResult
LuaSkinHostModules::invalidateFileHandles() noexcept {
  LuaFileHandleInvalidationResult result;
  for (auto &weak : impl_->handles) {
    if (auto handle = weak.lock(); handle && !handle->closed) {
      result.hadDirtyWrite =
          result.hadDirtyWrite ||
          (handle->mode != HandleMode::Read && handle->dirty);
      std::vector<std::byte>().swap(handle->bytes);
      handle->readOffset = 0;
      handle->dirty = false;
      handle->invalidated = true;
      impl_->releaseHandle(*handle);
    }
  }
  std::erase_if(impl_->handles,
                [](const auto &handle) { return handle.expired(); });
  return result;
}

std::span<const SkinCompatibilityDiagnostic>
LuaSkinHostModules::diagnostics() const noexcept {
  return impl_->diagnostics.entries();
}

} // namespace skin

#endif
