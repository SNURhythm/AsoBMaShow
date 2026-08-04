#include "LuaSkinTableDecoder.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "../../FileChecksum.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinRuntime.h"

extern "C" {
#include <lua.h>
}

#include <utf8proc.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
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
      pattern.find('/', star) != std::string_view::npos) {
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
    return true;
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
  const std::size_t slash = pattern.rfind('/');
  const std::string_view component =
      slash == std::string_view::npos ? pattern : pattern.substr(slash + 1);
  const std::size_t star = component.find('*');
  const std::string_view prefix = component.substr(0, star);
  const std::string_view suffix = component.substr(star + 1);
  return filename.size() >= prefix.size() + suffix.size() &&
         filename.starts_with(prefix) && filename.ends_with(suffix);
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
    if (!optionNames.insert(option.name).second || option.choices.empty()) {
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
    if (!fileNames.insert(file.name).second || !validPattern(file.pattern)) {
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
        directory, "", LuaSkinTableDecoderPolicy::maxOptionChoices + 1);
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
    if (!offsetNames.insert(offset.name).second ||
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
