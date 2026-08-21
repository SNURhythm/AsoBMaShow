#include "Lr2GameplaySkinDecoder.h"

#include "../GameplaySkinTraits.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace skin {
namespace {

constexpr std::size_t kMaximumCommands = 200'000;
constexpr std::size_t kMaximumObjects = 8'192;
constexpr std::size_t kMaximumFrames = 200'000;
constexpr int kMaximumOffsetId = 199;

int parseStrictInteger(std::string_view value) {
  int result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::invalid_argument("invalid LR2 integer");
  }
  return result;
}

int parseLooseInteger(std::string value) noexcept {
  std::ranges::replace(value, '!', '-');
  value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
  try {
    return parseStrictInteger(value);
  } catch (...) {
    return 0;
  }
}

std::array<int, 22> commandValues(const Lr2SkinCommand &command) {
  std::array<int, 22> values{};
  for (std::size_t index = 1;
       index < values.size() && index <= command.fields.size(); ++index) {
    values[index] = parseLooseInteger(command.fields[index - 1]);
  }
  return values;
}

std::string_view requiredField(const Lr2SkinCommand &command,
                               std::size_t javaIndex) {
  if (javaIndex == 0 || javaIndex > command.fields.size()) {
    throw std::out_of_range("missing LR2 field");
  }
  return command.fields[javaIndex - 1];
}

int requiredStrictInteger(const Lr2SkinCommand &command,
                          std::size_t javaIndex) {
  return parseStrictInteger(requiredField(command, javaIndex));
}

std::string normalizedPath(std::string value) {
  constexpr std::string_view legacy = "LR2files\\Theme";
  std::size_t position = 0;
  while ((position = value.find(legacy, position)) != std::string::npos) {
    value.replace(position, legacy.size(), "skin");
    position += 4;
  }
  std::ranges::replace(value, '\\', '/');
  return value;
}

std::string numericCharacters(std::string_view value) {
  std::string result;
  for (const char character : value) {
    if ((character >= '0' && character <= '9') || character == '-') {
      result.push_back(character);
    }
  }
  return result;
}

std::vector<int> readOffsets(const Lr2SkinCommand &command,
                             std::initializer_list<int> defaults = {}) {
  std::vector<int> output;
  std::set<int> seen;
  const auto append = [&](int value) {
    if (value > 0 && value <= kMaximumOffsetId && seen.insert(value).second) {
      output.push_back(value);
    }
  };
  for (const int value : defaults) append(value);
  for (std::size_t javaIndex = 21; javaIndex <= command.fields.size();
       ++javaIndex) {
    const std::string numeric =
        numericCharacters(command.fields[javaIndex - 1]);
    if (!numeric.empty()) append(parseStrictInteger(numeric));
  }
  return output;
}

SkinDiagnostic commandDiagnostic(const Lr2SkinCommand &command,
                                 std::string code,
                                 std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = command.source.virtualPath,
          .severity = DiagnosticSeverity::Warning,
          .source = command.source};
}

SkinDiagnostic documentDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
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

template <typename Result>
bool reconcileConfiguration(const BeatorajaSkinHeader &header,
                            const EntryProfileSettings *desired,
                            Result &result) {
  BeatorajaSkinConfiguration configuration;
  EntryProfileSettings settings;
  if (desired != nullptr) settings.viewport = desired->viewport;

  for (const auto &option : header.options) {
    if (option.name.empty()) {
      result.diagnostics.push_back(documentDiagnostic(
          "skin_lr2_configuration_invalid",
          "LR2 skin option has an empty name"));
      result.fatal = true;
      return false;
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
        const auto found = desired->options.find(option.name);
        if (found != desired->options.end()) {
          if (found->second == -1) {
            persistedValue = -1;
            runtimeValue = option.choices.front().value;
          } else if (std::ranges::any_of(
                         option.choices, [&](const auto &choice) {
                           return choice.value == found->second;
                         })) {
            runtimeValue = persistedValue = found->second;
          }
        }
      }
    }
    configuration.orderedOptions.push_back(
        {.name = option.name, .value = runtimeValue});
    configuration.options.insert_or_assign(option.name, persistedValue);
    if (runtimeValue != -1) configuration.enabledOptionIds.insert(runtimeValue);
    settings.options.insert_or_assign(option.name, persistedValue);
  }

  for (const auto &file : header.files) {
    if (file.name.empty()) {
      result.diagnostics.push_back(documentDiagnostic(
          "skin_lr2_configuration_invalid",
          "LR2 skin file declaration has an empty name"));
      result.fatal = true;
      return false;
    }
    std::string selected = file.defaultValue;
    if (desired != nullptr) {
      const auto found = desired->filePaths.find(file.name);
      if (found != desired->filePaths.end()) selected = found->second;
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
      result.diagnostics.push_back(documentDiagnostic(
          "skin_lr2_configuration_invalid",
          "LR2 skin offset has an empty name"));
      result.fatal = true;
      return false;
    }
    ConfigOffset value;
    if (desired != nullptr) {
      const auto found = desired->offsets.find(offset.name);
      if (found != desired->offsets.end()) value = found->second;
    }
    value = sanitizeOffset(value, offset.permissions);
    configuration.offsets.insert_or_assign(offset.name, value);
    configuration.offsetPermissions.insert_or_assign(offset.name,
                                                      offset.permissions);
    configuration.offsetsById.insert_or_assign(offset.id, value);
    settings.offsets.insert_or_assign(offset.name, value);
  }

  configuration.lowercaseSha256 = skinConfigurationDigest(settings);
  result.configuration = std::move(configuration);
  result.reconciledSettings = std::move(settings);
  return true;
}

class BindingRegistry final {
public:
  explicit BindingRegistry(SkinBuiltinBindingCatalogView builtins)
      : builtins_(builtins) {}

  std::optional<SkinBooleanPropertyId> boolean(int selector,
                                                std::uint32_t ordinal) {
    const SkinBindingType type{.kind = SkinBindingKind::BooleanProperty};
    if (!contains(type, selector)) return std::nullopt;
    return intern<SkinBooleanPropertyId>(booleans_, booleanIds_, selector,
                                         ordinal);
  }

  std::optional<SkinIntegerPropertyId>
  integer(int selector, SkinIntegerPropertyDomain domain,
          std::uint32_t ordinal) {
    const SkinBindingType type{.kind = SkinBindingKind::IntegerProperty,
                               .integerDomain = domain};
    if (!contains(type, selector)) return std::nullopt;
    const std::size_t bucket = static_cast<std::size_t>(domain);
    const auto found = integerIds_[bucket].find(selector);
    if (found != integerIds_[bucket].end()) return found->second;
    const SkinIntegerPropertyId id{
        static_cast<std::uint32_t>(integers_.size() + 1)};
    integers_.push_back({.id = id,
                         .domain = domain,
                         .source = SkinBuiltinPropertySelector{selector},
                         .authoredOrdinal = ordinal});
    integerIds_[bucket].emplace(selector, id);
    return id;
  }

  std::optional<SkinFloatPropertyId>
  floating(int selector, SkinFloatPropertyDomain domain,
           std::uint32_t ordinal) {
    const SkinBindingType type{.kind = SkinBindingKind::FloatProperty,
                               .floatDomain = domain};
    if (!contains(type, selector)) return std::nullopt;
    const std::size_t bucket = static_cast<std::size_t>(domain);
    const auto found = floatIds_[bucket].find(selector);
    if (found != floatIds_[bucket].end()) return found->second;
    const SkinFloatPropertyId id{
        static_cast<std::uint32_t>(floats_.size() + 1)};
    floats_.push_back({.id = id,
                       .domain = domain,
                       .source = SkinBuiltinPropertySelector{selector},
                       .authoredOrdinal = ordinal});
    floatIds_[bucket].emplace(selector, id);
    return id;
  }

  std::optional<SkinStringPropertyId> string(int selector,
                                              std::uint32_t ordinal) {
    const SkinBindingType type{.kind = SkinBindingKind::StringProperty};
    if (!contains(type, selector)) return std::nullopt;
    return intern<SkinStringPropertyId>(strings_, stringIds_, selector,
                                        ordinal);
  }

  std::optional<SkinTimerPropertyId> timer(int selector,
                                            std::uint32_t ordinal) {
    if (selector <= 0) return std::nullopt;
    const SkinBindingType type{.kind = SkinBindingKind::TimerProperty};
    if (!contains(type, selector)) return std::nullopt;
    return intern<SkinTimerPropertyId>(timers_, timerIds_, selector, ordinal);
  }

  std::optional<SkinFloatWriterId> floatWriter(int selector,
                                                std::uint32_t ordinal) {
    const SkinBindingType type{.kind = SkinBindingKind::FloatWriter};
    if (!contains(type, selector)) return std::nullopt;
    return intern<SkinFloatWriterId>(floatWriters_, floatWriterIds_, selector,
                                     ordinal);
  }

  std::optional<SkinStringWriterId> stringWriter(int selector,
                                                  std::uint32_t ordinal) {
    const SkinBindingType type{.kind = SkinBindingKind::StringWriter};
    if (!contains(type, selector)) return std::nullopt;
    return intern<SkinStringWriterId>(stringWriters_, stringWriterIds_,
                                      selector, ordinal);
  }

  std::optional<SkinEventBindingId> event(int selector,
                                           std::uint32_t ordinal) {
    const SkinBindingType type{.kind = SkinBindingKind::Event};
    if (!contains(type, selector)) return std::nullopt;
    return intern<SkinEventBindingId>(events_, eventIds_, selector, ordinal);
  }

  void moveInto(BeatorajaSkinModel &model) {
    model.booleanProperties = std::move(booleans_);
    model.integerProperties = std::move(integers_);
    model.floatProperties = std::move(floats_);
    model.stringProperties = std::move(strings_);
    model.timerProperties = std::move(timers_);
    model.floatWriters = std::move(floatWriters_);
    model.stringWriters = std::move(stringWriters_);
    model.events = std::move(events_);
  }

private:
  bool contains(SkinBindingType type, int selector) const {
    return builtins_.contains(type, SkinBuiltinPropertySelector{selector});
  }

  template <typename Id, typename Binding>
  std::optional<Id> intern(std::vector<Binding> &bindings,
                           std::map<int, Id> &ids, int selector,
                           std::uint32_t ordinal) {
    const auto found = ids.find(selector);
    if (found != ids.end()) return found->second;
    const Id id{static_cast<std::uint32_t>(bindings.size() + 1)};
    bindings.push_back({.id = id,
                        .source = SkinBuiltinPropertySelector{selector},
                        .authoredOrdinal = ordinal});
    ids.emplace(selector, id);
    return id;
  }

  SkinBuiltinBindingCatalogView builtins_;
  std::vector<SkinBooleanPropertyBinding> booleans_;
  std::vector<SkinIntegerPropertyBinding> integers_;
  std::vector<SkinFloatPropertyBinding> floats_;
  std::vector<SkinStringPropertyBinding> strings_;
  std::vector<SkinTimerPropertyBinding> timers_;
  std::vector<SkinFloatWriterBinding> floatWriters_;
  std::vector<SkinStringWriterBinding> stringWriters_;
  std::vector<SkinEventBinding> events_;
  std::map<int, SkinBooleanPropertyId> booleanIds_;
  std::array<std::map<int, SkinIntegerPropertyId>, 2> integerIds_;
  std::array<std::map<int, SkinFloatPropertyId>, 2> floatIds_;
  std::map<int, SkinStringPropertyId> stringIds_;
  std::map<int, SkinTimerPropertyId> timerIds_;
  std::map<int, SkinFloatWriterId> floatWriterIds_;
  std::map<int, SkinStringWriterId> stringWriterIds_;
  std::map<int, SkinEventBindingId> eventIds_;
};

struct ModeInfo {
  int lanes = 0;
  int players = 0;
  std::vector<int> scratch;
  bool popn = false;
};

std::optional<ModeInfo> modeForSkinType(int type) {
  switch (type) {
  case 0: return ModeInfo{.lanes = 8, .players = 1, .scratch = {7}};
  case 1: return ModeInfo{.lanes = 6, .players = 1, .scratch = {5}};
  case 2: return ModeInfo{.lanes = 16, .players = 2, .scratch = {7, 15}};
  case 3: return ModeInfo{.lanes = 12, .players = 2, .scratch = {5, 11}};
  case 4: return ModeInfo{.lanes = 9, .players = 1, .popn = true};
  case 16: return ModeInfo{.lanes = 26, .players = 1, .scratch = {24, 25}};
  case 17:
    return ModeInfo{.lanes = 52,
                    .players = 2,
                    .scratch = {24, 25, 50, 51}};
  default: return std::nullopt;
  }
}

SkinBlendMode blendMode(int value) {
  switch (value) {
  case 0:
  case 1: return SkinBlendMode::Normal;
  case 2: return SkinBlendMode::Additive;
  case 3: return SkinBlendMode::Subtractive;
  case 4: return SkinBlendMode::Multiply;
  case 9: return SkinBlendMode::Inverse;
  default: return SkinBlendMode::Normal;
  }
}

SkinZeroPaddingMode zeroPaddingMode(int value) {
  if (value <= 0) return SkinZeroPaddingMode::None;
  return value == 2 ? SkinZeroPaddingMode::AlternateZero
                    : SkinZeroPaddingMode::Zero;
}

std::optional<std::uint32_t> parseColor(std::string_view value) {
  if (!value.empty() && value.front() == '#') value.remove_prefix(1);
  if (value.size() != 6 && value.size() != 8) return std::nullopt;
  std::uint32_t result = 0;
  for (const char character : value) {
    int digit = -1;
    if (character >= '0' && character <= '9') digit = character - '0';
    if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
    if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
    if (digit < 0) return std::nullopt;
    result = result * 16U + static_cast<std::uint32_t>(digit);
  }
  return value.size() == 6 ? (result << 8U) | 0xffU : result;
}

std::uint32_t bpmColor(std::string_view authored, std::uint32_t fallback) {
  std::string normalized;
  for (const char character : authored) {
    if ((character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'f') ||
        (character >= 'A' && character <= 'F')) {
      normalized.push_back(character);
      if (normalized.size() == 6) break;
    }
  }
  if (normalized.empty()) return fallback;
  return parseColor(normalized).value_or(fallback);
}

std::uint32_t timingColor(std::string_view authored) {
  std::string normalized;
  for (const char character : authored) {
    if ((character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'f') ||
        (character >= 'A' && character <= 'F')) {
      normalized.push_back(character);
    }
  }
  if (normalized.size() != authored.size() || authored.size() < 6) {
    return 0xff0000ffU;
  }
  return parseColor(authored).value_or(0xff0000ffU);
}

bool moviePath(std::string_view path) {
  constexpr std::array<std::string_view, 9> extensions{
      ".mp4", ".m4v", ".wmv", ".webm", ".mpg",
      ".mpeg", ".m1v", ".m2v", ".avi"};
  const std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) return false;
  const std::string_view extension = path.substr(dot);
  return std::ranges::any_of(extensions, [&](std::string_view candidate) {
    return extension.size() == candidate.size() &&
           std::ranges::equal(extension, candidate, [](char left, char right) {
             if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
             return left == right;
           });
  });
}

struct DestinationControl {
  int blend = 0;
  int filter = 0;
  int center = 0;
};

struct LineSlot {
  std::optional<SkinSpriteFrames> sprite;
  std::optional<SkinDestinationBody> destination;
  DestinationControl control;
  SkinAuthoredRect groupRect;
};

struct JudgeBuildState {
  std::optional<SkinObjectId> object;
  std::array<std::optional<SkinObjectId>, 7> images;
  std::array<std::optional<SkinObjectId>, 7> numbers;
  bool detailsAdded = false;
};

class DecodeSession final {
public:
  DecodeSession(const BeatorajaSkinHeader &header,
                const BeatorajaSkinConfiguration &configuration,
                SkinBuiltinBindingCatalogView builtins,
                SkinSafetyPolicy safetyPolicy, std::stop_token stop,
                StaticSkinDecodeCheckpoint checkpoint,
                Lr2GameplaySkinDecodeResult &result)
      : header_(header), bindings_(builtins), result_(result),
        stop_(stop), checkpoint_(checkpoint),
        mode_(modeForSkinType(header.type)),
        maximumObjects_(static_cast<std::size_t>(safetyPolicy.limit(
            SkinSafetyGuard::LuaDecoderLimit, kMaximumObjects))),
        maximumFrames_(static_cast<std::size_t>(safetyPolicy.limit(
            SkinSafetyGuard::LuaDecoderLimit, kMaximumFrames))) {
    model_.header = header;
    for (const int option : configuration.enabledOptionIds) {
      options_.insert_or_assign(option, 1);
    }
    if (mode_) {
      const auto size = static_cast<std::size_t>(mode_->lanes);
      note_.resize(size);
      lnEnd_.resize(size);
      lnStart_.resize(size);
      lnBody_.resize(size);
      lnBodyActive_.resize(size);
      hcnEnd_.resize(size);
      hcnStart_.resize(size);
      hcnBody_.resize(size);
      hcnBodyActive_.resize(size);
      hcnDamage_.resize(size);
      hcnReactive_.resize(size);
      mine_.resize(size);
      laneRects_.resize(size);
      noteHeights_.resize(size);
    }
  }

  void execute(std::span<const Lr2SkinCommand> commands) {
    for (std::size_t index = 0; index < commands.size(); ++index) {
      if (cancelled()) return;
      ordinal_ = static_cast<std::uint32_t>(index);
      const auto &command = commands[index];
      try {
        if (processControl(command)) continue;
        if (skip_) continue;
        processCommand(command);
      } catch (...) {
        result_.diagnostics.push_back(commandDiagnostic(
            command, "skin_lr2_gameplay_command_invalid",
            "Invalid LR2 gameplay command #" + command.name));
      }
    }
    if (cancelled()) return;
    finalizeNote();
    bindings_.moveInto(model_);
  }

  BeatorajaSkinModel takeModel() { return std::move(model_); }

private:
  [[nodiscard]] bool cancelled() {
    ++workItem_;
    if (checkpoint_.notify != nullptr) {
      checkpoint_.notify(StaticSkinDecodePhase::Lr2Model, workItem_,
                         checkpoint_.context);
    }
    if (!stop_.stop_requested()) return false;
    result_.cancelled = true;
    return true;
  }

  bool processControl(const Lr2SkinCommand &command) {
    if (command.name == "IF") {
      ifs_ = evaluateCondition(command);
      skip_ = !ifs_;
      return true;
    }
    if (command.name == "ELSEIF") {
      if (ifs_) {
        skip_ = true;
      } else {
        ifs_ = evaluateCondition(command);
        skip_ = !ifs_;
      }
      return true;
    }
    if (command.name == "ELSE") {
      skip_ = ifs_;
      return true;
    }
    if (command.name == "ENDIF") {
      skip_ = false;
      ifs_ = false;
      return true;
    }
    if (command.name == "SETOPTION") {
      if (!skip_) {
        const int id = requiredStrictInteger(command, 1);
        options_.insert_or_assign(
            id, requiredStrictInteger(command, 2) >= 1 ? 1 : 0);
      }
      return true;
    }
    return false;
  }

  bool evaluateCondition(const Lr2SkinCommand &command) const {
    for (const auto &field : command.fields) {
      if (field.empty()) continue;
      std::string numeric;
      for (char character : field) {
        if (character == '!') character = '-';
        if ((character >= '0' && character <= '9') || character == '-') {
          numeric.push_back(character);
        }
      }
      const int condition = parseStrictInteger(numeric);
      const auto found = options_.find(std::abs(condition));
      const bool enabled =
          found != options_.end() &&
          (condition >= 0 ? found->second == 1 : found->second == 0);
      // LR2SkinLoader falls back to the current MainState for official
      // BooleanPropertyFactory IDs. Static document decoding has no mutable
      // MainState, so only configured/load-time options are decidable here.
      if (!enabled) return false;
    }
    return true;
  }

  SkinObjectDefinition *object(SkinObjectId id) {
    if (id == 0 || static_cast<std::size_t>(id) > model_.objects.size()) {
      return nullptr;
    }
    return &model_.objects[static_cast<std::size_t>(id - 1)];
  }

  SkinDestination *destination(SkinObjectId id) {
    const auto found = destinationIndices_.find(id);
    return found == destinationIndices_.end()
               ? nullptr
               : &model_.destinations[found->second];
  }

  SkinObjectId addObject(SkinObjectPayload payload,
                         const Lr2SkinCommand &command, bool topLevel = true) {
    if (model_.objects.size() >= maximumObjects_) {
      throw std::length_error("LR2 object limit");
    }
    const SkinObjectId id =
        static_cast<SkinObjectId>(model_.objects.size() + 1);
    model_.objects.push_back(
        {.id = id,
         .authoredName = "lr2:" + command.name + ":" +
                         std::to_string(command.source.line) + ":" +
                         std::to_string(id),
         .payload = std::move(payload),
         .authoredOrdinal = ordinal_,
         .critical = false,
         .source = command.source});
    if (topLevel) {
      const auto destinationOrdinal =
          static_cast<std::uint32_t>(model_.destinations.size());
      model_.destinations.push_back(
          {.object = id,
           .presentation = {.authoredOrdinal = destinationOrdinal},
           .source = command.source});
      destinationIndices_.emplace(id, model_.destinations.size() - 1);
      destinationControls_.try_emplace(id);
    }
    return id;
  }

  void setConditions(SkinDestinationBody &body,
                     const Lr2SkinCommand &command,
                     const std::array<int, 22> &values) {
    if (!body.conditions.empty() || body.drawCondition) return;
    for (std::size_t javaIndex = 18; javaIndex <= 20; ++javaIndex) {
      const int selector = values[javaIndex];
      if (selector == 0) continue;
      if (const auto boolean = bindings_.boolean(selector, ordinal_)) {
        body.conditions.emplace_back(*boolean);
      } else {
        body.conditions.emplace_back(selector);
      }
    }
  }

  void applyDestinationBody(SkinDestinationBody &body,
                            DestinationControl &control,
                            const Lr2SkinCommand &command,
                            SkinDestinationFrame frame,
                            std::initializer_list<int> defaultOffsets = {},
                            bool applyStretch = false,
                            std::optional<int> forcedTime = std::nullopt) {
    const auto values = commandValues(command);
    if (modelFrameCount_ >= maximumFrames_) {
      throw std::length_error("LR2 frame limit");
    }
    frame.timeMillis = forcedTime.value_or(values[2]);
    frame.acceleration = values[7];
    const auto channel = [](int value) {
      return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
    };
    frame.rgba = {channel(values[9]), channel(values[10]),
                  channel(values[11]), channel(values[8])};
    frame.angleDegrees = values[14];
    if (control.blend == 0) {
      control.blend = values[12];
      body.blend = blendMode(values[12]);
    }
    if (control.filter == 0) {
      control.filter = values[13];
      body.filter = values[13] == 0 ? SkinFilterMode::Nearest
                                    : SkinFilterMode::Linear;
    }
    if (control.center == 0 && values[15] >= 0 && values[15] < 10) {
      control.center = values[15];
      body.center = values[15];
    }
    if (body.loop == 0) body.loop = values[16];
    if (!body.timer) body.timer = bindings_.timer(values[17], ordinal_);
    setConditions(body, command, values);
    if (body.offsetIds.empty()) {
      body.offsetIds = readOffsets(command, defaultOffsets);
    }
    if (applyStretch && stretch_ >= 0 && stretch_ <= 10) {
      body.stretch = static_cast<SkinStretchMode>(stretch_);
    }
    body.frames.push_back(frame);
    ++modelFrameCount_;
    std::stable_sort(body.frames.begin(), body.frames.end(),
                     [](const auto &left, const auto &right) {
                       return left.timeMillis < right.timeMillis;
                     });
  }

  void applyDestination(SkinObjectId id, const Lr2SkinCommand &command,
                        SkinDestinationFrame frame,
                        std::initializer_list<int> defaultOffsets = {},
                        bool applyStretch = false,
                        std::optional<int> forcedTime = std::nullopt) {
    auto *target = destination(id);
    if (target == nullptr) return;
    if (target->presentation.frames.empty()) target->source = command.source;
    applyDestinationBody(target->presentation, destinationControls_[id],
                         command, frame, defaultOffsets, applyStretch,
                         forcedTime);
  }

  std::optional<SkinSpriteFrames>
  sourceSprite(const std::array<int, 22> &values, bool animated = true) {
    const int imageIndex = values[2];
    if (imageIndex < 0 ||
        static_cast<std::size_t>(imageIndex) >= imageResources_.size()) {
      return std::nullopt;
    }
    if (imageMovies_[static_cast<std::size_t>(imageIndex)]) {
      return std::nullopt;
    }
    const int columns = values[7] > 0 ? values[7] : 1;
    const int rows = values[8] > 0 ? values[8] : 1;
    if (rows <= 0 || static_cast<std::size_t>(columns) > maximumFrames_ ||
        static_cast<std::size_t>(columns) >
            maximumFrames_ / static_cast<std::size_t>(rows)) {
      throw std::length_error("LR2 sprite frame limit");
    }
    SkinSpriteFrames sprite{
        .resource = imageResources_[static_cast<std::size_t>(imageIndex)],
        .cycleMillis = animated ? values[9] : 0,
        .timer = animated ? bindings_.timer(values[10], ordinal_)
                          : std::nullopt};
    sprite.frames.reserve(static_cast<std::size_t>(columns) *
                          static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
      for (int column = 0; column < columns; ++column) {
        sprite.frames.push_back({.x = values[3],
                                 .y = values[4],
                                 .w = values[5],
                                 .h = values[6],
                                 .gridColumn = column,
                                 .gridRow = row,
                                 .gridColumns = columns,
                                 .gridRows = rows});
      }
    }
    return sprite;
  }

  SkinSpriteFrames subsetSprite(const SkinSpriteFrames &source,
                                std::span<const std::size_t> indices) {
    SkinSpriteFrames output{.resource = source.resource,
                            .cycleMillis = source.cycleMillis,
                            .timer = source.timer};
    output.frames.reserve(indices.size());
    for (const std::size_t index : indices) {
      if (index >= source.frames.size()) {
        throw std::out_of_range("LR2 sprite subset");
      }
      output.frames.push_back(source.frames[index]);
    }
    return output;
  }

  SkinDestinationFrame standardFrame(const std::array<int, 22> &values,
                                     bool normalizeNegative = false) const {
    int x = values[3];
    int y = values[4];
    int width = values[5];
    int height = values[6];
    if (normalizeNegative && width < 0) {
      x += width;
      width = -width;
    }
    if (normalizeNegative && height < 0) {
      y += height;
      height = -height;
    }
    return {.x = static_cast<double>(x),
            .y = static_cast<double>(header_.height - (y + height)),
            .width = static_cast<double>(width),
            .height = static_cast<double>(height)};
  }

  void addImageResource(const Lr2SkinCommand &command) {
    const SkinResourceId id =
        static_cast<SkinResourceId>(model_.resources.size() + 1);
    const std::string name = "lr2-image-" +
                             std::to_string(imageResources_.size());
    const std::string path =
        normalizedPath(std::string(requiredField(command, 1)));
    if (moviePath(path)) {
      model_.resources.emplace_back(SkinMovieResource{
          .id = id, .virtualPath = path, .authoredOrdinal = ordinal_});
    } else {
      model_.resources.emplace_back(SkinImageResource{
          .id = id,
          .authoredName = name,
          .virtualPath = path,
          .authoredOrdinal = ordinal_});
    }
    imageResources_.push_back(id);
    imageMovies_.push_back(moviePath(path));
  }

  void addFontResource(const Lr2SkinCommand &command) {
    const SkinResourceId id =
        static_cast<SkinResourceId>(model_.resources.size() + 1);
    const std::string name = "lr2-font-" +
                             std::to_string(fontResources_.size());
    const std::string path =
        normalizedPath(std::string(requiredField(command, 1)));
    model_.resources.emplace_back(SkinFontResource{
        .id = id,
        .authoredName = name,
        .virtualPath = path,
        .type = 0,
        .authoredOrdinal = ordinal_,
        .bitmap = SkinBitmapFontResource{.id = id,
                                         .virtualPath = path,
                                         .type = 0,
                                         .authoredOrdinal = ordinal_}});
    fontResources_.push_back(id);
  }

  SkinResourceId defaultFont() {
    if (defaultFont_ != 0) return defaultFont_;
    const SkinResourceId id =
        static_cast<SkinResourceId>(model_.resources.size() + 1);
    model_.resources.emplace_back(SkinFontResource{
        .id = id,
        .authoredName = "lr2-default-font",
        .virtualPath = "skin/default/VL-Gothic-Regular.ttf",
        .type = 0,
        .authoredOrdinal = ordinal_});
    defaultFont_ = id;
    return id;
  }

  void sourceImage(const Lr2SkinCommand &command) {
    activeImage_.reset();
    const auto values = commandValues(command);
    if (values[2] >= 100) {
      activeImage_ = addObject(
          SkinBuiltinImageObject{.referenceId = values[2]}, command);
      return;
    }
    if (values[2] >= 0 &&
        static_cast<std::size_t>(values[2]) < imageResources_.size() &&
        imageMovies_[static_cast<std::size_t>(values[2])]) {
      activeImage_ = addObject(
          SkinImageObject{.orderedStates = {
                              SkinSpriteFrames{
                                  .resource = imageResources_[
                                      static_cast<std::size_t>(values[2])],
                                  .frames = {{.w = -1, .h = -1}}}}},
          command);
      return;
    }
    const auto sprite = sourceSprite(values);
    if (!sprite) return;
    activeImage_ = addObject(
        SkinImageObject{.orderedStates = {*sprite}}, command);
  }

  void defineImageSet(const Lr2SkinCommand &command) {
    const auto values = commandValues(command);
    auto sprite = sourceSprite(values);
    if (!sprite) return;
    sprite->timer.reset();
    sprite->cycleMillis = 0;
    imageSets_.push_back(std::move(*sprite));
  }

  void sourceImageSet(const Lr2SkinCommand &command) {
    activeImage_.reset();
    const auto values = commandValues(command);
    if (values[4] < 0 || values[4] > 17) {
      throw std::out_of_range("LR2 image-set count");
    }
    SkinImageObject image;
    image.stateIndex = bindings_.integer(
        values[3], SkinIntegerPropertyDomain::ImageIndex, ordinal_);
    for (int index = 0; index < values[4]; ++index) {
      const int setIndex = values[5 + index];
      if (setIndex < 0 ||
          static_cast<std::size_t>(setIndex) >= imageSets_.size()) {
        throw std::out_of_range("LR2 image-set reference");
      }
      SkinSpriteFrames state = imageSets_[static_cast<std::size_t>(setIndex)];
      state.cycleMillis = values[1];
      state.timer = bindings_.timer(values[2], ordinal_);
      image.orderedStates.push_back(std::move(state));
    }
    activeImage_ = addObject(std::move(image), command);
  }

  void sourceNumber(const Lr2SkinCommand &command) {
    activeNumber_.reset();
    const auto values = commandValues(command);
    const auto source = sourceSprite(values);
    if (!source || source->frames.size() < 10) return;

    SkinNumberObject number;
    number.value = bindings_
                       .integer(values[11],
                                SkinIntegerPropertyDomain::IntegerValue,
                                ordinal_)
                       .value_or(SkinIntegerPropertyId{});
    number.alignment = values[12];
    number.spacing = values[15];

    if (source->frames.size() % 24 == 0) {
      const std::size_t animations = source->frames.size() / 24;
      std::vector<std::size_t> positive;
      std::vector<std::size_t> negative;
      positive.reserve(animations * 12);
      negative.reserve(animations * 12);
      for (std::size_t animation = 0; animation < animations; ++animation) {
        for (std::size_t glyph = 0; glyph < 12; ++glyph) {
          positive.push_back(animation * 24 + glyph);
          negative.push_back(animation * 24 + glyph + 12);
        }
      }
      number.digits = {
          .positive = subsetSprite(*source, positive),
          .negative = subsetSprite(*source, negative),
          .glyphsPerAnimationFrame = 12};
      number.digitCount = values[13] + 1;
      const bool authoredPadding = command.fields.size() >= 14 &&
                                   !command.fields[13].empty();
      number.zeroPadding =
          zeroPaddingMode(authoredPadding ? values[14] : 2);
    } else {
      const int glyphs = source->frames.size() % 10 == 0 ? 10 : 11;
      const std::size_t retained =
          source->frames.size() / static_cast<std::size_t>(glyphs) *
          static_cast<std::size_t>(glyphs);
      std::vector<std::size_t> indices(retained);
      for (std::size_t index = 0; index < indices.size(); ++index) {
        indices[index] = index;
      }
      number.digits = {
          .positive = subsetSprite(*source, indices),
          .glyphsPerAnimationFrame = glyphs};
      number.digitCount = values[13];
      number.zeroPadding = glyphs > 10
                               ? SkinZeroPaddingMode::AlternateZero
                               : SkinZeroPaddingMode::None;
    }
    activeNumber_ = addObject(std::move(number), command);
  }

  void sourceText(const Lr2SkinCommand &command) {
    activeText_.reset();
    const auto values = commandValues(command);
    SkinResourceId font = 0;
    if (values[2] >= 0 &&
        static_cast<std::size_t>(values[2]) < fontResources_.size()) {
      font = fontResources_[static_cast<std::size_t>(values[2])];
    } else {
      font = defaultFont();
    }
    SkinTextObject text{.font = font,
                        .value = bindings_.string(values[3], ordinal_),
                        .pointSize = font == defaultFont_ ? 48 : 0,
                        .alignment = values[4],
                        .editable = values[5] != 0};
    if (text.editable) {
      text.writer = bindings_.stringWriter(values[3], ordinal_);
    }
    activeText_ = addObject(std::move(text), command);
  }

  void sourceSlider(const Lr2SkinCommand &command, bool integerRange) {
    activeSlider_.reset();
    const auto values = commandValues(command);
    const auto sprite = sourceSprite(values);
    if (!sprite) return;
    SkinSliderObject slider{
        .knob = *sprite,
        .direction = values[11],
        .range = static_cast<double>(values[12]),
        .changeable = !integerRange && values[14] == 0};
    if (integerRange) {
      slider.value = SkinSliderObject::IntegerRangeSource{
          .value = bindings_
                       .integer(values[13],
                                SkinIntegerPropertyDomain::IntegerValue,
                                ordinal_)
                       .value_or(SkinIntegerPropertyId{}),
          .minimum = values[15],
          .maximum = values[16]};
    } else {
      slider.value = bindings_
                         .floating(values[13], SkinFloatPropertyDomain::Rate,
                                   ordinal_)
                         .value_or(SkinFloatPropertyId{});
      if (slider.changeable) {
        slider.writer = bindings_.floatWriter(values[13], ordinal_);
      }
    }
    activeSlider_ = addObject(std::move(slider), command);
    if (!integerRange && values[13] == 4) laneCover_ = activeSlider_;
  }

  void sourceGraph(const Lr2SkinCommand &command, bool integerRange) {
    activeGraph_.reset();
    const auto values = commandValues(command);
    SkinGraphObject graph{
        .fill = values[2] >= 100
                    ? SkinSpriteFrames{}
                    : sourceSprite(values).value_or(SkinSpriteFrames{}),
        .builtinImageReference =
            values[2] >= 100 ? std::optional<int>(values[2]) : std::nullopt,
        .direction = values[12]};
    if (integerRange) {
      graph.value = SkinSliderObject::IntegerRangeSource{
          .value = bindings_
                       .integer(values[11],
                                SkinIntegerPropertyDomain::IntegerValue,
                                ordinal_)
                       .value_or(SkinIntegerPropertyId{}),
          .minimum = values[13],
          .maximum = values[14]};
    } else {
      graph.value = bindings_
                        .floating(values[11] + 100,
                                  SkinFloatPropertyDomain::Rate, ordinal_)
                        .value_or(SkinFloatPropertyId{});
    }
    activeGraph_ = addObject(std::move(graph), command);
  }

  void sourceButton(const Lr2SkinCommand &command) {
    activeButton_.reset();
    const auto values = commandValues(command);
    const auto source = sourceSprite(values);
    if (!source) return;
    const int stateCount = values[15] <= 0
                               ? static_cast<int>(source->frames.size())
                               : values[15];
    if (stateCount <= 0 ||
        source->frames.size() % static_cast<std::size_t>(stateCount) != 0) {
      return;
    }
    SkinImageObject button;
    button.stateIndex = bindings_.integer(
        values[11], SkinIntegerPropertyDomain::ImageIndex, ordinal_);
    const std::size_t framesPerState =
        source->frames.size() / static_cast<std::size_t>(stateCount);
    for (int state = 0; state < stateCount; ++state) {
      std::vector<std::size_t> indices;
      for (std::size_t frame = 0; frame < framesPerState; ++frame) {
        indices.push_back(static_cast<std::size_t>(state) * framesPerState +
                          frame);
      }
      button.orderedStates.push_back(subsetSprite(*source, indices));
    }
    if (values[12] == 1) {
      button.clickEvent = bindings_.event(values[11], ordinal_);
      button.clickMode = values[14] > 0 ? 0 : values[14] < 0 ? 1 : 2;
    }
    activeButton_ = addObject(std::move(button), command);
  }

  void sourceOnMouse(const Lr2SkinCommand &command) {
    activeOnMouse_.reset();
    const auto values = commandValues(command);
    const auto sprite = sourceSprite(values);
    if (!sprite) return;
    activeOnMouse_ =
        addObject(SkinImageObject{.orderedStates = {*sprite}}, command);
    if (auto *target = destination(*activeOnMouse_)) {
      target->presentation.mouseRect = SkinAuthoredRect{
          .x = static_cast<double>(values[12]),
          .y = static_cast<double>(values[6] - values[13] - values[15]),
          .width = static_cast<double>(values[14]),
          .height = static_cast<double>(values[15])};
    }
  }

  void sourceGauge(const Lr2SkinCommand &command, bool extended) {
    activeGauge_.reset();
    const auto values = commandValues(command);
    const auto source = sourceSprite(values);
    if (!source) return;
    const std::size_t flickerGroup = extended ? 12U : 6U;
    const bool flicker = values[14] == 3 &&
                         source->frames.size() % flickerGroup == 0;
    const std::size_t group = extended ? (flicker ? 12U : 8U)
                                       : (flicker ? 6U : 4U);
    const std::size_t nodeCount = source->frames.size() / group;
    if (nodeCount == 0) return;
    std::vector<std::array<std::optional<SkinSourceRect>, 36>> nodes(nodeCount);
    for (std::size_t index = 0; index < source->frames.size(); ++index) {
      const std::size_t node = index / group;
      const std::size_t role = index % group;
      if (node >= nodes.size()) continue;
      const auto assign = [&](std::size_t target) {
        if (target < 36) nodes[node][target] = source->frames[index];
      };
      if (!extended && flicker) {
        for (std::size_t offset = 0; offset <= 30; offset += 6) {
          assign(role + offset);
        }
      } else if (!extended) {
        for (std::size_t offset = 0; offset <= 30; offset += 6) {
          assign(role + offset);
        }
        if (role < 2) {
          for (std::size_t offset = 4; offset <= 34; offset += 6) {
            assign(role + offset);
          }
        }
      } else if (flicker) {
        if (role < 4) {
          for (std::size_t offset = 0; offset <= 18; offset += 6) {
            assign(role + offset);
          }
        } else if (role < 8) {
          assign(role + 20);
          assign(role + 26);
        } else if (role == 8 || role == 9) {
          assign(role - 4);
          assign(role + 2);
          assign(role + 8);
          assign(role + 14);
        } else {
          assign(role + 18);
          assign(role + 24);
        }
      } else if (role < 4) {
        for (std::size_t offset = 0; offset <= 18; offset += 6) {
          assign(role + offset);
        }
        if (role < 2) {
          assign(role + 4);
          assign(role + 10);
          assign(role + 16);
          assign(role + 22);
        }
      } else {
        assign(role + 20);
        assign(role + 26);
        if (role < 6) {
          assign(role + 24);
          assign(role + 30);
        }
      }
    }

    SkinGaugeObject gauge;
    gauge.orderedNodes.reserve(36);
    for (std::size_t role = 0; role < 36; ++role) {
      SkinSpriteFrames frames{.resource = source->resource,
                              .cycleMillis = source->cycleMillis,
                              .timer = source->timer};
      frames.frames.reserve(nodeCount);
      for (std::size_t node = 0; node < nodeCount; ++node) {
        frames.frames.push_back(nodes[node][role].value_or(SkinSourceRect{}));
      }
      gauge.orderedNodes.push_back(std::move(frames));
    }
    gauge.parts = values[13] == 0 ? (mode_ && mode_->popn ? 24 : 50)
                                   : values[13];
    gauge.animation = values[13] == 0
                          ? SkinGaugeAnimationType::Random
                          : values[14] >= 0 && values[14] <= 3
                                ? static_cast<SkinGaugeAnimationType>(values[14])
                                : SkinGaugeAnimationType::Random;
    gauge.animationRange =
        values[13] == 0 ? (mode_ && mode_->popn ? 0 : 3) : values[15];
    gauge.animationCycleMillis = values[13] == 0 ? 33 : values[16];
    gauge.resultStartMillis = values[17];
    gauge.resultEndMillis = values[18];
    activeGauge_ = addObject(std::move(gauge), command);
    grooveX_ = values[11];
    grooveY_ = values[12];
  }

  void destinationGauge(const Lr2SkinCommand &command) {
    if (!activeGauge_) return;
    const auto values = commandValues(command);
    const double width = std::abs(grooveX_) >= 1
                             ? static_cast<double>(grooveX_ * 50)
                             : static_cast<double>(values[5]);
    const double height = std::abs(grooveY_) >= 1
                              ? static_cast<double>(grooveY_ * 50)
                              : static_cast<double>(values[6]);
    const double x = static_cast<double>(values[3]) -
                     (grooveX_ < 0 ? grooveX_ : 0);
    const double y = static_cast<double>(header_.height - values[4]) - height;
    applyDestination(*activeGauge_, command,
                     {.x = x, .y = y, .width = width, .height = height});
  }

  void sourceLine(const Lr2SkinCommand &command) {
    const auto values = commandValues(command);
    if (values[1] < 0 || values[1] >= static_cast<int>(lines_.size())) return;
    lines_[static_cast<std::size_t>(values[1])] = {};
    lines_[static_cast<std::size_t>(values[1])].sprite = sourceSprite(values);
  }

  void destinationLine(const Lr2SkinCommand &command) {
    const auto values = commandValues(command);
    if (values[1] < 0 || values[1] >= static_cast<int>(lines_.size())) return;
    auto &line = lines_[static_cast<std::size_t>(values[1])];
    if (!line.sprite) return;
    if (!line.destination) {
      line.destination = SkinDestinationBody{
          .authoredOrdinal = static_cast<std::uint32_t>(values[1])};
    }
    const auto frame = standardFrame(values, true);
    applyDestinationBody(*line.destination, line.control, command, frame,
                         {3});
    const double groupHeight = values[6] < 0
                                   ? static_cast<double>(values[4])
                                   : static_cast<double>(values[4] + values[6]);
    line.groupRect = {.x = frame.x,
                      .y = frame.y,
                      .width = frame.width,
                      .height = groupHeight};
  }

  int laneIndex(int authored) const {
    if (!mode_ || mode_->players <= 0) return -1;
    int lane = authored;
    if (lane % 10 == 0) {
      const int scratch = lane / 10;
      return scratch >= 0 &&
                     static_cast<std::size_t>(scratch) < mode_->scratch.size()
                 ? mode_->scratch[static_cast<std::size_t>(scratch)]
                 : -1;
    }
    const int offset =
        (lane / 10) * (mode_->lanes / mode_->players);
    lane = lane > 10 ? lane - 11 : lane - 1;
    const int keysPerPlayer =
        (mode_->lanes - static_cast<int>(mode_->scratch.size())) /
        mode_->players;
    return lane < 0 || lane >= keysPerPlayer ? -1 : lane + offset;
  }

  void addNoteSource(const Lr2SkinCommand &command,
                     std::vector<std::optional<SkinSpriteFrames>> &slots,
                     bool animated) {
    const auto values = commandValues(command);
    const int lane = laneIndex(values[1]);
    if (lane < 0 || static_cast<std::size_t>(lane) >= slots.size() ||
        slots[static_cast<std::size_t>(lane)]) {
      return;
    }
    slots[static_cast<std::size_t>(lane)] = sourceSprite(values, animated);
  }

  SkinNoteVisual noteVisual(
      const std::vector<std::optional<SkinSpriteFrames>> &slots,
      std::size_t lane, SkinNoteVisualKind kind) const {
    if (lane < slots.size() && slots[lane]) return *slots[lane];
    return SkinSynthesizedNoteVisual{.kind = kind};
  }

  void createNoteObject(const Lr2SkinCommand &command) {
    if (noteObject_ || !mode_) return;
    if (!hcnEnd_.empty() && !hcnEnd_[0]) hcnEnd_ = lnEnd_;
    if (!hcnStart_.empty() && !hcnStart_[0]) hcnStart_ = lnStart_;
    if (!hcnBody_.empty() && !hcnBody_[0]) hcnBody_ = lnBody_;
    if (!hcnBodyActive_.empty() && !hcnBodyActive_[0]) {
      hcnBodyActive_ = lnBodyActive_;
    }
    if (!hcnDamage_.empty() && !hcnDamage_[0]) hcnDamage_ = hcnBody_;
    if (!hcnReactive_.empty() && !hcnReactive_[0]) {
      hcnReactive_ = hcnBodyActive_;
    }

    SkinNoteObject note;
    note.expansionRatePercent = expansionRate_;
    note.hcnBodySlotLayout = SkinHcnBodySlotLayout::Legacy;
    for (std::size_t lane = 0; lane < note_.size(); ++lane) {
      SkinLaneNotePresentation presentation{
          .authoredLane = static_cast<int>(lane)};
      presentation.visuals.emplace(
          SkinNoteVisualKind::Normal,
          noteVisual(note_, lane, SkinNoteVisualKind::Normal));
      presentation.visuals.emplace(
          SkinNoteVisualKind::Mine,
          noteVisual(mine_, lane, SkinNoteVisualKind::Mine));
      presentation.visuals.emplace(
          SkinNoteVisualKind::Hidden,
          SkinSynthesizedNoteVisual{.kind = SkinNoteVisualKind::Hidden});
      presentation.visuals.emplace(
          SkinNoteVisualKind::Processed,
          SkinSynthesizedNoteVisual{.kind = SkinNoteVisualKind::Processed});
      presentation.visuals.emplace(
          SkinNoteVisualKind::LnEnd,
          noteVisual(lnEnd_, lane, SkinNoteVisualKind::LnEnd));
      presentation.visuals.emplace(
          SkinNoteVisualKind::LnStart,
          noteVisual(lnStart_, lane, SkinNoteVisualKind::LnStart));
      presentation.visuals.emplace(
          SkinNoteVisualKind::LnBodyActive,
          noteVisual(lnBodyActive_, lane, SkinNoteVisualKind::LnBodyActive));
      presentation.visuals.emplace(
          SkinNoteVisualKind::LnBodyInactive,
          noteVisual(lnBody_, lane, SkinNoteVisualKind::LnBodyInactive));
      presentation.visuals.emplace(
          SkinNoteVisualKind::HcnEnd,
          noteVisual(hcnEnd_, lane, SkinNoteVisualKind::HcnEnd));
      presentation.visuals.emplace(
          SkinNoteVisualKind::HcnStart,
          noteVisual(hcnStart_, lane, SkinNoteVisualKind::HcnStart));
      presentation.visuals.emplace(
          SkinNoteVisualKind::HcnBodyActive,
          noteVisual(hcnBodyActive_, lane,
                     SkinNoteVisualKind::HcnBodyActive));
      presentation.visuals.emplace(
          SkinNoteVisualKind::HcnBodyInactive,
          noteVisual(hcnBody_, lane, SkinNoteVisualKind::HcnBodyInactive));
      presentation.visuals.emplace(
          SkinNoteVisualKind::HcnDamage,
          noteVisual(hcnDamage_, lane, SkinNoteVisualKind::HcnDamage));
      presentation.visuals.emplace(
          SkinNoteVisualKind::HcnReactive,
          noteVisual(hcnReactive_, lane, SkinNoteVisualKind::HcnReactive));
      note.lanes.push_back(std::move(presentation));
    }
    noteObject_ = addObject(std::move(note), command);
    if (auto *target = destination(*noteObject_)) {
      target->presentation.offsetIds = readOffsets(command, {30});
    }
  }

  void destinationNote(const Lr2SkinCommand &command) {
    if (!mode_) return;
    createNoteObject(command);
    const auto values = commandValues(command);
    const int lane = laneIndex(values[1]);
    if (lane < 0 || static_cast<std::size_t>(lane) >= laneRects_.size() ||
        laneRects_[static_cast<std::size_t>(lane)]) {
      return;
    }
    laneRects_[static_cast<std::size_t>(lane)] = SkinAuthoredRect{
        .x = static_cast<double>(values[3]),
        .y = static_cast<double>(header_.height - (values[4] + values[6])),
        .width = static_cast<double>(values[5]),
        .height = static_cast<double>(values[4] + values[6])};
    noteHeights_[static_cast<std::size_t>(lane)] = values[6];
  }

  SkinJudgeObject *judgeObject(std::size_t player) {
    if (player >= judges_.size() || !judges_[player].object) return nullptr;
    auto *definition = object(*judges_[player].object);
    return definition == nullptr
               ? nullptr
               : std::get_if<SkinJudgeObject>(&definition->payload);
  }

  int judgeGrade(int authored) const {
    return authored <= 5 ? 5 - authored : authored;
  }

  void ensureJudge(std::size_t player, const Lr2SkinCommand &command,
                   int shiftField) {
    if (player >= judges_.size() || judges_[player].object) return;
    SkinJudgeObject judge{.grades = std::vector<SkinJudgeGradePresentation>(7),
                          .player = static_cast<int>(player),
                          .shiftImageByHalfDetailWidth = shiftField != 1};
    judges_[player].object = addObject(std::move(judge), command);
  }

  void sourceJudge(std::size_t player, const Lr2SkinCommand &command) {
    const auto values = commandValues(command);
    const auto sprite = sourceSprite(values);
    if (!sprite) return;
    ensureJudge(player, command, values[11]);
    const int grade = judgeGrade(values[1]);
    if (grade < 0 || grade >= 7) return;
    const SkinObjectId child = addObject(
        SkinImageObject{.orderedStates = {*sprite}}, command, false);
    judges_[player].images[static_cast<std::size_t>(grade)] = child;
    if (auto *judge = judgeObject(player)) {
      judge->grades[static_cast<std::size_t>(grade)].image =
          SkinNestedObjectPresentation{.object = child,
                                       .destination = {
                                           .authoredOrdinal = ordinal_}};
    }
  }

  void destinationJudge(std::size_t player,
                        const Lr2SkinCommand &command) {
    if (player >= judges_.size()) return;
    const auto values = commandValues(command);
    const int grade = judgeGrade(values[1]);
    if (grade < 0 || grade >= 7 ||
        !judges_[player].images[static_cast<std::size_t>(grade)]) {
      return;
    }
    auto *judge = judgeObject(player);
    if (judge == nullptr ||
        !judge->grades[static_cast<std::size_t>(grade)].image) {
      return;
    }
    auto &nested = *judge->grades[static_cast<std::size_t>(grade)].image;
    const int judgeOffset = 32;
    applyDestinationBody(nested.destination,
                         destinationControls_[nested.object], command,
                         standardFrame(values, true), {judgeOffset, 3});
    // The first destination per player is where the pinned loader appends
    // its optional judge-detail fallback objects. Their conditions keep them
    // dormant unless the synthesized header option selects 1998/1999.
    if (!judges_[player].detailsAdded) {
      judges_[player].detailsAdded = true;
      addJudgeDetails(player, values, command);
    }
  }

  SkinDigitSpriteSet digitRows(SkinResourceId resource, int firstRow,
                               std::optional<int> negativeRow,
                               std::optional<SkinTimerPropertyId> timer) {
    SkinSpriteFrames positive{.resource = resource, .timer = timer};
    SkinSpriteFrames negative{.resource = resource, .timer = timer};
    for (int glyph = 0; glyph < 10; ++glyph) {
      positive.frames.push_back(
          {.x = glyph * 10, .y = firstRow * 20, .w = 10, .h = 20});
      if (negativeRow) {
        negative.frames.push_back(
            {.x = glyph * 10, .y = *negativeRow * 20, .w = 10, .h = 20});
      }
    }
    return {.positive = std::move(positive),
            .negative = negativeRow
                            ? std::optional<SkinSpriteFrames>(
                                  std::move(negative))
                            : std::nullopt,
            .glyphsPerAnimationFrame = 10};
  }

  SkinResourceId judgeDetailResource() {
    if (judgeDetailResource_ != 0) return judgeDetailResource_;
    const SkinResourceId id =
        static_cast<SkinResourceId>(model_.resources.size() + 1);
    model_.resources.emplace_back(SkinImageResource{
        .id = id,
        .authoredName = "lr2-judge-detail",
        .virtualPath = "skin/default/judgedetail.png",
        .authoredOrdinal = ordinal_});
    judgeDetailResource_ = id;
    return id;
  }

  SkinResourceId defaultLineResource() {
    if (defaultLineResource_ != 0) return defaultLineResource_;
    const SkinResourceId id =
        static_cast<SkinResourceId>(model_.resources.size() + 1);
    model_.resources.emplace_back(SkinImageResource{
        .id = id,
        .authoredName = "lr2-default-line",
        .virtualPath = "skin/default/system.png",
        .authoredOrdinal = ordinal_});
    defaultLineResource_ = id;
    return id;
  }

  void addJudgeDetails(std::size_t player,
                       const std::array<int, 22> &judgeValues,
                       const Lr2SkinCommand &command) {
    static constexpr std::array<int, 3> timers{46, 47, 247};
    static constexpr std::array<int, 3> early{1242, 1262, 1362};
    static constexpr std::array<int, 3> late{1243, 1263, 1363};
    static constexpr std::array<int, 3> perfect{241, 261, 361};
    static constexpr std::array<int, 3> values{525, 526, 527};
    if (player >= timers.size()) return;
    if (maximumFrames_ < 8 || modelFrameCount_ > maximumFrames_ - 8) {
      throw std::length_error("LR2 judge-detail frame limit");
    }
    modelFrameCount_ += 8;
    const SkinResourceId resource = judgeDetailResource();
    const double x = judgeValues[3] + judgeValues[5] / 2.0;
    const double y = header_.height - (judgeValues[4] - 5.0);
    const double imageWidth = 40.0 * header_.width / 1280.0;
    const double imageHeight = 16.0 * header_.height / 720.0;
    const double numberWidth = 8.0 * header_.width / 1280.0;

    const auto appendImage = [&](int sourceX, int option) {
      SkinSpriteFrames sprite{.resource = resource,
                              .frames = {{.x = sourceX,
                                          .y = 0,
                                          .w = 50,
                                          .h = 20}}};
      const SkinObjectId id = addObject(
          SkinImageObject{.orderedStates = {std::move(sprite)}}, command);
      auto *target = destination(id);
      if (target == nullptr) return;
      target->presentation.timer = bindings_.timer(timers[player], ordinal_);
      target->presentation.conditions.emplace_back(1998);
      if (const auto condition = bindings_.boolean(option, ordinal_)) {
        target->presentation.conditions.emplace_back(*condition);
      }
      target->presentation.offsetIds = {33, 3};
      target->presentation.frames = {
          {.timeMillis = 0,
           .x = x,
           .y = y,
           .width = imageWidth,
           .height = imageHeight,
           .rgba = {255, 255, 255, 255}},
          {.timeMillis = 500,
           .x = x,
           .y = y,
           .width = imageWidth,
           .height = imageHeight,
           .rgba = {255, 255, 255, 255}}};
    };
    appendImage(0, early[player]);
    appendImage(50, late[player]);

    const auto appendNumber = [&](int positiveRow, int negativeRow,
                                  int option) {
      const auto timer = bindings_.timer(timers[player], ordinal_);
      SkinNumberObject number{
          .digits = digitRows(resource, positiveRow, negativeRow, timer),
          .value = bindings_
                       .integer(values[player],
                                SkinIntegerPropertyDomain::IntegerValue,
                                ordinal_)
                       .value_or(SkinIntegerPropertyId{}),
          .digitCount = 4,
          .spacing = judgeValues[15],
          .alignment = judgeValues[12]};
      const SkinObjectId id = addObject(std::move(number), command);
      auto *target = destination(id);
      if (target == nullptr) return;
      target->presentation.timer = timer;
      target->presentation.conditions.emplace_back(1999);
      if (const auto condition = bindings_.boolean(option, ordinal_)) {
        target->presentation.conditions.emplace_back(*condition);
      }
      target->presentation.offsetIds = {33, 3};
      target->presentation.frames = {
          {.timeMillis = 0,
           .x = x,
           .y = y,
           .width = numberWidth,
           .height = imageHeight,
           .rgba = {255, 255, 255, 255}},
          {.timeMillis = 500,
           .x = x,
           .y = y,
           .width = numberWidth,
           .height = imageHeight,
           .rgba = {255, 255, 255, 255}}};
    };
    appendNumber(1, 2, perfect[player]);
    appendNumber(3, 4, -perfect[player]);
  }

  void sourceCombo(std::size_t player, const Lr2SkinCommand &command) {
    if (player >= judges_.size() || !judges_[player].object) return;
    const auto values = commandValues(command);
    const auto source = sourceSprite(values);
    if (!source) return;
    const int columns = values[7] > 0 ? values[7] : 1;
    const int rows = values[8] > 0 ? values[8] : 1;
    if (source->frames.size() !=
        static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows)) {
      return;
    }
    SkinNumberObject number{
        .digits = {.positive = *source,
                   .glyphsPerAnimationFrame = columns},
        .value = bindings_
                     .integer(values[11],
                              SkinIntegerPropertyDomain::IntegerValue,
                              ordinal_)
                     .value_or(SkinIntegerPropertyId{}),
        .digitCount = values[13],
        .spacing = values[15],
        .alignment = values[12] == 1 ? 2 : values[12],
        .relativeToJudgeImage = true,
        .zeroPadding = rows > 10
                           ? SkinZeroPaddingMode::AlternateZero
                           : SkinZeroPaddingMode::None};
    const SkinObjectId child = addObject(std::move(number), command, false);
    const int grade = judgeGrade(values[1]);
    if (grade < 0 || grade >= 7) return;
    judges_[player].numbers[static_cast<std::size_t>(grade)] = child;
    if (auto *judge = judgeObject(player)) {
      judge->grades[static_cast<std::size_t>(grade)].detailNumber =
          SkinNestedObjectPresentation{.object = child,
                                       .destination = {
                                           .authoredOrdinal = ordinal_}};
    }
  }

  void destinationCombo(std::size_t player,
                        const Lr2SkinCommand &command) {
    if (player >= judges_.size()) return;
    const auto values = commandValues(command);
    const int grade = judgeGrade(values[1]);
    auto *judge = judgeObject(player);
    if (grade < 0 || grade >= 7 || judge == nullptr ||
        !judge->grades[static_cast<std::size_t>(grade)].detailNumber) {
      return;
    }
    auto &nested = *judge->grades[static_cast<std::size_t>(grade)].detailNumber;
    double x = values[3];
    const auto *child = object(nested.object);
    const auto *number = child == nullptr
                             ? nullptr
                             : std::get_if<SkinNumberObject>(&child->payload);
    if (number != nullptr && number->alignment == 2) {
      x -= number->digitCount * values[5] / 2.0;
    }
    SkinDestinationFrame frame{.x = x,
                               .y = static_cast<double>(-values[4]),
                               .width = static_cast<double>(values[5]),
                               .height = static_cast<double>(values[6])};
    applyDestinationBody(nested.destination,
                         destinationControls_[nested.object], command, frame,
                         {32, 3});
  }

  void sourceBga(const Lr2SkinCommand &command) {
    bga_ = addObject(SkinBgaObject{}, command);
  }

  void sourceGraphObject(const Lr2SkinCommand &command) {
    const auto values = commandValues(command);
    if (command.name == "SRC_NOTECHART_1P") {
      if (values[1] < 0 || values[1] > 2) {
        throw std::out_of_range("LR2 note-chart type");
      }
      noteChart_ = addObject(
          SkinNoteDistributionGraphObject{
              .type = static_cast<SkinNoteDistributionGraphType>(values[1]),
              .backgroundTextureOff = values[16] == 1,
              .delayMillis = values[15],
              .reverseOrder = values[17] == 1,
              .noGap = values[18] == 1,
              .noHorizontalGap = values[19] == 1},
          command);
      graphSize_ = {values[11], values[12]};
      return;
    }
    if (command.name == "SRC_BPMCHART") {
      const int delay = values[3] > 0 ? values[3] : 0;
      const int lineWidth = values[4] > 0 ? values[4] : 2;
      bpmChart_ = addObject(
          SkinBpmGraphObject{
              .delayMillis = delay,
              .lineWidth = lineWidth,
              .mainRgba = bpmColor(requiredField(command, 5), 0x00ff00ffU),
              .minimumRgba = bpmColor(requiredField(command, 6), 0x0000ffffU),
              .maximumRgba = bpmColor(requiredField(command, 7), 0xff0000ffU),
              .otherRgba = bpmColor(requiredField(command, 8), 0xffff00ffU),
              .stopRgba = bpmColor(requiredField(command, 9), 0xff00ffffU),
              .transitionRgba =
                  bpmColor(requiredField(command, 10), 0x7f7f7fffU)},
          command);
      graphSize_ = {values[1], values[2]};
      return;
    }
    if (command.name == "SRC_TIMING_1P") {
      SkinTimingVisualizerObject visualizer{
          .width = values[4],
          .judgeWidthMillis = values[6],
          .lineWidth = std::clamp(values[7], 1, 4),
          .judgeRgba = {timingColor(requiredField(command, 10)),
                        timingColor(requiredField(command, 11)),
                        timingColor(requiredField(command, 12)),
                        timingColor(requiredField(command, 13)),
                        values[15] == 1
                            ? 0x00000000U
                            : timingColor(requiredField(command, 14))},
          .lineRgba = timingColor(requiredField(command, 8)),
          .centerRgba = timingColor(requiredField(command, 9)),
          .transparent = values[15] == 1,
          .drawDecay = values[16] == 1};
      timingChart_ = addObject(std::move(visualizer), command);
      graphSize_ = {values[4], values[5]};
    }
  }

  void destinationGraphObject(const Lr2SkinCommand &command,
                              std::optional<SkinObjectId> id,
                              std::array<int, 2> size) {
    if (!id) return;
    const auto values = commandValues(command);
    applyDestination(*id, command,
                     {.x = static_cast<double>(values[3]),
                      .y = static_cast<double>(header_.height - values[4]),
                      .width = static_cast<double>(size[0]),
                      .height = static_cast<double>(size[1])});
  }

  void sourceCover(const Lr2SkinCommand &command, SkinCoverKind kind) {
    activeCover_.reset();
    const auto values = commandValues(command);
    const auto sprite = sourceSprite(values);
    if (!sprite) return;
    const bool hasDisappear = command.fields.size() >= 11 &&
                              !command.fields[10].empty() && values[11] > 0;
    const bool linksLift = command.fields.size() < 12 ||
                           command.fields[11].empty() || values[12] != 0;
    activeCover_ = addObject(
        SkinCoverObject{
            .kind = kind,
            .sprite = *sprite,
            .disappearLine = hasDisappear
                                 ? static_cast<double>(header_.height -
                                                       values[11])
                                 : -1.0,
            .disappearLineLinksLift = linksLift},
        command);
  }

  void destinationCover(const Lr2SkinCommand &command, SkinCoverKind kind) {
    if (!activeCover_) return;
    const auto values = commandValues(command);
    if (kind == SkinCoverKind::Hidden) {
      applyDestination(*activeCover_, command, standardFrame(values, true),
                       {3, 5});
    } else {
      applyDestination(*activeCover_, command, standardFrame(values, true),
                       {3});
    }
  }

  void addPmDestination(SkinObjectId id, const Lr2SkinCommand &command,
                        int x, int y, int width, int height,
                        std::optional<int> timer = std::nullopt,
                        std::array<int, 3> conditions = {},
                        std::optional<int> offset = std::nullopt) {
    auto *target = destination(id);
    if (target == nullptr) return;
    if (timer) target->presentation.timer = bindings_.timer(*timer, ordinal_);
    for (const int condition : conditions) {
      if (condition == 0) continue;
      if (const auto boolean = bindings_.boolean(condition, ordinal_)) {
        target->presentation.conditions.emplace_back(*boolean);
      } else {
        target->presentation.conditions.emplace_back(condition);
      }
    }
    if (offset && *offset > 0 && *offset <= kMaximumOffsetId) {
      target->presentation.offsetIds.push_back(*offset);
    }
    target->source = command.source;
    target->presentation.frames.push_back(
        {.timeMillis = 0,
         .x = static_cast<double>(x),
         .y = static_cast<double>(header_.height - (y + height)),
         .width = static_cast<double>(width),
         .height = static_cast<double>(height),
         .rgba = {255, 255, 255, 255}});
  }

  void directPm(const Lr2SkinCommand &command, int side) {
    auto values = commandValues(command);
    if (values[3] < 0) {
      values[1] += values[3];
      values[3] = -values[3];
    }
    if (values[4] < 0) {
      values[2] += values[4];
      values[4] = -values[4];
    }
    const std::string path =
        normalizedPath(std::string(requiredField(command, 7)));
    const SkinObjectId id = addObject(
        SkinPmCharaObject{.sourceName = path,
                          .sourcePath = path,
                          .color = values[5] == 2 ? 2 : 1,
                          .type = 0,
                          .side = side},
        command);
    addPmDestination(id, command, values[1], values[2], values[3], values[4],
                     std::nullopt, {}, values[6]);
  }

  void animatedPm(const Lr2SkinCommand &command) {
    auto values = commandValues(command);
    if (values[6] < 0 || values[6] > 9) return;
    if (values[3] < 0) {
      values[1] += values[3];
      values[3] = -values[3];
    }
    if (values[4] < 0) {
      values[2] += values[4];
      values[4] = -values[4];
    }
    const std::string path =
        normalizedPath(std::string(requiredField(command, 12)));
    const SkinObjectId id = addObject(
        SkinPmCharaObject{.sourceName = path,
                          .sourcePath = path,
                          .color = values[5] == 2 ? 2 : 1,
                          .type = values[6] + 6,
                          .side = 1},
        command);
    addPmDestination(id, command, values[1], values[2], values[3], values[4],
                     values[7], {values[8], values[9], values[10]}, values[11]);
  }

  void sourcePmImage(const Lr2SkinCommand &command) {
    activePmImage_.reset();
    const auto values = commandValues(command);
    if (values[2] < 0 || values[2] > 4) return;
    const std::string path =
        normalizedPath(std::string(requiredField(command, 3)));
    activePmImage_ = addObject(
        SkinPmCharaObject{.sourceName = path,
                          .sourcePath = path,
                          .color = values[1] == 2 ? 2 : 1,
                          .type = values[2] + 1,
                          .side = 1},
        command);
  }

  void destinationPmImage(const Lr2SkinCommand &command) {
    if (!activePmImage_) return;
    const auto values = commandValues(command);
    applyDestination(*activePmImage_, command, standardFrame(values, true));
  }

  void processCommand(const Lr2SkinCommand &command) {
    // The parser has already inserted the included command stream in place.
    if (command.name == "INCLUDE") return;
    if (command.name == "IMAGE") return addImageResource(command);
    if (command.name == "LR2FONT") return addFontResource(command);
    if (command.name == "SRC_IMAGE") return sourceImage(command);
    if (command.name == "IMAGESET") return defineImageSet(command);
    if (command.name == "SRC_IMAGESET") return sourceImageSet(command);
    if (command.name == "DST_IMAGE") {
      if (activeImage_) {
        const auto values = commandValues(command);
        applyDestination(*activeImage_, command, standardFrame(values, true),
                         {}, true);
      }
      return;
    }
    if (command.name == "SRC_NUMBER") return sourceNumber(command);
    if (command.name == "DST_NUMBER") {
      if (activeNumber_) {
        const auto values = commandValues(command);
        applyDestination(*activeNumber_, command, standardFrame(values));
      }
      return;
    }
    if (command.name == "SRC_TEXT") return sourceText(command);
    if (command.name == "DST_TEXT") {
      if (activeText_) {
        const auto values = commandValues(command);
        applyDestination(*activeText_, command, standardFrame(values));
      }
      return;
    }
    if (command.name == "SRC_SLIDER") return sourceSlider(command, false);
    if (command.name == "SRC_SLIDER_REFNUMBER") {
      return sourceSlider(command, true);
    }
    if (command.name == "DST_SLIDER") {
      if (activeSlider_) {
        const auto values = commandValues(command);
        applyDestination(*activeSlider_, command, standardFrame(values));
      }
      return;
    }
    if (command.name == "SRC_BARGRAPH") return sourceGraph(command, false);
    if (command.name == "SRC_BARGRAPH_REFNUMBER") {
      return sourceGraph(command, true);
    }
    if (command.name == "DST_BARGRAPH") {
      if (activeGraph_) {
        auto values = commandValues(command);
        const auto *definition = object(*activeGraph_);
        const auto *graph = definition == nullptr
                                ? nullptr
                                : std::get_if<SkinGraphObject>(
                                      &definition->payload);
        if (graph != nullptr && graph->direction == 1) {
          values[4] += values[6];
          values[6] = -values[6];
        }
        applyDestination(*activeGraph_, command, standardFrame(values));
      }
      return;
    }
    if (command.name == "SRC_BUTTON") return sourceButton(command);
    if (command.name == "DST_BUTTON") {
      if (activeButton_) {
        const auto values = commandValues(command);
        applyDestination(*activeButton_, command, standardFrame(values));
      }
      return;
    }
    if (command.name == "SRC_ONMOUSE") return sourceOnMouse(command);
    if (command.name == "DST_ONMOUSE") {
      if (activeOnMouse_) {
        const auto values = commandValues(command);
        applyDestination(*activeOnMouse_, command, standardFrame(values));
      }
      return;
    }
    if (command.name == "SRC_GROOVEGAUGE") return sourceGauge(command, false);
    if (command.name == "SRC_GROOVEGAUGE_EX") {
      return sourceGauge(command, true);
    }
    if (command.name == "DST_GROOVEGAUGE") return destinationGauge(command);
    if (command.name == "STARTINPUT") {
      model_.timing.inputMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "SCENETIME") {
      model_.timing.sceneMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "FADEOUT") {
      model_.timing.fadeoutMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "STRETCH") {
      stretch_ = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "FINISHMARGIN") {
      model_.timing.finishMarginMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "JUDGETIMER") {
      model_.timing.judgeTimerMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "SRC_BGA") return sourceBga(command);
    if (command.name == "DST_BGA") {
      if (bga_) {
        const auto values = commandValues(command);
        applyDestination(*bga_, command, standardFrame(values), {}, true, 0);
      }
      return;
    }
    if (command.name == "SRC_LINE") return sourceLine(command);
    if (command.name == "DST_LINE") return destinationLine(command);
    if (command.name == "SRC_NOTE") {
      return addNoteSource(command, note_, true);
    }
    if (command.name == "SRC_LN_END") {
      return addNoteSource(command, lnEnd_, true);
    }
    if (command.name == "SRC_LN_START") {
      return addNoteSource(command, lnStart_, true);
    }
    if (command.name == "SRC_LN_BODY") {
      addNoteSource(command, lnBody_, false);
      addNoteSource(command, lnBodyActive_, true);
      return;
    }
    if (command.name == "SRC_LN_BODY_INACTIVE") {
      return addNoteSource(command, lnBody_, true);
    }
    if (command.name == "SRC_LN_BODY_ACTIVE") {
      return addNoteSource(command, lnBodyActive_, true);
    }
    if (command.name == "SRC_HCN_END") {
      return addNoteSource(command, hcnEnd_, true);
    }
    if (command.name == "SRC_HCN_START") {
      return addNoteSource(command, hcnStart_, true);
    }
    if (command.name == "SRC_HCN_BODY") {
      addNoteSource(command, hcnBody_, false);
      addNoteSource(command, hcnBodyActive_, true);
      return;
    }
    if (command.name == "SRC_HCN_BODY_INACTIVE") {
      return addNoteSource(command, hcnBody_, true);
    }
    if (command.name == "SRC_HCN_BODY_ACTIVE") {
      return addNoteSource(command, hcnBodyActive_, true);
    }
    if (command.name == "SRC_HCN_DAMAGE") {
      return addNoteSource(command, hcnDamage_, true);
    }
    if (command.name == "SRC_HCN_REACTIVE") {
      return addNoteSource(command, hcnReactive_, true);
    }
    if (command.name == "SRC_MINE") {
      return addNoteSource(command, mine_, true);
    }
    if (command.name == "DST_NOTE") return destinationNote(command);
    if (command.name == "DST_NOTE2") {
      const int authored = requiredStrictInteger(command, 1);
      const int firstHeight = !noteHeights_.empty() && noteHeights_[0]
                                  ? *noteHeights_[0]
                                  : 0;
      secondaryDestinationY_ = header_.height - (authored + firstHeight);
      return;
    }
    if (command.name == "DST_NOTE_EXPANSION_RATE") {
      expansionRate_ = {requiredStrictInteger(command, 1),
                        requiredStrictInteger(command, 2)};
      if (noteObject_) {
        if (auto *definition = object(*noteObject_)) {
          if (auto *note = std::get_if<SkinNoteObject>(&definition->payload)) {
            note->expansionRatePercent = expansionRate_;
          }
        }
      }
      return;
    }
    if (command.name == "SRC_NOWJUDGE_1P") return sourceJudge(0, command);
    if (command.name == "DST_NOWJUDGE_1P") return destinationJudge(0, command);
    if (command.name == "SRC_NOWJUDGE_2P") return sourceJudge(1, command);
    if (command.name == "DST_NOWJUDGE_2P") return destinationJudge(1, command);
    if (command.name == "SRC_NOWJUDGE_3P") return sourceJudge(2, command);
    if (command.name == "DST_NOWJUDGE_3P") return destinationJudge(2, command);
    if (command.name == "SRC_NOWCOMBO_1P") return sourceCombo(0, command);
    if (command.name == "DST_NOWCOMBO_1P") return destinationCombo(0, command);
    if (command.name == "SRC_NOWCOMBO_2P") return sourceCombo(1, command);
    if (command.name == "DST_NOWCOMBO_2P") return destinationCombo(1, command);
    if (command.name == "SRC_NOWCOMBO_3P") return sourceCombo(2, command);
    if (command.name == "DST_NOWCOMBO_3P") return destinationCombo(2, command);
    if (command.name == "SRC_JUDGELINE") {
      activeJudgeLine_.reset();
      const auto values = commandValues(command);
      const auto sprite = sourceSprite(values);
      if (sprite) {
        activeJudgeLine_ = addObject(
            SkinImageObject{.orderedStates = {*sprite}}, command);
      }
      return;
    }
    if (command.name == "DST_JUDGELINE") {
      if (activeJudgeLine_) {
        const auto values = commandValues(command);
        applyDestination(*activeJudgeLine_, command,
                         standardFrame(values, true), {3});
      }
      return;
    }
    if (command.name == "SRC_NOTECHART_1P" ||
        command.name == "SRC_BPMCHART" ||
        command.name == "SRC_TIMING_1P") {
      return sourceGraphObject(command);
    }
    if (command.name == "DST_NOTECHART_1P") {
      return destinationGraphObject(command, noteChart_, graphSize_);
    }
    if (command.name == "DST_BPMCHART") {
      return destinationGraphObject(command, bpmChart_, graphSize_);
    }
    if (command.name == "DST_TIMING_1P") {
      return destinationGraphObject(command, timingChart_, graphSize_);
    }
    if (command.name == "SRC_HIDDEN") {
      return sourceCover(command, SkinCoverKind::Hidden);
    }
    if (command.name == "DST_HIDDEN") {
      return destinationCover(command, SkinCoverKind::Hidden);
    }
    if (command.name == "SRC_LIFT") {
      return sourceCover(command, SkinCoverKind::Lift);
    }
    if (command.name == "DST_LIFT") {
      return destinationCover(command, SkinCoverKind::Lift);
    }
    if (command.name == "DST_PM_CHARA_1P") return directPm(command, 1);
    if (command.name == "DST_PM_CHARA_2P") return directPm(command, 2);
    if (command.name == "DST_PM_CHARA_ANIMATION") return animatedPm(command);
    if (command.name == "SRC_PM_CHARA_IMAGE") return sourcePmImage(command);
    if (command.name == "DST_PM_CHARA_IMAGE") {
      return destinationPmImage(command);
    }
    if (command.name == "CLOSE") {
      model_.timing.closeMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "PLAYSTART") {
      model_.timing.playStartMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "LOADSTART") {
      model_.timing.loadStartMillis = requiredStrictInteger(command, 1);
      return;
    }
    if (command.name == "LOADEND") {
      model_.timing.loadEndMillis = requiredStrictInteger(command, 1);
      return;
    }
    // INCLUDE is already expanded by Lr2SkinCsvParser. Header declarations
    // and commands owned by other LR2 screen loaders are silently ignored by
    // the pinned gameplay loader's absent command-map entry.
  }

  void finalizeNote() {
    if (!noteObject_) return;
    auto *definition = object(*noteObject_);
    auto *note = definition == nullptr
                     ? nullptr
                     : std::get_if<SkinNoteObject>(&definition->payload);
    if (note == nullptr) return;

    double laneCoverPosition = -1.0;
    if (laneCover_) {
      if (const auto *coverDestination = destination(*laneCover_);
          coverDestination != nullptr &&
          !coverDestination->presentation.frames.empty()) {
        laneCoverPosition = coverDestination->presentation.frames.back().y;
      }
    }
    for (std::size_t lane = 0; lane < note->lanes.size(); ++lane) {
      if (lane < laneRects_.size() && laneRects_[lane]) {
        note->lanes[lane].laneDestination = *laneRects_[lane];
        if (laneCoverPosition > 0) {
          note->lanes[lane].laneDestination.height -=
              header_.height - laneCoverPosition;
        }
      }
      if (lane < noteHeights_.size() && noteHeights_[lane]) {
        note->lanes[lane].authoredNoteHeight = *noteHeights_[lane];
      }
      note->lanes[lane].secondaryDestinationY = secondaryDestinationY_;
    }
    note->expansionRatePercent = expansionRate_;

    const std::size_t groups = lines_[0].sprite
                                   ? (lines_[1].sprite ? 2U : 1U)
                                   : 0U;
    const auto appendLine = [&](std::size_t index, SkinNoteLineKind kind,
                                std::size_t group) {
      SkinNoteLinePresentation presentation{
          .kind = kind,
          .laneGroupDestination = lines_[group].groupRect};
      if (index < lines_.size() && lines_[index].sprite) {
        presentation.sprite = lines_[index].sprite;
        presentation.destination = lines_[index].destination;
      } else if (lines_[group].destination) {
        presentation.sprite = SkinSpriteFrames{
            .resource = defaultLineResource(),
            .frames = {{.x = 0, .y = 0, .w = 1, .h = 1}}};
        presentation.destination = lines_[group].destination;
        const double multiplier =
            kind == SkinNoteLineKind::Bpm || kind == SkinNoteLineKind::Stop
                ? 2.0
                : 1.0;
        const std::array<std::uint8_t, 4> rgba =
            kind == SkinNoteLineKind::Bpm
                ? std::array<std::uint8_t, 4>{0, 192, 0, 255}
                : kind == SkinNoteLineKind::Stop
                      ? std::array<std::uint8_t, 4>{192, 192, 0, 255}
                      : std::array<std::uint8_t, 4>{64, 192, 192, 255};
        for (auto &frame : presentation.destination->frames) {
          frame.height *= multiplier;
          frame.rgba = rgba;
        }
      }
      note->lines.push_back(std::move(presentation));
    };
    for (std::size_t group = 0; group < groups; ++group) {
      appendLine(group, SkinNoteLineKind::Group, group);
    }
    for (std::size_t group = 0; group < groups; ++group) {
      appendLine(group + 2, SkinNoteLineKind::Bpm, group);
    }
    for (std::size_t group = 0; group < groups; ++group) {
      appendLine(group + 4, SkinNoteLineKind::Stop, group);
    }
    for (std::size_t group = 0; group < groups; ++group) {
      appendLine(group + 6, SkinNoteLineKind::Time, group);
    }
  }

  const BeatorajaSkinHeader &header_;
  BindingRegistry bindings_;
  Lr2GameplaySkinDecodeResult &result_;
  std::stop_token stop_;
  StaticSkinDecodeCheckpoint checkpoint_;
  std::size_t workItem_ = 0;
  BeatorajaSkinModel model_;
  std::optional<ModeInfo> mode_;
  std::map<int, int> options_;
  bool skip_ = false;
  bool ifs_ = false;
  std::uint32_t ordinal_ = 0;
  std::size_t modelFrameCount_ = 0;
  std::size_t maximumObjects_ = kMaximumObjects;
  std::size_t maximumFrames_ = kMaximumFrames;
  int stretch_ = -1;
  std::map<SkinObjectId, DestinationControl> destinationControls_;
  std::map<SkinObjectId, std::size_t> destinationIndices_;

  std::vector<SkinResourceId> imageResources_;
  std::vector<bool> imageMovies_;
  std::vector<SkinResourceId> fontResources_;
  std::vector<SkinSpriteFrames> imageSets_;
  SkinResourceId defaultFont_ = 0;
  SkinResourceId judgeDetailResource_ = 0;
  SkinResourceId defaultLineResource_ = 0;

  std::optional<SkinObjectId> activeImage_;
  std::optional<SkinObjectId> activeNumber_;
  std::optional<SkinObjectId> activeText_;
  std::optional<SkinObjectId> activeSlider_;
  std::optional<SkinObjectId> laneCover_;
  std::optional<SkinObjectId> activeGraph_;
  std::optional<SkinObjectId> activeButton_;
  std::optional<SkinObjectId> activeOnMouse_;
  std::optional<SkinObjectId> activeGauge_;
  int grooveX_ = 0;
  int grooveY_ = 0;
  std::optional<SkinObjectId> bga_;
  std::optional<SkinObjectId> activeJudgeLine_;
  std::optional<SkinObjectId> activeCover_;
  std::optional<SkinObjectId> activePmImage_;

  std::array<LineSlot, 8> lines_;
  std::vector<std::optional<SkinSpriteFrames>> note_;
  std::vector<std::optional<SkinSpriteFrames>> lnEnd_;
  std::vector<std::optional<SkinSpriteFrames>> lnStart_;
  std::vector<std::optional<SkinSpriteFrames>> lnBody_;
  std::vector<std::optional<SkinSpriteFrames>> lnBodyActive_;
  std::vector<std::optional<SkinSpriteFrames>> hcnEnd_;
  std::vector<std::optional<SkinSpriteFrames>> hcnStart_;
  std::vector<std::optional<SkinSpriteFrames>> hcnBody_;
  std::vector<std::optional<SkinSpriteFrames>> hcnBodyActive_;
  std::vector<std::optional<SkinSpriteFrames>> hcnDamage_;
  std::vector<std::optional<SkinSpriteFrames>> hcnReactive_;
  std::vector<std::optional<SkinSpriteFrames>> mine_;
  std::vector<std::optional<SkinAuthoredRect>> laneRects_;
  std::vector<std::optional<double>> noteHeights_;
  std::optional<double> secondaryDestinationY_;
  std::array<int, 2> expansionRate_{100, 100};
  std::optional<SkinObjectId> noteObject_;
  std::array<JudgeBuildState, 3> judges_;

  std::optional<SkinObjectId> noteChart_;
  std::optional<SkinObjectId> bpmChart_;
  std::optional<SkinObjectId> timingChart_;
  std::array<int, 2> graphSize_{};
};

} // namespace

Lr2GameplaySkinConfigurationResult reconcileLr2GameplaySkinConfiguration(
    const BeatorajaSkinHeader &header,
    const EntryProfileSettings *desired) {
  Lr2GameplaySkinConfigurationResult result;
  (void)reconcileConfiguration(header, desired, result);
  return result;
}

Lr2GameplaySkinDecodeResult Lr2GameplaySkinDecoder::decode(
    const BeatorajaSkinHeader &header,
    std::span<const Lr2SkinCommand> commands,
    const EntryProfileSettings *desired,
    SkinBuiltinBindingCatalogView builtins,
    SkinSafetyPolicy safetyPolicy, std::stop_token stop,
    StaticSkinDecodeCheckpoint checkpoint) const {
  Lr2GameplaySkinDecodeResult result;
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  if (!gameplaySkinTraitForSkinType(header.type)) {
    result.fatal = true;
    result.diagnostics.push_back(documentDiagnostic(
        "skin_lr2_not_gameplay",
        "LR2 document header is not a gameplay skin type"));
    return result;
  }
  const auto maximumCommands = static_cast<std::size_t>(safetyPolicy.limit(
      SkinSafetyGuard::LuaDecoderLimit, kMaximumCommands));
  if (commands.size() > maximumCommands) {
    result.fatal = true;
    result.diagnostics.push_back(documentDiagnostic(
        "skin_lr2_gameplay_limit_exceeded",
        "LR2 gameplay command stream exceeds the fixed command limit"));
    return result;
  }
  try {
    if (!reconcileConfiguration(header, desired, result) ||
        !result.configuration) {
      return result;
    }
    DecodeSession session(header, *result.configuration, builtins,
                          safetyPolicy, stop, checkpoint, result);
    session.execute(commands);
    if (result.cancelled) return result;
    result.model = session.takeModel();
  } catch (...) {
    result.fatal = true;
    result.model.reset();
    result.configuration.reset();
    result.reconciledSettings.reset();
    result.diagnostics.push_back(documentDiagnostic(
        "skin_lr2_gameplay_limit_exceeded",
        "LR2 gameplay document could not be retained within host limits"));
  }
  return result;
}

} // namespace skin
