#include "ResultSkinStateBridge.h"

#include "../../scene/ResultPresentationModel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>

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

std::optional<int> resultNextRank(int score, int maximum) {
  if (maximum <= 0) return std::nullopt;
  for (int rank = 0; rank < 27; rank += 3) {
    if (static_cast<long long>(score) * 27 <
        static_cast<long long>(maximum) * rank) {
      return static_cast<int>(std::ceil(
          static_cast<double>(maximum) * rank / 27.0 - score));
    }
  }
  return maximum - score;
}

int localCalendarField(int id) {
  const std::time_t now = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  std::tm local{};
  localtime_r(&now, &local);
  switch (id) {
  case 21: return local.tm_year + 1900;
  case 22: return local.tm_mon + 1;
  case 23: return local.tm_mday;
  case 24: return local.tm_hour;
  case 25: return local.tm_min;
  case 26: return local.tm_sec;
  default: return std::numeric_limits<int>::min();
  }
}

Judgement beatorajaJudgement(int index) {
  switch (index) {
  case 0: return PGreat;
  case 1: return Great;
  case 2: return Good;
  case 3: return Bad;
  default: return Poor;
  }
}

} // namespace

ResultSkinStateBridge::ResultSkinStateBridge(ResultSkinData data,
                                             std::uint64_t frameSerial,
                                             std::int64_t elapsedMillis)
    : data_(std::move(data)), frameSerial_(frameSerial),
      elapsedMillis_(std::max<std::int64_t>(0, elapsedMillis)) {
  if (data_.state != nullptr) {
    gaugeHistory_ = data_.state->gaugeHistory;
  } else if (data_.presentation != nullptr &&
             !data_.presentation->gaugeSeries.empty()) {
    const auto &series = data_.presentation->gaugeSeries.front();
    gaugeHistory_.reserve(series.points.size());
    for (const auto point : series.points) {
      gaugeHistory_.push_back(point.value_or(0.0F));
    }
  }
  // Result snapshots are immutable for the lifetime of one bridge/session.
  // Keep graph texture caching stable while still distinguishing snapshots.
  std::uint64_t revision = 1469598103934665603ULL;
  for (const float point : gaugeHistory_) {
    revision ^= std::bit_cast<std::uint32_t>(point);
    revision *= 1099511628211ULL;
  }
  gaugeRevision_ = revision == 0 ? 1 : revision;
}

std::uint64_t ResultSkinStateBridge::frameSerial() const noexcept {
  return frameSerial_;
}

std::optional<int> ResultSkinStateBridge::count(Judgement judgement) const noexcept {
  if (data_.state != nullptr) {
    const auto found = data_.state->judgeCount.find(judgement);
    return found == data_.state->judgeCount.end() ? std::optional<int>(0)
                                                   : found->second;
  }
  if (data_.presentation == nullptr) return std::nullopt;
  const char *label = judgement == PGreat ? "P-GREAT" : judgement == Great ? "GREAT"
                       : judgement == Good ? "GOOD" : judgement == Bad ? "BAD"
                       : judgement == Poor ? "POOR" : nullptr;
  if (label == nullptr) return std::nullopt;
  for (const auto &row : data_.presentation->judgements) {
    if (row.label == label) return row.total;
  }
  return std::nullopt;
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

std::optional<float> ResultSkinStateBridge::finalGauge() const noexcept {
  if (data_.state != nullptr) return data_.state->currentGauge;
  return data_.presentation ? data_.presentation->finalGauge : std::nullopt;
}

std::optional<int>
ResultSkinStateBridge::timing(Judgement judgement, bool early) const noexcept {
  if (data_.state != nullptr) {
    const auto found = data_.state->judgementFastSlowCount.find(judgement);
    return found == data_.state->judgementFastSlowCount.end()
               ? std::optional<int>(0)
               : std::optional<int>(early ? found->second.fast
                                          : found->second.slow);
  }
  if (data_.presentation == nullptr) return std::nullopt;
  const char *label = judgement == PGreat ? "P-GREAT" : judgement == Great ? "GREAT"
                       : judgement == Good ? "GOOD" : judgement == Bad ? "BAD"
                       : judgement == Poor ? "POOR" : nullptr;
  if (label == nullptr) return std::nullopt;
  for (const auto &row : data_.presentation->judgements) {
    if (row.label == label) return early ? row.early : row.late;
  }
  return std::nullopt;
}

std::optional<int> ResultSkinStateBridge::integerSelector(
    const SkinBuiltinPropertySelector &selector) const noexcept {
  const auto *value = std::get_if<int>(&selector.value);
  if (value != nullptr) return *value;
  const auto *name = std::get_if<std::string>(&selector.value);
  if (name == nullptr) return std::nullopt;
  static constexpr std::array<std::pair<std::string_view, int>, 25> aliases{{
      {"title", 10}, {"fulltitle", 12}, {"subtitle", 11},
      {"artist", 14}, {"subartist", 15}, {"mode", 60},
      {"sort", 61}, {"difficulty", 62}, {"nowbpm", 92},
      {"score_rate", 1102}, {"total_rate", 1115},
      {"score_rate2", 155}, {"scorerate", 110},
      {"scorerate_final", 111}, {"bestscorerate_now", 112},
      {"bestscorerate", 113}, {"targetscorerate_now", 114},
      {"targetscorerate", 115}, {"rate_pgreat", 140},
      {"rate_great", 141}, {"rate_good", 142}, {"rate_bad", 143},
      {"rate_poor", 144}, {"rate_maxcombo", 145}, {"rate_exscore", 147},
  }};
  for (const auto &[alias, id] : aliases) {
    if (*name == alias) return id;
  }
  return std::nullopt;
}

SkinPropertyLookup<bool> ResultSkinStateBridge::booleanProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = integerSelector(selector);
  if (!id) return unsupported<bool>();
  if (*id < 0) {
    if (*id == std::numeric_limits<int>::min()) return unsupported<bool>();
    const auto positive = booleanProperty({-*id});
    return positive.supported ? supported(!positive.value) : unsupported<bool>();
  }
  const auto currentScore = score();
  const auto maximum = maxScore();
  const bool irOnline = data_.irOnline;
  if (*id == 50 || *id == 51) {
    return supported(*id == 51 ? irOnline : !irOnline);
  }
  if (*id == 1 || *id == 2 || *id == 3 || *id == 5 || *id == 21 ||
      *id == 22 || *id == 23 || *id == 33 || *id == 80 || *id == 1030 ||
      *id == 1031 || *id == 290 || *id == 291 || *id == 292 || *id == 293) {
    return supported(false);
  }
  if (*id == 32 || *id == 81) return supported(true);
  if (*id == 1008) return supported(!data_.tableName.empty());
  if (*id >= 150 && *id <= 155) {
    const int difficulty = data_.meta != nullptr ? data_.meta->Difficulty : 0;
    return supported(*id == 150 ? difficulty <= 0 || difficulty > 5
                                : difficulty == *id - 150);
  }
  if (*id >= 160 && *id <= 164) {
    const int keyMode = data_.meta != nullptr ? data_.meta->KeyMode : 0;
    return supported((*id == 160 && keyMode == 7) ||
                     (*id == 161 && keyMode == 5) ||
                     (*id == 162 && keyMode == 14) ||
                     (*id == 163 && keyMode == 10) ||
                     (*id == 164 && keyMode == 9));
  }
  if (*id == 1160 || *id == 1161) {
    const int keyMode = data_.meta != nullptr ? data_.meta->KeyMode : 0;
    return supported(keyMode == (*id == 1160 ? 24 : 48));
  }
  if (*id == 170 || *id == 171) {
    // ResultSkinData retains only ChartMeta.  Its BGA presence is not part of
    // that immutable result payload, matching Beatoraja's false result when
    // BMSResource has no loaded BGA.
    return supported(*id == 170);
  }
  if (*id == 172 || *id == 173) {
    const bool hasLongNote = data_.meta != nullptr &&
                             data_.meta->TotalLongNotes > 0;
    return supported(*id == 173 ? hasLongNote : !hasLongNote);
  }
  if (*id == 174 || *id == 175) {
    // ChartMeta does not retain BMS document-event presence; use the same
    // no-document result as a resource without an authored text document.
    return supported(*id == 174);
  }
  if (*id == 176 || *id == 177) {
    const bool changes = data_.meta != nullptr &&
                         data_.meta->MinBpm < data_.meta->MaxBpm;
    return supported(*id == 177 ? changes : !changes);
  }
  if (*id == 178 || *id == 179) {
    const bool random = data_.meta != nullptr &&
                        !data_.meta->RandomValues.empty();
    return supported(*id == 179 ? random : !random);
  }
  if (*id >= 220 && *id <= 227 && currentScore && maximum) {
    return supported(resultRank(*currentScore, *maximum) == 227 - *id);
  }
  if (*id == 190 || *id == 191) {
    const bool stageFile = data_.meta != nullptr && !data_.meta->StageFile.empty();
    return supported(*id == 191 ? stageFile : !stageFile);
  }
  if (*id == 192 || *id == 193) {
    const bool banner = data_.meta != nullptr && !data_.meta->Banner.empty();
    return supported(*id == 193 ? banner : !banner);
  }
  if (*id == 194 || *id == 195) {
    const bool backBmp = data_.meta != nullptr && !data_.meta->BackBmp.empty();
    return supported(*id == 195 ? backBmp : !backBmp);
  }
  if (*id == 90 || *id == 91) {
    const auto lamp = data_.state != nullptr
                          ? std::optional<int>(data_.state->getClearTypeRank())
                          : (data_.presentation ? data_.presentation->lampRank
                                                : std::nullopt);
    const bool clear = lamp && *lamp > kClearTypeFailedRank;
    return supported(*id == 90 ? clear : !clear);
  }
  if (*id >= 300 && *id <= 307 && currentScore && maximum) {
    return supported(resultRank(*currentScore, *maximum) == 307 - *id);
  }
  return unsupported<bool>();
}

SkinPropertyLookup<std::int64_t> ResultSkinStateBridge::integerProperty(
    const SkinBuiltinPropertySelector &selector, SkinIntegerPropertyDomain domain) {
  const auto id = integerSelector(selector);
  if (!id) return unsupported<std::int64_t>();
  if (domain == SkinIntegerPropertyDomain::ImageIndex) {
    switch (*id) {
    case 89:
    case 90:
      // Result snapshots do not retain persistent song-review flags. Match
      // Beatoraja's no-flag image index rather than leaking ValueType's BPM
      // selector into this separate factory domain.
      return supported<std::int64_t>(0);
    case 370: {
      const auto lamp = data_.currentClearRankOverride
                            ? data_.currentClearRankOverride
                            : (data_.presentation ? data_.presentation->lampRank
                                                  : (data_.state ? std::optional<int>(
                                                        data_.state->getClearTypeRank())
                                                                 : std::nullopt));
      return lamp ? supported<std::int64_t>(*lamp)
                  : unsupported<std::int64_t>();
    }
    case 371: {
      const auto lamp = data_.previousLampBest
                            ? std::optional<int>(data_.previousLampBest->clearType)
                            : (data_.previousBest
                                   ? std::optional<int>(data_.previousBest->clearType)
                                   : std::optional<int>(kClearTypeFailedRank));
      return supported<std::int64_t>(*lamp);
    }
    default:
      return unsupported<std::int64_t>();
    }
  }
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
    const auto targetScore = data_.pacemaker
                                 ? std::optional<int>(data_.pacemaker->targetScore)
                                 : std::nullopt;
    const auto badPoints = [this]() -> std::optional<int> {
      if (!data_.state) return std::nullopt;
      return count(Bad).value_or(0) + count(Poor).value_or(0) +
             count(Kpoor).value_or(0);
    };
    const auto scoreRate = [currentScore, maximum]() -> std::optional<double> {
      return currentScore && maximum && *maximum > 0
                 ? std::optional<double>(static_cast<double>(*currentScore) /
                                         *maximum)
                 : std::nullopt;
    };
    const auto targetRate = [targetScore, maximum]() -> std::optional<double> {
      return targetScore && maximum && *maximum > 0
                 ? std::optional<double>(static_cast<double>(*targetScore) /
                                         *maximum)
                 : std::nullopt;
    };
    const auto previousRate = [previousScore, maximum]() -> std::optional<double> {
      return previousScore && maximum && *maximum > 0
                 ? std::optional<double>(static_cast<double>(*previousScore) /
                                         *maximum)
                 : std::nullopt;
    };
    switch (*id) {
    case 21: case 22: case 23: case 24: case 25: case 26:
      return localCalendarField(*id);
    case 12: case 17: case 18: case 19: case 30: case 31: case 32:
    case 33: case 34: case 35: case 36: case 37: case 79: case 333:
      return std::numeric_limits<int>::min();
    case 100: return currentScore;
    case 71: case 101: case 171: return currentScore;
    case 72: return maximum;
    case 74: case 106: return notes;
    case 75: case 105: case 174: return maxCombo();
    case 76: case 177: return badPoints();
    case 45: case 46: case 47: case 48: case 49: case 96:
      return data_.meta
                 ? std::optional<int>(static_cast<int>(
                       std::lround(data_.meta->PlayLevel)))
                 : std::nullopt;
    case 115:
      if (const auto rate = scoreRate()) {
        return static_cast<int>(*rate * 100);
      }
      return std::nullopt;
    case 102: case 155:
      if (const auto rate = scoreRate()) return static_cast<int>(*rate * 100);
      return std::nullopt;
    case 103: case 156:
      if (const auto rate = scoreRate()) return static_cast<int>(*rate * 10'000) % 100;
      return std::nullopt;
    case 104:
      return data_.state ? std::optional<int>(data_.state->combo) : std::nullopt;
    case 116:
      if (const auto rate = scoreRate()) {
        return static_cast<int>(*rate * 10'000) % 100;
      }
      return std::nullopt;
    case 121: case 151: return targetScore.value_or(std::numeric_limits<int>::min());
    case 122: case 135: case 157:
      if (const auto rate = targetRate()) return static_cast<int>(*rate * 100);
      return std::numeric_limits<int>::min();
    case 123: case 136: case 158:
      if (const auto rate = targetRate()) return static_cast<int>(*rate * 10'000) % 100;
      return std::numeric_limits<int>::min();
    case 128:
      return currentScore && targetScore
                 ? std::optional<int>(*currentScore - *targetScore)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 150: case 170: return previousScore.value_or(std::numeric_limits<int>::min());
    case 152: case 172:
      return currentScore ? std::optional<int>(*currentScore -
                                                previousScore.value_or(0))
                          : std::nullopt;
    case 153:
      return currentScore && targetScore
                 ? std::optional<int>(*currentScore - *targetScore)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 173:
      return previousCombo && *previousCombo > 0
                 ? previousCombo
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 175:
      return maxCombo() && previousCombo && *previousCombo > 0
                 ? std::optional<int>(*maxCombo() - *previousCombo)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 176:
    case 178:
      return std::numeric_limits<int>::min();
    case 154:
      return currentScore && maximum ? resultNextRank(*currentScore, *maximum)
                                      : std::nullopt;
    case 107:
      return finalGauge() ? std::optional<int>(static_cast<int>(*finalGauge()))
                          : std::nullopt;
    case 407:
      if (!finalGauge()) return std::nullopt;
      if (*finalGauge() > 0.0F && *finalGauge() < 0.1F) {
        return 1;
      }
      return static_cast<int>(*finalGauge() * 10.0F) % 10;
    case 110: return count(PGreat);
    case 111: return count(Great);
    case 112: return count(Good);
    case 113: return count(Bad);
    case 114: return count(Poor);
    case 410: return this->timing(PGreat, true);
    case 411: return this->timing(PGreat, false);
    case 412: return this->timing(Great, true);
    case 413: return this->timing(Great, false);
    case 414: return this->timing(Good, true);
    case 415: return this->timing(Good, false);
    case 416: return this->timing(Bad, true);
    case 417: return this->timing(Bad, false);
    case 418: return this->timing(Poor, true);
    case 419: return this->timing(Poor, false);
    case 420: return count(Kpoor);
    case 421: return this->timing(Kpoor, true);
    case 422: return this->timing(Kpoor, false);
    case 423:
      if (data_.presentation && !data_.state) return data_.presentation->fast;
      return data_.state ? std::optional<int>(this->timing(Great, true).value_or(0) +
                                               this->timing(Good, true).value_or(0) +
                                               this->timing(Bad, true).value_or(0) +
                                               this->timing(Poor, true).value_or(0) +
                                               this->timing(Kpoor, true).value_or(0))
                         : std::nullopt;
    case 424:
      if (data_.presentation && !data_.state) return data_.presentation->slow;
      return data_.state ? std::optional<int>(this->timing(Great, false).value_or(0) +
                                               this->timing(Good, false).value_or(0) +
                                               this->timing(Bad, false).value_or(0) +
                                               this->timing(Poor, false).value_or(0) +
                                               this->timing(Kpoor, false).value_or(0))
                         : std::nullopt;
    case 426:
      return data_.state ? std::optional<int>(count(Poor).value_or(0) +
                                               count(Kpoor).value_or(0))
                         : std::nullopt;
    case 427:
      return data_.state ? std::optional<int>(count(Bad).value_or(0) +
                                               count(Poor).value_or(0) +
                                               count(Kpoor).value_or(0))
                         : std::nullopt;
    case 425:
      return data_.state ? std::optional<int>(data_.state->comboBreak)
                         : (data_.presentation ? data_.presentation->comboBreak
                                               : std::nullopt);
    case 179: case 180: return std::numeric_limits<int>::min();
    case 183:
      if (const auto rate = previousRate()) return static_cast<int>(*rate * 100);
      return std::numeric_limits<int>::min();
    case 184:
      if (const auto rate = previousRate()) return static_cast<int>(*rate * 10'000) % 100;
      return std::numeric_limits<int>::min();
    case 380: case 381: case 382: case 383: case 384:
    case 385: case 386: case 387: case 388: case 389:
    case 390: case 391: case 392: case 393: case 394:
    case 395: case 396: case 397: case 398: case 399:
      return std::numeric_limits<int>::min();
    case 368: return 0;
    case 370:
      return data_.state
                 ? std::optional<int>(data_.state->getClearTypeRank())
                 : std::nullopt;
    case 371:
      return data_.previousBest
                 ? std::optional<int>(data_.previousBest->clearType)
                 : std::optional<int>(kClearTypeFailedRank);
    case 372: case 373: case 374: case 375:
      return std::numeric_limits<int>::min();
    case 402: case 403: case 404:
      return 0;
    case 90: return data_.meta ? std::optional<int>(static_cast<int>(data_.meta->MaxBpm)) : std::nullopt;
    case 91: return data_.meta ? std::optional<int>(static_cast<int>(data_.meta->MinBpm)) : std::nullopt;
    case 92: return data_.meta ? std::optional<int>(static_cast<int>(data_.meta->Bpm)) : std::nullopt;
    default: return std::nullopt;
    }
  }();
  return result ? supported<std::int64_t>(*result) : unsupported<std::int64_t>();
}

SkinPropertyLookup<double> ResultSkinStateBridge::floatProperty(
    const SkinBuiltinPropertySelector &selector, SkinFloatPropertyDomain) {
  const auto id = integerSelector(selector);
  if (!id) return unsupported<double>();
  const auto currentScore = score();
  const auto maximum = maxScore();
  const auto rate = [currentScore, maximum]() -> std::optional<double> {
    return currentScore && maximum && *maximum > 0
               ? std::optional<double>(static_cast<double>(*currentScore) /
                                       *maximum)
               : std::nullopt;
  };
  const auto scoreRate = [maximum](std::optional<int> score)
      -> std::optional<double> {
    return score && maximum && *maximum > 0
               ? std::optional<double>(static_cast<float>(*score) /
                                       static_cast<float>(*maximum))
               : std::nullopt;
  };
  if (*id == 1102 || *id == 1115 || *id == 155) {
    const auto value = rate();
    return value ? supported(*value) : unsupported<double>();
  }
  if (*id == 110 || *id == 111) {
    const auto value = rate();
    return value ? supported(*value) : unsupported<double>();
  }
  if (*id == 112 || *id == 113) {
    const auto value = scoreRate(data_.previousBest
                                     ? std::optional<int>(data_.previousBest->score)
                                     : std::nullopt);
    return value ? supported(*value) : unsupported<double>();
  }
  if (*id == 114 || *id == 115) {
    const auto value = scoreRate(data_.pacemaker
                                     ? std::optional<int>(data_.pacemaker->targetScore)
                                     : std::nullopt);
    return value ? supported(*value) : unsupported<double>();
  }
  if ((*id >= 85 && *id <= 89) || (*id >= 140 && *id <= 144)) {
    const auto notes = data_.meta ? std::optional<int>(data_.meta->TotalNotes)
                                  : (maximum ? std::optional<int>(*maximum / 2)
                                             : std::nullopt);
    if (!notes || *notes <= 0) return unsupported<double>();
    const Judgement judgement = *id >= 140
                                    ? beatorajaJudgement(*id - 140)
                                    : beatorajaJudgement(*id - 85);
    return supported(static_cast<double>(count(judgement).value_or(0)) / *notes);
  }
  if (*id == 145) {
    const auto notes = data_.meta ? std::optional<int>(data_.meta->TotalNotes)
                                  : (maximum ? std::optional<int>(*maximum / 2)
                                             : std::nullopt);
    const auto combo = maxCombo();
    return notes && *notes > 0 && combo
               ? supported(static_cast<double>(*combo) / *notes)
               : unsupported<double>();
  }
  if (*id == 147) {
    const auto value = rate();
    return value ? supported(*value) : unsupported<double>();
  }
  if (*id == 1107) {
    const auto value = finalGauge();
    return value ? supported(static_cast<double>(*value)) : unsupported<double>();
  }
  if (*id == 183 && data_.previousBest && maximum && *maximum > 0) {
    return supported(static_cast<double>(data_.previousBest->score) / *maximum);
  }
  if ((*id == 122 || *id == 135 || *id == 157) && data_.pacemaker &&
      maximum && *maximum > 0) {
    return supported(static_cast<double>(data_.pacemaker->targetScore) /
                     *maximum);
  }
  return unsupported<double>();
}

SkinPropertyLookup<std::string_view> ResultSkinStateBridge::stringProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = integerSelector(selector);
  if (!id) return unsupported<std::string_view>();
  switch (*id) {
  case 1:
    stringValue_.clear();
    break;
  case 3:
    stringValue_ = data_.pacemaker ? data_.pacemaker->label : "";
    break;
  case 2:
    stringValue_ = data_.playerName;
    break;
  case 10:
    stringValue_ = data_.presentation ? data_.presentation->title
                                      : (data_.meta ? data_.meta->Title : "");
    break;
  case 12:
    stringValue_ = data_.presentation ? data_.presentation->title
                                      : (data_.meta ? data_.meta->Title : "");
    if (data_.meta != nullptr && !data_.meta->SubTitle.empty()) {
      stringValue_ += " " + data_.meta->SubTitle;
    }
    break;
  case 11:
    stringValue_ = data_.meta ? data_.meta->SubTitle : "";
    break;
  case 13:
    stringValue_ = data_.meta ? data_.meta->Genre : "";
    break;
  case 14:
    stringValue_ = data_.presentation && data_.presentation->artist
                       ? *data_.presentation->artist
                       : (data_.meta ? data_.meta->Artist : "");
    break;
  case 15:
    stringValue_ = data_.meta ? data_.meta->SubArtist : "";
    break;
  case 16:
    stringValue_ = data_.meta
                       ? (data_.meta->SubArtist.empty()
                              ? data_.meta->Artist
                              : data_.meta->Artist + " " + data_.meta->SubArtist)
                       : "";
    break;
  case 1010:
    stringValue_ = ASOBMASHOW_APPLICATION_VERSION;
    break;
  case 1001:
    stringValue_ = data_.tableName;
    break;
  case 1002:
    stringValue_ = data_.tableLevel;
    break;
  case 1003:
    stringValue_ = data_.tableName.empty() ? "" : data_.tableLevel.empty()
                                                ? data_.tableName
                                                : data_.tableName + " " + data_.tableLevel;
    break;
  case 1020:
  case 1021:
  case 119:
  case 149:
  case 219:
  case 220:
    stringValue_.clear();
    break;
  case 1030:
    stringValue_ = data_.meta ? data_.meta->MD5 : "";
    break;
  case 1031:
    stringValue_ = data_.meta ? data_.meta->SHA256 : "";
    break;
  case 62:
    stringValue_ = data_.difficultyLabel;
    break;
  case 60:
    stringValue_ = data_.playModeLabel;
    break;
  case 61:
    stringValue_ = data_.laneOrderLabel;
    break;
  default:
    if ((*id >= 120 && *id <= 129) || (*id >= 150 && *id <= 159) ||
        (*id >= 200 && *id <= 219) || (*id >= 1040 && *id <= 1075)) {
      stringValue_.clear();
      break;
    }
    return unsupported<std::string_view>();
  }
  return supported<std::string_view>(stringValue_);
}

SkinPropertyLookup<SkinRuntimeOffset>
ResultSkinStateBridge::offsetProperty(int) { return unsupported<SkinRuntimeOffset>(); }

std::int64_t ResultSkinStateBridge::timerProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = integerSelector(selector);
  constexpr auto kTimerOff = std::numeric_limits<std::int64_t>::min();
  if (!id) return kTimerOff;
  return *id == 0 || *id == 1 || *id == 150 || *id == 152
             ? elapsedMillis_ * 1'000
             : kTimerOff;
}

std::span<const SkinProjectedNoteView>
ResultSkinStateBridge::projectedNotes() const noexcept { return {}; }
std::span<const SkinProjectedLongNoteView>
ResultSkinStateBridge::projectedLongNotes() const noexcept { return {}; }
std::span<const SkinProjectedLineView>
ResultSkinStateBridge::projectedLines() const noexcept { return {}; }

SkinGameplayGraphStateView
ResultSkinStateBridge::gameplayGraphState() const noexcept {
  if (gaugeHistory_.empty()) return {};
  const GaugeType type = data_.state ? data_.state->gaugeType : GaugeType::Normal;
  const GaugeProfile profile = data_.state ? data_.state->gaugeProfile
                                           : GaugeProfile::Standard;
  return {.gaugeHistory = gaugeHistory_,
          .gaugeType = gaugeTypeIndex(type),
          .gaugeMinimum = gaugeMinimumValue(type, profile),
          .gaugeMaximum = gaugeMaximumValue(type, profile),
          .gaugeBorder = gaugeBorderValue(type, profile),
          .gaugeSupported = true,
          .gaugeRevision = gaugeRevision_};
}

SkinGaugeStateView ResultSkinStateBridge::gaugeState() const noexcept {
  if (!data_.state) return {};
  return {.supported = true, .value = data_.state->currentGauge,
          .gaugeType = gaugeTypeIndex(data_.state->gaugeType), .minimum = 0.0,
          .maximum = 100.0, .border = 80.0};
}

SkinJudgeStateView ResultSkinStateBridge::judgeState(int player) const noexcept {
  if (player != 0) return {};
  const auto combo = maxCombo();
  const auto gauge = finalGauge();
  if (!combo || !gauge) return {};
  return {.supported = true, .combo = *combo, .maximumGauge = *gauge >= 100.0F};
}

SkinNoteExpansionStateView ResultSkinStateBridge::noteExpansionState() const noexcept {
  return {};
}

} // namespace skin
