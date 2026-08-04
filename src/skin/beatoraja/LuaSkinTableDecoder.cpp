#include "LuaSkinTableDecoder.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "../../FileChecksum.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinRuntime.h"
#include "../package/SkinPackageTypes.h"

extern "C" {
#include <lua.h>
}

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
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
  if (pattern.empty() ||
      pattern.size() > SkinProfileSettingsPolicy::maxConfigurationValueBytes ||
      pattern.front() == '/' || pattern.find('\\') != std::string_view::npos ||
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
    if (component.empty() || component == "." || component == "..") {
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
  if (depth > LuaSkinTableDecoderPolicy::maxDepth) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed depth limit");
  }
  if (!lua_istable(state, index)) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header object field is not a table");
  }
  ++request.objects;
  if (request.objects > LuaSkinTableDecoderPolicy::maxDecodedObjects) {
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
    if (request.entries > LuaSkinTableDecoderPolicy::maxEntries) {
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
                std::size_t maximumBytes, bool allowEmpty,
                DecodeRequest &request) {
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
  if (value == nullptr || size > maximumBytes ||
      request.copiedTextBytes >
          LuaSkinTableDecoderPolicy::maxCopiedTextBytes -
              std::min(size, LuaSkinTableDecoderPolicy::maxCopiedTextBytes)) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header text exceeds its fixed byte limit");
  }
  output.assign(value, size);
  auto normalized = normalizeNfc(output, allowEmpty);
  if (!normalized || normalized->size() > maximumBytes) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header text is not valid normalized UTF-8");
  }
  request.copiedTextBytes += normalized->size();
  output = std::move(*normalized);
  return true;
}

bool stringField(lua_State *state, int index, std::string_view name,
                 std::string &output, std::size_t maximumBytes, bool allowEmpty,
                 DecodeRequest &request) {
  if (!rawGetField(state, index, name, request)) {
    return false;
  }
  const bool ok =
      copyString(state, -1, output, maximumBytes, allowEmpty, request);
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

bool strictArrayLength(lua_State *state, int index, std::size_t maximum,
                       std::size_t &length, DecodeRequest &request) {
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
    if (request.entries > LuaSkinTableDecoderPolicy::maxEntries ||
        count > maximum || lua_type(state, -2) != LUA_TNUMBER) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin header array exceeds its limit or has mixed keys");
    }
    const double numeric = static_cast<double>(lua_tonumber(state, -2));
    if (!std::isfinite(numeric) || std::trunc(numeric) != numeric ||
        numeric < 1.0 || numeric > static_cast<double>(maximum)) {
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

bool decodeStringArray(lua_State *state, int index, std::size_t depth,
                       std::size_t maximum, std::vector<std::string> &output,
                       DecodeRequest &request) {
  if (depth > LuaSkinTableDecoderPolicy::maxDepth) {
    return fail(request, "skin_lua_header_limit_exceeded",
                "Lua skin header exceeds the fixed depth limit");
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
    if (!copyString(state, -1, output.back(),
                    SkinProfileSettingsPolicy::maxConfigurationKeyBytes, false,
                    request)) {
      return false;
    }
    lua_pop(state, 1);
  }
  return true;
}

bool decodeCategory(lua_State *state, int index, std::size_t depth,
                    SkinHeaderCategory &output, DecodeRequest &request) {
  if (!requireObject(state, index, depth, request) ||
      !stringField(state, index, "name", output.name,
                   SkinProfileSettingsPolicy::maxConfigurationKeyBytes, false,
                   request) ||
      !rawGetField(state, index, "item", request)) {
    return false;
  }
  bool ok = true;
  if (lua_istable(state, -1)) {
    ok = decodeStringArray(state, -1, depth + 1,
                           LuaSkinTableDecoderPolicy::maxCategoryItems,
                           output.items, request);
  }
  lua_pop(state, 1);
  return ok;
}

bool decodeChoice(lua_State *state, int index, std::size_t depth,
                  SkinHeaderOptionChoice &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "name", output.label,
                     SkinProfileSettingsPolicy::maxConfigurationKeyBytes, false,
                     request) &&
         integerField(state, index, "op", output.value, request);
}

bool decodeOption(lua_State *state, int index, std::size_t depth,
                  SkinHeaderOption &output, DecodeRequest &request) {
  if (!requireObject(state, index, depth, request) ||
      !stringField(state, index, "category", output.category,
                   SkinProfileSettingsPolicy::maxConfigurationKeyBytes, true,
                   request) ||
      !stringField(state, index, "name", output.name,
                   SkinProfileSettingsPolicy::maxConfigurationKeyBytes, false,
                   request) ||
      !stringField(state, index, "def", output.defaultLabel,
                   SkinProfileSettingsPolicy::maxConfigurationKeyBytes, true,
                   request) ||
      !rawGetField(state, index, "item", request)) {
    return false;
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_header_invalid",
                "Lua skin option has no choices");
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  std::size_t length = 0;
  if (!strictArrayLength(state, -1, LuaSkinTableDecoderPolicy::maxOptionChoices,
                         length, request) ||
      length == 0) {
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
         stringField(state, index, "category", output.category,
                     SkinProfileSettingsPolicy::maxConfigurationKeyBytes, true,
                     request) &&
         stringField(state, index, "name", output.name,
                     SkinProfileSettingsPolicy::maxConfigurationKeyBytes, false,
                     request) &&
         stringField(state, index, "path", output.pattern,
                     SkinProfileSettingsPolicy::maxConfigurationValueBytes,
                     false, request) &&
         stringField(state, index, "def", output.defaultValue,
                     SkinProfileSettingsPolicy::maxConfigurationValueBytes,
                     true, request);
}

bool decodeOffset(lua_State *state, int index, std::size_t depth,
                  SkinHeaderOffset &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "category", output.category,
                     SkinProfileSettingsPolicy::maxConfigurationKeyBytes, true,
                     request) &&
         stringField(state, index, "name", output.name,
                     SkinProfileSettingsPolicy::maxConfigurationKeyBytes, false,
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
                            std::size_t maximum, std::vector<Output> &output,
                            DecodeRequest &request,
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
  if (header.width < 1 ||
      header.width > LuaSkinTableDecoderPolicy::maxAuthoredDimension ||
      header.height < 1 ||
      header.height > LuaSkinTableDecoderPolicy::maxAuthoredDimension) {
    return fail(request, "skin_lua_header_invalid",
                "Lua skin header dimensions are outside the fixed range");
  }

  std::set<std::string> categoryNames;
  for (const auto &category : header.categories) {
    if (!categoryNames.insert(category.name).second) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin header contains duplicate category names");
    }
  }

  std::set<std::string> optionNames;
  std::set<int> optionIds;
  for (const auto &option : header.options) {
    if (option.choices.empty() || !optionNames.insert(option.name).second) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin header contains duplicate option names");
    }
    std::set<std::string> labels;
    std::set<int> localIds;
    for (const auto &choice : option.choices) {
      if (!labels.insert(choice.label).second ||
          !localIds.insert(choice.value).second ||
          !optionIds.insert(choice.value).second) {
        return fail(request, "skin_lua_header_invalid",
                    "Lua skin option labels or IDs are ambiguous");
      }
    }
  }

  std::set<std::string> fileNames;
  std::vector<std::string> patterns;
  for (const auto &file : header.files) {
    if (!fileNames.insert(file.name).second || !validPattern(file.pattern)) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin file declaration is invalid or duplicated");
    }
    for (const std::string &prior : patterns) {
      if (file.pattern.starts_with(prior) || prior.starts_with(file.pattern)) {
        return fail(request, "skin_lua_header_invalid",
                    "Lua skin file patterns overlap ambiguously");
      }
    }
    patterns.push_back(file.pattern);
  }

  std::set<std::string> offsetNames;
  std::set<int> offsetIds;
  for (const auto &offset : header.offsets) {
    if (!offsetNames.insert(offset.name).second ||
        !offsetIds.insert(offset.id).second) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin offset names or IDs are ambiguous");
    }
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
    if (offsetNames.find(offset.name) != offsetNames.end() ||
        offsetIds.find(offset.id) != offsetIds.end()) {
      return fail(request, "skin_lua_header_invalid",
                  "Lua skin offset collides with a synthesized control");
    }
    header.offsets.push_back(offset);
    offsetNames.insert(offset.name);
    offsetIds.insert(offset.id);
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
        !stringField(state, index, "name", header.name,
                     LuaSkinTableDecoderPolicy::maxHeaderTextBytes, true,
                     *request) ||
        !stringField(state, index, "author", header.author,
                     LuaSkinTableDecoderPolicy::maxHeaderTextBytes, true,
                     *request) ||
        !decodeObjectArrayField(state, index, "category", 1,
                                LuaSkinTableDecoderPolicy::maxCategories,
                                header.categories, *request, decodeCategory) ||
        !decodeObjectArrayField(state, index, "property", 1,
                                LuaSkinTableDecoderPolicy::maxOptions,
                                header.options, *request, decodeOption) ||
        !decodeObjectArrayField(state, index, "filepath", 1,
                                LuaSkinTableDecoderPolicy::maxFiles,
                                header.files, *request, decodeFile) ||
        !decodeObjectArrayField(state, index, "offset", 1,
                                LuaSkinTableDecoderPolicy::maxOffsets,
                                header.offsets, *request, decodeOffset) ||
        !validateSemantics(header, *request)) {
      request->result.header.reset();
    }
  } catch (...) {
    request->allocationFailed = true;
    request->result.header.reset();
  }
}

void appendU32(std::string &bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

void appendI32(std::string &bytes, int value) {
  appendU32(bytes,
            static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
}

void appendText(std::string &bytes, std::string_view value) {
  appendU32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.append(value);
}

ConfigOffset sanitizeOffset(ConfigOffset value,
                            OffsetPermissionMask permissions) {
  const auto clamp = [](int component) {
    return std::clamp(component, SkinProfileSettingsPolicy::minOffsetComponent,
                      SkinProfileSettingsPolicy::maxOffsetComponent);
  };
  value.x = (permissions & kOffsetPermissionX) != 0 ? clamp(value.x) : 0;
  value.y = (permissions & kOffsetPermissionY) != 0 ? clamp(value.y) : 0;
  value.w = (permissions & kOffsetPermissionW) != 0 ? clamp(value.w) : 0;
  value.h = (permissions & kOffsetPermissionH) != 0 ? clamp(value.h) : 0;
  value.r = (permissions & kOffsetPermissionR) != 0 ? clamp(value.r) : 0;
  value.a = (permissions & kOffsetPermissionA) != 0 ? clamp(value.a) : 0;
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

struct RawSkinSource {
  std::string id;
  std::string path;
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
  std::optional<int> timerSelector;
  int cycleMillis = 0;
  int stateCount = 0;
  int stateSelector = 0;
  SkinSpriteFrames sprite;
};

struct RawSkinImageSet {
  std::string id;
  int stateSelector = 0;
  std::vector<std::string> imageIds;
};

struct RawSkinNumber {
  RawSkinImage image;
  int digitCount = 0;
  int alignment = 0;
  int padding = 0;
  int zeroPadding = 0;
  int spacing = 0;
};

struct RawSkinFloat {
  RawSkinImage image;
  int integerDigits = 0;
  int fractionalDigits = 0;
  int alignment = 0;
  int zeroPadding = 0;
  int spacing = 0;
  double gain = 1.0;
  bool signVisible = false;
};

struct RawSkinSlider {
  RawSkinImage image;
  int direction = 0;
  int range = 0;
  int valueSelector = 0;
  bool changeable = true;
};

struct RawSkinNoteLaneRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct RawSkinNote {
  std::string id;
  std::map<SkinNoteVisualKind, std::vector<std::string>> visualIds;
  std::vector<std::string> hidden;
  std::vector<std::string> processed;
  std::vector<RawSkinNoteLaneRect> laneRects;
  std::vector<double> noteHeights;
  std::vector<double> expansionRate;
  std::optional<double> secondaryDestinationY;
  std::array<int, 2> expansionRatePercent{100, 100};
  bool authoredHiddenOrProcessed = false;
  SkinNoteObject object;
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
  std::optional<int> timerSelector;
  int loop = -1;
  int center = 0;
  int blend = 0;
  int filter = 0;
  int stretch = -1;
  std::vector<RawDestinationFrame> frames;
};

struct GameplayDecodeRequest {
  DecodeRequest decoding;
  BeatorajaSkinModelDecodeResult result;
  std::map<std::string, SkinResourceId, std::less<>> resourceIds;
  std::map<std::string, RawSkinImage, std::less<>> images;
  std::map<std::string, RawSkinImageSet, std::less<>> imageSets;
  std::map<std::string, RawSkinNumber, std::less<>> numbers;
  std::map<std::string, RawSkinFloat, std::less<>> floats;
  std::map<std::string, RawSkinSlider, std::less<>> sliders;
  std::vector<RawSkinSource> rawSources;
  std::vector<RawSkinImage> rawImages;
  std::vector<RawSkinImageSet> rawImageSets;
  std::vector<RawSkinNumber> rawNumbers;
  std::vector<RawSkinFloat> rawFloats;
  std::vector<RawSkinSlider> rawSliders;
  std::vector<RawDestination> rawDestinations;
  std::optional<RawSkinNote> note;
  std::size_t decodedFrames = 0;
  std::size_t materializedSpriteFrames = 0;
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

bool decodeRawSource(lua_State *state, int index, std::size_t depth,
                     RawSkinSource &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxHeaderTextBytes, false,
                     request) &&
         stringField(state, index, "path", output.path,
                     SkinProfileSettingsPolicy::maxConfigurationValueBytes,
                     false, request);
}

bool decodeRawImage(lua_State *state, int index, std::size_t depth,
                    RawSkinImage &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxHeaderTextBytes, false,
                     request) &&
         stringField(state, index, "src", output.source,
                     LuaSkinTableDecoderPolicy::maxHeaderTextBytes, false,
                     request) &&
         integerField(state, index, "x", output.x, request) &&
         integerField(state, index, "y", output.y, request) &&
         integerField(state, index, "w", output.width, request) &&
         integerField(state, index, "h", output.height, request) &&
         integerField(state, index, "divx", output.divisionsX, request) &&
         integerField(state, index, "divy", output.divisionsY, request) &&
         optionalIntegerField(state, index, "timer", output.timerSelector,
                              request) &&
         integerField(state, index, "cycle", output.cycleMillis, request) &&
         integerField(state, index, "len", output.stateCount, request) &&
         integerField(state, index, "ref", output.stateSelector, request);
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
  if (lua_type(state, -1) != LUA_TBOOLEAN) {
    lua_pop(state, 1);
    return fail(request, "skin_lua_model_invalid",
                "Lua skin Boolean field has an invalid type");
  }
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

bool decodeRawImageSet(lua_State *state, int index, std::size_t depth,
                       RawSkinImageSet &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxHeaderTextBytes, false,
                     request) &&
         integerField(state, index, "ref", output.stateSelector, request) &&
         stringArrayField(state, index, "images", output.imageIds, request);
}

bool decodeRawNumber(lua_State *state, int index, std::size_t depth,
                     RawSkinNumber &output, DecodeRequest &request) {
  return decodeRawImage(state, index, depth, output.image, request) &&
         integerField(state, index, "digit", output.digitCount, request) &&
         integerField(state, index, "align", output.alignment, request) &&
         integerField(state, index, "padding", output.padding, request) &&
         integerField(state, index, "zeropadding", output.zeroPadding,
                      request) &&
         integerField(state, index, "space", output.spacing, request);
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
                      request);
}

bool decodeRawSlider(lua_State *state, int index, std::size_t depth,
                     RawSkinSlider &output, DecodeRequest &request) {
  return decodeRawImage(state, index, depth, output.image, request) &&
         integerField(state, index, "angle", output.direction, request) &&
         integerField(state, index, "range", output.range, request) &&
         integerField(state, index, "type", output.valueSelector, request) &&
         booleanField(state, index, "changeable", output.changeable, request);
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

bool optionalNumberField(lua_State *state, int index, std::string_view name,
                         std::optional<double> &output,
                         DecodeRequest &request) {
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
                "Lua skin optional numeric field has an invalid type");
  }
  const double value = static_cast<double>(lua_tonumber(state, -1));
  lua_pop(state, 1);
  if (!std::isfinite(value)) {
    return fail(request, "skin_lua_model_invalid",
                "Lua skin optional numeric field is not finite");
  }
  output = value;
  return true;
}

bool decodeRawNote(lua_State *state, int index, std::size_t depth,
                   RawSkinNote &output, DecodeRequest &request) {
  if (!requireObject(state, index, depth, request) ||
      !stringField(state, index, "id", output.id,
                   LuaSkinTableDecoderPolicy::maxHeaderTextBytes, false,
                   request)) {
    return false;
  }

  constexpr std::array visualFields{
      std::pair{"note", SkinNoteVisualKind::Normal},
      std::pair{"mine", SkinNoteVisualKind::Mine},
      std::pair{"lnend", SkinNoteVisualKind::LnEnd},
      std::pair{"lnstart", SkinNoteVisualKind::LnStart},
      std::pair{"lnbodyActive", SkinNoteVisualKind::LnBodyActive},
      std::pair{"lnbody", SkinNoteVisualKind::LnBodyInactive},
      std::pair{"hcnend", SkinNoteVisualKind::HcnEnd},
      std::pair{"hcnstart", SkinNoteVisualKind::HcnStart},
      std::pair{"hcnbodyActive", SkinNoteVisualKind::HcnBodyActive},
      std::pair{"hcnbody", SkinNoteVisualKind::HcnBodyInactive},
      std::pair{"hcnbodyMiss", SkinNoteVisualKind::HcnDamage},
      std::pair{"hcnbodyReactive", SkinNoteVisualKind::HcnReactive},
  };
  for (const auto &[field, kind] : visualFields) {
    auto &ids = output.visualIds[kind];
    if (!stringArrayField(state, index, field, ids, request)) {
      return false;
    }
  }

  if (!stringArrayField(state, index, "hidden", output.hidden, request) ||
      !stringArrayField(state, index, "processed", output.processed, request) ||
      !decodeObjectArrayField(state, index, "dst", depth,
                              LuaSkinTableDecoderPolicy::maxDecodedObjects,
                              output.laneRects, request,
                              decodeRawNoteLaneRect) ||
      !numberArrayField(state, index, "size", output.noteHeights, request) ||
      !optionalNumberField(state, index, "dst2", output.secondaryDestinationY,
                           request)) {
    return false;
  }
  output.authoredHiddenOrProcessed =
      !output.hidden.empty() || !output.processed.empty();

  if (!numberArrayField(state, index, "expansionrate", output.expansionRate,
                        request)) {
    return false;
  }
  if (!output.expansionRate.empty()) {
    if (output.expansionRate.size() != 2 ||
        output.expansionRate[0] < std::numeric_limits<int>::min() ||
        output.expansionRate[0] > std::numeric_limits<int>::max() ||
        output.expansionRate[1] < std::numeric_limits<int>::min() ||
        output.expansionRate[1] > std::numeric_limits<int>::max()) {
      return fail(request, "skin_lua_model_invalid",
                  "Lua skin note expansion rate must contain two integers");
    }
    output.expansionRatePercent = {
        static_cast<int>(output.expansionRate[0]),
        static_cast<int>(output.expansionRate[1]),
    };
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

bool decodeRawDestination(lua_State *state, int index, std::size_t depth,
                          RawDestination &output, DecodeRequest &request) {
  return requireObject(state, index, depth, request) &&
         stringField(state, index, "id", output.id,
                     LuaSkinTableDecoderPolicy::maxHeaderTextBytes, false,
                     request) &&
         optionalIntegerField(state, index, "timer", output.timerSelector,
                              request) &&
         integerField(state, index, "loop", output.loop, request) &&
         integerField(state, index, "center", output.center, request) &&
         integerField(state, index, "blend", output.blend, request) &&
         integerField(state, index, "filter", output.filter, request) &&
         integerField(state, index, "stretch", output.stretch, request) &&
         decodeObjectArrayField(state, index, "dst", depth,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                output.frames, request,
                                decodeRawDestinationFrame);
}

bool safeResourcePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != path.npos ||
      (path.size() >= 2 && path[1] == ':')) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find('/', start);
    const std::string_view component =
        path.substr(start, (end == path.npos ? path.size() : end) - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == path.npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

const int *builtinIntegerSelector(
    const std::variant<SkinBuiltinPropertySelector, LuaCallbackId> &source) {
  const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(&source);
  return builtin != nullptr ? std::get_if<int>(&builtin->value) : nullptr;
}

SkinIntegerPropertyId internIntegerBinding(GameplayDecodeRequest &request,
                                           SkinIntegerPropertyDomain domain,
                                           int selector) {
  auto &bindings = request.result.model->integerProperties;
  for (const auto &binding : bindings) {
    const int *existing = builtinIntegerSelector(binding.source);
    if (binding.domain == domain && existing != nullptr &&
        *existing == selector) {
      return binding.id;
    }
  }
  const auto id =
      SkinIntegerPropertyId{static_cast<std::uint32_t>(bindings.size() + 1)};
  SkinBuiltinPropertySelector source;
  source.value = selector;
  bindings.push_back(
      {.id = id,
       .domain = domain,
       .source = std::move(source),
       .authoredOrdinal = static_cast<std::uint32_t>(bindings.size())});
  return id;
}

SkinFloatPropertyId internFloatBinding(GameplayDecodeRequest &request,
                                       SkinFloatPropertyDomain domain,
                                       int selector) {
  auto &bindings = request.result.model->floatProperties;
  for (const auto &binding : bindings) {
    const int *existing = builtinIntegerSelector(binding.source);
    if (binding.domain == domain && existing != nullptr &&
        *existing == selector) {
      return binding.id;
    }
  }
  const auto id =
      SkinFloatPropertyId{static_cast<std::uint32_t>(bindings.size() + 1)};
  SkinBuiltinPropertySelector source;
  source.value = selector;
  bindings.push_back(
      {.id = id,
       .domain = domain,
       .source = std::move(source),
       .authoredOrdinal = static_cast<std::uint32_t>(bindings.size())});
  return id;
}

SkinTimerPropertyId internTimerBinding(GameplayDecodeRequest &request,
                                       int selector) {
  auto &bindings = request.result.model->timerProperties;
  for (const auto &binding : bindings) {
    const int *existing = builtinIntegerSelector(binding.source);
    if (existing != nullptr && *existing == selector) {
      return binding.id;
    }
  }
  const auto id =
      SkinTimerPropertyId{static_cast<std::uint32_t>(bindings.size() + 1)};
  SkinBuiltinPropertySelector source;
  source.value = selector;
  bindings.push_back(
      {.id = id,
       .source = std::move(source),
       .authoredOrdinal = static_cast<std::uint32_t>(bindings.size())});
  return id;
}

bool expandImageFrames(GameplayDecodeRequest &request, RawSkinImage &image) {
  image.divisionsX = image.divisionsX > 0 ? image.divisionsX : 1;
  image.divisionsY = image.divisionsY > 0 ? image.divisionsY : 1;
  const auto divisionsX = static_cast<std::size_t>(image.divisionsX);
  const auto divisionsY = static_cast<std::size_t>(image.divisionsY);
  if (divisionsX > LuaSkinTableDecoderPolicy::maxEntries /
                       std::max<std::size_t>(divisionsY, 1)) {
    return fail(request.decoding, "skin_lua_model_limit_exceeded",
                "Lua skin image divisions exceed the fixed model limit");
  }
  const std::size_t frameCount = divisionsX * divisionsY;
  if (frameCount == 0 ||
      request.decodedFrames >
          LuaSkinTableDecoderPolicy::maxEntries -
              std::min(frameCount, LuaSkinTableDecoderPolicy::maxEntries)) {
    return fail(request.decoding, "skin_lua_model_limit_exceeded",
                "Lua skin image frames exceed the fixed model limit");
  }

  const auto resource = request.resourceIds.find(image.source);
  image.sprite.resource = resource != request.resourceIds.end()
                              ? resource->second
                              : SkinResourceId{0};
  image.sprite.cycleMillis = image.cycleMillis;
  if (image.timerSelector) {
    image.sprite.timer = internTimerBinding(request, *image.timerSelector);
  }
  image.sprite.frames.reserve(frameCount);
  const bool fullTextureWidth = image.width == -1;
  const bool fullTextureHeight = image.height == -1;
  const int cellWidth = fullTextureWidth ? -1 : image.width / image.divisionsX;
  const int cellHeight =
      fullTextureHeight ? -1 : image.height / image.divisionsY;
  for (int row = 0; row < image.divisionsY; ++row) {
    for (int column = 0; column < image.divisionsX; ++column) {
      const std::int64_t x =
          static_cast<std::int64_t>(image.x) +
          (fullTextureWidth ? 0
                            : static_cast<std::int64_t>(cellWidth) * column);
      const std::int64_t y =
          static_cast<std::int64_t>(image.y) +
          (fullTextureHeight ? 0 : static_cast<std::int64_t>(cellHeight) * row);
      if (x < std::numeric_limits<int>::min() ||
          x > std::numeric_limits<int>::max() ||
          y < std::numeric_limits<int>::min() ||
          y > std::numeric_limits<int>::max()) {
        return fail(request.decoding, "skin_lua_model_invalid",
                    "Lua skin image crop arithmetic overflows");
      }
      image.sprite.frames.push_back({.x = static_cast<int>(x),
                                     .y = static_cast<int>(y),
                                     .w = cellWidth,
                                     .h = cellHeight,
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
  if (count > LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames ||
      request.materializedSpriteFrames >
          LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames - count) {
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
  return image != request.images.end() ? &image->second.sprite : nullptr;
}

bool buildNoteObject(GameplayDecodeRequest &request, RawSkinNote &note) {
  note.object.expansionRatePercent = note.expansionRatePercent;
  const auto normal = note.visualIds.find(SkinNoteVisualKind::Normal);
  const std::size_t laneCount =
      normal != note.visualIds.end() ? normal->second.size() : 0;
  note.object.lanes.reserve(laneCount);
  for (std::size_t laneIndex = 0; laneIndex < laneCount; ++laneIndex) {
    SkinLaneNotePresentation lane;
    lane.authoredLane = static_cast<int>(laneIndex);
    if (laneIndex < note.laneRects.size()) {
      const auto &rect = note.laneRects[laneIndex];
      lane.laneDestination = {.x = static_cast<double>(rect.x),
                              .y = static_cast<double>(rect.y),
                              .width = static_cast<double>(rect.width),
                              .height = static_cast<double>(rect.height)};
    }
    if (laneIndex < note.noteHeights.size()) {
      lane.authoredNoteHeight = note.noteHeights[laneIndex];
    }
    lane.secondaryDestinationY = note.secondaryDestinationY;
    for (const auto &[kind, imageIds] : note.visualIds) {
      if (laneIndex < imageIds.size()) {
        const auto *sprite = noteSprite(request, imageIds[laneIndex]);
        const std::size_t frameCount =
            sprite != nullptr ? sprite->frames.size() : 0;
        if (!consumeMaterializedSpriteFrames(request, frameCount)) {
          return false;
        }
        lane.visuals.emplace(kind,
                             sprite != nullptr ? *sprite : SkinSpriteFrames{});
      }
    }
    lane.visuals.emplace(
        SkinNoteVisualKind::Hidden,
        SkinSynthesizedNoteVisual{.kind = SkinNoteVisualKind::Hidden});
    lane.visuals.emplace(
        SkinNoteVisualKind::Processed,
        SkinSynthesizedNoteVisual{.kind = SkinNoteVisualKind::Processed});
    note.object.lanes.push_back(std::move(lane));
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
    output.stateIndex =
        internIntegerBinding(request, SkinIntegerPropertyDomain::ImageIndex,
                             definition.stateSelector);
  }
  return true;
}

SkinZeroPaddingMode zeroPaddingMode(int value) {
  switch (value) {
  case 1:
    return SkinZeroPaddingMode::Zero;
  case 2:
    return SkinZeroPaddingMode::AlternateZero;
  default:
    return SkinZeroPaddingMode::None;
  }
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

bool makeObjectPayload(GameplayDecodeRequest &request, std::string_view name,
                       SkinObjectPayload &output, bool &critical) {
  std::size_t matches = 0;
  const auto image = request.images.find(name);
  const auto imageSet = request.imageSets.find(name);
  const auto number = request.numbers.find(name);
  const auto floating = request.floats.find(name);
  const auto slider = request.sliders.find(name);
  const bool isNote = request.note && request.note->id == name;
  matches += image != request.images.end();
  matches += imageSet != request.imageSets.end();
  matches += number != request.numbers.end();
  matches += floating != request.floats.end();
  matches += slider != request.sliders.end();
  matches += isNote;
  if (matches != 1) {
    return fail(request.decoding, "skin_lua_model_invalid",
                "Lua skin destination must resolve to exactly one supported "
                "object definition");
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
    object.stateIndex =
        internIntegerBinding(request, SkinIntegerPropertyDomain::ImageIndex,
                             imageSet->second.stateSelector);
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
  if (number != request.numbers.end()) {
    if (!consumeMaterializedSpriteFrames(
            request, number->second.image.sprite.frames.size())) {
      return false;
    }
    SkinNumberObject object;
    object.digits.positive = number->second.image.sprite;
    object.digits.glyphsPerAnimationFrame =
        static_cast<int>(object.digits.positive.frames.size());
    object.value =
        internIntegerBinding(request, SkinIntegerPropertyDomain::IntegerValue,
                             number->second.image.stateSelector);
    object.digitCount = number->second.digitCount;
    object.spacing = number->second.spacing;
    object.alignment = number->second.alignment;
    object.zeroPadding = zeroPaddingMode(number->second.zeroPadding != 0
                                             ? number->second.zeroPadding
                                             : number->second.padding);
    output = std::move(object);
    return true;
  }
  if (floating != request.floats.end()) {
    if (!consumeMaterializedSpriteFrames(
            request, floating->second.image.sprite.frames.size())) {
      return false;
    }
    SkinFloatObject object;
    object.digits.positive = floating->second.image.sprite;
    object.digits.glyphsPerAnimationFrame =
        static_cast<int>(object.digits.positive.frames.size());
    object.value =
        internFloatBinding(request, SkinFloatPropertyDomain::FloatValue,
                           floating->second.image.stateSelector);
    object.integerDigits = floating->second.integerDigits;
    object.fractionalDigits = floating->second.fractionalDigits;
    object.spacing = floating->second.spacing;
    object.alignment = floating->second.alignment;
    object.zeroPadding = zeroPaddingMode(floating->second.zeroPadding);
    object.signVisible = floating->second.signVisible;
    object.gain = floating->second.gain;
    output = std::move(object);
    return true;
  }
  if (slider != request.sliders.end()) {
    if (slider->second.direction < 0 || slider->second.direction > 255) {
      return fail(request.decoding, "skin_lua_model_invalid",
                  "Lua skin slider direction is outside byte range");
    }
    if (!consumeMaterializedSpriteFrames(
            request, slider->second.image.sprite.frames.size())) {
      return false;
    }
    SkinSliderObject object;
    object.knob = slider->second.image.sprite;
    object.value = internFloatBinding(request, SkinFloatPropertyDomain::Rate,
                                      slider->second.valueSelector);
    object.direction = static_cast<std::uint8_t>(slider->second.direction);
    object.range = static_cast<double>(slider->second.range);
    object.changeable = slider->second.changeable;
    output = std::move(object);
    return true;
  }

  critical = true;
  output = std::move(request.note->object);
  return true;
}

bool normalizeDestination(GameplayDecodeRequest &request,
                          const RawDestination &raw,
                          std::uint32_t authoredOrdinal,
                          SkinDestinationBody &output) {
  const auto mappedBlend = blendMode(raw.blend);
  if (!mappedBlend || raw.filter < 0 || raw.filter > 1 || raw.stretch < -1 ||
      raw.stretch > 10) {
    return fail(request.decoding, "skin_lua_model_invalid",
                "Lua skin destination presentation mode is unsupported");
  }
  output.loop = raw.loop;
  output.center = raw.center;
  output.blend = *mappedBlend;
  output.filter = static_cast<SkinFilterMode>(raw.filter);
  if (raw.stretch >= 0) {
    output.stretch = static_cast<SkinStretchMode>(raw.stretch);
  }
  output.authoredOrdinal = authoredOrdinal;
  if (raw.timerSelector) {
    output.timer = internTimerBinding(request, *raw.timerSelector);
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
  std::stable_sort(output.frames.begin(), output.frames.end(),
                   [](const auto &left, const auto &right) {
                     return left.timeMillis < right.timeMillis;
                   });
  return true;
}

void transferDecodeDiagnostics(GameplayDecodeRequest &request) {
  auto &source = request.decoding.result.diagnostics;
  request.result.diagnostics.insert(request.result.diagnostics.end(),
                                    std::make_move_iterator(source.begin()),
                                    std::make_move_iterator(source.end()));
  source.clear();
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

    request->result.model.emplace();
    auto &model = *request->result.model;
    model.header = std::move(*request->decoding.result.header);
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
      if (!safeResourcePath(source.path) ||
          request->resourceIds.contains(source.id)) {
        fail(request->decoding, "skin_lua_model_invalid",
             "Lua skin sources must have unique IDs and safe relative paths");
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
      const auto id = SkinResourceId{static_cast<std::uint32_t>(ordinal + 1)};
      request->resourceIds.emplace(source.id, id);
      model.resources.emplace_back(SkinImageResource{
          .id = id,
          .authoredName = source.id,
          .virtualPath = source.path,
          .authoredOrdinal = static_cast<std::uint32_t>(ordinal),
      });
    }

    if (!decodeObjectArrayField(state, index, "image", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawImages, request->decoding,
                                decodeRawImage)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (auto &image : request->rawImages) {
      const std::string id = image.id;
      if (request->images.contains(image.id) ||
          !expandImageFrames(*request, image)) {
        if (request->decoding.result.diagnostics.empty()) {
          fail(request->decoding, "skin_lua_model_invalid",
               "Lua skin image IDs must be unique");
        }
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
      request->images.emplace(id, std::move(image));
    }

    if (!decodeObjectArrayField(state, index, "imageset", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawImageSets, request->decoding,
                                decodeRawImageSet)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (auto &imageSet : request->rawImageSets) {
      const std::string id = imageSet.id;
      if (!request->imageSets.emplace(id, std::move(imageSet)).second) {
        fail(request->decoding, "skin_lua_model_invalid",
             "Lua skin ImageSet IDs must be unique");
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

    if (!decodeObjectArrayField(state, index, "value", 1,
                                LuaSkinTableDecoderPolicy::maxDecodedObjects,
                                request->rawNumbers, request->decoding,
                                decodeRawNumber)) {
      transferDecodeDiagnostics(*request);
      request->result.model.reset();
      return;
    }
    for (auto &number : request->rawNumbers) {
      const std::string id = number.image.id;
      if (!expandImageFrames(*request, number.image) ||
          !request->numbers.emplace(id, std::move(number)).second) {
        if (request->decoding.result.diagnostics.empty()) {
          fail(request->decoding, "skin_lua_model_invalid",
               "Lua skin Value IDs must be unique");
        }
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
    for (auto &number : request->rawFloats) {
      const std::string id = number.image.id;
      if (!expandImageFrames(*request, number.image) ||
          !request->floats.emplace(id, std::move(number)).second) {
        if (request->decoding.result.diagnostics.empty()) {
          fail(request->decoding, "skin_lua_model_invalid",
               "Lua skin FloatValue IDs must be unique");
        }
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
    for (auto &slider : request->rawSliders) {
      const std::string id = slider.image.id;
      if (!expandImageFrames(*request, slider.image) ||
          !request->sliders.emplace(id, std::move(slider)).second) {
        if (request->decoding.result.diagnostics.empty()) {
          fail(request->decoding, "skin_lua_model_invalid",
               "Lua skin Slider IDs must be unique");
        }
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
    }

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
      if (!buildNoteObject(*request, *request->note)) {
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
    std::set<std::string> destinationIds;
    model.objects.reserve(request->rawDestinations.size());
    model.destinations.reserve(request->rawDestinations.size());
    for (std::size_t ordinal = 0; ordinal < request->rawDestinations.size();
         ++ordinal) {
      const auto &destination = request->rawDestinations[ordinal];
      if (!destinationIds.insert(destination.id).second) {
        fail(request->decoding, "skin_lua_model_invalid",
             "Lua skin destination IDs must be unique");
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }

      SkinObjectPayload payload;
      bool critical = false;
      SkinDestinationBody presentation;
      if (!makeObjectPayload(*request, destination.id, payload, critical) ||
          !normalizeDestination(*request, destination,
                                static_cast<std::uint32_t>(ordinal),
                                presentation)) {
        transferDecodeDiagnostics(*request);
        request->result.model.reset();
        return;
      }
      const auto objectId =
          SkinObjectId{static_cast<std::uint32_t>(ordinal + 1)};
      model.objects.push_back(
          {.id = objectId,
           .authoredName = destination.id,
           .payload = std::move(payload),
           .authoredOrdinal = static_cast<std::uint32_t>(ordinal),
           .critical = critical});
      model.destinations.push_back(
          {.object = objectId, .presentation = std::move(presentation)});
    }
  } catch (...) {
    request->allocationFailed = true;
    request->result.model.reset();
  }
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

BeatorajaSkinModelDecodeResult
LuaSkinTableDecoder::decodeGameplay(const LuaValueHandle &value) const {
  GameplayDecodeRequest request;
  if (auto failure =
          value.withValueProtected(&request, decodeGameplayProtected)) {
    request.result.model.reset();
    request.result.diagnostics.push_back(std::move(*failure));
  } else if (request.allocationFailed || request.decoding.allocationFailed) {
    request.result.model.reset();
    request.result.diagnostics.push_back(diagnostic(
        "skin_lua_model_limit_exceeded",
        "Lua skin gameplay model could not be copied within host limits"));
  }
  return std::move(request.result);
}

std::string
skinConfigurationDigest(const BeatorajaSkinConfiguration &configuration) {
  std::string framed("ASOBMSKIN-CONFIG-V1", 19);
  framed.push_back('\0');

  framed.push_back(static_cast<char>(0x01));
  appendU32(framed, static_cast<std::uint32_t>(configuration.options.size()));
  for (const auto &[key, value] : configuration.options) {
    appendText(framed, key);
    appendI32(framed, value);
  }

  framed.push_back(static_cast<char>(0x02));
  appendU32(framed, static_cast<std::uint32_t>(configuration.filePaths.size()));
  for (const auto &[key, value] : configuration.filePaths) {
    appendText(framed, key);
    appendText(framed, value);
  }

  framed.push_back(static_cast<char>(0x03));
  appendU32(framed, static_cast<std::uint32_t>(configuration.offsets.size()));
  for (const auto &[key, value] : configuration.offsets) {
    appendText(framed, key);
    appendI32(framed, value.x);
    appendI32(framed, value.y);
    appendI32(framed, value.w);
    appendI32(framed, value.h);
    appendI32(framed, value.r);
    appendI32(framed, value.a);
  }
  return file_checksum::sha256(framed);
}

ConfigurationReconcileResult
reconcileSkinConfiguration(const BeatorajaSkinHeader &header,
                           const EntryProfileSettings *saved,
                           LuaSkinFileSystem &fileSystem) {
  ConfigurationReconcileResult result;
  if (header.options.size() > LuaSkinTableDecoderPolicy::maxOptions ||
      header.files.size() > LuaSkinTableDecoderPolicy::maxFiles ||
      header.offsets.size() > LuaSkinTableDecoderPolicy::maxOffsets + 4) {
    result.diagnostics.push_back(
        diagnostic("skin_lua_configuration_invalid",
                   "Lua skin header cannot be reconciled within fixed limits"));
    return result;
  }

  BeatorajaSkinConfiguration configuration;
  EntryProfileSettings settings;
  if (saved != nullptr) {
    settings.viewport = saved->viewport;
  }

  std::set<std::string> optionNames;
  std::set<int> optionIds;
  for (const auto &option : header.options) {
    if (option.name.empty() || !optionNames.insert(option.name).second ||
        option.choices.empty()) {
      result.diagnostics.push_back(
          diagnostic("skin_lua_configuration_invalid",
                     "Lua skin options are ambiguous or empty"));
      return result;
    }
    const SkinHeaderOptionChoice *selected = &option.choices.front();
    for (const auto &choice : option.choices) {
      if (!optionIds.insert(choice.value).second) {
        result.diagnostics.push_back(
            diagnostic("skin_lua_configuration_invalid",
                       "Lua skin option IDs are ambiguous"));
        return result;
      }
      if (choice.label == option.defaultLabel) {
        selected = &choice;
      }
    }
    if (saved != nullptr) {
      const auto desired = saved->options.find(option.name);
      if (desired != saved->options.end()) {
        for (const auto &choice : option.choices) {
          if (choice.value == desired->second) {
            selected = &choice;
            break;
          }
        }
      }
    }
    settings.options.emplace(option.name, selected->value);
    configuration.orderedOptions.push_back(
        {.name = option.name, .value = selected->value});
    configuration.options.emplace(option.name, selected->value);
    configuration.enabledOptionIds.insert(selected->value);
  }

  std::set<std::string> fileNames;
  std::vector<std::string> patterns;
  for (const auto &file : header.files) {
    if (file.name.empty() || !fileNames.insert(file.name).second ||
        !validPattern(file.pattern)) {
      result.diagnostics.push_back(
          diagnostic("skin_lua_configuration_invalid",
                     "Lua skin file declaration is invalid or duplicated"));
      return result;
    }
    for (const std::string &prior : patterns) {
      if (file.pattern.starts_with(prior) || prior.starts_with(file.pattern)) {
        result.diagnostics.push_back(
            diagnostic("skin_lua_configuration_invalid",
                       "Lua skin file patterns overlap ambiguously"));
        return result;
      }
    }
    patterns.push_back(file.pattern);

    const std::size_t slash = file.pattern.rfind('/');
    const std::string directory =
        slash == std::string::npos ? "." : file.pattern.substr(0, slash);
    auto listed = fileSystem.list(
        directory, "", static_cast<std::size_t>(SkinPackagePolicy::maxFiles));
    if (listed.failure) {
      result.diagnostics.push_back(
          diagnostic("skin_lua_configuration_invalid",
                     "Lua skin file choices cannot be enumerated",
                     listed.failure->virtualPath));
      return result;
    }
    std::vector<std::string> choices;
    for (const std::string &entry : listed.entries) {
      const std::string filename = filenameOf(entry);
      if (!matchesFilePattern(file.pattern, filename)) {
        continue;
      }
      const std::string candidate = substitutePattern(file.pattern, filename);
      const auto resolved =
          fileSystem.resolve(candidate, SkinFileUse::Resource);
      if (resolved.normalizedVirtualPath) {
        choices.push_back(filename);
      }
    }
    std::sort(choices.begin(), choices.end());
    choices.erase(std::unique(choices.begin(), choices.end()), choices.end());
    if (choices.empty() ||
        choices.size() > LuaSkinTableDecoderPolicy::maxOptionChoices) {
      result.diagnostics.push_back(diagnostic(
          "skin_lua_configuration_invalid",
          "Lua skin file declaration has no bounded deterministic choice"));
      return result;
    }

    std::string selected = choices.front();
    for (const std::string &choice : choices) {
      if (asciiCaseEqual(choice, file.defaultValue) ||
          asciiCaseEqual(stemOf(choice), file.defaultValue)) {
        selected = choice;
        break;
      }
    }
    if (saved != nullptr) {
      const auto desired = saved->filePaths.find(file.name);
      if (desired != saved->filePaths.end() &&
          std::find(choices.begin(), choices.end(), desired->second) !=
              choices.end()) {
        selected = desired->second;
      }
    }
    settings.filePaths.emplace(file.name, selected);
    configuration.orderedFiles.push_back({.name = file.name,
                                          .pattern = file.pattern,
                                          .selectedValue = selected});
    configuration.filePaths.emplace(file.name, selected);
  }

  std::set<std::string> offsetNames;
  std::set<int> offsetIds;
  for (const auto &offset : header.offsets) {
    if (offset.name.empty() || !offsetNames.insert(offset.name).second ||
        !offsetIds.insert(offset.id).second) {
      result.diagnostics.push_back(
          diagnostic("skin_lua_configuration_invalid",
                     "Lua skin offset names or IDs are ambiguous"));
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
    settings.offsets.emplace(offset.name, value);
    configuration.offsets.emplace(offset.name, value);
    configuration.offsetPermissions.emplace(offset.name, offset.permissions);
    configuration.offsetsById.emplace(offset.id, value);
  }

  configuration.lowercaseSha256 = skinConfigurationDigest(configuration);
  result.reconciledSettings = std::move(settings);
  result.configuration = std::move(configuration);
  return result;
}

} // namespace skin

#endif
