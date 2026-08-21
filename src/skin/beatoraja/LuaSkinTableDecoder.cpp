#include "LuaSkinTableDecoder.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "LuaSkinBindingDecoder.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinRuntime.h"
#include "NumericGlyphAtlas.h"
#include "SkinGaugeNodeExpansion.h"
#include "SkinCoverNormalization.h"
#include "SkinJudgeNormalization.h"
#include "SkinJudgeNumberNormalization.h"
#include "SkinNoteLineNormalization.h"
#include "SkinObjectResolutionPrecedence.h"
#include "SkinNoteLaneGeometryNormalization.h"
#include "SkinNoteLineNormalization.h"
#include "SkinNoteNormalization.h"
#include "SkinTextGraphNormalization.h"
#include "../GameplaySkinTraits.h"
#include "../package/SkinPackageTypes.h"

extern "C" {
#include <lua.h>
}

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skin {
namespace {

constexpr int kPlay7KeysType = 0;
constexpr int kMaximumPinnedSkinType = 18;

SkinDiagnostic diagnostic(std::string code, std::string message,
                          std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

std::optional<std::string> normalizeNfc(std::string_view value,
                                        bool allowEmpty = false) {
  if ((!allowEmpty && value.empty()) ||
      value.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  utf8proc_uint8_t *mapped = nullptr;
  const auto size = utf8proc_map(
      reinterpret_cast<const utf8proc_uint8_t *>(value.data()),
      static_cast<utf8proc_ssize_t>(value.size()), &mapped,
      static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
  if (size < 0 || mapped == nullptr) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char *>(mapped),
                     static_cast<std::size_t>(size));
  std::free(mapped);
  if (!allowEmpty && result.empty()) {
    return std::nullopt;
  }
  return result;
}

bool asciiCaseEqual(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto fold = [](unsigned char value) {
      return value >= 'A' && value <= 'Z'
                 ? static_cast<unsigned char>(value - 'A' + 'a')
                 : value;
    };
    if (fold(static_cast<unsigned char>(left[index])) !=
        fold(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

std::optional<std::string>
normalizedFilePatternComponent(std::string_view pattern) {
  const std::size_t slash = pattern.rfind('/');
  const std::string_view component =
      slash == std::string_view::npos ? pattern : pattern.substr(slash + 1);
  const std::size_t star = component.find('*');
  if (star == std::string_view::npos ||
      component.find('*', star + 1) != std::string_view::npos) {
    return std::nullopt;
  }

  const std::size_t firstAlternative = component.find('|');
  if (firstAlternative == std::string_view::npos) {
    return std::string(component);
  }
  const std::size_t lastAlternative = component.rfind('|');
  if (firstAlternative <= star || lastAlternative == firstAlternative ||
      lastAlternative == firstAlternative + 1 ||
      component.find('|', firstAlternative + 1) != lastAlternative) {
    return std::nullopt;
  }
  std::string normalized(component.substr(0, firstAlternative));
  normalized.append(component.substr(lastAlternative + 1));
  return normalized;
}

bool validPattern(std::string_view pattern) {
  if (pattern.empty() || pattern.front() == '/' ||
      pattern.find('\\') != std::string_view::npos ||
      (pattern.size() >= 2 && pattern[1] == ':')) {
    return false;
  }
  const std::size_t star = pattern.find('*');
  if (star == std::string_view::npos ||
      pattern.find('*', star + 1) != std::string_view::npos ||
      pattern.find('/', star) != std::string_view::npos ||
      !normalizedFilePatternComponent(pattern)) {
    return false;
  }
  std::size_t start = 0;
  while (start <= pattern.size()) {
    const std::size_t end = pattern.find('/', start);
    const std::string_view component = pattern.substr(
        start, (end == std::string_view::npos ? pattern.size() : end) - start);
    // Beatoraja resolves custom-file patterns from the selected entry's
    // directory. Leave dot components to LuaSkinFileSystem, which normalizes
    // them against that directory and rejects package escapes.
    if (component.empty()) {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

struct DecodeRequest {
  HeaderDecodeResult result;
  std::size_t entries = 0;
  std::size_t objects = 0;
  std::size_t copiedTextBytes = 0;
  bool enforceGameplayLimits = false;
  bool allocationFailed = false;
};

bool fail(DecodeRequest &request, std::string code, std::string message) {
  if (request.result.diagnostics.empty()) {
    request.result.diagnostics.push_back(
        diagnostic(std::move(code), std::move(message)));
  }
  return false;
}

int absoluteIndex(lua_State *state, int index) {
  return index > 0 || index <= LUA_REGISTRYINDEX
             ? index
             : lua_gettop(state) + index + 1;
}

bool requireObject(lua_State *state, int index, std::size_t depth,
                   DecodeRequest &request) {
  if (request.enforceGameplayLimits &&
      depth > LuaSkinTableDecoderPolicy::maxDepth) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed depth limit");
  }
  if (!lua_istable(state, index)) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header object field is not a table");
  }
  ++request.objects;
  if (request.enforceGameplayLimits &&
      request.objects > LuaSkinTableDecoderPolicy::maxDecodedObjects) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed object limit");
  }
  if (!lua_checkstack(state, 3)) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed stack limit");
  }
  const int tableIndex = absoluteIndex(state, index);
  lua_pushnil(state);
  while (lua_next(state, tableIndex) != 0) {
    ++request.entries;
    if (request.enforceGameplayLimits &&
        request.entries > LuaSkinTableDecoderPolicy::maxEntries) {
      return fail(request, "skin_lua_header_limit_exceeded",
                  "Lua skin header exceeds the fixed entry limit");
    }
    lua_pop(state, 1);
  }
  return true;
}

bool rawGetField(lua_State *state, int index, std::string_view name,
                 DecodeRequest &request) {
  if (!lua_checkstack(state, 2)) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed stack limit");
  }
  const int tableIndex = absoluteIndex(state, index);
  lua_pushlstring(state, name.data(), name.size());
  lua_rawget(state, tableIndex);
  return true;
}

bool copyString(lua_State *state, int index, std::string &output,
                std::optional<std::size_t> maximumBytes, bool allowEmpty,
                DecodeRequest &request) {
  if (!request.enforceGameplayLimits) {
    switch (lua_type(state, index)) {
    case LUA_TNIL:
      output.clear();
      return true;
    case LUA_TBOOLEAN:
      output = lua_toboolean(state, index) != 0 ? "true" : "false";
      return true;
    case LUA_TSTRING:
    case LUA_TNUMBER: {
      std::size_t size = 0;
      const char *value = lua_tolstring(state, index, &size);
      if (value == nullptr) {
        return false;
      }
      output.assign(value, size);
      return true;
    }
    default:
      output = lua_typename(state, lua_type(state, index));
      return true;
    }
  }
  if (lua_isnil(state, index)) {
    return allowEmpty ||
           fail(request, "skin_lua_header_invalid",
                "Lua skin header required string field is missing");
  }
  if (lua_type(state, index) != LUA_TSTRING &&
      lua_type(state, index) != LUA_TNUMBER) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header string field has an invalid type");
  }
  std::size_t size = 0;
  const char *value = lua_tolstring(state, index, &size);
  if (value == nullptr || (maximumBytes && size > *maximumBytes) ||
      (maximumBytes &&
       request.copiedTextBytes >
           LuaSkinTableDecoderPolicy::maxGameplayTextBytes -
               std::min(size,
                        LuaSkinTableDecoderPolicy::maxGameplayTextBytes))) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header text exceeds its fixed byte limit (size=" +
                    std::to_string(size) + ", maximum=" +
                    std::to_string(maximumBytes.value_or(0)) + ")");
  }
  output.assign(value, size);
  auto normalized = normalizeNfc(output, allowEmpty);
  if (!normalized || (maximumBytes && normalized->size() > *maximumBytes)) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header text is not valid normalized UTF-8");
  }
  if (maximumBytes) {
    request.copiedTextBytes += normalized->size();
  }
  output = std::move(*normalized);
  return true;
}

bool stringField(lua_State *state, int index, std::string_view name,
                 std::string &output, std::optional<std::size_t> maximumBytes,
                 bool allowEmpty, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  const bool ok =
      copyString(state, -1, output, maximumBytes, allowEmpty, request);
  if (!ok && !request.result.diagnostics.empty() &&
      request.result.diagnostics.front().code ==
          "skin_lua_header_limit_exceeded") {
    request.result.diagnostics.front().message +=
        " [field: " + std::string(name) + "]";
  }
  lua_pop(state, 1);
  return ok;
}

bool integerAt(lua_State *state, int index, int &output,
               DecodeRequest &request) {
  if (lua_isnil(state, index)) {
    return true;
  }
  if (lua_isnumber(state, index) == 0) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header integer field has an invalid type");
  }
  const double value = static_cast<double>(lua_tonumber(state, index));
  const double truncated = std::trunc(value);
  if (!std::isfinite(value) ||
      truncated < static_cast<double>(std::numeric_limits<int>::min()) ||
      truncated > static_cast<double>(std::numeric_limits<int>::max())) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header integer is outside signed 32-bit range");
  }
  output = static_cast<int>(truncated);
  return true;
}

bool integerField(lua_State *state, int index, std::string_view name,
                  int &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  const bool ok = integerAt(state, -1, output, request);
  lua_pop(state, 1);
  return ok;
}

bool permissionField(lua_State *state, int index, std::string_view name,
                     OffsetPermissionMask permission,
                     OffsetPermissionMask &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_toboolean(state, -1) != 0) {
    output = static_cast<OffsetPermissionMask>(output | permission);
  }
  lua_pop(state, 1);
  return true;
}

bool strictArrayLength(lua_State *state, int index,
                       std::optional<std::size_t> maximum, std::size_t &length,
                       DecodeRequest &request) {
  if (!request.enforceGameplayLimits) {
    maximum.reset();
  }
  if (!lua_istable(state, index)) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header array field is not a table");
  }
  if (!lua_checkstack(state, 3)) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed stack limit");
  }
  const int tableIndex = absoluteIndex(state, index);
  std::size_t count = 0;
  std::size_t maximumIndex = 0;
  lua_pushnil(state);
  while (lua_next(state, tableIndex) != 0) {
    ++request.entries;
    ++count;
    if ((request.enforceGameplayLimits &&
         request.entries > LuaSkinTableDecoderPolicy::maxEntries) ||
        (maximum && count > *maximum) || lua_type(state, -2) != LUA_TNUMBER) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin header array exceeds its limit or has mixed keys");
    }
    const double numeric = static_cast<double>(lua_tonumber(state, -2));
    if (!std::isfinite(numeric) || std::trunc(numeric) != numeric ||
        numeric < 1.0 || (maximum && numeric > static_cast<double>(*maximum))) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin header array key is not a positive integer");
    }
    maximumIndex = std::max(maximumIndex, static_cast<std::size_t>(numeric));
    lua_pop(state, 1);
  }
  if (maximumIndex != count) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header array contains a hole");
  }
  length = count;
  return true;
}

template <typename DecodeValue>
bool forEachHeaderTableValue(lua_State *state, int index,
                             DecodeRequest &request, DecodeValue decodeValue) {
  if (!lua_istable(state, index)) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header array field is not a table");
  }
  if (!lua_checkstack(state, 3)) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed stack limit");
  }
  const int tableIndex = absoluteIndex(state, index);
  lua_pushnil(state);
  while (lua_next(state, tableIndex) != 0) {
    ++request.entries;
    const bool ok = decodeValue(state, -1);
    lua_pop(state, 1);
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool decodeStringArray(lua_State *state, int index, std::size_t depth,
                       std::optional<std::size_t> maximum,
                       std::vector<std::string> &output,
                       DecodeRequest &request) {
  if (request.enforceGameplayLimits &&
      depth > LuaSkinTableDecoderPolicy::maxDepth) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed depth limit");
  }
  if (!request.enforceGameplayLimits) {
    return forEachHeaderTableValue(
        state, index, request, [&](lua_State *state, int valueIndex) {
          output.emplace_back();
          return copyString(state, valueIndex, output.back(), std::nullopt,
                            false, request);
        });
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, index, maximum, length, request)) {
    return false;
  }
  output.reserve(length);
  const int tableIndex = absoluteIndex(state, index);
  for (std::size_t position = 1; position <= length; ++position) {
    lua_rawgeti(state, tableIndex, static_cast<int>(position));
    output.emplace_back();
    if (!copyString(state, -1, output.back(), std::nullopt, false, request)) {
      return false;
    }
    lua_pop(state, 1);
  }
  return true;
}

bool decodeCategory(lua_State *state, int index, std::size_t depth,
                    SkinHeaderCategory &output, DecodeRequest &request) {
  if (!requireObject(state, index, depth, request) ||
      !stringField(state, index, "name", output.name, std::nullopt, false,
                   request) ||
      !rawGetField(state, index, "item", request)) {
    return false;
  }
  bool ok = true;
  if (lua_istable(state, -1)) {
    ok = decodeStringArray(state, -1, depth + 1, std::nullopt, output.items,
                           request);
  }
  lua_pop(state, 1);
  return ok;
}

bool decodeChoice(lua_State *state, int index, std::size_t depth,
                  SkinHeaderOptionChoice &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "name", output.label, std::nullopt, false,
                     request) &&
         integerField(state, index, "op", output.value, request);
}

bool decodeOption(lua_State *state, int index, std::size_t depth,
                  SkinHeaderOption &output, DecodeRequest &request) {
  if (!requireObject(state, index, depth, request) ||
      !stringField(state, index, "category", output.category, std::nullopt,
                   true, request) ||
      !stringField(state, index, "name", output.name, std::nullopt, false,
                   request) ||
      !stringField(state, index, "def", output.defaultLabel, std::nullopt, true,
                   request) ||
      !rawGetField(state, index, "item", request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!request.enforceGameplayLimits) {
    const bool ok = forEachHeaderTableValue(
        state, -1, request, [&](lua_State *state, int valueIndex) {
          output.choices.emplace_back();
          return decodeChoice(state, valueIndex, depth + 2,
                              output.choices.back(), request);
        });
    lua_pop(state, 1);
    return ok;
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, -1, std::nullopt, length, request)) {
    return false;
  }
  output.choices.reserve(length);
  const int tableIndex = absoluteIndex(state, -1);
  for (std::size_t position = 1; position <= length; ++position) {
    lua_rawgeti(state, tableIndex, static_cast<int>(position));
    output.choices.emplace_back();
    if (!decodeChoice(state, -1, depth + 2, output.choices.back(), request)) {
      return false;
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

bool decodeFile(lua_State *state, int index, std::size_t depth,
                SkinHeaderFile &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "category", output.category, std::nullopt,
                     true, request) &&
         stringField(state, index, "name", output.name, std::nullopt, false,
                     request) &&
         stringField(state, index, "path", output.pattern, std::nullopt, false,
                     request) &&
         stringField(state, index, "def", output.defaultValue, std::nullopt,
                     true, request);
}

bool decodeOffset(lua_State *state, int index, std::size_t depth,
                  SkinHeaderOffset &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "category", output.category, std::nullopt,
                     true, request) &&
         stringField(state, index, "name", output.name, std::nullopt, false,
                     request) &&
         integerField(state, index, "id", output.id, request) &&
         permissionField(state, index, "x", kOffsetPermissionX,
                         output.permissions, request) &&
         permissionField(state, index, "y", kOffsetPermissionY,
                         output.permissions, request) &&
         permissionField(state, index, "w", kOffsetPermissionW,
                         output.permissions, request) &&
         permissionField(state, index, "h", kOffsetPermissionH,
                         output.permissions, request) &&
         permissionField(state, index, "r", kOffsetPermissionR,
                         output.permissions, request) &&
         permissionField(state, index, "a", kOffsetPermissionA,
                         output.permissions, request);
}

template <typename Output, typename DecodeElement>
bool decodeObjectArrayField(lua_State *state, int rootIndex,
                            std::string_view field, std::size_t depth,
                            std::optional<std::size_t> maximum,
                            std::vector<Output> &output, DecodeRequest &request,
                            DecodeElement decodeElement) {
  if (!rawGetField(state, rootIndex, field, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!request.enforceGameplayLimits) {
    const bool ok = forEachHeaderTableValue(
        state, -1, request, [&](lua_State *state, int valueIndex) {
          output.emplace_back();
          return decodeElement(state, valueIndex, depth + 1, output.back(),
                               request);
        });
    lua_pop(state, 1);
    return ok;
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, -1, maximum, length, request)) {
    return false;
  }
  output.reserve(length);
  const int tableIndex = absoluteIndex(state, -1);
  for (std::size_t position = 1; position <= length; ++position) {
    lua_rawgeti(state, tableIndex, static_cast<int>(position));
    output.emplace_back();
    if (!decodeElement(state, -1, depth + 1, output.back(), request)) {
      return false;
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

bool validateSemantics(BeatorajaSkinHeader &header, DecodeRequest &request) {
  if (header.type < 0 || header.type > kMaximumPinnedSkinType) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header does not declare a valid type");
  }
  if (header.width < 1 || header.height < 1) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header dimensions are outside the fixed range");
  }

  if (header.type != kPlay7KeysType) {
    return true;
  }
  const SkinHeaderOffset synthesized[] = {
      {.name = "All offset(%)", .id = 10, .permissions = 0x0f},
      {.name = "Notes offset", .id = 30, .permissions = kOffsetPermissionH},
      {.name = "Judge offset", .id = 32, .permissions = 0x2f},
      {.name = "Judge Detail offset", .id = 33, .permissions = 0x2f},
  };
  for (const auto &offset : synthesized) {
    header.offsets.push_back(offset);
  }
  return true;
}

void decodeHeaderProtected(lua_State *state, int index, void *opaque) noexcept {
  auto *request = static_cast<DecodeRequest *>(opaque);
  if (request == nullptr) {
    return;
  }
  try {
    request->result.header.emplace();
    auto &header = *request->result.header;
    if (!requireObject(state, index, 1, *request) ||
        !integerField(state, index, "type", header.type, *request) ||
        !integerField(state, index, "w", header.width, *request) ||
        !integerField(state, index, "h", header.height, *request) ||
        !stringField(state, index, "name", header.name, std::nullopt, true,
                     *request) ||
        !stringField(state, index, "author", header.author, std::nullopt, true,
                     *request) ||
        !decodeObjectArrayField(state, index, "category", 1, std::nullopt,
                                header.categories, *request, decodeCategory) ||
        !decodeObjectArrayField(state, index, "property", 1, std::nullopt,
                                header.options, *request, decodeOption) ||
        !decodeObjectArrayField(state, index, "filepath", 1, std::nullopt,
                                header.files, *request, decodeFile) ||
        !decodeObjectArrayField(state, index, "offset", 1, std::nullopt,
                                header.offsets, *request, decodeOffset) ||
        !validateSemantics(header, *request)) {
      request->result.header.reset();
    }
  } catch (...) {
    request->allocationFailed = true;
    request->result.header.reset();
  }
}

ConfigOffset sanitizeOffset(ConfigOffset value,
                            OffsetPermissionMask permissions) {
  value.x = (permissions & kOffsetPermissionX) != 0 ? value.x : 0;
  value.y = (permissions & kOffsetPermissionY) != 0 ? value.y : 0;
  value.w = (permissions & kOffsetPermissionW) != 0 ? value.w : 0;
  value.h = (permissions & kOffsetPermissionH) != 0 ? value.h : 0;
  value.r = (permissions & kOffsetPermissionR) != 0 ? value.r : 0;
  value.a = (permissions & kOffsetPermissionA) != 0 ? value.a : 0;
  return value;
}

bool matchesFilePattern(std::string_view pattern, std::string_view filename) {
  const auto normalized = normalizedFilePatternComponent(pattern);
  if (!normalized) {
    return false;
  }
  const std::size_t star = normalized->find('*');
  const std::string_view prefix(normalized->data(), star);
  const std::string_view suffix(normalized->data() + star + 1,
                                normalized->size() - star - 1);
  return filename.size() >= prefix.size() + suffix.size() &&
         asciiCaseEqual(filename.substr(0, prefix.size()), prefix) &&
         asciiCaseEqual(filename.substr(filename.size() - suffix.size()),
                        suffix);
}

std::string filenameOf(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path
                                                     : path.substr(slash + 1));
}

std::string stemOf(std::string_view filename) {
  const std::size_t dot = filename.rfind('.');
  return std::string(dot == std::string_view::npos ? filename
                                                   : filename.substr(0, dot));
}

std::string substitutePattern(std::string_view pattern,
                              std::string_view selected) {
  const std::size_t star = pattern.find('*');
  std::string result(pattern.substr(0, star));
  result.append(selected);
  return result;
}

std::size_t chooseRandomIndex(std::size_t count) {
  if (count == 0) {
    return 0;
  }
  std::random_device entropy;
  std::mt19937 generator(entropy());
  std::uniform_int_distribution<std::size_t> distribution(0, count - 1);
  return distribution(generator);
}

std::string chooseRandomFile(const std::vector<std::string> &choices) {
  if (choices.empty()) {
    return {};
  }
  return choices[chooseRandomIndex(choices.size())];
}

struct RawSkinSource {
  std::string id;
  std::string path;
};

struct RawSkinFontFallback {
  std::string path;
  int type = 0;
};

struct RawSkinFont {
  std::string id;
  std::string path;
  std::vector<RawSkinFontFallback> fallbacks;
  int type = 0;
};

struct RawSkinImage {
  std::string id;
  std::string source;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int divisionsX = 1;
  int divisionsY = 1;
  int cycleMillis = 0;
  int stateCount = 0;
  int stateSelector = 0;
  int clickMode = 0;
  std::uint32_t authoredIndex = 0;
  std::optional<SkinTimerPropertyId> timer;
  std::optional<SkinIntegerPropertyId> stateIndex;
  std::optional<SkinEventBindingId> clickEvent;
  SkinSpriteFrames sprite;
};

struct RawSkinImageSet {
  std::string id;
  int stateSelector = 0;
  std::vector<std::string> imageIds;
  int clickMode = 0;
  std::uint32_t authoredIndex = 0;
  SkinIntegerPropertyId stateIndex{};
  std::optional<SkinEventBindingId> clickEvent;
};

struct RawSkinNumber {
  RawSkinImage image;
  SkinIntegerPropertyId value{};
  int digitCount = 0;
  int alignment = 0;
  int padding = 0;
  int zeroPadding = 0;
  int spacing = 0;
  std::vector<SkinDigitOffset> perDigitOffsets;
};

struct RawSkinFloat {
  RawSkinImage image;
  SkinFloatPropertyId value{};
  int integerDigits = 0;
  int fractionalDigits = 0;
  int alignment = 0;
  int zeroPadding = 0;
  int spacing = 0;
  double gain = 1.0;
  bool signVisible = false;
  std::vector<SkinDigitOffset> perDigitOffsets;
};

struct RawSkinSlider {
  RawSkinImage image;
  int direction = 0;
  int range = 0;
  int typeSelector = 0;
  bool changeable = true;
  bool isRefNum = false;
  int minimum = 0;
  int maximum = 0;
  std::optional<SkinFloatPropertyId> explicitValue;
  std::optional<SkinFloatWriterId> writer;
  std::optional<SkinIntegerPropertyId> integerValue;
  std::optional<SkinFloatPropertyId> implicitValue;
};

struct RawSkinGauge {
  std::string id;
  std::vector<std::string> nodes;
  int parts = 50;
  int animationType = 0;
  int animationRange = 3;
  int animationCycleMillis = 33;
  int resultStartMillis = 0;
  int resultEndMillis = 500;
};

struct RawSkinText {
  std::string id;
  std::string font;
  int pointSize = 0;
  int alignment = 0;
  int refSelector = 0;
  std::uint32_t authoredIndex = 0;
  SkinStringPropertyId value{};
  std::optional<SkinStringWriterId> writer;
  bool writerFieldPresent = false;
  bool writerWasExplicit = false;
  std::string literal;
  bool editable = false;
  bool wrapping = false;
  int overflow = 0;
  std::string outlineColor = "ffffff00";
  double outlineWidth = 0.0;
  std::string shadowColor = "ffffff00";
  double shadowOffsetX = 0.0;
  double shadowOffsetY = 0.0;
  double shadowSmoothness = 0.0;
};

struct RawCustomTimer {
  int id = 0;
  std::uint32_t authoredIndex = 0;
  std::optional<SkinTimerPropertyId> timer;
};

struct RawCustomEvent {
  int id = 0;
  std::uint32_t authoredIndex = 0;
  SkinEventBindingId action{};
  std::optional<SkinBooleanPropertyId> condition;
  int minimumIntervalMillis = 0;
};

struct RawSkinGraph {
  RawSkinImage image;
  int direction = 1;
  int type = 0;
  bool isRefNum = false;
  int minimum = 0;
  int maximum = 0;
  std::optional<SkinFloatPropertyId> explicitValue;
  std::optional<SkinIntegerPropertyId> integerValue;
  std::optional<SkinFloatPropertyId> implicitValue;
};

struct RawSkinNoteLaneRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct RawSkinCover {
  RawSkinImage image;
  SkinCoverKind kind = SkinCoverKind::Hidden;
  int disappearLine = -1;
  bool disappearLineLinksLift = false;
};

struct RawDestinationFrame {
  std::optional<int> time;
  std::optional<int> x;
  std::optional<int> y;
  std::optional<int> width;
  std::optional<int> height;
  std::optional<int> angle;
  std::optional<int> acceleration;
  std::optional<int> alpha;
  std::optional<int> red;
  std::optional<int> green;
  std::optional<int> blue;
  std::optional<int> clipX;
  std::optional<int> clipY;
  std::optional<int> clipWidth;
  std::optional<int> clipHeight;
};

struct RawDestination {
  std::string id;
  struct Condition {
    std::optional<int> optionId;
    std::optional<SkinBooleanPropertyId> property;
  };
  std::uint32_t authoredIndex = 0;
  std::optional<SkinTimerPropertyId> timer;
  int loop = 0;
  int center = 0;
  int blend = 0;
  int filter = 0;
  int stretch = -1;
  int offset = 0;
  std::vector<int> offsets;
  std::vector<Condition> conditions;
  std::optional<SkinBooleanPropertyId> drawCondition;
  std::vector<RawDestinationFrame> frames;
  std::optional<SkinAuthoredRect> mouseRect;
};

struct RawSkinJudge {
  std::string id;
  int player = 0;
  bool shift = false;
  std::vector<RawDestination> images;
  std::vector<RawDestination> numbers;
};

struct RawSkinIdentity {
  std::string id;
};

struct RawSkinGaugeGraph {
  std::string id;
  std::optional<std::vector<std::optional<std::string>>> colors;
  std::string assistClearBackground = "440044";
  std::string assistEasyFailBackground = "004444";
  std::string grooveFailBackground = "004400";
  std::string grooveClearHardBackground = "440000";
  std::string exHardBackground = "444400";
  std::string hazardBackground = "444444";
  std::string assistClearLine = "ff00ff";
  std::string assistEasyFailLine = "00ffff";
  std::string grooveFailLine = "00ff00";
  std::string grooveClearHardLine = "ff0000";
  std::string exHardLine = "ffff00";
  std::string hazardLine = "cccccc";
  std::string borderLine = "ff0000";
  std::string borderBackground = "440000";
};

struct RawSkinBpmGraph {
  std::string id;
  int delayMillis = 0;
  int lineWidth = 2;
  std::string mainBpmColor = "00ff00";
  std::string minimumBpmColor = "0000ff";
  std::string maximumBpmColor = "ff0000";
  std::string otherBpmColor = "ffff00";
  std::string stopLineColor = "ff00ff";
  std::string transitionLineColor = "7f7f7f";
};

struct RawSkinNoteDistributionGraph {
  std::string id;
  int type = 0;
  int backTextureOff = 0;
  int delayMillis = 500;
  int reverseOrder = 0;
  int noGap = 0;
  int noHorizontalGap = 0;
};

struct RawSkinTimingVisualizer {
  std::string id;
  int width = 301;
  int judgeWidthMillis = 150;
  int lineWidth = 1;
  std::string lineColor = "00FF00FF";
  std::string centerColor = "FFFFFFFF";
  std::string pgColor = "000088FF";
  std::string grColor = "008800FF";
  std::string gdColor = "888800FF";
  std::string bdColor = "880000FF";
  std::string prColor = "000000FF";
  int transparent = 0;
  int drawDecay = 1;
};

struct RawSkinTimingDistributionGraph {
  std::string id;
  int width = 301;
  int lineWidth = 1;
  std::string graphColor = "00FF00FF";
  std::string averageColor = "FFFFFFFF";
  std::string devColor = "FFFFFFFF";
  std::string pgColor = "000088FF";
  std::string grColor = "008800FF";
  std::string gdColor = "888800FF";
  std::string bdColor = "880000FF";
  std::string prColor = "000000FF";
  int drawAverage = 1;
  int drawDev = 1;
};

struct RawSkinHitErrorVisualizer {
  std::string id;
  int width = 301;
  int judgeWidthMillis = 150;
  int lineWidth = 1;
  int colorMode = 1;
  int hitErrorMode = 1;
  int emaMode = 1;
  std::string lineColor = "99CCFF80";
  std::string centerColor = "FFFFFFFF";
  std::string pgColor = "99CCFF80";
  std::string grColor = "F2CB3080";
  std::string gdColor = "14CC8f80";
  std::string bdColor = "FF1AB380";
  std::string prColor = "CC292980";
  std::string emaColor = "FF0000FF";
  double alpha = 0.1;
  int windowLength = 30;
  int transparent = 0;
  int drawDecay = 1;
};

struct RawSkinPmChara {
  std::string id;
  std::string source;
  int color = 1;
  int type = std::numeric_limits<int>::min();
  int side = 1;
};

struct RawSkinNote {
  std::string id;
  std::vector<std::string> note;
  std::vector<std::string> mine;
  std::vector<std::string> lnEnd;
  std::vector<std::string> lnStart;
  std::vector<std::string> lnBody;
  std::vector<std::string> lnActive;
  std::optional<std::vector<std::string>> lnBodyActive;
  std::vector<std::string> hcnEnd;
  std::vector<std::string> hcnStart;
  std::vector<std::string> hcnBody;
  std::vector<std::string> hcnActive;
  std::vector<std::string> hcnDamage;
  std::vector<std::string> hcnReactive;
  std::optional<std::vector<std::string>> hcnBodyActive;
  std::vector<std::string> hcnBodyReactive;
  std::vector<std::string> hcnBodyMiss;
  std::vector<std::string> hidden;
  std::vector<std::string> processed;
  std::vector<RawSkinNoteLaneRect> laneRects;
  std::vector<double> noteHeights;
  std::optional<int> secondaryDestinationY;
  std::array<int, 2> expansionRatePercent{100, 100};
  std::vector<RawDestination> group;
  std::vector<RawDestination> bpm;
  std::vector<RawDestination> stop;
  std::vector<RawDestination> time;
  bool authoredHiddenOrProcessed = false;
  SkinNoteObject object;
};

struct GameplayDecodeRequest {
  DecodeRequest decoding;
  bool enforceGameplayLimits = true;
  BeatorajaSkinModelDecodeResult result;
  std::map<std::string, SkinResourceId, std::less<>> sourceIds;
  std::map<std::string, RawSkinImage, std::less<>> images;
  std::map<std::string, RawSkinImageSet, std::less<>> imageSets;
  std::map<std::string, RawSkinNumber, std::less<>> numbers;
  std::map<std::string, RawSkinFloat, std::less<>> floats;
  std::map<std::string, RawSkinSlider, std::less<>> sliders;
  std::map<std::string, RawSkinText, std::less<>> texts;
  std::map<std::string, RawSkinGraph, std::less<>> graphs;
  std::map<std::string, RawSkinGaugeGraph, std::less<>> gaugeGraphs;
  std::map<std::string, RawSkinCover, std::less<>> hiddenCovers;
  std::map<std::string, RawSkinCover, std::less<>> liftCovers;
  std::map<std::string, RawSkinJudge, std::less<>> judges;
  std::map<std::string, RawSkinBpmGraph, std::less<>> bpmGraphs;
  std::map<std::string, RawSkinHitErrorVisualizer, std::less<>>
      hitErrorVisualizers;
  std::map<std::string, RawSkinNoteDistributionGraph, std::less<>>
      judgeGraphs;
  std::map<std::string, RawSkinTimingVisualizer, std::less<>>
      timingVisualizers;
  std::map<std::string, RawSkinTimingDistributionGraph, std::less<>>
      timingDistributionGraphs;
  std::map<std::string, RawSkinPmChara, std::less<>> pmCharas;
  std::optional<RawSkinGauge> gauge;
  std::optional<RawSkinIdentity> bga;
  std::vector<RawSkinSource> rawSources;
  std::vector<RawSkinFont> rawFonts;
  std::vector<SkinFontResource> fonts;
  std::vector<RawSkinImage> rawImages;
  std::vector<RawSkinImageSet> rawImageSets;
  std::vector<RawSkinNumber> rawNumbers;
  std::vector<RawSkinFloat> rawFloats;
  std::vector<RawSkinSlider> rawSliders;
  std::vector<RawSkinText> rawTexts;
  std::vector<RawSkinGraph> rawGraphs;
  std::vector<RawSkinGaugeGraph> rawGaugeGraphs;
  std::vector<RawSkinCover> rawHiddenCovers;
  std::vector<RawSkinCover> rawLiftCovers;
  std::vector<RawSkinJudge> rawJudges;
  std::vector<RawSkinBpmGraph> rawBpmGraphs;
  std::vector<RawSkinHitErrorVisualizer> rawHitErrorVisualizers;
  std::vector<RawSkinNoteDistributionGraph> rawJudgeGraphs;
  std::vector<RawSkinTimingVisualizer> rawTimingVisualizers;
  std::vector<RawSkinTimingDistributionGraph> rawTimingDistributionGraphs;
  std::vector<RawSkinPmChara> rawPmCharas;
  std::vector<RawDestination> rawDestinations;
  std::vector<RawCustomTimer> rawCustomTimers;
  std::vector<RawCustomEvent> rawCustomEvents;
  std::optional<RawSkinNote> note;
  std::size_t decodedFrames = 0;
  std::size_t materializedSpriteFrames = 0;
  SkinObjectId nextSyntheticObjectId = 0;
  bool allocationFailed = false;
};

bool optionalIntegerField(lua_State *state, int index, std::string_view name,
                          std::optional<int> &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  int value = 0;
  const bool ok = integerAt(state, -1, value, request);
  lua_pop(state, 1);
  if (ok) {
    output = value;
  }
  return ok;
}

bool bindingFieldPresent(lua_State *state, int index, std::string_view name,
                         bool &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  output = !lua_isnil(state, -1);
  lua_pop(state, 1);
  return true;
}

bool decodeRawSource(lua_State *state, int index, std::size_t depth,
                     RawSkinSource &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         stringField(state, index, "path", output.path,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request);
}

bool decodeRawFontFallback(lua_State *state, int index, std::size_t depth,
                           RawSkinFontFallback &output,
                           DecodeRequest &request) {
  if (lua_type(state, index) == LUA_TSTRING) {
    std::string ignored;
    return copyString(state, index, ignored,
                      LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                      request);
  }
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "path", output.path,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         integerField(state, index, "type", output.type, request);
}

bool decodeRawFont(lua_State *state, int index, std::size_t depth,
                   RawSkinFont &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         stringField(state, index, "path", output.path,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         decodeObjectArrayField(state, index, "fallback", depth,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                output.fallbacks, request,
                                decodeRawFontFallback) &&
         integerField(state, index, "type", output.type, request);
}

bool decodeRawImage(lua_State *state, int index, std::size_t depth,
                    RawSkinImage &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         stringField(state, index, "src", output.source,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "x", output.x, request) &&
         integerField(state, index, "y", output.y, request) &&
         integerField(state, index, "w", output.width, request) &&
         integerField(state, index, "h", output.height, request) &&
         integerField(state, index, "divx", output.divisionsX, request) &&
         integerField(state, index, "divy", output.divisionsY, request) &&
         integerField(state, index, "cycle", output.cycleMillis, request) &&
         integerField(state, index, "len", output.stateCount, request) &&
         integerField(state, index, "ref", output.stateSelector, request) &&
         integerField(state, index, "click", output.clickMode, request);
}

bool booleanField(lua_State *state, int index, std::string_view name,
                  bool &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  // LuaSkinLoader maps every Java boolean field through LuaValue::toboolean:
  // only nil and false are false; every other Lua value is true.
  output = lua_toboolean(state, -1) != 0;
  lua_pop(state, 1);
  return true;
}

bool numberField(lua_State *state, int index, std::string_view name,
                 double &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (lua_isnumber(state, -1) == 0) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_model_invalid",
                "Lua skin numeric field has an invalid type");
  }
  const double value = static_cast<double>(lua_tonumber(state, -1));
  lua_pop(state, 1);
  if (!std::isfinite(value)) {
    return fail(request, "skin_lua_model_invalid",
                "Lua skin numeric field is not finite");
  }
  output = value;
  return true;
}

bool stringArrayField(lua_State *state, int index, std::string_view name,
                      std::vector<std::string> &output,
                      DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_model_invalid",
                "Lua skin string array field is not a table");
  }
  const bool ok = decodeStringArray(
      state, -1, 2, LuaSkinTableDecoderPolicy::maxDecodedObjects, output,
      request);
  lua_pop(state, 1);
  return ok;
}

bool optionalStringArrayField(lua_State *state, int index,
                              std::string_view name,
                              std::optional<std::vector<std::string>> &output,
                              DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_model_invalid",
                "Lua skin optional string array field is not a table");
  }
  output.emplace();
  const bool ok = decodeStringArray(
      state, -1, 2, LuaSkinTableDecoderPolicy::maxDecodedObjects, *output,
      request);
  lua_pop(state, 1);
  return ok;
}

bool decodeRawImageSet(lua_State *state, int index, std::size_t depth,
                       RawSkinImageSet &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "ref", output.stateSelector, request) &&
         integerField(state, index, "click", output.clickMode, request) &&
         stringArrayField(state, index, "images", output.imageIds, request);
}

bool decodeRawDigitOffset(lua_State *state, int index, std::size_t depth,
                          SkinDigitOffset &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         numberField(state, index, "x", output.x, request) &&
         numberField(state, index, "y", output.y, request) &&
         numberField(state, index, "w", output.width, request) &&
         numberField(state, index, "h", output.height, request);
}

bool decodeRawNumber(lua_State *state, int index, std::size_t depth,
                     RawSkinNumber &output, DecodeRequest &request) {
  return decodeRawImage(state, index, depth, output.image, request) &&
         integerField(state, index, "digit", output.digitCount, request) &&
         integerField(state, index, "align", output.alignment, request) &&
         integerField(state, index, "padding", output.padding, request) &&
         integerField(state, index, "zeropadding", output.zeroPadding,
                      request) &&
         integerField(state, index, "space", output.spacing, request) &&
         decodeObjectArrayField(state, index, "offset", depth,
                                LuaSkinTableDecoderPolicy::maxGameplayOffsets,
                                output.perDigitOffsets, request,
                                decodeRawDigitOffset);
}

bool decodeRawFloat(lua_State *state, int index, std::size_t depth,
                    RawSkinFloat &output, DecodeRequest &request) {
  return decodeRawImage(state, index, depth, output.image, request) &&
         integerField(state, index, "iketa", output.integerDigits, request) &&
         integerField(state, index, "fketa", output.fractionalDigits,
                      request) &&
         integerField(state, index, "align", output.alignment, request) &&
         integerField(state, index, "zeropadding", output.zeroPadding,
                      request) &&
         integerField(state, index, "space", output.spacing, request) &&
         numberField(state, index, "gain", output.gain, request) &&
         booleanField(state, index, "isSignvisible", output.signVisible,
                      request) &&
         decodeObjectArrayField(state, index, "offset", depth,
                                LuaSkinTableDecoderPolicy::maxGameplayOffsets,
                                output.perDigitOffsets, request,
                                decodeRawDigitOffset);
}

bool decodeRawSlider(lua_State *state, int index, std::size_t depth,
                     RawSkinSlider &output, DecodeRequest &request) {
  return decodeRawImage(state, index, depth, output.image, request) &&
         integerField(state, index, "angle", output.direction, request) &&
         integerField(state, index, "range", output.range, request) &&
         integerField(state, index, "type", output.typeSelector, request) &&
         booleanField(state, index, "changeable", output.changeable, request) &&
         booleanField(state, index, "isRefNum", output.isRefNum, request) &&
         integerField(state, index, "min", output.minimum, request) &&
         integerField(state, index, "max", output.maximum, request);
}

bool decodeRawGauge(lua_State *state, int index, std::size_t depth,
                    RawSkinGauge &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         stringArrayField(state, index, "nodes", output.nodes, request) &&
         integerField(state, index, "parts", output.parts, request) &&
         integerField(state, index, "type", output.animationType, request) &&
         integerField(state, index, "range", output.animationRange, request) &&
         integerField(state, index, "cycle", output.animationCycleMillis,
                      request) &&
         integerField(state, index, "starttime", output.resultStartMillis,
                      request) &&
         integerField(state, index, "endtime", output.resultEndMillis, request);
}

bool decodeRawText(lua_State *state, int index, std::size_t depth,
                   RawSkinText &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         stringField(state, index, "font", output.font,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "size", output.pointSize, request) &&
         integerField(state, index, "align", output.alignment, request) &&
         integerField(state, index, "ref", output.refSelector, request) &&
         bindingFieldPresent(state, index, "event", output.writerFieldPresent,
                             request) &&
         stringField(state, index, "constantText", output.literal,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         booleanField(state, index, "editable", output.editable, request) &&
         booleanField(state, index, "wrapping", output.wrapping, request) &&
         integerField(state, index, "overflow", output.overflow, request) &&
         stringField(state, index, "outlineColor", output.outlineColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         numberField(state, index, "outlineWidth", output.outlineWidth,
                     request) &&
         stringField(state, index, "shadowColor", output.shadowColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         numberField(state, index, "shadowOffsetX", output.shadowOffsetX,
                     request) &&
         numberField(state, index, "shadowOffsetY", output.shadowOffsetY,
                     request) &&
         numberField(state, index, "shadowSmoothness", output.shadowSmoothness,
                     request);
}

bool decodeRawCustomTimer(lua_State *state, int index, std::size_t depth,
                          RawCustomTimer &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         integerField(state, index, "id", output.id, request);
}

bool decodeRawCustomEvent(lua_State *state, int index, std::size_t depth,
                          RawCustomEvent &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         integerField(state, index, "id", output.id, request) &&
         integerField(state, index, "minInterval", output.minimumIntervalMillis,
                      request);
}

bool decodeRawGraph(lua_State *state, int index, std::size_t depth,
                    RawSkinGraph &output, DecodeRequest &request) {
  return decodeRawImage(state, index, depth, output.image, request) &&
         integerField(state, index, "angle", output.direction, request) &&
         integerField(state, index, "type", output.type, request) &&
         booleanField(state, index, "isRefNum", output.isRefNum, request) &&
         integerField(state, index, "min", output.minimum, request) &&
         integerField(state, index, "max", output.maximum, request);
}

bool decodeRawNoteLaneRect(lua_State *state, int index, std::size_t depth,
                           RawSkinNoteLaneRect &output,
                           DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         integerField(state, index, "x", output.x, request) &&
         integerField(state, index, "y", output.y, request) &&
         integerField(state, index, "w", output.width, request) &&
         integerField(state, index, "h", output.height, request);
}

bool numberArrayField(lua_State *state, int index, std::string_view name,
                      std::vector<double> &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, -1,
                         LuaSkinTableDecoderPolicy::maxDecodedObjects, length,
                         request)) {
    return false;
  }
  const int tableIndex = absoluteIndex(state, -1);
  output.reserve(length);
  for (std::size_t position = 1; position <= length; ++position) {
    lua_rawgeti(state, tableIndex, static_cast<int>(position));
    if (lua_isnumber(state, -1) == 0) {
      return fail(request, "skin_lua_model_invalid",
                  "Lua skin numeric array contains a non-number");
    }
    const double value = static_cast<double>(lua_tonumber(state, -1));
    if (!std::isfinite(value)) {
      return fail(request, "skin_lua_model_invalid",
                  "Lua skin numeric array contains a non-finite value");
    }
    output.push_back(value);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

bool exactExpansionRateField(lua_State *state, int index,
                             std::array<int, 2> &output,
                             DecodeRequest &request) {
  if (!rawGetField(state, index, "expansionrate", request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, -1, output.size(), length, request)) {
    return false;
  }
  if (length != output.size()) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_model_invalid",
                "Lua skin note expansion rate must contain two integers");
  }
  const int tableIndex = absoluteIndex(state, -1);
  for (std::size_t position = 0; position < output.size(); ++position) {
    lua_rawgeti(state, tableIndex, static_cast<int>(position + 1));
    if (lua_isnumber(state, -1) == 0) {
      return fail(request, "skin_lua_model_invalid",
                  "Lua skin note expansion rate contains a non-number");
    }
    const double value = static_cast<double>(lua_tonumber(state, -1));
    if (!std::isfinite(value) || std::trunc(value) != value ||
        value < static_cast<double>(std::numeric_limits<int>::min()) ||
        value > static_cast<double>(std::numeric_limits<int>::max())) {
      return fail(request, "skin_lua_model_invalid",
                  "Lua skin note expansion rate must contain two integers");
    }
    output[position] = static_cast<int>(value);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

bool decodeRawDestination(lua_State *, int, std::size_t, RawDestination &,
                          DecodeRequest &);

bool decodeRawNote(lua_State *state, int index, std::size_t depth,
                   RawSkinNote &output, DecodeRequest &request) {
  if (!requireObject(state, index, depth, request) ||
      !stringField(state, index, "id", output.id,
                   LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                   request)) {
    return false;
  }

  if (!stringArrayField(state, index, "note", output.note, request) ||
      !stringArrayField(state, index, "mine", output.mine, request) ||
      !stringArrayField(state, index, "lnend", output.lnEnd, request) ||
      !stringArrayField(state, index, "lnstart", output.lnStart, request) ||
      !stringArrayField(state, index, "lnbody", output.lnBody, request) ||
      !stringArrayField(state, index, "lnactive", output.lnActive, request) ||
      !optionalStringArrayField(state, index, "lnbodyActive",
                                output.lnBodyActive, request) ||
      !stringArrayField(state, index, "hcnend", output.hcnEnd, request) ||
      !stringArrayField(state, index, "hcnstart", output.hcnStart, request) ||
      !stringArrayField(state, index, "hcnbody", output.hcnBody, request) ||
      !stringArrayField(state, index, "hcnactive", output.hcnActive, request) ||
      !stringArrayField(state, index, "hcndamage", output.hcnDamage, request) ||
      !stringArrayField(state, index, "hcnreactive", output.hcnReactive,
                        request) ||
      !optionalStringArrayField(state, index, "hcnbodyActive",
                                output.hcnBodyActive, request) ||
      !stringArrayField(state, index, "hcnbodyReactive", output.hcnBodyReactive,
                        request) ||
      !stringArrayField(state, index, "hcnbodyMiss", output.hcnBodyMiss,
                        request) ||
      !stringArrayField(state, index, "hidden", output.hidden, request) ||
      !stringArrayField(state, index, "processed", output.processed, request) ||
      !decodeObjectArrayField(state, index, "dst", depth,
                              LuaSkinTableDecoderPolicy::maxDecodedObjects,
                              output.laneRects, request,
                              decodeRawNoteLaneRect) ||
      !numberArrayField(state, index, "size", output.noteHeights, request) ||
      !optionalIntegerField(state, index, "dst2", output.secondaryDestinationY,
                            request) ||
      !exactExpansionRateField(state, index, output.expansionRatePercent,
                               request) ||
      !decodeObjectArrayField(state, index, "group", depth,
                              SkinNoteLineNormalizationPolicy::maxGroups,
                              output.group, request, decodeRawDestination) ||
      !decodeObjectArrayField(
          state, index, "bpm", depth,
          SkinNoteLineNormalizationPolicy::maxAuxiliarySlots, output.bpm,
          request, decodeRawDestination) ||
      !decodeObjectArrayField(
          state, index, "stop", depth,
          SkinNoteLineNormalizationPolicy::maxAuxiliarySlots, output.stop,
          request, decodeRawDestination) ||
      !decodeObjectArrayField(
          state, index, "time", depth,
          SkinNoteLineNormalizationPolicy::maxAuxiliarySlots, output.time,
          request, decodeRawDestination)) {
    return false;
  }
  output.authoredHiddenOrProcessed =
      !output.hidden.empty() || !output.processed.empty();
  for (auto *lines : {&output.group, &output.bpm, &output.stop, &output.time}) {
    for (std::size_t position = 0; position < lines->size(); ++position) {
      (*lines)[position].authoredIndex =
          static_cast<std::uint32_t>(position + 1);
    }
  }
  return true;
}

bool decodeRawDestinationFrame(lua_State *state, int index, std::size_t depth,
                               RawDestinationFrame &output,
                               DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         optionalIntegerField(state, index, "time", output.time, request) &&
         optionalIntegerField(state, index, "x", output.x, request) &&
         optionalIntegerField(state, index, "y", output.y, request) &&
         optionalIntegerField(state, index, "w", output.width, request) &&
         optionalIntegerField(state, index, "h", output.height, request) &&
         optionalIntegerField(state, index, "angle", output.angle, request) &&
         optionalIntegerField(state, index, "acc", output.acceleration,
                              request) &&
         optionalIntegerField(state, index, "a", output.alpha, request) &&
         optionalIntegerField(state, index, "r", output.red, request) &&
         optionalIntegerField(state, index, "g", output.green, request) &&
         optionalIntegerField(state, index, "b", output.blue, request) &&
         optionalIntegerField(state, index, "clip_x", output.clipX, request) &&
         optionalIntegerField(state, index, "clip_y", output.clipY, request) &&
         optionalIntegerField(state, index, "clip_w", output.clipWidth,
                              request) &&
         optionalIntegerField(state, index, "clip_h", output.clipHeight,
                              request);
}

bool integerArrayField(lua_State *state, int index, std::string_view name,
                       std::vector<int> &output, DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, -1,
                         LuaSkinTableDecoderPolicy::maxGameplayOffsets, length,
                         request)) {
    return false;
  }
  const int tableIndex = absoluteIndex(state, -1);
  output.reserve(length);
  for (std::size_t position = 1; position <= length; ++position) {
    lua_rawgeti(state, tableIndex, static_cast<int>(position));
    int value = 0;
    if (!integerAt(state, -1, value, request)) {
      return false;
    }
    output.push_back(value);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

bool destinationConditionsField(lua_State *state, int index,
                                std::vector<RawDestination::Condition> &output,
                                DecodeRequest &request) {
  if (!rawGetField(state, index, "op", request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, -1,
                         LuaSkinTableDecoderPolicy::maxDecodedObjects, length,
                         request)) {
    return false;
  }
  const int tableIndex = absoluteIndex(state, -1);
  output.reserve(length);
  for (std::size_t position = 1; position <= length; ++position) {
    lua_rawgeti(state, tableIndex, static_cast<int>(position));
    RawDestination::Condition condition;
    if (lua_isnumber(state, -1) != 0) {
      int option = 0;
      if (!integerAt(state, -1, option, request)) {
        return false;
      }
      condition.optionId = option;
    }
    output.push_back(std::move(condition));
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

bool destinationMouseRectField(lua_State *state, int index,
                               std::optional<SkinAuthoredRect> &output,
                               DecodeRequest &request) {
  if (!rawGetField(state, index, "mouseRect", request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_model_invalid",
                "Lua skin destination mouseRect is not an object");
  }
  output.emplace();
  const int rectangleIndex = absoluteIndex(state, -1);
  if (!numberField(state, rectangleIndex, "x", output->x, request) ||
      !numberField(state, rectangleIndex, "y", output->y, request) ||
      !numberField(state, rectangleIndex, "w", output->width, request) ||
      !numberField(state, rectangleIndex, "h", output->height, request)) {
    return false;
  }
  lua_pop(state, 1);
  return true;
}

bool decodeRawDestination(lua_State *state, int index, std::size_t depth,
                          RawDestination &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "loop", output.loop, request) &&
         integerField(state, index, "center", output.center, request) &&
         integerField(state, index, "blend", output.blend, request) &&
         integerField(state, index, "filter", output.filter, request) &&
         integerField(state, index, "stretch", output.stretch, request) &&
         integerField(state, index, "offset", output.offset, request) &&
         integerArrayField(state, index, "offsets", output.offsets, request) &&
         destinationConditionsField(state, index, output.conditions, request) &&
         destinationMouseRectField(state, index, output.mouseRect, request) &&
         decodeObjectArrayField(state, index, "dst", depth,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                output.frames, request,
                                decodeRawDestinationFrame);
}

bool decodeRawCover(lua_State *state, int index, std::size_t depth,
                    RawSkinCover &output, DecodeRequest &request) {
  return decodeRawImage(state, index, depth, output.image, request) &&
         integerField(state, index, "disapearLine", output.disappearLine,
                      request) &&
         booleanField(state, index, "isDisapearLineLinkLift",
                      output.disappearLineLinksLift, request);
}

bool decodeRawHiddenCover(lua_State *state, int index, std::size_t depth,
                          RawSkinCover &output, DecodeRequest &request) {
  output.kind = SkinCoverKind::Hidden;
  output.disappearLineLinksLift = true;
  return decodeRawCover(state, index, depth, output, request);
}

bool decodeRawLiftCover(lua_State *state, int index, std::size_t depth,
                        RawSkinCover &output, DecodeRequest &request) {
  output.kind = SkinCoverKind::Lift;
  output.disappearLineLinksLift = false;
  return decodeRawCover(state, index, depth, output, request);
}

bool decodeRawJudge(lua_State *state, int index, std::size_t depth,
                    RawSkinJudge &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "index", output.player, request) &&
         booleanField(state, index, "shift", output.shift, request) &&
         decodeObjectArrayField(state, index, "images", depth,
                                SkinJudgeNormalizationPolicy::maxAuthoredGrades,
                                output.images, request, decodeRawDestination) &&
         decodeObjectArrayField(state, index, "numbers", depth,
                                SkinJudgeNormalizationPolicy::maxAuthoredGrades,
                                output.numbers, request, decodeRawDestination);
}

bool decodeRawIdentity(lua_State *state, int index, std::size_t depth,
                       RawSkinIdentity &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request);
}

bool gaugeGraphColorArrayField(lua_State *state, int index,
                               RawSkinGaugeGraph &output,
                               DecodeRequest &request) {
  if (!rawGetField(state, index, "color", request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1) || !lua_checkstack(state, 3)) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_model_invalid",
                "Lua skin gaugegraph color field is not a bounded array");
  }
  output.colors.emplace();
  const int tableIndex = absoluteIndex(state, -1);
  lua_pushnil(state);
  while (lua_next(state, tableIndex) != 0) {
    ++request.entries;
    const double numeric = lua_type(state, -2) == LUA_TNUMBER
                               ? static_cast<double>(lua_tonumber(state, -2))
                               : 0.0;
    if ((request.enforceGameplayLimits &&
         request.entries > LuaSkinTableDecoderPolicy::maxEntries) ||
        !std::isfinite(numeric) || std::trunc(numeric) != numeric ||
        numeric < 1.0 ||
        numeric >
            static_cast<double>(LuaSkinTableDecoderPolicy::maxDecodedObjects)) {
      lua_pop(state, 3);
      return fail(request, "skin_lua_model_invalid",
                  "Lua skin gaugegraph color array has an invalid key");
    }
    const std::size_t position = static_cast<std::size_t>(numeric);
    if (output.colors->size() < position) {
      output.colors->resize(position);
    }
    std::string value;
    if (!copyString(state, -1, value,
                    LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                    request)) {
      lua_pop(state, 3);
      return false;
    }
    (*output.colors)[position - 1] = std::move(value);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

bool decodeRawGaugeGraph(lua_State *state, int index, std::size_t depth,
                         RawSkinGaugeGraph &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         gaugeGraphColorArrayField(state, index, output, request) &&
         stringField(state, index, "assistClearBGColor",
                     output.assistClearBackground,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "assistAndEasyFailBGColor",
                     output.assistEasyFailBackground,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "grooveFailBGColor",
                     output.grooveFailBackground,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "grooveClearAndHardBGColor",
                     output.grooveClearHardBackground,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "exHardBGColor", output.exHardBackground,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "hazardBGColor", output.hazardBackground,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "assistClearLineColor",
                     output.assistClearLine,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "assistAndEasyFailLineColor",
                     output.assistEasyFailLine,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "grooveFailLineColor",
                     output.grooveFailLine,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "grooveClearAndHardLineColor",
                     output.grooveClearHardLine,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "exHardLineColor", output.exHardLine,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "hazardLineColor", output.hazardLine,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "borderlineColor", output.borderLine,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "borderColor", output.borderBackground,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request);
}

bool decodeRawBpmGraph(lua_State *state, int index, std::size_t depth,
                       RawSkinBpmGraph &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "delay", output.delayMillis, request) &&
         integerField(state, index, "lineWidth", output.lineWidth, request) &&
         stringField(state, index, "mainBPMColor", output.mainBpmColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "minBPMColor", output.minimumBpmColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "maxBPMColor", output.maximumBpmColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "otherBPMColor", output.otherBpmColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "stopLineColor", output.stopLineColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "transitionLineColor",
                     output.transitionLineColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request);
}

bool decodeRawNoteDistributionGraph(lua_State *state, int index,
                                    std::size_t depth,
                                    RawSkinNoteDistributionGraph &output,
                                    DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "type", output.type, request) &&
         integerField(state, index, "backTexOff", output.backTextureOff,
                      request) &&
         integerField(state, index, "delay", output.delayMillis, request) &&
         integerField(state, index, "orderReverse", output.reverseOrder,
                      request) &&
         integerField(state, index, "noGap", output.noGap, request) &&
         integerField(state, index, "noGapX", output.noHorizontalGap,
                      request);
}

bool decodeRawTimingVisualizer(lua_State *state, int index, std::size_t depth,
                               RawSkinTimingVisualizer &output,
                               DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "width", output.width, request) &&
         integerField(state, index, "judgeWidthMillis", output.judgeWidthMillis,
                      request) &&
         integerField(state, index, "lineWidth", output.lineWidth, request) &&
         stringField(state, index, "lineColor", output.lineColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "centerColor", output.centerColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "PGColor", output.pgColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "GRColor", output.grColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "GDColor", output.gdColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "BDColor", output.bdColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "PRColor", output.prColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         integerField(state, index, "transparent", output.transparent,
                      request) &&
         integerField(state, index, "drawDecay", output.drawDecay, request);
}

bool decodeRawTimingDistributionGraph(
    lua_State *state, int index, std::size_t depth,
    RawSkinTimingDistributionGraph &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "width", output.width, request) &&
         integerField(state, index, "lineWidth", output.lineWidth, request) &&
         stringField(state, index, "graphColor", output.graphColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "averageColor", output.averageColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "devColor", output.devColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "PGColor", output.pgColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "GRColor", output.grColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "GDColor", output.gdColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "BDColor", output.bdColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "PRColor", output.prColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         integerField(state, index, "drawAverage", output.drawAverage,
                      request) &&
         integerField(state, index, "drawDev", output.drawDev, request);
}

bool decodeRawHitErrorVisualizer(lua_State *state, int index,
                                 std::size_t depth,
                                 RawSkinHitErrorVisualizer &output,
                                 DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         integerField(state, index, "width", output.width, request) &&
         integerField(state, index, "judgeWidthMillis", output.judgeWidthMillis,
                      request) &&
         integerField(state, index, "lineWidth", output.lineWidth, request) &&
         integerField(state, index, "colorMode", output.colorMode, request) &&
         integerField(state, index, "hiterrorMode", output.hitErrorMode,
                      request) &&
         integerField(state, index, "emaMode", output.emaMode, request) &&
         stringField(state, index, "lineColor", output.lineColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "centerColor", output.centerColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "PGColor", output.pgColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "GRColor", output.grColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "GDColor", output.gdColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "BDColor", output.bdColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "PRColor", output.prColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         stringField(state, index, "emaColor", output.emaColor,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         numberField(state, index, "alpha", output.alpha, request) &&
         integerField(state, index, "windowLength", output.windowLength,
                      request) &&
         integerField(state, index, "transparent", output.transparent,
                      request) &&
         integerField(state, index, "drawDecay", output.drawDecay, request);
}

bool decodeRawPmChara(lua_State *state, int index, std::size_t depth,
                      RawSkinPmChara &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, false,
                     request) &&
         stringField(state, index, "src", output.source,
                     LuaSkinTableDecoderPolicy::maxGameplayTextBytes, true,
                     request) &&
         integerField(state, index, "color", output.color, request) &&
         integerField(state, index, "type", output.type, request) &&
         integerField(state, index, "side", output.side, request);
}

bool expandImageFrames(GameplayDecodeRequest &request, RawSkinImage &image) {
  image.divisionsX = image.divisionsX > 0 ? image.divisionsX : 1;
  image.divisionsY = image.divisionsY > 0 ? image.divisionsY : 1;
  const auto divisionsX = static_cast<std::size_t>(image.divisionsX);
  const auto divisionsY = static_cast<std::size_t>(image.divisionsY);
  if (request.enforceGameplayLimits &&
      divisionsX > LuaSkinTableDecoderPolicy::maxEntries /
                       std::max<std::size_t>(divisionsY, 1)) {
    return fail(request.decoding, "skin_lua_model_limit_exceeded",
                "Lua skin image divisions exceed the fixed model limit");
  }
  const std::size_t frameCount = divisionsX * divisionsY;
  if (frameCount == 0 || (request.enforceGameplayLimits &&
      request.decodedFrames >
          LuaSkinTableDecoderPolicy::maxEntries -
              std::min(frameCount, LuaSkinTableDecoderPolicy::maxEntries))) {
    return fail(request.decoding, "skin_lua_model_limit_exceeded",
                "Lua skin image frames exceed the fixed model limit");
  }

  const auto resource = request.sourceIds.find(image.source);
  image.sprite.resource = resource != request.sourceIds.end()
                              ? resource->second
                              : SkinResourceId{0};
  image.sprite.cycleMillis = image.cycleMillis;
  image.sprite.frames.reserve(frameCount);
  for (int row = 0; row < image.divisionsY; ++row) {
    for (int column = 0; column < image.divisionsX; ++column) {
      // JsonSkinObjectLoader keeps the original TextureRegion rectangle and
      // selects its grid cell at texture preparation.  Preserve that deferred
      // identity here; pre-dividing and then resolving again shrinks every
      // cell and rejects valid edge crops.
      image.sprite.frames.push_back({.x = image.x,
                                     .y = image.y,
                                     .w = image.width,
                                     .h = image.height,
                                     .gridColumn = column,
                                     .gridRow = row,
                                     .gridColumns = image.divisionsX,
                                     .gridRows = image.divisionsY});
    }
  }
  request.decodedFrames += frameCount;
  return true;
}

bool consumeMaterializedSpriteFrames(GameplayDecodeRequest &request,
                                     std::size_t count) {
  if (request.enforceGameplayLimits &&
      (count > LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames ||
      request.materializedSpriteFrames >
          LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames - count)) {
    return fail(request.decoding, "skin_lua_model_limit_exceeded",
                "Lua skin materialized sprite frames exceed the fixed model "
                "limit");
  }
  request.materializedSpriteFrames += count;
  return true;
}

const SkinSpriteFrames *noteSprite(const GameplayDecodeRequest &request,
                                   std::string_view imageId) {
  const auto image = request.images.find(imageId);
  if (image == request.images.end() || image->second.sprite.resource == 0 ||
      image->second.sprite.frames.empty()) {
    return nullptr;
  }
  return &image->second.sprite;
}

SkinAuthoredNoteVisualSlots
resolveNoteVisuals(const GameplayDecodeRequest &request,
                   const std::vector<std::string> &imageIds) {
  SkinAuthoredNoteVisualSlots result;
  result.reserve(imageIds.size());
  for (const auto &imageId : imageIds) {
    const auto *sprite = noteSprite(request, imageId);
    if (sprite != nullptr) {
      result.emplace_back(*sprite);
    } else {
      result.emplace_back(std::nullopt);
    }
  }
  return result;
}

std::optional<SkinAuthoredNoteVisualSlots> resolveOptionalNoteVisuals(
    const GameplayDecodeRequest &request,
    const std::optional<std::vector<std::string>> &imageIds) {
  return imageIds
             ? std::optional<SkinAuthoredNoteVisualSlots>{resolveNoteVisuals(
                   request, *imageIds)}
             : std::nullopt;
}

bool failNoteNormalization(GameplayDecodeRequest &request, bool limit,
                           std::string message) {
  return fail(request.decoding,
              limit ? "skin_lua_model_limit_exceeded"
                    : "skin_lua_model_invalid",
              std::move(message));
}

bool normalizeDestination(GameplayDecodeRequest &, const RawDestination &,
                          std::uint32_t, SkinDestinationBody &, bool);

bool buildNoteLineSlots(GameplayDecodeRequest &request,
                        const std::vector<RawDestination> &raw,
                        std::size_t selectedCount,
                        SkinAuthoredNoteLineSlots &output) {
  output.resize(raw.size());
  for (std::size_t index = 0; index < selectedCount; ++index) {
    SkinAuthoredNoteLineSlot slot;
    if (const auto *sprite = noteSprite(request, raw[index].id)) {
      slot.image = *sprite;
    }
    slot.destination.emplace();
    if (!normalizeDestination(request, raw[index],
                              static_cast<std::uint32_t>(index),
                              *slot.destination, false)) {
      return false;
    }
    output[index] = std::move(slot);
  }
  return true;
}

bool buildNoteObject(GameplayDecodeRequest &request, RawSkinNote &note) {
  SkinNoteNormalizationInput visualInput{
      .note = resolveNoteVisuals(request, note.note),
      .mine = resolveNoteVisuals(request, note.mine),
      .lnEnd = resolveNoteVisuals(request, note.lnEnd),
      .lnStart = resolveNoteVisuals(request, note.lnStart),
      .lnBody = resolveNoteVisuals(request, note.lnBody),
      .lnActive = resolveNoteVisuals(request, note.lnActive),
      .lnBodyActive = resolveOptionalNoteVisuals(request, note.lnBodyActive),
      .hcnEnd = resolveNoteVisuals(request, note.hcnEnd),
      .hcnStart = resolveNoteVisuals(request, note.hcnStart),
      .hcnBody = resolveNoteVisuals(request, note.hcnBody),
      .hcnActive = resolveNoteVisuals(request, note.hcnActive),
      .hcnDamage = resolveNoteVisuals(request, note.hcnDamage),
      .hcnReactive = resolveNoteVisuals(request, note.hcnReactive),
      .hcnBodyActive = resolveOptionalNoteVisuals(request, note.hcnBodyActive),
      .hcnBodyReactive = resolveNoteVisuals(request, note.hcnBodyReactive),
      .hcnBodyMiss = resolveNoteVisuals(request, note.hcnBodyMiss),
  };
  auto normalizedVisuals = normalizeSkinNote(visualInput);
  if (!normalizedVisuals.note) {
    const bool limit = normalizedVisuals.error ==
                           SkinNoteNormalizationError::LaneLimitExceeded ||
                       normalizedVisuals.error ==
                           SkinNoteNormalizationError::FrameLimitExceeded;
    return failNoteNormalization(
        request, limit,
        "Lua skin Note visual arrays cannot be normalized safely");
  }

  SkinNoteLaneGeometryNormalizationInput geometryInput;
  geometryInput.normalFirstFrameHeights.reserve(visualInput.note.size());
  for (const auto &normal : visualInput.note) {
    if (normal && !normal->frames.empty() && normal->frames.front().h >= 0) {
      geometryInput.normalFirstFrameHeights.emplace_back(
          static_cast<double>(normal->frames.front().h));
    } else {
      geometryInput.normalFirstFrameHeights.emplace_back(std::nullopt);
    }
  }
  geometryInput.laneDestinations.reserve(note.laneRects.size());
  for (const auto &rect : note.laneRects) {
    geometryInput.laneDestinations.push_back(
        {.x = static_cast<double>(rect.x),
         .y = static_cast<double>(rect.y),
         .width = static_cast<double>(rect.width),
         .height = static_cast<double>(rect.height)});
  }
  geometryInput.authoredNoteHeights = note.noteHeights;
  geometryInput.secondaryDestinationY = note.secondaryDestinationY;
  geometryInput.expansionRatePercent = note.expansionRatePercent;
  auto normalizedGeometry = normalizeSkinNoteLaneGeometry(geometryInput);
  if (!normalizedGeometry.geometry) {
    const bool limit =
        normalizedGeometry.error ==
        SkinNoteLaneGeometryNormalizationError::LaneLimitExceeded;
    return failNoteNormalization(
        request, limit,
        "Lua skin Note lane geometry cannot be normalized safely");
  }

  SkinNoteLineNormalizationInput lineInput;
  const std::size_t groupCount = note.group.size();
  if (!buildNoteLineSlots(request, note.group, groupCount, lineInput.group) ||
      !buildNoteLineSlots(request, note.bpm,
                          std::min(groupCount, note.bpm.size()),
                          lineInput.bpm) ||
      !buildNoteLineSlots(request, note.stop,
                          std::min(groupCount, note.stop.size()),
                          lineInput.stop) ||
      !buildNoteLineSlots(request, note.time,
                          std::min(groupCount, note.time.size()),
                          lineInput.time)) {
    return false;
  }
  auto normalizedLines = normalizeSkinNoteLines(lineInput);
  if (!normalizedLines.lines) {
    const bool limit =
        normalizedLines.error ==
            SkinNoteLineNormalizationError::GroupLimitExceeded ||
        normalizedLines.error ==
            SkinNoteLineNormalizationError::AuxiliaryLimitExceeded ||
        normalizedLines.error ==
            SkinNoteLineNormalizationError::OutputLimitExceeded ||
        normalizedLines.error ==
            SkinNoteLineNormalizationError::FrameLimitExceeded;
    return failNoteNormalization(
        request, limit,
        "Lua skin Note line presentations cannot be normalized safely");
  }

  note.object.expansionRatePercent =
      normalizedGeometry.geometry->expansionRatePercent;
  note.object.hcnBodySlotLayout = normalizedVisuals.note->hcnBodySlotLayout;
  note.object.lanes.reserve(normalizedVisuals.note->lanes.size());
  for (std::size_t laneIndex = 0;
       laneIndex < normalizedVisuals.note->lanes.size(); ++laneIndex) {
    const auto &normalizedLane = normalizedVisuals.note->lanes[laneIndex];
    const auto &geometry = normalizedGeometry.geometry->lanes[laneIndex];
    SkinLaneNotePresentation lane{
        .authoredLane = static_cast<int>(normalizedLane.authoredLane),
        .laneDestination = geometry.laneDestination,
        .authoredNoteHeight = geometry.authoredNoteHeight,
        .secondaryDestinationY = geometry.secondaryDestinationY,
    };
    for (std::size_t visualIndex = 0;
         visualIndex < normalizedLane.visuals.size(); ++visualIndex) {
      const auto kind = static_cast<SkinNoteVisualKind>(visualIndex);
      if (const auto *sprite = std::get_if<SkinSpriteFrames>(
              &normalizedLane.visuals[visualIndex])) {
        if (!consumeMaterializedSpriteFrames(request, sprite->frames.size())) {
          return false;
        }
        lane.visuals.emplace(kind, *sprite);
      } else {
        lane.visuals.emplace(kind, SkinSynthesizedNoteVisual{.kind = kind});
      }
    }
    note.object.lanes.push_back(std::move(lane));
  }

  note.object.lines.reserve(normalizedLines.lines->lines.size());
  for (const auto &normalizedLine : normalizedLines.lines->lines) {
    const std::size_t imageFrames =
        normalizedLine.image ? normalizedLine.image->frames.size() : 0;
    if (!consumeMaterializedSpriteFrames(request, imageFrames)) {
      return false;
    }
    note.object.lines.push_back(
        {.kind = normalizedLine.kind,
         .sprite = normalizedLine.image,
         .laneGroupDestination =
             normalizedLines.lines->groups[normalizedLine.laneGroup].laneRect,
         .destination = normalizedLine.destination});
  }

  if (note.authoredHiddenOrProcessed) {
    request.result.diagnostics.push_back(
        {.code = "skin_lua_model_authored_note_visual_ignored",
         .message = "Pinned Beatoraja ignores authored hidden and processed "
                    "note images and synthesizes them instead",
         .severity = DiagnosticSeverity::Warning});
  }
  return true;
}

bool makeImageObject(GameplayDecodeRequest &request,
                     const RawSkinImage &definition, SkinImageObject &output) {
  const int stateCount = definition.stateCount > 1 ? definition.stateCount : 1;
  const std::size_t frames = definition.sprite.frames.size();
  if (stateCount <= 0 || frames == 0 ||
      frames % static_cast<std::size_t>(stateCount) != 0) {
    return fail(request.decoding, "skin_lua_model_invalid",
                "Lua skin Image.len must evenly partition its frames");
  }
  const std::size_t framesPerState =
      frames / static_cast<std::size_t>(stateCount);
  if (!consumeMaterializedSpriteFrames(request, frames)) {
    return false;
  }
  output.orderedStates.reserve(static_cast<std::size_t>(stateCount));
  for (int state = 0; state < stateCount; ++state) {
    SkinSpriteFrames sprite{
        .resource = definition.sprite.resource,
        .cycleMillis = definition.sprite.cycleMillis,
        .timer = definition.sprite.timer,
    };
    const auto first = definition.sprite.frames.begin() +
                       static_cast<std::ptrdiff_t>(state) *
                           static_cast<std::ptrdiff_t>(framesPerState);
    sprite.frames.assign(first,
                         first + static_cast<std::ptrdiff_t>(framesPerState));
    output.orderedStates.push_back(std::move(sprite));
  }
  if (definition.stateCount > 1) {
    output.stateIndex = definition.stateIndex;
  }
  output.clickEvent = definition.clickEvent;
  output.clickMode = definition.clickMode;
  return true;
}

bool materializeNumericGlyphAtlas(GameplayDecodeRequest &request,
                                  NumericGlyphAtlasKind kind,
                                  const SkinSpriteFrames &source,
                                  NumericGlyphFormatRequest format,
                                  NumericGlyphAtlas &output) {
  const std::size_t remaining = request.enforceGameplayLimits
                                    ? LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames -
                                          request.materializedSpriteFrames
                                    : std::numeric_limits<std::size_t>::max();
  auto normalized = partitionNumericGlyphAtlas(
      {.kind = kind,
       .source = source,
       .format = std::move(format),
       .budget = {.remainingMaterializedFrames = remaining}});
  if (!normalized.atlas) {
    const bool limit =
        normalized.error == NumericGlyphAtlasError::InputLimitExceeded ||
        normalized.error == NumericGlyphAtlasError::OutputLimitExceeded;
    return fail(request.decoding,
                limit ? "skin_lua_model_limit_exceeded"
                      : "skin_lua_model_invalid",
                limit ? "Lua skin numeric glyph atlas exceeds the remaining "
                        "materialized frame budget"
                      : "Lua skin numeric glyph atlas is invalid");
  }

  std::size_t outputFrames = normalized.atlas->digits.positive.frames.size();
  if (normalized.atlas->digits.negative) {
    const std::size_t negativeFrames =
        normalized.atlas->digits.negative->frames.size();
    if (negativeFrames >
        std::numeric_limits<std::size_t>::max() - outputFrames) {
      return fail(request.decoding, "skin_lua_model_limit_exceeded",
                  "Lua skin numeric glyph output frame count overflows");
    }
    outputFrames += negativeFrames;
  }
  if (!consumeMaterializedSpriteFrames(request, outputFrames)) {
    return false;
  }
  output = std::move(*normalized.atlas);
  return true;
}

bool makeGaugeObject(GameplayDecodeRequest &request,
                     const RawSkinGauge &definition, SkinGaugeObject &output) {
  SkinGaugeNodeExpansionInput input{
      .nodes = definition.nodes,
      .parts = definition.parts,
      .animationType = definition.animationType,
      .animationRange = definition.animationRange,
      .animationCycleMillis = definition.animationCycleMillis,
      .resultStartMillis = definition.resultStartMillis,
      .resultEndMillis = definition.resultEndMillis,
  };
  input.images.reserve(request.images.size());
  for (const auto &[id, image] : request.images) {
    input.images.push_back({.id = id, .sprite = image.sprite});
  }

  auto expanded = expandSkinGaugeNodes(input);
  if (!expanded.gauge) {
    if (expanded.error == SkinGaugeNodeExpansionError::FrameLimitExceeded) {
      return fail(request.decoding, "skin_lua_model_limit_exceeded",
                  "Lua skin Gauge expansion exceeds the fixed model frame "
                  "limit");
    }
    request.result.diagnostics.push_back(
        diagnostic("skin_lua_model_gauge_invalid",
                   "Lua skin Gauge node definitions cannot be expanded"));
    output = SkinGaugeObject{};
    return true;
  }

  std::size_t frameCount = 0;
  for (const auto &node : expanded.gauge->orderedNodes) {
    if (node.frames.size() >
        std::numeric_limits<std::size_t>::max() - frameCount) {
      return fail(request.decoding, "skin_lua_model_limit_exceeded",
                  "Lua skin Gauge frame count overflows");
    }
    frameCount += node.frames.size();
  }
  if (!consumeMaterializedSpriteFrames(request, frameCount)) {
    return false;
  }
  output = std::move(*expanded.gauge);
  return true;
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool applyPinnedRgbColor(std::string_view value, std::uint32_t &color) {
  std::array<char, 6> normalized{};
  std::size_t size = 0;
  for (const char character : value) {
    if (hexNibble(character) < 0) {
      continue;
    }
    if (size < normalized.size()) {
      normalized[size++] = character;
    }
  }
  if (size == 0) {
    return true;
  }
  if (size < normalized.size()) {
    return false;
  }
  std::uint32_t rgb = 0;
  for (const char character : normalized) {
    rgb = rgb * 16U + static_cast<std::uint32_t>(hexNibble(character));
  }
  color = (rgb << 8U) | 0xffU;
  return true;
}

bool applyPinnedDirectColor(std::string_view value, std::uint32_t &color) {
  if (!value.empty() && value.front() == '#') {
    value.remove_prefix(1);
  }
  if (value.size() < 6) {
    return false;
  }
  std::uint32_t rgba = 0;
  const std::size_t parsedSize = value.size() == 8 ? 8 : 6;
  for (std::size_t index = 0; index < parsedSize; ++index) {
    const char character = value[index];
    const int nibble = hexNibble(character);
    if (nibble < 0) {
      return false;
    }
    rgba = rgba * 16U + static_cast<std::uint32_t>(nibble);
  }
  color = parsedSize == 6 ? (rgba << 8U) | 0xffU : rgba;
  return true;
}

std::array<std::uint8_t, 4> parseTextColor(std::string_view value) {
  if (value.size() != 6 && value.size() != 8) {
    return {255, 255, 255, 255};
  }
  std::array<std::uint8_t, 4> result{255, 255, 255, 255};
  for (std::size_t channel = 0; channel < value.size() / 2; ++channel) {
    const int high = hexNibble(value[channel * 2]);
    const int low = hexNibble(value[channel * 2 + 1]);
    if (high < 0 || low < 0) {
      return {255, 255, 255, 255};
    }
    result[channel] = static_cast<std::uint8_t>(high * 16 + low);
  }
  return result;
}

bool makeTextObject(GameplayDecodeRequest &request,
                    const RawSkinText &definition, SkinTextObject &output) {
  const auto normalized = normalizeSkinText(
      {.fontName = definition.font,
       .value = definition.value
                    ? std::optional<SkinStringPropertyId>{definition.value}
                    : std::nullopt,
       .writer = definition.writer,
       .literal = definition.literal,
       .pointSize = definition.pointSize,
       .alignment = definition.alignment,
       .wrapping = definition.wrapping,
       .overflow = static_cast<SkinTextOverflow>(definition.overflow),
       .outlineRgba = parseTextColor(definition.outlineColor),
       .outlineWidth = definition.outlineWidth,
       .shadowRgba = parseTextColor(definition.shadowColor),
       .shadowOffsetX = definition.shadowOffsetX,
       .shadowOffsetY = definition.shadowOffsetY,
       .shadowSmoothness = definition.shadowSmoothness,
       .writerWasExplicit = definition.writerWasExplicit,
       .authoredEditable = definition.editable},
      request.fonts);
  if (!normalized.text) {
    request.result.diagnostics.push_back(
        diagnostic("skin_lua_model_text_invalid",
                   "Lua skin Text definition cannot be normalized"));
    output = SkinTextObject{};
    return true;
  }
  output = std::move(*normalized.text);
  return true;
}

bool makeGraphObject(GameplayDecodeRequest &request,
                     const RawSkinGraph &definition, SkinGraphObject &output) {
  SkinGraphNormalizationInput input{
      .fill = definition.image.sprite,
      .isRefNum = definition.isRefNum,
      .type = definition.type,
      .direction = definition.direction,
  };
  if (definition.explicitValue) {
    input.explicitRate = definition.explicitValue;
  } else if (definition.isRefNum) {
    input.integerRange = SkinSliderObject::IntegerRangeSource{
        .value = definition.integerValue.value_or(SkinIntegerPropertyId{}),
        .minimum = definition.minimum,
        .maximum = definition.maximum,
    };
  } else if (definition.type >= 0) {
    input.implicitRate =
        definition.implicitValue.value_or(SkinFloatPropertyId{});
  }

  auto normalized = normalizeSkinGraph(input);
  if (!normalized.graph) {
    const bool distribution =
        normalized.error ==
        SkinTextGraphNormalizationError::UnsupportedDistributionGraph;
    request.result.diagnostics.push_back(diagnostic(
        distribution ? "skin_lua_model_distribution_graph_unsupported"
                     : "skin_lua_model_graph_invalid",
        distribution ? "Lua skin distribution Graph objects are unsupported"
                     : "Lua skin Graph definition cannot be normalized"));
    output = SkinGraphObject{};
    return true;
  }
  if (!consumeMaterializedSpriteFrames(request,
                                       normalized.graph->fill.frames.size())) {
    return false;
  }
  output = std::move(*normalized.graph);
  return true;
}

bool makeNoteDistributionGraphObject(
    GameplayDecodeRequest &request,
    const RawSkinNoteDistributionGraph &definition,
    SkinNoteDistributionGraphObject &output) {
  SkinNoteDistributionGraphType type;
  switch (definition.type) {
  case 0:
    type = SkinNoteDistributionGraphType::Normal;
    break;
  case 1:
    type = SkinNoteDistributionGraphType::Judge;
    break;
  case 2:
    type = SkinNoteDistributionGraphType::EarlyLate;
    break;
  default:
    return fail(request.decoding, "skin_lua_model_judgegraph_invalid",
                "Lua skin judgegraph type is outside the pinned range");
  }
  output = {
      .type = type,
      .backgroundTextureOff = definition.backTextureOff == 1,
      .delayMillis = definition.delayMillis,
      .reverseOrder = definition.reverseOrder == 1,
      .noGap = definition.noGap == 1,
      .noHorizontalGap = definition.noHorizontalGap == 1,
  };
  return true;
}

bool makeGaugeGraphObject(GameplayDecodeRequest &request,
                          const RawSkinGaugeGraph &definition,
                          SkinGaugeGraphObject &output) {
  if (definition.colors) {
    for (auto &row : output.rgba) {
      row.fill(0x000000ffU);
    }
    const std::size_t count =
        std::min<std::size_t>(24, definition.colors->size());
    for (std::size_t index = 0; index < count; ++index) {
      if (!(*definition.colors)[index]) {
        continue;
      }
      if (!applyPinnedDirectColor(*(*definition.colors)[index],
                                  output.rgba[index / 4][index % 4])) {
        return fail(request.decoding, "skin_lua_model_gaugegraph_invalid",
                    "Lua skin gaugegraph has an invalid direct colour");
      }
    }
    return true;
  }

  const std::array<std::array<std::string_view, 4>, 6> colors{
      std::array<std::string_view, 4>{definition.borderLine,
                                      definition.borderBackground,
                                      definition.assistClearLine,
                                      definition.assistClearBackground},
      std::array<std::string_view, 4>{definition.borderLine,
                                      definition.borderBackground,
                                      definition.assistEasyFailLine,
                                      definition.assistEasyFailBackground},
      std::array<std::string_view, 4>{definition.borderLine,
                                      definition.borderBackground,
                                      definition.grooveFailLine,
                                      definition.grooveFailBackground},
      std::array<std::string_view, 4>{definition.grooveClearHardLine,
                                      definition.grooveClearHardBackground,
                                      definition.grooveClearHardLine,
                                      definition.grooveClearHardBackground},
      std::array<std::string_view, 4>{definition.exHardLine,
                                      definition.exHardBackground,
                                      definition.exHardLine,
                                      definition.exHardBackground},
      std::array<std::string_view, 4>{definition.hazardLine,
                                      definition.hazardBackground,
                                      definition.hazardLine,
                                      definition.hazardBackground},
  };
  for (std::size_t row = 0; row < colors.size(); ++row) {
    for (std::size_t column = 0; column < colors[row].size(); ++column) {
      if (!applyPinnedDirectColor(colors[row][column],
                                  output.rgba[row][column])) {
        return fail(request.decoding, "skin_lua_model_gaugegraph_invalid",
                    "Lua skin gaugegraph has an invalid direct colour");
      }
    }
  }
  return true;
}

bool makeBpmGraphObject(GameplayDecodeRequest &request,
                        const RawSkinBpmGraph &definition,
                        SkinBpmGraphObject &output) {
  output.delayMillis = definition.delayMillis > 0 ? definition.delayMillis : 0;
  output.lineWidth = definition.lineWidth > 0 ? definition.lineWidth : 2;
  if (!applyPinnedRgbColor(definition.mainBpmColor, output.mainRgba) ||
      !applyPinnedRgbColor(definition.minimumBpmColor, output.minimumRgba) ||
      !applyPinnedRgbColor(definition.maximumBpmColor, output.maximumRgba) ||
      !applyPinnedRgbColor(definition.otherBpmColor, output.otherRgba) ||
      !applyPinnedRgbColor(definition.stopLineColor, output.stopRgba) ||
      !applyPinnedRgbColor(definition.transitionLineColor,
                           output.transitionRgba)) {
    return fail(request.decoding, "skin_lua_model_bpmgraph_invalid",
                "Lua skin bpmgraph has a nonempty normalized colour shorter "
                "than six hexadecimal digits");
  }
  return true;
}

std::uint32_t timingVisualizerColor(std::string_view value) {
  if (value.size() < 6 ||
      std::ranges::any_of(value, [](char character) {
        return hexNibble(character) < 0;
      })) {
    return 0xff0000ffU;
  }
  const auto component = [&](std::size_t offset) {
    return static_cast<std::uint32_t>(hexNibble(value[offset]) * 16 +
                                      hexNibble(value[offset + 1]));
  };
  const std::uint32_t alpha = value.size() == 8 ? component(6) : 0xffU;
  return (component(0) << 24U) | (component(2) << 16U) |
         (component(4) << 8U) | alpha;
}

std::optional<std::uint32_t>
opaqueTimingVisualizerPoorColor(std::string_view value) {
  if (!value.empty() && value.front() == '#') {
    value.remove_prefix(1);
  }
  if (value.size() < 6) {
    return std::nullopt;
  }
  const auto component = [&](std::size_t offset)
      -> std::optional<std::uint32_t> {
    const int high = hexNibble(value[offset]);
    const int low = hexNibble(value[offset + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(high * 16 + low);
  };
  const auto red = component(0);
  const auto green = component(2);
  const auto blue = component(4);
  const auto alpha = value.size() == 8 ? component(6)
                                       : std::optional<std::uint32_t>{0xffU};
  if (!red || !green || !blue || !alpha) {
    return std::nullopt;
  }
  return (*red << 24U) | (*green << 16U) | (*blue << 8U) | *alpha;
}

bool makeTimingVisualizerObject(GameplayDecodeRequest &request,
                                const RawSkinTimingVisualizer &definition,
                                SkinTimingVisualizerObject &output) {
  const bool transparent = definition.transparent == 1;
  const auto poorColor = transparent
                             ? std::optional<std::uint32_t>{0U}
                             : opaqueTimingVisualizerPoorColor(
                                   definition.prColor);
  if (!poorColor) {
    return fail(request.decoding, "skin_lua_model_timingvisualizer_invalid",
                "Lua skin timingvisualizer opaque PRColor is invalid");
  }
  output = {
      .width = definition.width,
      .judgeWidthMillis = definition.judgeWidthMillis,
      .lineWidth = std::clamp(definition.lineWidth, 1, 4),
      .judgeRgba = {timingVisualizerColor(definition.pgColor),
                    timingVisualizerColor(definition.grColor),
                    timingVisualizerColor(definition.gdColor),
                    timingVisualizerColor(definition.bdColor),
                    *poorColor},
      .lineRgba = timingVisualizerColor(definition.lineColor),
      .centerRgba = timingVisualizerColor(definition.centerColor),
      .transparent = transparent,
      .drawDecay = definition.drawDecay == 1,
  };
  return true;
}

bool makeTimingDistributionGraphObject(
    GameplayDecodeRequest &request,
    const RawSkinTimingDistributionGraph &definition,
    SkinTimingDistributionGraphObject &output) {
  // MathUtils.clamp checks value < min before value > max even when its
  // bounds are reversed. Preserve that order before the constructor's only
  // failing operation, its integer division by lw.
  const int width = definition.width > 1 ? definition.width : 1;
  const int lineWidth = definition.lineWidth < 1
                            ? 1
                            : definition.lineWidth > definition.width
                                  ? definition.width
                                  : definition.lineWidth;
  if (lineWidth == 0) {
    return fail(request.decoding, "skin_lua_model_timingdistributiongraph_invalid",
                "Lua skin timingdistributiongraph constructor divides by "
                "zero after its pinned line-width clamp");
  }
  output = {
      .width = width,
      .lineWidth = lineWidth,
      .graphRgba = timingVisualizerColor(definition.graphColor),
      .averageRgba = timingVisualizerColor(definition.averageColor),
      .devRgba = timingVisualizerColor(definition.devColor),
      .judgeRgba = {timingVisualizerColor(definition.pgColor),
                    timingVisualizerColor(definition.grColor),
                    timingVisualizerColor(definition.gdColor),
                    timingVisualizerColor(definition.bdColor),
                    timingVisualizerColor(definition.prColor)},
      .drawAverage = definition.drawAverage == 1,
      .drawDev = definition.drawDev == 1,
  };
  return true;
}

bool makeHitErrorVisualizerObject(
    GameplayDecodeRequest &request,
    const RawSkinHitErrorVisualizer &definition,
    SkinHitErrorVisualizerObject &output) {
  const bool transparent = definition.transparent == 1;
  const auto poorColor = transparent
                             ? std::optional<std::uint32_t>{0U}
                             : opaqueTimingVisualizerPoorColor(
                                   definition.prColor);
  if (!poorColor) {
    return fail(request.decoding, "skin_lua_model_hiterrorvisualizer_invalid",
                "Lua skin hiterrorvisualizer opaque PRColor is invalid");
  }
  output = {
      .width = definition.width,
      .judgeWidthMillis = definition.judgeWidthMillis,
      .lineWidth = std::clamp(definition.lineWidth, 1, 4),
      .colorMode = definition.colorMode == 1,
      .hitErrorMode = definition.hitErrorMode == 1,
      .emaMode = definition.emaMode,
      .judgeRgba = {timingVisualizerColor(definition.pgColor),
                    timingVisualizerColor(definition.grColor),
                    timingVisualizerColor(definition.gdColor),
                    timingVisualizerColor(definition.bdColor), *poorColor},
      .lineRgba = timingVisualizerColor(definition.lineColor),
      .centerRgba = timingVisualizerColor(definition.centerColor),
      .emaRgba = timingVisualizerColor(definition.emaColor),
      .alpha = static_cast<float>(definition.alpha),
      .windowLength = std::clamp(definition.windowLength, 1, 100),
      .transparent = transparent,
      .drawDecay = definition.drawDecay == 1,
  };
  return true;
}

std::optional<SkinBlendMode> blendMode(int value) {
  switch (value) {
  case 0:
  case 1:
    return SkinBlendMode::Normal;
  case 2:
    return SkinBlendMode::Additive;
  case 3:
    return SkinBlendMode::Subtractive;
  case 4:
    return SkinBlendMode::Multiply;
  case 9:
    return SkinBlendMode::Inverse;
  default:
    return std::nullopt;
  }
}

bool normalizeDestination(GameplayDecodeRequest &request,
                          const RawDestination &raw,
                          std::uint32_t authoredOrdinal,
                          SkinDestinationBody &output, bool sortFrames = true);

bool makeCoverObject(GameplayDecodeRequest &request,
                     const RawSkinCover &definition, SkinCoverObject &output) {
  const auto normalized = normalizeSkinCover(
      {.kind = definition.kind,
       .sprite = definition.image.sprite,
       .authoredDisappearLine = static_cast<double>(definition.disappearLine),
       .authoredDisappearLineLinksLift = definition.disappearLineLinksLift,
       .lineScale = 1.0});
  if (!normalized.cover) {
    request.result.diagnostics.push_back(
        diagnostic("skin_lua_model_cover_invalid",
                   "Lua skin cover definition cannot be normalized"));
    output = SkinCoverObject{};
    return true;
  }
  if (!consumeMaterializedSpriteFrames(
          request, normalized.cover->sprite.frames.size())) {
    return false;
  }
  output = *normalized.cover;
  return true;
}

bool makeJudgeObject(GameplayDecodeRequest &request, BeatorajaSkinModel &model,
                     const RawSkinJudge &definition, SkinJudgeObject &output);

std::optional<int> parsePinnedDestinationInteger(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  const char *first = value.data();
  const char *last = first + value.size();
  if (*first == '+') {
    ++first;
    if (first == last) {
      return std::nullopt;
    }
  }
  int parsed = 0;
  const auto [end, error] = std::from_chars(first, last, parsed);
  if (error != std::errc{} || end != last) {
    return std::nullopt;
  }
  return parsed;
}

bool makeObjectPayload(GameplayDecodeRequest &request, std::string_view name,
                       BeatorajaSkinModel &model, SkinObjectPayload &output,
                       bool &critical, bool &ignored) {
  ignored = false;
  // JSONSkinLoader parses destination IDs before it asks the gameplay object
  // loader.  A negative ID is always a SkinImage(SkinSourceReference(-id)),
  // including references whose source resolves to null at frame time.
  if (const auto destinationId = parsePinnedDestinationInteger(name);
      destinationId && *destinationId < 0) {
    const int referenceId = *destinationId == std::numeric_limits<int>::min()
                                ? std::numeric_limits<int>::min()
                                : -*destinationId;
    output = SkinBuiltinImageObject{.referenceId = referenceId};
    return true;
  }
  const auto image = request.images.find(name);
  const auto imageSet = request.imageSets.find(name);
  const auto number = request.numbers.find(name);
  const auto floating = request.floats.find(name);
  const auto slider = request.sliders.find(name);
  const auto text = request.texts.find(name);
  const auto graph = request.graphs.find(name);
  const auto gaugeGraph = request.gaugeGraphs.find(name);
  const auto bpmGraph = request.bpmGraphs.find(name);
  const auto hitErrorVisualizer = request.hitErrorVisualizers.find(name);
  const auto judgeGraph = request.judgeGraphs.find(name);
  const auto timingVisualizer = request.timingVisualizers.find(name);
  const auto timingDistributionGraph =
      request.timingDistributionGraphs.find(name);
  const auto pmChara = request.pmCharas.find(name);
  const auto hiddenCover = request.hiddenCovers.find(name);
  const auto liftCover = request.liftCovers.find(name);
  const auto judge = request.judges.find(name);
  const bool isNote = request.note && request.note->id == name;
  const bool isGauge = request.gauge && request.gauge->id == name;
  const bool isBga = request.bga && request.bga->id == name;
  const std::array candidates{
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Image,
                                    .matches = image != request.images.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::ImageSet,
                                    .matches =
                                        imageSet != request.imageSets.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Value,
                                    .matches = number != request.numbers.end()},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::FloatValue,
          .matches = floating != request.floats.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Text,
                                    .matches = text != request.texts.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Slider,
                                    .matches = slider != request.sliders.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Graph,
                                    .matches = graph != request.graphs.end()},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::GaugeGraph,
          .matches = gaugeGraph != request.gaugeGraphs.end()},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::JudgeGraph,
          .matches = judgeGraph != request.judgeGraphs.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::BpmGraph,
                                    .matches =
                                        bpmGraph != request.bpmGraphs.end()},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::HitErrorVisualizer,
          .matches = hitErrorVisualizer != request.hitErrorVisualizers.end()},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::TimingVisualizer,
          .matches = timingVisualizer != request.timingVisualizers.end()},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::TimingDistributionGraph,
          .matches = timingDistributionGraph !=
                     request.timingDistributionGraphs.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Gauge,
                                    .matches = isGauge},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Note,
                                    .matches = isNote},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::HiddenCover,
          .matches = hiddenCover != request.hiddenCovers.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::LiftCover,
                                    .matches =
                                        liftCover != request.liftCovers.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Bga,
                                    .matches = isBga},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Judge,
                                    .matches = judge != request.judges.end()},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::PmChara,
                                    .matches =
                                        pmChara != request.pmCharas.end()},
  };
  const auto resolved = resolveSkinObjectPrecedence(candidates);
  if (resolved.status == SkinObjectResolutionStatus::Unsupported) {
    output = SkinBlankObject{};
    return true;
  }
  // JSONSkinLoader simply leaves obj null when neither its generic loader nor
  // JsonPlaySkinObjectLoader resolves a destination ID, then skips that
  // destination. This is common in skins that share a module-level
  // destination name with a configuration-dependent object definition.
  if (resolved.status == SkinObjectResolutionStatus::NotFound) {
    ignored = true;
    return true;
  }
  if (resolved.status != SkinObjectResolutionStatus::Found) {
    return fail(request.decoding, "skin_lua_model_unsupported_object",
                "Lua skin destination '" + std::string(name) +
                    "' does not resolve to an audited v1 object");
  }

  if (image != request.images.end()) {
    SkinImageObject object;
    if (!makeImageObject(request, image->second, object)) {
      return false;
    }
    output = std::move(object);
    return true;
  }
  if (imageSet != request.imageSets.end()) {
    SkinImageObject object;
    object.stateIndex = imageSet->second.stateIndex;
    object.clickEvent = imageSet->second.clickEvent;
    object.clickMode = imageSet->second.clickMode;
    object.orderedStates.reserve(imageSet->second.imageIds.size());
    for (const std::string &imageId : imageSet->second.imageIds) {
      const auto state = request.images.find(imageId);
      const std::size_t frameCount = state != request.images.end()
                                         ? state->second.sprite.frames.size()
                                         : 0;
      if (!consumeMaterializedSpriteFrames(request, frameCount)) {
        return false;
      }
      object.orderedStates.push_back(state != request.images.end()
                                         ? state->second.sprite
                                         : SkinSpriteFrames{});
    }
    output = std::move(object);
    return true;
  }
  if (pmChara != request.pmCharas.end()) {
    const RawSkinPmChara &definition = pmChara->second;
    const auto source = request.sourceIds.find(definition.source);
    // JsonPlaySkinObjectLoader leaves obj null when getSrcIdPath cannot
    // resolve the named source or PomyuCharaLoader rejects its type.
    if (source == request.sourceIds.end() || definition.type < 0 ||
        definition.type > 15) {
      ignored = true;
      return true;
    }
    output = SkinPmCharaObject{
        .source = source->second,
        .color = definition.color == 2 ? 2 : 1,
        .type = definition.type,
        .side = definition.side == 2 ? 2 : 1,
    };
    return true;
  }
  if (number != request.numbers.end()) {
    NumericGlyphAtlas atlas;
    if (!materializeNumericGlyphAtlas(
            request, NumericGlyphAtlasKind::Number, number->second.image.sprite,
            {.integerDigits = number->second.digitCount,
             .zeroPadding = number->second.zeroPadding,
             .numberPadding = number->second.padding,
             .perDigitOffsets = number->second.perDigitOffsets},
            atlas)) {
      return false;
    }
    SkinNumberObject object;
    object.digits = std::move(atlas.digits);
    object.value = number->second.value;
    object.digitCount = atlas.format.integerDigits;
    object.spacing = number->second.spacing;
    object.alignment = number->second.alignment;
    object.zeroPadding = atlas.format.zeroPadding;
    object.perDigitOffsets = std::move(atlas.format.perDigitOffsets);
    output = std::move(object);
    return true;
  }
  if (floating != request.floats.end()) {
    NumericGlyphAtlas atlas;
    if (!materializeNumericGlyphAtlas(
            request, NumericGlyphAtlasKind::Float,
            floating->second.image.sprite,
            {.integerDigits = floating->second.integerDigits,
             .fractionalDigits = floating->second.fractionalDigits,
             .zeroPadding = floating->second.zeroPadding,
             .signVisible = floating->second.signVisible,
             .gain = floating->second.gain,
             .perDigitOffsets = floating->second.perDigitOffsets},
            atlas)) {
      return false;
    }
    SkinFloatObject object;
    object.digits = std::move(atlas.digits);
    object.value = floating->second.value;
    object.integerDigits = atlas.format.integerDigits;
    object.fractionalDigits = atlas.format.fractionalDigits;
    object.spacing = floating->second.spacing;
    object.alignment = floating->second.alignment;
    object.zeroPadding = atlas.format.zeroPadding;
    object.signVisible = atlas.format.signVisible;
    object.gain = atlas.format.gain;
    object.perDigitOffsets = std::move(atlas.format.perDigitOffsets);
    output = std::move(object);
    return true;
  }
  if (slider != request.sliders.end()) {
    if (!consumeMaterializedSpriteFrames(
            request, slider->second.image.sprite.frames.size())) {
      return false;
    }
    SkinSliderObject object;
    object.knob = slider->second.image.sprite;
    if (slider->second.explicitValue) {
      object.value = *slider->second.explicitValue;
      object.writer = slider->second.writer;
    } else if (slider->second.isRefNum) {
      object.value = SkinSliderObject::IntegerRangeSource{
          .value =
              slider->second.integerValue.value_or(SkinIntegerPropertyId{}),
          .minimum = slider->second.minimum,
          .maximum = slider->second.maximum,
      };
    } else {
      object.value =
          slider->second.implicitValue.value_or(SkinFloatPropertyId{});
      object.writer = slider->second.writer;
    }
    object.direction = slider->second.direction;
    object.range = static_cast<double>(slider->second.range);
    object.changeable = slider->second.changeable;
    output = std::move(object);
    return true;
  }
  if (text != request.texts.end()) {
    SkinTextObject object;
    if (!makeTextObject(request, text->second, object)) {
      return false;
    }
    output = std::move(object);
    return true;
  }
  if (graph != request.graphs.end()) {
    SkinGraphObject object;
    if (!makeGraphObject(request, graph->second, object)) {
      return false;
    }
    output = std::move(object);
    return true;
  }

  // JsonSkinObjectLoader resolves generic definitions before the PlaySkin
  // special branches. Preserve that behavior even when an authored ID is
  // shared across categories.
  if (resolved.kind == SkinObjectResolutionKind::GaugeGraph &&
      gaugeGraph != request.gaugeGraphs.end()) {
    SkinGaugeGraphObject object;
    if (!makeGaugeGraphObject(request, gaugeGraph->second, object)) {
      return false;
    }
    output = object;
    return true;
  }
  if (resolved.kind == SkinObjectResolutionKind::BpmGraph &&
      bpmGraph != request.bpmGraphs.end()) {
    SkinBpmGraphObject object;
    if (!makeBpmGraphObject(request, bpmGraph->second, object)) {
      return false;
    }
    output = object;
    return true;
  }

  if (isGauge) {
    SkinGaugeObject object;
    if (!makeGaugeObject(request, *request.gauge, object)) {
      return false;
    }
    output = std::move(object);
    return true;
  }

  if (resolved.kind == SkinObjectResolutionKind::JudgeGraph &&
      judgeGraph != request.judgeGraphs.end()) {
    SkinNoteDistributionGraphObject object;
    if (!makeNoteDistributionGraphObject(request, judgeGraph->second, object)) {
      return false;
    }
    output = object;
    return true;
  }

  if (resolved.kind == SkinObjectResolutionKind::TimingVisualizer &&
      timingVisualizer != request.timingVisualizers.end()) {
    SkinTimingVisualizerObject object;
    if (!makeTimingVisualizerObject(request, timingVisualizer->second, object)) {
      return false;
    }
    output = object;
    return true;
  }

  if (resolved.kind == SkinObjectResolutionKind::TimingDistributionGraph &&
      timingDistributionGraph != request.timingDistributionGraphs.end()) {
    SkinTimingDistributionGraphObject object;
    if (!makeTimingDistributionGraphObject(request,
                                           timingDistributionGraph->second,
                                           object)) {
      return false;
    }
    output = object;
    return true;
  }

  if (resolved.kind == SkinObjectResolutionKind::HitErrorVisualizer &&
      hitErrorVisualizer != request.hitErrorVisualizers.end()) {
    SkinHitErrorVisualizerObject object;
    if (!makeHitErrorVisualizerObject(request, hitErrorVisualizer->second,
                                      object)) {
      return false;
    }
    output = object;
    return true;
  }

  if (isNote) {
    critical = true;
    output = request.note->object;
    return true;
  }
  if (hiddenCover != request.hiddenCovers.end()) {
    SkinCoverObject object;
    if (!makeCoverObject(request, hiddenCover->second, object)) {
      return false;
    }
    output = std::move(object);
    return true;
  }
  if (liftCover != request.liftCovers.end()) {
    SkinCoverObject object;
    if (!makeCoverObject(request, liftCover->second, object)) {
      return false;
    }
    output = std::move(object);
    return true;
  }
  if (isBga) {
    output = SkinBgaObject{};
    return true;
  }
  if (judge != request.judges.end()) {
    SkinJudgeObject object;
    if (!makeJudgeObject(request, model, judge->second, object)) {
      return false;
    }
    output = std::move(object);
    return true;
  }
  return fail(request.decoding, "skin_lua_model_invalid",
              "Lua skin destination '" + std::string(name) +
                  "' does not resolve to an audited v1 object");
}

bool normalizeDestination(GameplayDecodeRequest &request,
                          const RawDestination &raw,
                          std::uint32_t authoredOrdinal,
                          SkinDestinationBody &output, bool sortFrames) {
  const auto mappedBlend = blendMode(raw.blend);
  if (!mappedBlend || raw.filter < 0 || raw.filter > 1 || raw.stretch < -1 ||
      raw.stretch > 10) {
    return fail(request.decoding, "skin_lua_model_invalid",
                "Lua skin destination presentation mode is unsupported");
  }
  output.loop = raw.loop;
  output.center = raw.center >= 0 && raw.center < 10 ? raw.center : 0;
  output.blend = *mappedBlend;
  output.filter = static_cast<SkinFilterMode>(raw.filter);
  if (raw.stretch >= 0) {
    output.stretch = static_cast<SkinStretchMode>(raw.stretch);
  }
  output.authoredOrdinal = authoredOrdinal;
  output.timer = raw.timer;
  output.offsetIds = raw.offsets;
  output.offsetIds.push_back(raw.offset);
  output.drawCondition = raw.drawCondition;
  output.mouseRect = raw.mouseRect;
  output.conditions.reserve(raw.conditions.size());
  for (const auto &condition : raw.conditions) {
    if (condition.property) {
      output.conditions.emplace_back(*condition.property);
    } else if (condition.optionId && *condition.optionId != 0) {
      output.conditions.emplace_back(*condition.optionId);
    }
  }

  SkinDestinationFrame current;
  std::optional<int> clipX;
  std::optional<int> clipY;
  std::optional<int> clipWidth;
  std::optional<int> clipHeight;
  output.frames.reserve(raw.frames.size());
  for (const auto &frame : raw.frames) {
    if (frame.time) {
      current.timeMillis = *frame.time;
    }
    if (frame.x) {
      current.x = static_cast<double>(*frame.x);
    }
    if (frame.y) {
      current.y = static_cast<double>(*frame.y);
    }
    if (frame.width) {
      current.width = static_cast<double>(*frame.width);
    }
    if (frame.height) {
      current.height = static_cast<double>(*frame.height);
    }
    if (frame.angle) {
      current.angleDegrees = static_cast<double>(*frame.angle);
    }
    if (frame.acceleration) {
      current.acceleration = *frame.acceleration;
    }
    const std::array colorValues{
        frame.red.value_or(current.rgba[0]),
        frame.green.value_or(current.rgba[1]),
        frame.blue.value_or(current.rgba[2]),
        frame.alpha.value_or(current.rgba[3]),
    };
    if (std::ranges::any_of(
            colorValues, [](int value) { return value < 0 || value > 255; })) {
      return fail(request.decoding, "skin_lua_model_invalid",
                  "Lua skin destination color is outside byte range");
    }
    current.rgba = {
        static_cast<std::uint8_t>(colorValues[0]),
        static_cast<std::uint8_t>(colorValues[1]),
        static_cast<std::uint8_t>(colorValues[2]),
        static_cast<std::uint8_t>(colorValues[3]),
    };

    if (frame.clipX) {
      clipX = frame.clipX;
    }
    if (frame.clipY) {
      clipY = frame.clipY;
    }
    if (frame.clipWidth) {
      clipWidth = frame.clipWidth;
    }
    if (frame.clipHeight) {
      clipHeight = frame.clipHeight;
    }
    if (clipX && clipY && clipWidth && clipHeight) {
      current.clip = SkinSourceRect{
          .x = *clipX,
          .y = *clipY,
          .w = *clipWidth,
          .h = *clipHeight,
      };
    }
    output.frames.push_back(current);
  }
  if (sortFrames) {
    std::stable_sort(output.frames.begin(), output.frames.end(),
                     [](const auto &left, const auto &right) {
                       return left.timeMillis < right.timeMillis;
                     });
  }
  return true;
}

bool makeJudgeObject(GameplayDecodeRequest &request, BeatorajaSkinModel &model,
                     const RawSkinJudge &definition, SkinJudgeObject &output) {
  SkinJudgeNormalizationInput input;
  input.player = definition.player;
  input.shiftImageByHalfDetailWidth = definition.shift;
  input.images.resize(definition.images.size());
  input.numbers.resize(definition.numbers.size());

  for (std::size_t grade = 0; grade < definition.images.size(); ++grade) {
    const auto image = request.images.find(definition.images[grade].id);
    if (image == request.images.end()) {
      continue;
    }
    // JsonPlaySkinObjectLoader constructs Judge images directly from the
    // referenced Image's source regions. Its Image.len/ref/act fields never
    // participate in this nested path.
    SkinImageObject child{.orderedStates = {image->second.sprite}};
    SkinDestinationBody destination;
    if (!consumeMaterializedSpriteFrames(request,
                                         image->second.sprite.frames.size()) ||
        !normalizeDestination(request, definition.images[grade],
                              static_cast<std::uint32_t>(grade), destination)) {
      return false;
    }
    input.images[grade] = SkinJudgeInlineImageChild{
        .authoredId = definition.images[grade].id,
        .authoredIndex = grade,
        .image = std::move(child),
        .destination = std::move(destination),
    };
  }
  for (std::size_t grade = 0; grade < definition.numbers.size(); ++grade) {
    const auto number = request.numbers.find(definition.numbers[grade].id);
    if (number == request.numbers.end()) {
      continue;
    }
    SkinDestinationBody destination;
    if (!normalizeDestination(request, definition.numbers[grade],
                              static_cast<std::uint32_t>(grade), destination)) {
      return false;
    }
    auto normalizedNumber =
        normalizeSkinJudgeNumber({.source = number->second.image.sprite,
                                  .value = number->second.value,
                                  .digitCount = number->second.digitCount,
                                  .spacing = number->second.spacing,
                                  .offsets = number->second.perDigitOffsets,
                                  .destination = std::move(destination)});
    if (!normalizedNumber.number) {
      request.result.diagnostics.push_back(
          diagnostic("skin_lua_model_judge_number_invalid",
                     "Lua skin Judge detail number cannot be normalized"));
      continue;
    }
    if (!consumeMaterializedSpriteFrames(
            request, number->second.image.sprite.frames.size())) {
      return false;
    }
    input.numbers[grade] = SkinJudgeInlineNumberChild{
        .authoredId = definition.numbers[grade].id,
        .authoredIndex = grade,
        .presentation = std::move(*normalizedNumber.number),
    };
  }

  auto normalized = normalizeSkinJudge(input);
  if (!normalized.judge) {
    return fail(request.decoding, "skin_lua_model_judge_invalid",
                "Lua skin Judge has unsafe child cardinality or presentation");
  }
  output.player = normalized.judge->player;
  output.shiftImageByHalfDetailWidth =
      normalized.judge->shiftImageByHalfDetailWidth;
  output.grades.resize(SkinJudgeNormalizationPolicy::runtimeGradeSlots);
  for (std::size_t grade = 0; grade < output.grades.size(); ++grade) {
    auto &normalizedGrade = normalized.judge->grades[grade];
    auto addChild = [&](SkinObjectPayload payload,
                        SkinDestinationBody destination,
                        std::string suffix) -> SkinNestedObjectPresentation {
      const SkinObjectId id = request.nextSyntheticObjectId++;
      model.objects.push_back({
          .id = id,
          .authoredName = "__judge/" + definition.id + "/" + suffix + "/" +
                          std::to_string(grade),
          .payload = std::move(payload),
          .authoredOrdinal = static_cast<std::uint32_t>(grade),
          .critical = false,
      });
      return {.object = id, .destination = std::move(destination)};
    };
    if (normalizedGrade.image) {
      output.grades[grade].image =
          addChild(std::move(normalizedGrade.image->image),
                   std::move(normalizedGrade.image->destination), "image");
    }
    if (normalizedGrade.detailNumber) {
      output.grades[grade].detailNumber = addChild(
          std::move(normalizedGrade.detailNumber->presentation.number),
          std::move(normalizedGrade.detailNumber->presentation.destination),
          "number");
    }
  }
  return true;
}

void transferDecodeDiagnostics(GameplayDecodeRequest &request) {
  auto &source = request.decoding.result.diagnostics;
  request.result.diagnostics.insert(request.result.diagnostics.end(),
                                    std::make_move_iterator(source.begin()),
                                    std::make_move_iterator(source.end()));
  source.clear();
}

template <typename Definitions>
void recordUnsupportedDefinitions(GameplayDecodeRequest &request,
                                  const Definitions &definitions,
                                  std::string_view code,
                                  std::string_view surface) {
  if (definitions.empty()) {
    return;
  }
  request.result.diagnostics.push_back(
      {.code = std::string(code),
       .message = "Lua skin " + std::string(surface) +
                  " definitions are unsupported in v1",
       .severity = DiagnosticSeverity::Warning});
}

void decodeGameplayProtected(lua_State *state, int index,
                             void *opaque) noexcept {
  auto *request = static_cast<GameplayDecodeRequest *>(opaque);
  if (request == nullptr) {
    return;
  }
  try {
    decodeHeaderProtected(state, index, &request->decoding);
    if (!request->decoding.result.header) {
      transferDecodeDiagnostics(*request);
      return;
    }
    request->decoding.enforceGameplayLimits = request->enforceGameplayLimits;

    request->result.model.emplace();
    auto &model = *request->result.model;
    model.header = std::move(*request->decoding.result.header);
    if (!gameplaySkinTraitForSkinType(model.header.type)) {
      fail(request->decoding, "skin_lua_model_type_unsupported",
           "Lua gameplay skins require a supported Beatoraja gameplay type");
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    if (!integerField(state, index, "fadeout", model.timing.fadeoutMillis,
                      request->decoding) ||
        !integerField(state, index, "input", model.timing.inputMillis,
                      request->decoding) ||
        !integerField(state, index, "scene", model.timing.sceneMillis,
                      request->decoding) ||
        !integerField(state, index, "close", model.timing.closeMillis,
                      request->decoding) ||
        !integerField(state, index, "loadend", model.timing.loadEndMillis,
                      request->decoding) ||
        !integerField(state, index, "playstart", model.timing.playStartMillis,
                      request->decoding) ||
        !integerField(state, index, "judgetimer", model.timing.judgeTimerMillis,
                      request->decoding) ||
        !integerField(state, index, "finishmargin",
                      model.timing.finishMarginMillis, request->decoding)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }

    if (!decodeObjectArrayField(state, index, "source", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawSources, request->decoding,
                                decodeRawSource)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawSources.size();
         ++ordinal) {
      const auto &source = request->rawSources[ordinal];
      const auto id = SkinResourceId{static_cast<std::uint32_t>(ordinal + 1)};
      // JSONSkinLoader's source map overwrites earlier declarations. Preserve
      // every resource for ownership, but bind image references to the last
      // authored source with the same name.
      request->sourceIds.insert_or_assign(source.id, id);
      model.resources.emplace_back(SkinImageResource{
          .id = id,
          .authoredName = source.id,
          .virtualPath = source.path,
          .authoredOrdinal = static_cast<std::uint32_t>(ordinal),
      });
    }

    if (!decodeObjectArrayField(state, index, "font", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawFonts, request->decoding,
                                decodeRawFont)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    request->fonts.reserve(request->rawFonts.size());
    for (auto &font : request->rawFonts) {
      const auto ordinal = static_cast<std::uint32_t>(model.resources.size());
      const auto id = SkinResourceId{ordinal + 1};
      SkinFontResource resource{
          .id = id,
          .authoredName = font.id,
          .virtualPath = font.path,
          .type = font.type,
          .authoredOrdinal = ordinal,
      };
      resource.fallbacks.reserve(font.fallbacks.size());
      for (auto &fallback : font.fallbacks) {
        resource.fallbacks.push_back(
            {.virtualPath = std::move(fallback.path), .type = fallback.type});
      }
      request->fonts.push_back(resource);
      model.resources.emplace_back(std::move(resource));
    }

    if (!decodeObjectArrayField(state, index, "image", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawImages, request->decoding,
                                decodeRawImage)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawImages.size();
         ++ordinal) {
      auto &image = request->rawImages[ordinal];
      image.authoredIndex = static_cast<std::uint32_t>(ordinal + 1);
      if (!expandImageFrames(*request, image)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

    if (!decodeObjectArrayField(state, index, "imageset", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawImageSets, request->decoding,
                                decodeRawImageSet)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawImageSets.size();
         ++ordinal) {
      request->rawImageSets[ordinal].authoredIndex =
          static_cast<std::uint32_t>(ordinal + 1);
    }

    if (!decodeObjectArrayField(state, index, "value", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawNumbers, request->decoding,
                                decodeRawNumber)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawNumbers.size();
         ++ordinal) {
      auto &number = request->rawNumbers[ordinal];
      number.image.authoredIndex = static_cast<std::uint32_t>(ordinal + 1);
      if (!expandImageFrames(*request, number.image)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

    if (!decodeObjectArrayField(state, index, "floatvalue", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawFloats, request->decoding,
                                decodeRawFloat)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawFloats.size();
         ++ordinal) {
      auto &number = request->rawFloats[ordinal];
      number.image.authoredIndex = static_cast<std::uint32_t>(ordinal + 1);
      if (!expandImageFrames(*request, number.image)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

    if (!decodeObjectArrayField(state, index, "slider", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawSliders, request->decoding,
                                decodeRawSlider)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawSliders.size();
         ++ordinal) {
      auto &slider = request->rawSliders[ordinal];
      slider.image.authoredIndex = static_cast<std::uint32_t>(ordinal + 1);
      if (!expandImageFrames(*request, slider.image)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

    if (!decodeObjectArrayField(state, index, "text", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawTexts, request->decoding,
                                decodeRawText)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawTexts.size();
         ++ordinal) {
      request->rawTexts[ordinal].authoredIndex =
          static_cast<std::uint32_t>(ordinal + 1);
    }

    if (!decodeObjectArrayField(state, index, "graph", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawGraphs, request->decoding,
                                decodeRawGraph)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawGraphs.size();
         ++ordinal) {
      auto &graph = request->rawGraphs[ordinal];
      graph.image.authoredIndex = static_cast<std::uint32_t>(ordinal + 1);
      if (!expandImageFrames(*request, graph.image)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

    if (!decodeObjectArrayField(state, index, "gaugegraph", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawGaugeGraphs, request->decoding,
                                decodeRawGaugeGraph) ||
        !decodeObjectArrayField(state, index, "bpmgraph", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawBpmGraphs, request->decoding,
                                decodeRawBpmGraph) ||
        !decodeObjectArrayField(state, index, "hiterrorvisualizer", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawHitErrorVisualizers,
                                request->decoding,
                                decodeRawHitErrorVisualizer) ||
        !decodeObjectArrayField(state, index, "judgegraph", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawJudgeGraphs, request->decoding,
                                decodeRawNoteDistributionGraph) ||
        !decodeObjectArrayField(state, index, "timingvisualizer", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawTimingVisualizers,
                                request->decoding, decodeRawTimingVisualizer) ||
        !decodeObjectArrayField(state, index, "timingdistributiongraph", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawTimingDistributionGraphs,
                                request->decoding,
                                decodeRawTimingDistributionGraph) ||
        !decodeObjectArrayField(state, index, "pmchara", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawPmCharas, request->decoding,
                                decodeRawPmChara)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    if (!decodeObjectArrayField(state, index, "hiddenCover", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawHiddenCovers, request->decoding,
                                decodeRawHiddenCover) ||
        !decodeObjectArrayField(state, index, "liftCover", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawLiftCovers, request->decoding,
                                decodeRawLiftCover) ||
        !decodeObjectArrayField(state, index, "judge", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawJudges, request->decoding,
                                decodeRawJudge)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawHiddenCovers.size();
         ++ordinal) {
      auto &cover = request->rawHiddenCovers[ordinal];
      cover.image.authoredIndex = static_cast<std::uint32_t>(ordinal + 1);
      if (!expandImageFrames(*request, cover.image)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }
    for (std::size_t ordinal = 0; ordinal < request->rawLiftCovers.size();
         ++ordinal) {
      auto &cover = request->rawLiftCovers[ordinal];
      cover.image.authoredIndex = static_cast<std::uint32_t>(ordinal + 1);
      if (!expandImageFrames(*request, cover.image)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

    request->bga.emplace();
    if (!rawGetField(state, index, "bga", request->decoding)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    if (!lua_isnil(state, -1)) {
      if (!decodeRawIdentity(state, -1, 2, *request->bga, request->decoding)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    } else {
      request->bga.reset();
    }
    lua_pop(state, 1);

    request->gauge.emplace();
    if (!rawGetField(state, index, "gauge", request->decoding)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    if (!lua_isnil(state, -1)) {
      if (!decodeRawGauge(state, -1, 2, *request->gauge, request->decoding)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    } else {
      request->gauge.reset();
    }
    lua_pop(state, 1);

    request->note.emplace();
    if (!rawGetField(state, index, "note", request->decoding)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    if (!lua_isnil(state, -1)) {
      if (!decodeRawNote(state, -1, 2, *request->note, request->decoding)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    } else {
      request->note.reset();
    }
    lua_pop(state, 1);

    if (!decodeObjectArrayField(state, index, "destination", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawDestinations, request->decoding,
                                decodeRawDestination)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawDestinations.size();
         ++ordinal) {
      request->rawDestinations[ordinal].authoredIndex =
          static_cast<std::uint32_t>(ordinal + 1);
    }

    // JsonSkinLoader constructs custom events before custom timers, while the
    // runtime evaluates timers before events. Preserve each authored vector
    // independently and make that phase-order mismatch explicit once.
    if (!decodeObjectArrayField(state, index, "customEvents", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawCustomEvents, request->decoding,
                                decodeRawCustomEvent) ||
        !decodeObjectArrayField(state, index, "customTimers", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawCustomTimers, request->decoding,
                                decodeRawCustomTimer)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (std::size_t ordinal = 0; ordinal < request->rawCustomEvents.size();
         ++ordinal) {
      request->rawCustomEvents[ordinal].authoredIndex =
          static_cast<std::uint32_t>(ordinal + 1);
    }
    for (std::size_t ordinal = 0; ordinal < request->rawCustomTimers.size();
         ++ordinal) {
      request->rawCustomTimers[ordinal].authoredIndex =
          static_cast<std::uint32_t>(ordinal + 1);
    }
    if (!request->rawCustomEvents.empty() ||
        !request->rawCustomTimers.empty()) {
      request->result.diagnostics.push_back(
          {.code = "custom_object_order_authored_divergence",
           .message = "Pinned custom object construction order differs from "
                      "runtime timer/event phase order",
           .severity = DiagnosticSeverity::Warning});
    }
  } catch (...) {
    request->allocationFailed = true;
    request->result.model.reset();
  }
}

LuaValuePath bindingPath(std::string_view array, std::uint32_t index,
                         std::string_view field) {
  return {LuaValuePathElement::field(array), LuaValuePathElement::index(index),
          LuaValuePathElement::field(field)};
}

LuaValuePath bindingPath(std::string_view array, std::uint32_t index,
                         std::string_view field, std::uint32_t nestedIndex) {
  auto result = bindingPath(array, index, field);
  result.push_back(LuaValuePathElement::index(nestedIndex));
  return result;
}

std::string bindingPathText(std::string_view array, std::uint32_t index,
                            std::string_view field,
                            std::optional<std::uint32_t> nestedIndex = {}) {
  std::string result(array);
  result.push_back('[');
  result.append(std::to_string(index));
  result.append("].");
  result.append(field);
  if (nestedIndex) {
    result.push_back('[');
    result.append(std::to_string(*nestedIndex));
    result.push_back(']');
  }
  return result;
}

LuaValuePath
noteLineBindingPath(std::string_view array, std::uint32_t index,
                    std::string_view field,
                    std::optional<std::uint32_t> nestedIndex = {}) {
  LuaValuePath result{
      LuaValuePathElement::field("note"), LuaValuePathElement::field(array),
      LuaValuePathElement::index(index), LuaValuePathElement::field(field)};
  if (nestedIndex) {
    result.push_back(LuaValuePathElement::index(*nestedIndex));
  }
  return result;
}

std::string
noteLineBindingPathText(std::string_view array, std::uint32_t index,
                        std::string_view field,
                        std::optional<std::uint32_t> nestedIndex = {}) {
  std::string result("note.");
  result.append(array);
  result.push_back('[');
  result.append(std::to_string(index));
  result.append("].");
  result.append(field);
  if (nestedIndex) {
    result.push_back('[');
    result.append(std::to_string(*nestedIndex));
    result.push_back(']');
  }
  return result;
}

bool retainBindingFailure(GameplayDecodeRequest &request,
                          LuaSkinBindingDecodeResult decoded,
                          std::string path) {
  SkinDiagnostic failure =
      decoded.failure
          ? std::move(*decoded.failure)
          : diagnostic("skin_lua_binding_invalid",
                       "Lua binding decoder returned no typed binding");
  failure.virtualPath = std::move(path);
  const bool fatal = luaSkinBindingFailureIsFatal(failure.code);
  request.result.diagnostics.push_back(std::move(failure));
  return !fatal;
}

std::uint32_t nextBindingOrdinal(const LuaSkinBindingDecoder &decoder,
                                 SkinBindingKind kind) {
  const auto bindings = decoder.bindings();
  switch (kind) {
  case SkinBindingKind::BooleanProperty:
    return static_cast<std::uint32_t>(bindings.booleanProperties.size());
  case SkinBindingKind::IntegerProperty:
    return static_cast<std::uint32_t>(bindings.integerProperties.size());
  case SkinBindingKind::FloatProperty:
    return static_cast<std::uint32_t>(bindings.floatProperties.size());
  case SkinBindingKind::StringProperty:
    return static_cast<std::uint32_t>(bindings.stringProperties.size());
  case SkinBindingKind::TimerProperty:
    return static_cast<std::uint32_t>(bindings.timerProperties.size());
  case SkinBindingKind::FloatWriter:
    return static_cast<std::uint32_t>(bindings.floatWriters.size());
  case SkinBindingKind::StringWriter:
    return static_cast<std::uint32_t>(bindings.stringWriters.size());
  case SkinBindingKind::Event:
    return static_cast<std::uint32_t>(bindings.events.size());
  }
  return 0;
}

template <typename Id>
bool decodeRequiredBinding(GameplayDecodeRequest &request,
                           LuaSkinBindingDecoder &decoder,
                           const LuaValueHandle &value, SkinBindingType type,
                           LuaValuePath path, std::string pathText,
                           std::uint32_t, std::optional<int> fallbackNumeric,
                           Id &output) {
  auto decoded = decoder.decode(
      value, {.type = type,
              .path = std::move(path),
              .authoredOrdinal = nextBindingOrdinal(decoder, type.kind),
              .fallbackNumeric = fallbackNumeric});
  if (!decoded.id) {
    output = Id{};
    if (!decoded.failure) {
      // JsonSkin's Lua serializer returns null for non-scalar values.  Some
      // loader fields are structurally required even though the resolved
      // Beatoraja property is null; preserve that null for model validation
      // instead of manufacturing a binding-type diagnostic.
      return true;
    }
    return retainBindingFailure(request, std::move(decoded),
                                std::move(pathText));
  }
  const auto *typed = std::get_if<Id>(&*decoded.id);
  if (typed == nullptr || !*typed) {
    output = Id{};
    return retainBindingFailure(request, std::move(decoded),
                                std::move(pathText));
  }
  output = *typed;
  return true;
}

template <typename Id>
bool decodeOptionalBinding(GameplayDecodeRequest &request,
                           LuaSkinBindingDecoder &decoder,
                           const LuaValueHandle &value, SkinBindingType type,
                           LuaValuePath path, std::string pathText,
                           std::uint32_t, std::optional<Id> &output,
                           std::optional<int> fallbackNumeric = {},
                           bool numericFallbackOnly = false) {
  auto decoded = decoder.decode(
      value, {.type = type,
              .path = std::move(path),
              .authoredOrdinal = nextBindingOrdinal(decoder, type.kind),
              .fallbackNumeric = fallbackNumeric,
              .numericFallbackOnly = numericFallbackOnly});
  if (!decoded.id) {
    if (!decoded.failure ||
        decoded.failure->code == "skin_lua_binding_missing") {
      return true;
    }
    output = Id{};
    return retainBindingFailure(request, std::move(decoded),
                                std::move(pathText));
  }
  const auto *typed = std::get_if<Id>(&*decoded.id);
  if (typed == nullptr || !*typed) {
    output = Id{};
    return retainBindingFailure(request, std::move(decoded),
                                std::move(pathText));
  }
  output = *typed;
  return true;
}

bool bindImageTimer(GameplayDecodeRequest &request,
                    LuaSkinBindingDecoder &decoder, const LuaValueHandle &value,
                    std::string_view array, RawSkinImage &image) {
  if (!decodeOptionalBinding(
          request, decoder, value, {.kind = SkinBindingKind::TimerProperty},
          bindingPath(array, image.authoredIndex, "timer"),
          bindingPathText(array, image.authoredIndex, "timer"),
          image.authoredIndex - 1, image.timer)) {
    return false;
  }
  image.sprite.timer = image.timer;
  return true;
}

bool bindNoteLineDestination(GameplayDecodeRequest &request,
                             LuaSkinBindingDecoder &decoder,
                             const LuaValueHandle &value,
                             std::string_view array,
                             RawDestination &destination,
                             SkinBuiltinBindingCatalogView builtins) {
  if (!decodeOptionalBinding(
          request, decoder, value, {.kind = SkinBindingKind::TimerProperty},
          noteLineBindingPath(array, destination.authoredIndex, "timer"),
          noteLineBindingPathText(array, destination.authoredIndex, "timer"),
          destination.authoredIndex - 1, destination.timer)) {
    return false;
  }
  for (std::size_t index = 0; index < destination.conditions.size(); ++index) {
    auto &condition = destination.conditions[index];
    // SkinObject.setDrawCondition resolves BooleanPropertyFactory values
    // before retaining unknown numeric values as static skin options.
    if (condition.optionId &&
        !builtins.contains({.kind = SkinBindingKind::BooleanProperty},
                           SkinBuiltinPropertySelector{*condition.optionId})) {
      continue;
    }
    std::optional<SkinBooleanPropertyId> property;
    const auto oneBased = static_cast<std::uint32_t>(index + 1);
    if (!decodeOptionalBinding(
            request, decoder, value, {.kind = SkinBindingKind::BooleanProperty},
            noteLineBindingPath(array, destination.authoredIndex, "op",
                                oneBased),
            noteLineBindingPathText(array, destination.authoredIndex, "op",
                                    oneBased),
            destination.authoredIndex - 1, property)) {
      return false;
    }
    condition.property = property;
  }
  return decodeOptionalBinding(
      request, decoder, value, {.kind = SkinBindingKind::BooleanProperty},
      noteLineBindingPath(array, destination.authoredIndex, "draw"),
      noteLineBindingPathText(array, destination.authoredIndex, "draw"),
      destination.authoredIndex - 1, destination.drawCondition);
}

bool bindNoteLinePrefix(GameplayDecodeRequest &request,
                        LuaSkinBindingDecoder &decoder,
                        const LuaValueHandle &value, std::string_view array,
                        std::vector<RawDestination> &destinations,
                        std::size_t selectedCount,
                        SkinBuiltinBindingCatalogView builtins) {
  for (std::size_t index = 0; index < selectedCount; ++index) {
    if (!bindNoteLineDestination(request, decoder, value, array,
                                 destinations[index], builtins)) {
      return false;
    }
  }
  return true;
}

bool bindGameplayDefinitions(GameplayDecodeRequest &request,
                             LuaSkinBindingDecoder &decoder,
                             const LuaValueHandle &value,
                             SkinBuiltinBindingCatalogView builtins) {
  for (auto &image : request.rawImages) {
    if (!bindImageTimer(request, decoder, value, "image", image)) {
      return false;
    }
    const bool hasGenericDestination = std::ranges::any_of(
        request.rawDestinations,
        [&](const auto &destination) { return destination.id == image.id; });
    if (hasGenericDestination && image.stateCount > 1) {
      SkinIntegerPropertyId id;
      if (!decodeRequiredBinding(
              request, decoder, value,
              {.kind = SkinBindingKind::IntegerProperty,
               .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
              bindingPath("image", image.authoredIndex, "ref"),
              bindingPathText("image", image.authoredIndex, "ref"),
              image.authoredIndex - 1, image.stateSelector, id)) {
        return false;
      }
      image.stateIndex = id;
    }
    if (hasGenericDestination &&
        !decodeOptionalBinding(
            request, decoder, value, {.kind = SkinBindingKind::Event},
            bindingPath("image", image.authoredIndex, "act"),
            bindingPathText("image", image.authoredIndex, "act"),
            image.authoredIndex - 1, image.clickEvent)) {
      return false;
    }
  }

  for (auto &imageSet : request.rawImageSets) {
    if (!decodeRequiredBinding(
            request, decoder, value,
            {.kind = SkinBindingKind::IntegerProperty,
             .integerDomain = SkinIntegerPropertyDomain::ImageIndex},
            bindingPath("imageset", imageSet.authoredIndex, "value"),
            bindingPathText("imageset", imageSet.authoredIndex, "value"),
            imageSet.authoredIndex - 1, imageSet.stateSelector,
            imageSet.stateIndex) ||
        !decodeOptionalBinding(
            request, decoder, value, {.kind = SkinBindingKind::Event},
            bindingPath("imageset", imageSet.authoredIndex, "act"),
            bindingPathText("imageset", imageSet.authoredIndex, "act"),
            imageSet.authoredIndex - 1, imageSet.clickEvent)) {
      return false;
    }
  }

  for (auto &cover : request.rawHiddenCovers) {
    if (!bindImageTimer(request, decoder, value, "hiddenCover", cover.image)) {
      return false;
    }
  }
  for (auto &cover : request.rawLiftCovers) {
    if (!bindImageTimer(request, decoder, value, "liftCover", cover.image)) {
      return false;
    }
  }

  for (auto &number : request.rawNumbers) {
    if (!bindImageTimer(request, decoder, value, "value", number.image) ||
        !decodeRequiredBinding(
            request, decoder, value,
            {.kind = SkinBindingKind::IntegerProperty,
             .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
            bindingPath("value", number.image.authoredIndex, "value"),
            bindingPathText("value", number.image.authoredIndex, "value"),
            number.image.authoredIndex - 1, number.image.stateSelector,
            number.value)) {
      return false;
    }
  }

  for (auto &number : request.rawFloats) {
    if (!bindImageTimer(request, decoder, value, "floatvalue", number.image) ||
        !decodeRequiredBinding(
            request, decoder, value,
            {.kind = SkinBindingKind::FloatProperty,
             .floatDomain = SkinFloatPropertyDomain::FloatValue},
            bindingPath("floatvalue", number.image.authoredIndex, "value"),
            bindingPathText("floatvalue", number.image.authoredIndex, "value"),
            number.image.authoredIndex - 1, number.image.stateSelector,
            number.value)) {
      return false;
    }
  }

  for (auto &slider : request.rawSliders) {
    if (!bindImageTimer(request, decoder, value, "slider", slider.image) ||
        !decodeOptionalBinding(
            request, decoder, value,
            {.kind = SkinBindingKind::FloatProperty,
             .floatDomain = SkinFloatPropertyDomain::Rate},
            bindingPath("slider", slider.image.authoredIndex, "value"),
            bindingPathText("slider", slider.image.authoredIndex, "value"),
            slider.image.authoredIndex - 1, slider.explicitValue)) {
      return false;
    }
    if (slider.explicitValue) {
      if (!decodeOptionalBinding(
              request, decoder, value, {.kind = SkinBindingKind::FloatWriter},
              bindingPath("slider", slider.image.authoredIndex, "event"),
              bindingPathText("slider", slider.image.authoredIndex, "event"),
              slider.image.authoredIndex - 1, slider.writer)) {
        return false;
      }
      continue;
    }
    if (slider.isRefNum) {
      SkinIntegerPropertyId id;
      if (!decodeRequiredBinding(
              request, decoder, value,
              {.kind = SkinBindingKind::IntegerProperty,
               .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
              bindingPath("slider", slider.image.authoredIndex, "type"),
              bindingPathText("slider", slider.image.authoredIndex, "type"),
              slider.image.authoredIndex - 1, slider.typeSelector, id)) {
        return false;
      }
      slider.integerValue = id;
    } else {
      SkinFloatPropertyId id;
      if (!decodeRequiredBinding(
              request, decoder, value,
              {.kind = SkinBindingKind::FloatProperty,
               .floatDomain = SkinFloatPropertyDomain::Rate},
              bindingPath("slider", slider.image.authoredIndex, "type"),
              bindingPathText("slider", slider.image.authoredIndex, "type"),
              slider.image.authoredIndex - 1, slider.typeSelector, id)) {
        return false;
      }
      slider.implicitValue = id;
      const SkinBindingType writerType{.kind = SkinBindingKind::FloatWriter};
      const SkinBuiltinPropertySelector selector{slider.typeSelector};
      if (slider.changeable && builtins.contains(writerType, selector) &&
          !decodeOptionalBinding(
              request, decoder, value, writerType,
              bindingPath("slider", slider.image.authoredIndex, "event"),
              bindingPathText("slider", slider.image.authoredIndex, "event"),
              slider.image.authoredIndex - 1, slider.writer,
              slider.typeSelector, true)) {
        return false;
      }
    }
  }

  for (auto &text : request.rawTexts) {
    if (!decodeRequiredBinding(
            request, decoder, value, {.kind = SkinBindingKind::StringProperty},
            bindingPath("text", text.authoredIndex, "value"),
            bindingPathText("text", text.authoredIndex, "value"),
            text.authoredIndex - 1, text.refSelector, text.value)) {
      return false;
    }
    const SkinBindingType writerType{.kind = SkinBindingKind::StringWriter};
    if (text.writerFieldPresent) {
      if (!decodeOptionalBinding(
              request, decoder, value, writerType,
              bindingPath("text", text.authoredIndex, "event"),
              bindingPathText("text", text.authoredIndex, "event"),
              text.authoredIndex - 1, text.writer)) {
        return false;
      }
      text.writerWasExplicit = text.writer && static_cast<bool>(*text.writer);
    } else if (builtins.contains(
                   writerType, SkinBuiltinPropertySelector{text.refSelector}) &&
               !decodeOptionalBinding(
                   request, decoder, value, writerType,
                   bindingPath("text", text.authoredIndex, "event"),
                   bindingPathText("text", text.authoredIndex, "event"),
                   text.authoredIndex - 1, text.writer, text.refSelector,
                   true)) {
      return false;
    }
  }

  for (auto &graph : request.rawGraphs) {
    if (!bindImageTimer(request, decoder, value, "graph", graph.image) ||
        !decodeOptionalBinding(
            request, decoder, value,
            {.kind = SkinBindingKind::FloatProperty,
             .floatDomain = SkinFloatPropertyDomain::Rate},
            bindingPath("graph", graph.image.authoredIndex, "value"),
            bindingPathText("graph", graph.image.authoredIndex, "value"),
            graph.image.authoredIndex - 1, graph.explicitValue)) {
      return false;
    }
    if (graph.explicitValue || graph.type < 0) {
      continue;
    }
    if (graph.isRefNum) {
      SkinIntegerPropertyId id;
      if (!decodeRequiredBinding(
              request, decoder, value,
              {.kind = SkinBindingKind::IntegerProperty,
               .integerDomain = SkinIntegerPropertyDomain::IntegerValue},
              bindingPath("graph", graph.image.authoredIndex, "type"),
              bindingPathText("graph", graph.image.authoredIndex, "type"),
              graph.image.authoredIndex - 1, graph.type, id)) {
        return false;
      }
      graph.integerValue = id;
    } else {
      SkinFloatPropertyId id;
      if (!decodeRequiredBinding(
              request, decoder, value,
              {.kind = SkinBindingKind::FloatProperty,
               .floatDomain = SkinFloatPropertyDomain::Rate},
              bindingPath("graph", graph.image.authoredIndex, "type"),
              bindingPathText("graph", graph.image.authoredIndex, "type"),
              graph.image.authoredIndex - 1, graph.type, id)) {
        return false;
      }
      graph.implicitValue = id;
    }
  }

  if (request.note) {
    const std::size_t groupCount = request.note->group.size();
    if (!bindNoteLinePrefix(request, decoder, value, "group",
                            request.note->group, groupCount, builtins) ||
        !bindNoteLinePrefix(request, decoder, value, "bpm", request.note->bpm,
                            std::min(groupCount, request.note->bpm.size()),
                            builtins) ||
        !bindNoteLinePrefix(request, decoder, value, "stop", request.note->stop,
                            std::min(groupCount, request.note->stop.size()),
                            builtins) ||
        !bindNoteLinePrefix(request, decoder, value, "time", request.note->time,
                            std::min(groupCount, request.note->time.size()),
                            builtins)) {
      return false;
    }
  }

  for (auto &destination : request.rawDestinations) {
    if (!decodeOptionalBinding(
            request, decoder, value, {.kind = SkinBindingKind::TimerProperty},
            bindingPath("destination", destination.authoredIndex, "timer"),
            bindingPathText("destination", destination.authoredIndex, "timer"),
            destination.authoredIndex - 1, destination.timer)) {
      return false;
    }
    for (std::size_t index = 0; index < destination.conditions.size();
         ++index) {
      auto &condition = destination.conditions[index];
      // Keep only BooleanPropertyFactory misses as static skin options.
      if (condition.optionId &&
          !builtins.contains(
              {.kind = SkinBindingKind::BooleanProperty},
              SkinBuiltinPropertySelector{*condition.optionId})) {
        continue;
      }
      std::optional<SkinBooleanPropertyId> property;
      const auto oneBased = static_cast<std::uint32_t>(index + 1);
      if (!decodeOptionalBinding(
              request, decoder, value,
              {.kind = SkinBindingKind::BooleanProperty},
              bindingPath("destination", destination.authoredIndex, "op",
                          oneBased),
              bindingPathText("destination", destination.authoredIndex, "op",
                              oneBased),
              destination.authoredIndex - 1, property)) {
        return false;
      }
      condition.property = property;
    }
    if (!decodeOptionalBinding(
            request, decoder, value, {.kind = SkinBindingKind::BooleanProperty},
            bindingPath("destination", destination.authoredIndex, "draw"),
            bindingPathText("destination", destination.authoredIndex, "draw"),
            destination.authoredIndex - 1, destination.drawCondition)) {
      return false;
    }
  }
  const auto bindJudgeDestination = [&](RawDestination &destination,
                                        std::uint32_t judgeIndex,
                                        std::string_view childArray,
                                        std::uint32_t childIndex) {
    const auto path = [&](std::string_view field) {
      return LuaValuePath{LuaValuePathElement::field("judge"),
                          LuaValuePathElement::index(judgeIndex),
                          LuaValuePathElement::field(childArray),
                          LuaValuePathElement::index(childIndex),
                          LuaValuePathElement::field(field)};
    };
    const auto pathText = [&](std::string_view field) {
      return std::string("judge[") + std::to_string(judgeIndex) + "]." +
             std::string(childArray) + "[" + std::to_string(childIndex) + "]." +
             std::string(field);
    };
    if (!decodeOptionalBinding(request, decoder, value,
                               {.kind = SkinBindingKind::TimerProperty},
                               path("timer"), pathText("timer"), judgeIndex - 1,
                               destination.timer)) {
      return false;
    }
    for (std::size_t conditionIndex = 0;
         conditionIndex < destination.conditions.size(); ++conditionIndex) {
      auto &condition = destination.conditions[conditionIndex];
      if (condition.optionId &&
          !builtins.contains(
              {.kind = SkinBindingKind::BooleanProperty},
              SkinBuiltinPropertySelector{*condition.optionId})) {
        continue;
      }
      std::optional<SkinBooleanPropertyId> property;
      auto conditionPath = path("op");
      conditionPath.push_back(LuaValuePathElement::index(
          static_cast<std::uint32_t>(conditionIndex + 1)));
      if (!decodeOptionalBinding(request, decoder, value,
                                 {.kind = SkinBindingKind::BooleanProperty},
                                 std::move(conditionPath),
                                 pathText("op") + "[" +
                                     std::to_string(conditionIndex + 1) + "]",
                                 judgeIndex - 1, property)) {
        return false;
      }
      condition.property = property;
    }
    return decodeOptionalBinding(request, decoder, value,
                                 {.kind = SkinBindingKind::BooleanProperty},
                                 path("draw"), pathText("draw"), judgeIndex - 1,
                                 destination.drawCondition);
  };
  for (std::size_t judgeIndex = 0; judgeIndex < request.rawJudges.size();
       ++judgeIndex) {
    auto &judge = request.rawJudges[judgeIndex];
    for (std::size_t childIndex = 0; childIndex < judge.images.size();
         ++childIndex) {
      if (!bindJudgeDestination(judge.images[childIndex],
                                static_cast<std::uint32_t>(judgeIndex + 1),
                                "images",
                                static_cast<std::uint32_t>(childIndex + 1))) {
        return false;
      }
    }
    for (std::size_t childIndex = 0; childIndex < judge.numbers.size();
         ++childIndex) {
      if (!bindJudgeDestination(judge.numbers[childIndex],
                                static_cast<std::uint32_t>(judgeIndex + 1),
                                "numbers",
                                static_cast<std::uint32_t>(childIndex + 1))) {
        return false;
      }
    }
  }

  // Match JsonSkinLoader construction order. Runtime evaluation order remains
  // timers then events and is represented by the compatibility manifest.
  for (auto &customEvent : request.rawCustomEvents) {
    if (!decodeRequiredBinding(
            request, decoder, value, {.kind = SkinBindingKind::Event},
            bindingPath("customEvents", customEvent.authoredIndex, "action"),
            bindingPathText("customEvents", customEvent.authoredIndex,
                            "action"),
            customEvent.authoredIndex - 1, std::nullopt, customEvent.action) ||
        !decodeOptionalBinding(
            request, decoder, value, {.kind = SkinBindingKind::BooleanProperty},
            bindingPath("customEvents", customEvent.authoredIndex, "condition"),
            bindingPathText("customEvents", customEvent.authoredIndex,
                            "condition"),
            customEvent.authoredIndex - 1, customEvent.condition)) {
      return false;
    }
  }
  for (auto &customTimer : request.rawCustomTimers) {
    if (!decodeOptionalBinding(
            request, decoder, value, {.kind = SkinBindingKind::TimerProperty},
            bindingPath("customTimers", customTimer.authoredIndex, "timer"),
            bindingPathText("customTimers", customTimer.authoredIndex, "timer"),
            customTimer.authoredIndex - 1, customTimer.timer)) {
      return false;
    }
  }
  return true;
}

template <typename Raw, typename Name>
void moveFirstDefinitions(std::vector<Raw> &definitions,
                          std::map<std::string, Raw, std::less<>> &output,
                          Name name) {
  for (auto &definition : definitions) {
    const std::string id(name(definition));
    // JsonSkinObjectLoader resolves each authored category by a forward scan
    // and returns the first matching definition.  Duplicate definitions are
    // therefore legal; later entries in the same category are inert.
    output.try_emplace(id, std::move(definition));
  }
}

void transferBindings(BeatorajaSkinModel &model,
                      SkinBindingCatalogView bindings) {
  model.booleanProperties.assign(bindings.booleanProperties.begin(),
                                 bindings.booleanProperties.end());
  model.integerProperties.assign(bindings.integerProperties.begin(),
                                 bindings.integerProperties.end());
  model.floatProperties.assign(bindings.floatProperties.begin(),
                               bindings.floatProperties.end());
  model.stringProperties.assign(bindings.stringProperties.begin(),
                                bindings.stringProperties.end());
  model.timerProperties.assign(bindings.timerProperties.begin(),
                               bindings.timerProperties.end());
  model.floatWriters.assign(bindings.floatWriters.begin(),
                            bindings.floatWriters.end());
  model.stringWriters.assign(bindings.stringWriters.begin(),
                             bindings.stringWriters.end());
  model.events.assign(bindings.events.begin(), bindings.events.end());
}

bool materializeGameplay(GameplayDecodeRequest &request,
                         const LuaValueHandle &value,
                         LuaSkinGameplayDecodeContext context) {
  LuaSkinBindingDecoder decoder(context.runtime, context.builtins,
                                context.safetyPolicy);
  if (!bindGameplayDefinitions(request, decoder, value, context.builtins)) {
    return false;
  }

  moveFirstDefinitions(request.rawImages, request.images,
                       [](const RawSkinImage &image) -> const std::string & {
                         return image.id;
                       });
  moveFirstDefinitions(request.rawImageSets, request.imageSets,
                       [](const RawSkinImageSet &image) -> const std::string & {
                         return image.id;
                       });
  moveFirstDefinitions(request.rawNumbers, request.numbers,
                       [](const RawSkinNumber &number) -> const std::string & {
                         return number.image.id;
                       });
  moveFirstDefinitions(request.rawFloats, request.floats,
                       [](const RawSkinFloat &number) -> const std::string & {
                         return number.image.id;
                       });
  moveFirstDefinitions(request.rawSliders, request.sliders,
                       [](const RawSkinSlider &slider) -> const std::string & {
                         return slider.image.id;
                       });
  moveFirstDefinitions(
      request.rawTexts, request.texts,
      [](const RawSkinText &text) -> const std::string & { return text.id; });
  moveFirstDefinitions(request.rawGraphs, request.graphs,
                       [](const RawSkinGraph &graph) -> const std::string & {
                         return graph.image.id;
                       });
  moveFirstDefinitions(
      request.rawGaugeGraphs, request.gaugeGraphs,
      [](const RawSkinGaugeGraph &graph) -> const std::string & {
        return graph.id;
      });
  moveFirstDefinitions(
      request.rawBpmGraphs, request.bpmGraphs,
      [](const RawSkinBpmGraph &graph) -> const std::string & {
        return graph.id;
      });
  moveFirstDefinitions(
      request.rawHitErrorVisualizers, request.hitErrorVisualizers,
      [](const RawSkinHitErrorVisualizer &visualizer) -> const std::string & {
        return visualizer.id;
      });
  moveFirstDefinitions(
      request.rawJudgeGraphs, request.judgeGraphs,
      [](const RawSkinNoteDistributionGraph &graph) -> const std::string & {
        return graph.id;
      });
  moveFirstDefinitions(
      request.rawTimingVisualizers, request.timingVisualizers,
      [](const RawSkinTimingVisualizer &visualizer) -> const std::string & {
        return visualizer.id;
      });
  moveFirstDefinitions(
      request.rawTimingDistributionGraphs, request.timingDistributionGraphs,
      [](const RawSkinTimingDistributionGraph &graph) -> const std::string & {
        return graph.id;
      });
  moveFirstDefinitions(
      request.rawPmCharas, request.pmCharas,
      [](const RawSkinPmChara &identity) -> const std::string & {
        return identity.id;
      });
  moveFirstDefinitions(request.rawHiddenCovers, request.hiddenCovers,
                       [](const RawSkinCover &cover) -> const std::string & {
                         return cover.image.id;
                       });
  moveFirstDefinitions(request.rawLiftCovers, request.liftCovers,
                       [](const RawSkinCover &cover) -> const std::string & {
                         return cover.image.id;
                       });
  moveFirstDefinitions(request.rawJudges, request.judges,
                       [](const RawSkinJudge &judge) -> const std::string & {
                         return judge.id;
                       });

  if (request.note && !buildNoteObject(request, *request.note)) {
    transferDecodeDiagnostics(request);
    return false;
  }

  auto &model = *request.result.model;
  transferBindings(model, decoder.bindings());
  model.customEvents.reserve(request.rawCustomEvents.size());
  for (const auto &event : request.rawCustomEvents) {
    model.customEvents.push_back(
        {.id = event.id,
         .action = event.action,
         .condition = event.condition,
         .minimumIntervalMillis = event.minimumIntervalMillis});
  }
  model.customTimers.reserve(request.rawCustomTimers.size());
  for (const auto &timer : request.rawCustomTimers) {
    model.customTimers.push_back({.id = timer.id, .timer = timer.timer});
  }
  request.nextSyntheticObjectId =
      static_cast<SkinObjectId>(request.rawDestinations.size() + 1U);
  model.objects.reserve(request.rawDestinations.size());
  model.destinations.reserve(request.rawDestinations.size());
  for (std::size_t ordinal = 0; ordinal < request.rawDestinations.size();
       ++ordinal) {
    const auto &destination = request.rawDestinations[ordinal];
    SkinObjectPayload payload;
    bool critical = false;
    bool ignored = false;
    SkinDestinationBody presentation;
    if (!makeObjectPayload(request, destination.id, model, payload, critical,
                           ignored)) {
      transferDecodeDiagnostics(request);
      return false;
    }
    if (ignored) {
      continue;
    }
    if (!normalizeDestination(request, destination,
                              static_cast<std::uint32_t>(ordinal), presentation,
                              true)) {
      transferDecodeDiagnostics(request);
      return false;
    }
    if (const auto *cover = std::get_if<SkinCoverObject>(&payload)) {
      const auto normalized = normalizeSkinCover(
          {.kind = cover->kind,
           .sprite = cover->sprite,
           .authoredDisappearLine = cover->disappearLine,
           .authoredDisappearLineLinksLift = cover->disappearLineLinksLift,
           .lineScale = 1.0,
           .authoredDestinationOffsetIds = presentation.offsetIds});
      if (!normalized.cover) {
        fail(request.decoding, "skin_lua_model_cover_invalid",
             "Lua skin cover destination offsets cannot be normalized");
        transferDecodeDiagnostics(request);
        return false;
      }
      presentation.offsetIds = std::move(normalized.destinationOffsetIds);
    }
    const auto objectId = SkinObjectId{static_cast<std::uint32_t>(ordinal + 1)};
    model.objects.push_back(
        {.id = objectId,
         .authoredName = destination.id,
         .payload = std::move(payload),
         .authoredOrdinal = static_cast<std::uint32_t>(ordinal),
         .critical = critical});
    model.destinations.push_back(
        {.object = objectId, .presentation = std::move(presentation)});
  }
  return true;
}

} // namespace

HeaderDecodeResult
LuaSkinTableDecoder::decodeHeader(const LuaValueHandle &value) const {
  DecodeRequest request;
  if (auto failure =
          value.withValueProtected(&request, decodeHeaderProtected)) {
    request.result.header.reset();
    request.result.diagnostics.push_back(std::move(*failure));
  } else if (request.allocationFailed) {
    request.result.header.reset();
    request.result.diagnostics.push_back(diagnostic(
        "skin_lua_header_limit_exceeded",
        "Lua skin header could not be copied within host allocation limits"));
  }
  return std::move(request.result);
}

BeatorajaSkinModelDecodeResult LuaSkinTableDecoder::decodeGameplay(
    const LuaValueHandle &value, LuaSkinGameplayDecodeContext context) const {
  GameplayDecodeRequest request{
      .enforceGameplayLimits =
          safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)};
  if (auto failure =
          value.withValueProtected(&request, decodeGameplayProtected)) {
    request.result.model.reset();
    request.result.diagnostics.push_back(std::move(*failure));
  } else if (request.allocationFailed || request.decoding.allocationFailed) {
    request.result.model.reset();
    request.result.diagnostics.push_back(diagnostic(
        "skin_lua_model_limit_exceeded",
        "Lua skin gameplay model could not be copied within host limits"));
  } else if (request.result.model) {
    try {
      if (!materializeGameplay(request, value, context)) {
        request.result.model.reset();
      }
    } catch (...) {
      request.result.model.reset();
      request.result.diagnostics.push_back(
          diagnostic("skin_lua_model_limit_exceeded",
                     "Lua skin gameplay bindings could not be retained within "
                     "host limits"));
    }
  }
  return std::move(request.result);
}

std::string
skinConfigurationDigest(const BeatorajaSkinConfiguration &configuration) {
  return skinConfigurationDigest(EntryProfileSettings{
      .options = configuration.options,
      .filePaths = configuration.filePaths,
      .offsets = configuration.offsets,
  });
}

ConfigurationReconcileResult
reconcileSkinConfiguration(const BeatorajaSkinHeader &header,
                           const EntryProfileSettings *saved,
                           LuaSkinFileSystem &fileSystem,
                           const RuntimeSkinConfigurationSelection *pinned) {
  ConfigurationReconcileResult result;
  BeatorajaSkinConfiguration configuration;
  EntryProfileSettings settings;
  if (saved != nullptr) {
    settings.viewport = saved->viewport;
  }

  for (std::size_t optionIndex = 0; optionIndex < header.options.size();
       ++optionIndex) {
    const auto &option = header.options[optionIndex];
    if (option.name.empty()) {
      result.diagnostics.push_back(
          diagnostic("skin_lua_configuration_invalid",
                     "Lua skin option has an empty name"));
      return result;
    }
    // SkinHeader.CustomOption#getDefaultOption returns OPTION_RANDOM_VALUE
    // (-1) when a declaration has no choices. It is still exported to Lua;
    // persist the same effective sentinel so the activation configuration
    // digest exactly represents what the configured Lua state will receive.
    if (option.choices.empty()) {
      configuration.orderedOptions.push_back(
          {.name = option.name, .value = -1});
      configuration.options.insert_or_assign(option.name, -1);
      configuration.enabledOptionIds.insert(-1);
      settings.options.insert_or_assign(option.name, -1);
      continue;
    }
    const SkinHeaderOptionChoice *selected = &option.choices.front();
    int persistedValue = selected->value;
    for (const auto &choice : option.choices) {
      if (choice.label == option.defaultLabel) {
        selected = &choice;
        persistedValue = choice.value;
      }
    }
    if (saved != nullptr) {
      const auto desired = saved->options.find(option.name);
      if (desired != saved->options.end()) {
        // SkinHeader#setSkinConfigProperty stores OPTION_RANDOM_VALUE (-1)
        // but picks a fresh authored option for this configured execution.
        // Preserve -1 in profile/digest state while exporting that actual
        // selection through orderedOptions below.
        if (desired->second == -1) {
          const auto pinnedOption =
              pinned != nullptr && optionIndex < pinned->orderedOptions.size() &&
                      pinned->orderedOptions[optionIndex].name == option.name
                  ? std::find_if(
                        option.choices.begin(), option.choices.end(),
                        [&](const SkinHeaderOptionChoice &choice) {
                          return choice.value ==
                                 pinned->orderedOptions[optionIndex].value;
                        })
                  : option.choices.end();
          selected = pinnedOption != option.choices.end()
                         ? &*pinnedOption
                         : &option.choices[chooseRandomIndex(
                               option.choices.size())];
          persistedValue = -1;
        } else {
          for (const auto &choice : option.choices) {
            if (choice.value == desired->second) {
              selected = &choice;
              persistedValue = choice.value;
              break;
            }
          }
        }
      }
    }
    settings.options.insert_or_assign(option.name, persistedValue);
    configuration.orderedOptions.push_back(
        {.name = option.name, .value = selected->value});
    configuration.options.insert_or_assign(option.name, persistedValue);
    configuration.enabledOptionIds.insert(selected->value);
  }

  for (std::size_t fileIndex = 0; fileIndex < header.files.size(); ++fileIndex) {
    const auto &file = header.files[fileIndex];
    if (file.name.empty()) {
      result.diagnostics.push_back(
          diagnostic("skin_lua_configuration_invalid",
                     "Lua skin file declaration has an empty name"));
      return result;
    }

    std::vector<std::string> choices;
    bool directoryAvailable = false;
    if (validPattern(file.pattern)) {
      const std::size_t slash = file.pattern.rfind('/');
      const std::string directory =
          slash == std::string::npos ? "." : file.pattern.substr(0, slash);
      auto listed = fileSystem.listResourceDirectory(directory);
      if (!listed.failure) {
        directoryAvailable = true;
        for (const std::string &entry : listed.entries) {
          const std::string filename = filenameOf(entry);
          if (!matchesFilePattern(file.pattern, filename)) {
            continue;
          }
          const std::string candidate =
              substitutePattern(file.pattern, filename);
          const auto resolved =
              fileSystem.resolveResourceCandidates(candidate, candidate);
          if (resolved.normalizedVirtualPath) {
            choices.push_back(filename);
          }
        }
      }
    }
    std::sort(choices.begin(), choices.end());
    choices.erase(std::unique(choices.begin(), choices.end()), choices.end());

    std::vector<std::string> catalogChoices = choices;
    if (directoryAvailable) {
      // SkinConfiguration.updateCustomFiles adds Random after every file it
      // discovers, including a directory with no matching files.
      catalogChoices.push_back("Random");
    }

    std::optional<std::string> selected;
    // SkinHeader#setSkinConfigProperty accepts an existing FilePath by name
    // without requiring it to still match the declaration or exist on disk.
    // This also makes repeated CustomFile names share one persisted choice.
    if (saved != nullptr) {
      const auto desired = saved->filePaths.find(file.name);
      if (desired != saved->filePaths.end()) {
        selected = desired->second;
      }
    }
    if (!selected && !choices.empty()) {
      selected = choices.front();
      for (const std::string &choice : choices) {
        if (asciiCaseEqual(choice, file.defaultValue) ||
            asciiCaseEqual(stemOf(choice), file.defaultValue)) {
          selected = choice;
          break;
        }
      }
    } else if (!selected && directoryAvailable) {
      selected = "Random";
    }

    std::string runtimeSelection;
    if (selected) {
      if (*selected == "Random") {
        const auto pinnedFile =
            pinned != nullptr && fileIndex < pinned->orderedFiles.size() &&
                    pinned->orderedFiles[fileIndex].name == file.name &&
                    pinned->orderedFiles[fileIndex].pattern == file.pattern
                ? std::find(choices.begin(), choices.end(),
                            pinned->orderedFiles[fileIndex].selectedValue)
                : choices.end();
        runtimeSelection = pinnedFile != choices.end()
                               ? *pinnedFile
                               : chooseRandomFile(choices);
      } else {
        runtimeSelection = *selected;
      }
      settings.filePaths.insert_or_assign(file.name, *selected);
      configuration.filePaths.insert_or_assign(file.name, *selected);
    }
    configuration.orderedFiles.push_back(
        {.name = file.name,
         .pattern = file.pattern,
         .selectedValue = std::move(runtimeSelection),
         .choices = std::move(catalogChoices)});
  }

  for (const auto &offset : header.offsets) {
    if (offset.name.empty()) {
      result.diagnostics.push_back(
          diagnostic("skin_lua_configuration_invalid",
                     "Lua skin offset has an empty name"));
      return result;
    }
    ConfigOffset value;
    if (saved != nullptr) {
      const auto desired = saved->offsets.find(offset.name);
      if (desired != saved->offsets.end()) {
        value = desired->second;
      }
    }
    value = sanitizeOffset(value, offset.permissions);
    settings.offsets.insert_or_assign(offset.name, value);
    configuration.offsets.insert_or_assign(offset.name, value);
    configuration.offsetPermissions.insert_or_assign(offset.name,
                                                     offset.permissions);
    configuration.offsetsById.insert_or_assign(offset.id, value);
  }

  configuration.lowercaseSha256 = skinConfigurationDigest(configuration);
  result.reconciledSettings = std::move(settings);
  result.configuration = std::move(configuration);
  return result;
}

} // namespace skin

#endif
