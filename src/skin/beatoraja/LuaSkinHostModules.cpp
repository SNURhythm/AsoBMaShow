#include "LuaSkinHostModules.h"

#include "../LuaGameplaySkinFeature.h"
#include "../package/SkinPathPolicy.h"
#include "LuaSkinFileIo.h"
#include "LuaSkinAudioHost.h"
#include "LuaSkinLegacyInputHost.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "LuaSkinFileSystem.h"
#include "LuaSkinHttpClient.h"
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
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skin {
namespace {

constexpr const char *kHandleMetatable = "AsoBMaShow.LuaSkinFileHandle";
constexpr const char *kLegacyHttpConnectionMetatable =
    "AsoBMaShow.LegacyLuaHttpConnection";
constexpr const char *kLegacyHttpReaderMetatable =
    "AsoBMaShow.LegacyLuaHttpReader";
constexpr const char *kErrorPrefix = "@ASOBMSKIN:";
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
  case SkinFileError::LimitExceeded:
  case SkinFileError::QuotaExceeded:
    return "skin_lua_host_limit_exceeded";
  case SkinFileError::RenderPhase:
  case SkinFileError::BinaryChunk:
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
  LuaSkinHttpTransport *httpTransport = nullptr;
  LuaSkinAudioHost *audioHost = nullptr;
  LuaSkinLegacyInputHost *legacyInputHost = nullptr;
  std::unique_ptr<LuaSkinLegacyInputHost> ownedLegacyInputHost;
  std::size_t maximumSourceBytes = std::numeric_limits<std::size_t>::max();
  std::size_t maximumModuleSearchTemplates =
      std::numeric_limits<std::size_t>::max();
  bool allowProcessGlobalOperations = false;
  ISkinFrameState *frameState = nullptr;
  bool frameCallbackActive = false;
  void *coroutineContext = nullptr;
  LuaCoroutineCreatedCallback coroutineCreated = nullptr;
  LuaSkinEventExecutor eventExecutor;
  SkinCompatibilityDiagnostics diagnostics;
  int fileTokenReference = LUA_NOREF;
  int gdxTokenReference = LUA_NOREF;
  int inputTokenReference = LUA_NOREF;
  int controllersTokenReference = LUA_NOREF;
  int controllerTokenReference = LUA_NOREF;
  const BeatorajaSkinConfiguration *pendingConfiguration = nullptr;
  std::vector<ConfiguredFile> configuredFiles;
  std::string configurationPathPrefix;
  std::string initialPackagePath;
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
                authority == "java.io.File.constructor" ||
                authority == "java.net.URL.member" ||
                authority == "java.net.URL.connection.member" ||
                authority == "java.io.BufferedReader.member" ||
                authority == "java.net.URL.constructor" ||
                authority == "java.io.BufferedReader.constructor" ||
                authority == "com.badlogic.gdx.Gdx.member" ||
                authority == "com.badlogic.gdx.Input.member" ||
                authority == "com.badlogic.gdx.Controllers.member" ||
                authority == "com.badlogic.gdx.Controller.member" ||
                authority == "com.badlogic.gdx.ControllerList.member" ||
                authority == "bindClass"
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

  bool copyConfiguredFiles(const std::vector<ConfiguredFile> &) noexcept;
  bool resolveConfiguredPath(std::string_view) noexcept;
};

namespace {

struct LuaFileHandle {
  LuaSkinHostModulesImpl *owner = nullptr;
  std::string virtualPath;
  std::FILE *file = nullptr;
  std::int64_t cursor = 0;
  bool writable = false;
  bool closeOnEndOfLines = false;
  bool closed = false;
  bool invalidated = false;

  ~LuaFileHandle() {
    if (file != nullptr) {
      std::fclose(file);
    }
  }
};

} // namespace

bool LuaSkinHostModulesImpl::copyConfiguredFiles(
    const std::vector<ConfiguredFile> &files) noexcept {
  try {
    configuredFiles.clear();
    configuredFiles.reserve(files.size());
    for (const auto &file : files) {
      ConfiguredFile configured = file;
      // LuaSkinLoader stores CustomFile paths as p.getParent() + "/" +
      // the authored pattern before passing them to SkinLoader#getPath.
      configured.pattern = configurationPathPrefix + "/" + file.pattern;
      configuredFiles.push_back(std::move(configured));
    }
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
    // LuaSkinLoader passes p.getParent() + "/" + the authored request to
    // SkinLoader#getPath.  That helper returns a plain File path; it neither
    // normalizes nor validates it, and it does not make get_path itself a
    // filesystem access capability.
    std::string imagePath = configurationPathPrefix;
    imagePath.push_back('/');
    imagePath.append(request);

    for (const auto &file : configuredFiles) {
      if (!imagePath.starts_with(file.pattern)) {
        continue;
      }
      const std::size_t wildcard = imagePath.rfind('*');
      std::string result = imagePath;
      if (wildcard != std::string::npos) {
        result.erase(wildcard);
        result.append(file.selectedValue);
        result.append(imagePath.substr(file.pattern.size()));
      }
      while (result.size() > configurationPathPrefix.size() + 1 &&
             result.ends_with('/')) {
        result.pop_back();
      }
      resolvedConfigurationPath = std::move(result);
      return true;
    }

    std::string result = imagePath;
    const std::size_t wildcard = imagePath.rfind('*');
    if (wildcard != std::string::npos) {
      std::string extension = imagePath.substr(wildcard + 1);
      const std::size_t pipe = imagePath.find('|');
      if (pipe != std::string::npos) {
        extension = imagePath.substr(wildcard + 1, pipe - wildcard - 1);
        if (pipe + 1 < imagePath.size()) {
          extension += imagePath.substr(pipe + 1);
        }
      }
      const std::size_t slash = imagePath.rfind('/');
      if (slash != std::string::npos &&
          slash >= configurationPathPrefix.size()) {
        const std::string directory = imagePath.substr(
            configurationPathPrefix.size() + 1,
            slash - configurationPathPrefix.size() - 1);
        const auto listed = fileSystem->listResourceDirectory(directory);
        if (!listed.failure) {
          const auto lowercase = [](std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
              return static_cast<char>(std::tolower(c));
            });
            return value;
          };
          const std::string lowercaseExtension = lowercase(extension);
          for (const std::string &path : listed.entries) {
            const std::string lowercasePath = lowercase(path);
            if (lowercasePath.ends_with(lowercaseExtension)) {
              const std::size_t filename = path.rfind('/');
              result = imagePath.substr(0, slash + 1);
              result.append(path.substr(filename + 1));
              break;
            }
          }
        }
      }
    }
    while (result.size() > configurationPathPrefix.size() + 1 &&
           result.ends_with('/')) {
      result.pop_back();
    }
    resolvedConfigurationPath = std::move(result);
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

std::string_view audioPathArgument(lua_State *state, int index) {
  std::size_t size = 0;
  if (const char *value = lua_tolstring(state, index, &size)) {
    return {value, size};
  }
  if (lua_isnoneornil(state, index)) {
    return "nil";
  }
  if (lua_isboolean(state, index)) {
    return lua_toboolean(state, index) ? "true" : "false";
  }
  return lua_typename(state, lua_type(state, index));
}

float audioVolumeArgument(lua_State *state, int index) {
  if (lua_isnoneornil(state, index)) {
    return 1.0F;
  }
  return lua_isnumber(state, index)
             ? static_cast<float>(lua_tonumber(state, index))
             : 0.0F;
}

int returnAudioResult(lua_State *state, LuaSkinHostModulesImpl *impl,
                      LuaSkinAudioOperationResult result) {
  if (!result.ok()) {
    if (result.failure) {
      impl->storeFileError(*result.failure);
    } else {
      impl->storeError("skin_lua_audio_operation_failed",
                       "Lua skin audio operation failed");
    }
    return raiseStoredError(state, impl);
  }
  lua_pushboolean(state, 1);
  return 1;
}

int mainStateAudioPlay(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  const std::string_view path = audioPathArgument(state, 1);
  const float volume = audioVolumeArgument(state, 2);
  return returnAudioResult(
      state, impl,
      impl->audioHost->play(path, std::clamp(volume, 0.0F, 2.0F), false));
}

int mainStateAudioLoop(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  const std::string_view path = audioPathArgument(state, 1);
  const float volume = audioVolumeArgument(state, 2);
  return returnAudioResult(
      state, impl,
      impl->audioHost->play(path, std::clamp(volume, 0.0F, 2.0F), true));
}

int mainStateAudioPreload(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  return returnAudioResult(
      state, impl, impl->audioHost->preload(audioPathArgument(state, 1)));
}

int mainStateAudioStop(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  return returnAudioResult(
      state, impl, impl->audioHost->stop(audioPathArgument(state, 1)));
}

int mainStateAudioDispose(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  return returnAudioResult(
      state, impl, impl->audioHost->dispose(audioPathArgument(state, 1)));
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
  const auto result = current->integerProperty(
      {.value = id}, SkinIntegerPropertyDomain::IntegerValue);
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
  const auto result = current->floatProperty({.value = id},
                                             SkinFloatPropertyDomain::Rate);
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
  lua_pushnumber(state, result.value.x);
  lua_setfield(state, -2, "x");
  lua_pushnumber(state, result.value.y);
  lua_setfield(state, -2, "y");
  lua_pushnumber(state, result.value.w);
  lua_setfield(state, -2, "w");
  lua_pushnumber(state, result.value.h);
  lua_setfield(state, -2, "h");
  lua_pushnumber(state, result.value.r);
  lua_setfield(state, -2, "r");
  lua_pushnumber(state, result.value.a);
  lua_setfield(state, -2, "a");
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

int mainStateSetTimer(lua_State *state) {
  auto *current = frameState(state);
  const int id = boundedIntegerArgument(state, 1, 0, false);
  const auto value = static_cast<std::int64_t>(lua_tointeger(state, 2));
  if (current == nullptr || !current->setTimerProperty(id, value)) {
    return luaL_error(state,
                      "the timer cannot be changed by the selected skin");
  }
  lua_pushboolean(state, 1);
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
      {.value = std::string{name}}, SkinIntegerPropertyDomain::IntegerValue);
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.%.*s",
                      static_cast<int>(name.size()), name.data());
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int pushIntegerProperty(lua_State *state, int id, std::string_view name) {
  auto *current = frameState(state);
  if (current == nullptr) {
    return luaL_error(state, "main_state.%.*s has no configured state",
                      static_cast<int>(name.size()), name.data());
  }
  const auto result = current->integerProperty(
      {.value = id}, SkinIntegerPropertyDomain::IntegerValue);
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
      {.value = std::string{name}}, SkinFloatPropertyDomain::Rate);
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.%.*s",
                      static_cast<int>(name.size()), name.data());
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int pushFloatProperty(lua_State *state, int id,
                      SkinFloatPropertyDomain domain, std::string_view name) {
  auto *current = frameState(state);
  if (current == nullptr) {
    return luaL_error(state, "main_state.%.*s has no configured state",
                      static_cast<int>(name.size()), name.data());
  }
  const auto result =
      current->floatProperty({.value = id}, domain);
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
  const auto result = current->integerProperty(
      {.value = id}, SkinIntegerPropertyDomain::ImageIndex);
  if (!result.supported) {
    return luaL_error(state, "unsupported main_state.event_index id: %d", id);
  }
  lua_pushnumber(state, static_cast<lua_Number>(result.value));
  return 1;
}

int mainStateExscore(lua_State *state) {
  // MainStatePropertyLuaApiExporter reads ScoreDataProperty.getNowEXScore(),
  // which is IntegerPropertyFactory's current-score selector rather than the
  // result-only Lua alias.
  return pushIntegerProperty(state, 71, "exscore");
}

int mainStateGauge(lua_State *state) {
  // Beatoraja returns a gauge only for BMSPlayer and returns zero from all
  // result MainStates. Keep this distinct from the result gauge renderer.
  return pushNamedFloat(state, "lua_gauge");
}

int mainStateGaugeType(lua_State *state) {
  return pushNamedInteger(state, "lua_gauge_type");
}

int mainStateJudge(lua_State *state) {
  const int judge = boundedIntegerArgument(state, 1, 0, false);
  return pushNamedInteger(state, "judge:" + std::to_string(judge));
}

int mainStateRate(lua_State *state) {
  return pushFloatProperty(state, 1102, SkinFloatPropertyDomain::FloatValue,
                           "rate");
}

int mainStateRateBest(lua_State *state) {
  return pushFloatProperty(state, 112, SkinFloatPropertyDomain::Rate,
                           "rate_best");
}

int mainStateExscoreBest(lua_State *state) {
  return pushIntegerProperty(state, 150, "exscore_best");
}

int mainStateRateRival(lua_State *state) {
  return pushFloatProperty(state, 115, SkinFloatPropertyDomain::Rate,
                           "rate_rival");
}

int mainStateExscoreRival(lua_State *state) {
  return pushIntegerProperty(state, 121, "exscore_rival");
}

int mainStateTime(lua_State *state) { return pushNamedInteger(state, "time"); }

int mainStateVolumeBg(lua_State *state) {
  return pushFloatProperty(state, 19, SkinFloatPropertyDomain::Rate,
                           "volume_bg");
}

int mainStateVolumeKey(lua_State *state) {
  return pushFloatProperty(state, 18, SkinFloatPropertyDomain::Rate,
                           "volume_key");
}

int mainStateVolumeSys(lua_State *state) {
  return pushFloatProperty(state, 17, SkinFloatPropertyDomain::Rate,
                           "volume_sys");
}

std::string luaToJString(lua_State *state, int index) {
  std::size_t size = 0;
  if (const char *value = lua_tolstring(state, index, &size)) {
    return {value, size};
  }
  switch (lua_type(state, index)) {
  case LUA_TNIL:
  case LUA_TNONE:
    return "nil";
  case LUA_TBOOLEAN:
    return lua_toboolean(state, index) ? "true" : "false";
  default:
    return luaL_typename(state, index);
  }
}

bool validUtf8FileContents(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7f) {
      ++index;
      continue;
    }
    std::size_t continuationCount = 0;
    std::uint32_t codePoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuationCount = 1;
      codePoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuationCount = 2;
      codePoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuationCount = 3;
      codePoint = first & 0x07;
    } else {
      return false;
    }
    if (continuationCount >= value.size() - index) {
      return false;
    }
    for (std::size_t continuation = 1; continuation <= continuationCount;
         ++continuation) {
      const auto byte = static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0) != 0x80) {
        return false;
      }
      codePoint = (codePoint << 6) | (byte & 0x3f);
    }
    if ((continuationCount == 2 && codePoint < 0x800) ||
        (continuationCount == 3 && codePoint < 0x10000) ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff) ||
        codePoint > 0x10ffff) {
      return false;
    }
    index += continuationCount + 1;
  }
  return true;
}

std::vector<std::string_view> utf8FileLines(std::string_view contents) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < contents.size()) {
    const std::size_t separator = contents.find_first_of("\r\n", start);
    if (separator == std::string_view::npos) {
      lines.push_back(contents.substr(start));
      break;
    }
    lines.push_back(contents.substr(start, separator - start));
    start = separator + 1;
    if (contents[separator] == '\r' && start < contents.size() &&
        contents[start] == '\n') {
      ++start;
    }
  }
  return lines;
}

int mainStateFileExists(lua_State *state) {
  auto *impl = host(state);
  const std::string path = luaToJString(state, 1);
  const auto result = impl->fileSystem->exists(path);
  if (result.failure) {
    impl->storeFileError(*result.failure);
    return raiseStoredError(state, impl);
  }
  lua_pushboolean(state, result.exists);
  return 1;
}

int mainStateFileMkdir(lua_State *state) {
  auto *impl = host(state);
  const std::string path = luaToJString(state, 1);
  const auto result = impl->fileSystem->mkdirData(path, true);
  lua_pushboolean(state, !result.failure);
  return 1;
}

int mainStateFileList(lua_State *state) {
  auto *impl = host(state);
  if (!lua_isstring(state, 1)) {
    lua_pushliteral(state, "");
    lua_pushinteger(state, 0);
    return 2;
  }
  std::size_t pathSize = 0;
  const char *path = lua_tolstring(state, 1, &pathSize);
  std::string pattern;
  if (lua_gettop(state) >= 2 && !lua_isnil(state, 2)) {
    pattern = luaToJString(state, 2);
  }
  const auto result = impl->fileSystem->list(
      std::string_view(path, pathSize), pattern,
      std::numeric_limits<std::size_t>::max());
  if (result.failure) {
    lua_pushliteral(state, "");
    lua_pushinteger(state, 0);
    return 2;
  }
  luaL_Buffer buffer;
  luaL_buffinit(state, &buffer);
  for (const auto &entry : result.entries) {
    luaL_addlstring(&buffer, entry.data(), entry.size());
    luaL_addchar(&buffer, '\n');
  }
  luaL_pushresult(&buffer);
  lua_pushinteger(state, static_cast<lua_Integer>(result.entries.size()));
  return 2;
}

int mainStateFileReadLines(lua_State *state) {
  auto *impl = host(state);
  const std::string path = luaToJString(state, 1);
  const auto read = impl->fileSystem->read(
      path, SkinFileUse::DataRead, impl->maximumSourceBytes);
  lua_newtable(state);
  if (read.failure) {
    return 1;
  }
  const std::string_view contents(
      reinterpret_cast<const char *>(read.bytes.data()), read.bytes.size());
  if (!validUtf8FileContents(contents)) {
    return 1;
  }
  const auto lines = utf8FileLines(contents);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    lua_pushlstring(state, lines[index].data(), lines[index].size());
    lua_rawseti(state, -2, static_cast<int>(index + 1));
  }
  return 1;
}

int writeMainStateFile(lua_State *state, bool append) {
  auto *impl = host(state);
  const std::string path = luaToJString(state, 1);
  const std::string text = luaToJString(state, 2);
  const auto result = impl->fileSystem->writeData(
      path, std::as_bytes(std::span(text.data(), text.size())), append);
  lua_pushboolean(state, !result.failure);
  return 1;
}

int mainStateFileWrite(lua_State *state) {
  return writeMainStateFile(state, false);
}

int mainStateFileAppend(lua_State *state) {
  return writeMainStateFile(state, true);
}

int mainStateFileClear(lua_State *state) {
  auto *impl = host(state);
  const std::string path = luaToJString(state, 1);
  const auto result = impl->fileSystem->writeData(path, {}, false);
  lua_pushboolean(state, !result.failure);
  return 1;
}

int mainStateFileCountLines(lua_State *state) {
  auto *impl = host(state);
  const std::string path = luaToJString(state, 1);
  const auto read = impl->fileSystem->read(
      path, SkinFileUse::DataRead, impl->maximumSourceBytes);
  if (read.failure) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const std::string_view contents(
      reinterpret_cast<const char *>(read.bytes.data()), read.bytes.size());
  if (!validUtf8FileContents(contents)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  lua_pushinteger(state,
                  static_cast<lua_Integer>(utf8FileLines(contents).size()));
  return 1;
}

int pushHttpFailure(lua_State *state, std::string_view message) {
  lua_pushnil(state);
  lua_pushlstring(state, message.data(), message.size());
  return 2;
}

LuaSkinHttpLinesResult mainStateHttpLines(lua_State *state,
                                         LuaSkinHostModulesImpl &impl) {
  if (impl.frameCallbackActive) {
    return {.failure =
                "HTTP is unavailable during gameplay frame callbacks"};
  }
  std::size_t urlSize = 0;
  const char *url = luaL_checklstring(state, 1, &urlSize);
  const int timeout = boundedIntegerArgument(
      state, 2, LuaSkinHttpClient::defaultTimeoutMilliseconds, true);
  LuaSkinHttpClient client(impl.httpTransport);
  LuaSkinHttpResult fetched =
      client.get(std::string_view(url, urlSize), timeout);
  if (fetched.failure) {
    return {.failure = std::move(fetched.failure)};
  }
  if (!fetched.response) {
    return {.failure = "HTTP transport returned no response"};
  }
  return LuaSkinHttpClient::readLines(fetched.response->body);
}

int mainStateHttpGetLines(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  LuaSkinHttpLinesResult result = mainStateHttpLines(state, *impl);
  if (result.failure) {
    return pushHttpFailure(state, *result.failure);
  }
  lua_createtable(state, static_cast<int>(result.lines.size()), 0);
  int index = 1;
  for (const std::string &line : result.lines) {
    lua_pushlstring(state, line.data(), line.size());
    lua_rawseti(state, -2, index++);
  }
  lua_pushboolean(state, 1);
  return 2;
}

int mainStateHttpGet(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  LuaSkinHttpLinesResult result = mainStateHttpLines(state, *impl);
  if (result.failure) {
    return pushHttpFailure(state, *result.failure);
  }
  try {
    std::size_t size = result.lines.empty() ? 0 : result.lines.size() - 1;
    for (const std::string &line : result.lines) {
      if (line.size() > std::numeric_limits<std::size_t>::max() - size) {
        return pushHttpFailure(state, "HTTP response allocation failed");
      }
      size += line.size();
    }
    std::string text;
    text.reserve(size);
    for (std::size_t index = 0; index < result.lines.size(); ++index) {
      if (index != 0) {
        text.push_back('\n');
      }
      text += result.lines[index];
    }
    lua_pushlstring(state, text.data(), text.size());
    lua_pushboolean(state, 1);
    return 2;
  } catch (...) {
    return pushHttpFailure(state, "HTTP response allocation failed");
  }
}

void populateMainState(lua_State *state, LuaSkinHostModulesImpl *impl) {
  lua_getglobal(state, "main_state");
  if (!lua_istable(state, -1)) {
    luaL_error(state, "main_state compatibility table is unavailable");
  }
  installClosure(state, impl, mainStateOption);
  lua_setfield(state, -2, "option");
  installClosure(state, impl, mainStateNumber);
  lua_setfield(state, -2, "number");
  installClosure(state, impl, mainStateFloatNumber);
  lua_setfield(state, -2, "float_number");
  installClosure(state, impl, mainStateText);
  lua_setfield(state, -2, "text");
  installClosure(state, impl, mainStateOffset);
  lua_setfield(state, -2, "offset");
  installClosure(state, impl, mainStateTimer);
  lua_setfield(state, -2, "timer");
  installClosure(state, impl, mainStateSetTimer);
  lua_setfield(state, -2, "set_timer");
  lua_pushnumber(
      state, static_cast<lua_Number>(std::numeric_limits<std::int64_t>::min()));
  lua_setfield(state, -2, "timer_off_value");
  installClosure(state, impl, mainStateEventExec);
  lua_setfield(state, -2, "event_exec");
  installClosure(state, impl, mainStateEventIndex);
  lua_setfield(state, -2, "event_index");
  installClosure(state, impl, mainStateExscore);
  lua_setfield(state, -2, "exscore");
  installClosure(state, impl, mainStateGauge);
  lua_setfield(state, -2, "gauge");
  installClosure(state, impl, mainStateGaugeType);
  lua_setfield(state, -2, "gauge_type");
  installClosure(state, impl, mainStateJudge);
  lua_setfield(state, -2, "judge");
  installClosure(state, impl, mainStateRate);
  lua_setfield(state, -2, "rate");
  installClosure(state, impl, mainStateRateBest);
  lua_setfield(state, -2, "rate_best");
  installClosure(state, impl, mainStateExscoreBest);
  lua_setfield(state, -2, "exscore_best");
  installClosure(state, impl, mainStateRateRival);
  lua_setfield(state, -2, "rate_rival");
  installClosure(state, impl, mainStateExscoreRival);
  lua_setfield(state, -2, "exscore_rival");
  installClosure(state, impl, mainStateTime);
  lua_setfield(state, -2, "time");
  installClosure(state, impl, mainStateVolumeBg);
  lua_setfield(state, -2, "volume_bg");
  installClosure(state, impl, mainStateVolumeKey);
  lua_setfield(state, -2, "volume_key");
  installClosure(state, impl, mainStateVolumeSys);
  lua_setfield(state, -2, "volume_sys");
  installClosure(state, impl, mainStateFileExists);
  lua_setfield(state, -2, "file_exists");
  installClosure(state, impl, mainStateFileMkdir);
  lua_setfield(state, -2, "file_mkdir");
  installClosure(state, impl, mainStateFileList);
  lua_setfield(state, -2, "file_list");
  installClosure(state, impl, mainStateFileReadLines);
  lua_setfield(state, -2, "file_read_lines");
  installClosure(state, impl, mainStateFileWrite);
  lua_setfield(state, -2, "file_write");
  installClosure(state, impl, mainStateFileAppend);
  lua_setfield(state, -2, "file_append");
  installClosure(state, impl, mainStateFileClear);
  lua_setfield(state, -2, "file_clear");
  installClosure(state, impl, mainStateFileCountLines);
  lua_setfield(state, -2, "file_count_lines");
  installClosure(state, impl, mainStateHttpGet);
  lua_setfield(state, -2, "http_get");
  installClosure(state, impl, mainStateHttpGetLines);
  lua_setfield(state, -2, "http_get_lines");
  installClosure(state, impl, mainStateAudioPlay);
  lua_setfield(state, -2, "audio_play");
  installClosure(state, impl, mainStateAudioLoop);
  lua_setfield(state, -2, "audio_loop");
  installClosure(state, impl, mainStateAudioPreload);
  lua_setfield(state, -2, "audio_preload");
  installClosure(state, impl, mainStateAudioStop);
  lua_setfield(state, -2, "audio_stop");
  installClosure(state, impl, mainStateAudioDispose);
  lua_setfield(state, -2, "audio_dispose");
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

enum class HandleReadStatus : std::uint8_t {
  Success,
  EndOfFile,
  AllocationFailure,
  LineLimitExceeded,
  ReadLimitExceeded,
  IoFailure,
};

HandleReadStatus readHandleLine(LuaFileHandle &handle, std::string &line);
int reportHandleReadFailure(lua_State *state, LuaFileHandle &handle,
                            HandleReadStatus status);

bool seekHandle(LuaFileHandle &handle, std::int64_t offset, int origin) {
  if (handle.file == nullptr ||
      offset > static_cast<std::int64_t>(std::numeric_limits<long>::max()) ||
      offset < static_cast<std::int64_t>(std::numeric_limits<long>::min())) {
    return false;
  }
  std::clearerr(handle.file);
  return std::fseek(handle.file, static_cast<long>(offset), origin) == 0;
}

std::optional<std::int64_t> tellHandle(LuaFileHandle &handle) {
  if (handle.file == nullptr) {
    return std::nullopt;
  }
  const long position = std::ftell(handle.file);
  return position < 0 ? std::nullopt
                      : std::optional<std::int64_t>{position};
}

bool guardHandle(LuaFileHandle &handle) {
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
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  if (!seekHandle(*handle, handle->cursor, SEEK_SET)) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not be read",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  std::string line;
  const auto lineStatus = readHandleLine(*handle, line);
  if (lineStatus != HandleReadStatus::Success) {
    if (lineStatus == HandleReadStatus::EndOfFile) {
      if (handle->closeOnEndOfLines) {
        std::fclose(handle->file);
        handle->file = nullptr;
        handle->closed = true;
      }
      return 0;
    }
    return reportHandleReadFailure(state, *handle, lineStatus);
  }
  const auto next = tellHandle(*handle);
  if (!next) {
    return reportHandleReadFailure(state, *handle, HandleReadStatus::IoFailure);
  }
  handle->cursor = *next;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  lua_pushlstring(state, line.data(), line.size());
  return 1;
}

int fileLines(lua_State *state) {
  luaL_argcheck(state, lua_gettop(state) == 1, 1, "lines accepts no arguments");
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  lua_pushvalue(state, 1);
  lua_pushcclosure(state, fileLineIterator, 1);
  return 1;
}

bool beginHandleRead(LuaFileHandle &handle) {
  return seekHandle(handle, handle.cursor, SEEK_SET);
}

void finishHandleRead(LuaFileHandle &handle) {
  if (const auto next = tellHandle(handle)) {
    handle.cursor = *next;
  }
}

std::optional<std::size_t> unreadHandleBytes(LuaFileHandle &handle) {
  const auto current = tellHandle(handle);
  if (!current || !seekHandle(handle, 0, SEEK_END)) {
    return std::nullopt;
  }
  const auto end = tellHandle(handle);
  if (!seekHandle(handle, *current, SEEK_SET) || !end) {
    return std::nullopt;
  }
  const std::int64_t remaining = *end - *current;
  if (remaining < 0 || static_cast<std::uintmax_t>(remaining) >
                           std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(remaining);
}

HandleReadStatus readHandleLine(LuaFileHandle &handle, std::string &line) {
  line.clear();
  try {
    while (true) {
      const int character = std::fgetc(handle.file);
      if (character == EOF) {
        return std::ferror(handle.file) != 0 ? HandleReadStatus::IoFailure
                                 : (line.empty() ? HandleReadStatus::EndOfFile
                                                 : HandleReadStatus::Success);
      }
      if (character == '\n') {
        return HandleReadStatus::Success;
      }
      if (line.size() >= handle.owner->maximumSourceBytes) {
        return HandleReadStatus::LineLimitExceeded;
      }
      line.push_back(static_cast<char>(character));
    }
  } catch (const std::bad_alloc &) {
    return HandleReadStatus::AllocationFailure;
  } catch (const std::length_error &) {
    return HandleReadStatus::AllocationFailure;
  }
}

HandleReadStatus readHandleBytes(LuaFileHandle &handle,
                                 std::size_t requestedBytes,
                                 std::string &bytes) {
  try {
    bytes.assign(requestedBytes, '\0');
  } catch (const std::bad_alloc &) {
    return HandleReadStatus::AllocationFailure;
  } catch (const std::length_error &) {
    return HandleReadStatus::AllocationFailure;
  }
  if (requestedBytes == 0) {
    return HandleReadStatus::Success;
  }
  const std::size_t read =
      std::fread(bytes.data(), 1, requestedBytes, handle.file);
  if (std::ferror(handle.file) != 0) {
    return HandleReadStatus::IoFailure;
  }
  bytes.resize(read);
  return HandleReadStatus::Success;
}

int reportHandleReadFailure(lua_State *state, LuaFileHandle &handle,
                            HandleReadStatus status) {
  handle.owner->storeError(
      "skin_lua_file_operation_failed",
      status == HandleReadStatus::AllocationFailure
          ? "Lua skin file read buffer could not be allocated"
          : status == HandleReadStatus::LineLimitExceeded
                ? "Lua skin file line exceeds runtime storage budget"
                : status == HandleReadStatus::ReadLimitExceeded
                      ? "Lua skin file read exceeds runtime storage budget"
                : "Lua skin file could not be read",
      handle.virtualPath);
  return raiseStoredError(state, handle.owner);
}

int fileRead(lua_State *state) {
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  const int argumentCount = lua_gettop(state);
  const int firstArgument = argumentCount == 1 ? 1 : 2;
  const int lastArgument = argumentCount == 1 ? 1 : argumentCount;
  int results = 0;
  for (int index = firstArgument; index <= lastArgument; ++index) {
    const bool defaultLine = argumentCount == 1;
    const int valueType = defaultLine ? LUA_TSTRING : lua_type(state, index);
    if (!beginHandleRead(*handle)) {
      handle->owner->storeError("skin_lua_file_operation_failed",
                                "Lua skin file could not be read",
                                handle->virtualPath);
      return raiseStoredError(state, handle->owner);
    }
    if (valueType == LUA_TNUMBER) {
      const lua_Integer requested = luaL_checkinteger(state, index);
      if (requested < 0) {
        return luaL_argerror(state, index, "non-negative byte count expected");
      }
      const auto remainingBytes = unreadHandleBytes(*handle);
      if (!remainingBytes) {
        handle->owner->storeError("skin_lua_file_operation_failed",
                                  "Lua skin file could not be inspected",
                                  handle->virtualPath);
        return raiseStoredError(state, handle->owner);
      }
      const std::uintmax_t requestedBytes =
          static_cast<std::uintmax_t>(requested);
      const std::size_t bytesToRead =
          requestedBytes > *remainingBytes
              ? *remainingBytes
              : static_cast<std::size_t>(requestedBytes);
      if (bytesToRead > handle->owner->maximumSourceBytes) {
        return reportHandleReadFailure(state, *handle,
                                       HandleReadStatus::ReadLimitExceeded);
      }
      std::string bytes;
      const HandleReadStatus readStatus =
          readHandleBytes(*handle, bytesToRead, bytes);
      if (readStatus != HandleReadStatus::Success) {
        return reportHandleReadFailure(state, *handle, readStatus);
      }
      if (bytes.empty() && requested != 0) {
        lua_pushnil(state);
        return results + 1;
      }
      finishHandleRead(*handle);
      lua_pushlstring(state, bytes.data(), bytes.size());
      ++results;
      continue;
    }

    std::size_t formatSize = 0;
    const char *format = defaultLine ? "*l"
                                     : luaL_checklstring(state, index, &formatSize);
    const std::string_view requestedFormat(
        format, defaultLine ? std::size_t{2} : formatSize);
    if (requestedFormat == "*l") {
      std::string line;
      const auto lineStatus = readHandleLine(*handle, line);
      if (lineStatus != HandleReadStatus::Success) {
        if (lineStatus != HandleReadStatus::EndOfFile) {
          return reportHandleReadFailure(state, *handle, lineStatus);
        }
        std::clearerr(handle->file);
        lua_pushnil(state);
        return results + 1;
      }
      finishHandleRead(*handle);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lua_pushlstring(state, line.data(), line.size());
      ++results;
      continue;
    }
    if (requestedFormat == "*a") {
      const auto remainingBytes = unreadHandleBytes(*handle);
      if (!remainingBytes) {
        handle->owner->storeError("skin_lua_file_operation_failed",
                                  "Lua skin file could not be inspected",
                                  handle->virtualPath);
        return raiseStoredError(state, handle->owner);
      }
      if (*remainingBytes > handle->owner->maximumSourceBytes) {
        return reportHandleReadFailure(state, *handle,
                                       HandleReadStatus::ReadLimitExceeded);
      }
      std::string bytes;
      const HandleReadStatus readStatus =
          readHandleBytes(*handle, *remainingBytes, bytes);
      if (readStatus != HandleReadStatus::Success) {
        return reportHandleReadFailure(state, *handle, readStatus);
      }
      finishHandleRead(*handle);
      lua_pushlstring(state, bytes.data(), bytes.size());
      ++results;
      continue;
    }
    if (requestedFormat == "*n") {
      double parsedNumber = 0;
      if (std::fscanf(handle->file, "%lf", &parsedNumber) != 1) {
        if (std::ferror(handle->file) != 0) {
          handle->owner->storeError("skin_lua_file_operation_failed",
                                    "Lua skin file could not be read",
                                    handle->virtualPath);
          return raiseStoredError(state, handle->owner);
        }
        std::clearerr(handle->file);
        lua_pushnil(state);
        return results + 1;
      }
      finishHandleRead(*handle);
      lua_pushnumber(state, static_cast<lua_Number>(parsedNumber));
      ++results;
      continue;
    }
    return luaL_argerror(state, index, "invalid read format");
  }
  return results;
}

int fileWrite(lua_State *state) {
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (!handle->writable) {
    handle->owner->storeError("skin_lua_file_mode_denied",
                              "write is unavailable on a read-only handle",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }

  if (!seekHandle(*handle, handle->cursor, SEEK_SET)) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not be written",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  for (int index = 2; index <= lua_gettop(state); ++index) {
    std::size_t size = 0;
    const char *text = lua_tolstring(state, index, &size);
    if (text == nullptr) {
      return luaL_argerror(state, index, "string or number expected");
    }
    if (std::fwrite(text, 1, size, handle->file) != size) {
      handle->owner->storeError("skin_lua_file_operation_failed",
                                "Lua skin file could not be written",
                                handle->virtualPath);
      return raiseStoredError(state, handle->owner);
    }
  }
  if (std::fflush(handle->file) != 0) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not be flushed",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  const auto next = tellHandle(*handle);
  if (!next) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not be written",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  handle->cursor = *next;
  lua_pushvalue(state, 1);
  return 1;
}

int fileSeek(lua_State *state) {
  luaL_argcheck(state, lua_gettop(state) >= 1 && lua_gettop(state) <= 3, 1,
                "seek accepts an optional whence and byte offset");
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  const char *whence = luaL_optstring(state, 2, "cur");
  const lua_Integer offset = luaL_optinteger(state, 3, 0);
  std::streamoff base = 0;
  if (std::strcmp(whence, "set") == 0) {
    base = 0;
  } else if (std::strcmp(whence, "cur") == 0) {
    base = static_cast<std::streamoff>(handle->cursor);
  } else if (std::strcmp(whence, "end") == 0) {
    if (handle->writable) {
      std::fflush(handle->file);
    }
    if (!seekHandle(*handle, 0, SEEK_END)) {
      handle->owner->storeError("skin_lua_file_operation_failed",
                                "Lua skin file could not seek",
                                handle->virtualPath);
      return raiseStoredError(state, handle->owner);
    }
    const auto end = tellHandle(*handle);
    if (!end) {
      handle->owner->storeError("skin_lua_file_operation_failed",
                                "Lua skin file could not seek",
                                handle->virtualPath);
      return raiseStoredError(state, handle->owner);
    }
    base = *end;
  } else {
    return luaL_argerror(state, 2, "invalid seek option");
  }
  static_assert(std::numeric_limits<lua_Integer>::is_signed &&
                std::numeric_limits<lua_Integer>::digits <=
                    std::numeric_limits<std::int64_t>::digits);
  const auto position =
      lua_file_io::checkedSeekPosition(base, static_cast<std::int64_t>(offset));
  if (!position) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not seek",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  if (!seekHandle(*handle, *position, SEEK_SET)) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not seek",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  handle->cursor = *position;
  lua_pushnumber(state, static_cast<lua_Number>(*position));
  return 1;
}

int fileFlush(lua_State *state) {
  luaL_argcheck(state, lua_gettop(state) == 1, 1, "flush accepts no arguments");
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  if (std::fflush(handle->file) != 0) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not be flushed",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  lua_pushboolean(state, 1);
  return 1;
}

int fileSetvbuf(lua_State *state) {
  luaL_argcheck(state, lua_gettop(state) >= 2 && lua_gettop(state) <= 3, 1,
                "setvbuf accepts a mode and optional size");
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  const char *mode = luaL_checkstring(state, 2);
  if (std::strcmp(mode, "no") != 0 && std::strcmp(mode, "full") != 0 &&
      std::strcmp(mode, "line") != 0) {
    return luaL_argerror(state, 2, "invalid buffering mode");
  }
  if (lua_gettop(state) == 3) {
    (void)luaL_checkinteger(state, 3);
  }
  // RestrictedIoLib implements setvbuf as a no-op.
  lua_pushboolean(state, 1);
  return 1;
}

int fileClose(lua_State *state) {
  luaL_argcheck(state, lua_gettop(state) == 1, 1, "close accepts no arguments");
  SharedLuaFileHandle &shared = checkedHandle(state, 1);
  LuaFileHandle *handle = shared.get();
  if (handle->closed && !handle->invalidated) {
    lua_pushboolean(state, 1);
    return 1;
  }
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  if (handle->writable) {
    std::fflush(handle->file);
  }
  const int closeStatus = std::fclose(handle->file);
  handle->file = nullptr;
  if (closeStatus != 0) {
    handle->owner->storeError("skin_lua_file_operation_failed",
                              "Lua skin file could not be closed",
                              handle->virtualPath);
    return raiseStoredError(state, handle->owner);
  }
  handle->closed = true;
  lua_pushboolean(state, 1);
  return 1;
}

struct LuaOpenMode {
  bool readMode = false;
  bool appendMode = false;
  bool updateMode = false;
};

std::optional<LuaOpenMode> parseOpenMode(std::string_view mode) {
  if (mode.empty()) {
    return std::nullopt;
  }
  LuaOpenMode result;
  switch (mode.front()) {
  case 'r':
    result.readMode = true;
    break;
  case 'w':
    break;
  case 'a':
    result.appendMode = true;
    break;
  default:
    return std::nullopt;
  }
  bool binary = false;
  for (std::size_t index = 1; index < mode.size(); ++index) {
    if (mode[index] == '+' && !result.updateMode) {
      result.updateMode = true;
    } else if (mode[index] == 'b' && !binary) {
      binary = true;
    } else {
      return std::nullopt;
    }
  }
  return result;
}

int ioOpen(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  std::size_t pathSize = 0;
  const char *path = luaL_checklstring(state, 1, &pathSize);
  std::size_t modeSize = 0;
  const char *mode = luaL_optlstring(state, 2, "r", &modeSize);
  const std::string_view virtualPath(path, pathSize);
  if (lua_gettop(state) > 2) {
    return expectedFailure(state, "io.open accepts only path and mode");
  }
  const auto selectedMode = parseOpenMode(std::string_view(mode, modeSize));
  if (!selectedMode) {
    return expectedFailure(state, "Lua skin file mode is not allowed");
  }

  // Beatoraja's RestrictedIoLib treats r+ as a writable RandomAccessFile.
  // Keep the same behavior while routing every mode through the filesystem's
  // descriptor-bound Lua I/O path.
  const bool requiresWriteAccess =
      !selectedMode->readMode || selectedMode->updateMode;
  const LuaSkinFileOpenMode fileMode =
      selectedMode->readMode
          ? (selectedMode->updateMode ? LuaSkinFileOpenMode::ReadUpdate
                                      : LuaSkinFileOpenMode::Read)
          : (selectedMode->appendMode
                 ? (selectedMode->updateMode ? LuaSkinFileOpenMode::AppendUpdate
                                             : LuaSkinFileOpenMode::Append)
                 : (selectedMode->updateMode ? LuaSkinFileOpenMode::WriteUpdate
                                             : LuaSkinFileOpenMode::Write));
  try {
    auto handle = std::make_shared<LuaFileHandle>();
    handle->owner = impl;
    handle->virtualPath.assign(virtualPath);
    handle->writable = requiresWriteAccess;
    const auto opened = impl->fileSystem->openLuaFile(virtualPath, fileMode);
    if (opened.failure) {
      return expectedFailure(state, opened.failure->message);
    }
    handle->file = opened.file;
    if (selectedMode->appendMode) {
      if (!seekHandle(*handle, 0, SEEK_END)) {
        return expectedFailure(state, "Lua skin file could not be opened");
      }
      const auto end = tellHandle(*handle);
      if (!end) {
        return expectedFailure(state, "Lua skin file could not be opened");
      }
      handle->cursor = *end;
    }
    void *storage = lua_newuserdata(state, sizeof(SharedLuaFileHandle));
    new (storage) SharedLuaFileHandle(handle);
    luaL_getmetatable(state, kHandleMetatable);
    lua_setmetatable(state, -2);
  } catch (...) {
    return expectedFailure(state, "Lua skin file handle could not allocate");
  }
  return 1;
}

int emptyLineIterator(lua_State *) { return 0; }

int ioLines(lua_State *state) {
  const int argumentCount = lua_gettop(state);
  luaL_argcheck(state, argumentCount <= 1, 1,
                "lines accepts an optional path");
  if (argumentCount == 0 || lua_isnil(state, 1)) {
    // SandboxIoLib inherits IoLib's no-argument behavior.  Its wrapped stdin
    // is an empty file, so io.lines() immediately reaches end of input.
    lua_pushcfunction(state, emptyLineIterator);
    return 1;
  }

  const int openResults = ioOpen(state);
  if (openResults != 1) {
    return openResults;
  }
  SharedLuaFileHandle &shared = checkedHandle(state, -1);
  LuaFileHandle *handle = shared.get();
  if (!guardHandle(*handle)) {
    return raiseStoredError(state, handle->owner);
  }
  // LuaJ's IoLib closes the temporary handle returned by io.lines(filename)
  // when its iterator reaches EOF.  file:lines() deliberately remains open.
  handle->closeOnEndOfLines = true;
  lua_pushcclosure(state, fileLineIterator, 1);
  return 1;
}

int textLoader(lua_State *state) {
  std::size_t size = 0;
  const char *text = luaL_checklstring(state, 1, &size);
  const char *name = luaL_optstring(state, 2, "=(skin-load)");
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
    const std::string_view virtualPath(path, pathSize);
    const auto read = impl->fileSystem->readLuaPath(
        virtualPath, impl->maximumSourceBytes);
    if (read.failure) {
      impl->storeFileError(*read.failure);
    } else {
      std::string chunkName = "@" + std::string(virtualPath);
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

int loadFile(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  luaL_argcheck(state, lua_gettop(state) == 1, 1,
                "loadfile accepts exactly one path");
  std::size_t pathSize = 0;
  const char *path = luaL_checklstring(state, 1, &pathSize);
  const std::string_view virtualPath(path, pathSize);
  const auto read = impl->fileSystem->readLuaPath(
      virtualPath, impl->maximumSourceBytes);
  if (read.failure) {
    return expectedFailure(state, read.failure->message);
  }
  std::string chunkName = "@" + std::string(virtualPath);
  const int loadStatus = luaL_loadbuffer(
      state, reinterpret_cast<const char *>(read.bytes.data()),
      read.bytes.size(), chunkName.c_str());
  if (loadStatus == 0) {
    return 1;
  }
  lua_pushnil(state);
  lua_insert(state, -2);
  return 2;
}

std::string modulePathSubstitution(std::string_view moduleName) {
  std::string substitution(moduleName);
  if (substitution.find('/') == std::string::npos) {
    std::ranges::replace(substitution, '.', '/');
  }
  return substitution;
}

bool modulePathExpansionFitsBudget(std::string_view pattern,
                                   std::string_view substitution,
                                   std::size_t maximumBytes,
                                   std::size_t &expandedSize) {
  const std::size_t substitutions =
      static_cast<std::size_t>(std::ranges::count(pattern, '?'));
  const std::size_t fixedBytes = pattern.size() - substitutions;
  if (fixedBytes > maximumBytes ||
      (substitutions != 0 &&
       substitution.size() > (maximumBytes - fixedBytes) / substitutions)) {
    return false;
  }
  expandedSize = fixedBytes + substitutions * substitution.size();
  return true;
}

int moduleLoader(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  std::size_t nameSize = 0;
  const char *name = luaL_checklstring(state, 1, &nameSize);
  int loadStatus = LUA_ERRFILE;
  bool found = false;
  lua_getglobal(state, "package");
  lua_getfield(state, -1, "path");
  std::size_t searchPathSize = 0;
  const char *searchPath = luaL_checklstring(state, -1, &searchPathSize);
  const std::string substitution =
      modulePathSubstitution(std::string_view(name, nameSize));
  const std::string_view templates(searchPath, searchPathSize);
  lua_pop(state, 2);

  std::vector<std::string> searchedCandidates;
  std::size_t start = 0;
  std::size_t searchedTemplates = 0;
  while (start <= templates.size()) {
    if (searchedTemplates == impl->maximumModuleSearchTemplates) {
      lua_pushliteral(
          state, "\n\tLua module path search exceeds Lua runtime storage budget");
      return 1;
    }
    ++searchedTemplates;
    const std::size_t end = templates.find(';', start);
    const std::string_view pattern = templates.substr(
        start,
        end == std::string_view::npos ? templates.size() - start : end - start);
    if (!pattern.empty() && pattern.find('?') != std::string_view::npos) {
      std::size_t expandedSize = 0;
      if (!modulePathExpansionFitsBudget(pattern, substitution,
                                         impl->maximumSourceBytes,
                                         expandedSize)) {
        lua_pushliteral(
            state, "\n\tLua module path expansion exceeds Lua runtime storage budget");
        return 1;
      }
      std::string candidate;
      candidate.reserve(expandedSize);
      for (const char character : pattern) {
        if (character == '?') {
          candidate += substitution;
        } else {
          candidate.push_back(character);
        }
      }
      if (searchedCandidates.size() < 4) {
        searchedCandidates.push_back(candidate);
      }
      const auto read = impl->fileSystem->readLuaPath(
          candidate, impl->maximumSourceBytes);
      if (!read.failure) {
        found = true;
        std::string chunkName = "@module:" + std::string(name, nameSize);
        loadStatus = luaL_loadbuffer(
            state, reinterpret_cast<const char *>(read.bytes.data()),
            read.bytes.size(), chunkName.c_str());
        break;
      }
      if (read.failure->code != SkinFileError::Missing) {
        impl->storeFileError(*read.failure);
        return raiseStoredError(state, impl);
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (!found) {
    try {
      std::string message = "\n\tvirtual module '";
      message.append(name, nameSize);
      message += "' was not found";
      if (!searchedCandidates.empty()) {
        message += " (searched: ";
        for (std::size_t index = 0; index < searchedCandidates.size();
             ++index) {
          if (index != 0) {
            message += ", ";
          }
          message += searchedCandidates[index];
        }
        message += ')';
      }
      lua_pushlstring(state, message.data(), message.size());
    } catch (...) {
      lua_pushfstring(state, "\n\tvirtual module '%s' was not found", name);
    }
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

void installClosedMemberMetatable(lua_State *, LuaSkinHostModulesImpl *,
                                  lua_CFunction);

struct LegacyHttpConnection {
  std::string url;
  int timeoutMilliseconds = LuaSkinHttpClient::defaultTimeoutMilliseconds;
  bool connected = false;
  std::unique_ptr<LuaSkinHttpConnection> connection;

  std::optional<std::string>
  connect(LuaSkinHttpTransport *transport) noexcept {
    if (connected) {
      return std::nullopt;
    }
    LuaSkinHttpClient client(transport);
    LuaSkinHttpOpenResult opened = client.open(url, timeoutMilliseconds);
    if (opened.failure) {
      return std::move(opened.failure);
    }
    if (!opened.connection) {
      return std::string("HTTP transport returned no connection");
    }
    if (auto failure = opened.connection->connect()) {
      opened.connection->disconnect();
      return failure;
    }
    connection = std::move(opened.connection);
    connected = true;
    return std::nullopt;
  }

  ~LegacyHttpConnection() {
    if (connection) {
      connection->disconnect();
    }
  }
};

struct LegacyHttpReader {
  std::vector<std::string> lines;
  std::size_t position = 0;
};

using SharedLegacyHttpConnection = std::shared_ptr<LegacyHttpConnection>;
using SharedLegacyHttpReader = std::shared_ptr<LegacyHttpReader>;

template <typename Shared>
int legacyHttpSharedGc(lua_State *state) {
  auto *shared = static_cast<Shared *>(lua_touserdata(state, 1));
  shared->~Shared();
  return 0;
}

void pushLegacyHttpShared(lua_State *state,
                          const SharedLegacyHttpConnection &connection) {
  void *storage = lua_newuserdata(state, sizeof(SharedLegacyHttpConnection));
  new (storage) SharedLegacyHttpConnection(connection);
  luaL_getmetatable(state, kLegacyHttpConnectionMetatable);
  lua_setmetatable(state, -2);
}

void pushLegacyHttpShared(lua_State *state,
                          const SharedLegacyHttpReader &reader) {
  void *storage = lua_newuserdata(state, sizeof(SharedLegacyHttpReader));
  new (storage) SharedLegacyHttpReader(reader);
  luaL_getmetatable(state, kLegacyHttpReaderMetatable);
  lua_setmetatable(state, -2);
}

SharedLegacyHttpConnection &legacyHttpConnection(lua_State *state,
                                                 int upvalue = 2) {
  return *static_cast<SharedLegacyHttpConnection *>(luaL_checkudata(
      state, lua_upvalueindex(upvalue), kLegacyHttpConnectionMetatable));
}

SharedLegacyHttpReader &legacyHttpReader(lua_State *state, int upvalue = 2) {
  return *static_cast<SharedLegacyHttpReader *>(luaL_checkudata(
      state, lua_upvalueindex(upvalue), kLegacyHttpReaderMetatable));
}

int legacyUrlMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("java.net.URL.member");
  return raiseStoredError(state, impl);
}

int legacyConnectionMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("java.net.URL.connection.member");
  return raiseStoredError(state, impl);
}

int legacyReaderMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("java.io.BufferedReader.member");
  return raiseStoredError(state, impl);
}

int raiseLegacyHttpFailure(lua_State *state, std::string_view prefix,
                           std::string_view failure) {
  lua_pushlstring(state, prefix.data(), prefix.size());
  lua_pushlstring(state, failure.data(), failure.size());
  lua_concat(state, 2);
  return lua_error(state);
}

bool legacyHttpReceiver(lua_State *state, int arguments,
                        int tableUpvalue = 3) {
  return lua_gettop(state) == arguments &&
         sameUpvalueTable(state, 1, tableUpvalue);
}

void pushLegacyConnectionObject(
    lua_State *state, LuaSkinHostModulesImpl *impl,
    const SharedLegacyHttpConnection &connection);

int legacyOpenConnection(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (!legacyHttpReceiver(state, 1)) {
    impl->reportLegacyDenial("java.net.URL.member");
    return raiseStoredError(state, impl);
  }
  pushLegacyConnectionObject(state, impl, legacyHttpConnection(state));
  return 1;
}

void pushLegacyUrlObject(lua_State *state, LuaSkinHostModulesImpl *impl,
                         std::string_view url) {
  auto connection = std::make_shared<LegacyHttpConnection>();
  connection->url.assign(url);

  lua_createtable(state, 0, 1);
  const int tableIndex = lua_gettop(state);
  lua_pushlightuserdata(state, impl);
  pushLegacyHttpShared(state, connection);
  lua_pushvalue(state, tableIndex);
  lua_pushcclosure(state, legacyOpenConnection, 3);
  lua_setfield(state, tableIndex, "openConnection");
  installClosedMemberMetatable(state, impl, legacyUrlMemberDenied);
}

int legacySetRequestMethod(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (!legacyHttpReceiver(state, 2)) {
    impl->reportLegacyDenial("java.net.URL.connection.member");
    return raiseStoredError(state, impl);
  }
  const char *method = luaL_checkstring(state, 2);
  if (std::string_view(method) != "GET") {
    return luaL_error(state, "Legacy Lua skin HTTP method denied: %s", method);
  }
  lua_pushnil(state);
  return 1;
}

int legacySetConnectTimeout(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (!legacyHttpReceiver(state, 2)) {
    impl->reportLegacyDenial("java.net.URL.connection.member");
    return raiseStoredError(state, impl);
  }
  legacyHttpConnection(state)->timeoutMilliseconds =
      LuaSkinHttpClient::clampTimeout(
          boundedIntegerArgument(state, 2, 0, false));
  lua_pushnil(state);
  return 1;
}

int legacyConnect(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (!legacyHttpReceiver(state, 1)) {
    impl->reportLegacyDenial("java.net.URL.connection.member");
    return raiseStoredError(state, impl);
  }
  SharedLegacyHttpConnection &connection = legacyHttpConnection(state);
  if (const auto failure = connection->connect(impl->httpTransport)) {
    return raiseLegacyHttpFailure(
        state, "Legacy Lua skin HTTP connection failed: ", *failure);
  }
  lua_pushnil(state);
  return 1;
}

int legacyGetResponseCode(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (!legacyHttpReceiver(state, 1)) {
    impl->reportLegacyDenial("java.net.URL.connection.member");
    return raiseStoredError(state, impl);
  }
  SharedLegacyHttpConnection &connection = legacyHttpConnection(state);
  if (const auto failure = connection->connect(impl->httpTransport)) {
    return raiseLegacyHttpFailure(
        state, "Legacy Lua skin HTTP connection failed: ", *failure);
  }
  LuaSkinHttpCodeResult result = connection->connection->responseCode();
  if (result.failure) {
    return raiseLegacyHttpFailure(
        state, "Legacy Lua skin HTTP response failed: ", *result.failure);
  }
  if (!result.code) {
    return raiseLegacyHttpFailure(
        state, "Legacy Lua skin HTTP response failed: ",
        "HTTP transport returned no response code");
  }
  lua_pushinteger(state, *result.code);
  return 1;
}

int legacyReadLine(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (!legacyHttpReceiver(state, 1)) {
    impl->reportLegacyDenial("java.io.BufferedReader.member");
    return raiseStoredError(state, impl);
  }
  SharedLegacyHttpReader &reader = legacyHttpReader(state);
  if (reader->position >= reader->lines.size()) {
    lua_pushnil(state);
    return 1;
  }
  const std::string &line = reader->lines[reader->position++];
  lua_pushlstring(state, line.data(), line.size());
  return 1;
}

void pushLegacyReaderObject(lua_State *state, LuaSkinHostModulesImpl *impl,
                            LuaSkinHttpLinesResult result) {
  auto reader = std::make_shared<LegacyHttpReader>();
  reader->lines = std::move(result.lines);

  lua_createtable(state, 0, 1);
  const int tableIndex = lua_gettop(state);
  lua_pushlightuserdata(state, impl);
  pushLegacyHttpShared(state, reader);
  lua_pushvalue(state, tableIndex);
  lua_pushcclosure(state, legacyReadLine, 3);
  lua_setfield(state, tableIndex, "readLine");
  installClosedMemberMetatable(state, impl, legacyReaderMemberDenied);
}

int legacyGetInputStream(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (!legacyHttpReceiver(state, 1)) {
    impl->reportLegacyDenial("java.net.URL.connection.member");
    return raiseStoredError(state, impl);
  }
  SharedLegacyHttpConnection &connection = legacyHttpConnection(state);
  if (const auto failure = connection->connect(impl->httpTransport)) {
    return raiseLegacyHttpFailure(
        state, "Legacy Lua skin HTTP connection failed: ", *failure);
  }
  LuaSkinHttpBodyResult body = connection->connection->readBody();
  connection->connection->disconnect();
  if (body.failure) {
    return raiseLegacyHttpFailure(state,
                                  "Legacy Lua skin HTTP read failed: ",
                                  *body.failure);
  }
  if (!body.body) {
    return raiseLegacyHttpFailure(
        state, "Legacy Lua skin HTTP read failed: ",
        "HTTP transport returned no response body");
  }
  LuaSkinHttpLinesResult result = LuaSkinHttpClient::readLines(*body.body);
  if (result.failure) {
    return raiseLegacyHttpFailure(state,
                                  "Legacy Lua skin HTTP read failed: ",
                                  *result.failure);
  }
  try {
    pushLegacyReaderObject(state, impl, std::move(result));
    return 1;
  } catch (...) {
    return luaL_error(state,
                      "Legacy Lua skin HTTP read failed: allocation failed");
  }
}

void pushLegacyConnectionObject(
    lua_State *state, LuaSkinHostModulesImpl *impl,
    const SharedLegacyHttpConnection &connection) {
  lua_createtable(state, 0, 5);
  const int tableIndex = lua_gettop(state);
  const auto member = [&](const char *name, lua_CFunction function) {
    lua_pushlightuserdata(state, impl);
    pushLegacyHttpShared(state, connection);
    lua_pushvalue(state, tableIndex);
    lua_pushcclosure(state, function, 3);
    lua_setfield(state, tableIndex, name);
  };
  member("setRequestMethod", legacySetRequestMethod);
  member("setConnectTimeout", legacySetConnectTimeout);
  member("connect", legacyConnect);
  member("getResponseCode", legacyGetResponseCode);
  member("getInputStream", legacyGetInputStream);
  installClosedMemberMetatable(state, impl, legacyConnectionMemberDenied);
}

int legacyNewInstance(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  std::size_t classSize = 0;
  const char *className = luaL_checklstring(state, 1, &classSize);
  const std::string_view requested(className, classSize);
  if (requested == "java.net.URL") {
    std::size_t urlSize = 0;
    const char *url = luaL_checklstring(state, 2, &urlSize);
    try {
      pushLegacyUrlObject(state, impl, std::string_view(url, urlSize));
      return 1;
    } catch (...) {
      return luaL_error(state,
                        "Legacy Lua skin HTTP connection failed: allocation failed");
    }
  }
  if (requested == "java.io.InputStreamReader") {
    lua_pushvalue(state, 2);
    return 1;
  }
  if (requested == "java.io.BufferedReader") {
    if (!lua_istable(state, 2)) {
      impl->reportLegacyDenial("java.io.BufferedReader.constructor");
      return luaL_error(state, "Legacy Lua skin reader access denied");
    }
    lua_pushvalue(state, 2);
    return 1;
  }
  impl->reportLegacyDenial("java.net.URL.constructor");
  return luaL_error(state, "Legacy Lua skin constructor access denied: %s",
                    className);
}

enum class LegacyListStatus : std::uint8_t {
  Success,
  OrdinaryFailure,
};

LegacyListStatus pushLegacyList(lua_State *state, LuaSkinHostModulesImpl &impl,
                                std::string_view path) {
  const auto listed = impl.fileSystem->list(
      path, {}, std::numeric_limits<std::size_t>::max());
  if (listed.failure) {
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
  if (status == LegacyListStatus::OrdinaryFailure) {
    return 0;
  }
  return 1;
}

bool performLegacyMkdir(LuaSkinHostModulesImpl &impl,
                        std::string_view path) {
  const auto result = impl.fileSystem->mkdirData(path, false);
  if (!result.failure) {
    return true;
  }
  return false;
}

int legacyMkdir(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state, 1);
  if (lua_gettop(state) != 1 || !sameUpvalueTable(state, 1, 3)) {
    impl->reportLegacyDenial("java.io.File.mkdir");
    return raiseStoredError(state, impl);
  }
  std::size_t pathSize = 0;
  const char *path = lua_tolstring(state, lua_upvalueindex(2), &pathSize);
  const bool created =
      performLegacyMkdir(*impl, std::string_view(path, pathSize));
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

int legacyInputMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("com.badlogic.gdx.Input.member");
  return raiseStoredError(state, impl);
}

int legacyControllersMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("com.badlogic.gdx.Controllers.member");
  return raiseStoredError(state, impl);
}

int legacyControllerMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("com.badlogic.gdx.Controller.member");
  return raiseStoredError(state, impl);
}

int legacyControllerListMemberDenied(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  impl->reportLegacyDenial("com.badlogic.gdx.ControllerList.member");
  return raiseStoredError(state, impl);
}

bool legacyInputReceiver(lua_State *state, int arguments) {
  return lua_gettop(state) == arguments && sameUpvalueTable(state, 1, 2);
}

int legacyGraphicsWidth(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (!legacyInputReceiver(state, 1)) {
    impl->reportLegacyDenial("com.badlogic.gdx.Gdx.graphics.getWidth");
    return raiseStoredError(state, impl);
  }
  lua_pushinteger(state, impl->legacyInputHost->drawableWidth());
  return 1;
}

int legacyGraphicsHeight(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (!legacyInputReceiver(state, 1)) {
    impl->reportLegacyDenial("com.badlogic.gdx.Gdx.graphics.getHeight");
    return raiseStoredError(state, impl);
  }
  lua_pushinteger(state, impl->legacyInputHost->drawableHeight());
  return 1;
}

int legacyIsKeyPressed(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (!legacyInputReceiver(state, 2)) {
    impl->reportLegacyDenial("com.badlogic.gdx.Gdx.input.isKeyPressed");
    return raiseStoredError(state, impl);
  }
  const int key = static_cast<int>(luaL_checkinteger(state, 2));
  lua_pushboolean(state, impl->legacyInputHost->isKeyPressed(key));
  return 1;
}

int legacyKeyLookup(lua_State *state) {
  std::size_t size = 0;
  const char *name = luaL_checklstring(state, 2, &size);
  lua_pushinteger(
      state, LuaSkinLegacyInputHost::keyCode(std::string_view(name, size)));
  return 1;
}

int legacyControllerGetName(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (!legacyInputReceiver(state, 1)) {
    impl->reportLegacyDenial("com.badlogic.gdx.Controller.getName");
    return raiseStoredError(state, impl);
  }
  lua_pushvalue(state, lua_upvalueindex(3));
  return 1;
}

int legacyControllerGetButton(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (!legacyInputReceiver(state, 2)) {
    impl->reportLegacyDenial("com.badlogic.gdx.Controller.getButton");
    return raiseStoredError(state, impl);
  }
  const int button = static_cast<int>(luaL_checkinteger(state, 2));
  lua_pushvalue(state, lua_upvalueindex(3));
  lua_pushinteger(state, button);
  lua_rawget(state, -2);
  const bool pressed = lua_toboolean(state, -1) != 0;
  lua_pop(state, 2);
  lua_pushboolean(state, pressed);
  return 1;
}

void pushLegacyControllerObject(
    lua_State *state, LuaSkinHostModulesImpl *impl,
    std::size_t controllerIndex) {
  lua_createtable(state, 0, 2);
  const int objectIndex = lua_gettop(state);

  lua_pushlightuserdata(state, impl);
  lua_pushvalue(state, objectIndex);
  const std::string_view name =
      impl->legacyInputHost->controllerName(controllerIndex);
  lua_pushlstring(state, name.data(), name.size());
  lua_pushcclosure(state, legacyControllerGetName, 3);
  lua_setfield(state, objectIndex, "getName");

  lua_newtable(state);
  const int buttonsIndex = lua_gettop(state);
  for (std::size_t button = 0;
       button < input::kLegacyInputMaximumButtons; ++button) {
    if (impl->legacyInputHost->controllerButtonPressed(
            controllerIndex, static_cast<int>(button))) {
      lua_pushboolean(state, 1);
      lua_rawseti(state, buttonsIndex, static_cast<lua_Integer>(button));
    }
  }
  lua_pushlightuserdata(state, impl);
  lua_pushvalue(state, objectIndex);
  lua_pushvalue(state, buttonsIndex);
  lua_pushcclosure(state, legacyControllerGetButton, 3);
  lua_setfield(state, objectIndex, "getButton");
  lua_pop(state, 1);

  installClosedMemberMetatable(state, impl, legacyControllerMemberDenied);
}

int legacyControllerListFirst(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (!legacyInputReceiver(state, 1)) {
    impl->reportLegacyDenial("com.badlogic.gdx.ControllerList.first");
    return raiseStoredError(state, impl);
  }
  lua_pushvalue(state, lua_upvalueindex(3));
  return 1;
}

int legacyGetControllers(lua_State *state) {
  LuaSkinHostModulesImpl *impl = host(state);
  if (!legacyInputReceiver(state, 1)) {
    impl->reportLegacyDenial("com.badlogic.gdx.Controllers.getControllers");
    return raiseStoredError(state, impl);
  }

  lua_createtable(state, 0, 2);
  const int listIndex = lua_gettop(state);
  lua_pushinteger(
      state, static_cast<lua_Integer>(impl->legacyInputHost->controllerCount()));
  lua_setfield(state, listIndex, "size");
  if (impl->legacyInputHost->controllerCount() != 0) {
    pushLegacyControllerObject(state, impl, 0);
  } else {
    lua_pushnil(state);
  }
  lua_pushlightuserdata(state, impl);
  lua_pushvalue(state, listIndex);
  lua_pushvalue(state, -3);
  lua_pushcclosure(state, legacyControllerListFirst, 3);
  lua_setfield(state, listIndex, "first");
  lua_pop(state, 1);
  installClosedMemberMetatable(state, impl,
                               legacyControllerListMemberDenied);
  return 1;
}

void installLegacyInputMethod(lua_State *state,
                              LuaSkinHostModulesImpl *impl,
                              int receiverIndex, const char *name,
                              lua_CFunction function) {
  lua_pushlightuserdata(state, impl);
  lua_pushvalue(state, receiverIndex);
  lua_pushcclosure(state, function, 2);
  lua_setfield(state, receiverIndex, name);
}

void pushLegacyGdxClass(lua_State *state, LuaSkinHostModulesImpl *impl) {
  lua_createtable(state, 0, 2);
  const int gdxIndex = lua_gettop(state);

  lua_createtable(state, 0, 2);
  const int graphicsIndex = lua_gettop(state);
  installLegacyInputMethod(state, impl, graphicsIndex, "getWidth",
                           legacyGraphicsWidth);
  installLegacyInputMethod(state, impl, graphicsIndex, "getHeight",
                           legacyGraphicsHeight);
  installClosedMemberMetatable(state, impl, legacyGdxMember);
  lua_setfield(state, gdxIndex, "graphics");

  lua_createtable(state, 0, 1);
  const int inputIndex = lua_gettop(state);
  installLegacyInputMethod(state, impl, inputIndex, "isKeyPressed",
                           legacyIsKeyPressed);
  installClosedMemberMetatable(state, impl, legacyGdxMember);
  lua_setfield(state, gdxIndex, "input");
  installClosedMemberMetatable(state, impl, legacyGdxMember);
}

void pushLegacyInputClass(lua_State *state, LuaSkinHostModulesImpl *impl) {
  lua_createtable(state, 0, 1);
  const int inputIndex = lua_gettop(state);
  lua_newtable(state);
  lua_createtable(state, 0, 2);
  installClosure(state, impl, legacyKeyLookup);
  lua_setfield(state, -2, "__index");
  lua_pushboolean(state, 0);
  lua_setfield(state, -2, "__metatable");
  lua_setmetatable(state, -2);
  lua_setfield(state, inputIndex, "Keys");
  installClosedMemberMetatable(state, impl, legacyInputMemberDenied);
}

void pushLegacyControllersClass(lua_State *state,
                                LuaSkinHostModulesImpl *impl) {
  lua_createtable(state, 0, 1);
  const int controllersIndex = lua_gettop(state);
  installLegacyInputMethod(state, impl, controllersIndex, "getControllers",
                           legacyGetControllers);
  installClosedMemberMetatable(state, impl, legacyControllersMemberDenied);
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
  if (requested == "com.badlogic.gdx.Input") {
    lua_rawgeti(state, LUA_REGISTRYINDEX, impl->inputTokenReference);
    return 1;
  }
  if (requested == "com.badlogic.gdx.controllers.Controllers") {
    lua_rawgeti(state, LUA_REGISTRYINDEX, impl->controllersTokenReference);
    return 1;
  }
  if (requested == "com.badlogic.gdx.controllers.Controller") {
    lua_rawgeti(state, LUA_REGISTRYINDEX, impl->controllerTokenReference);
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
  const auto resolved = impl->fileSystem->normalizeVirtualPath(
      std::string_view(path, pathSize), true);
  if (resolved.failure || !resolved.normalizedVirtualPath) {
    if (resolved.failure) {
      impl->storeFileError(*resolved.failure);
    } else {
      impl->storeError("skin_lua_file_operation_failed",
                       "legacy File path could not be resolved");
    }
    return raiseStoredError(state, impl);
  }
  pushLegacyFileObject(state, impl, resolved.normalizedVirtualPath->data(),
                       resolved.normalizedVirtualPath->size());
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

void installTableCompatibility(lua_State *state) {
  lua_getglobal(state, LUA_TABLIBNAME);
  lua_getfield(state, -1, "unpack");
  const bool hasTableUnpack = !lua_isnil(state, -1);
  lua_pop(state, 1);
  if (!hasTableUnpack) {
    lua_getglobal(state, "unpack");
    lua_setfield(state, -2, "unpack");
  }
  lua_pop(state, 1);
}

void installSafeOsLibrary(lua_State *state, bool allowProcessGlobalOperations) {
  openLibrary(state, LUA_OSLIBNAME, luaopen_os);
  if (allowProcessGlobalOperations) {
    return;
  }
  lua_getglobal(state, LUA_OSLIBNAME);
  for (const char *name : {"execute", "exit", "getenv", "remove", "rename",
                           "setlocale", "tmpname"}) {
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
  lua_createtable(state, 0, 7);
  lua_pushcfunction(state, fileLines);
  lua_setfield(state, -2, "lines");
  lua_pushcfunction(state, fileRead);
  lua_setfield(state, -2, "read");
  lua_pushcfunction(state, fileWrite);
  lua_setfield(state, -2, "write");
  lua_pushcfunction(state, fileSeek);
  lua_setfield(state, -2, "seek");
  lua_pushcfunction(state, fileFlush);
  lua_setfield(state, -2, "flush");
  lua_pushcfunction(state, fileSetvbuf);
  lua_setfield(state, -2, "setvbuf");
  lua_pushcfunction(state, fileClose);
  lua_setfield(state, -2, "close");
  lua_setfield(state, -2, "__index");
  lua_pop(state, 1);
}

void installLegacyHttpMetatables(lua_State *state) {
  luaL_newmetatable(state, kLegacyHttpConnectionMetatable);
  lua_pushcfunction(
      state, legacyHttpSharedGc<SharedLegacyHttpConnection>);
  lua_setfield(state, -2, "__gc");
  lua_pushboolean(state, 0);
  lua_setfield(state, -2, "__metatable");
  lua_pop(state, 1);

  luaL_newmetatable(state, kLegacyHttpReaderMetatable);
  lua_pushcfunction(state, legacyHttpSharedGc<SharedLegacyHttpReader>);
  lua_setfield(state, -2, "__gc");
  lua_pushboolean(state, 0);
  lua_setfield(state, -2, "__metatable");
  lua_pop(state, 1);
}

int installHost(lua_State *state) {
  auto *impl = static_cast<LuaSkinHostModulesImpl *>(lua_touserdata(state, 1));
  openLibrary(state, "", luaopen_base);
  openLibrary(state, LUA_LOADLIBNAME, luaopen_package);
  openLibrary(state, LUA_TABLIBNAME, luaopen_table);
  installTableCompatibility(state);
  openLibrary(state, LUA_STRLIBNAME, luaopen_string);
  openLibrary(state, LUA_MATHLIBNAME, luaopen_math);
  installSafeOsLibrary(state, impl->allowProcessGlobalOperations);

  for (const char *name : {"ffi", "jit", "debug", "bit"}) {
    setNilGlobal(state, name);
  }
  for (const char *name : {"collectgarbage", "gcinfo", "newproxy", "module"}) {
    setNilGlobal(state, name);
  }

  installClosure(state, impl, doFile);
  lua_setglobal(state, "dofile");
  installClosure(state, impl, loadFile);
  lua_setglobal(state, "loadfile");
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
  lua_pushlstring(state, impl->initialPackagePath.data(),
                  impl->initialPackagePath.size());
  lua_setfield(state, -2, "path");
  lua_createtable(state, 1, 0);
  installClosure(state, impl, moduleLoader);
  lua_rawseti(state, -2, 1);
  lua_setfield(state, -2, "loaders");
  lua_pop(state, 1);

  installBit32(state);
  installFileMetatable(state);
  installLegacyHttpMetatables(state);
  lua_newtable(state);
  lua_setglobal(state, "main_state");

  lua_createtable(state, 0, 2);
  installClosure(state, impl, ioOpen);
  lua_setfield(state, -2, "open");
  installClosure(state, impl, ioLines);
  lua_setfield(state, -2, "lines");
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
  pushLegacyGdxClass(state, impl);
  impl->gdxTokenReference = luaL_ref(state, LUA_REGISTRYINDEX);
  pushLegacyInputClass(state, impl);
  impl->inputTokenReference = luaL_ref(state, LUA_REGISTRYINDEX);
  pushLegacyControllersClass(state, impl);
  impl->controllersTokenReference = luaL_ref(state, LUA_REGISTRYINDEX);
  lua_newtable(state);
  installClosedMemberMetatable(state, impl, legacyControllerMemberDenied);
  impl->controllerTokenReference = luaL_ref(state, LUA_REGISTRYINDEX);

  lua_createtable(state, 0, 2);
  installClosure(state, impl, legacyBindClass);
  lua_setfield(state, -2, "bindClass");
  installClosure(state, impl, legacyNew);
  lua_setfield(state, -2, "new");
  installClosure(state, impl, legacyNewInstance);
  lua_setfield(state, -2, "newInstance");
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
  impl->httpTransport = options.httpTransport;
  impl->audioHost = options.audioHost;
  impl->legacyInputHost = options.legacyInputHost;
  if (impl->legacyInputHost == nullptr) {
    try {
      impl->ownedLegacyInputHost =
          std::make_unique<LuaSkinLegacyInputHost>();
      impl->legacyInputHost = impl->ownedLegacyInputHost.get();
    } catch (...) {
      return {.failure =
                  diagnostic("skin_lua_runtime_create_failed",
                             "Lua legacy-input host allocation failed")};
    }
  }
  impl->maximumSourceBytes = options.maximumSourceBytes;
  impl->maximumModuleSearchTemplates = options.maximumModuleSearchTemplates;
  impl->allowProcessGlobalOperations = options.allowProcessGlobalOperations;
  impl->coroutineContext = options.coroutineContext;
  impl->coroutineCreated = options.coroutineCreated;
  try {
    // LuaSkinAccessor.setDirectory() preserves Luaj's default `?.lua` then
    // appends the selected file's Beatoraja-relative directory.  Some real
    // skins intentionally inspect this string, so do not substitute absolute
    // native paths here.
    const std::string_view entryPath =
        options.fileSystem->entry().packageRelativePath;
    const std::size_t slash = entryPath.rfind('/');
    const std::string_view entryDirectory = slash == std::string_view::npos
                                                ? std::string_view{}
                                                : entryPath.substr(0, slash);
    std::string virtualDirectory = "skin/";
    virtualDirectory += options.fileSystem->entry().package.directoryName;
    if (!entryDirectory.empty()) {
      virtualDirectory.push_back('/');
      virtualDirectory.append(entryDirectory);
    }
    impl->configurationPathPrefix = virtualDirectory;
    impl->initialPackagePath = "?.lua;" + virtualDirectory + "/?.lua";
  } catch (...) {
    return {.failure =
                diagnostic("skin_lua_runtime_create_failed",
                           "Lua virtual package path allocation failed")};
  }
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

void LuaSkinHostModules::setFrameCallbackActive(bool active) noexcept {
  if (impl_) {
    impl_->frameCallbackActive = active;
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
  // Beatoraja keeps RestrictedIoLib handles live across gameplay.  Retain the
  // compatibility hook for callers compiled against the old host surface,
  // but do not add an app-specific invalidation boundary.
  return {};
}

std::span<const SkinCompatibilityDiagnostic>
LuaSkinHostModules::diagnostics() const noexcept {
  return impl_->diagnostics.entries();
}

} // namespace skin

#endif
