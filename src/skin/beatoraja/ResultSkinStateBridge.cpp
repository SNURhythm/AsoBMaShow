#include "ResultSkinStateBridge.h"

#include "../../scene/ResultPresentationModel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace skin {
namespace {

template <typename Value>
SkinPropertyLookup<Value> unsupported() {
  return {};
}

template <typename Value> SkinPropertyLookup<Value> supported(Value value) {
  return {.value = std::move(value), .supported = true};
}

int resultRank(int score, int maximum) {
  if (maximum <= 0) return 0;
  // BooleanPropertyFactory.createNowRank uses these exact lower bounds in
  // ScoreDataProperty's 27-step rank table.
  constexpr std::array<int, 8> lowerBounds{0, 6, 9, 12, 15, 18, 21, 24};
  for (int rank = 7; rank >= 0; --rank) {
    if (static_cast<long long>(score) * 27 >=
        static_cast<long long>(maximum) * lowerBounds[rank]) {
      return rank;
    }
  }
  return 0;
}

} // namespace

ResultSkinStateBridge::ResultSkinStateBridge(ResultSkinData data,
                                             std::uint64_t frameSerial,
                                             std::int64_t elapsedMillis)
    : data_(std::move(data)), frameSerial_(frameSerial),
      elapsedMillis_(std::max<std::int64_t>(0, elapsedMillis)) {
  if (data_.state != nullptr) gaugeHistory_ = data_.state->gaugeHistory;
}

std::uint64_t ResultSkinStateBridge::frameSerial() const noexcept {
  return frameSerial_;
}

std::optional<int> ResultSkinStateBridge::count(Judgement judgement) const noexcept {
  if (data_.state == nullptr) return std::nullopt;
  const auto found = data_.state->judgeCount.find(judgement);
  return found == data_.state->judgeCount.end() ? std::optional<int>(0)
                                                 : found->second;
}

std::optional<int> ResultSkinStateBridge::score() const noexcept {
  if (data_.presentation && data_.presentation->score) {
    return data_.presentation->score;
  }
  return data_.state ? std::optional<int>(data_.state->getScore()) : std::nullopt;
}

std::optional<int> ResultSkinStateBridge::maxScore() const noexcept {
  if (data_.presentation && data_.presentation->maxScore) {
    return data_.presentation->maxScore;
  }
  return data_.meta ? std::optional<int>(data_.meta->TotalNotes * 2)
                    : std::nullopt;
}

std::optional<int> ResultSkinStateBridge::maxCombo() const noexcept {
  if (data_.presentation && data_.presentation->maxCombo) {
    return data_.presentation->maxCombo;
  }
  return data_.state ? std::optional<int>(data_.state->maxCombo) : std::nullopt;
}

std::optional<int> ResultSkinStateBridge::integerSelector(
    const SkinBuiltinPropertySelector &selector) const noexcept {
  const auto *value = std::get_if<int>(&selector.value);
  return value ? std::optional<int>(*value) : std::nullopt;
}

SkinPropertyLookup<bool> ResultSkinStateBridge::booleanProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = integerSelector(selector);
  if (!id) return unsupported<bool>();
  const auto currentScore = score();
  const auto maximum = maxScore();
  if (*id == 90 || *id == 91) {
    const bool clear = data_.state != nullptr &&
                       data_.state->getClearTypeRank() > kClearTypeFailedRank;
    return supported(*id == 90 ? clear : !clear);
  }
  if (*id >= 300 && *id <= 307 && currentScore && maximum) {
    return supported(resultRank(*currentScore, *maximum) == 307 - *id);
  }
  return unsupported<bool>();
}

SkinPropertyLookup<std::int64_t> ResultSkinStateBridge::integerProperty(
    const SkinBuiltinPropertySelector &selector, SkinIntegerPropertyDomain) {
  const auto id = integerSelector(selector);
  if (!id) return unsupported<std::int64_t>();
  const auto currentScore = score();
  const auto maximum = maxScore();
  const auto notes = data_.meta ? std::optional<int>(data_.meta->TotalNotes)
                                : std::nullopt;
  const auto result = [this, id, currentScore, maximum, notes]()
      -> std::optional<int> {
    const auto previousScore = data_.previousBest
                                   ? std::optional<int>(data_.previousBest->score)
                                   : std::nullopt;
    const auto previousCombo = data_.previousBest
                                   ? std::optional<int>(data_.previousBest->maxCombo)
                                   : std::nullopt;
    const auto timing = [this](Judgement judgement, bool early) {
      if (!data_.state) return std::optional<int>{};
      const auto found = data_.state->judgementFastSlowCount.find(judgement);
      if (found == data_.state->judgementFastSlowCount.end()) {
        return std::optional<int>(0);
      }
      return std::optional<int>(early ? found->second.fast : found->second.slow);
    };
    switch (*id) {
    case 71: case 101: case 171: return currentScore;
    case 72: return maximum;
    case 74: case 106: return notes;
    case 75: case 105: case 174: return maxCombo();
    case 150: case 170: return previousScore;
    case 152: case 172:
      return currentScore && previousScore ? std::optional<int>(*currentScore - *previousScore)
                                           : std::nullopt;
    case 173: return previousCombo;
    case 175:
      return maxCombo() && previousCombo ? std::optional<int>(*maxCombo() - *previousCombo)
                                          : std::nullopt;
    case 107: return data_.state ? std::optional<int>(static_cast<int>(std::lround(data_.state->currentGauge))) : std::nullopt;
    case 407: return data_.state ? std::optional<int>(static_cast<int>(std::lround(data_.state->currentGauge * 10.0F)) % 10) : std::nullopt;
    case 110: return count(PGreat);
    case 111: return count(Great);
    case 112: return count(Good);
    case 113: return count(Bad);
    case 114: return count(Kpoor);
    case 410: return timing(PGreat, true);
    case 411: return timing(PGreat, false);
    case 412: return timing(Great, true);
    case 413: return timing(Great, false);
    case 414: return timing(Good, true);
    case 415: return timing(Good, false);
    case 416: return timing(Bad, true);
    case 417: return timing(Bad, false);
    case 418: return timing(Kpoor, true);
    case 419: return timing(Kpoor, false);
    case 423: return data_.state ? std::optional<int>(data_.state->fastCount) : std::nullopt;
    case 424: return data_.state ? std::optional<int>(data_.state->slowCount) : std::nullopt;
    case 90: return data_.meta ? std::optional<int>(static_cast<int>(data_.meta->MaxBpm)) : std::nullopt;
    case 91: return data_.meta ? std::optional<int>(static_cast<int>(data_.meta->MinBpm)) : std::nullopt;
    case 92: return data_.meta ? std::optional<int>(static_cast<int>(data_.meta->Bpm)) : std::nullopt;
    default: return std::nullopt;
    }
  }();
  return result ? supported<std::int64_t>(*result) : unsupported<std::int64_t>();
}

SkinPropertyLookup<double> ResultSkinStateBridge::floatProperty(
    const SkinBuiltinPropertySelector &, SkinFloatPropertyDomain) {
  return unsupported<double>();
}

SkinPropertyLookup<std::string_view> ResultSkinStateBridge::stringProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = integerSelector(selector);
  if (!id) return unsupported<std::string_view>();
  switch (*id) {
  case 10: case 12:
    stringValue_ = data_.presentation ? data_.presentation->title
                                      : (data_.meta ? data_.meta->Title : "");
    break;
  case 14:
    stringValue_ = data_.presentation && data_.presentation->artist
                       ? *data_.presentation->artist
                       : (data_.meta ? data_.meta->Artist : "");
    break;
  case 62:
    stringValue_ = data_.difficultyLabel;
    break;
  default:
    return unsupported<std::string_view>();
  }
  return supported<std::string_view>(stringValue_);
}

SkinPropertyLookup<SkinRuntimeOffset>
ResultSkinStateBridge::offsetProperty(int) { return unsupported<SkinRuntimeOffset>(); }

std::int64_t ResultSkinStateBridge::timerProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = integerSelector(selector);
  if (!id) return -1;
  return *id == 0 || *id == 1 || *id == 150 || *id == 152 ? elapsedMillis_ : -1;
}

std::span<const SkinProjectedNoteView>
ResultSkinStateBridge::projectedNotes() const noexcept { return {}; }
std::span<const SkinProjectedLongNoteView>
ResultSkinStateBridge::projectedLongNotes() const noexcept { return {}; }
std::span<const SkinProjectedLineView>
ResultSkinStateBridge::projectedLines() const noexcept { return {}; }

SkinGameplayGraphStateView
ResultSkinStateBridge::gameplayGraphState() const noexcept {
  if (!data_.state) return {};
  return {.gaugeHistory = gaugeHistory_,
          .gaugeType = gaugeTypeIndex(data_.state->gaugeType),
          .gaugeMinimum = 0.0F,
          .gaugeMaximum = 100.0F,
          .gaugeBorder = 80.0F,
          .gaugeSupported = true,
          .gaugeRevision = frameSerial_};
}

SkinGaugeStateView ResultSkinStateBridge::gaugeState() const noexcept {
  if (!data_.state) return {};
  return {.supported = true, .value = data_.state->currentGauge,
          .gaugeType = gaugeTypeIndex(data_.state->gaugeType), .minimum = 0.0,
          .maximum = 100.0, .border = 80.0};
}

SkinJudgeStateView ResultSkinStateBridge::judgeState(int player) const noexcept {
  if (player != 0 || !data_.state) return {};
  return {.supported = true, .combo = data_.state->maxCombo,
          .maximumGauge = data_.state->currentGauge >= 100.0F};
}

SkinNoteExpansionStateView ResultSkinStateBridge::noteExpansionState() const noexcept {
  return {};
}

} // namespace skin
