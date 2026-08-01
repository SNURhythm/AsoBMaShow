#include "BeatorajaReplayCodec.h"

#include "Base64Url.h"
#include "BeatorajaLongNoteMode.h"
#include "GzipCodec.h"
#include "ReplayKeyMode.h"
#include "ReplayOption.h"

#include "../bms_parser.hpp"
#include "../scene/play/GameplayScoreState.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace replay {
namespace {

using Json = nlohmann::ordered_json;
using Bytes = std::vector<std::byte>;

constexpr int kAsoSchemaVersion = BeatorajaReplayCodec::kCodecVersion;
constexpr std::size_t kKeyRecordSize = 9;

bool fail(std::string &diagnostic, std::string message) {
  diagnostic = std::move(message);
  return false;
}

bool validJsonDepth(std::string_view source, std::size_t maximum,
                    std::string &diagnostic) {
  std::size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (const char ch : source) {
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
    } else if (ch == '{' || ch == '[') {
      if (++depth > maximum) {
        return fail(diagnostic, "Replay JSON nesting exceeds the limit");
      }
    } else if ((ch == '}' || ch == ']') && depth > 0) {
      --depth;
    }
  }
  return true;
}

std::int64_t littleEndianInt64(std::span<const std::byte> value,
                               std::size_t offset) noexcept {
  std::uint64_t result = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    result |= static_cast<std::uint64_t>(
                  std::to_integer<unsigned char>(value[offset++]))
              << shift;
  }
  return static_cast<std::int64_t>(result);
}

void appendLittleEndianInt64(Bytes &output, std::int64_t value) {
  const auto raw = static_cast<std::uint64_t>(value);
  for (int shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<std::byte>((raw >> shift) & 0xffU));
  }
}

LogicalControl lane(int player, int index) noexcept {
  return {.kind = LogicalControlKind::Lane, .player = player, .lane = index};
}

LogicalControl scratch(LogicalControlKind kind, int player) noexcept {
  return {.kind = kind, .player = player, .lane = -1};
}

struct AssignmentLayout {
  std::string symbols;
  std::vector<LogicalControl> controls;
};

std::optional<AssignmentLayout> assignmentLayout(int keyMode) {
  const auto keyLayout = replayKeyModeLayout(keyMode);
  if (!keyLayout || !keyLayout->hasDirectionalScratch ||
      keyLayout->manualAssignmentSymbols.empty()) {
    return std::nullopt;
  }
  AssignmentLayout result;
  result.symbols = keyLayout->manualAssignmentSymbols;
  const auto addKeys = [&](int player, int count) {
    for (int index = 0; index < count; ++index) {
      result.controls.push_back(lane(player, index));
    }
  };
  result.controls.push_back(scratch(LogicalControlKind::ScratchClockwise, 1));
  addKeys(1, keyLayout->laneCodeWidthPerPlayer);
  if (keyLayout->players == 2) {
    addKeys(2, keyLayout->laneCodeWidthPerPlayer);
    result.controls.push_back(scratch(LogicalControlKind::ScratchClockwise, 2));
  }
  return result;
}

struct ManualAssignment {
  std::vector<LogicalControl> destinations;
  std::vector<LogicalControl> sources;
};

std::optional<ManualAssignment> parseManualAssignment(std::string_view option,
                                                      int keyMode,
                                                      std::string &diagnostic) {
  constexpr std::string_view prefix = "ASSIGN:";
  if (!option.starts_with(prefix)) {
    return std::nullopt;
  }
  const auto layout = assignmentLayout(keyMode);
  const std::string_view notation = option.substr(prefix.size());
  if (!layout || notation.size() != layout->symbols.size()) {
    fail(diagnostic, "Replay manual assignment does not match its key mode");
    return std::nullopt;
  }
  ManualAssignment assignment{.destinations = layout->controls};
  assignment.sources.reserve(notation.size());
  std::vector<char> seen;
  for (const char symbol : notation) {
    const auto found = layout->symbols.find(symbol);
    if (found == std::string::npos ||
        std::ranges::find(seen, symbol) != seen.end()) {
      fail(diagnostic, "Replay manual assignment is not a lane bijection");
      return std::nullopt;
    }
    seen.push_back(symbol);
    assignment.sources.push_back(layout->controls[found]);
  }
  return assignment;
}

bool isScratch(const LogicalControl &control) noexcept {
  return control.kind == LogicalControlKind::ScratchClockwise ||
         control.kind == LogicalControlKind::ScratchCounterClockwise;
}

std::optional<LogicalControl>
projectManualControl(const LogicalControl &control,
                     const ManualAssignment &assignment) {
  if (control.kind == LogicalControlKind::Start ||
      control.kind == LogicalControlKind::Select) {
    return control;
  }
  for (std::size_t index = 0; index < assignment.destinations.size(); ++index) {
    const auto &destination = assignment.destinations[index];
    const bool matches = destination.player == control.player &&
                         ((destination.kind == LogicalControlKind::Lane &&
                           control.kind == LogicalControlKind::Lane &&
                           destination.lane == control.lane) ||
                          (isScratch(destination) && isScratch(control)));
    if (!matches) {
      continue;
    }
    auto source = assignment.sources[index];
    if (isScratch(source) && isScratch(control)) {
      source.kind = control.kind;
    }
    return source;
  }
  return std::nullopt;
}

std::optional<ManualAssignment> setupManualAssignment(const ReplaySetup &setup,
                                                      std::string &diagnostic) {
  std::optional<ManualAssignment> first;
  std::optional<ManualAssignment> second;
  if (setup.player1.option.starts_with("ASSIGN:")) {
    first = parseManualAssignment(setup.player1.option, setup.chart.keyMode,
                                  diagnostic);
    if (!first) {
      return std::nullopt;
    }
  }
  if (setup.player2.option.starts_with("ASSIGN:")) {
    second = parseManualAssignment(setup.player2.option, setup.chart.keyMode,
                                   diagnostic);
    if (!second) {
      return std::nullopt;
    }
  }
  if (first && second &&
      (first->destinations != second->destinations ||
       first->sources != second->sources)) {
    fail(diagnostic, "Replay contains conflicting manual assignments");
    return std::nullopt;
  }
  return first ? first : second;
}

std::optional<std::vector<InputTransition>>
projectStockInput(const ReplayPlaybackData &replay, const ReplayLimits &limits,
                  std::string &diagnostic) {
  const auto manual = setupManualAssignment(replay.setup, diagnostic);
  if ((replay.setup.player1.option.starts_with("ASSIGN:") ||
       replay.setup.player2.option.starts_with("ASSIGN:")) &&
      !manual) {
    return std::nullopt;
  }

  std::unordered_map<int, bool> states;
  std::vector<InputTransition> output;
  output.reserve(replay.input.size());
  bool emittedInitialState = false;
  const auto append = [&](int code, std::int64_t time, bool pressed) {
    const auto control =
        BeatorajaReplayCodec::logicalControl(code, replay.setup.chart.keyMode);
    if (!control) {
      return false;
    }
    output.push_back(
        {.songTimeMicros = time, .control = *control, .pressed = pressed});
    return true;
  };
  const auto emitInitial = [&]() {
    if (emittedInitialState) {
      return true;
    }
    emittedInitialState = true;
    std::vector<int> held;
    for (const auto &[code, pressed] : states) {
      if (pressed) {
        held.push_back(code);
      }
    }
    std::ranges::sort(held);
    for (const int code : held) {
      if (!append(code, 0, true)) {
        return false;
      }
    }
    return true;
  };

  for (const auto &transition : replay.input) {
    std::optional<LogicalControl> projected = transition.control;
    if (manual) {
      projected = projectManualControl(transition.control, *manual);
      if (!projected) {
        fail(diagnostic, "Replay manual input cannot be projected to stock");
        return std::nullopt;
      }
    }
    const auto code = BeatorajaReplayCodec::beatorajaKeyCode(
        *projected, replay.setup.chart.keyMode);
    if (!code) {
      continue;
    }
    const bool current = states.contains(*code) && states[*code];
    if (current == transition.pressed) {
      continue;
    }
    if (transition.songTimeMicros >= 0 && !emitInitial()) {
      fail(diagnostic, "Replay initial stock input cannot be projected");
      return std::nullopt;
    }
    states[*code] = transition.pressed;
    if (transition.songTimeMicros >= 0 &&
        !append(*code, transition.songTimeMicros, transition.pressed)) {
      fail(diagnostic, "Replay stock input cannot be projected");
      return std::nullopt;
    }
  }
  if (!emitInitial()) {
    fail(diagnostic, "Replay initial stock input cannot be projected");
    return std::nullopt;
  }
  if (!withinReplayCountLimit(output.size(), limits.maxInputTransitions)) {
    fail(diagnostic, "Replay stock input exceeds the transition limit");
    return std::nullopt;
  }
  return output;
}

std::optional<Bytes> encodeStockKeyRecords(const ReplayPlaybackData &replay,
                                           const ReplayLimits &limits,
                                           std::string &diagnostic) {
  const auto projected = projectStockInput(replay, limits, diagnostic);
  if (!projected) {
    return std::nullopt;
  }
  const std::size_t records = std::max<std::size_t>(projected->size(), 1);
  if (records > limits.maxInputTransitions ||
      records > limits.maxKeyInputBytes / kKeyRecordSize) {
    fail(diagnostic, "Replay keyinput exceeds the configured limit");
    return std::nullopt;
  }
  Bytes output;
  output.reserve(records * kKeyRecordSize);
  for (const auto &transition : *projected) {
    const auto code = BeatorajaReplayCodec::beatorajaKeyCode(
        transition.control, replay.setup.chart.keyMode);
    if (!code || *code >= std::numeric_limits<std::int8_t>::max()) {
      fail(diagnostic, "Replay stock key code cannot be encoded");
      return std::nullopt;
    }
    const int signedCode = (*code + 1) * (transition.pressed ? 1 : -1);
    output.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(static_cast<std::int8_t>(signedCode))));
    appendLittleEndianInt64(output, transition.songTimeMicros);
  }
  if (output.empty()) {
    // Pinned Beatoraja requires one key record. An unmatched release is
    // playback-neutral and this decoder collapses it back to empty input.
    output.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(-1)));
    appendLittleEndianInt64(output, 0);
  }
  return output;
}

std::optional<std::vector<InputTransition>>
decodeStockKeyRecords(const Json &stage, int keyMode,
                      const ReplayLimits &limits, std::string &diagnostic) {
  const auto found = stage.find("keyinput");
  if (found == stage.end() || !found->is_string()) {
    fail(diagnostic, "Replay keyinput is missing or has the wrong type");
    return std::nullopt;
  }
  const auto compressed =
      base64UrlDecodeBounded(found->get_ref<const std::string &>(),
                             limits.maxCompressedBytes, diagnostic);
  if (!compressed) {
    diagnostic = "Replay keyinput Base64URL is invalid: " + diagnostic;
    return std::nullopt;
  }
  const auto records =
      gzipDecompressBounded(*compressed, limits.maxKeyInputBytes, diagnostic);
  if (!records) {
    diagnostic = "Replay keyinput gzip is invalid: " + diagnostic;
    return std::nullopt;
  }
  if (records->size() % kKeyRecordSize != 0) {
    fail(diagnostic, "Replay keyinput contains a partial record");
    return std::nullopt;
  }
  const std::size_t count = records->size() / kKeyRecordSize;
  if (!withinReplayCountLimit(count, limits.maxInputTransitions)) {
    fail(diagnostic, "Replay keyinput record count exceeds the limit");
    return std::nullopt;
  }

  std::unordered_map<int, bool> states;
  std::vector<InputTransition> output;
  output.reserve(count);
  std::int64_t previous = 0;
  bool hasPrevious = false;
  for (std::size_t offset = 0; offset < records->size();
       offset += kKeyRecordSize) {
    const auto raw = std::to_integer<std::uint8_t>((*records)[offset]);
    if (raw == 0 || raw == 0x80U) {
      fail(diagnostic, "Replay keyinput has an invalid signed key byte");
      return std::nullopt;
    }
    const auto signedCode = static_cast<std::int8_t>(raw);
    const bool pressed = signedCode > 0;
    const int code = std::abs(static_cast<int>(signedCode)) - 1;
    const auto control = BeatorajaReplayCodec::logicalControl(code, keyMode);
    const std::int64_t time = littleEndianInt64(*records, offset + 1);
    if (!control || time < limits.minimumSongTimeMicros ||
        (hasPrevious && time < previous)) {
      fail(diagnostic, "Replay keyinput control or timestamp is invalid");
      return std::nullopt;
    }
    previous = time;
    hasPrevious = true;
    const bool current = states.contains(code) && states[code];
    if (current == pressed) {
      continue;
    }
    states[code] = pressed;
    output.push_back(
        {.songTimeMicros = time, .control = *control, .pressed = pressed});
  }
  return output;
}

template <typename T>
bool readRequired(const Json &object, std::string_view name, T &output,
                  std::string &diagnostic) {
  const auto found = object.find(std::string(name));
  if (found == object.end()) {
    return fail(diagnostic, "Replay field is missing: " + std::string(name));
  }
  try {
    output = found->template get<T>();
    return true;
  } catch (const Json::exception &) {
    return fail(diagnostic,
                "Replay field has the wrong type: " + std::string(name));
  }
}

template <typename T> Json optionalJson(const std::optional<T> &value) {
  return value ? Json(*value) : Json(nullptr);
}

template <typename T>
bool readOptional(const Json &object, std::string_view name,
                  std::optional<T> &output, std::string &diagnostic) {
  const auto found = object.find(std::string(name));
  if (found == object.end()) {
    return fail(diagnostic, "Replay field is missing: " + std::string(name));
  }
  if (found->is_null()) {
    output.reset();
    return true;
  }
  try {
    output = found->template get<T>();
    return true;
  } catch (const Json::exception &) {
    return fail(diagnostic,
                "Replay field has the wrong type: " + std::string(name));
  }
}

Json encodePlayerOption(const ReplayPlayerOption &option) {
  return {{"option", option.option},
          {"seed", optionalJson(option.seed)},
          {"laneShufflePattern", optionalJson(option.laneShufflePattern)}};
}

Json encodeSetup(const ReplaySetup &setup) {
  return {
      {"chartMd5", setup.chart.md5},
      {"chartSha256", setup.chart.sha256},
      {"keyMode", setup.chart.keyMode},
      {"longNoteMode", setup.longNoteMode},
      {"hasUndefinedLongNotes", setup.hasUndefinedLongNotes},
      {"chartRandomSeed", optionalJson(setup.chartRandomSeed)},
      {"chartRandomPrng", optionalJson(setup.chartRandomPrng)},
      {"chartRandomValues", setup.chartRandomValues},
      {"player1", encodePlayerOption(setup.player1)},
      {"player2", encodePlayerOption(setup.player2)},
      {"doublePlayOption", static_cast<int>(setup.doublePlayOption)},
      {"assistOption", setup.assistOption},
      {"initialGaugeType", gaugeTypeIndex(setup.initialGaugeType)},
      {"gaugeProfile", static_cast<int>(setup.gaugeProfile)},
      {"gaugeAutoShift", static_cast<int>(setup.gaugeAutoShift)},
      {"gaugeAutoShiftLowerBound",
       gaugeTypeIndex(setup.gaugeAutoShiftLowerBound)},
      {"ruleset",
       {{"id", setup.ruleset.id},
        {"version", setup.ruleset.version},
        {"scoringModel", setup.ruleset.scoringModel},
        {"judgementModel", setup.ruleset.judgementModel},
        {"gaugeModel", setup.ruleset.gaugeModel}}},
      {"playbackPercent", setup.playback.percent},
      {"playbackMode", static_cast<int>(setup.playback.mode)},
      {"candidateSelection", static_cast<int>(setup.candidateSelection)},
      {"judgeWindowScalePercent", setup.judgeWindowScalePercent},
      {"startingGaugePercent", setup.startingGaugePercent},
      {"initialLaneCoverPercent", setup.initialLaneCoverPercent},
      {"laneCoverEnabled", setup.laneCoverEnabled},
      {"clubMode", setup.clubMode},
  };
}

bool decodePlayerOption(const Json &source, ReplayPlayerOption &output,
                        std::string &diagnostic) {
  return source.is_object() &&
         readRequired(source, "option", output.option, diagnostic) &&
         readOptional(source, "seed", output.seed, diagnostic) &&
         readOptional(source, "laneShufflePattern", output.laneShufflePattern,
                      diagnostic);
}

bool decodeSetup(const Json &source, ReplaySetup &output,
                 std::string &diagnostic) {
  if (!source.is_object()) {
    return fail(diagnostic, "Replay extension setup is not an object");
  }
  int doubleOption = 0;
  int gauge = 0;
  int profile = 0;
  int autoShift = 0;
  int lowerBound = 0;
  int playbackMode = 0;
  int candidate = 0;
  const auto player1 = source.find("player1");
  const auto player2 = source.find("player2");
  const auto ruleset = source.find("ruleset");
  if (!readRequired(source, "chartMd5", output.chart.md5, diagnostic) ||
      !readRequired(source, "chartSha256", output.chart.sha256, diagnostic) ||
      !readRequired(source, "keyMode", output.chart.keyMode, diagnostic) ||
      !readRequired(source, "longNoteMode", output.longNoteMode, diagnostic) ||
      !readRequired(source, "hasUndefinedLongNotes",
                    output.hasUndefinedLongNotes, diagnostic) ||
      !readOptional(source, "chartRandomSeed", output.chartRandomSeed,
                    diagnostic) ||
      !readOptional(source, "chartRandomPrng", output.chartRandomPrng,
                    diagnostic) ||
      !readRequired(source, "chartRandomValues", output.chartRandomValues,
                    diagnostic) ||
      player1 == source.end() ||
      !decodePlayerOption(*player1, output.player1, diagnostic) ||
      player2 == source.end() ||
      !decodePlayerOption(*player2, output.player2, diagnostic) ||
      !readRequired(source, "doublePlayOption", doubleOption, diagnostic) ||
      !readRequired(source, "assistOption", output.assistOption, diagnostic) ||
      !readRequired(source, "initialGaugeType", gauge, diagnostic) ||
      !readRequired(source, "gaugeProfile", profile, diagnostic) ||
      !readRequired(source, "gaugeAutoShift", autoShift, diagnostic) ||
      !readRequired(source, "gaugeAutoShiftLowerBound", lowerBound,
                    diagnostic) ||
      ruleset == source.end() || !ruleset->is_object() ||
      !readRequired(*ruleset, "id", output.ruleset.id, diagnostic) ||
      !readRequired(*ruleset, "version", output.ruleset.version, diagnostic) ||
      !readRequired(*ruleset, "scoringModel", output.ruleset.scoringModel,
                    diagnostic) ||
      !readRequired(*ruleset, "judgementModel", output.ruleset.judgementModel,
                    diagnostic) ||
      !readRequired(*ruleset, "gaugeModel", output.ruleset.gaugeModel,
                    diagnostic) ||
      !readRequired(source, "playbackPercent", output.playback.percent,
                    diagnostic) ||
      !readRequired(source, "playbackMode", playbackMode, diagnostic) ||
      !readRequired(source, "candidateSelection", candidate, diagnostic) ||
      !readRequired(source, "judgeWindowScalePercent",
                    output.judgeWindowScalePercent, diagnostic) ||
      !readRequired(source, "startingGaugePercent", output.startingGaugePercent,
                    diagnostic) ||
      !readRequired(source, "initialLaneCoverPercent",
                    output.initialLaneCoverPercent, diagnostic) ||
      !readRequired(source, "laneCoverEnabled", output.laneCoverEnabled,
                    diagnostic) ||
      !readRequired(source, "clubMode", output.clubMode, diagnostic)) {
    return false;
  }
  output.doublePlayOption = static_cast<DoublePlayOption>(doubleOption);
  output.initialGaugeType = gaugeTypeAtIndex(gauge);
  output.gaugeProfile = static_cast<GaugeProfile>(profile);
  output.gaugeAutoShift = static_cast<GaugeAutoShiftMode>(autoShift);
  output.gaugeAutoShiftLowerBound = gaugeTypeAtIndex(lowerBound);
  output.playback.mode = static_cast<audio::PlaybackMode>(playbackMode);
  output.candidateSelection =
      static_cast<gameplay::CandidateSelectionMode>(candidate);
  if (gauge < 0 || gauge >= static_cast<int>(kGaugeTypeCount) ||
      lowerBound < 0 || lowerBound >= static_cast<int>(kGaugeTypeCount)) {
    return fail(diagnostic, "Replay extension gauge enum is invalid");
  }
  return true;
}

Json encodeInput(std::span<const InputTransition> input) {
  Json output = Json::array();
  for (const auto &transition : input) {
    output.push_back({
        {"songTimeMicros", transition.songTimeMicros},
        {"kind", static_cast<int>(transition.control.kind)},
        {"player", transition.control.player},
        {"lane", transition.control.lane},
        {"pressed", transition.pressed},
        {"replayOnly", transition.replayOnly},
    });
  }
  return output;
}

Json encodeTouch(std::span<const ReplayTouchSample> samples) {
  Json output = Json::array();
  for (const auto &sample : samples) {
    output.push_back({
        {"action", static_cast<int>(sample.action)},
        {"fingerId", sample.fingerId},
        {"songTimeMicros", sample.songTimeMicros},
        {"x", sample.x},
        {"y", sample.y},
    });
  }
  return output;
}

Json encodeLaneCover(std::span<const ReplayLaneCoverEvent> events) {
  Json output = Json::array();
  for (const auto &event : events) {
    output.push_back({
        {"songTimeMicros", event.songTimeMicros},
        {"noteStartPositionPercent", event.noteStartPositionPercent},
        {"resetVisibleTimeReference", event.resetVisibleTimeReference},
    });
  }
  return output;
}

bool decodeInput(const Json &source, std::vector<InputTransition> &output,
                 const ReplayLimits &limits, std::string &diagnostic) {
  if (!source.is_array() ||
      !withinReplayCountLimit(source.size(), limits.maxInputTransitions)) {
    return fail(diagnostic, "Replay extension input array is invalid");
  }
  output.clear();
  output.reserve(source.size());
  for (const auto &item : source) {
    InputTransition transition;
    int kind = 0;
    if (!item.is_object() ||
        !readRequired(item, "songTimeMicros", transition.songTimeMicros,
                      diagnostic) ||
        !readRequired(item, "kind", kind, diagnostic) ||
        !readRequired(item, "player", transition.control.player, diagnostic) ||
        !readRequired(item, "lane", transition.control.lane, diagnostic) ||
        !readRequired(item, "pressed", transition.pressed, diagnostic) ||
        !readRequired(item, "replayOnly", transition.replayOnly, diagnostic)) {
      return false;
    }
    if (kind < static_cast<int>(LogicalControlKind::Lane) ||
        kind > static_cast<int>(LogicalControlKind::Select)) {
      return fail(diagnostic, "Replay extension input kind is invalid");
    }
    transition.control.kind = static_cast<LogicalControlKind>(kind);
    output.push_back(transition);
  }
  return true;
}

bool decodeTouch(const Json &source, std::vector<ReplayTouchSample> &output,
                 const ReplayLimits &limits, std::string &diagnostic) {
  if (!source.is_array() ||
      !withinReplayCountLimit(source.size(), limits.maxTouchSamples)) {
    return fail(diagnostic, "Replay extension touch array is invalid");
  }
  output.clear();
  output.reserve(source.size());
  for (const auto &item : source) {
    ReplayTouchSample sample;
    int action = 0;
    if (!item.is_object() ||
        !readRequired(item, "action", action, diagnostic) ||
        !readRequired(item, "fingerId", sample.fingerId, diagnostic) ||
        !readRequired(item, "songTimeMicros", sample.songTimeMicros,
                      diagnostic) ||
        !readRequired(item, "x", sample.x, diagnostic) ||
        !readRequired(item, "y", sample.y, diagnostic)) {
      return false;
    }
    if (action < static_cast<int>(ReplayTouchAction::Down) ||
        action > static_cast<int>(ReplayTouchAction::Cancel)) {
      return fail(diagnostic, "Replay extension touch action is invalid");
    }
    sample.action = static_cast<ReplayTouchAction>(action);
    output.push_back(sample);
  }
  return true;
}

bool decodeLaneCover(const Json &source,
                     std::vector<ReplayLaneCoverEvent> &output,
                     const ReplayLimits &limits, std::string &diagnostic) {
  if (!source.is_array() ||
      !withinReplayCountLimit(source.size(), limits.maxLaneCoverEvents)) {
    return fail(diagnostic, "Replay extension lane-cover array is invalid");
  }
  output.clear();
  output.reserve(source.size());
  for (const auto &item : source) {
    ReplayLaneCoverEvent event;
    if (!item.is_object() ||
        !readRequired(item, "songTimeMicros", event.songTimeMicros,
                      diagnostic) ||
        !readRequired(item, "noteStartPositionPercent",
                      event.noteStartPositionPercent, diagnostic) ||
        !readRequired(item, "resetVisibleTimeReference",
                      event.resetVisibleTimeReference, diagnostic)) {
      return false;
    }
    output.push_back(event);
  }
  return true;
}

template <typename T>
bool readStock(const Json &stage, std::string_view name, T &output, T fallback,
               std::string &diagnostic) {
  const auto found = stage.find(std::string(name));
  if (found == stage.end()) {
    output = std::move(fallback);
    return true;
  }
  try {
    output = found->template get<T>();
    return true;
  } catch (const Json::exception &) {
    return fail(diagnostic,
                "Replay stock field has the wrong type: " + std::string(name));
  }
}

bool decodeLaneShufflePatterns(const Json &stage, int keyMode,
                               ReplaySetup &setup, std::string &diagnostic) {
  const auto found = stage.find("laneShufflePattern");
  if (found == stage.end() || found->is_null()) {
    setup.player1.laneShufflePattern.reset();
    setup.player2.laneShufflePattern.reset();
    return true;
  }
  const auto layout = replayKeyModeLayout(keyMode);
  if (!layout) {
    return fail(diagnostic, "Replay stock key mode is unsupported");
  }
  const std::size_t expectedRows = static_cast<std::size_t>(layout->players);
  if (!found->is_array() || found->size() != expectedRows) {
    return fail(diagnostic, "Replay stock lane-shuffle row count is invalid");
  }
  const auto decodeRow = [&](const Json &row,
                             std::optional<std::vector<int>> &output) {
    if (row.is_null()) {
      output.reset();
      return true;
    }
    if (!row.is_array()) {
      return false;
    }
    try {
      output = row.get<std::vector<int>>();
      return true;
    } catch (const Json::exception &) {
      return false;
    }
  };
  if (!decodeRow((*found)[0], setup.player1.laneShufflePattern) ||
      (expectedRows == 2 &&
       !decodeRow((*found)[1], setup.player2.laneShufflePattern))) {
    return fail(diagnostic,
                "Replay stock lane-shuffle pattern has the wrong type");
  }
  return true;
}

bool decodeStockSetup(const Json &stage, int keyMode, bool course,
                      const ReplayLimits &limits, ReplaySetup &setup,
                      std::string &diagnostic) {
  if (!stage.is_object()) {
    return fail(diagnostic, "Replay stage is not a JSON object");
  }
  int stockMode = 0;
  int gauge = 0;
  int option1 = 0;
  int option2 = 0;
  int doubleOption = 0;
  std::int64_t seed1 = -1;
  std::int64_t seed2 = -1;
  if (!readRequired(stage, "sha256", setup.chart.sha256, diagnostic) ||
      !readRequired(stage, "mode", stockMode, diagnostic) ||
      !readRequired(stage, "gauge", gauge, diagnostic) ||
      !readStock(stage, "randomoption", option1, 0, diagnostic) ||
      !readStock(stage, "randomoption2", option2, 0, diagnostic) ||
      !readStock(stage, "doubleoption", doubleOption, 0, diagnostic) ||
      !readStock(stage, "randomoptionseed", seed1, std::int64_t{-1},
                 diagnostic) ||
      !readStock(stage, "randomoption2seed", seed2, std::int64_t{-1},
                 diagnostic)) {
    return false;
  }
  const auto applicationMode = applicationLongNoteMode(stockMode);
  const auto name1 = beatorajaReplayOptionName(option1);
  const auto name2 = beatorajaReplayOptionName(option2);
  if (!applicationMode || !name1 || !name2 || gauge < 0 ||
      gauge >= static_cast<int>(kGaugeTypeCount) || doubleOption < 0 ||
      doubleOption > static_cast<int>(DoublePlayOption::Flip)) {
    return fail(diagnostic, "Replay stock setup contains an invalid enum");
  }
  setup.chart.keyMode = keyMode;
  setup.chart.md5.clear();
  setup.longNoteMode = *applicationMode;
  setup.hasUndefinedLongNotes = false;
  setup.player1.option = *name1;
  setup.player2.option = *name2;
  setup.player1.seed =
      seed1 >= 0 ? std::optional<std::int64_t>(seed1) : std::nullopt;
  setup.player2.seed =
      seed2 >= 0 ? std::optional<std::int64_t>(seed2) : std::nullopt;
  setup.doublePlayOption = static_cast<DoublePlayOption>(doubleOption);
  setup.initialGaugeType = gaugeTypeAtIndex(gauge);
  setup.gaugeProfile = gaugeProfileForKeyMode(keyMode, course);
  setup.startingGaugePercent =
      gaugeInitialValue(setup.initialGaugeType, setup.gaugeProfile);
  setup.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  setup.playback = {.percent = 100, .mode = audio::PlaybackMode::PitchShift};
  setup.candidateSelection = gameplay::CandidateSelectionMode::Lowest;
  setup.initialLaneCoverPercent = 0;
  setup.laneCoverEnabled = false;

  const auto random = stage.find("rand");
  if (random != stage.end()) {
    if (!random->is_array() ||
        !withinReplayCountLimit(random->size(), limits.maxRandomValues)) {
      return fail(diagnostic, "Replay stock RANDOM values are invalid");
    }
    try {
      setup.chartRandomValues = random->get<std::vector<int>>();
    } catch (const Json::exception &) {
      return fail(diagnostic,
                  "Replay stock RANDOM values contain a non-integer");
    }
  }
  if (!decodeLaneShufflePatterns(stage, keyMode, setup, diagnostic)) {
    return false;
  }

  const auto config = stage.find("config");
  if (config != stage.end() && !config->is_null()) {
    if (!config->is_object()) {
      return fail(diagnostic, "Replay stock config is not an object");
    }
    float cover = 0.2F;
    bool enabled = true;
    if (!readStock(*config, "lanecover", cover, 0.2F, diagnostic) ||
        !readStock(*config, "enablelanecover", enabled, true, diagnostic) ||
        !std::isfinite(cover) || cover < 0.0F || cover > 1.0F) {
      if (diagnostic.empty()) {
        diagnostic = "Replay stock lane cover is invalid";
      }
      return false;
    }
    setup.initialLaneCoverPercent =
        static_cast<int>(std::lround(cover * 100.0F));
    setup.laneCoverEnabled = enabled;
  }
  const auto layout = replayKeyModeLayout(keyMode);
  if (setup.doublePlayOption == DoublePlayOption::Flip &&
      (!layout || !layout->supportsDoublePlayFlip)) {
    return fail(diagnostic,
                "Replay double-play option is invalid for its key mode");
  }
  return true;
}

Json encodeStockLanePatterns(const ReplaySetup &setup) {
  Json output = Json::array();
  output.push_back(optionalJson(setup.player1.laneShufflePattern));
  const auto layout = replayKeyModeLayout(setup.chart.keyMode);
  if (layout && layout->players == 2) {
    output.push_back(optionalJson(setup.player2.laneShufflePattern));
  }
  return output;
}

std::optional<Json>
encodeStage(const ReplayPlaybackData &playback, ReplayTimeBounds timeBounds,
            std::int64_t playedAtUnixMillis, std::string_view envelope,
            std::size_t stageIndex, std::size_t stageCount,
            std::int64_t restMicrosAfterStage, const ReplayLimits &limits,
            std::string &diagnostic) {
  if (playedAtUnixMillis < 0 || stageCount == 0 || stageIndex >= stageCount ||
      !validCourseRestMicros(restMicrosAfterStage, limits)) {
    fail(diagnostic, "Replay stage envelope is invalid");
    return std::nullopt;
  }
  const auto validation = validateReplayPlayback(
      playback, ReplaySetupSource::LocalCapture, timeBounds, limits);
  if (!validation.valid()) {
    fail(diagnostic, "Replay stage fails canonical playback validation");
    return std::nullopt;
  }
  const auto option1 =
      projectedBeatorajaReplayOptionIndex(playback.setup.player1.option);
  const auto option2 =
      projectedBeatorajaReplayOptionIndex(playback.setup.player2.option);
  const auto stockMode = stockLongNoteMode(playback.setup.longNoteMode);
  if (!option1 || !option2 || !stockMode) {
    fail(diagnostic, "Replay setup cannot be represented by stock Beatoraja");
    return std::nullopt;
  }
  const auto records = encodeStockKeyRecords(playback, limits, diagnostic);
  if (!records) {
    return std::nullopt;
  }
  const auto compressedKeys = gzipCompress(*records, diagnostic);
  if (!compressedKeys || compressedKeys->size() > limits.maxCompressedBytes) {
    if (diagnostic.empty()) {
      diagnostic = "Compressed replay keyinput exceeds the limit";
    }
    return std::nullopt;
  }

  Json extension{
      {"schemaVersion", kAsoSchemaVersion},
      {"envelope", envelope},
      {"stageIndex", stageIndex},
      {"stageCount", stageCount},
      {"completionSongTimeMicros", timeBounds.completionSongTimeMicros},
      {"restMicrosAfterStage", restMicrosAfterStage},
      {"setup", encodeSetup(playback.setup)},
      {"input", encodeInput(playback.input)},
      {"touchSamples", encodeTouch(playback.touchSamples)},
      {"laneCoverEvents", encodeLaneCover(playback.laneCoverEvents)},
  };
  return Json{
      {"player", "AsoBMaShow"},
      {"sha256", playback.setup.chart.sha256},
      {"mode", *stockMode},
      {"keyinput", base64UrlEncode(*compressedKeys)},
      {"gauge", gaugeTypeIndex(playback.setup.initialGaugeType)},
      {"laneShufflePattern", encodeStockLanePatterns(playback.setup)},
      {"rand", playback.setup.chartRandomValues},
      {"date", playedAtUnixMillis / 1000},
      {"sevenToNinePattern", 0},
      {"randomoption", *option1},
      {"randomoptionseed", playback.setup.player1.seed.value_or(-1)},
      {"randomoption2", *option2},
      {"randomoption2seed", playback.setup.player2.seed.value_or(-1)},
      {"doubleoption", static_cast<int>(playback.setup.doublePlayOption)},
      {"config",
       {{"lanecover", playback.setup.initialLaneCoverPercent / 100.0F},
        {"enablelanecover", playback.setup.laneCoverEnabled}}},
      {"asobmashow", std::move(extension)},
  };
}

struct StageDecode {
  ReplayPlaybackData playback;
  ReplayTimeBounds timeBounds;
  std::int64_t restMicrosAfterStage = 0;
  std::size_t stageIndex = 0;
  std::size_t stageCount = 1;
  ReplayStageDecodeSource source = ReplayStageDecodeSource::Stock;
  bool unsupportedExtension = false;
};

bool decodeStage(const Json &stage, bool course, std::size_t expectedIndex,
                 std::size_t expectedCount, int expectedKeyMode,
                 std::optional<ReplayTimeBounds> expectedTimeBounds,
                 const ReplayLimits &limits, StageDecode &output,
                 std::string &diagnostic) {
  ReplaySetup stockSetup;
  if (!decodeStockSetup(stage, expectedKeyMode, course, limits, stockSetup,
                        diagnostic)) {
    return false;
  }
  const auto stockInput =
      decodeStockKeyRecords(stage, expectedKeyMode, limits, diagnostic);
  if (!stockInput) {
    return false;
  }

  const auto extension = stage.find("asobmashow");
  if (extension == stage.end()) {
    if (!expectedTimeBounds.has_value()) {
      return fail(diagnostic,
                  "Stock replay requires an authoritative time bound");
    }
    output.playback.setup = std::move(stockSetup);
    output.playback.input = *stockInput;
    output.timeBounds = *expectedTimeBounds;
    const auto validation = validateReplayPlayback(
        output.playback, ReplaySetupSource::StockBeatoraja,
        *expectedTimeBounds,
        limits);
    if (!validation.valid()) {
      return fail(diagnostic,
                  "Replay stock playback fails canonical validation");
    }
    return true;
  }
  if (!extension->is_object()) {
    return fail(diagnostic, "Aso replay extension is not an object");
  }
  int schemaVersion = 0;
  if (!readRequired(*extension, "schemaVersion", schemaVersion, diagnostic)) {
    return false;
  }
  if (schemaVersion != kAsoSchemaVersion) {
    output.unsupportedExtension = true;
    if (!expectedTimeBounds.has_value()) {
      return fail(diagnostic,
                  "Unsupported Aso extension requires a stock time bound");
    }
    output.playback.setup = std::move(stockSetup);
    output.playback.input = *stockInput;
    output.timeBounds = *expectedTimeBounds;
    const auto validation = validateReplayPlayback(
        output.playback, ReplaySetupSource::StockBeatoraja,
        *expectedTimeBounds,
        limits);
    if (!validation.valid()) {
      return fail(diagnostic,
                  "Replay stock fallback fails canonical validation");
    }
    return true;
  }

  std::string envelope;
  std::int64_t completion = -1;
  const auto setup = extension->find("setup");
  const auto input = extension->find("input");
  const auto touch = extension->find("touchSamples");
  const auto cover = extension->find("laneCoverEvents");
  if (!readRequired(*extension, "envelope", envelope, diagnostic) ||
      !readRequired(*extension, "stageIndex", output.stageIndex, diagnostic) ||
      !readRequired(*extension, "stageCount", output.stageCount, diagnostic) ||
      !readRequired(*extension, "completionSongTimeMicros", completion,
                    diagnostic) ||
      !readRequired(*extension, "restMicrosAfterStage",
                    output.restMicrosAfterStage, diagnostic) ||
      setup == extension->end() ||
      !decodeSetup(*setup, output.playback.setup, diagnostic) ||
      input == extension->end() ||
      !decodeInput(*input, output.playback.input, limits, diagnostic) ||
      touch == extension->end() ||
      !decodeTouch(*touch, output.playback.touchSamples, limits, diagnostic) ||
      cover == extension->end() ||
      !decodeLaneCover(*cover, output.playback.laneCoverEvents, limits,
                       diagnostic)) {
    return false;
  }
  output.timeBounds = {.completionSongTimeMicros = completion};
  if (envelope != (course ? "course-stage" : "chart") ||
      output.stageIndex != expectedIndex ||
      output.stageCount != expectedCount ||
      (expectedTimeBounds.has_value() &&
       output.timeBounds != *expectedTimeBounds) ||
      !validCourseRestMicros(output.restMicrosAfterStage, limits) ||
      (!course && output.restMicrosAfterStage != 0) ||
      output.playback.setup.chart.keyMode != expectedKeyMode) {
    return fail(diagnostic, "Replay extension context is inconsistent");
  }
  const auto validation =
      validateReplayPlayback(output.playback, ReplaySetupSource::AsoExtension,
                             output.timeBounds, limits);
  if (!validation.valid()) {
    return fail(diagnostic,
                "Replay extension playback fails canonical validation");
  }
  output.source = ReplayStageDecodeSource::AsoExtension;
  return true;
}

bool addWithin(std::size_t &total, std::size_t count,
               std::size_t maximum) noexcept {
  if (count > maximum || total > maximum - count) {
    return false;
  }
  total += count;
  return true;
}

struct AggregateCounts {
  std::size_t input = 0;
  std::size_t touch = 0;
  std::size_t cover = 0;

  bool include(const ReplayPlaybackData &stage, const ReplayLimits &limits,
               std::string &diagnostic) {
    if (!addWithin(input, stage.input.size(), limits.maxInputTransitions) ||
        !addWithin(touch, stage.touchSamples.size(), limits.maxTouchSamples) ||
        !addWithin(cover, stage.laneCoverEvents.size(),
                   limits.maxLaneCoverEvents)) {
      return fail(diagnostic,
                  "Replay course arrays exceed aggregate shared limits");
    }
    return true;
  }
};

std::optional<Bytes> encodeDocument(const Json &document,
                                    const ReplayLimits &limits,
                                    std::string &diagnostic) {
  const std::string serialized = document.dump();
  if (serialized.size() > limits.maxJsonBytes) {
    fail(diagnostic, "Replay JSON exceeds the configured limit");
    return std::nullopt;
  }
  const auto raw =
      std::as_bytes(std::span(serialized.data(), serialized.size()));
  auto encoded = gzipCompress(raw, diagnostic);
  if (!encoded) {
    return std::nullopt;
  }
  if (encoded->size() > limits.maxCompressedBytes) {
    fail(diagnostic, "Compressed replay exceeds the configured limit");
    return std::nullopt;
  }
  return encoded;
}

} // namespace

std::optional<bool>
ReplayDecodeOutcome::replayPathHasUndefinedLongNotes() const noexcept {
  if (chart) {
    if (course || stageSources.size() != 1) {
      return std::nullopt;
    }
    return stageSources.front() == ReplayStageDecodeSource::AsoExtension
               ? std::optional<bool>(
                     chart->playback.setup.hasUndefinedLongNotes)
               : std::nullopt;
  }
  if (!course || stageSources.size() != course->playback.stages.size()) {
    return std::nullopt;
  }
  bool containsStock = false;
  for (std::size_t index = 0; index < stageSources.size(); ++index) {
    if (stageSources[index] == ReplayStageDecodeSource::Stock) {
      containsStock = true;
    } else if (course->playback.stages[index].setup.hasUndefinedLongNotes) {
      return true;
    }
  }
  return containsStock ? std::nullopt : std::optional<bool>(false);
}

BeatorajaReplayCodec::BeatorajaReplayCodec(ReplayLimits limits)
    : limits_(limits) {}

std::optional<std::vector<std::byte>>
BeatorajaReplayCodec::encodeChart(const ReplayChartDocument &replay,
                                  std::int64_t playedAtUnixMillis,
                                  std::string &diagnostic) const {
  diagnostic.clear();
  if (!limits_.valid()) {
    diagnostic = "Replay limits are invalid";
    return std::nullopt;
  }
  const auto stage =
      encodeStage(replay.playback, replay.timeBounds, playedAtUnixMillis,
                  "chart", 0, 1, 0, limits_, diagnostic);
  return stage ? encodeDocument(*stage, limits_, diagnostic) : std::nullopt;
}

std::optional<std::vector<std::byte>>
BeatorajaReplayCodec::encodeCourse(const ReplayCourseDocument &replay,
                                   std::int64_t playedAtUnixMillis,
                                   std::string &diagnostic) const {
  diagnostic.clear();
  if (!limits_.valid()) {
    diagnostic = "Replay limits are invalid";
    return std::nullopt;
  }
  if (replay.playback.stages.empty() ||
      !withinReplayCountLimit(replay.playback.stages.size(),
                              limits_.maxCourseStages) ||
      replay.timeBounds.size() != replay.playback.stages.size() ||
      replay.playback.restMicrosAfterStage.size() !=
          replay.playback.stages.size()) {
    diagnostic = "Replay course envelope is invalid";
    return std::nullopt;
  }
  AggregateCounts aggregate;
  for (const auto &stage : replay.playback.stages) {
    if (!aggregate.include(stage, limits_, diagnostic)) {
      return std::nullopt;
    }
  }
  Json document = Json::array();
  for (std::size_t index = 0; index < replay.playback.stages.size(); ++index) {
    const auto stage = encodeStage(
        replay.playback.stages[index], replay.timeBounds[index],
        playedAtUnixMillis, "course-stage", index,
        replay.playback.stages.size(),
        replay.playback.restMicrosAfterStage[index], limits_, diagnostic);
    if (!stage) {
      return std::nullopt;
    }
    document.push_back(*stage);
  }
  return encodeDocument(document, limits_, diagnostic);
}

ReplayDecodeOutcome
BeatorajaReplayCodec::decode(std::span<const std::byte> encoded,
                             const ReplayDecodeContext &context) const {
  ReplayDecodeOutcome outcome;
  if (!limits_.valid()) {
    outcome.diagnostic = "Replay limits are invalid";
    return outcome;
  }
  if (encoded.size() > limits_.maxCompressedBytes) {
    outcome.diagnostic = "Compressed replay exceeds the configured limit";
    return outcome;
  }
  if (context.stageKeyModes.empty() ||
      (!context.stageTimeBounds.empty() &&
       context.stageKeyModes.size() != context.stageTimeBounds.size()) ||
      std::ranges::any_of(context.stageTimeBounds, [](ReplayTimeBounds value) {
        return !value.valid();
      })) {
    outcome.diagnostic = "Replay decode context is incomplete";
    return outcome;
  }
  const auto jsonBytes =
      gzipDecompressBounded(encoded, limits_.maxJsonBytes, outcome.diagnostic);
  if (!jsonBytes) {
    outcome.diagnostic = "Replay gzip is invalid: " + outcome.diagnostic;
    return outcome;
  }
  const std::string source(reinterpret_cast<const char *>(jsonBytes->data()),
                           jsonBytes->size());
  if (!validJsonDepth(source, limits_.maxJsonDepth, outcome.diagnostic)) {
    return outcome;
  }

  Json document;
  try {
    document = Json::parse(source);
  } catch (const Json::exception &error) {
    outcome.diagnostic = "Replay JSON is invalid: " + std::string(error.what());
    return outcome;
  }
  const bool course = document.is_array();
  if (!course && !document.is_object()) {
    outcome.diagnostic = "Replay root is neither chart nor course";
    return outcome;
  }
  const std::size_t stageCount = course ? document.size() : 1;
  if (stageCount == 0 ||
      !withinReplayCountLimit(stageCount, limits_.maxCourseStages) ||
      context.stageKeyModes.size() != stageCount) {
    outcome.diagnostic = "Replay stage count differs from decode context";
    return outcome;
  }

  std::vector<StageDecode> stages;
  stages.reserve(stageCount);
  AggregateCounts aggregate;
  for (std::size_t index = 0; index < stageCount; ++index) {
    StageDecode stage;
    const Json &stageJson = course ? document[index] : document;
    const std::optional<ReplayTimeBounds> expectedTimeBounds =
        context.stageTimeBounds.empty()
            ? std::nullopt
            : std::optional(context.stageTimeBounds[index]);
    if (!decodeStage(stageJson, course, index, stageCount,
                     context.stageKeyModes[index],
                     expectedTimeBounds, limits_, stage,
                     outcome.diagnostic) ||
        !aggregate.include(stage.playback, limits_, outcome.diagnostic)) {
      outcome.unsupportedAsoExtension |= stage.unsupportedExtension;
      return outcome;
    }
    stages.push_back(std::move(stage));
  }

  outcome.stockOnly = true;
  outcome.stageSources.reserve(stageCount);
  for (const auto &stage : stages) {
    outcome.stageSources.push_back(stage.source);
    outcome.stockOnly &= stage.source == ReplayStageDecodeSource::Stock;
    outcome.unsupportedAsoExtension |= stage.unsupportedExtension;
  }
  if (!course) {
    outcome.chart = ReplayChartDocument{
        .playback = std::move(stages.front().playback),
        .timeBounds = stages.front().timeBounds,
    };
  } else {
    ReplayCourseDocument value;
    value.playback.stages.reserve(stageCount);
    value.playback.restMicrosAfterStage.reserve(stageCount);
    value.timeBounds.reserve(stageCount);
    for (auto &stage : stages) {
      value.playback.stages.push_back(std::move(stage.playback));
      value.playback.restMicrosAfterStage.push_back(
          stage.source == ReplayStageDecodeSource::AsoExtension
              ? stage.restMicrosAfterStage
              : 0);
      value.timeBounds.push_back(stage.timeBounds);
    }
    outcome.course = std::move(value);
  }
  if (outcome.unsupportedAsoExtension) {
    outcome.diagnostic =
        "Unsupported Aso replay extension ignored; using stock playback";
  }
  return outcome;
}

std::optional<int>
BeatorajaReplayCodec::beatorajaKeyCode(const LogicalControl &control,
                                       int keyMode) noexcept {
  const auto layout = replayKeyModeLayout(keyMode);
  if (!layout) {
    return std::nullopt;
  }
  if (control.player < 1 || control.player > layout->players) {
    return std::nullopt;
  }
  const int playerWidth =
      layout->laneCodeWidthPerPlayer +
      (layout->hasDirectionalScratch ? 2 : 0);
  const int playerOffset = (control.player - 1) * playerWidth;
  if (control.kind == LogicalControlKind::Lane) {
    return control.lane >= 0 && control.lane < layout->laneCodeWidthPerPlayer
               ? std::optional<int>(playerOffset + control.lane)
               : std::nullopt;
  }
  if (control.lane != -1 || !layout->hasDirectionalScratch) {
    return std::nullopt;
  }
  if (control.kind == LogicalControlKind::ScratchClockwise) {
    return playerOffset + layout->laneCodeWidthPerPlayer;
  }
  if (control.kind == LogicalControlKind::ScratchCounterClockwise) {
    return playerOffset + layout->laneCodeWidthPerPlayer + 1;
  }
  return std::nullopt;
}

std::optional<LogicalControl>
BeatorajaReplayCodec::logicalControl(int keyCode, int keyMode) noexcept {
  const auto layout = replayKeyModeLayout(keyMode);
  if (!layout || keyCode < 0) {
    return std::nullopt;
  }
  const int playerWidth =
      layout->laneCodeWidthPerPlayer +
      (layout->hasDirectionalScratch ? 2 : 0);
  const int player = keyCode / playerWidth + 1;
  const int offset = keyCode % playerWidth;
  if (player > layout->players) {
    return std::nullopt;
  }
  if (offset < layout->laneCodeWidthPerPlayer) {
    return lane(player, offset);
  }
  if (!layout->hasDirectionalScratch) {
    return std::nullopt;
  }
  return scratch(offset == layout->laneCodeWidthPerPlayer
                     ? LogicalControlKind::ScratchClockwise
                     : LogicalControlKind::ScratchCounterClockwise,
                 player);
}

} // namespace replay
