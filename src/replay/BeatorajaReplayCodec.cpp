#include "BeatorajaReplayCodec.h"
#include "ReplayInputValidation.h"

#include "Base64Url.h"
#include "BeatorajaLongNoteMode.h"
#include "BeatorajaReplayPath.h"
#include "GzipCodec.h"
#include "../bms_parser.hpp"
#include "../scene/play/GameplayGaugeRules.h"
#include "../scene/play/GameplayScoreState.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace replay {
namespace {

using Json = nlohmann::ordered_json;
using Bytes = std::vector<std::byte>;

constexpr int kAsoSchemaVersion = 2;
constexpr std::size_t kKeyRecordSize = 9;
constexpr std::array<std::string_view, 10> kStockOptions = {
    "NORMAL", "MIRROR",   "RANDOM",  "R-RANDOM",  "S-RANDOM",
    "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX"};

struct StageDecode {
  ReplayPlaybackData data;
  bool extensionPresent = false;
  bool supportedExtension = false;
  bool unsupportedExtension = false;
  int stageIndex = 0;
  int stageCount = 1;
  std::int64_t restMicrosAfterStage = 0;
};

struct ManualAssignment {
  std::vector<LogicalControl> destinations;
  std::vector<LogicalControl> sources;

  bool operator==(const ManualAssignment &) const = default;
};

struct OptionProjection {
  int stockIndex = 0;
  std::optional<ManualAssignment> manualAssignment;
};

struct SetupOptionProjection {
  int option1 = 0;
  int option2 = 0;
  std::optional<ManualAssignment> manualAssignment;
};

bool fail(std::string &diagnostic, std::string message) {
  diagnostic = std::move(message);
  return false;
}

bool isCanonicalHex(std::string_view value, std::size_t length) {
  return value.size() == length &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool isKeyModeSupported(int keyMode) {
  switch (keyMode) {
  case 5:
  case 7:
  case 9:
  case 10:
  case 14:
  case 24:
  case 48:
    return true;
  default:
    return false;
  }
}

bool isGaugeTypeSupported(GaugeType gaugeType) {
  const int value = static_cast<int>(gaugeType);
  return value >= static_cast<int>(GaugeType::AssistedEasy) &&
         value <= static_cast<int>(GaugeType::Hazard);
}

int playerCount(int keyMode) {
  return keyMode == 10 || keyMode == 14 || keyMode == 48 ? 2 : 1;
}

std::optional<int> optionIndex(const std::optional<std::string> &option) {
  const std::string_view value = option ? std::string_view(*option) : "NORMAL";
  for (std::size_t i = 0; i < kStockOptions.size(); ++i) {
    if (value == kStockOptions[i]) {
      return static_cast<int>(i);
    }
  }
  return std::nullopt;
}

LogicalControl assignmentLane(int player, int lane) {
  return {.kind = LogicalControlKind::Lane, .player = player, .lane = lane};
}

LogicalControl assignmentScratch(int player) {
  return {.kind = LogicalControlKind::ScratchClockwise,
          .player = player,
          .lane = -1};
}

struct AssignmentLayout {
  std::string symbols;
  std::vector<LogicalControl> controls;
};

std::optional<AssignmentLayout> assignmentLayout(int keyMode) {
  AssignmentLayout layout;
  const auto addKeys = [&](int player, int count) {
    for (int lane = 0; lane < count; ++lane) {
      layout.controls.push_back(assignmentLane(player, lane));
    }
  };
  switch (keyMode) {
  case 5:
    layout.symbols = "S12345";
    layout.controls.push_back(assignmentScratch(1));
    addKeys(1, 5);
    break;
  case 7:
    layout.symbols = "S1234567";
    layout.controls.push_back(assignmentScratch(1));
    addKeys(1, 7);
    break;
  case 10:
    layout.symbols = "L123456789AR";
    layout.controls.push_back(assignmentScratch(1));
    addKeys(1, 5);
    addKeys(2, 5);
    layout.controls.push_back(assignmentScratch(2));
    break;
  case 14:
    layout.symbols = "L123456789ABCDER";
    layout.controls.push_back(assignmentScratch(1));
    addKeys(1, 7);
    addKeys(2, 7);
    layout.controls.push_back(assignmentScratch(2));
    break;
  default:
    return std::nullopt;
  }
  return layout;
}

std::optional<ManualAssignment>
parseManualAssignment(std::string_view option, int keyMode,
                      std::string &diagnostic) {
  constexpr std::string_view prefix = "ASSIGN:";
  if (!option.starts_with(prefix)) {
    return std::nullopt;
  }
  const auto layout = assignmentLayout(keyMode);
  const std::string_view notation = option.substr(prefix.size());
  if (!layout || notation.size() != layout->symbols.size()) {
    fail(diagnostic,
         "Replay manual assignment does not match its key mode");
    return std::nullopt;
  }

  ManualAssignment assignment;
  assignment.destinations = layout->controls;
  assignment.sources.reserve(notation.size());
  std::vector<char> used;
  used.reserve(notation.size());
  for (char symbol : notation) {
    const auto found = layout->symbols.find(symbol);
    if (found == std::string::npos ||
        std::ranges::find(used, symbol) != used.end()) {
      fail(diagnostic, "Replay manual assignment is not a lane bijection");
      return std::nullopt;
    }
    used.push_back(symbol);
    assignment.sources.push_back(layout->controls[found]);
  }
  return assignment;
}

std::optional<OptionProjection>
projectOption(const std::optional<std::string> &option, int keyMode,
              std::string &diagnostic) {
  if (const auto stock = optionIndex(option)) {
    return OptionProjection{.stockIndex = *stock};
  }
  const std::string_view value = option ? std::string_view(*option) : "NORMAL";
  if (!value.starts_with("ASSIGN:")) {
    fail(diagnostic, "Replay contains an unsupported stock play option");
    return std::nullopt;
  }
  auto manual = parseManualAssignment(value, keyMode, diagnostic);
  if (!manual) {
    return std::nullopt;
  }
  return OptionProjection{.stockIndex = 0,
                          .manualAssignment = std::move(*manual)};
}

std::optional<SetupOptionProjection>
projectSetupOptions(const ChartPlaybackSetup &setup, std::string &diagnostic) {
  auto first = projectOption(setup.playOption, setup.keyMode, diagnostic);
  auto second = projectOption(setup.playOption2, setup.keyMode, diagnostic);
  if (!first || !second) {
    return std::nullopt;
  }
  if (first->manualAssignment && second->manualAssignment &&
      first->manualAssignment != second->manualAssignment) {
    fail(diagnostic, "Replay contains conflicting manual assignments");
    return std::nullopt;
  }
  if ((first->manualAssignment && second->stockIndex != 0) ||
      (second->manualAssignment && first->stockIndex != 0)) {
    fail(diagnostic,
         "Replay cannot combine a manual assignment with a stock shuffle");
    return std::nullopt;
  }
  return SetupOptionProjection{
      .option1 = first->stockIndex,
      .option2 = second->stockIndex,
      .manualAssignment = first->manualAssignment
                              ? std::move(first->manualAssignment)
                              : std::move(second->manualAssignment)};
}

bool isScratch(const LogicalControl &control) {
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
  for (std::size_t i = 0; i < assignment.destinations.size(); ++i) {
    const auto &destination = assignment.destinations[i];
    const bool matches =
        destination.player == control.player &&
        ((destination.kind == LogicalControlKind::Lane &&
          destination.lane == control.lane &&
          control.kind == LogicalControlKind::Lane) ||
         (isScratch(destination) && isScratch(control)));
    if (!matches) {
      continue;
    }
    auto source = assignment.sources[i];
    if (isScratch(source) && isScratch(control)) {
      source.kind = control.kind;
    }
    return source;
  }
  return std::nullopt;
}

std::optional<std::string> optionName(int index) {
  if (index < 0 || index >= static_cast<int>(kStockOptions.size())) {
    return std::nullopt;
  }
  return std::string(kStockOptions[static_cast<std::size_t>(index)]);
}

bool validateJsonDepth(std::string_view source, std::size_t maximumDepth,
                       std::string &diagnostic) {
  std::size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (char ch : source) {
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
      if (++depth > maximumDepth) {
        return fail(diagnostic, "Replay JSON nesting exceeds the limit");
      }
    } else if ((ch == '}' || ch == ']') && depth > 0) {
      --depth;
    }
  }
  return true;
}

std::optional<int> inferredKeyMode(const Json &stage) {
  const auto found = stage.find("laneShufflePattern");
  if (found == stage.end() || !found->is_array() || found->empty() ||
      found->size() > 2) {
    return std::nullopt;
  }
  std::optional<std::size_t> width;
  for (const Json &row : *found) {
    if (row.is_null()) {
      continue;
    }
    if (!row.is_array()) {
      return std::nullopt;
    }
    if (width && *width != row.size()) {
      return std::nullopt;
    }
    width = row.size();
  }
  if (!width) {
    return std::nullopt;
  }
  if (found->size() == 1) {
    switch (*width) {
    case 6:
      return 5;
    case 8:
      return 7;
    case 9:
      return 9;
    case 26:
      return 24;
    default:
      return std::nullopt;
    }
  }
  switch (*width) {
  case 6:
    return 10;
  case 8:
    return 14;
  case 26:
    return 48;
  default:
    return std::nullopt;
  }
}

std::int64_t littleEndianInt64(std::span<const std::byte> value,
                               std::size_t offset) {
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

bool validateLogicalControl(const LogicalControl &control, int keyMode) {
  if (control.kind == LogicalControlKind::Start ||
      control.kind == LogicalControlKind::Select) {
    return control.lane == -1 && control.player >= 1 &&
           control.player <= playerCount(keyMode);
  }
  return BeatorajaReplayCodec::beatorajaKeyCode(control, keyMode).has_value();
}

bool validateInput(const std::vector<InputTransition> &input, int keyMode,
                   std::size_t maximumCount, bool allowRedundant,
                   std::vector<InputTransition> *effective,
                   std::string &diagnostic) {
  if (input.size() > maximumCount) {
    return fail(diagnostic, "Replay input transition count exceeds the limit");
  }
  std::unordered_map<int, bool> stockStates;
  std::unordered_map<std::string, bool> extensionStates;
  std::int64_t previousTime = 0;
  bool hasPreviousTime = false;
  if (effective != nullptr) {
    effective->clear();
    effective->reserve(input.size());
  }
  for (const auto &transition : input) {
    if (transition.songTimeMicros < kMinimumReplaySongTimeMicros ||
        (hasPreviousTime && transition.songTimeMicros < previousTime)) {
      return fail(diagnostic, "Replay input timestamps are invalid");
    }
    previousTime = transition.songTimeMicros;
    hasPreviousTime = true;
    if (!validateLogicalControl(transition.control, keyMode)) {
      return fail(diagnostic,
                  "Replay input control is invalid for its key mode");
    }

    const auto stockCode =
        BeatorajaReplayCodec::beatorajaKeyCode(transition.control, keyMode);
    std::string identity;
    if (stockCode) {
      identity = "k" + std::to_string(*stockCode);
    } else {
      identity = "x" +
                 std::to_string(static_cast<int>(transition.control.kind)) +
                 ":" + std::to_string(transition.control.player);
    }
    const auto state = extensionStates.find(identity);
    const bool current = state != extensionStates.end() && state->second;
    if (current == transition.pressed) {
      if (!allowRedundant) {
        return fail(diagnostic,
                    "Replay contains a redundant logical input transition");
      }
      continue;
    }
    extensionStates[identity] = transition.pressed;
    if (stockCode) {
      stockStates[*stockCode] = transition.pressed;
    }
    if (effective != nullptr) {
      effective->push_back(transition);
    }
  }
  return validateReplayOnlyScratchHandoffs(
      effective != nullptr ? std::span<const InputTransition>(*effective)
                           : std::span<const InputTransition>(input),
      diagnostic);
}

std::optional<std::vector<InputTransition>>
projectStockInput(const ReplayPlaybackData &replay,
                  const ReplayCodecLimits &limits, std::string &diagnostic) {
  std::vector<InputTransition> effective;
  if (!validateInput(replay.input, replay.setup.keyMode,
                     limits.maxInputTransitions, false, &effective,
                     diagnostic)) {
    return std::nullopt;
  }
  const auto options = projectSetupOptions(replay.setup, diagnostic);
  if (!options) {
    return std::nullopt;
  }
  std::vector<InputTransition> projected;
  projected.reserve(effective.size());
  std::unordered_map<int, bool> stockStates;
  bool emittedInitialState = false;
  const auto appendProjected = [&](int keyCode, std::int64_t songTimeMicros,
                                   bool pressed) -> bool {
    const auto stockControl =
        BeatorajaReplayCodec::logicalControl(keyCode, replay.setup.keyMode);
    if (!stockControl) {
      return fail(diagnostic, "Replay stock input projection is invalid");
    }
    projected.push_back({.songTimeMicros = songTimeMicros,
                         .control = *stockControl,
                         .pressed = pressed});
    return true;
  };
  const auto emitInitialState = [&]() -> bool {
    if (emittedInitialState) {
      return true;
    }
    emittedInitialState = true;
    std::vector<int> heldKeys;
    for (const auto &[keyCode, pressed] : stockStates) {
      if (pressed) {
        heldKeys.push_back(keyCode);
      }
    }
    std::ranges::sort(heldKeys);
    for (const int keyCode : heldKeys) {
      if (!appendProjected(keyCode, 0, true)) {
        return false;
      }
    }
    return true;
  };
  for (const auto &transition : effective) {
    auto projectedControl = std::optional<LogicalControl>(transition.control);
    if (options->manualAssignment) {
      projectedControl =
          projectManualControl(transition.control, *options->manualAssignment);
      if (!projectedControl) {
        fail(diagnostic, "Replay manual assignment input cannot be mapped");
        return std::nullopt;
      }
    }
    const auto keyCode = BeatorajaReplayCodec::beatorajaKeyCode(
        *projectedControl, replay.setup.keyMode);
    if (!keyCode) {
      continue;
    }
    const bool current = stockStates.contains(*keyCode) &&
                         stockStates[*keyCode];
    if (current == transition.pressed) {
      continue;
    }
    if (transition.songTimeMicros >= 0 && !emitInitialState()) {
      return std::nullopt;
    }
    stockStates[*keyCode] = transition.pressed;
    if (transition.songTimeMicros < 0) {
      continue;
    }
    if (!appendProjected(*keyCode, transition.songTimeMicros,
                         transition.pressed)) {
      return std::nullopt;
    }
  }
  if (!emitInitialState()) {
    return std::nullopt;
  }
  return projected;
}

std::optional<Bytes> stockKeyRecords(const ReplayPlaybackData &replay,
                                     const ReplayCodecLimits &limits,
                                     std::string &diagnostic) {
  const auto projected = projectStockInput(replay, limits, diagnostic);
  if (!projected) {
    return std::nullopt;
  }
  const std::size_t recordCount = std::max<std::size_t>(projected->size(), 1);
  if (recordCount > limits.maxInputTransitions ||
      recordCount > limits.maxKeyInputBytes / kKeyRecordSize) {
    fail(diagnostic, "Replay keyinput exceeds the configured limit");
    return std::nullopt;
  }
  Bytes records;
  records.reserve(recordCount * kKeyRecordSize);
  for (const auto &transition : *projected) {
    const auto keyCode = BeatorajaReplayCodec::beatorajaKeyCode(
        transition.control, replay.setup.keyMode);
    if (!keyCode) {
      fail(diagnostic, "Replay stock input projection is invalid");
      return std::nullopt;
    }
    const int signedCode = (*keyCode + 1) * (transition.pressed ? 1 : -1);
    records.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(static_cast<std::int8_t>(signedCode))));
    appendLittleEndianInt64(records, transition.songTimeMicros);
  }
  if (records.empty()) {
    // ReplayData.validate() in Beatoraja requires at least one key record.
    // An unmatched release is accepted by Beatoraja but cannot hit a note;
    // Aso's tolerant stock decoder also collapses it back to empty input.
    records.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(-1)));
    appendLittleEndianInt64(records, 0);
  }
  return records;
}

std::optional<std::vector<InputTransition>>
decodeStockInput(const Json &stage, int keyMode,
                 const ReplayCodecLimits &limits, std::string &diagnostic) {
  const auto found = stage.find("keyinput");
  if (found == stage.end() || !found->is_string()) {
    fail(diagnostic, "Replay keyinput is missing or is not a string");
    return std::nullopt;
  }
  const std::string &encoded = found->get_ref<const std::string &>();
  const auto compressed =
      base64UrlDecodeBounded(encoded, limits.maxCompressedBytes, diagnostic);
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
    fail(diagnostic, "Replay keyinput has a partial key record");
    return std::nullopt;
  }
  const std::size_t recordCount = records->size() / kKeyRecordSize;
  if (recordCount == 0) {
    fail(diagnostic, "Replay keyinput contains no key records");
    return std::nullopt;
  }
  if (recordCount > limits.maxInputTransitions) {
    fail(diagnostic, "Replay keyinput record count exceeds the limit");
    return std::nullopt;
  }

  std::vector<InputTransition> raw;
  raw.reserve(recordCount);
  for (std::size_t offset = 0; offset < records->size();
       offset += kKeyRecordSize) {
    const std::uint8_t rawCode =
        std::to_integer<std::uint8_t>((*records)[offset]);
    if (rawCode == 0 || rawCode == 0x80U) {
      fail(diagnostic, "Replay keyinput contains an invalid signed key byte");
      return std::nullopt;
    }
    const auto signedCode = static_cast<std::int8_t>(rawCode);
    const bool pressed = signedCode > 0;
    const int keyCode = std::abs(static_cast<int>(signedCode)) - 1;
    const auto control = BeatorajaReplayCodec::logicalControl(keyCode, keyMode);
    if (!control) {
      fail(diagnostic, "Replay keyinput key is invalid for its key mode");
      return std::nullopt;
    }
    raw.push_back({.songTimeMicros = littleEndianInt64(*records, offset + 1),
                   .control = *control,
                   .pressed = pressed});
  }
  std::vector<InputTransition> effective;
  if (!validateInput(raw, keyMode, limits.maxInputTransitions, true, &effective,
                     diagnostic)) {
    return std::nullopt;
  }
  return effective;
}

bool validateSetup(const ChartPlaybackSetup &setup, std::string &diagnostic) {
  if (!isCanonicalHex(setup.chartSha256, 64)) {
    return fail(diagnostic, "Replay chart SHA-256 is not canonical");
  }
  if (!setup.chartMd5.empty() && !isCanonicalHex(setup.chartMd5, 32)) {
    return fail(diagnostic, "Replay chart MD5 is not canonical");
  }
  if (!isKeyModeSupported(setup.keyMode)) {
    return fail(diagnostic, "Replay key mode is unsupported");
  }
  if (setup.randomValues.size() > 100'000) {
    return fail(diagnostic, "Replay RANDOM sequence is too large");
  }
  if (setup.randomPrng.has_value() &&
      *setup.randomPrng != bms_parser::Parser::RandomPrngId) {
    return fail(diagnostic, "Replay random PRNG is unsupported");
  }
  if (!stockLongNoteMode(setup.longNoteMode)) {
    return fail(diagnostic, "Replay long-note mode is invalid");
  }
  const int doublePlayOption = static_cast<int>(setup.doublePlayOption);
  if (doublePlayOption < static_cast<int>(DoublePlayOption::Normal) ||
      doublePlayOption > static_cast<int>(DoublePlayOption::Flip) ||
      (setup.doublePlayOption == DoublePlayOption::Flip &&
       setup.keyMode != 10 && setup.keyMode != 14)) {
    return fail(diagnostic, "Replay double-play option is unsupported");
  }
  if (!projectSetupOptions(setup, diagnostic)) {
    return false;
  }
  if (setup.initialLaneCoverPercent < 0 ||
      setup.initialLaneCoverPercent > 100) {
    return fail(diagnostic, "Replay initial lane cover is out of range");
  }
  if (!audio::PlaybackRate{.percent = setup.playbackRatePercent,
                           .mode = setup.playbackMode}
           .valid() ||
      setup.judgeWindowScalePercent <= 0 ||
      setup.judgeWindowScalePercent > 1000 ||
      !std::isfinite(setup.startingGaugePercent) ||
      setup.startingGaugePercent < 0.0F) {
    return fail(diagnostic, "Replay playback setup contains an invalid scale");
  }
  const int gaugeProfile = static_cast<int>(setup.gaugeProfile);
  const int gaugeAutoShift = static_cast<int>(setup.gaugeAutoShift);
  const int candidateSelection = static_cast<int>(setup.candidateSelection);
  if (!isGaugeTypeSupported(setup.initialGaugeType) ||
      !isGaugeTypeSupported(setup.gaugeAutoShiftLowerBound) ||
      gaugeProfile < static_cast<int>(GaugeProfile::Standard) ||
      gaugeProfile > static_cast<int>(GaugeProfile::Standard24Keys) ||
      gaugeAutoShift < static_cast<int>(GaugeAutoShiftMode::None) ||
      gaugeAutoShift > static_cast<int>(GaugeAutoShiftMode::BestClear) ||
      candidateSelection <
          static_cast<int>(gameplay::CandidateSelectionMode::LR2) ||
      candidateSelection >
          static_cast<int>(gameplay::CandidateSelectionMode::Score)) {
    return fail(diagnostic, "Replay gauge setup is invalid");
  }
  const GaugeProfile resolvedGaugeProfile =
      resolveGaugeProfile(setup.gaugeProfile, setup.keyMode);
  if (setup.startingGaugePercent >
      gaugeStartingMaximumValue(
          setup.initialGaugeType, setup.gaugeAutoShift,
          setup.gaugeAutoShiftLowerBound, resolvedGaugeProfile)) {
    return fail(diagnostic, "Replay starting gauge is out of range");
  }
  if (setup.startingGaugeState.has_value()) {
    const auto &state = *setup.startingGaugeState;
    const auto ruleset = gameplayRulesetFromId(setup.playbackRulesetId);
    const GaugeProfile resolvedSetupProfile =
        ruleset.has_value()
            ? resolveGaugeProfileForRuleset(
                  *ruleset, setup.gaugeProfile, setup.keyMode)
            : resolveGaugeProfile(setup.gaugeProfile, setup.keyMode);
    const GaugeProfile resolvedStateProfile =
        ruleset.has_value()
            ? resolveGaugeProfileForRuleset(
                  *ruleset, state.gaugeProfile, setup.keyMode)
            : resolveGaugeProfile(state.gaugeProfile, setup.keyMode);
    const int gaugeType = gaugeTypeIndex(state.gaugeType);
    const int selectedGaugeType = gaugeTypeIndex(state.selectedGaugeType);
    const int lowerBound = gaugeTypeIndex(state.gaugeAutoShiftLowerBound);
    const int stateProfile = static_cast<int>(state.gaugeProfile);
    const int stateAutoShift = static_cast<int>(state.gaugeAutoShift);
    if (!isGaugeTypeSupported(state.gaugeType) ||
        !isGaugeTypeSupported(state.selectedGaugeType) ||
        !isGaugeTypeSupported(state.gaugeAutoShiftLowerBound) ||
        gaugeType < 0 || gaugeType >= static_cast<int>(kGaugeTypeCount) ||
        selectedGaugeType < 0 ||
        selectedGaugeType >= static_cast<int>(kGaugeTypeCount) ||
        lowerBound < 0 || lowerBound >= static_cast<int>(kGaugeTypeCount) ||
        stateProfile < static_cast<int>(GaugeProfile::Standard) ||
        stateProfile > static_cast<int>(GaugeProfile::Standard24Keys) ||
        stateAutoShift < static_cast<int>(GaugeAutoShiftMode::None) ||
        stateAutoShift > static_cast<int>(GaugeAutoShiftMode::BestClear) ||
        state.selectedGaugeType != setup.initialGaugeType ||
        resolvedStateProfile != resolvedSetupProfile ||
        state.gaugeAutoShift != setup.gaugeAutoShift ||
        state.gaugeAutoShiftLowerBound != setup.gaugeAutoShiftLowerBound ||
        !std::isfinite(state.currentGauge) ||
        std::ranges::any_of(
            state.gaugeValues,
            [](float value) { return !std::isfinite(value); })) {
      return fail(diagnostic, "Replay starting gauge state is invalid");
    }
    for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
      const float value = state.gaugeValues[static_cast<std::size_t>(index)];
      if (value < 0.0F ||
          value > gaugeMaximumValue(gaugeTypeAtIndex(index), state.gaugeProfile)) {
        return fail(diagnostic, "Replay starting gauge value is out of range");
      }
    }
    if (state.currentGauge != state.gaugeValues[static_cast<std::size_t>(
                                  gaugeTypeIndex(state.gaugeType))]) {
      return fail(diagnostic, "Replay active starting gauge is inconsistent");
    }
  }
  return true;
}

template <typename T> Json optionalJson(const std::optional<T> &value) {
  return value ? Json(*value) : Json(nullptr);
}

Json encodeSetupExtension(const ChartPlaybackSetup &setup) {
  Json startingGaugeState = nullptr;
  if (setup.startingGaugeState.has_value()) {
    const auto &state = *setup.startingGaugeState;
    startingGaugeState = {
        {"gaugeType", gaugeTypeIndex(state.gaugeType)},
        {"selectedGaugeType", gaugeTypeIndex(state.selectedGaugeType)},
        {"gaugeAutoShiftLowerBound",
         gaugeTypeIndex(state.gaugeAutoShiftLowerBound)},
        {"gaugeProfile", static_cast<int>(state.gaugeProfile)},
        {"gaugeAutoShift", static_cast<int>(state.gaugeAutoShift)},
        {"currentGauge", state.currentGauge},
        {"gaugeValues", state.gaugeValues},
        {"gaugeSurvivalFailed", state.gaugeSurvivalFailed},
    };
  }
  return Json{
      {"chartSha256", setup.chartSha256},
      {"chartMd5", setup.chartMd5},
      {"keyMode", setup.keyMode},
      {"longNoteMode", setup.longNoteMode},
      {"hasUndefinedLongNotes", setup.hasUndefinedLongNotes},
      {"randomSeed", optionalJson(setup.randomSeed)},
      {"randomPrng", optionalJson(setup.randomPrng)},
      {"randomValues", setup.randomValues},
      {"playOption", optionalJson(setup.playOption)},
      {"playOptionSeed", optionalJson(setup.playOptionSeed)},
      {"playOption2", optionalJson(setup.playOption2)},
      {"playOption2Seed", optionalJson(setup.playOption2Seed)},
      {"doublePlayOption", static_cast<int>(setup.doublePlayOption)},
      {"assistOption", setup.assistOption},
      {"initialGaugeType", gaugeTypeIndex(setup.initialGaugeType)},
      {"gaugeProfile", static_cast<int>(setup.gaugeProfile)},
      {"gaugeAutoShift", static_cast<int>(setup.gaugeAutoShift)},
      {"gaugeAutoShiftLowerBound",
       gaugeTypeIndex(setup.gaugeAutoShiftLowerBound)},
      {"playbackRulesetId", setup.playbackRulesetId},
      {"playbackRulesetRevision", setup.playbackRulesetRevision},
      {"playbackRatePercent", setup.playbackRatePercent},
      {"playbackMode", static_cast<int>(setup.playbackMode)},
      {"candidateSelection", static_cast<int>(setup.candidateSelection)},
      {"judgeWindowScalePercent", setup.judgeWindowScalePercent},
      {"startingGaugePercent", setup.startingGaugePercent},
      {"startingGaugeState", std::move(startingGaugeState)},
      {"clubMode", setup.clubMode},
      {"initialLaneCoverPercent", setup.initialLaneCoverPercent},
      {"laneCoverEnabled", setup.laneCoverEnabled},
  };
}

Json encodeInputExtension(const std::vector<InputTransition> &input) {
  Json result = Json::array();
  for (const auto &transition : input) {
    Json item{
        {"songTimeMicros", transition.songTimeMicros},
        {"kind", static_cast<int>(transition.control.kind)},
        {"player", transition.control.player},
        {"lane", transition.control.lane},
        {"pressed", transition.pressed},
    };
    if (transition.replayOnly) {
      item["replayOnly"] = true;
    }
    result.push_back(std::move(item));
  }
  return result;
}

Json encodeTouchExtension(const std::vector<ReplayTouchSample> &samples) {
  Json result = Json::array();
  for (const auto &sample : samples) {
    result.push_back({
        {"action", static_cast<int>(sample.action)},
        {"fingerId", sample.fingerId},
        {"songTimeMicros", sample.songTimeMicros},
        {"x", sample.x},
        {"y", sample.y},
    });
  }
  return result;
}

Json encodeCoverExtension(const std::vector<ReplayLaneCoverEvent> &events) {
  Json result = Json::array();
  for (const auto &event : events) {
    result.push_back({
        {"songTimeMicros", event.songTimeMicros},
        {"noteStartPositionPercent", event.noteStartPositionPercent},
        {"resetVisibleTimeReference", event.resetVisibleTimeReference},
    });
  }
  return result;
}

Json encodeLegacyExtension(const std::optional<LegacyPlaybackTrack> &legacy) {
  if (!legacy) {
    return nullptr;
  }
  Json events = Json::array();
  for (const auto &event : legacy->events) {
    events.push_back({
        {"action", static_cast<int>(event.action)},
        {"lane", event.lane},
        {"noteTimeMicros", event.noteTimeMicros},
        {"songTimeMicros", event.songTimeMicros},
        {"judgeTimeMicros", event.judgeTimeMicros},
        {"judgement", static_cast<int>(event.judgement)},
        {"diffMicros", event.diffMicros},
        {"gauge", event.gauge},
        {"gaugeType", gaugeTypeIndex(event.gaugeType)},
        {"combo", event.combo},
        {"score", event.score},
    });
  }
  return Json{{"stockScratchDirectionBestEffort",
               legacy->stockScratchDirectionBestEffort},
              {"events", std::move(events)}};
}

bool validateSupplementalTracks(const ReplayPlaybackData &replay,
                                const ReplayCodecLimits &limits,
                                std::string &diagnostic) {
  if (replay.touchSamples.size() > limits.maxTouchSamples ||
      replay.laneCoverEvents.size() > limits.maxLaneCoverEvents ||
      (replay.legacy &&
       replay.legacy->events.size() > limits.maxInputTransitions)) {
    return fail(diagnostic,
                "Replay extension array exceeds its configured limit");
  }
  std::int64_t previousTouch = 0;
  bool hasPreviousTouch = false;
  for (const auto &sample : replay.touchSamples) {
    if ((hasPreviousTouch && sample.songTimeMicros < previousTouch) ||
        sample.songTimeMicros < kMinimumReplaySongTimeMicros ||
        !std::isfinite(sample.x) || !std::isfinite(sample.y) ||
        sample.x < 0.0F || sample.x > 1.0F || sample.y < 0.0F ||
        sample.y > 1.0F ||
        static_cast<int>(sample.action) <
            static_cast<int>(ReplayTouchAction::Down) ||
        static_cast<int>(sample.action) >
            static_cast<int>(ReplayTouchAction::Cancel)) {
      return fail(diagnostic, "Replay touch sample is invalid");
    }
    previousTouch = sample.songTimeMicros;
    hasPreviousTouch = true;
  }
  std::int64_t previousCover = 0;
  bool hasPreviousCover = false;
  for (const auto &event : replay.laneCoverEvents) {
    if ((hasPreviousCover && event.songTimeMicros < previousCover) ||
        event.songTimeMicros < kMinimumReplaySongTimeMicros ||
        event.noteStartPositionPercent < 0 ||
        event.noteStartPositionPercent > 100) {
      return fail(diagnostic, "Replay timed lane-cover event is invalid");
    }
    previousCover = event.songTimeMicros;
    hasPreviousCover = true;
  }
  if (replay.legacy) {
    std::int64_t previousLegacy = 0;
    bool hasPreviousLegacy = false;
    for (const auto &event : replay.legacy->events) {
      if ((hasPreviousLegacy && event.songTimeMicros < previousLegacy) ||
          !std::isfinite(event.gauge) ||
          !isGaugeTypeSupported(event.gaugeType) ||
          static_cast<int>(event.action) < 0 ||
          static_cast<int>(event.action) >
              static_cast<int>(LegacyPlaybackAction::MultiBad) ||
          static_cast<int>(event.judgement) < 0 ||
          static_cast<int>(event.judgement) >= JudgementCount) {
        return fail(diagnostic, "Replay legacy playback annotation is invalid");
      }
      previousLegacy = event.songTimeMicros;
      hasPreviousLegacy = true;
    }
  }
  return true;
}

struct AggregateReplayCounts {
  std::size_t inputTransitions = 0;
  std::size_t touchSamples = 0;
  std::size_t laneCoverEvents = 0;
  std::size_t legacyEvents = 0;

  bool include(const ReplayPlaybackData &replay,
               const ReplayCodecLimits &limits, std::string &diagnostic) {
    const auto addWithinLimit = [](std::size_t &total, std::size_t count,
                                   std::size_t limit) {
      if (count > limit || total > limit - count) {
        return false;
      }
      total += count;
      return true;
    };
    if (!addWithinLimit(inputTransitions, replay.input.size(),
                        limits.maxInputTransitions) ||
        !addWithinLimit(touchSamples, replay.touchSamples.size(),
                        limits.maxTouchSamples) ||
        !addWithinLimit(laneCoverEvents, replay.laneCoverEvents.size(),
                        limits.maxLaneCoverEvents) ||
        !addWithinLimit(legacyEvents,
                        replay.legacy ? replay.legacy->events.size() : 0,
                        limits.maxInputTransitions)) {
      return fail(diagnostic,
                  "Replay course arrays exceed their aggregate limits");
    }
    return true;
  }
};

std::optional<Json>
encodeStage(const ReplayPlaybackData &replay, std::int64_t playedAtUnixMillis,
            std::string_view envelope, int stageIndex, int stageCount,
            std::int64_t restMicrosAfterStage, const ReplayCodecLimits &limits,
            std::string &diagnostic) {
  if (playedAtUnixMillis < 0 || !validateSetup(replay.setup, diagnostic) ||
      !validateSupplementalTracks(replay, limits, diagnostic)) {
    return std::nullopt;
  }
  const auto records = stockKeyRecords(replay, limits, diagnostic);
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
  const auto options = projectSetupOptions(replay.setup, diagnostic);
  if (!options) {
    diagnostic = "Replay play option cannot be represented by Beatoraja";
    return std::nullopt;
  }
  const auto stockMode = stockLongNoteMode(replay.setup.longNoteMode);
  if (!stockMode) {
    diagnostic = "Replay long-note mode cannot be represented by Beatoraja";
    return std::nullopt;
  }

  Json extension{
      {"schemaVersion", kAsoSchemaVersion},
      {"envelope", envelope},
      {"stageIndex", stageIndex},
      {"stageCount", stageCount},
      {"setup", encodeSetupExtension(replay.setup)},
      {"input", encodeInputExtension(replay.input)},
      {"touchSamples", encodeTouchExtension(replay.touchSamples)},
      {"laneCoverEvents", encodeCoverExtension(replay.laneCoverEvents)},
      {"legacy", encodeLegacyExtension(replay.legacy)},
      {"restMicrosAfterStage", restMicrosAfterStage},
  };

  return Json{
      {"player", "AsoBMaShow"},
      {"sha256", replay.setup.chartSha256},
      {"mode", *stockMode},
      {"keyinput", base64UrlEncode(*compressedKeys)},
      {"gauge", gaugeTypeIndex(replay.setup.initialGaugeType)},
      {"rand", replay.setup.randomValues},
      {"date", playedAtUnixMillis / 1000},
      {"sevenToNinePattern", 0},
      {"randomoption", options->option1},
      {"randomoptionseed", replay.setup.playOptionSeed.value_or(-1)},
      {"randomoption2", options->option2},
      {"randomoption2seed", replay.setup.playOption2Seed.value_or(-1)},
      {"doubleoption", static_cast<int>(replay.setup.doublePlayOption)},
      {"config",
       {{"lanecover", replay.setup.initialLaneCoverPercent / 100.0F},
        {"enablelanecover", replay.setup.laneCoverEnabled}}},
      {"asobmashow", std::move(extension)},
  };
}

template <typename T>
bool readRequired(const Json &object, std::string_view name, T &output,
                  std::string &diagnostic) {
  const auto found = object.find(std::string(name));
  if (found == object.end()) {
    return fail(diagnostic,
                "Replay extension field is missing: " + std::string(name));
  }
  try {
    output = found->get<T>();
    return true;
  } catch (const Json::exception &) {
    return fail(diagnostic, "Replay extension field has the wrong type: " +
                                std::string(name));
  }
}

template <typename T>
bool readOptional(const Json &object, std::string_view name,
                  std::optional<T> &output, std::string &diagnostic) {
  const auto found = object.find(std::string(name));
  if (found == object.end()) {
    return fail(diagnostic,
                "Replay extension field is missing: " + std::string(name));
  }
  if (found->is_null()) {
    output.reset();
    return true;
  }
  try {
    output = found->get<T>();
    return true;
  } catch (const Json::exception &) {
    return fail(diagnostic, "Replay extension field has the wrong type: " +
                                std::string(name));
  }
}

bool decodeSetupExtension(const Json &source, ChartPlaybackSetup &setup,
                          std::string &diagnostic) {
  if (!source.is_object()) {
    return fail(diagnostic, "Replay extension setup is not an object");
  }
  int profile = 0;
  int autoShift = 0;
  int lowerBound = 0;
  int initialGaugeType = 0;
  int playbackMode = 0;
  int candidateSelection = 0;
  if (!readRequired(source, "chartSha256", setup.chartSha256, diagnostic) ||
      !readRequired(source, "chartMd5", setup.chartMd5, diagnostic) ||
      !readRequired(source, "keyMode", setup.keyMode, diagnostic) ||
      !readRequired(source, "longNoteMode", setup.longNoteMode, diagnostic) ||
      !readRequired(source, "hasUndefinedLongNotes",
                    setup.hasUndefinedLongNotes, diagnostic) ||
      !readOptional(source, "randomSeed", setup.randomSeed, diagnostic) ||
      !readOptional(source, "randomPrng", setup.randomPrng, diagnostic) ||
      !readRequired(source, "randomValues", setup.randomValues, diagnostic) ||
      !readOptional(source, "playOption", setup.playOption, diagnostic) ||
      !readOptional(source, "playOptionSeed", setup.playOptionSeed,
                    diagnostic) ||
      !readOptional(source, "playOption2", setup.playOption2, diagnostic) ||
      !readOptional(source, "playOption2Seed", setup.playOption2Seed,
                    diagnostic) ||
      !readRequired(source, "assistOption", setup.assistOption, diagnostic) ||
      !readRequired(source, "initialGaugeType", initialGaugeType,
                    diagnostic) ||
      !readRequired(source, "gaugeProfile", profile, diagnostic) ||
      !readRequired(source, "gaugeAutoShift", autoShift, diagnostic) ||
      !readRequired(source, "gaugeAutoShiftLowerBound", lowerBound,
                    diagnostic) ||
      !readRequired(source, "playbackRulesetId", setup.playbackRulesetId,
                    diagnostic) ||
      !readRequired(source, "playbackRulesetRevision",
                    setup.playbackRulesetRevision, diagnostic) ||
      !readRequired(source, "playbackRatePercent", setup.playbackRatePercent,
                    diagnostic) ||
      !readRequired(source, "playbackMode", playbackMode, diagnostic) ||
      !readRequired(source, "candidateSelection", candidateSelection,
                    diagnostic) ||
      !readRequired(source, "judgeWindowScalePercent",
                    setup.judgeWindowScalePercent, diagnostic) ||
      !readRequired(source, "startingGaugePercent", setup.startingGaugePercent,
                    diagnostic) ||
      !readRequired(source, "clubMode", setup.clubMode, diagnostic) ||
      !readRequired(source, "initialLaneCoverPercent",
                    setup.initialLaneCoverPercent, diagnostic) ||
      !readRequired(source, "laneCoverEnabled", setup.laneCoverEnabled,
                    diagnostic)) {
    return false;
  }
  const auto doublePlayOption = source.find("doublePlayOption");
  if (doublePlayOption != source.end()) {
    int value = 0;
    try {
      value = doublePlayOption->get<int>();
    } catch (const Json::exception &) {
      return fail(diagnostic,
                  "Replay extension double-play option has the wrong type");
    }
    if (value < static_cast<int>(DoublePlayOption::Normal) ||
        value > static_cast<int>(DoublePlayOption::Flip)) {
      return fail(diagnostic,
                  "Replay extension double-play option is unsupported");
    }
    setup.doublePlayOption = static_cast<DoublePlayOption>(value);
  }
  const auto startingState = source.find("startingGaugeState");
  if (startingState != source.end() && !startingState->is_null()) {
    if (!startingState->is_object()) {
      return fail(diagnostic, "Replay starting gauge state is not an object");
    }
    int gaugeType = 0;
    int selectedGaugeType = 0;
    int stateLowerBound = 0;
    int stateProfile = 0;
    int stateAutoShift = 0;
    GaugeStateSnapshot state;
    if (!readRequired(*startingState, "gaugeType", gaugeType, diagnostic) ||
        !readRequired(*startingState, "selectedGaugeType", selectedGaugeType,
                      diagnostic) ||
        !readRequired(*startingState, "gaugeAutoShiftLowerBound",
                      stateLowerBound, diagnostic) ||
        !readRequired(*startingState, "gaugeProfile", stateProfile,
                      diagnostic) ||
        !readRequired(*startingState, "gaugeAutoShift", stateAutoShift,
                      diagnostic) ||
        !readRequired(*startingState, "currentGauge", state.currentGauge,
                      diagnostic) ||
        !readRequired(*startingState, "gaugeValues", state.gaugeValues,
                      diagnostic) ||
        !readRequired(*startingState, "gaugeSurvivalFailed",
                      state.gaugeSurvivalFailed, diagnostic)) {
      return false;
    }
    if (gaugeType < 0 || gaugeType >= static_cast<int>(kGaugeTypeCount) ||
        selectedGaugeType < 0 ||
        selectedGaugeType >= static_cast<int>(kGaugeTypeCount) ||
        stateLowerBound < 0 ||
        stateLowerBound >= static_cast<int>(kGaugeTypeCount) ||
        stateProfile < static_cast<int>(GaugeProfile::Standard) ||
        stateProfile > static_cast<int>(GaugeProfile::Standard24Keys) ||
        stateAutoShift < static_cast<int>(GaugeAutoShiftMode::None) ||
        stateAutoShift > static_cast<int>(GaugeAutoShiftMode::BestClear)) {
      return fail(diagnostic, "Replay starting gauge enum is out of range");
    }
    state.gaugeType = gaugeTypeAtIndex(gaugeType);
    state.selectedGaugeType = gaugeTypeAtIndex(selectedGaugeType);
    state.gaugeAutoShiftLowerBound = gaugeTypeAtIndex(stateLowerBound);
    state.gaugeProfile = static_cast<GaugeProfile>(stateProfile);
    state.gaugeAutoShift = static_cast<GaugeAutoShiftMode>(stateAutoShift);
    setup.startingGaugeState = state;
  } else {
    setup.startingGaugeState.reset();
  }
  if (profile < static_cast<int>(GaugeProfile::Standard) ||
      profile > static_cast<int>(GaugeProfile::Standard24Keys) ||
      autoShift < static_cast<int>(GaugeAutoShiftMode::None) ||
      autoShift > static_cast<int>(GaugeAutoShiftMode::BestClear) ||
      lowerBound < 0 || lowerBound >= static_cast<int>(kGaugeTypeCount) ||
      initialGaugeType < 0 ||
      initialGaugeType >= static_cast<int>(kGaugeTypeCount) ||
      setup.randomValues.size() > 100'000 ||
      playbackMode < static_cast<int>(audio::PlaybackMode::PitchShift) ||
      playbackMode > static_cast<int>(audio::PlaybackMode::TimeStretch) ||
      candidateSelection <
          static_cast<int>(gameplay::CandidateSelectionMode::LR2) ||
      candidateSelection >
          static_cast<int>(gameplay::CandidateSelectionMode::Score)) {
    return fail(diagnostic, "Replay extension setup enum is out of range");
  }
  setup.gaugeProfile = static_cast<GaugeProfile>(profile);
  setup.initialGaugeType = gaugeTypeAtIndex(initialGaugeType);
  setup.gaugeAutoShift = static_cast<GaugeAutoShiftMode>(autoShift);
  setup.gaugeAutoShiftLowerBound = gaugeTypeAtIndex(lowerBound);
  setup.playbackMode = static_cast<audio::PlaybackMode>(playbackMode);
  setup.candidateSelection =
      static_cast<gameplay::CandidateSelectionMode>(candidateSelection);
  return validateSetup(setup, diagnostic);
}

bool decodeInputExtension(const Json &source, int keyMode,
                          const ReplayCodecLimits &limits,
                          std::vector<InputTransition> &output,
                          std::string &diagnostic) {
  if (!source.is_array() || source.size() > limits.maxInputTransitions) {
    return fail(diagnostic,
                "Replay extension input array is invalid or too large");
  }
  std::vector<InputTransition> raw;
  raw.reserve(source.size());
  for (const Json &item : source) {
    if (!item.is_object()) {
      return fail(diagnostic, "Replay extension input item is not an object");
    }
    InputTransition transition;
    int kind = 0;
    if (!readRequired(item, "songTimeMicros", transition.songTimeMicros,
                      diagnostic) ||
        !readRequired(item, "kind", kind, diagnostic) ||
        !readRequired(item, "player", transition.control.player, diagnostic) ||
        !readRequired(item, "lane", transition.control.lane, diagnostic) ||
        !readRequired(item, "pressed", transition.pressed, diagnostic)) {
      return false;
    }
    if (kind < static_cast<int>(LogicalControlKind::Lane) ||
        kind > static_cast<int>(LogicalControlKind::Select)) {
      return fail(diagnostic,
                  "Replay extension logical control kind is invalid");
    }
    if (const auto replayOnly = item.find("replayOnly");
        replayOnly != item.end()) {
      if (!replayOnly->is_boolean()) {
        return fail(diagnostic,
                    "Replay extension replay-only marker has the wrong type");
      }
      transition.replayOnly = replayOnly->get<bool>();
    }
    transition.control.kind = static_cast<LogicalControlKind>(kind);
    raw.push_back(transition);
  }
  return validateInput(raw, keyMode, limits.maxInputTransitions, false, &output,
                       diagnostic);
}

bool decodeTouchExtension(const Json &source, const ReplayCodecLimits &limits,
                          std::vector<ReplayTouchSample> &output,
                          std::string &diagnostic) {
  if (!source.is_array() || source.size() > limits.maxTouchSamples) {
    return fail(diagnostic,
                "Replay extension touch array is invalid or too large");
  }
  output.clear();
  output.reserve(source.size());
  for (const Json &item : source) {
    ReplayTouchSample sample;
    int action = 0;
    if (!item.is_object() ||
        !readRequired(item, "action", action, diagnostic) ||
        !readRequired(item, "fingerId", sample.fingerId, diagnostic) ||
        !readRequired(item, "songTimeMicros", sample.songTimeMicros,
                      diagnostic) ||
        !readRequired(item, "x", sample.x, diagnostic) ||
        !readRequired(item, "y", sample.y, diagnostic)) {
      if (diagnostic.empty()) {
        diagnostic = "Replay extension touch item is invalid";
      }
      return false;
    }
    if (action < static_cast<int>(ReplayTouchAction::Down) ||
        action > static_cast<int>(ReplayTouchAction::Cancel)) {
      return fail(diagnostic, "Replay extension touch action is invalid");
    }
    sample.action = static_cast<ReplayTouchAction>(action);
    output.push_back(sample);
  }
  ReplayPlaybackData validation;
  validation.touchSamples = output;
  return validateSupplementalTracks(validation, limits, diagnostic);
}

bool decodeCoverExtension(const Json &source, const ReplayCodecLimits &limits,
                          std::vector<ReplayLaneCoverEvent> &output,
                          std::string &diagnostic) {
  if (!source.is_array() || source.size() > limits.maxLaneCoverEvents) {
    return fail(diagnostic,
                "Replay extension lane-cover array is invalid or too large");
  }
  output.clear();
  output.reserve(source.size());
  for (const Json &item : source) {
    ReplayLaneCoverEvent event;
    if (!item.is_object() ||
        !readRequired(item, "songTimeMicros", event.songTimeMicros,
                      diagnostic) ||
        !readRequired(item, "noteStartPositionPercent",
                      event.noteStartPositionPercent, diagnostic) ||
        !readRequired(item, "resetVisibleTimeReference",
                      event.resetVisibleTimeReference, diagnostic)) {
      if (diagnostic.empty()) {
        diagnostic = "Replay extension lane-cover item is invalid";
      }
      return false;
    }
    output.push_back(event);
  }
  ReplayPlaybackData validation;
  validation.laneCoverEvents = output;
  return validateSupplementalTracks(validation, limits, diagnostic);
}

bool decodeLegacyExtension(const Json &source, const ReplayCodecLimits &limits,
                           std::optional<LegacyPlaybackTrack> &output,
                           std::string &diagnostic) {
  if (source.is_null()) {
    output.reset();
    return true;
  }
  if (!source.is_object()) {
    return fail(diagnostic, "Replay extension legacy track is not an object");
  }
  LegacyPlaybackTrack track;
  const auto events = source.find("events");
  if (!readRequired(source, "stockScratchDirectionBestEffort",
                    track.stockScratchDirectionBestEffort, diagnostic) ||
      events == source.end() || !events->is_array() ||
      events->size() > limits.maxInputTransitions) {
    return fail(diagnostic, "Replay extension legacy event array is invalid");
  }
  track.events.reserve(events->size());
  for (const Json &item : *events) {
    LegacyPlaybackEvent event;
    int action = 0;
    int judgement = 0;
    int gauge = 0;
    if (!item.is_object() ||
        !readRequired(item, "action", action, diagnostic) ||
        !readRequired(item, "lane", event.lane, diagnostic) ||
        !readRequired(item, "noteTimeMicros", event.noteTimeMicros,
                      diagnostic) ||
        !readRequired(item, "songTimeMicros", event.songTimeMicros,
                      diagnostic) ||
        !readRequired(item, "judgeTimeMicros", event.judgeTimeMicros,
                      diagnostic) ||
        !readRequired(item, "judgement", judgement, diagnostic) ||
        !readRequired(item, "diffMicros", event.diffMicros, diagnostic) ||
        !readRequired(item, "gauge", event.gauge, diagnostic) ||
        !readRequired(item, "gaugeType", gauge, diagnostic) ||
        !readRequired(item, "combo", event.combo, diagnostic) ||
        !readRequired(item, "score", event.score, diagnostic)) {
      return false;
    }
    if (action < 0 ||
        action > static_cast<int>(LegacyPlaybackAction::MultiBad) ||
        judgement < 0 || judgement >= JudgementCount || gauge < 0 ||
        gauge >= static_cast<int>(kGaugeTypeCount)) {
      return fail(diagnostic, "Replay extension legacy event enum is invalid");
    }
    event.action = static_cast<LegacyPlaybackAction>(action);
    event.judgement = static_cast<Judgement>(judgement);
    event.gaugeType = gaugeTypeAtIndex(gauge);
    track.events.push_back(event);
  }
  ReplayPlaybackData validation;
  validation.legacy = track;
  if (!validateSupplementalTracks(validation, limits, diagnostic)) {
    return false;
  }
  output = std::move(track);
  return true;
}

bool decodeStockSetup(const Json &stage, ChartPlaybackSetup &setup,
                      std::string &diagnostic) {
  if (!stage.is_object()) {
    return fail(diagnostic, "Replay stage is not a JSON object");
  }
  int gauge = 0;
  int option1 = 0;
  int option2 = 0;
  int doubleOption = 0;
  std::int64_t seed1 = -1;
  std::int64_t seed2 = -1;
  int stockMode = 0;
  if (!readRequired(stage, "sha256", setup.chartSha256, diagnostic) ||
      !readRequired(stage, "mode", stockMode, diagnostic) ||
      !readRequired(stage, "gauge", gauge, diagnostic)) {
    return false;
  }
  const auto applicationMode = applicationLongNoteMode(stockMode);
  if (!isCanonicalHex(setup.chartSha256, 64) || !applicationMode || gauge < 0 ||
      gauge >= static_cast<int>(kGaugeTypeCount)) {
    return fail(diagnostic,
                "Replay stock setup contains an invalid identity or enum");
  }
  setup.longNoteMode = *applicationMode;
  setup.initialGaugeType = gaugeTypeAtIndex(gauge);
  setup.playbackRulesetId = "beatoraja";
  setup.playbackRulesetRevision =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja).version;
  setup.playbackMode = audio::PlaybackMode::PitchShift;
  setup.candidateSelection = gameplay::CandidateSelectionMode::Lowest;

  const auto rand = stage.find("rand");
  if (rand != stage.end()) {
    if (!rand->is_array() || rand->size() > 100'000) {
      return fail(diagnostic,
                  "Replay stock RANDOM sequence is invalid or too large");
    }
    try {
      setup.randomValues = rand->get<std::vector<int>>();
    } catch (const Json::exception &) {
      return fail(diagnostic, "Replay stock RANDOM sequence has a non-integer");
    }
  }

  const auto readStockInteger = [&](std::string_view name, auto &value,
                                    auto fallback) -> bool {
    const auto found = stage.find(std::string(name));
    if (found == stage.end()) {
      value = fallback;
      return true;
    }
    try {
      value = found->template get<std::decay_t<decltype(value)>>();
      return true;
    } catch (const Json::exception &) {
      return fail(diagnostic, "Replay stock field has the wrong type: " +
                                  std::string(name));
    }
  };
  if (!readStockInteger("randomoption", option1, 0) ||
      !readStockInteger("randomoption2", option2, 0) ||
      !readStockInteger("doubleoption", doubleOption, 0) ||
      !readStockInteger("randomoptionseed", seed1,
                        static_cast<std::int64_t>(-1)) ||
      !readStockInteger("randomoption2seed", seed2,
                        static_cast<std::int64_t>(-1))) {
    return false;
  }
  setup.playOption = optionName(option1);
  setup.playOption2 = optionName(option2);
  if (!setup.playOption || !setup.playOption2) {
    return fail(diagnostic, "Replay stock play option is unsupported");
  }
  if (doubleOption < static_cast<int>(DoublePlayOption::Normal) ||
      doubleOption > static_cast<int>(DoublePlayOption::Flip)) {
    return fail(diagnostic, "Replay stock double-play option is unsupported");
  }
  setup.doublePlayOption = static_cast<DoublePlayOption>(doubleOption);
  setup.playOptionSeed =
      seed1 >= 0 ? std::optional<std::int64_t>(seed1) : std::nullopt;
  setup.playOption2Seed =
      seed2 >= 0 ? std::optional<std::int64_t>(seed2) : std::nullopt;

  const auto config = stage.find("config");
  if (config != stage.end() && !config->is_null()) {
    if (!config->is_object()) {
      return fail(diagnostic, "Replay stock config is not an object");
    }
    float cover = 0.2F;
    bool enabled = true;
    const auto coverField = config->find("lanecover");
    const auto enabledField = config->find("enablelanecover");
    try {
      if (coverField != config->end()) {
        cover = coverField->get<float>();
      }
      if (enabledField != config->end()) {
        enabled = enabledField->get<bool>();
      }
    } catch (const Json::exception &) {
      return fail(diagnostic,
                  "Replay stock lane-cover config has a wrong type");
    }
    if (!std::isfinite(cover) || cover < 0.0F || cover > 1.0F) {
      return fail(diagnostic, "Replay stock lane-cover value is out of range");
    }
    setup.initialLaneCoverPercent = static_cast<int>(std::lround(cover * 100));
    setup.laneCoverEnabled = enabled;
  }
  return true;
}

bool validateStockProjection(const ChartPlaybackSetup &stock,
                             const ReplayPlaybackData &extension,
                             const ReplayCodecLimits &limits,
                             const std::vector<InputTransition> &stockInput,
                             std::string &diagnostic) {
  const auto options = projectSetupOptions(extension.setup, diagnostic);
  if (!options) {
    return false;
  }
  if (stock.playOption != optionName(options->option1) ||
      stock.playOption2 != optionName(options->option2) ||
      stock.playOptionSeed != extension.setup.playOptionSeed ||
      stock.playOption2Seed != extension.setup.playOption2Seed ||
      stock.randomValues != extension.setup.randomValues ||
      stock.initialGaugeType != extension.setup.initialGaugeType ||
      stock.initialLaneCoverPercent !=
          extension.setup.initialLaneCoverPercent ||
      stock.laneCoverEnabled != extension.setup.laneCoverEnabled) {
    return fail(diagnostic,
                "Replay stock and extension playback setup fields differ");
  }
  const auto expectedInput = projectStockInput(extension, limits, diagnostic);
  if (!expectedInput) {
    return false;
  }
  const bool sameInputSemantics =
      expectedInput->size() == stockInput.size() &&
      std::ranges::equal(*expectedInput, stockInput,
                         [](const auto &expected, const auto &actual) {
                           return expected.songTimeMicros ==
                                      actual.songTimeMicros &&
                                  expected.control == actual.control &&
                                  expected.pressed == actual.pressed;
                         });
  if (!sameInputSemantics) {
    return fail(diagnostic,
                "Replay stock and extension input projections differ");
  }
  return true;
}

bool decodeStage(const Json &stage, std::string_view expectedEnvelope,
                 std::optional<int> expectedKeyMode,
                 const ReplayCodecLimits &limits, StageDecode &result,
                 std::string &diagnostic) {
  if (!decodeStockSetup(stage, result.data.setup, diagnostic)) {
    return false;
  }
  const ChartPlaybackSetup stockSetup = result.data.setup;
  const std::string stockChartSha256 = result.data.setup.chartSha256;
  const auto stockMode = stockLongNoteMode(result.data.setup.longNoteMode);
  const DoublePlayOption stockDoublePlayOption =
      result.data.setup.doublePlayOption;

  const auto extension = stage.find("asobmashow");
  const Json *supported = nullptr;
  if (extension != stage.end()) {
    result.extensionPresent = true;
    if (!extension->is_object()) {
      return fail(diagnostic, "Aso replay extension is not an object");
    }
    int schemaVersion = 0;
    if (!readRequired(*extension, "schemaVersion", schemaVersion, diagnostic)) {
      return false;
    }
    if (schemaVersion == kAsoSchemaVersion) {
      result.supportedExtension = true;
      supported = &*extension;
      const auto setup = supported->find("setup");
      if (setup == supported->end() ||
          !decodeSetupExtension(*setup, result.data.setup, diagnostic)) {
        return false;
      }
      if (result.data.setup.doublePlayOption != stockDoublePlayOption) {
        return fail(diagnostic,
                    "Replay stock and extension double-play options differ");
      }
      const auto extensionMode =
          stockLongNoteMode(result.data.setup.longNoteMode);
      if (result.data.setup.chartSha256 != stockChartSha256 || !stockMode ||
          !extensionMode || *extensionMode != *stockMode) {
        return fail(diagnostic,
                    "Replay stock and extension chart identities differ");
      }
    } else {
      result.unsupportedExtension = true;
    }
  }

  std::optional<int> keyMode;
  if (supported != nullptr) {
    keyMode = result.data.setup.keyMode;
  } else if (expectedKeyMode) {
    keyMode = expectedKeyMode;
  } else {
    keyMode = inferredKeyMode(stage);
  }
  if (!keyMode || !isKeyModeSupported(*keyMode)) {
    return fail(
        diagnostic,
        "Stock replay needs a supported chart key mode to decode input");
  }
  if (expectedKeyMode && *expectedKeyMode != *keyMode) {
    return fail(diagnostic, "Replay key mode does not match the chart context");
  }
  result.data.setup.keyMode = *keyMode;
  if (result.data.setup.doublePlayOption == DoublePlayOption::Flip &&
      *keyMode != 10 && *keyMode != 14) {
    return fail(diagnostic,
                "Replay double-play option is unsupported for its key mode");
  }
  const auto stockInput = decodeStockInput(stage, *keyMode, limits, diagnostic);
  if (!stockInput) {
    return false;
  }
  result.data.input = *stockInput;
  if (supported == nullptr) {
    result.data.setup.gaugeProfile =
        gaugeProfileForKeyMode(*keyMode, expectedEnvelope == "course-stage");
    result.data.setup.startingGaugePercent = gaugeInitialValue(
        result.data.setup.initialGaugeType, result.data.setup.gaugeProfile);
  }

  if (supported == nullptr) {
    return true;
  }

  std::string envelope;
  const auto input = supported->find("input");
  const auto touch = supported->find("touchSamples");
  const auto cover = supported->find("laneCoverEvents");
  const auto legacy = supported->find("legacy");
  if (!readRequired(*supported, "envelope", envelope, diagnostic) ||
      envelope != expectedEnvelope ||
      !readRequired(*supported, "stageIndex", result.stageIndex, diagnostic) ||
      !readRequired(*supported, "stageCount", result.stageCount, diagnostic) ||
      !readRequired(*supported, "restMicrosAfterStage",
                    result.restMicrosAfterStage, diagnostic) ||
      input == supported->end() || touch == supported->end() ||
      cover == supported->end() || legacy == supported->end()) {
    if (diagnostic.empty()) {
      diagnostic = "Replay extension envelope or arrays are missing";
    }
    return false;
  }
  if (result.stageIndex < 0 || result.stageCount <= 0 ||
      result.restMicrosAfterStage < 0) {
    return fail(diagnostic, "Replay extension course metadata is invalid");
  }
  if (!decodeInputExtension(*input, *keyMode, limits, result.data.input,
                            diagnostic) ||
      !decodeTouchExtension(*touch, limits, result.data.touchSamples,
                            diagnostic) ||
      !decodeCoverExtension(*cover, limits, result.data.laneCoverEvents,
                            diagnostic) ||
      !decodeLegacyExtension(*legacy, limits, result.data.legacy, diagnostic)) {
    return false;
  }
  return validateStockProjection(stockSetup, result.data, limits, *stockInput,
                                 diagnostic);
}

std::optional<Bytes> encodeDocument(const Json &document,
                                    const ReplayCodecLimits &limits,
                                    std::string &diagnostic) {
  const std::string serialized = document.dump();
  if (serialized.size() > limits.maxJsonBytes) {
    diagnostic = "Replay JSON exceeds the configured limit";
    return std::nullopt;
  }
  const auto raw =
      std::span(reinterpret_cast<const std::byte *>(serialized.data()),
                serialized.size());
  auto compressed = gzipCompress(raw, diagnostic);
  if (!compressed) {
    return std::nullopt;
  }
  if (compressed->size() > limits.maxCompressedBytes) {
    diagnostic = "Compressed replay exceeds the configured limit";
    return std::nullopt;
  }
  return compressed;
}

} // namespace

std::optional<bool>
ReplayDecodeOutcome::replayPathHasUndefinedLongNotes() const noexcept {
  if (chart.has_value()) {
    if (course.has_value() || stageSources.size() != 1) {
      return std::nullopt;
    }
    return stageSources.front() == ReplayStageDecodeSource::AsoExtension
               ? std::optional<bool>(chart->setup.hasUndefinedLongNotes)
               : std::nullopt;
  }
  if (!course.has_value() ||
      stageSources.size() != course->stages.size()) {
    return std::nullopt;
  }

  bool hasStockStage = false;
  for (std::size_t index = 0; index < course->stages.size(); ++index) {
    if (stageSources[index] == ReplayStageDecodeSource::Stock) {
      hasStockStage = true;
    } else if (course->stages[index].setup.hasUndefinedLongNotes) {
      return true;
    }
  }
  return hasStockStage ? std::nullopt : std::optional<bool>(false);
}

BeatorajaReplayCodec::BeatorajaReplayCodec(ReplayCodecLimits limits)
    : limits_(limits) {}

std::optional<std::vector<std::byte>>
BeatorajaReplayCodec::encodeChart(const ReplayPlaybackData &replay,
                                  std::int64_t playedAtUnixMillis,
                                  std::string &diagnostic) const {
  diagnostic.clear();
  const auto stage = encodeStage(replay, playedAtUnixMillis, "chart", 0, 1, 0,
                                 limits_, diagnostic);
  return stage ? encodeDocument(*stage, limits_, diagnostic) : std::nullopt;
}

std::optional<std::vector<std::byte>>
BeatorajaReplayCodec::encodeCourse(const CourseReplayPlaybackData &replay,
                                   std::int64_t playedAtUnixMillis,
                                   std::string &diagnostic) const {
  diagnostic.clear();
  if (replay.stages.empty() ||
      replay.stages.size() != replay.restMicrosAfterStage.size() ||
      std::ranges::any_of(replay.restMicrosAfterStage,
                          [](std::int64_t rest) { return rest < 0; }) ||
      replay.stages.size() > kMaximumCourseReplayStages ||
      replay.stages.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    diagnostic = "Replay course stage/rest envelope is invalid";
    return std::nullopt;
  }
  AggregateReplayCounts aggregateCounts;
  for (const auto &stage : replay.stages) {
    if (!aggregateCounts.include(stage, limits_, diagnostic)) {
      return std::nullopt;
    }
  }
  Json document = Json::array();
  for (std::size_t i = 0; i < replay.stages.size(); ++i) {
    const auto stage =
        encodeStage(replay.stages[i], playedAtUnixMillis, "course-stage",
                    static_cast<int>(i), static_cast<int>(replay.stages.size()),
                    replay.restMicrosAfterStage[i], limits_, diagnostic);
    if (!stage) {
      return std::nullopt;
    }
    document.push_back(*stage);
  }
  return encodeDocument(document, limits_, diagnostic);
}

ReplayDecodeOutcome
BeatorajaReplayCodec::decode(std::span<const std::byte> encoded,
                             std::optional<int> expectedKeyMode) const {
  if (!expectedKeyMode) {
    return decode(encoded, std::span<const int>{});
  }
  return decode(encoded, std::span<const int>(&*expectedKeyMode, 1));
}

ReplayDecodeOutcome
BeatorajaReplayCodec::decode(
    std::span<const std::byte> encoded,
    std::span<const int> expectedStageKeyModes) const {
  ReplayDecodeOutcome outcome;
  if (encoded.size() > limits_.maxCompressedBytes) {
    outcome.diagnostic = "Compressed replay exceeds the configured limit";
    return outcome;
  }
  if (std::ranges::any_of(expectedStageKeyModes,
                          [](int mode) { return !isKeyModeSupported(mode); })) {
    outcome.diagnostic = "Chart context has an unsupported replay key mode";
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
  if (!validateJsonDepth(source, limits_.maxJsonDepth, outcome.diagnostic)) {
    return outcome;
  }

  Json document;
  try {
    document = Json::parse(source);
  } catch (const Json::exception &error) {
    outcome.diagnostic = "Replay JSON is invalid: " + std::string(error.what());
    return outcome;
  }

  const bool courseEnvelope = document.is_array();
  if (!courseEnvelope && !document.is_object()) {
    outcome.diagnostic = "Replay JSON root is neither a chart nor a course";
    return outcome;
  }
  if (courseEnvelope && document.empty()) {
    outcome.diagnostic = "Replay course contains no stages";
    return outcome;
  }

  std::vector<StageDecode> stages;
  const std::size_t stageCount = courseEnvelope ? document.size() : 1;
  if (courseEnvelope && stageCount > kMaximumCourseReplayStages) {
    outcome.diagnostic = "Replay course stage count exceeds the limit";
    return outcome;
  }
  if (expectedStageKeyModes.size() > 1 &&
      expectedStageKeyModes.size() != stageCount) {
    outcome.diagnostic =
        "Replay stage count differs from the chart key-mode context";
    return outcome;
  }
  stages.reserve(stageCount);
  AggregateReplayCounts aggregateCounts;
  for (std::size_t i = 0; i < stageCount; ++i) {
    StageDecode stage;
    const Json &stageJson = courseEnvelope ? document[i] : document;
    const std::optional<int> expectedKeyMode =
        expectedStageKeyModes.empty()
            ? std::nullopt
            : std::optional<int>(expectedStageKeyModes.size() == 1
                                     ? expectedStageKeyModes.front()
                                     : expectedStageKeyModes[i]);
    if (!decodeStage(stageJson, courseEnvelope ? "course-stage" : "chart",
                     expectedKeyMode, limits_, stage, outcome.diagnostic)) {
      return outcome;
    }
    if (!aggregateCounts.include(stage.data, limits_, outcome.diagnostic)) {
      return outcome;
    }
    stages.push_back(std::move(stage));
  }

  outcome.stockOnly = true;
  outcome.stageSources.reserve(stages.size());
  for (std::size_t i = 0; i < stages.size(); ++i) {
    outcome.unsupportedAsoExtension |= stages[i].unsupportedExtension;
    outcome.stockOnly &= !stages[i].supportedExtension;
    outcome.stageSources.push_back(
        stages[i].supportedExtension
            ? ReplayStageDecodeSource::AsoExtension
            : ReplayStageDecodeSource::Stock);
    if (stages[i].supportedExtension &&
        (stages[i].stageIndex != static_cast<int>(i) ||
         stages[i].stageCount != static_cast<int>(stages.size()))) {
      outcome.diagnostic = "Replay extension stage envelope is inconsistent";
      return outcome;
    }
  }

  if (!courseEnvelope) {
    outcome.chart = std::move(stages.front().data);
  } else {
    CourseReplayPlaybackData course;
    course.stages.reserve(stages.size());
    course.restMicrosAfterStage.reserve(stages.size());
    for (auto &stage : stages) {
      course.stages.push_back(std::move(stage.data));
      course.restMicrosAfterStage.push_back(
          stage.supportedExtension ? stage.restMicrosAfterStage : 0);
    }
    outcome.course = std::move(course);
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
  const auto scratch = [&](int clockwise1P,
                           int clockwise2P) -> std::optional<int> {
    if (control.lane != -1 || control.player < 1 ||
        control.player > playerCount(keyMode)) {
      return std::nullopt;
    }
    const int clockwise = control.player == 1 ? clockwise1P : clockwise2P;
    if (clockwise < 0) {
      return std::nullopt;
    }
    return control.kind == LogicalControlKind::ScratchClockwise ? clockwise
           : control.kind == LogicalControlKind::ScratchCounterClockwise
               ? clockwise + 1
               : std::optional<int>{};
  };

  if (control.kind == LogicalControlKind::Lane) {
    if (control.player < 1 || control.player > playerCount(keyMode) ||
        control.lane < 0) {
      return std::nullopt;
    }
    switch (keyMode) {
    case 5:
      return control.player == 1 && control.lane < 5
                 ? std::optional<int>(control.lane)
                 : std::nullopt;
    case 7:
      return control.player == 1 && control.lane < 7
                 ? std::optional<int>(control.lane)
                 : std::nullopt;
    case 9:
      return control.player == 1 && control.lane < 9
                 ? std::optional<int>(control.lane)
                 : std::nullopt;
    case 10:
      return control.lane < 5
                 ? std::optional<int>(control.lane +
                                      (control.player == 2 ? 7 : 0))
                 : std::nullopt;
    case 14:
      return control.lane < 7
                 ? std::optional<int>(control.lane +
                                      (control.player == 2 ? 9 : 0))
                 : std::nullopt;
    case 24:
      return control.player == 1 && control.lane < 26
                 ? std::optional<int>(control.lane)
                 : std::nullopt;
    case 48:
      return control.lane < 26
                 ? std::optional<int>(control.lane +
                                      (control.player == 2 ? 26 : 0))
                 : std::nullopt;
    default:
      return std::nullopt;
    }
  }

  switch (keyMode) {
  case 5:
    return scratch(5, -1);
  case 7:
    return scratch(7, -1);
  case 10:
    return scratch(5, 12);
  case 14:
    return scratch(7, 16);
  default:
    return std::nullopt;
  }
}

std::optional<LogicalControl>
BeatorajaReplayCodec::logicalControl(int keyCode, int keyMode) noexcept {
  if (keyCode < 0) {
    return std::nullopt;
  }
  const auto scratch = [](LogicalControlKind kind, int player) {
    return LogicalControl{.kind = kind, .player = player, .lane = -1};
  };
  switch (keyMode) {
  case 5:
    if (keyCode < 5)
      return LogicalControl{
          .kind = LogicalControlKind::Lane, .player = 1, .lane = keyCode};
    if (keyCode == 5)
      return scratch(LogicalControlKind::ScratchClockwise, 1);
    if (keyCode == 6)
      return scratch(LogicalControlKind::ScratchCounterClockwise, 1);
    return std::nullopt;
  case 7:
    if (keyCode < 7)
      return LogicalControl{
          .kind = LogicalControlKind::Lane, .player = 1, .lane = keyCode};
    if (keyCode == 7)
      return scratch(LogicalControlKind::ScratchClockwise, 1);
    if (keyCode == 8)
      return scratch(LogicalControlKind::ScratchCounterClockwise, 1);
    return std::nullopt;
  case 9:
    return keyCode < 9 ? std::optional<LogicalControl>(
                             LogicalControl{.kind = LogicalControlKind::Lane,
                                            .player = 1,
                                            .lane = keyCode})
                       : std::nullopt;
  case 10:
    if (keyCode < 5)
      return LogicalControl{
          .kind = LogicalControlKind::Lane, .player = 1, .lane = keyCode};
    if (keyCode == 5 || keyCode == 6)
      return scratch(keyCode == 5 ? LogicalControlKind::ScratchClockwise
                                  : LogicalControlKind::ScratchCounterClockwise,
                     1);
    if (keyCode >= 7 && keyCode < 12)
      return LogicalControl{
          .kind = LogicalControlKind::Lane, .player = 2, .lane = keyCode - 7};
    if (keyCode == 12 || keyCode == 13)
      return scratch(keyCode == 12
                         ? LogicalControlKind::ScratchClockwise
                         : LogicalControlKind::ScratchCounterClockwise,
                     2);
    return std::nullopt;
  case 14:
    if (keyCode < 7)
      return LogicalControl{
          .kind = LogicalControlKind::Lane, .player = 1, .lane = keyCode};
    if (keyCode == 7 || keyCode == 8)
      return scratch(keyCode == 7 ? LogicalControlKind::ScratchClockwise
                                  : LogicalControlKind::ScratchCounterClockwise,
                     1);
    if (keyCode >= 9 && keyCode < 16)
      return LogicalControl{
          .kind = LogicalControlKind::Lane, .player = 2, .lane = keyCode - 9};
    if (keyCode == 16 || keyCode == 17)
      return scratch(keyCode == 16
                         ? LogicalControlKind::ScratchClockwise
                         : LogicalControlKind::ScratchCounterClockwise,
                     2);
    return std::nullopt;
  case 24:
    return keyCode < 26 ? std::optional<LogicalControl>(
                              LogicalControl{.kind = LogicalControlKind::Lane,
                                             .player = 1,
                                             .lane = keyCode})
                        : std::nullopt;
  case 48:
    return keyCode < 52 ? std::optional<LogicalControl>(
                              LogicalControl{.kind = LogicalControlKind::Lane,
                                             .player = keyCode < 26 ? 1 : 2,
                                             .lane = keyCode % 26})
                        : std::nullopt;
  default:
    return std::nullopt;
  }
}

} // namespace replay
