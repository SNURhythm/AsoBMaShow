#include "JsonGameplaySkinDecoder.h"

#include "../GameplaySkinTraits.h"
#include "NumericGlyphAtlas.h"
#include "SkinCoverNormalization.h"
#include "SkinGaugeNodeExpansion.h"
#include "SkinNoteNormalization.h"
#include "SkinObjectResolutionPrecedence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace skin {
namespace {

using Json = nlohmann::json;
constexpr int kJavaIntegerMinimum = std::numeric_limits<int>::min();

class JsonSourceIndex;

SkinDiagnostic makeDiagnostic(std::string code, std::string message,
                              const SkinEntryId &entry,
                              DiagnosticSeverity severity =
                                  DiagnosticSeverity::Error,
                              std::optional<SkinSourceLocation> source = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = entry.packageRelativePath,
          .severity = severity,
          .source = std::move(source)};
}

struct DecodeContext {
  const SkinEntryId &entry;
  SkinBuiltinBindingCatalogView builtins;
  SkinSafetyPolicy safetyPolicy;
  JsonGameplaySkinDecodeResult &result;
  const JsonSourceIndex *sources = nullptr;
  bool failed = false;

  [[nodiscard]] std::optional<SkinSourceLocation>
  source(const Json *value) const;

  void error(std::string code, std::string message,
             std::optional<SkinSourceLocation> source = {}) {
    result.diagnostics.push_back(makeDiagnostic(
        std::move(code), std::move(message), entry,
        DiagnosticSeverity::Error, std::move(source)));
    failed = true;
  }

  void warning(std::string code, std::string message,
               std::optional<SkinSourceLocation> source = {}) {
    result.diagnostics.push_back(makeDiagnostic(
        std::move(code), std::move(message), entry,
        DiagnosticSeverity::Warning, std::move(source)));
  }
};

const Json *member(const Json &object, std::string_view name) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto found = object.find(std::string(name));
  return found == object.end() ? nullptr : &*found;
}

bool parseIntegerString(std::string_view text, int &output) {
  if (text.empty()) {
    return false;
  }
  const char *first = text.data();
  const char *last = first + text.size();
  if (*first == '+') {
    ++first;
    if (first == last) {
      return false;
    }
  }
  const auto [end, error] = std::from_chars(first, last, output);
  return error == std::errc{} && end == last;
}

std::optional<int> integerValue(const Json &value) {
  if (value.is_number_integer()) {
    const auto raw = value.get<std::int64_t>();
    if (raw >= std::numeric_limits<int>::min() &&
        raw <= std::numeric_limits<int>::max()) {
      return static_cast<int>(raw);
    }
    return std::nullopt;
  }
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    return raw <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())
               ? std::optional<int>(static_cast<int>(raw))
               : std::nullopt;
  }
  if (value.is_number_float()) {
    const double raw = value.get<double>();
    if (std::isfinite(raw) &&
        raw >= static_cast<double>(std::numeric_limits<int>::min()) &&
        raw <= static_cast<double>(std::numeric_limits<int>::max())) {
      return static_cast<int>(raw);
    }
    return std::nullopt;
  }
  if (value.is_string()) {
    int parsed = 0;
    if (parseIntegerString(value.get_ref<const std::string &>(), parsed)) {
      return parsed;
    }
  }
  return std::nullopt;
}

int integerField(const Json &object, std::string_view name, int fallback,
                 DecodeContext &context, std::string_view surface) {
  const Json *value = member(object, name);
  if (value == nullptr || value->is_null()) {
    return fallback;
  }
  const auto decoded = integerValue(*value);
  if (!decoded) {
    context.error("skin_json_model_invalid",
                  std::string(surface) + "." + std::string(name) +
                      " is not a bounded integer",
                  context.source(value));
    return fallback;
  }
  return *decoded;
}

std::optional<int> inheritedIntegerField(const Json &object,
                                         std::string_view name,
                                         DecodeContext &context,
                                         std::string_view surface) {
  const Json *value = member(object, name);
  if (value == nullptr || value->is_null()) {
    return std::nullopt;
  }
  const auto decoded = integerValue(*value);
  if (!decoded) {
    context.error("skin_json_model_invalid",
                  std::string(surface) + "." + std::string(name) +
                      " is not a bounded integer",
                  context.source(value));
    return std::nullopt;
  }
  return *decoded == kJavaIntegerMinimum ? std::nullopt : decoded;
}

double numberField(const Json &object, std::string_view name, double fallback,
                   DecodeContext &context, std::string_view surface) {
  const Json *value = member(object, name);
  if (value == nullptr || value->is_null()) {
    return fallback;
  }
  if (value->is_number()) {
    const double decoded = value->get<double>();
    if (std::isfinite(decoded)) {
      return decoded;
    }
  }
  context.error("skin_json_model_invalid",
                std::string(surface) + "." + std::string(name) +
                    " is not a finite number",
                context.source(value));
  return fallback;
}

bool booleanField(const Json &object, std::string_view name, bool fallback,
                  DecodeContext &context, std::string_view surface) {
  const Json *value = member(object, name);
  if (value == nullptr || value->is_null()) {
    return fallback;
  }
  if (value->is_boolean()) {
    return value->get<bool>();
  }
  if (value->is_number()) {
    return value->get<double>() != 0.0;
  }
  if (value->is_string()) {
    const auto &text = value->get_ref<const std::string &>();
    if (text == "true") {
      return true;
    }
    if (text == "false") {
      return false;
    }
  }
  context.error("skin_json_model_invalid",
                std::string(surface) + "." + std::string(name) +
                    " is not a boolean",
                context.source(value));
  return fallback;
}

std::string stringField(const Json &object, std::string_view name,
                        std::string fallback, DecodeContext &context,
                        std::string_view surface) {
  const Json *value = member(object, name);
  if (value == nullptr || value->is_null()) {
    return fallback;
  }
  if (value->is_string()) {
    return value->get<std::string>();
  }
  context.error("skin_json_model_invalid",
                std::string(surface) + "." + std::string(name) +
                    " is not a string",
                context.source(value));
  return fallback;
}

std::vector<std::string> stringArrayField(const Json &object,
                                          std::string_view name,
                                          DecodeContext &context,
                                          std::string_view surface) {
  std::vector<std::string> result;
  const Json *value = member(object, name);
  if (value == nullptr || value->is_null()) {
    return result;
  }
  if (!value->is_array()) {
    context.error("skin_json_model_invalid",
                  std::string(surface) + "." + std::string(name) +
                      " is not an array",
                  context.source(value));
    return result;
  }
  if (value->size() > JsonGameplaySkinDecoderPolicy::maxArrayEntries) {
    context.error("skin_json_limit_exceeded",
                  std::string(surface) + "." + std::string(name) +
                      " exceeds the fixed array limit",
                  context.source(value));
    return result;
  }
  result.reserve(value->size());
  for (const auto &item : *value) {
    if (!item.is_string()) {
      context.error("skin_json_model_invalid",
                    std::string(surface) + "." + std::string(name) +
                        " contains a non-string",
                    context.source(&item));
      continue;
    }
    result.push_back(item.get<std::string>());
  }
  return result;
}

std::vector<int> integerArrayField(const Json &object, std::string_view name,
                                   DecodeContext &context,
                                   std::string_view surface) {
  std::vector<int> result;
  const Json *value = member(object, name);
  if (value == nullptr || value->is_null()) {
    return result;
  }
  if (!value->is_array() ||
      value->size() > JsonGameplaySkinDecoderPolicy::maxArrayEntries) {
    context.error("skin_json_model_invalid",
                  std::string(surface) + "." + std::string(name) +
                      " is not a bounded integer array",
                  context.source(value));
    return result;
  }
  result.reserve(value->size());
  for (const auto &item : *value) {
    if (const auto decoded = integerValue(item)) {
      result.push_back(*decoded);
    } else {
      context.error("skin_json_model_invalid",
                    std::string(surface) + "." + std::string(name) +
                        " contains a non-integer",
                    context.source(&item));
    }
  }
  return result;
}

void checkFields(const Json &object,
                 std::initializer_list<std::string_view> allowed,
                 std::string_view surface, DecodeContext &context) {
  if (!object.is_object()) {
    context.error("skin_json_model_invalid",
                  std::string(surface) + " is not an object",
                  context.source(&object));
    return;
  }
  for (auto field = object.begin(); field != object.end(); ++field) {
    if (std::ranges::find(allowed, std::string_view(field.key())) ==
        allowed.end()) {
      context.warning("skin_json_field_unclassified",
                      std::string(surface) + "." + field.key() +
                          " is not a pinned JsonSkin field",
                      context.source(&field.value()));
    }
  }
}

template <typename Visitor>
void visitObjectArray(const Json &root, std::string_view name,
                      DecodeContext &context, Visitor visitor) {
  const Json *array = member(root, name);
  if (array == nullptr || array->is_null()) {
    return;
  }
  if (!array->is_array()) {
    context.error("skin_json_model_invalid",
                  std::string("JsonSkin.Skin.") + std::string(name) +
                      " is not an array",
                  context.source(array));
    return;
  }
  if (array->size() > JsonGameplaySkinDecoderPolicy::maxArrayEntries) {
    context.error("skin_json_limit_exceeded",
                  std::string("JsonSkin.Skin.") + std::string(name) +
                      " exceeds the fixed array limit",
                  context.source(array));
    return;
  }
  for (std::size_t index = 0; index < array->size(); ++index) {
    visitor((*array)[index], index);
  }
}

void classifyDestination(const Json &destination, DecodeContext &context,
                         std::string_view surface) {
  checkFields(destination,
              {"id", "blend", "filter", "timer", "loop", "center",
               "offset", "offsets", "stretch", "op", "draw", "dst",
               "mouseRect"},
              surface, context);
  if (const Json *frames = member(destination, "dst");
      frames != nullptr && frames->is_array()) {
    for (const auto &frame : *frames) {
      checkFields(frame,
                  {"time", "x", "y", "w", "h", "clip_x", "clip_y",
                   "clip_w", "clip_h", "acc", "a", "r", "g", "b",
                   "angle"},
                  "JsonSkin.Animation", context);
    }
  }
  if (const Json *rect = member(destination, "mouseRect");
      rect != nullptr && !rect->is_null()) {
    checkFields(*rect, {"x", "y", "w", "h"}, "JsonSkin.Rect", context);
  }
}

void classifyDocumentFields(const Json &root, DecodeContext &context) {
  checkFields(root,
              {"type", "name", "author", "w", "h", "fadeout", "input",
               "scene", "close", "loadend", "playstart", "judgetimer",
               "finishmargin", "category", "property", "filepath", "offset",
               "source", "font", "image", "imageset", "value",
               "floatvalue", "text", "slider", "graph", "gaugegraph",
               "judgegraph", "bpmgraph", "hiterrorvisualizer",
               "timingvisualizer", "timingdistributiongraph", "note",
               "gauge", "hiddenCover", "liftCover", "bga", "skinpreview",
               "practice", "judge", "songlist", "pmchara", "skinSelect",
               "customEvents", "customTimers", "destination"},
              "JsonSkin.Skin", context);

  const auto simpleArray = [&](std::string_view name,
                               std::initializer_list<std::string_view> fields,
                               std::string_view surface) {
    visitObjectArray(root, name, context, [&](const Json &item, std::size_t) {
      checkFields(item, fields, surface, context);
    });
  };
  simpleArray("category", {"name", "item"}, "JsonSkin.Category");
  visitObjectArray(root, "property", context,
                   [&](const Json &item, std::size_t) {
    checkFields(item, {"category", "name", "item", "def"},
                "JsonSkin.Property", context);
    if (const Json *choices = member(item, "item");
        choices != nullptr && choices->is_array()) {
      for (const auto &choice : *choices) {
        checkFields(choice, {"name", "op"}, "JsonSkin.PropertyItem",
                    context);
      }
    }
  });
  simpleArray("filepath", {"category", "name", "path", "def"},
              "JsonSkin.Filepath");
  simpleArray("offset", {"category", "name", "id", "x", "y", "w", "h",
                              "r", "a"},
              "JsonSkin.Offset");
  simpleArray("source", {"id", "path"}, "JsonSkin.Source");
  visitObjectArray(root, "font", context, [&](const Json &item, std::size_t) {
    checkFields(item, {"id", "path", "fallback", "type"}, "JsonSkin.Font",
                context);
    if (const Json *fallbacks = member(item, "fallback");
        fallbacks != nullptr && fallbacks->is_array()) {
      for (const auto &fallback : *fallbacks) {
        if (fallback.is_object()) {
          checkFields(fallback, {"path", "value", "type"},
                      "JsonSkin.FontFallback", context);
        }
      }
    }
  });
  const std::initializer_list<std::string_view> imageFields = {
      "id", "src", "x", "y", "w", "h", "divx", "divy", "timer",
      "cycle", "len", "ref", "act", "click"};
  simpleArray("image", imageFields, "JsonSkin.Image");
  simpleArray("imageset", {"id", "ref", "value", "images", "act", "click"},
              "JsonSkin.ImageSet");
  const std::initializer_list<std::string_view> valueFields = {
      "id", "src", "x", "y", "w", "h", "divx", "divy", "timer",
      "cycle", "align", "digit", "padding", "zeropadding", "space",
      "ref", "value", "offset"};
  visitObjectArray(root, "value", context, [&](const Json &item, std::size_t) {
    checkFields(item, valueFields, "JsonSkin.Value", context);
    if (const Json *offsets = member(item, "offset");
        offsets != nullptr && offsets->is_array()) {
      for (const auto &offset : *offsets) {
        checkFields(offset, valueFields, "JsonSkin.Value", context);
      }
    }
  });
  const std::initializer_list<std::string_view> floatFields = {
      "id",      "src",           "x",       "y",      "w",
      "h",       "divx",          "divy",    "timer",  "cycle",
      "align",   "fketa",         "iketa",   "gain",   "isSignvisible",
      "padding", "zeropadding",   "space",   "ref",    "value",
      "offset"};
  visitObjectArray(root, "floatvalue", context,
                   [&](const Json &item, std::size_t) {
    checkFields(item, floatFields, "JsonSkin.FloatValue", context);
    if (const Json *offsets = member(item, "offset");
        offsets != nullptr && offsets->is_array()) {
      for (const auto &offset : *offsets) {
        checkFields(offset, valueFields, "JsonSkin.Value", context);
      }
    }
  });
  simpleArray("text", {"id", "font", "size", "align", "ref", "value",
                              "event", "constantText", "editable", "wrapping",
                              "overflow", "outlineColor", "outlineWidth",
                              "shadowColor", "shadowOffsetX", "shadowOffsetY",
                              "shadowSmoothness"},
              "JsonSkin.Text");
  simpleArray("slider", {"id", "src", "x", "y", "w", "h", "divx",
                                "divy", "timer", "cycle", "angle", "range",
                                "type", "changeable", "value", "event",
                                "isRefNum", "min", "max"},
              "JsonSkin.Slider");
  simpleArray("graph", {"id", "src", "x", "y", "w", "h", "divx",
                               "divy", "timer", "cycle", "angle", "type",
                               "value", "isRefNum", "min", "max"},
              "JsonSkin.Graph");
  simpleArray("gaugegraph",
              {"id", "color", "assistClearBGColor",
               "assistAndEasyFailBGColor", "grooveFailBGColor",
               "grooveClearAndHardBGColor", "exHardBGColor", "hazardBGColor",
               "assistClearLineColor", "assistAndEasyFailLineColor",
               "grooveFailLineColor", "grooveClearAndHardLineColor",
               "exHardLineColor", "hazardLineColor", "borderlineColor",
               "borderColor"},
              "JsonSkin.GaugeGraph");
  simpleArray("judgegraph", {"id", "type", "backTexOff", "delay",
                                    "orderReverse", "noGap", "noGapX"},
              "JsonSkin.JudgeGraph");
  simpleArray("bpmgraph", {"id", "delay", "lineWidth", "mainBPMColor",
                                  "minBPMColor", "maxBPMColor", "otherBPMColor",
                                  "stopLineColor", "transitionLineColor"},
              "JsonSkin.BPMGraph");
  simpleArray("hiterrorvisualizer",
              {"id", "width", "judgeWidthMillis", "lineWidth", "colorMode",
               "hiterrorMode", "emaMode", "lineColor", "centerColor",
               "PGColor", "GRColor", "GDColor", "BDColor", "PRColor",
               "emaColor", "alpha", "windowLength", "transparent",
               "drawDecay"},
              "JsonSkin.HitErrorVisualizer");
  simpleArray("timingvisualizer",
              {"id", "width", "judgeWidthMillis", "lineWidth", "lineColor",
               "centerColor", "PGColor", "GRColor", "GDColor", "BDColor",
               "PRColor", "transparent", "drawDecay"},
              "JsonSkin.TimingVisualizer");
  simpleArray("timingdistributiongraph",
              {"id", "width", "lineWidth", "graphColor", "averageColor",
               "devColor", "PGColor", "GRColor", "GDColor", "BDColor",
               "PRColor", "drawAverage", "drawDev"},
              "JsonSkin.TimingDistributionGraph");
  simpleArray("hiddenCover",
              {"id", "src", "x", "y", "w", "h", "divx", "divy",
               "timer", "cycle", "disapearLine",
               "isDisapearLineLinkLift"},
              "JsonSkin.HiddenCover");
  simpleArray("liftCover",
              {"id", "src", "x", "y", "w", "h", "divx", "divy",
               "timer", "cycle", "disapearLine",
               "isDisapearLineLinkLift"},
              "JsonSkin.LiftCover");
  simpleArray("pmchara", {"id", "src", "color", "type", "side"},
              "JsonSkin.PMchara");
  simpleArray("customEvents", {"id", "action", "condition", "minInterval"},
              "JsonSkin.CustomEvent");
  simpleArray("customTimers", {"id", "timer"}, "JsonSkin.CustomTimer");
  visitObjectArray(root, "destination", context,
                   [&](const Json &item, std::size_t) {
    classifyDestination(item, context, "JsonSkin.Destination");
  });

  if (const Json *note = member(root, "note"); note != nullptr && !note->is_null()) {
    checkFields(*note,
                {"id", "note", "lnstart", "lnend", "lnbody",
                 "lnbodyActive", "lnactive", "hcnstart", "hcnend", "hcnbody",
                 "hcnactive", "hcnbodyActive", "hcndamage", "hcnbodyMiss",
                 "hcnreactive", "hcnbodyReactive", "mine", "hidden",
                 "processed", "dst", "dst2", "expansionrate", "size",
                 "group", "bpm", "stop", "time"},
                "JsonSkin.NoteSet", context);
    for (const auto name : {"group", "bpm", "stop", "time"}) {
      if (const Json *lines = member(*note, name);
          lines != nullptr && lines->is_array()) {
        for (const auto &line : *lines) {
          classifyDestination(line, context, "JsonSkin.Destination");
        }
      }
    }
    if (const Json *rects = member(*note, "dst");
        rects != nullptr && rects->is_array()) {
      for (const auto &rect : *rects) {
        checkFields(rect,
                    {"time", "x", "y", "w", "h", "clip_x", "clip_y",
                     "clip_w", "clip_h", "acc", "a", "r", "g", "b",
                     "angle"},
                    "JsonSkin.Animation", context);
      }
    }
  }
  if (const Json *gauge = member(root, "gauge"); gauge != nullptr && !gauge->is_null()) {
    checkFields(*gauge, {"id", "nodes", "parts", "type", "range", "cycle",
                         "starttime", "endtime"},
                "JsonSkin.Gauge", context);
  }
  for (const auto name : {"bga", "skinpreview"}) {
    if (const Json *identity = member(root, name);
        identity != nullptr && !identity->is_null()) {
      checkFields(*identity, {"id"}, "JsonSkin identity", context);
    }
  }
  if (const Json *practice = member(root, "practice");
      practice != nullptr && !practice->is_null()) {
    checkFields(*practice, {"id", "visibleItems"}, "JsonSkin.Practice",
                context);
  }
  visitObjectArray(root, "judge", context, [&](const Json &judge, std::size_t) {
    checkFields(judge, {"id", "index", "images", "numbers", "shift"},
                "JsonSkin.Judge", context);
    for (const auto name : {"images", "numbers"}) {
      if (const Json *children = member(judge, name);
          children != nullptr && children->is_array()) {
        for (const auto &child : *children) {
          classifyDestination(child, context, "JsonSkin.Destination");
        }
      }
    }
  });
  if (const Json *song = member(root, "songlist"); song != nullptr && !song->is_null()) {
    checkFields(*song,
                {"id", "center", "clickable", "listoff", "liston", "text",
                 "level", "lamp", "playerlamp", "rivallamp", "trophy",
                 "label", "graph"},
                "JsonSkin.SongList", context);
  }
  if (const Json *select = member(root, "skinSelect");
      select != nullptr && !select->is_null()) {
    checkFields(*select,
                {"customBMS", "defaultCategory", "customPropertyCount",
                 "customOffsetStyle"},
                "JsonSkin.SkinConfigurationProperty", context);
  }
}

SkinSourceLocation locationAt(std::string_view text, std::size_t offset,
                              std::string virtualPath) {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  for (std::size_t index = 0; index < std::min(offset, text.size()); ++index) {
    if (text[index] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  return {.virtualPath = std::move(virtualPath), .line = line, .column = column};
}

std::size_t skipJsonString(std::string_view text, std::size_t quote) {
  bool escaped = false;
  for (std::size_t index = quote + 1; index < text.size(); ++index) {
    const char value = text[index];
    if (escaped) {
      escaped = false;
    } else if (value == '\\') {
      escaped = true;
    } else if (value == '"') {
      return index + 1;
    }
  }
  return text.size();
}

std::string jsonPointerToken(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    if (character == '~') {
      result += "~0";
    } else if (character == '/') {
      result += "~1";
    } else {
      result.push_back(character);
    }
  }
  return result;
}

class JsonSourceIndex final {
public:
  JsonSourceIndex(std::string_view text, std::string virtualPath)
      : text_(text), virtualPath_(std::move(virtualPath)), lineStarts_{0} {
    for (std::size_t index = 0; index < text_.size(); ++index) {
      if (text_[index] == '\n') lineStarts_.push_back(index + 1);
    }
  }

  [[nodiscard]] bool build(const Json &root) {
    std::size_t offset = 0;
    std::size_t values = 0;
    if (!indexValue(offset, "", 1, values)) return false;
    skipWhitespace(offset);
    if (offset != text_.size()) return false;
    bind(root, "");
    return true;
  }

  [[nodiscard]] std::optional<SkinSourceLocation>
  source(const Json *value) const {
    if (value == nullptr) return std::nullopt;
    const auto found = nodeSources_.find(value);
    return found == nodeSources_.end()
               ? std::nullopt
               : std::optional<SkinSourceLocation>(found->second);
  }

private:
  [[nodiscard]] SkinSourceLocation sourceAt(std::size_t offset) const {
    const auto next =
        std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    const std::size_t lineIndex =
        static_cast<std::size_t>(next - lineStarts_.begin() - 1);
    return {.virtualPath = virtualPath_,
            .line = static_cast<std::uint32_t>(lineIndex + 1),
            .column = static_cast<std::uint32_t>(
                offset - lineStarts_[lineIndex] + 1)};
  }

  void skipWhitespace(std::size_t &offset) const {
    while (offset < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[offset]))) {
      ++offset;
    }
  }

  [[nodiscard]] std::optional<std::string>
  decodeString(std::size_t &offset) const {
    if (offset >= text_.size() || text_[offset] != '"') return std::nullopt;
    const std::size_t end = skipJsonString(text_, offset);
    if (end > text_.size() || end == 0 || text_[end - 1] != '"') {
      return std::nullopt;
    }
    try {
      Json decoded = Json::parse(text_.begin() +
                                     static_cast<std::ptrdiff_t>(offset),
                                 text_.begin() +
                                     static_cast<std::ptrdiff_t>(end));
      offset = end;
      return decoded.get<std::string>();
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] bool indexValue(std::size_t &offset, std::string pointer,
                                std::size_t depth, std::size_t &values) {
    skipWhitespace(offset);
    if (offset >= text_.size() ||
        depth > JsonGameplaySkinDecoderPolicy::maxDepth ||
        ++values > JsonGameplaySkinDecoderPolicy::maxValues) {
      return false;
    }
    locations_.insert_or_assign(pointer, sourceAt(offset));
    const char leading = text_[offset];
    if (leading == '"') {
      const std::size_t end = skipJsonString(text_, offset);
      if (end > text_.size() || end == 0 || text_[end - 1] != '"') {
        return false;
      }
      offset = end;
      return true;
    }
    if (leading == '{') {
      ++offset;
      skipWhitespace(offset);
      if (offset < text_.size() && text_[offset] == '}') {
        ++offset;
        return true;
      }
      std::size_t members = 0;
      while (offset < text_.size()) {
        if (++members > JsonGameplaySkinDecoderPolicy::maxArrayEntries) {
          return false;
        }
        auto key = decodeString(offset);
        if (!key) return false;
        skipWhitespace(offset);
        if (offset >= text_.size() || text_[offset] != ':') return false;
        ++offset;
        if (!indexValue(offset,
                        pointer + "/" + jsonPointerToken(*key), depth + 1,
                        values)) {
          return false;
        }
        skipWhitespace(offset);
        if (offset < text_.size() && text_[offset] == '}') {
          ++offset;
          return true;
        }
        if (offset >= text_.size() || text_[offset] != ',') return false;
        ++offset;
        skipWhitespace(offset);
      }
      return false;
    }
    if (leading == '[') {
      ++offset;
      skipWhitespace(offset);
      if (offset < text_.size() && text_[offset] == ']') {
        ++offset;
        return true;
      }
      std::size_t index = 0;
      while (offset < text_.size()) {
        if (index >= JsonGameplaySkinDecoderPolicy::maxArrayEntries ||
            !indexValue(offset, pointer + "/" + std::to_string(index),
                        depth + 1, values)) {
          return false;
        }
        ++index;
        skipWhitespace(offset);
        if (offset < text_.size() && text_[offset] == ']') {
          ++offset;
          return true;
        }
        if (offset >= text_.size() || text_[offset] != ',') return false;
        ++offset;
        skipWhitespace(offset);
      }
      return false;
    }
    const std::size_t start = offset;
    while (offset < text_.size() && text_[offset] != ',' &&
           text_[offset] != '}' && text_[offset] != ']' &&
           !std::isspace(static_cast<unsigned char>(text_[offset]))) {
      ++offset;
    }
    return offset != start;
  }

  void bind(const Json &value, const std::string &pointer) {
    if (const auto found = locations_.find(pointer);
        found != locations_.end()) {
      nodeSources_.insert_or_assign(&value, found->second);
    }
    if (value.is_object()) {
      for (auto field = value.begin(); field != value.end(); ++field) {
        bind(field.value(), pointer + "/" + jsonPointerToken(field.key()));
      }
    } else if (value.is_array()) {
      for (std::size_t index = 0; index < value.size(); ++index) {
        bind(value[index], pointer + "/" + std::to_string(index));
      }
    }
  }

  std::string_view text_;
  std::string virtualPath_;
  std::vector<std::size_t> lineStarts_;
  std::map<std::string, SkinSourceLocation, std::less<>> locations_;
  std::map<const Json *, SkinSourceLocation> nodeSources_;
};

std::optional<SkinSourceLocation>
DecodeContext::source(const Json *value) const {
  return sources == nullptr ? std::nullopt : sources->source(value);
}

bool validateBudget(const Json &root, DecodeContext &context) {
  struct Work {
    const Json *value;
    std::size_t depth;
  };
  std::vector<Work> pending{{&root, 1}};
  std::size_t values = 0;
  while (!pending.empty()) {
    const auto current = pending.back();
    pending.pop_back();
    if (++values > JsonGameplaySkinDecoderPolicy::maxValues ||
        current.depth > JsonGameplaySkinDecoderPolicy::maxDepth) {
      context.error("skin_json_limit_exceeded",
                    "JSON gameplay document exceeds the fixed structure limit");
      return false;
    }
    if (current.value->is_array() || current.value->is_object()) {
      if (current.value->size() > JsonGameplaySkinDecoderPolicy::maxArrayEntries) {
        context.error("skin_json_limit_exceeded",
                      "JSON gameplay collection exceeds the fixed entry limit");
        return false;
      }
      for (const auto &child : *current.value) {
        pending.push_back({&child, current.depth + 1});
      }
    }
  }
  return true;
}

struct StaticBindingRegistry {
  DecodeContext &context;
  std::vector<SkinBooleanPropertyBinding> booleans;
  std::vector<SkinIntegerPropertyBinding> integers;
  std::vector<SkinFloatPropertyBinding> floats;
  std::vector<SkinStringPropertyBinding> strings;
  std::vector<SkinTimerPropertyBinding> timers;
  std::vector<SkinFloatWriterBinding> floatWriters;
  std::vector<SkinStringWriterBinding> stringWriters;
  std::vector<SkinEventBinding> events;
  std::map<std::variant<int, std::string>, SkinBooleanPropertyId> booleanIds;
  std::array<std::map<std::variant<int, std::string>, SkinIntegerPropertyId>, 2>
      integerIds;
  std::array<std::map<std::variant<int, std::string>, SkinFloatPropertyId>, 2>
      floatIds;
  std::map<std::variant<int, std::string>, SkinStringPropertyId> stringIds;
  std::map<std::variant<int, std::string>, SkinTimerPropertyId> timerIds;
  std::map<std::variant<int, std::string>, SkinFloatWriterId> floatWriterIds;
  std::map<std::variant<int, std::string>, SkinStringWriterId> stringWriterIds;
  std::map<std::variant<int, std::string>, SkinEventBindingId> eventIds;

  std::optional<SkinBuiltinPropertySelector>
  selector(const Json *authored, SkinBindingType type,
           std::optional<int> fallback, std::string_view path) {
    std::optional<SkinBuiltinPropertySelector> result;
    if (authored != nullptr && !authored->is_null()) {
      if (const auto numeric = integerValue(*authored)) {
        result = SkinBuiltinPropertySelector{*numeric};
      } else if (authored->is_string()) {
        result = SkinBuiltinPropertySelector{authored->get<std::string>()};
      } else {
        return std::nullopt;
      }
    } else if (fallback) {
      result = SkinBuiltinPropertySelector{*fallback};
    }
    if (!result) {
      return std::nullopt;
    }
    if (!context.builtins.contains(type, *result)) {
      if (std::holds_alternative<std::string>(result->value)) {
        context.warning("skin_json_callback_unsupported",
                        "JSON binding '" + std::string(path) +
                            "' requires Lua and was left static/unbound",
                        context.source(authored));
      }
      return std::nullopt;
    }
    return result;
  }

  std::optional<SkinBooleanPropertyId>
  boolean(const Json *authored, std::optional<int> fallback,
          std::uint32_t ordinal, std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::BooleanProperty};
    auto selected = selector(authored, type, fallback, path);
    if (!selected) return std::nullopt;
    if (const auto found = booleanIds.find(selected->value);
        found != booleanIds.end()) return found->second;
    const SkinBooleanPropertyId id{static_cast<std::uint32_t>(booleans.size() + 1)};
    booleans.push_back({.id = id, .source = *selected,
                        .authoredOrdinal = ordinal});
    booleanIds.emplace(selected->value, id);
    return id;
  }

  std::optional<SkinIntegerPropertyId>
  integer(const Json *authored, std::optional<int> fallback,
          SkinIntegerPropertyDomain domain, std::uint32_t ordinal,
          std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::IntegerProperty,
                               .integerDomain = domain};
    auto selected = selector(authored, type, fallback, path);
    if (!selected) return std::nullopt;
    const std::size_t bucket = static_cast<std::size_t>(domain);
    if (const auto found = integerIds[bucket].find(selected->value);
        found != integerIds[bucket].end()) return found->second;
    const SkinIntegerPropertyId id{static_cast<std::uint32_t>(integers.size() + 1)};
    integers.push_back({.id = id, .domain = domain, .source = *selected,
                        .authoredOrdinal = ordinal});
    integerIds[bucket].emplace(selected->value, id);
    return id;
  }

  std::optional<SkinFloatPropertyId>
  floating(const Json *authored, std::optional<int> fallback,
           SkinFloatPropertyDomain domain, std::uint32_t ordinal,
           std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::FloatProperty,
                               .floatDomain = domain};
    auto selected = selector(authored, type, fallback, path);
    if (!selected) return std::nullopt;
    const std::size_t bucket = static_cast<std::size_t>(domain);
    if (const auto found = floatIds[bucket].find(selected->value);
        found != floatIds[bucket].end()) return found->second;
    const SkinFloatPropertyId id{static_cast<std::uint32_t>(floats.size() + 1)};
    floats.push_back({.id = id, .domain = domain, .source = *selected,
                      .authoredOrdinal = ordinal});
    floatIds[bucket].emplace(selected->value, id);
    return id;
  }

  std::optional<SkinStringPropertyId>
  string(const Json *authored, std::optional<int> fallback,
         std::uint32_t ordinal, std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::StringProperty};
    auto selected = selector(authored, type, fallback, path);
    if (!selected) return std::nullopt;
    if (const auto found = stringIds.find(selected->value);
        found != stringIds.end()) return found->second;
    const SkinStringPropertyId id{static_cast<std::uint32_t>(strings.size() + 1)};
    strings.push_back({.id = id, .source = *selected,
                       .authoredOrdinal = ordinal});
    stringIds.emplace(selected->value, id);
    return id;
  }

  std::optional<SkinTimerPropertyId>
  timer(const Json *authored, std::uint32_t ordinal, std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::TimerProperty};
    auto selected = selector(authored, type, std::nullopt, path);
    if (!selected) return std::nullopt;
    if (const auto found = timerIds.find(selected->value);
        found != timerIds.end()) return found->second;
    const SkinTimerPropertyId id{static_cast<std::uint32_t>(timers.size() + 1)};
    timers.push_back({.id = id, .source = *selected,
                      .authoredOrdinal = ordinal});
    timerIds.emplace(selected->value, id);
    return id;
  }

  std::optional<SkinFloatWriterId>
  floatWriter(const Json *authored, std::optional<int> fallback,
              std::uint32_t ordinal, std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::FloatWriter};
    auto selected = selector(authored, type, fallback, path);
    if (!selected) return std::nullopt;
    if (const auto found = floatWriterIds.find(selected->value);
        found != floatWriterIds.end()) return found->second;
    const SkinFloatWriterId id{static_cast<std::uint32_t>(floatWriters.size() + 1)};
    floatWriters.push_back({.id = id, .source = *selected,
                            .authoredOrdinal = ordinal});
    floatWriterIds.emplace(selected->value, id);
    return id;
  }

  std::optional<SkinStringWriterId>
  stringWriter(const Json *authored, std::optional<int> fallback,
               std::uint32_t ordinal, std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::StringWriter};
    auto selected = selector(authored, type, fallback, path);
    if (!selected) return std::nullopt;
    if (const auto found = stringWriterIds.find(selected->value);
        found != stringWriterIds.end()) return found->second;
    const SkinStringWriterId id{static_cast<std::uint32_t>(stringWriters.size() + 1)};
    stringWriters.push_back({.id = id, .source = *selected,
                             .authoredOrdinal = ordinal});
    stringWriterIds.emplace(selected->value, id);
    return id;
  }

  std::optional<SkinEventBindingId>
  event(const Json *authored, std::uint32_t ordinal, std::string_view path) {
    const SkinBindingType type{.kind = SkinBindingKind::Event};
    auto selected = selector(authored, type, std::nullopt, path);
    if (!selected) return std::nullopt;
    if (const auto found = eventIds.find(selected->value);
        found != eventIds.end()) return found->second;
    const SkinEventBindingId id{static_cast<std::uint32_t>(events.size() + 1)};
    events.push_back({.id = id, .source = *selected,
                      .authoredOrdinal = ordinal});
    eventIds.emplace(selected->value, id);
    return id;
  }

  void moveInto(BeatorajaSkinModel &model) {
    model.booleanProperties = std::move(booleans);
    model.integerProperties = std::move(integers);
    model.floatProperties = std::move(floats);
    model.stringProperties = std::move(strings);
    model.timerProperties = std::move(timers);
    model.floatWriters = std::move(floatWriters);
    model.stringWriters = std::move(stringWriters);
    model.events = std::move(events);
  }
};

OffsetPermissionMask permissionMask(const Json &offset, DecodeContext &context) {
  OffsetPermissionMask result = 0;
  if (booleanField(offset, "x", false, context, "JsonSkin.Offset"))
    result |= kOffsetPermissionX;
  if (booleanField(offset, "y", false, context, "JsonSkin.Offset"))
    result |= kOffsetPermissionY;
  if (booleanField(offset, "w", false, context, "JsonSkin.Offset"))
    result |= kOffsetPermissionW;
  if (booleanField(offset, "h", false, context, "JsonSkin.Offset"))
    result |= kOffsetPermissionH;
  if (booleanField(offset, "r", false, context, "JsonSkin.Offset"))
    result |= kOffsetPermissionR;
  if (booleanField(offset, "a", false, context, "JsonSkin.Offset"))
    result |= kOffsetPermissionA;
  return result;
}

BeatorajaSkinHeader decodeHeader(const Json &root, DecodeContext &context) {
  BeatorajaSkinHeader header;
  header.type = integerField(root, "type", -1, context, "JsonSkin.Skin");
  header.width = integerField(root, "w", 1280, context, "JsonSkin.Skin");
  header.height = integerField(root, "h", 720, context, "JsonSkin.Skin");
  header.name = stringField(root, "name", {}, context, "JsonSkin.Skin");
  header.author = stringField(root, "author", {}, context, "JsonSkin.Skin");
  visitObjectArray(root, "category", context,
                   [&](const Json &item, std::size_t) {
    header.categories.push_back(
        {.name = stringField(item, "name", {}, context, "JsonSkin.Category"),
         .items = stringArrayField(item, "item", context,
                                   "JsonSkin.Category")});
  });
  visitObjectArray(root, "property", context,
                   [&](const Json &item, std::size_t) {
    SkinHeaderOption option{
        .category = stringField(item, "category", {}, context,
                                "JsonSkin.Property"),
        .name = stringField(item, "name", {}, context,
                            "JsonSkin.Property"),
        .defaultLabel = stringField(item, "def", {}, context,
                                    "JsonSkin.Property")};
    if (const Json *choices = member(item, "item");
        choices != nullptr && choices->is_array()) {
      for (const auto &choice : *choices) {
        option.choices.push_back(
            {.label = stringField(choice, "name", {}, context,
                                  "JsonSkin.PropertyItem"),
             .value = integerField(choice, "op", 0, context,
                                   "JsonSkin.PropertyItem")});
      }
    }
    header.options.push_back(std::move(option));
  });
  visitObjectArray(root, "filepath", context,
                   [&](const Json &item, std::size_t) {
    header.files.push_back(
        {.category = stringField(item, "category", {}, context,
                                 "JsonSkin.Filepath"),
         .name = stringField(item, "name", {}, context,
                             "JsonSkin.Filepath"),
         .pattern = stringField(item, "path", {}, context,
                                "JsonSkin.Filepath"),
         .defaultValue = stringField(item, "def", {}, context,
                                     "JsonSkin.Filepath")});
  });
  visitObjectArray(root, "offset", context,
                   [&](const Json &item, std::size_t) {
    header.offsets.push_back(
        {.category = stringField(item, "category", {}, context,
                                 "JsonSkin.Offset"),
         .name = stringField(item, "name", {}, context, "JsonSkin.Offset"),
         .id = integerField(item, "id", 0, context, "JsonSkin.Offset"),
         .permissions = permissionMask(item, context)});
  });
  if (gameplaySkinTraitForSkinType(header.type)) {
    header.offsets.push_back({.name = "All offset(%)", .id = 10,
                              .permissions = kOffsetPermissionX |
                                             kOffsetPermissionY |
                                             kOffsetPermissionW |
                                             kOffsetPermissionH});
    header.offsets.push_back({.name = "Notes offset", .id = 30,
                              .permissions = kOffsetPermissionH});
    header.offsets.push_back({.name = "Judge offset", .id = 32,
                              .permissions = kOffsetPermissionX |
                                             kOffsetPermissionY |
                                             kOffsetPermissionW |
                                             kOffsetPermissionH |
                                             kOffsetPermissionA});
    header.offsets.push_back({.name = "Judge Detail offset", .id = 33,
                              .permissions = kOffsetPermissionX |
                                             kOffsetPermissionY |
                                             kOffsetPermissionW |
                                             kOffsetPermissionH |
                                             kOffsetPermissionA});
  }
  return header;
}

ConfigOffset sanitizeOffset(ConfigOffset value,
                            OffsetPermissionMask permissions) {
  if ((permissions & kOffsetPermissionX) == 0) value.x = 0;
  if ((permissions & kOffsetPermissionY) == 0) value.y = 0;
  if ((permissions & kOffsetPermissionW) == 0) value.w = 0;
  if ((permissions & kOffsetPermissionH) == 0) value.h = 0;
  if ((permissions & kOffsetPermissionR) == 0) value.r = 0;
  if ((permissions & kOffsetPermissionA) == 0) value.a = 0;
  return value;
}

void reconcileConfiguration(const BeatorajaSkinHeader &header,
                            const EntryProfileSettings *desired,
                            JsonGameplaySkinDecodeResult &result,
                            DecodeContext &context) {
  BeatorajaSkinConfiguration configuration;
  EntryProfileSettings settings;
  if (desired != nullptr) {
    settings.viewport = desired->viewport;
  }
  for (const auto &option : header.options) {
    if (option.name.empty()) {
      context.error("skin_json_configuration_invalid",
                    "JSON skin option has an empty name");
      return;
    }
    int runtimeValue = -1;
    int persistedValue = -1;
    if (!option.choices.empty()) {
      runtimeValue = option.choices.front().value;
      for (const auto &choice : option.choices) {
        if (choice.label == option.defaultLabel) runtimeValue = choice.value;
      }
      persistedValue = runtimeValue;
      if (desired != nullptr) {
        if (const auto found = desired->options.find(option.name);
            found != desired->options.end()) {
          if (found->second == -1) {
            persistedValue = -1;
            runtimeValue = option.choices.front().value;
          } else if (std::ranges::any_of(
                         option.choices, [&](const auto &choice) {
                           return choice.value == found->second;
                         })) {
            persistedValue = found->second;
            runtimeValue = found->second;
          }
        }
      }
    }
    configuration.orderedOptions.push_back(
        {.name = option.name, .value = runtimeValue});
    configuration.options.insert_or_assign(option.name, persistedValue);
    configuration.enabledOptionIds.insert(runtimeValue);
    settings.options.insert_or_assign(option.name, persistedValue);
  }
  for (const auto &file : header.files) {
    if (file.name.empty()) {
      context.error("skin_json_configuration_invalid",
                    "JSON skin file declaration has an empty name");
      return;
    }
    std::string selected = file.defaultValue;
    if (desired != nullptr) {
      if (const auto found = desired->filePaths.find(file.name);
          found != desired->filePaths.end()) selected = found->second;
    }
    configuration.orderedFiles.push_back(
        {.name = file.name,
         .pattern = file.pattern,
         .selectedValue = selected,
         .choices = {}});
    if (!selected.empty()) {
      configuration.filePaths.insert_or_assign(file.name, selected);
      settings.filePaths.insert_or_assign(file.name, selected);
    }
  }
  for (const auto &offset : header.offsets) {
    if (offset.name.empty()) {
      context.error("skin_json_configuration_invalid",
                    "JSON skin offset has an empty name");
      return;
    }
    ConfigOffset value;
    if (desired != nullptr) {
      if (const auto found = desired->offsets.find(offset.name);
          found != desired->offsets.end()) value = found->second;
    }
    value = sanitizeOffset(value, offset.permissions);
    configuration.offsets.insert_or_assign(offset.name, value);
    configuration.offsetPermissions.insert_or_assign(offset.name,
                                                      offset.permissions);
    configuration.offsetsById.insert_or_assign(offset.id, value);
    settings.offsets.insert_or_assign(offset.name, value);
  }
  configuration.lowercaseSha256 = skinConfigurationDigest(EntryProfileSettings{
      .options = configuration.options,
      .filePaths = configuration.filePaths,
      .offsets = configuration.offsets});
  result.configuration = std::move(configuration);
  result.reconciledSettings = std::move(settings);
}

struct DefinitionReference {
  const Json *value = nullptr;
  std::size_t authoredIndex = 0;
};

using DefinitionMap =
    std::map<std::string, DefinitionReference, std::less<>>;

void indexDefinitions(const Json &root, std::string_view arrayName,
                      DefinitionMap &output, DecodeContext &context,
                      std::string_view surface) {
  visitObjectArray(root, arrayName, context,
                   [&](const Json &item, std::size_t index) {
    const std::string id =
        stringField(item, "id", {}, context, surface);
    output.try_emplace(id, DefinitionReference{.value = &item,
                                               .authoredIndex = index});
  });
}

struct BuildState {
  DecodeContext &context;
  const Json &root;
  BeatorajaSkinModel &model;
  StaticBindingRegistry bindings;
  std::map<std::string, SkinResourceId, std::less<>> sourceIds;
  std::map<std::string, std::string, std::less<>> sourcePaths;
  std::map<std::string, SkinResourceId, std::less<>> fontIds;
  DefinitionMap images;
  DefinitionMap imageSets;
  DefinitionMap values;
  DefinitionMap floatValues;
  DefinitionMap texts;
  DefinitionMap sliders;
  DefinitionMap graphs;
  DefinitionMap gaugeGraphs;
  DefinitionMap judgeGraphs;
  DefinitionMap bpmGraphs;
  DefinitionMap hitErrorVisualizers;
  DefinitionMap timingVisualizers;
  DefinitionMap timingDistributionGraphs;
  DefinitionMap hiddenCovers;
  DefinitionMap liftCovers;
  DefinitionMap judges;
  DefinitionMap pmCharas;
  std::vector<SkinObjectDefinition> nestedObjects;
  std::optional<DefinitionReference> note;
  std::optional<DefinitionReference> gauge;
  std::optional<DefinitionReference> practice;
  std::optional<DefinitionReference> bga;
  SkinObjectId nextSyntheticObjectId = 1;

  BuildState(DecodeContext &contextValue, const Json &rootValue,
             BeatorajaSkinModel &modelValue)
      : context(contextValue), root(rootValue), model(modelValue),
        bindings{contextValue} {}
};

bool asciiCaseEndsWith(std::string_view value, std::string_view suffix) {
  if (value.size() < suffix.size()) return false;
  value.remove_prefix(value.size() - suffix.size());
  return std::ranges::equal(value, suffix, [](char left, char right) {
    const auto fold = [](unsigned char character) {
      return character >= 'A' && character <= 'Z'
                 ? static_cast<unsigned char>(character - 'A' + 'a')
                 : character;
    };
    return fold(static_cast<unsigned char>(left)) ==
           fold(static_cast<unsigned char>(right));
  });
}

void decodeResources(BuildState &state) {
  visitObjectArray(state.root, "source", state.context,
                   [&](const Json &source, std::size_t ordinal) {
    const std::string name = stringField(source, "id", {}, state.context,
                                         "JsonSkin.Source");
    const std::string path = stringField(source, "path", {}, state.context,
                                         "JsonSkin.Source");
    const SkinResourceId id{static_cast<std::uint32_t>(
        state.model.resources.size() + 1)};
    state.sourceIds.insert_or_assign(name, id);
    state.sourcePaths.insert_or_assign(name, path);
    state.model.resources.emplace_back(SkinImageResource{
        .id = id,
        .authoredName = name,
        .virtualPath = path,
        .authoredOrdinal = static_cast<std::uint32_t>(ordinal),
    });
  });
  visitObjectArray(state.root, "font", state.context,
                   [&](const Json &font, std::size_t) {
    const std::string name = stringField(font, "id", {}, state.context,
                                         "JsonSkin.Font");
    const std::string path = stringField(font, "path", {}, state.context,
                                         "JsonSkin.Font");
    const auto ordinal =
        static_cast<std::uint32_t>(state.model.resources.size());
    const SkinResourceId id{ordinal + 1};
    SkinFontResource resource{
        .id = id,
        .authoredName = name,
        .virtualPath = path,
        .type = integerField(font, "type", 0, state.context,
                             "JsonSkin.Font"),
        .authoredOrdinal = ordinal,
    };
    if (const Json *fallbacks = member(font, "fallback");
        fallbacks != nullptr && fallbacks->is_array()) {
      for (const auto &fallback : *fallbacks) {
        if (fallback.is_string()) {
          resource.fallbacks.push_back(
              {.virtualPath = fallback.get<std::string>(), .type = 0});
        } else if (fallback.is_object()) {
          std::string fallbackPath = stringField(
              fallback, "path", {}, state.context, "JsonSkin.FontFallback");
          if (fallbackPath.empty()) {
            fallbackPath = stringField(fallback, "value", {}, state.context,
                                       "JsonSkin.FontFallback");
          }
          resource.fallbacks.push_back(
              {.virtualPath = std::move(fallbackPath),
               .type = integerField(fallback, "type", 0, state.context,
                                    "JsonSkin.FontFallback")});
        }
      }
    }
    if (asciiCaseEndsWith(path, ".fnt")) {
      resource.bitmap = SkinBitmapFontResource{
          .id = id,
          .virtualPath = path,
          .type = resource.type,
          .authoredOrdinal = ordinal,
      };
    }
    state.fontIds.try_emplace(name, id);
    state.model.resources.emplace_back(std::move(resource));
  });
}

void indexGameplayDefinitions(BuildState &state) {
  indexDefinitions(state.root, "image", state.images, state.context,
                   "JsonSkin.Image");
  indexDefinitions(state.root, "imageset", state.imageSets, state.context,
                   "JsonSkin.ImageSet");
  indexDefinitions(state.root, "value", state.values, state.context,
                   "JsonSkin.Value");
  indexDefinitions(state.root, "floatvalue", state.floatValues, state.context,
                   "JsonSkin.FloatValue");
  indexDefinitions(state.root, "text", state.texts, state.context,
                   "JsonSkin.Text");
  indexDefinitions(state.root, "slider", state.sliders, state.context,
                   "JsonSkin.Slider");
  indexDefinitions(state.root, "graph", state.graphs, state.context,
                   "JsonSkin.Graph");
  indexDefinitions(state.root, "gaugegraph", state.gaugeGraphs, state.context,
                   "JsonSkin.GaugeGraph");
  indexDefinitions(state.root, "judgegraph", state.judgeGraphs, state.context,
                   "JsonSkin.JudgeGraph");
  indexDefinitions(state.root, "bpmgraph", state.bpmGraphs, state.context,
                   "JsonSkin.BPMGraph");
  indexDefinitions(state.root, "hiterrorvisualizer", state.hitErrorVisualizers,
                   state.context, "JsonSkin.HitErrorVisualizer");
  indexDefinitions(state.root, "timingvisualizer", state.timingVisualizers,
                   state.context, "JsonSkin.TimingVisualizer");
  indexDefinitions(state.root, "timingdistributiongraph",
                   state.timingDistributionGraphs, state.context,
                   "JsonSkin.TimingDistributionGraph");
  indexDefinitions(state.root, "hiddenCover", state.hiddenCovers,
                   state.context, "JsonSkin.HiddenCover");
  indexDefinitions(state.root, "liftCover", state.liftCovers, state.context,
                   "JsonSkin.LiftCover");
  indexDefinitions(state.root, "judge", state.judges, state.context,
                   "JsonSkin.Judge");
  indexDefinitions(state.root, "pmchara", state.pmCharas, state.context,
                   "JsonSkin.PMchara");
  const auto identity = [&](std::string_view name,
                            std::optional<DefinitionReference> &output) {
    if (const Json *value = member(state.root, name);
        value != nullptr && !value->is_null()) {
      output = DefinitionReference{.value = value, .authoredIndex = 0};
    }
  };
  identity("note", state.note);
  identity("gauge", state.gauge);
  identity("practice", state.practice);
  identity("bga", state.bga);
}

std::string bindingPath(std::string_view array, std::size_t index,
                        std::string_view field) {
  return std::string(array) + "[" + std::to_string(index + 1) + "]." +
         std::string(field);
}

SkinSpriteFrames spriteForImage(BuildState &state,
                                const DefinitionReference &reference,
                                std::string_view arrayName) {
  const Json &image = *reference.value;
  const std::string source = stringField(image, "src", {}, state.context,
                                         "JsonSkin.Image");
  const auto sourceId = state.sourceIds.find(source);
  int divisionsX = integerField(image, "divx", 1, state.context,
                                "JsonSkin.Image");
  int divisionsY = integerField(image, "divy", 1, state.context,
                                "JsonSkin.Image");
  divisionsX = divisionsX > 0 ? divisionsX : 1;
  divisionsY = divisionsY > 0 ? divisionsY : 1;
  if (static_cast<std::size_t>(divisionsX) >
      JsonGameplaySkinDecoderPolicy::maxValues /
          static_cast<std::size_t>(divisionsY)) {
    state.context.error("skin_json_limit_exceeded",
                        "JSON image divisions exceed the frame limit",
                        state.context.source(member(image, "divx")));
    return {};
  }
  SkinSpriteFrames result{
      .resource = sourceId == state.sourceIds.end() ? 0 : sourceId->second,
      .cycleMillis = integerField(image, "cycle", 0, state.context,
                                  "JsonSkin.Image"),
      .timer = state.bindings.timer(
          member(image, "timer"),
          static_cast<std::uint32_t>(reference.authoredIndex),
          bindingPath(arrayName, reference.authoredIndex, "timer")),
  };
  const int x = integerField(image, "x", 0, state.context, "JsonSkin.Image");
  const int y = integerField(image, "y", 0, state.context, "JsonSkin.Image");
  const int width =
      integerField(image, "w", 0, state.context, "JsonSkin.Image");
  const int height =
      integerField(image, "h", 0, state.context, "JsonSkin.Image");
  result.frames.reserve(static_cast<std::size_t>(divisionsX) *
                        static_cast<std::size_t>(divisionsY));
  for (int row = 0; row < divisionsY; ++row) {
    for (int column = 0; column < divisionsX; ++column) {
      result.frames.push_back({.x = x,
                               .y = y,
                               .w = width,
                               .h = height,
                               .gridColumn = column,
                               .gridRow = row,
                               .gridColumns = divisionsX,
                               .gridRows = divisionsY});
    }
  }
  return result;
}

SkinSpriteFrames spriteForImageId(BuildState &state, std::string_view id) {
  const auto found = state.images.find(id);
  return found == state.images.end()
             ? SkinSpriteFrames{}
             : spriteForImage(state, found->second, "image");
}

std::vector<SkinDigitOffset> digitOffsets(const Json &definition,
                                          DecodeContext &context,
                                          std::string_view surface) {
  std::vector<SkinDigitOffset> result;
  const Json *offsets = member(definition, "offset");
  if (offsets == nullptr || offsets->is_null()) return result;
  if (!offsets->is_array() ||
      offsets->size() > JsonGameplaySkinDecoderPolicy::maxArrayEntries) {
    context.error("skin_json_model_invalid",
                  std::string(surface) + ".offset is not a bounded array",
                  context.source(offsets));
    return result;
  }
  result.reserve(offsets->size());
  for (const auto &offset : *offsets) {
    result.push_back(
        {.x = numberField(offset, "x", 0.0, context, "JsonSkin.Value"),
         .y = numberField(offset, "y", 0.0, context, "JsonSkin.Value"),
         .width = numberField(offset, "w", 0.0, context, "JsonSkin.Value"),
         .height = numberField(offset, "h", 0.0, context,
                               "JsonSkin.Value")});
  }
  return result;
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::optional<std::uint32_t> directColor(std::string_view value) {
  if (!value.empty() && value.front() == '#') value.remove_prefix(1);
  if (value.size() != 6 && value.size() != 8) return std::nullopt;
  std::uint32_t result = 0;
  for (char character : value) {
    const int nibble = hexNibble(character);
    if (nibble < 0) return std::nullopt;
    result = result * 16U + static_cast<std::uint32_t>(nibble);
  }
  return value.size() == 6 ? (result << 8U) | 0xffU : result;
}

std::uint32_t colorOr(std::string_view value, std::uint32_t fallback) {
  const auto decoded = directColor(value);
  return decoded.value_or(fallback);
}

std::array<std::uint8_t, 4> textColor(std::string_view value) {
  const auto decoded = directColor(value);
  if (!decoded) return {255, 255, 255, 255};
  return {static_cast<std::uint8_t>(*decoded >> 24U),
          static_cast<std::uint8_t>(*decoded >> 16U),
          static_cast<std::uint8_t>(*decoded >> 8U),
          static_cast<std::uint8_t>(*decoded)};
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

SkinDestinationBody decodeDestinationBody(BuildState &state,
                                          const Json &destination,
                                          std::uint32_t authoredOrdinal,
                                          std::string_view path,
                                          bool sortFrames = true) {
  SkinDestinationBody output;
  output.authoredOrdinal = authoredOrdinal;
  output.loop = integerField(destination, "loop", 0, state.context,
                             "JsonSkin.Destination");
  const int center = integerField(destination, "center", 0, state.context,
                                  "JsonSkin.Destination");
  output.center = center >= 0 && center < 10 ? center : 0;
  const int blend = integerField(destination, "blend", 0, state.context,
                                 "JsonSkin.Destination");
  const auto mappedBlend = blendMode(blend);
  if (!mappedBlend) {
    state.context.error("skin_json_model_invalid",
                        "JsonSkin.Destination.blend is outside the pinned set",
                        state.context.source(member(destination, "blend")));
  } else {
    output.blend = *mappedBlend;
  }
  const int filter = integerField(destination, "filter", 0, state.context,
                                  "JsonSkin.Destination");
  if (filter < 0 || filter > 1) {
    state.context.error("skin_json_model_invalid",
                        "JsonSkin.Destination.filter is outside the pinned set",
                        state.context.source(member(destination, "filter")));
  } else {
    output.filter = static_cast<SkinFilterMode>(filter);
  }
  const int stretch = integerField(destination, "stretch", -1, state.context,
                                   "JsonSkin.Destination");
  if (stretch < -1 || stretch > 10) {
    state.context.error("skin_json_model_invalid",
                        "JsonSkin.Destination.stretch is outside the pinned set",
                        state.context.source(member(destination, "stretch")));
  } else if (stretch >= 0) {
    output.stretch = static_cast<SkinStretchMode>(stretch);
  }
  output.timer = state.bindings.timer(
      member(destination, "timer"), authoredOrdinal,
      std::string(path) + ".timer");
  output.offsetIds = integerArrayField(destination, "offsets", state.context,
                                       "JsonSkin.Destination");
  output.offsetIds.push_back(integerField(destination, "offset", 0,
                                          state.context,
                                          "JsonSkin.Destination"));
  if (const Json *conditions = member(destination, "op");
      conditions != nullptr && !conditions->is_null()) {
    if (!conditions->is_array()) {
      state.context.error("skin_json_model_invalid",
                          "JsonSkin.Destination.op is not an array");
    } else {
      for (std::size_t index = 0; index < conditions->size(); ++index) {
        const Json &condition = (*conditions)[index];
        const auto property = state.bindings.boolean(
            &condition, std::nullopt, authoredOrdinal,
            std::string(path) + ".op[" + std::to_string(index + 1) + "]");
        if (property) {
          output.conditions.emplace_back(*property);
        } else if (const auto option = integerValue(condition); option && *option != 0) {
          output.conditions.emplace_back(*option);
        }
      }
    }
  }
  output.drawCondition = state.bindings.boolean(
      member(destination, "draw"), std::nullopt, authoredOrdinal,
      std::string(path) + ".draw");
  if (const Json *mouse = member(destination, "mouseRect");
      mouse != nullptr && !mouse->is_null()) {
    output.mouseRect = SkinAuthoredRect{
        .x = numberField(*mouse, "x", 0.0, state.context, "JsonSkin.Rect"),
        .y = numberField(*mouse, "y", 0.0, state.context, "JsonSkin.Rect"),
        .width = numberField(*mouse, "w", 0.0, state.context,
                             "JsonSkin.Rect"),
        .height = numberField(*mouse, "h", 0.0, state.context,
                              "JsonSkin.Rect")};
  }

  SkinDestinationFrame current;
  std::optional<int> clipX;
  std::optional<int> clipY;
  std::optional<int> clipWidth;
  std::optional<int> clipHeight;
  if (const Json *frames = member(destination, "dst");
      frames != nullptr && frames->is_array()) {
    output.frames.reserve(frames->size());
    for (const Json &frame : *frames) {
      if (const auto value = inheritedIntegerField(
              frame, "time", state.context, "JsonSkin.Animation"))
        current.timeMillis = *value;
      if (const auto value = inheritedIntegerField(
              frame, "x", state.context, "JsonSkin.Animation"))
        current.x = *value;
      if (const auto value = inheritedIntegerField(
              frame, "y", state.context, "JsonSkin.Animation"))
        current.y = *value;
      if (const auto value = inheritedIntegerField(
              frame, "w", state.context, "JsonSkin.Animation"))
        current.width = *value;
      if (const auto value = inheritedIntegerField(
              frame, "h", state.context, "JsonSkin.Animation"))
        current.height = *value;
      if (const auto value = inheritedIntegerField(
              frame, "angle", state.context, "JsonSkin.Animation"))
        current.angleDegrees = *value;
      if (const auto value = inheritedIntegerField(
              frame, "acc", state.context, "JsonSkin.Animation"))
        current.acceleration = *value;
      std::array<int, 4> rgba{current.rgba[0], current.rgba[1],
                              current.rgba[2], current.rgba[3]};
      const std::array names{"r", "g", "b", "a"};
      for (std::size_t channel = 0; channel < names.size(); ++channel) {
        if (const auto value = inheritedIntegerField(
                frame, names[channel], state.context, "JsonSkin.Animation")) {
          rgba[channel] = *value;
        }
      }
      if (std::ranges::any_of(rgba,
                              [](int value) { return value < 0 || value > 255; })) {
        state.context.error("skin_json_model_invalid",
                            "JsonSkin.Animation color is outside byte range",
                            state.context.source(&frame));
      } else {
        current.rgba = {static_cast<std::uint8_t>(rgba[0]),
                        static_cast<std::uint8_t>(rgba[1]),
                        static_cast<std::uint8_t>(rgba[2]),
                        static_cast<std::uint8_t>(rgba[3])};
      }
      if (const auto value = inheritedIntegerField(
              frame, "clip_x", state.context, "JsonSkin.Animation"))
        clipX = value;
      if (const auto value = inheritedIntegerField(
              frame, "clip_y", state.context, "JsonSkin.Animation"))
        clipY = value;
      if (const auto value = inheritedIntegerField(
              frame, "clip_w", state.context, "JsonSkin.Animation"))
        clipWidth = value;
      if (const auto value = inheritedIntegerField(
              frame, "clip_h", state.context, "JsonSkin.Animation"))
        clipHeight = value;
      if (clipX && clipY && clipWidth && clipHeight) {
        current.clip = SkinSourceRect{.x = *clipX,
                                      .y = *clipY,
                                      .w = *clipWidth,
                                      .h = *clipHeight};
      }
      output.frames.push_back(current);
    }
  }
  if (sortFrames) {
    std::stable_sort(output.frames.begin(), output.frames.end(),
                     [](const auto &left, const auto &right) {
      return left.timeMillis < right.timeMillis;
    });
  }
  return output;
}

std::optional<NumericGlyphAtlas>
numericAtlas(BuildState &state, NumericGlyphAtlasKind kind,
             SkinSpriteFrames source, const Json &definition) {
  NumericGlyphFormatRequest format{
      .integerDigits = integerField(
          definition, kind == NumericGlyphAtlasKind::Number ? "digit" : "iketa",
          0, state.context, kind == NumericGlyphAtlasKind::Number
                                ? "JsonSkin.Value"
                                : "JsonSkin.FloatValue"),
      .fractionalDigits =
          kind == NumericGlyphAtlasKind::Float
              ? integerField(definition, "fketa", 0, state.context,
                             "JsonSkin.FloatValue")
              : 0,
      .zeroPadding = integerField(
          definition, "zeropadding", 0, state.context,
          kind == NumericGlyphAtlasKind::Number ? "JsonSkin.Value"
                                                : "JsonSkin.FloatValue"),
      .numberPadding =
          kind == NumericGlyphAtlasKind::Number
              ? integerField(definition, "padding", 0, state.context,
                             "JsonSkin.Value")
              : 0,
      .signVisible = kind == NumericGlyphAtlasKind::Float &&
                     booleanField(definition, "isSignvisible", false,
                                  state.context, "JsonSkin.FloatValue"),
      .gain = kind == NumericGlyphAtlasKind::Float
                  ? numberField(definition, "gain", 1.0, state.context,
                                "JsonSkin.FloatValue")
                  : 1.0,
      .perDigitOffsets = digitOffsets(
          definition, state.context,
          kind == NumericGlyphAtlasKind::Number ? "JsonSkin.Value"
                                                : "JsonSkin.FloatValue"),
  };
  auto partitioned = partitionNumericGlyphAtlas(
      {.kind = kind, .source = std::move(source), .format = std::move(format)});
  if (!partitioned.atlas) {
    state.context.error("skin_json_model_invalid",
                        "JSON numeric glyph atlas cannot be normalized",
                        state.context.source(&definition));
    return std::nullopt;
  }
  return std::move(partitioned.atlas);
}

SkinObjectPayload buildImage(BuildState &state,
                             const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  SkinSpriteFrames all = spriteForImage(state, reference, "image");
  int stateCount = integerField(definition, "len", 0, state.context,
                                "JsonSkin.Image");
  stateCount = stateCount > 1 ? stateCount : 1;
  if (all.frames.empty() || all.frames.size() %
                                  static_cast<std::size_t>(stateCount) !=
                              0) {
    state.context.error("skin_json_model_invalid",
                        "JsonSkin.Image.len does not partition its frames",
                        state.context.source(member(definition, "len")));
    return SkinImageObject{};
  }
  SkinImageObject output;
  const std::size_t framesPerState =
      all.frames.size() / static_cast<std::size_t>(stateCount);
  for (int stateIndex = 0; stateIndex < stateCount; ++stateIndex) {
    SkinSpriteFrames stateFrames{.resource = all.resource,
                                 .cycleMillis = all.cycleMillis,
                                 .timer = all.timer};
    const auto first = all.frames.begin() +
                       static_cast<std::ptrdiff_t>(stateIndex) *
                           static_cast<std::ptrdiff_t>(framesPerState);
    stateFrames.frames.assign(first,
                              first + static_cast<std::ptrdiff_t>(framesPerState));
    output.orderedStates.push_back(std::move(stateFrames));
  }
  if (stateCount > 1) {
    output.stateIndex = state.bindings.integer(
        member(definition, "ref"),
        integerField(definition, "ref", 0, state.context, "JsonSkin.Image"),
        SkinIntegerPropertyDomain::ImageIndex,
        static_cast<std::uint32_t>(reference.authoredIndex),
        bindingPath("image", reference.authoredIndex, "ref"));
  }
  output.clickEvent = state.bindings.event(
      member(definition, "act"),
      static_cast<std::uint32_t>(reference.authoredIndex),
      bindingPath("image", reference.authoredIndex, "act"));
  output.clickMode = integerField(definition, "click", 0, state.context,
                                  "JsonSkin.Image");
  return output;
}

SkinObjectPayload buildImageSet(BuildState &state,
                                const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  SkinImageObject output;
  output.stateIndex = state.bindings.integer(
      member(definition, "value"),
      integerField(definition, "ref", 0, state.context, "JsonSkin.ImageSet"),
      SkinIntegerPropertyDomain::ImageIndex,
      static_cast<std::uint32_t>(reference.authoredIndex),
      bindingPath("imageset", reference.authoredIndex, "value"));
  output.clickEvent = state.bindings.event(
      member(definition, "act"),
      static_cast<std::uint32_t>(reference.authoredIndex),
      bindingPath("imageset", reference.authoredIndex, "act"));
  output.clickMode = integerField(definition, "click", 0, state.context,
                                  "JsonSkin.ImageSet");
  for (const auto &id : stringArrayField(definition, "images", state.context,
                                         "JsonSkin.ImageSet")) {
    output.orderedStates.push_back(spriteForImageId(state, id));
  }
  return output;
}

SkinObjectPayload buildNumber(BuildState &state,
                              const DefinitionReference &reference,
                              bool judgeRelative = false) {
  const Json &definition = *reference.value;
  auto atlas = numericAtlas(state, NumericGlyphAtlasKind::Number,
                            spriteForImage(state, reference, "value"),
                            definition);
  SkinNumberObject output;
  if (atlas) {
    output.digits = std::move(atlas->digits);
    output.digitCount = atlas->format.integerDigits;
    output.zeroPadding = atlas->format.zeroPadding;
    output.perDigitOffsets = std::move(atlas->format.perDigitOffsets);
  }
  output.value = state.bindings
                     .integer(member(definition, "value"),
                              integerField(definition, "ref", 0,
                                           state.context, "JsonSkin.Value"),
                              SkinIntegerPropertyDomain::IntegerValue,
                              static_cast<std::uint32_t>(reference.authoredIndex),
                              bindingPath("value", reference.authoredIndex,
                                          "value"))
                     .value_or(SkinIntegerPropertyId{});
  output.spacing = integerField(definition, "space", 0, state.context,
                                "JsonSkin.Value");
  output.alignment = judgeRelative
                         ? 2
                         : integerField(definition, "align", 0, state.context,
                                        "JsonSkin.Value");
  output.relativeToJudgeImage = judgeRelative;
  return output;
}

SkinObjectPayload buildFloat(BuildState &state,
                             const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  auto atlas = numericAtlas(state, NumericGlyphAtlasKind::Float,
                            spriteForImage(state, reference, "floatvalue"),
                            definition);
  SkinFloatObject output;
  if (atlas) {
    output.digits = std::move(atlas->digits);
    output.integerDigits = atlas->format.integerDigits;
    output.fractionalDigits = atlas->format.fractionalDigits;
    output.zeroPadding = atlas->format.zeroPadding;
    output.signVisible = atlas->format.signVisible;
    output.gain = atlas->format.gain;
    output.perDigitOffsets = std::move(atlas->format.perDigitOffsets);
  }
  output.value = state.bindings
                     .floating(member(definition, "value"),
                               integerField(definition, "ref", 0,
                                            state.context,
                                            "JsonSkin.FloatValue"),
                               SkinFloatPropertyDomain::FloatValue,
                               static_cast<std::uint32_t>(reference.authoredIndex),
                               bindingPath("floatvalue", reference.authoredIndex,
                                           "value"))
                     .value_or(SkinFloatPropertyId{});
  output.spacing = integerField(definition, "space", 0, state.context,
                                "JsonSkin.FloatValue");
  output.alignment = integerField(definition, "align", 0, state.context,
                                  "JsonSkin.FloatValue");
  return output;
}

SkinObjectPayload buildText(BuildState &state,
                            const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const std::string fontName = stringField(definition, "font", {},
                                           state.context, "JsonSkin.Text");
  const auto font = state.fontIds.find(fontName);
  SkinTextObject output{
      .font = font == state.fontIds.end() ? 0 : font->second,
      .value = state.bindings.string(
          member(definition, "value"),
          integerField(definition, "ref", 0, state.context, "JsonSkin.Text"),
          static_cast<std::uint32_t>(reference.authoredIndex),
          bindingPath("text", reference.authoredIndex, "value")),
      .literal = stringField(definition, "constantText", {}, state.context,
                             "JsonSkin.Text"),
      .pointSize = integerField(definition, "size", 0, state.context,
                                "JsonSkin.Text"),
      .alignment = integerField(definition, "align", 0, state.context,
                                "JsonSkin.Text"),
      .wrapping = booleanField(definition, "wrapping", false, state.context,
                               "JsonSkin.Text"),
      .overflow = integerField(definition, "overflow", 0, state.context,
                               "JsonSkin.Text"),
      .outlineRgba = textColor(stringField(definition, "outlineColor",
                                            "ffffff00", state.context,
                                            "JsonSkin.Text")),
      .outlineWidth = numberField(definition, "outlineWidth", 0.0,
                                  state.context, "JsonSkin.Text"),
      .shadowRgba = textColor(stringField(definition, "shadowColor",
                                           "ffffff00", state.context,
                                           "JsonSkin.Text")),
      .shadowOffsetX = numberField(definition, "shadowOffsetX", 0.0,
                                   state.context, "JsonSkin.Text"),
      .shadowOffsetY = numberField(definition, "shadowOffsetY", 0.0,
                                   state.context, "JsonSkin.Text"),
      .shadowSmoothness = numberField(definition, "shadowSmoothness", 0.0,
                                      state.context, "JsonSkin.Text"),
      .editable = booleanField(definition, "editable", false, state.context,
                               "JsonSkin.Text"),
  };
  const Json *event = member(definition, "event");
  const bool implicitWriter = event == nullptr || event->is_null();
  output.writer = state.bindings.stringWriter(
      event,
      implicitWriter
          ? std::optional<int>(integerField(definition, "ref", 0,
                                            state.context, "JsonSkin.Text"))
          : std::nullopt,
      static_cast<std::uint32_t>(reference.authoredIndex),
      bindingPath("text", reference.authoredIndex, "event"));
  if (implicitWriter && output.writer) {
    output.editable = true;
  }
  return output;
}

SkinObjectPayload buildSlider(BuildState &state,
                              const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  SkinSliderObject output;
  output.knob = spriteForImage(state, reference, "slider");
  const bool isRefNum = booleanField(definition, "isRefNum", false,
                                     state.context, "JsonSkin.Slider");
  if (member(definition, "value") != nullptr &&
      !member(definition, "value")->is_null()) {
    output.value = state.bindings
                       .floating(member(definition, "value"), std::nullopt,
                                 SkinFloatPropertyDomain::Rate,
                                 static_cast<std::uint32_t>(reference.authoredIndex),
                                 bindingPath("slider", reference.authoredIndex,
                                             "value"))
                       .value_or(SkinFloatPropertyId{});
    output.writer = state.bindings.floatWriter(
        member(definition, "event"), std::nullopt,
        static_cast<std::uint32_t>(reference.authoredIndex),
        bindingPath("slider", reference.authoredIndex, "event"));
  } else if (isRefNum) {
    const int type = integerField(definition, "type", 0, state.context,
                                  "JsonSkin.Slider");
    output.value = SkinSliderObject::IntegerRangeSource{
        .value = state.bindings
                     .integer(nullptr, type,
                              SkinIntegerPropertyDomain::IntegerValue,
                              static_cast<std::uint32_t>(reference.authoredIndex),
                              bindingPath("slider", reference.authoredIndex,
                                          "type"))
                     .value_or(SkinIntegerPropertyId{}),
        .minimum = integerField(definition, "min", 0, state.context,
                                "JsonSkin.Slider"),
        .maximum = integerField(definition, "max", 0, state.context,
                                "JsonSkin.Slider")};
  } else {
    const int type = integerField(definition, "type", 0, state.context,
                                  "JsonSkin.Slider");
    output.value = state.bindings
                       .floating(nullptr, type, SkinFloatPropertyDomain::Rate,
                                 static_cast<std::uint32_t>(reference.authoredIndex),
                                 bindingPath("slider", reference.authoredIndex,
                                             "type"))
                       .value_or(SkinFloatPropertyId{});
    if (booleanField(definition, "changeable", true, state.context,
                     "JsonSkin.Slider")) {
      output.writer = state.bindings.floatWriter(
          member(definition, "event"), type,
          static_cast<std::uint32_t>(reference.authoredIndex),
          bindingPath("slider", reference.authoredIndex, "event"));
    }
  }
  output.direction = integerField(definition, "angle", 0, state.context,
                                  "JsonSkin.Slider");
  output.range = integerField(definition, "range", 0, state.context,
                              "JsonSkin.Slider");
  output.changeable = booleanField(definition, "changeable", true,
                                   state.context, "JsonSkin.Slider");
  return output;
}

SkinObjectPayload buildGraph(BuildState &state,
                             const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const int type = integerField(definition, "type", 0, state.context,
                                "JsonSkin.Graph");
  if (type < 0) {
    state.context.warning(
        "skin_json_distribution_graph_invalid_in_gameplay",
        "negative generic Graph creates a select-only distribution object "
        "that is invalid in BMSPlayer",
        state.context.source(member(definition, "type")));
    return SkinInvalidInGameplayObject{
        .kind = SkinInvalidInGameplayKind::SelectDistributionGraph};
  }
  SkinGraphObject output;
  output.fill = spriteForImage(state, reference, "graph");
  if (member(definition, "value") != nullptr &&
             !member(definition, "value")->is_null()) {
    output.value = state.bindings
                       .floating(member(definition, "value"), std::nullopt,
                                 SkinFloatPropertyDomain::Rate,
                                 static_cast<std::uint32_t>(reference.authoredIndex),
                                 bindingPath("graph", reference.authoredIndex,
                                             "value"))
                       .value_or(SkinFloatPropertyId{});
  } else if (booleanField(definition, "isRefNum", false, state.context,
                          "JsonSkin.Graph")) {
    output.value = SkinSliderObject::IntegerRangeSource{
        .value = state.bindings
                     .integer(nullptr, type,
                              SkinIntegerPropertyDomain::IntegerValue,
                              static_cast<std::uint32_t>(reference.authoredIndex),
                              bindingPath("graph", reference.authoredIndex,
                                          "type"))
                     .value_or(SkinIntegerPropertyId{}),
        .minimum = integerField(definition, "min", 0, state.context,
                                "JsonSkin.Graph"),
        .maximum = integerField(definition, "max", 0, state.context,
                                "JsonSkin.Graph")};
  } else {
    output.value = state.bindings
                       .floating(nullptr, type, SkinFloatPropertyDomain::Rate,
                                 static_cast<std::uint32_t>(reference.authoredIndex),
                                 bindingPath("graph", reference.authoredIndex,
                                             "type"))
                       .value_or(SkinFloatPropertyId{});
  }
  output.direction = integerField(definition, "angle", 1, state.context,
                                  "JsonSkin.Graph");
  return output;
}

SkinObjectPayload buildGaugeGraph(BuildState &state,
                                  const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  SkinGaugeGraphObject output;
  if (const Json *colors = member(definition, "color");
      colors != nullptr && !colors->is_null()) {
    for (auto &row : output.rgba) row.fill(0x000000ffU);
    if (!colors->is_array()) {
      state.context.error("skin_json_model_invalid",
                          "JsonSkin.GaugeGraph.color is not an array",
                          state.context.source(colors));
      return output;
    }
    const std::size_t count = std::min<std::size_t>(24, colors->size());
    for (std::size_t index = 0; index < count; ++index) {
      if (!(*colors)[index].is_string()) continue;
      const auto decoded =
          directColor((*colors)[index].get_ref<const std::string &>());
      if (!decoded) {
        state.context.error("skin_json_model_invalid",
                            "JsonSkin.GaugeGraph.color contains invalid color",
                            state.context.source(&(*colors)[index]));
      } else {
        output.rgba[index / 4][index % 4] = *decoded;
      }
    }
    return output;
  }
  const auto color = [&](std::string_view field, std::string fallback) {
    return colorOr(stringField(definition, field, std::move(fallback),
                               state.context, "JsonSkin.GaugeGraph"),
                   0x000000ffU);
  };
  const auto borderLine = color("borderlineColor", "ff0000");
  const auto border = color("borderColor", "440000");
  const auto assistLine = color("assistClearLineColor", "ff00ff");
  const auto assist = color("assistClearBGColor", "440044");
  const auto easyLine = color("assistAndEasyFailLineColor", "00ffff");
  const auto easy = color("assistAndEasyFailBGColor", "004444");
  const auto grooveFailLine = color("grooveFailLineColor", "00ff00");
  const auto grooveFail = color("grooveFailBGColor", "004400");
  const auto grooveLine = color("grooveClearAndHardLineColor", "ff0000");
  const auto groove = color("grooveClearAndHardBGColor", "440000");
  const auto exLine = color("exHardLineColor", "ffff00");
  const auto ex = color("exHardBGColor", "444400");
  const auto hazardLine = color("hazardLineColor", "cccccc");
  const auto hazard = color("hazardBGColor", "444444");
  output.rgba = {{{borderLine, border, assistLine, assist},
                  {borderLine, border, easyLine, easy},
                  {borderLine, border, grooveFailLine, grooveFail},
                  {grooveLine, groove, grooveLine, groove},
                  {exLine, ex, exLine, ex},
                  {hazardLine, hazard, hazardLine, hazard}}};
  return output;
}

SkinObjectPayload buildJudgeGraph(BuildState &state,
                                  const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const int rawType = integerField(definition, "type", 0, state.context,
                                   "JsonSkin.JudgeGraph");
  if (rawType < 0 || rawType > 2) {
    state.context.error("skin_json_model_invalid",
                        "JsonSkin.JudgeGraph.type is outside the pinned range",
                        state.context.source(member(definition, "type")));
  }
  return SkinNoteDistributionGraphObject{
      .type = static_cast<SkinNoteDistributionGraphType>(
          std::clamp(rawType, 0, 2)),
      .backgroundTextureOff =
          integerField(definition, "backTexOff", 0, state.context,
                       "JsonSkin.JudgeGraph") == 1,
      .delayMillis = integerField(definition, "delay", 500, state.context,
                                  "JsonSkin.JudgeGraph"),
      .reverseOrder = integerField(definition, "orderReverse", 0,
                                   state.context, "JsonSkin.JudgeGraph") == 1,
      .noGap = integerField(definition, "noGap", 0, state.context,
                            "JsonSkin.JudgeGraph") == 1,
      .noHorizontalGap = integerField(definition, "noGapX", 0, state.context,
                                      "JsonSkin.JudgeGraph") == 1};
}

SkinObjectPayload buildBpmGraph(BuildState &state,
                                const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const auto rgb = [&](std::string_view field, std::string fallback,
                       std::uint32_t defaultColor) {
    return colorOr(stringField(definition, field, std::move(fallback),
                               state.context, "JsonSkin.BPMGraph"),
                   defaultColor);
  };
  const int delay = integerField(definition, "delay", 0, state.context,
                                 "JsonSkin.BPMGraph");
  const int width = integerField(definition, "lineWidth", 2, state.context,
                                 "JsonSkin.BPMGraph");
  return SkinBpmGraphObject{
      .delayMillis = std::max(delay, 0),
      .lineWidth = width > 0 ? width : 2,
      .mainRgba = rgb("mainBPMColor", "00ff00", 0x00ff00ffU),
      .minimumRgba = rgb("minBPMColor", "0000ff", 0x0000ffffU),
      .maximumRgba = rgb("maxBPMColor", "ff0000", 0xff0000ffU),
      .otherRgba = rgb("otherBPMColor", "ffff00", 0xffff00ffU),
      .stopRgba = rgb("stopLineColor", "ff00ff", 0xff00ffffU),
      .transitionRgba =
          rgb("transitionLineColor", "7f7f7f", 0x7f7f7fffU)};
}

std::uint32_t visualizerColor(const Json &definition, std::string_view field,
                              std::string fallback, DecodeContext &context,
                              std::string_view surface) {
  const std::string value = stringField(definition, field, std::move(fallback),
                                        context, surface);
  return colorOr(value, 0xff0000ffU);
}

SkinObjectPayload buildTimingVisualizer(
    BuildState &state, const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const bool transparent =
      integerField(definition, "transparent", 0, state.context,
                   "JsonSkin.TimingVisualizer") == 1;
  return SkinTimingVisualizerObject{
      .width = integerField(definition, "width", 301, state.context,
                            "JsonSkin.TimingVisualizer"),
      .judgeWidthMillis = integerField(
          definition, "judgeWidthMillis", 150, state.context,
          "JsonSkin.TimingVisualizer"),
      .lineWidth = std::clamp(
          integerField(definition, "lineWidth", 1, state.context,
                       "JsonSkin.TimingVisualizer"),
          1, 4),
      .judgeRgba = {
          visualizerColor(definition, "PGColor", "000088FF", state.context,
                          "JsonSkin.TimingVisualizer"),
          visualizerColor(definition, "GRColor", "008800FF", state.context,
                          "JsonSkin.TimingVisualizer"),
          visualizerColor(definition, "GDColor", "888800FF", state.context,
                          "JsonSkin.TimingVisualizer"),
          visualizerColor(definition, "BDColor", "880000FF", state.context,
                          "JsonSkin.TimingVisualizer"),
          transparent ? 0U
                      : visualizerColor(definition, "PRColor", "000000FF",
                                        state.context,
                                        "JsonSkin.TimingVisualizer")},
      .lineRgba = visualizerColor(definition, "lineColor", "00FF00FF",
                                  state.context,
                                  "JsonSkin.TimingVisualizer"),
      .centerRgba = visualizerColor(definition, "centerColor", "FFFFFFFF",
                                    state.context,
                                    "JsonSkin.TimingVisualizer"),
      .transparent = transparent,
      .drawDecay = integerField(definition, "drawDecay", 1, state.context,
                                "JsonSkin.TimingVisualizer") == 1};
}

SkinObjectPayload buildTimingDistribution(
    BuildState &state, const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const int width = std::max(
      1, integerField(definition, "width", 301, state.context,
                      "JsonSkin.TimingDistributionGraph"));
  int lineWidth = integerField(definition, "lineWidth", 1, state.context,
                               "JsonSkin.TimingDistributionGraph");
  lineWidth = std::clamp(lineWidth, 1, width);
  return SkinTimingDistributionGraphObject{
      .width = width,
      .lineWidth = lineWidth,
      .graphRgba = visualizerColor(
          definition, "graphColor", "00FF00FF", state.context,
          "JsonSkin.TimingDistributionGraph"),
      .averageRgba = visualizerColor(
          definition, "averageColor", "FFFFFFFF", state.context,
          "JsonSkin.TimingDistributionGraph"),
      .devRgba = visualizerColor(definition, "devColor", "FFFFFFFF",
                                 state.context,
                                 "JsonSkin.TimingDistributionGraph"),
      .judgeRgba = {
          visualizerColor(definition, "PGColor", "000088FF", state.context,
                          "JsonSkin.TimingDistributionGraph"),
          visualizerColor(definition, "GRColor", "008800FF", state.context,
                          "JsonSkin.TimingDistributionGraph"),
          visualizerColor(definition, "GDColor", "888800FF", state.context,
                          "JsonSkin.TimingDistributionGraph"),
          visualizerColor(definition, "BDColor", "880000FF", state.context,
                          "JsonSkin.TimingDistributionGraph"),
          visualizerColor(definition, "PRColor", "000000FF", state.context,
                          "JsonSkin.TimingDistributionGraph")},
      .drawAverage = integerField(definition, "drawAverage", 1, state.context,
                                  "JsonSkin.TimingDistributionGraph") == 1,
      .drawDev = integerField(definition, "drawDev", 1, state.context,
                              "JsonSkin.TimingDistributionGraph") == 1};
}

SkinObjectPayload buildHitError(BuildState &state,
                                const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const bool transparent =
      integerField(definition, "transparent", 0, state.context,
                   "JsonSkin.HitErrorVisualizer") == 1;
  return SkinHitErrorVisualizerObject{
      .width = integerField(definition, "width", 301, state.context,
                            "JsonSkin.HitErrorVisualizer"),
      .judgeWidthMillis = integerField(
          definition, "judgeWidthMillis", 150, state.context,
          "JsonSkin.HitErrorVisualizer"),
      .lineWidth = std::clamp(
          integerField(definition, "lineWidth", 1, state.context,
                       "JsonSkin.HitErrorVisualizer"),
          1, 4),
      .colorMode = integerField(definition, "colorMode", 1, state.context,
                                "JsonSkin.HitErrorVisualizer") == 1,
      .hitErrorMode = integerField(definition, "hiterrorMode", 1,
                                   state.context,
                                   "JsonSkin.HitErrorVisualizer") == 1,
      .emaMode = integerField(definition, "emaMode", 1, state.context,
                              "JsonSkin.HitErrorVisualizer"),
      .judgeRgba = {
          visualizerColor(definition, "PGColor", "99CCFF80", state.context,
                          "JsonSkin.HitErrorVisualizer"),
          visualizerColor(definition, "GRColor", "F2CB3080", state.context,
                          "JsonSkin.HitErrorVisualizer"),
          visualizerColor(definition, "GDColor", "14CC8f80", state.context,
                          "JsonSkin.HitErrorVisualizer"),
          visualizerColor(definition, "BDColor", "FF1AB380", state.context,
                          "JsonSkin.HitErrorVisualizer"),
          transparent ? 0U
                      : visualizerColor(definition, "PRColor", "CC292980",
                                        state.context,
                                        "JsonSkin.HitErrorVisualizer")},
      .lineRgba = visualizerColor(definition, "lineColor", "99CCFF80",
                                  state.context,
                                  "JsonSkin.HitErrorVisualizer"),
      .centerRgba = visualizerColor(definition, "centerColor", "FFFFFFFF",
                                    state.context,
                                    "JsonSkin.HitErrorVisualizer"),
      .emaRgba = visualizerColor(definition, "emaColor", "FF0000FF",
                                 state.context,
                                 "JsonSkin.HitErrorVisualizer"),
      .alpha = static_cast<float>(numberField(
          definition, "alpha", 0.1, state.context,
          "JsonSkin.HitErrorVisualizer")),
      .windowLength = std::clamp(
          integerField(definition, "windowLength", 30, state.context,
                       "JsonSkin.HitErrorVisualizer"),
          1, 100),
      .transparent = transparent,
      .drawDecay = integerField(definition, "drawDecay", 1, state.context,
                                "JsonSkin.HitErrorVisualizer") == 1};
}

SkinObjectPayload buildGauge(BuildState &state,
                             const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  SkinGaugeNodeExpansionInput input{
      .nodes = stringArrayField(definition, "nodes", state.context,
                                "JsonSkin.Gauge"),
      .parts = integerField(definition, "parts", 50, state.context,
                            "JsonSkin.Gauge"),
      .animationType = integerField(definition, "type", 0, state.context,
                                    "JsonSkin.Gauge"),
      .animationRange = integerField(definition, "range", 3, state.context,
                                     "JsonSkin.Gauge"),
      .animationCycleMillis = integerField(
          definition, "cycle", 33, state.context, "JsonSkin.Gauge"),
      .resultStartMillis = integerField(
          definition, "starttime", 0, state.context, "JsonSkin.Gauge"),
      .resultEndMillis = integerField(definition, "endtime", 500,
                                      state.context, "JsonSkin.Gauge")};
  input.images.reserve(state.images.size());
  for (const auto &[name, image] : state.images) {
    input.images.push_back(
        {.id = name, .sprite = spriteForImage(state, image, "image")});
  }
  auto expanded = expandSkinGaugeNodes(input);
  if (!expanded.gauge) {
    state.context.error("skin_json_model_invalid",
                        "JsonSkin.Gauge nodes cannot be expanded",
                        state.context.source(&definition));
    return SkinGaugeObject{};
  }
  return std::move(*expanded.gauge);
}

SkinAuthoredNoteVisualSlots noteVisuals(BuildState &state,
                                       const Json &note,
                                       std::string_view field) {
  SkinAuthoredNoteVisualSlots result;
  for (const auto &id :
       stringArrayField(note, field, state.context, "JsonSkin.NoteSet")) {
    SkinSpriteFrames sprite = spriteForImageId(state, id);
    result.push_back(sprite.resource != 0 && !sprite.frames.empty()
                         ? std::optional<SkinSpriteFrames>(std::move(sprite))
                         : std::nullopt);
  }
  return result;
}

std::optional<SkinAuthoredNoteVisualSlots>
optionalNoteVisuals(BuildState &state, const Json &note,
                    std::string_view field) {
  if (member(note, field) == nullptr || member(note, field)->is_null()) {
    return std::nullopt;
  }
  return noteVisuals(state, note, field);
}

SkinAuthoredRect firstDestinationRect(BuildState &state,
                                      const Json &destination) {
  const Json *frames = member(destination, "dst");
  if (frames == nullptr || !frames->is_array() || frames->empty()) return {};
  const Json &frame = frames->front();
  return {.x = static_cast<double>(integerField(
              frame, "x", 0, state.context, "JsonSkin.Animation")),
          .y = static_cast<double>(integerField(
              frame, "y", 0, state.context, "JsonSkin.Animation")),
          .width = static_cast<double>(integerField(
              frame, "w", 0, state.context, "JsonSkin.Animation")),
          .height = static_cast<double>(integerField(
              frame, "h", 0, state.context, "JsonSkin.Animation"))};
}

SkinObjectPayload buildNote(BuildState &state,
                            const DefinitionReference &reference) {
  const Json &note = *reference.value;
  SkinNoteNormalizationInput input{
      .note = noteVisuals(state, note, "note"),
      .mine = noteVisuals(state, note, "mine"),
      .lnEnd = noteVisuals(state, note, "lnend"),
      .lnStart = noteVisuals(state, note, "lnstart"),
      .lnBody = noteVisuals(state, note, "lnbody"),
      .lnActive = noteVisuals(state, note, "lnactive"),
      .lnBodyActive = optionalNoteVisuals(state, note, "lnbodyActive"),
      .hcnEnd = noteVisuals(state, note, "hcnend"),
      .hcnStart = noteVisuals(state, note, "hcnstart"),
      .hcnBody = noteVisuals(state, note, "hcnbody"),
      .hcnActive = noteVisuals(state, note, "hcnactive"),
      .hcnDamage = noteVisuals(state, note, "hcndamage"),
      .hcnReactive = noteVisuals(state, note, "hcnreactive"),
      .hcnBodyActive = optionalNoteVisuals(state, note, "hcnbodyActive"),
      .hcnBodyReactive = noteVisuals(state, note, "hcnbodyReactive"),
      .hcnBodyMiss = noteVisuals(state, note, "hcnbodyMiss")};
  auto normalized = normalizeSkinNote(input);
  if (!normalized.note) {
    state.context.error("skin_json_model_invalid",
                        "JsonSkin.NoteSet visual arrays cannot be normalized",
                        state.context.source(&note));
    return SkinNoteObject{};
  }
  SkinNoteObject output;
  output.hcnBodySlotLayout = normalized.note->hcnBodySlotLayout;
  const Json *rects = member(note, "dst");
  const Json *sizes = member(note, "size");
  std::optional<int> secondary;
  if (const Json *dst2 = member(note, "dst2"); dst2 != nullptr) {
    secondary = integerValue(*dst2);
    if (secondary == kJavaIntegerMinimum) secondary.reset();
  }
  for (std::size_t laneIndex = 0; laneIndex < normalized.note->lanes.size();
       ++laneIndex) {
    SkinLaneNotePresentation lane;
    lane.authoredLane = static_cast<int>(laneIndex);
    if (rects != nullptr && rects->is_array() && laneIndex < rects->size()) {
      const Json &rect = (*rects)[laneIndex];
      lane.laneDestination =
          {.x = static_cast<double>(integerField(
               rect, "x", 0, state.context, "JsonSkin.Animation")),
           .y = static_cast<double>(integerField(
               rect, "y", 0, state.context, "JsonSkin.Animation")),
           .width = static_cast<double>(integerField(
               rect, "w", 0, state.context, "JsonSkin.Animation")),
           .height = static_cast<double>(integerField(
               rect, "h", 0, state.context, "JsonSkin.Animation"))};
    }
    if (sizes != nullptr && sizes->is_array() && laneIndex < sizes->size() &&
        (*sizes)[laneIndex].is_number()) {
      lane.authoredNoteHeight = (*sizes)[laneIndex].get<double>();
    } else if (const auto *sprite = std::get_if<SkinSpriteFrames>(
                   &normalized.note->lanes[laneIndex].visuals[0]);
               sprite != nullptr && !sprite->frames.empty()) {
      lane.authoredNoteHeight = sprite->frames.front().h;
    }
    lane.secondaryDestinationY = secondary;
    for (std::size_t visual = 0;
         visual < normalized.note->lanes[laneIndex].visuals.size(); ++visual) {
      const auto kind = static_cast<SkinNoteVisualKind>(visual);
      if (const auto *sprite = std::get_if<SkinSpriteFrames>(
              &normalized.note->lanes[laneIndex].visuals[visual])) {
        lane.visuals.emplace(kind, *sprite);
      } else {
        lane.visuals.emplace(kind, SkinSynthesizedNoteVisual{.kind = kind});
      }
    }
    output.lanes.push_back(std::move(lane));
  }
  if (const Json *rate = member(note, "expansionrate");
      rate != nullptr && rate->is_array() && rate->size() == 2) {
    output.expansionRatePercent = {
        integerValue((*rate)[0]).value_or(100),
        integerValue((*rate)[1]).value_or(100)};
  }
  const Json *groups = member(note, "group");
  const std::size_t groupCount =
      groups != nullptr && groups->is_array() ? groups->size() : 0;
  for (const auto &[field, kind] :
       std::array<std::pair<std::string_view, SkinNoteLineKind>, 4>{
           {{"group", SkinNoteLineKind::Group},
            {"bpm", SkinNoteLineKind::Bpm},
            {"stop", SkinNoteLineKind::Stop},
            {"time", SkinNoteLineKind::Time}}}) {
    const Json *lines = member(note, field);
    if (lines == nullptr || !lines->is_array()) continue;
    const std::size_t count = field == "group"
                                  ? lines->size()
                                  : std::min(groupCount, lines->size());
    for (std::size_t index = 0; index < count; ++index) {
      const Json &line = (*lines)[index];
      const std::string id = stringField(line, "id", {}, state.context,
                                         "JsonSkin.Destination");
      SkinSpriteFrames sprite = spriteForImageId(state, id);
      SkinNoteLinePresentation presentation{
          .kind = kind,
          .sprite = sprite.resource != 0
                        ? std::optional<SkinSpriteFrames>(std::move(sprite))
                        : std::nullopt,
          .laneGroupDestination =
              groups != nullptr && index < groups->size()
                  ? firstDestinationRect(state, (*groups)[index])
                  : SkinAuthoredRect{},
          .destination = decodeDestinationBody(
              state, line, static_cast<std::uint32_t>(index),
              std::string("note.") + std::string(field) + "[" +
                  std::to_string(index + 1) + "]",
              false)};
      output.lines.push_back(std::move(presentation));
    }
  }
  if (!stringArrayField(note, "hidden", state.context, "JsonSkin.NoteSet")
           .empty() ||
      !stringArrayField(note, "processed", state.context,
                        "JsonSkin.NoteSet")
           .empty()) {
    state.context.warning(
        "skin_json_model_authored_note_visual_ignored",
        "Pinned Beatoraja ignores authored hidden and processed note images");
  }
  return output;
}

SkinObjectPayload buildCover(BuildState &state,
                             const DefinitionReference &reference,
                             SkinCoverKind kind) {
  const Json &definition = *reference.value;
  const auto normalized = normalizeSkinCover(
      {.kind = kind,
       .sprite = spriteForImage(
           state, reference,
           kind == SkinCoverKind::Hidden ? "hiddenCover" : "liftCover"),
       .authoredDisappearLine = static_cast<double>(integerField(
           definition, "disapearLine", -1, state.context,
           kind == SkinCoverKind::Hidden ? "JsonSkin.HiddenCover"
                                         : "JsonSkin.LiftCover")),
       .authoredDisappearLineLinksLift = booleanField(
           definition, "isDisapearLineLinkLift",
           kind == SkinCoverKind::Hidden, state.context,
           kind == SkinCoverKind::Hidden ? "JsonSkin.HiddenCover"
                                         : "JsonSkin.LiftCover"),
       .lineScale = 1.0});
  if (!normalized.cover) {
    state.context.error("skin_json_model_invalid",
                        "JSON cover object cannot be normalized",
                        state.context.source(&definition));
    return SkinCoverObject{.kind = kind};
  }
  return *normalized.cover;
}

SkinObjectPayload buildPractice(BuildState &state,
                                const DefinitionReference &reference) {
  return SkinPracticeObject{.visibleItems = std::clamp(
                                integerField(*reference.value, "visibleItems",
                                             10, state.context,
                                             "JsonSkin.Practice"),
                                0, 16)};
}

SkinObjectPayload buildPmChara(BuildState &state,
                               const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  const std::string source = stringField(definition, "src", {}, state.context,
                                         "JsonSkin.PMchara");
  const auto id = state.sourceIds.find(source);
  const auto path = state.sourcePaths.find(source);
  return SkinPmCharaObject{
      .source = id == state.sourceIds.end() ? 0 : id->second,
      .sourceName = source,
      .sourcePath = path == state.sourcePaths.end() ? std::string{}
                                                    : path->second,
      .color = integerField(definition, "color", 1, state.context,
                            "JsonSkin.PMchara") == 2
                   ? 2
                   : 1,
      .type = integerField(definition, "type", kJavaIntegerMinimum,
                           state.context, "JsonSkin.PMchara"),
      .side = integerField(definition, "side", 1, state.context,
                           "JsonSkin.PMchara") == 2
                  ? 2
                  : 1};
}

SkinObjectPayload buildJudge(BuildState &state,
                             const DefinitionReference &reference) {
  const Json &definition = *reference.value;
  SkinJudgeObject output{
      .player = integerField(definition, "index", 0, state.context,
                             "JsonSkin.Judge"),
      .shiftImageByHalfDetailWidth = booleanField(
          definition, "shift", false, state.context, "JsonSkin.Judge")};
  const Json *images = member(definition, "images");
  const Json *numbers = member(definition, "numbers");
  const std::size_t count = std::max(
      images != nullptr && images->is_array() ? images->size() : 0,
      numbers != nullptr && numbers->is_array() ? numbers->size() : 0);
  output.grades.resize(std::max<std::size_t>(6, count));
  for (std::size_t grade = 0; grade < count; ++grade) {
    if (images != nullptr && images->is_array() && grade < images->size()) {
      const Json &child = (*images)[grade];
      const std::string name = stringField(child, "id", {}, state.context,
                                           "JsonSkin.Destination");
      if (const auto found = state.images.find(name);
          found != state.images.end()) {
        const SkinObjectId id = state.nextSyntheticObjectId++;
        SkinImageObject childImage{
            .orderedStates = {spriteForImage(state, found->second, "image")}};
        state.nestedObjects.push_back(
            {.id = id,
             .authoredName = "__judge/" +
                             stringField(definition, "id", {}, state.context,
                                         "JsonSkin.Judge") +
                             "/image/" + std::to_string(grade),
             .payload = std::move(childImage),
             .authoredOrdinal = static_cast<std::uint32_t>(grade),
             .critical = false,
             .source = state.context.source(found->second.value).value_or(
                 SkinSourceLocation{.virtualPath =
                                        state.context.entry.packageRelativePath})});
        output.grades[grade].image = SkinNestedObjectPresentation{
            .object = id,
            .destination = decodeDestinationBody(
                state, child, static_cast<std::uint32_t>(grade),
                "judge.images[" + std::to_string(grade + 1) + "]"),
            .source = state.context.source(&child).value_or(
                SkinSourceLocation{.virtualPath =
                                       state.context.entry.packageRelativePath})};
      }
    }
    if (numbers != nullptr && numbers->is_array() && grade < numbers->size()) {
      const Json &child = (*numbers)[grade];
      const std::string name = stringField(child, "id", {}, state.context,
                                           "JsonSkin.Destination");
      if (const auto found = state.values.find(name);
          found != state.values.end()) {
        const SkinObjectId id = state.nextSyntheticObjectId++;
        SkinNumberObject childNumber =
            std::get<SkinNumberObject>(buildNumber(state, found->second, true));
        SkinDestinationBody destination = decodeDestinationBody(
            state, child, static_cast<std::uint32_t>(grade),
            "judge.numbers[" + std::to_string(grade + 1) + "]");
        const int digits = integerField(*found->second.value, "digit", 0,
                                        state.context, "JsonSkin.Value");
        for (auto &frame : destination.frames) {
          frame.x -= frame.width * digits / 2.0;
        }
        state.nestedObjects.push_back(
            {.id = id,
             .authoredName = "__judge/" +
                             stringField(definition, "id", {}, state.context,
                                         "JsonSkin.Judge") +
                             "/number/" + std::to_string(grade),
             .payload = std::move(childNumber),
             .authoredOrdinal = static_cast<std::uint32_t>(grade),
             .critical = false,
             .source = state.context.source(found->second.value).value_or(
                 SkinSourceLocation{.virtualPath =
                                        state.context.entry.packageRelativePath})});
        output.grades[grade].detailNumber = SkinNestedObjectPresentation{
            .object = id,
            .destination = std::move(destination),
            .source = state.context.source(&child).value_or(
                SkinSourceLocation{.virtualPath =
                                       state.context.entry.packageRelativePath})};
      }
    }
  }
  return output;
}

std::optional<int> destinationInteger(std::string_view text) {
  int value = 0;
  return parseIntegerString(text, value) ? std::optional<int>(value)
                                         : std::nullopt;
}

std::optional<SkinObjectResolutionKind>
resolvedKind(BuildState &state, std::string_view name) {
  const auto matches = [&](const DefinitionMap &definitions) {
    return definitions.contains(name);
  };
  const bool note = state.note &&
                    stringField(*state.note->value, "id", {}, state.context,
                                "JsonSkin.NoteSet") == name;
  const bool gauge = state.gauge &&
                     stringField(*state.gauge->value, "id", {}, state.context,
                                 "JsonSkin.Gauge") == name;
  const bool practice =
      state.practice &&
      stringField(*state.practice->value, "id", {}, state.context,
                  "JsonSkin.Practice") == name;
  const bool bga = state.bga &&
                   stringField(*state.bga->value, "id", {}, state.context,
                               "JsonSkin.BGA") == name;
  const std::array candidates{
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Image,
                                    .matches = matches(state.images)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::ImageSet,
                                    .matches = matches(state.imageSets)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Value,
                                    .matches = matches(state.values)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::FloatValue,
                                    .matches = matches(state.floatValues)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Text,
                                    .matches = matches(state.texts)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Slider,
                                    .matches = matches(state.sliders)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Graph,
                                    .matches = matches(state.graphs)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::GaugeGraph,
                                    .matches = matches(state.gaugeGraphs)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::JudgeGraph,
                                    .matches = matches(state.judgeGraphs)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::BpmGraph,
                                    .matches = matches(state.bpmGraphs)},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::HitErrorVisualizer,
          .matches = matches(state.hitErrorVisualizers)},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::TimingVisualizer,
          .matches = matches(state.timingVisualizers)},
      SkinObjectResolutionCandidate{
          .kind = SkinObjectResolutionKind::TimingDistributionGraph,
          .matches = matches(state.timingDistributionGraphs)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Gauge,
                                    .matches = gauge},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Note,
                                    .matches = note},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::HiddenCover,
                                    .matches = matches(state.hiddenCovers)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::LiftCover,
                                    .matches = matches(state.liftCovers)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Practice,
                                    .matches = practice},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Bga,
                                    .matches = bga},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::Judge,
                                    .matches = matches(state.judges)},
      SkinObjectResolutionCandidate{.kind = SkinObjectResolutionKind::PmChara,
                                    .matches = matches(state.pmCharas)}};
  const auto resolved = resolveSkinObjectPrecedence(candidates);
  return resolved.status == SkinObjectResolutionStatus::Found ? resolved.kind
                                                               : std::nullopt;
}

SkinObjectPayload buildPayload(BuildState &state,
                               SkinObjectResolutionKind kind,
                               std::string_view name) {
  switch (kind) {
  case SkinObjectResolutionKind::Image:
    return buildImage(state, state.images.at(std::string(name)));
  case SkinObjectResolutionKind::ImageSet:
    return buildImageSet(state, state.imageSets.at(std::string(name)));
  case SkinObjectResolutionKind::Value:
    return buildNumber(state, state.values.at(std::string(name)));
  case SkinObjectResolutionKind::FloatValue:
    return buildFloat(state, state.floatValues.at(std::string(name)));
  case SkinObjectResolutionKind::Text:
    return buildText(state, state.texts.at(std::string(name)));
  case SkinObjectResolutionKind::Slider:
    return buildSlider(state, state.sliders.at(std::string(name)));
  case SkinObjectResolutionKind::Graph:
    return buildGraph(state, state.graphs.at(std::string(name)));
  case SkinObjectResolutionKind::GaugeGraph:
    return buildGaugeGraph(state, state.gaugeGraphs.at(std::string(name)));
  case SkinObjectResolutionKind::JudgeGraph:
    return buildJudgeGraph(state, state.judgeGraphs.at(std::string(name)));
  case SkinObjectResolutionKind::BpmGraph:
    return buildBpmGraph(state, state.bpmGraphs.at(std::string(name)));
  case SkinObjectResolutionKind::HitErrorVisualizer:
    return buildHitError(state,
                         state.hitErrorVisualizers.at(std::string(name)));
  case SkinObjectResolutionKind::TimingVisualizer:
    return buildTimingVisualizer(state,
                                 state.timingVisualizers.at(std::string(name)));
  case SkinObjectResolutionKind::TimingDistributionGraph:
    return buildTimingDistribution(state,
                                   state.timingDistributionGraphs.at(
                                       std::string(name)));
  case SkinObjectResolutionKind::Gauge:
    return buildGauge(state, *state.gauge);
  case SkinObjectResolutionKind::Note:
    return buildNote(state, *state.note);
  case SkinObjectResolutionKind::HiddenCover:
    return buildCover(state, state.hiddenCovers.at(std::string(name)),
                      SkinCoverKind::Hidden);
  case SkinObjectResolutionKind::LiftCover:
    return buildCover(state, state.liftCovers.at(std::string(name)),
                      SkinCoverKind::Lift);
  case SkinObjectResolutionKind::Practice:
    return buildPractice(state, *state.practice);
  case SkinObjectResolutionKind::Bga:
    return SkinBgaObject{};
  case SkinObjectResolutionKind::Judge:
    return buildJudge(state, state.judges.at(std::string(name)));
  case SkinObjectResolutionKind::PmChara:
    return buildPmChara(state, state.pmCharas.at(std::string(name)));
  }
  return SkinImageObject{};
}

const DefinitionReference *definitionForKind(
    BuildState &state, SkinObjectResolutionKind kind, std::string_view name) {
  const auto mapped = [&](DefinitionMap &definitions) {
    const auto found = definitions.find(name);
    return found == definitions.end() ? nullptr : &found->second;
  };
  switch (kind) {
  case SkinObjectResolutionKind::Image:
    return mapped(state.images);
  case SkinObjectResolutionKind::ImageSet:
    return mapped(state.imageSets);
  case SkinObjectResolutionKind::Value:
    return mapped(state.values);
  case SkinObjectResolutionKind::FloatValue:
    return mapped(state.floatValues);
  case SkinObjectResolutionKind::Text:
    return mapped(state.texts);
  case SkinObjectResolutionKind::Slider:
    return mapped(state.sliders);
  case SkinObjectResolutionKind::Graph:
    return mapped(state.graphs);
  case SkinObjectResolutionKind::GaugeGraph:
    return mapped(state.gaugeGraphs);
  case SkinObjectResolutionKind::JudgeGraph:
    return mapped(state.judgeGraphs);
  case SkinObjectResolutionKind::BpmGraph:
    return mapped(state.bpmGraphs);
  case SkinObjectResolutionKind::HitErrorVisualizer:
    return mapped(state.hitErrorVisualizers);
  case SkinObjectResolutionKind::TimingVisualizer:
    return mapped(state.timingVisualizers);
  case SkinObjectResolutionKind::TimingDistributionGraph:
    return mapped(state.timingDistributionGraphs);
  case SkinObjectResolutionKind::Gauge:
    return state.gauge ? &*state.gauge : nullptr;
  case SkinObjectResolutionKind::Note:
    return state.note ? &*state.note : nullptr;
  case SkinObjectResolutionKind::HiddenCover:
    return mapped(state.hiddenCovers);
  case SkinObjectResolutionKind::LiftCover:
    return mapped(state.liftCovers);
  case SkinObjectResolutionKind::Practice:
    return state.practice ? &*state.practice : nullptr;
  case SkinObjectResolutionKind::Bga:
    return state.bga ? &*state.bga : nullptr;
  case SkinObjectResolutionKind::Judge:
    return mapped(state.judges);
  case SkinObjectResolutionKind::PmChara:
    return mapped(state.pmCharas);
  }
  return nullptr;
}

void decodeCustomBindings(BuildState &state) {
  visitObjectArray(state.root, "customEvents", state.context,
                   [&](const Json &event, std::size_t index) {
    const auto action = state.bindings.event(
        member(event, "action"), static_cast<std::uint32_t>(index),
        bindingPath("customEvents", index, "action"));
    if (!action) {
      state.context.warning("skin_json_binding_missing",
                            "JSON custom event has no static action binding",
                            state.context.source(member(event, "action")));
    }
    state.model.customEvents.push_back(
        {.id = integerField(event, "id", 0, state.context,
                            "JsonSkin.CustomEvent"),
         .action = action.value_or(SkinEventBindingId{}),
         .condition = state.bindings.boolean(
             member(event, "condition"), std::nullopt,
             static_cast<std::uint32_t>(index),
             bindingPath("customEvents", index, "condition")),
         .minimumIntervalMillis = integerField(
             event, "minInterval", 0, state.context,
             "JsonSkin.CustomEvent")});
  });
  visitObjectArray(state.root, "customTimers", state.context,
                   [&](const Json &timer, std::size_t index) {
    state.model.customTimers.push_back(
        {.id = integerField(timer, "id", 0, state.context,
                            "JsonSkin.CustomTimer"),
         .timer = state.bindings.timer(
             member(timer, "timer"), static_cast<std::uint32_t>(index),
             bindingPath("customTimers", index, "timer"))});
  });
}

void decodeDestinations(BuildState &state) {
  const Json *destinations = member(state.root, "destination");
  if (destinations == nullptr || !destinations->is_array()) return;
  state.nextSyntheticObjectId =
      static_cast<SkinObjectId>(destinations->size() + 1);
  for (std::size_t ordinal = 0; ordinal < destinations->size(); ++ordinal) {
    const Json &destination = (*destinations)[ordinal];
    const std::string name = stringField(destination, "id", {}, state.context,
                                         "JsonSkin.Destination");
    const SkinSourceLocation destinationSource =
        state.context.source(&destination).value_or(
            SkinSourceLocation{.virtualPath =
                                   state.context.entry.packageRelativePath});
    SkinSourceLocation objectSource = destinationSource;
    SkinObjectPayload payload;
    bool critical = false;
    if (const auto numeric = destinationInteger(name); numeric && *numeric < 0) {
      payload = SkinBuiltinImageObject{
          .referenceId = *numeric == kJavaIntegerMinimum
                             ? kJavaIntegerMinimum
                             : -*numeric};
    } else {
      const auto kind = resolvedKind(state, name);
      if (!kind) {
        continue;
      }
      if (const auto *definition = definitionForKind(state, *kind, name)) {
        objectSource =
            state.context.source(definition->value).value_or(objectSource);
      }
      if (*kind == SkinObjectResolutionKind::PmChara) {
        const Json &definition = *state.pmCharas.at(std::string(name)).value;
        const int type = integerField(definition, "type", kJavaIntegerMinimum,
                                      state.context, "JsonSkin.PMchara");
        const std::string source = stringField(definition, "src", {},
                                               state.context,
                                               "JsonSkin.PMchara");
        if (type < 0 || type > 15 || !state.sourceIds.contains(source)) {
          continue;
        }
      }
      payload = buildPayload(state, *kind, name);
      critical = *kind == SkinObjectResolutionKind::Note;
    }
    SkinDestinationBody presentation = decodeDestinationBody(
        state, destination, static_cast<std::uint32_t>(ordinal),
        bindingPath("destination", ordinal, "presentation"));
    if (const auto *cover = std::get_if<SkinCoverObject>(&payload)) {
      const auto normalized = normalizeSkinCover(
          {.kind = cover->kind,
           .sprite = cover->sprite,
           .authoredDisappearLine = cover->disappearLine,
           .authoredDisappearLineLinksLift = cover->disappearLineLinksLift,
           .lineScale = 1.0,
           .authoredDestinationOffsetIds = presentation.offsetIds});
      if (normalized.cover) {
        presentation.offsetIds = normalized.destinationOffsetIds;
      }
    }
    const SkinObjectId objectId{
        static_cast<std::uint32_t>(state.model.destinations.size() + 1)};
    state.model.objects.push_back(
        {.id = objectId,
         .authoredName = name,
         .payload = std::move(payload),
         .authoredOrdinal = static_cast<std::uint32_t>(ordinal),
         .critical = critical,
         .source = std::move(objectSource)});
    state.model.destinations.push_back(
        {.object = objectId,
         .presentation = std::move(presentation),
         .source = destinationSource});
  }
  state.model.objects.insert(state.model.objects.end(),
                             std::make_move_iterator(state.nestedObjects.begin()),
                             std::make_move_iterator(state.nestedObjects.end()));
}

void buildGameplayModel(const Json &root, const BeatorajaSkinHeader &header,
                        DecodeContext &context) {
  BeatorajaSkinModel model;
  model.header = header;
  model.timing = {
      .fadeoutMillis = integerField(root, "fadeout", 0, context,
                                    "JsonSkin.Skin"),
      .inputMillis = integerField(root, "input", 0, context,
                                  "JsonSkin.Skin"),
      .sceneMillis = integerField(root, "scene", 0, context,
                                  "JsonSkin.Skin"),
      .closeMillis = integerField(root, "close", 0, context,
                                  "JsonSkin.Skin"),
      .loadEndMillis = integerField(root, "loadend", 0, context,
                                    "JsonSkin.Skin"),
      .playStartMillis = integerField(root, "playstart", 0, context,
                                      "JsonSkin.Skin"),
      .judgeTimerMillis = integerField(root, "judgetimer", 1, context,
                                       "JsonSkin.Skin"),
      .finishMarginMillis = integerField(root, "finishmargin", 0, context,
                                         "JsonSkin.Skin")};
  BuildState state(context, root, model);
  decodeResources(state);
  indexGameplayDefinitions(state);
  decodeCustomBindings(state);
  decodeDestinations(state);
  state.bindings.moveInto(model);
  context.result.model = std::move(model);
}

} // namespace

JsonGameplaySkinDecodeResult JsonGameplaySkinDecoder::decode(
    std::span<const std::byte> bytes, const SkinEntryId &entry,
    const EntryProfileSettings *desired, SkinBuiltinBindingCatalogView builtins,
    SkinSafetyPolicy safetyPolicy) const {
  JsonGameplaySkinDecodeResult result;
  DecodeContext context{.entry = entry,
                        .builtins = builtins,
                        .safetyPolicy = safetyPolicy,
                        .result = result};
  const std::uint64_t byteLimit = safetyPolicy.limit(
      SkinSafetyGuard::LuaDecoderLimit,
      JsonGameplaySkinDecoderPolicy::maxDocumentBytes);
  if (bytes.size() > byteLimit) {
    context.error("skin_json_limit_exceeded",
                  "JSON gameplay document exceeds the fixed byte limit");
    return result;
  }
  const std::string_view text(
      reinterpret_cast<const char *>(bytes.data()), bytes.size());
  Json root;
  try {
    root = Json::parse(text.begin(), text.end(), nullptr, true, true);
  } catch (const Json::parse_error &error) {
    const std::size_t byte = error.byte > 0 ? error.byte - 1 : 0;
    const auto source = locationAt(text, byte, entry.packageRelativePath);
    context.error("skin_json_parse_failed", "JSON gameplay document is invalid",
                  source);
    return result;
  } catch (...) {
    context.error("skin_json_parse_failed",
                  "JSON gameplay document could not be parsed");
    return result;
  }
  try {
    if (!root.is_object()) {
      context.error("skin_json_model_invalid",
                    "JSON gameplay document root is not an object");
      return result;
    }
    if (!validateBudget(root, context)) {
      return result;
    }
    JsonSourceIndex sourceIndex(text, entry.packageRelativePath);
    if (!sourceIndex.build(root)) {
      context.error("skin_json_source_index_failed",
                    "JSON gameplay provenance index could not be built");
      return result;
    }
    context.sources = &sourceIndex;
    classifyDocumentFields(root, context);
    BeatorajaSkinHeader header = decodeHeader(root, context);
    result.header = header;
    reconcileConfiguration(header, desired, result, context);
    if (!gameplaySkinTraitForSkinType(header.type)) {
      context.error("skin_json_model_type_unsupported",
                    "JSON gameplay document does not declare a gameplay type");
      return result;
    }
    buildGameplayModel(root, header, context);
    if (context.failed) {
      // Header and reconciled configuration remain useful for diagnostics and
      // catalog metadata. Only the unsafe/incomplete canonical model fails.
      result.model.reset();
    }
    return result;
  } catch (...) {
    result.header.reset();
    result.configuration.reset();
    result.reconciledSettings.reset();
    result.model.reset();
    result.diagnostics.push_back(makeDiagnostic(
        "skin_json_decode_failed",
        "JSON gameplay document could not be decoded within host limits",
        entry));
    return result;
  }
}

} // namespace skin
