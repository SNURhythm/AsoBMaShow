#include "ResultSkinStateBridge.h"

#include "BeatorajaBooleanPropertyNames.h"
#include "BeatorajaIntegerPropertyNames.h"
#include "BeatorajaStringPropertyNames.h"
#include "BeatorajaTargetPropertyNames.h"
#include "GameplaySkinBuiltinCatalog.h"

#include "../../LongNoteModeUtils.h"
#include "../../scene/play/PlayfieldChartVisualModel.h"
#include "../../scene/ResultPresentationModel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <memory>

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

std::pair<int, int> scoreRateParts(double rate) {
  // ScoreDataProperty preserves Java float intermediates before truncating.
  const auto javaRate = static_cast<float>(rate);
  return {static_cast<int>(static_cast<float>(javaRate * 100.0F)),
          static_cast<int>(static_cast<float>(javaRate * 10'000.0F)) % 100};
}

std::optional<GaugeType> resultGaugeType(const ResultSkinData &data) {
  return data.state != nullptr ? std::optional<GaugeType>(data.state->gaugeType)
                               : data.gaugeTypeOverride;
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

int localCalendarField(int id, std::time_t time) {
  std::tm local{};
  localtime_r(&time, &local);
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

int currentLocalCalendarField(int id) {
  return localCalendarField(
      id, std::chrono::system_clock::to_time_t(
              std::chrono::system_clock::now()));
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

bool hasBeatorajaLaneAssignment(int option) {
  // Random.OPTION_GENERAL's lane-changing result choices.
  return option == 2 || option == 3 || option == 8;
}

int beatorajaClearTypeImageIndex(int rank) noexcept {
  // Result image properties expose ClearType.id, not AsoBMaShow's durable
  // clear-rank values. ClearType.getClearTypeByID falls back to NoPlay (0)
  // for an unrecognized value, so preserve that upstream fallback here.
  switch (rank) {
  case kClearTypeFailedRank: return 1;
  case kClearTypeAssistedEasyClearRank: return 2;
  case kClearTypeLightAssistedEasyClearRank: return 3;
  case kClearTypeEasyClearRank: return 4;
  case kClearTypeNormalClearRank: return 5;
  case kClearTypeHardClearRank: return 6;
  case kClearTypeExHardClearRank: return 7;
  case kClearTypeFullComboRank: return 8;
  default: return 0;
  }
}

int javaDoubleToInt(double value) noexcept {
  if (std::isnan(value)) return 0;
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(value);
}

SkinPropertyLookup<std::int64_t> resultLaneAssignment(
    const ResultSkinData &data, int player, int key) {
  const auto &option = player == 0 ? data.replayRandomOption1P
                                   : data.replayRandomOption2P;
  const auto &pattern = player == 0 ? data.replayLaneShufflePattern1P
                                    : data.replayLaneShufflePattern2P;
  const int keyCount = data.replayKeyMode == 10 ? 5
                       : data.replayKeyMode == 14 ? 7
                                                  : data.replayKeyMode;
  if (keyCount <= 0 ||
      (player == 1 && data.replayKeyMode != 10 && data.replayKeyMode != 14)) {
    return supported<std::int64_t>(0);
  }
  if (!option || !hasBeatorajaLaneAssignment(*option) || !pattern) {
    return supported<std::int64_t>(0);
  }
  int index = key;
  if (key == -1) {
    // Random.RANDOM_EX alone changes the source scratch lane.
    if (*option != 8) return supported<std::int64_t>(0);
    index = keyCount;
  } else if (key < 0 || key >= keyCount) {
    return supported<std::int64_t>(0);
  }
  if (static_cast<std::size_t>(index) >= pattern->size()) {
    return supported<std::int64_t>(0);
  }
  const int sideOffset = player == 1 ? keyCount : 0;
  return supported<std::int64_t>((*pattern)[static_cast<std::size_t>(index)] +
                                 1 - sideOffset);
}

std::optional<int>
resultFloatSelector(const SkinBuiltinPropertySelector &selector) {
  if (const auto *value = std::get_if<int>(&selector.value)) {
    return *value;
  }
  const auto *name = std::get_if<std::string>(&selector.value);
  if (name == nullptr) {
    return std::nullopt;
  }
  // This mirrors FloatPropertyFactory's name table. Lua's convenience
  // helpers use `rate` and the three volume names directly, while authored
  // skins can use every factory name below.
  static constexpr std::array<std::pair<std::string_view, int>, 75> aliases{{
      {"rate", 110},
      {"volume_sys", 17}, {"volume_key", 18}, {"volume_bg", 19},
      {"musicselect_position", 1}, {"lanecover", 4},
      {"lanecover2", 5}, {"music_progress", 6},
      {"skinselect_position", 7}, {"ranking_position", 8},
      {"mastervolume", 17}, {"keyvolume", 18}, {"bgmvolume", 19},
      {"practice_position", 20}, {"music_progress_bar", 101},
      {"load_progress", 102}, {"level", 103},
      {"level_beginner", 105}, {"level_normal", 106},
      {"level_hyper", 107}, {"level_another", 108}, {"level_insane", 109},
      {"score_rate", 1102}, {"total_rate", 1115},
      {"score_rate2", 155}, {"scorerate", 110},
      {"scorerate_final", 111}, {"bestscorerate_now", 112},
      {"bestscorerate", 113}, {"targetscorerate_now", 114},
      {"targetscorerate", 115}, {"rate_pgreat", 140},
      {"rate_great", 141}, {"rate_good", 142}, {"rate_bad", 143},
      {"rate_poor", 144}, {"rate_maxcombo", 145},
      {"rate_exscore", 147}, {"duration_average", 372},
      {"timing_average", 374}, {"timign_stddev", 376},
      {"perfect_rate", 85}, {"great_rate", 86}, {"good_rate", 87},
      {"bad_rate", 88}, {"poor_rate", 89},
      {"rival_perfect_rate", 285}, {"rival_great_rate", 286},
      {"rival_good_rate", 287}, {"rival_bad_rate", 288},
      {"rival_poor_rate", 289}, {"best_rate", 183},
      {"rival_rate", 122}, {"target_rate", 135}, {"target_rate2", 157},
      {"hispeed", 310}, {"groovegauge_1p", 1107},
      {"chart_averagedensity", 367}, {"chart_enddensity", 362},
      {"chart_peakdensity", 360}, {"chart_totalgauge", 368},
      {"loading_progress", 165}, {"ir_totalclearrate", 227},
      {"ir_totalfullcomborate", 229},
      {"ir_player_noplay_rate", 203}, {"ir_player_failed_rate", 211},
      {"ir_player_assist_rate", 205},
      {"ir_player_lightassist_rate", 207},
      {"ir_player_easy_rate", 213}, {"ir_player_normal_rate", 215},
      {"ir_player_hard_rate", 217}, {"ir_player_exhard_rate", 209},
      {"ir_player_fullcombo_rate", 219},
      {"ir_player_perfect_rate", 223}, {"ir_player_max_rate", 225},
  }};
  for (const auto &[alias, id] : aliases) {
    if (*name == alias) {
      return id;
    }
  }
  return std::nullopt;
}

std::optional<int> resultNumberedRankingSelector(std::string_view name) {
  constexpr std::array<std::pair<std::string_view, int>, 4> prefixes{{
      {"ranking_exscore", 380}, {"ranking_index", 390},
      {"playertype_ranking", 380}, {"cleartype_ranking", 390},
  }};
  for (const auto &[prefix, firstId] : prefixes) {
    if (!name.starts_with(prefix)) continue;
    const std::string_view suffix = name.substr(prefix.size());
    int index = 0;
    const auto parsed = std::from_chars(suffix.data(),
                                        suffix.data() + suffix.size(), index);
    if (parsed.ec == std::errc{} &&
        parsed.ptr == suffix.data() + suffix.size() && index >= 1 &&
        index <= 10) {
      return firstId + index - 1;
    }
  }
  return std::nullopt;
}

} // namespace

ResultSkinStateBridge::ResultSkinStateBridge(ResultSkinData data,
                                             std::uint64_t frameSerial,
                                             std::int64_t elapsedMillis,
                                             const BeatorajaSkinConfiguration *configuration,
                                             const BeatorajaSkinModel *model)
    : data_(std::move(data)), frameSerial_(frameSerial),
      elapsedMillis_(std::max<std::int64_t>(0, elapsedMillis)),
      configuration_(configuration), model_(model) {
  if (data_.state != nullptr) {
    gaugeHistory_ = data_.state->gaugeHistory;
  } else if (data_.presentation != nullptr &&
             !data_.presentation->gaugeSeries.empty()) {
    const auto &series = data_.presentation->gaugeSeries.front();
    // SkinGameplayGraphStateView has a dense gauge span. Never turn an IR
    // service's missing sample into a fabricated 0% point: omit this graph
    // until it can be represented losslessly.
    if (std::ranges::all_of(series.points,
                            [](const auto &point) { return point.has_value(); })) {
      gaugeHistory_.reserve(series.points.size());
      for (const auto point : series.points) {
        gaugeHistory_.push_back(*point);
      }
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
  if (const auto id = beatorajaIntegerValuePropertySelector(*name)) return id;
  if (const auto id = resultNumberedRankingSelector(*name)) return id;
  constexpr std::string_view coursePrefix = "coursetitle";
  if (name->starts_with(coursePrefix)) {
    const std::string_view suffix(name->data() + coursePrefix.size(),
                                  name->size() - coursePrefix.size());
    int index = 0;
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(),
                                        index);
    if (parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() &&
        index >= 1 && index <= 10) {
      return 149 + index;
    }
  }
  constexpr std::string_view rankingPrefix = "rankingname";
  if (name->starts_with(rankingPrefix)) {
    const std::string_view suffix(name->data() + rankingPrefix.size(),
                                  name->size() - rankingPrefix.size());
    int index = 0;
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(),
                                        index);
    if (parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() &&
        index >= 1 && index <= 10) {
      return 119 + index;
    }
  }
  static constexpr std::array<std::pair<std::string_view, int>, 43> aliases{{
      {"rival", 1}, {"player", 2}, {"target", 3},
      {"title", 10}, {"fulltitle", 12}, {"subtitle", 11},
      {"genre", 13}, {"artist", 14}, {"subartist", 15},
      {"fullartist", 16}, {"mode", 60},
      {"sort", 61}, {"difficulty", 62}, {"skinname", 50},
      {"skinauthor", 51}, {"nowbpm", 92},
      {"tablename", 1001}, {"tablelevel", 1002}, {"tablefull", 1003},
      {"version", 1010}, {"songhashmd5", 1030},
      {"songhashsha256", 1031},
      {"score_rate", 1102}, {"total_rate", 1115},
      {"score_rate2", 155}, {"scorerate", 110},
      {"scorerate_final", 111}, {"bestscorerate_now", 112},
      {"bestscorerate", 113}, {"targetscorerate_now", 114},
      {"targetscorerate", 115}, {"rate_pgreat", 140},
      {"rate_great", 141}, {"rate_good", 142}, {"rate_bad", 143},
      {"rate_poor", 144}, {"rate_maxcombo", 145}, {"rate_exscore", 147},
      {"mastervolume", 17}, {"keyvolume", 18}, {"bgmvolume", 19},
      {"cleartype", 370}, {"cleartype_target", 371},
  }};
  for (const auto &[alias, id] : aliases) {
    if (*name == alias) return id;
  }
  return std::nullopt;
}

SkinPropertyLookup<bool> ResultSkinStateBridge::booleanProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = [&]() -> std::optional<int> {
    if (const auto *numeric = std::get_if<int>(&selector.value)) return *numeric;
    return beatorajaBooleanPropertySelector(
        std::get<std::string>(selector.value));
  }();
  if (!id) return unsupported<bool>();
  if (*id < 0) {
    if (*id == std::numeric_limits<int>::min()) return unsupported<bool>();
    const auto positive = booleanProperty({-*id});
    return positive.supported ? supported(!positive.value) : unsupported<bool>();
  }
  const auto currentScore = score();
  const auto maximum = maxScore();
  const auto previousScore = data_.previousBest
                                 ? std::optional<int>(data_.previousBest->score)
                                 : data_.state != nullptr ? std::optional<int>(0)
                                                          : std::nullopt;
  const auto previousCombo = data_.previousBest
                                 ? std::optional<int>(data_.previousBest->maxCombo)
                                 : data_.state != nullptr ? std::optional<int>(0)
                                                          : std::nullopt;
  const auto previousBadPoints =
      data_.previousBest && data_.previousBest->badPoints
          ? data_.previousBest->badPoints
          : data_.state != nullptr
                ? std::optional<int>(std::numeric_limits<int>::max())
                : std::nullopt;
  const auto targetScore = data_.pacemaker
                               ? std::optional<int>(data_.pacemaker->targetScore)
                               : data_.state != nullptr ? std::optional<int>(0)
                                                        : std::nullopt;
  const bool irOnline = data_.irOnline;
  if (*id == 50 || *id == 51) {
    return supported(*id == 51 ? irOnline : !irOnline);
  }
  if (*id == 1 || *id == 2 || *id == 3 || *id == 5 || *id == 21 ||
      *id == 22 || *id == 23 || *id == 80 || *id == 1030 ||
      *id == 1031 || *id == 290 || *id == 291 || *id == 292 || *id == 293) {
    return supported(false);
  }
  // BooleanPropertyFactory evaluates this pair only for BMSPlayer. A result
  // MainState therefore exposes neither option, including AUTO PLAY results.
  if (*id == 32 || *id == 33) return supported(false);
  // OPTION_LOADED is a BMSPlayer-only loading-state property. AbstractResult
  // reaches the factory's false branch rather than claiming player loading.
  if (*id == 81) return supported(false);
  if (*id == 1008) return supported(!data_.tableName.empty());
  if (*id >= 2241 && *id <= 2246) {
    const Judgement judgement = *id == 2241 ? PGreat
                                : *id == 2242 ? Great
                                : *id == 2243 ? Good
                                : *id == 2244 ? Bad
                                : *id == 2245 ? Poor
                                              : Kpoor;
    return supported(count(judgement).value_or(0) > 0);
  }
  if (*id >= 150 && *id <= 155) {
    const int difficulty = data_.difficultyOverride.value_or(
        data_.meta != nullptr ? data_.meta->Difficulty : 0);
    return supported(*id == 150 ? difficulty <= 0 || difficulty > 5
                                : difficulty == *id - 150);
  }
  if (*id >= 160 && *id <= 164) {
    const int keyMode = data_.keyModeOverride.value_or(
        data_.meta != nullptr ? data_.meta->KeyMode : 0);
    return supported((*id == 160 && keyMode == 7) ||
                     (*id == 161 && keyMode == 5) ||
                     (*id == 162 && keyMode == 14) ||
                     (*id == 163 && keyMode == 10) ||
                     (*id == 164 && keyMode == 9));
  }
  if (*id == 1160 || *id == 1161) {
    const int keyMode = data_.keyModeOverride.value_or(
        data_.meta != nullptr ? data_.meta->KeyMode : 0);
    return supported(keyMode == (*id == 1160 ? 24 : 48));
  }
  if (*id == 42 || *id == 43) {
    const auto type = resultGaugeType(data_);
    const int index = type ? gaugeTypeIndex(*type) : -1;
    return supported(*id == 42 ? index >= 0 && index <= 2 : index >= 3);
  }
  if (*id == 1046) {
    const auto type = resultGaugeType(data_);
    if (!type) return supported(false);
    const int index = gaugeTypeIndex(*type);
    return supported(index == 0 || index == 1 || index == 4 || index == 5 ||
                     index == 7 || index == 8);
  }
  if (*id == 170 || *id == 171) {
    const auto *chart = data_.gameplayGraph.chart.get();
    const bool hasBga = chart && chart->hasBga && *chart->hasBga;
    return supported(*id == 171 ? hasBga : !hasBga);
  }
  if (*id == 1177) {
    const auto *chart = data_.gameplayGraph.chart.get();
    // SongDataBooleanProperty reads the prepared chart's BpmStop flag, not
    // the runtime BPM graph. Retain that flag with the result snapshot.
    return supported(chart && chart->hasBpmStop && *chart->hasBpmStop);
  }
  if (*id == 172 || *id == 173) {
    const auto *chart = data_.gameplayGraph.chart.get();
    const bool hasLongNote = chart && chart->hasAnyLongNote
                                 ? *chart->hasAnyLongNote
                                 : data_.meta != nullptr &&
                                       (data_.meta->TotalLongNotes > 0 ||
                                        data_.meta->TotalBackSpinNotes > 0);
    return supported(*id == 173 ? hasLongNote : !hasLongNote);
  }
  if (*id == 174 || *id == 175) {
    return supported(*id == 175 ? data_.chartHasDocument
                                : !data_.chartHasDocument);
  }
  if (*id == 176 || *id == 177) {
    const bool changes = data_.meta != nullptr &&
                         data_.meta->MinBpm < data_.meta->MaxBpm;
    return supported(*id == 177 ? changes : !changes);
  }
  if (*id == 178 || *id == 179) {
    const auto *chart = data_.gameplayGraph.chart.get();
    const bool random = chart && chart->hasRandomSequence
                            ? *chart->hasRandomSequence
                            : data_.meta != nullptr &&
                                  !data_.meta->RandomValues.empty();
    return supported(*id == 179 ? random : !random);
  }
  if (*id >= 220 && *id <= 227 && currentScore && maximum) {
    // SkinProperty OPTION_AAA through OPTION_F are cumulative `qualifyRank`
    // thresholds, unlike the exact result-rank families (200/300/340).
    constexpr std::array<int, 8> lowerBounds{0, 6, 9, 12, 15, 18, 21, 24};
    const int threshold = lowerBounds[static_cast<std::size_t>(227 - *id)];
    return supported(static_cast<long long>(*currentScore) * 27 >=
                     static_cast<long long>(*maximum) * threshold);
  }
  if (((*id >= 200 && *id <= 207) || (*id >= 300 && *id <= 307) ||
       (*id >= 340 && *id <= 347)) && currentScore && maximum) {
    const int first = *id >= 340 ? 340 : *id >= 300 ? 300 : 200;
    return supported(resultRank(*currentScore, *maximum) == 7 - (*id - first));
  }
  if (*id >= 320 && *id <= 327) {
    return supported(previousScore && maximum &&
                     resultRank(*previousScore, *maximum) ==
                         7 - (*id - 320));
  }
  if (*id >= 180 && *id <= 184) {
    if (data_.meta == nullptr) return supported(false);
    const int judgeRank = data_.meta->Rank;
    switch (*id) {
    case 180:
      return supported(judgeRank == 0 || (judgeRank >= 10 && judgeRank < 35));
    case 181:
      return supported(judgeRank == 1 || (judgeRank >= 35 && judgeRank < 60));
    case 182:
      return supported(judgeRank == 2 || (judgeRank >= 60 && judgeRank < 85));
    case 183:
      return supported(judgeRank == 3 || (judgeRank >= 85 && judgeRank < 110));
    case 184:
      return supported(judgeRank == 4 || judgeRank >= 110);
    default:
      break;
    }
  }
  if ((*id == 330 || *id == 1330) && currentScore && previousScore) {
    return supported(*id == 330 ? *currentScore > *previousScore
                                : *currentScore == *previousScore);
  }
  if ((*id == 331 || *id == 1331) && previousCombo) {
    const auto combo = maxCombo();
    return combo ? supported(*id == 331 ? *combo > *previousCombo
                                        : *combo == *previousCombo)
                 : unsupported<bool>();
  }
  if ((*id == 332 || *id == 1332) && previousBadPoints) {
    const auto currentBadPoints = data_.presentation && !data_.state
                                      ? data_.presentation->badPoints
                                      : (data_.state
                                             ? std::optional<int>(
                                                   count(Bad).value_or(0) +
                                                   count(Poor).value_or(0) +
                                                   count(Kpoor).value_or(0))
                                             : std::nullopt);
    if (!currentBadPoints) return unsupported<bool>();
    return supported(*id == 332 ? *currentBadPoints < *previousBadPoints
                                : *currentBadPoints == *previousBadPoints);
  }
  if ((*id == 335 || *id == 1335) && currentScore && maximum &&
      previousScore) {
    const auto currentRate = static_cast<float>(*currentScore) / *maximum;
    const auto bestRate = static_cast<float>(*previousScore) / *maximum;
    return supported(*id == 335 ? currentRate > bestRate : currentRate == bestRate);
  }
  if ((*id == 336 || *id == 1336) && currentScore && targetScore) {
    return supported(*id == 336 ? *currentScore > *targetScore
                                : *currentScore == *targetScore);
  }
  if ((*id >= 352 && *id <= 354) && currentScore && targetScore) {
    return supported(*id == 352 ? *currentScore > *targetScore
                                : *id == 353 ? *currentScore < *targetScore
                                             : *currentScore == *targetScore);
  }
  if (*id == 190 || *id == 191) {
    return supported(*id == 191 ? data_.stageFileAvailable
                                : !data_.stageFileAvailable);
  }
  if (*id == 192 || *id == 193) {
    return supported(*id == 193 ? data_.bannerAvailable
                                : !data_.bannerAvailable);
  }
  if (*id == 194 || *id == 195) {
    return supported(*id == 195 ? data_.backBmpAvailable
                                : !data_.backBmpAvailable);
  }
  if (*id == 90 || *id == 91) {
    const auto lamp = data_.currentClearRankOverride
                          ? data_.currentClearRankOverride
                          : (data_.state != nullptr
                          ? std::optional<int>(data_.state->getClearTypeRank())
                          : (data_.presentation ? data_.presentation->lampRank
                                                : std::nullopt));
    const bool clear = lamp && *lamp > kClearTypeFailedRank;
    return supported(*id == 90 ? clear : !clear);
  }
  if (*id >= 300 && *id <= 307 && currentScore && maximum) {
    return supported(resultRank(*currentScore, *maximum) == 307 - *id);
  }
  if (isPinnedBeatorajaBooleanPropertyId(*id)) {
    // BooleanPropertyFactory has a false branch for every official property
    // outside the current Result state family. Preserve that source fallback
    // instead of treating a valid authored selector as an invalid skin.
    return supported(false);
  }
  return unsupported<bool>();
}

SkinPropertyLookup<std::int64_t> ResultSkinStateBridge::integerProperty(
    const SkinBuiltinPropertySelector &selector, SkinIntegerPropertyDomain domain) {
  if (domain == SkinIntegerPropertyDomain::IntegerValue) {
    if (const auto *name = std::get_if<std::string>(&selector.value)) {
      if (*name == "lua_gauge_type") {
        // MainStatePropertyLuaApiExporter exposes gauges only for BMSPlayer;
        // AbstractResult returns zero even though result gauge objects render.
        return supported<std::int64_t>(0);
      }
      if (*name == "exscore") {
        return score() ? supported<std::int64_t>(*score())
                       : unsupported<std::int64_t>();
      }
      if (*name == "time") {
        return supported<std::int64_t>(elapsedMillis_ * 1'000);
      }
      constexpr std::string_view judgePrefix = "judge:";
      if (name->starts_with(judgePrefix)) {
        int index = -1;
        const std::string_view suffix(name->data() + judgePrefix.size(),
                                      name->size() - judgePrefix.size());
        const auto parsed = std::from_chars(
            suffix.data(), suffix.data() + suffix.size(), index);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == suffix.data() + suffix.size()) {
          const Judgement judgement = index == 0   ? PGreat
                                     : index == 1 ? Great
                                     : index == 2 ? Good
                                     : index == 3 ? Bad
                                     : index == 4 ? Poor
                                     : index == 5 ? Kpoor
                                                  : None;
          return supported<std::int64_t>(
              judgement == None ? 0 : count(judgement).value_or(0));
        }
        return supported<std::int64_t>(0);
      }
    }
  }
  const auto id = integerSelector(selector);
  const auto imageId = [&]() -> std::optional<int> {
    if (domain != SkinIntegerPropertyDomain::ImageIndex) return id;
    if (const auto *name = std::get_if<std::string>(&selector.value)) {
      // IntegerPropertyFactory has independent name namespaces for ValueType
      // and IndexType. Resolve a named image selector in its own namespace
      // before considering aliases such as ValueType.mode (ID 60).
      return beatorajaImageIndexPropertySelector(*name);
    }
    return id;
  }();
  if (!imageId) return unsupported<std::int64_t>();
  if (domain == SkinIntegerPropertyDomain::ImageIndex) {
    const auto *configuration = data_.configuration
                                    ? std::addressof(*data_.configuration)
                                    : nullptr;
    if ((*imageId >= 500 && *imageId <= 519) ||
        (*imageId >= 1510 && *imageId <= 1699)) {
      // IntegerPropertyFactory creates judge image properties before its
      // result-only selectors. AbstractResult is not a BMSPlayer, so their
      // documented value is zero (rather than an absent property).
      return supported<std::int64_t>(0);
    }
    if ((*imageId >= 170 && *imageId <= 185) ||
        (*imageId >= 386 && *imageId <= 388)) {
      // SkinPropertyMapper recognizes these as skin-selection controls.
      // They are valid image properties, but only SkinConfiguration can
      // evaluate them; an AbstractResult therefore receives MIN_VALUE.
      // This deliberately precedes the 380-389 ranking pattern because
      // Beatoraja's 24-key skin selectors shadow 386-388.
      return supported<std::int64_t>(std::numeric_limits<int>::min());
    }
    switch (*imageId) {
    case 10:
    case 11:
    case 12:
      // MusicSelector-only filters have no AbstractResult branch.
      return supported<std::int64_t>(std::numeric_limits<int>::min());
    case 40: {
      if (data_.state != nullptr) {
        return supported<std::int64_t>(gaugeTypeIndex(data_.state->gaugeType));
      }
      return data_.gaugeTypeOverride
                 ? supported<std::int64_t>(
                       gaugeTypeIndex(*data_.gaugeTypeOverride))
                 : unsupported<std::int64_t>();
    }
    case 42:
      return data_.replayRandomOption1P
                 ? supported<std::int64_t>(*data_.replayRandomOption1P)
                 : unsupported<std::int64_t>();
    case 43:
      return data_.replayRandomOption2P
                 ? supported<std::int64_t>(*data_.replayRandomOption2P)
                 : unsupported<std::int64_t>();
    case 54:
      return data_.replayDoublePlayOption
                 ? supported<std::int64_t>(*data_.replayDoublePlayOption)
                 : unsupported<std::int64_t>();
    case 55:
      return configuration
                 ? supported<std::int64_t>(configuration->hispeedFixMode)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 61:
    case 62:
    case 63:
      // The target ScoreData option is distinct from the displayed target
      // score. It is unavailable until a result transport retains it.
      return supported<std::int64_t>(std::numeric_limits<int>::min());
    case 72:
      // Config.BGA_ON is 0 and Config.BGA_OFF is 2. Aso has no source
      // equivalent of Config.BGA_AUTO, so retain only those two states.
      return configuration
                 ? supported<std::int64_t>(configuration->bgaEnabled ? 0 : 2)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 75:
      return configuration
                 ? supported<std::int64_t>(
                       configuration->notesDisplayTimingAutoAdjust ? 1 : 0)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 78:
      return supported<std::int64_t>(
          configuration != nullptr ? configuration->gaugeAutoShiftImageIndex
                                   : std::numeric_limits<int>::min());
    case 301:
    case 303:
      return configuration
                 ? supported<std::int64_t>(
                       *imageId == 301
                           ? (configuration->customJudge ? 1 : 0)
                           : (configuration->showJudgeArea ? 1 : 0))
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 302:
    case 304:
    case 307:
      if (configuration == nullptr) {
        return supported<std::int64_t>(std::numeric_limits<int>::min());
      }
      return supported<std::int64_t>(
          *imageId == 302 ? (configuration->scrollMode == 1 ? 1 : 0)
          : *imageId == 304 ? (configuration->longNoteModifierMode == 1 ? 1 : 0)
                            : (configuration->mineMode == 1 ? 1 : 0));
    case 305:
      return configuration
                 ? supported<std::int64_t>(
                       configuration->markProcessedNotes ? 1 : 0)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 306:
      return configuration
                 ? supported<std::int64_t>(configuration->bpmGuideEnabled ? 1
                                                                           : 0)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 321: case 322: case 323: case 324:
      return configuration
                 ? supported<std::int64_t>(configuration->autoSaveReplay[
                       static_cast<std::size_t>(*imageId - 321)])
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 330: case 331: case 332:
      if (configuration == nullptr) {
        return supported<std::int64_t>(std::numeric_limits<int>::min());
      }
      return supported<std::int64_t>(
          *imageId == 330 ? (configuration->laneCoverEnabled ? 1 : 0)
          : *imageId == 331 ? (configuration->liftEnabled ? 1 : 0)
                            : (configuration->hiddenEnabled ? 1 : 0));
    case 340:
      return configuration
                 ? supported<std::int64_t>(
                       configuration->judgeAlgorithmImageIndex)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 341:
      return configuration
                 ? supported<std::int64_t>(
                       configuration->bottomShiftableGaugeImageIndex)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 342:
      return configuration
                 ? supported<std::int64_t>(
                       configuration->hispeedAutoAdjust ? 1 : 0)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 343:
      return configuration
                 ? supported<std::int64_t>(
                       configuration->guideSoundEffects ? 1 : 0)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    case 350: case 351: case 352: case 353:
      if (configuration == nullptr) {
        return supported<std::int64_t>(std::numeric_limits<int>::min());
      }
      return supported<std::int64_t>(
          *imageId == 350   ? configuration->extraNoteDepth
          : *imageId == 351 ? configuration->mineMode
          : *imageId == 352 ? configuration->scrollMode
                             : configuration->longNoteModifierMode);
    case 360: case 361:
      if (configuration == nullptr) {
        return supported<std::int64_t>(std::numeric_limits<int>::min());
      }
      return supported<std::int64_t>(
          *imageId == 360 ? configuration->sevenToNinePattern
                           : configuration->sevenToNineType);
    case 308:
      if (const auto *chart = data_.gameplayGraph.chart.get();
          chart != nullptr && chart->hasAnyLongNote &&
          chart->hasUndefinedLongNote && chart->hasLongNote &&
          chart->hasChargeNote && chart->hasHellChargeNote &&
          *chart->hasAnyLongNote && !*chart->hasUndefinedLongNote) {
        const int mode = *chart->hasLongNote       ? long_note_mode::kLnValue
                         : *chart->hasChargeNote   ? long_note_mode::kCnValue
                                                    : long_note_mode::kHcnValue;
        return supported<std::int64_t>(mode - long_note_mode::kLnValue);
      }
      if (const auto *chart = data_.gameplayGraph.chart.get();
          chart != nullptr && chart->selectedLongNoteMode) {
        return supported<std::int64_t>(
            long_note_mode::normalizeSelectedValue(*chart->selectedLongNoteMode) -
            long_note_mode::kLnValue);
      }
      return data_.meta != nullptr
                 ? supported<std::int64_t>(
                       long_note_mode::normalizeSelectedValue(data_.meta->LnMode) -
                       long_note_mode::kLnValue)
                 : unsupported<std::int64_t>();
    case 450: case 451: case 452: case 453: case 454:
    case 455: case 456: case 457: case 458:
      return resultLaneAssignment(data_, 0, *imageId - 450);
    case 459:
      return resultLaneAssignment(data_, 0, -1);
    case 460: case 461: case 462: case 463: case 464:
    case 465: case 466:
      return resultLaneAssignment(data_, 1, *imageId - 460);
    case 469:
      return resultLaneAssignment(data_, 1, -1);
    case 89:
    case 90:
      if (!data_.songReviewFavorite) {
        return supported<std::int64_t>(std::numeric_limits<int>::min());
      }
      {
        const int favoriteBit = *imageId == 89 ? 1 : 2;
        const int invisibleBit = *imageId == 89 ? 4 : 8;
        const int favorite = *data_.songReviewFavorite;
        return supported<std::int64_t>(
            (favorite & invisibleBit) != 0 ? 2
            : (favorite & favoriteBit) != 0 ? 1
                                               : 0);
      }
    case 370: {
      const auto lamp = data_.currentClearRankOverride
                            ? data_.currentClearRankOverride
                            : (data_.presentation ? data_.presentation->lampRank
                                                  : (data_.state ? std::optional<int>(
                                                        data_.state->getClearTypeRank())
                                                                 : std::nullopt));
      return lamp ? supported<std::int64_t>(
                        beatorajaClearTypeImageIndex(*lamp))
                  : unsupported<std::int64_t>();
    }
    case 371: {
      const auto lamp = data_.previousLampBest
                            ? std::optional<int>(data_.previousLampBest->clearType)
                            : (data_.previousBest
                                   ? std::optional<int>(data_.previousBest->clearType)
                                   : std::nullopt);
      return supported<std::int64_t>(lamp ? beatorajaClearTypeImageIndex(*lamp)
                                          : 0);
    }
    case 380: case 381: case 382: case 383: case 384:
    case 385: case 389: {
      const std::size_t index = static_cast<std::size_t>(*imageId - 380);
      // RankingData distinguishes only You, Rival, and None. The application
      // ranking snapshot retains current-user identity but no rival roster,
      // so preserve the two source states it can represent exactly.
      return index < data_.irRankingEntries.size()
                 ? supported<std::int64_t>(
                       data_.irRankingEntries[index].currentUser ? 1 : 0)
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    }
    case 390: case 391: case 392: case 393: case 394:
    case 395: case 396: case 397: case 398: case 399: {
      const std::size_t index = static_cast<std::size_t>(*imageId - 390);
      return index < data_.irRankingEntries.size()
                 ? supported<std::int64_t>(beatorajaClearTypeImageIndex(
                       data_.irRankingEntries[index].clearType))
                 : supported<std::int64_t>(std::numeric_limits<int>::min());
    }
    case 400:
      // IndexType.constant intentionally has no AbstractResult branch; its
      // source fallback is -1, not the generic image-cache frame zero.
      return supported<std::int64_t>(-1);
    default:
      // SkinImage stores a null ref when getImageIndexProperty(int) has no
      // factory property. Its prepare path then uses frame zero, so retain
      // that behavior for numeric selectors admitted by the catalog.
      const bool numericInFactoryRange =
          std::holds_alternative<int>(selector.value) && *imageId >= 0 &&
          *imageId < 65'536;
      return numericInFactoryRange
                 ? supported<std::int64_t>(0)
                 : unsupported<std::int64_t>();
    }
  }
  const auto currentScore = score();
  const auto maximum = maxScore();
  const auto notes = data_.meta ? std::optional<int>(data_.meta->TotalNotes)
                                : (maximum ? std::optional<int>(*maximum / 2)
                                           : std::nullopt);
  const auto result = [this, id, imageId, currentScore, maximum, notes]()
      -> std::optional<int> {
    const auto *configuration = data_.configuration
                                    ? std::addressof(*data_.configuration)
                                    : nullptr;
    // AbstractResult always supplies an old ScoreData: a first play uses the
    // default score (EX 0, combo 0) and no configured target uses score 0.
    // A remote result has neither source object, so preserve absence there.
    const auto previousScore = data_.previousBest
                                   ? std::optional<int>(data_.previousBest->score)
                                   : data_.state != nullptr ? std::optional<int>(0)
                                                            : std::nullopt;
    const auto previousCombo = data_.previousBest
                                   ? std::optional<int>(data_.previousBest->maxCombo)
                                   : data_.state != nullptr ? std::optional<int>(0)
                                                            : std::nullopt;
    const auto targetScore = data_.pacemaker
                                 ? std::optional<int>(data_.pacemaker->targetScore)
                                 : data_.state != nullptr ? std::optional<int>(0)
                                                          : std::nullopt;
    const auto badPoints = [this]() -> std::optional<int> {
      if (data_.presentation && !data_.state) return data_.presentation->badPoints;
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
    switch (*imageId) {
    case 10:
      return configuration
                 ? std::optional<int>(static_cast<int>(
                       configuration->gameplayHispeed * 100.0F))
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 20:
      return data_.context != nullptr
                 ? std::optional<int>(data_.context->currentFramesPerSecond.load(
                       std::memory_order_acquire))
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 21: case 22: case 23: case 24: case 25: case 26:
      return currentLocalCalendarField(*imageId);
    case 27: case 28: case 29: {
      const std::int64_t uptime = data_.context != nullptr
                                       ? std::max<std::int64_t>(
                                             0, data_.context->applicationUptimeMillis.load(
                                                    std::memory_order_acquire))
                                       : 0;
      return *id == 27   ? uptime / 3'600'000
             : *id == 28 ? (uptime / 60'000) % 60
                         : (uptime / 1'000) % 60;
    }
    case 12:
      return configuration
                 ? std::optional<int>(
                       configuration->notesDisplayTimingMilliseconds)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 14:
    case 314:
    case 315:
    case 316:
      // LaneRenderer-only covers have no AbstractResult branch.
      return std::numeric_limits<int>::min();
    case 77:
    case 78:
    case 79:
      // Per-chart play counts are MusicSelector-only.
      return std::numeric_limits<int>::min();
    case 17:
      return data_.playerHistory
                 ? std::optional<int>(static_cast<int>(
                       data_.playerHistory->playDurationSeconds / 3600))
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 18:
      return data_.playerHistory
                 ? std::optional<int>(static_cast<int>(
                       (data_.playerHistory->playDurationSeconds / 60) % 60))
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 19:
      return data_.playerHistory
                 ? std::optional<int>(static_cast<int>(
                       data_.playerHistory->playDurationSeconds % 60))
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 30:
      return data_.playerHistory
                 ? std::optional<int>(data_.playerHistory->playCount)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 31:
      return data_.playerHistory
                 ? std::optional<int>(data_.playerHistory->clearCount)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 32:
      return data_.playerHistory
                 ? std::optional<int>(data_.playerHistory->playCount -
                                      data_.playerHistory->clearCount)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 33: case 34: case 35: case 36: case 37:
      return data_.playerHistory
                 ? std::optional<int>(data_.playerHistory->judgementCounts[
                       static_cast<std::size_t>(*id - 33)])
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 333:
      if (!data_.playerHistory) return std::numeric_limits<int>::min();
      return data_.playerHistory->judgementCounts[0] +
             data_.playerHistory->judgementCounts[1] +
             data_.playerHistory->judgementCounts[2] +
             data_.playerHistory->judgementCounts[3];
    case 57:
      return data_.context != nullptr
                 ? std::optional<int>(static_cast<int>(
                       data_.context->settings.audioVideo.audio.masterVolume *
                       100.0F))
                 : std::nullopt;
    case 58:
      return data_.context != nullptr
                 ? std::optional<int>(static_cast<int>(
                       data_.context->settings.audioVideo.audio.keysoundVolume *
                       100.0F))
                 : std::nullopt;
    case 59:
      return data_.context != nullptr
                 ? std::optional<int>(static_cast<int>(
                       data_.context->settings.audioVideo.audio.bgmVolume *
                       100.0F))
                 : std::nullopt;
    case 165:
      // MusicResult is entered only after BMSResource preparation, so the
      // source's combined BGA/audio load progress is complete.
      return 100;
    case 100: {
      // ScoreDataProperty.getNowScore is a mode-specific score point, not EX
      // score. At a result screen the result's final combo and judgement
      // counts are the same inputs Beatoraja uses for this calculation.
      if (!data_.state || !notes || *notes <= 0) return std::nullopt;
      const long long perfect = count(PGreat).value_or(0);
      const long long great = count(Great).value_or(0);
      const long long good = count(Good).value_or(0);
      long long numerator = 0;
      switch (data_.keyModeOverride.value_or(
          data_.meta != nullptr ? data_.meta->KeyMode : 0)) {
      case 5:
      case 10:
        numerator = 100'000LL * (perfect + great) + 50'000LL * good;
        break;
      case 7:
      case 14:
        numerator = 150'000LL * perfect + 100'000LL * great +
                    20'000LL * good +
                    50'000LL * maxCombo().value_or(0);
        break;
      case 9:
        numerator = 100'000LL * perfect + 70'000LL * great + 40'000LL * good;
        break;
      default:
        numerator = 1'000'000LL * perfect + 700'000LL * great +
                    400'000LL * good;
        break;
      }
      return static_cast<int>(numerator / *notes);
    }
    case 71: case 101: case 171: return currentScore;
    case 72: return maximum;
    case 74: case 106: return notes;
    case 75: case 105: case 174: return maxCombo();
    case 76: case 177: return badPoints();
    case 80: case 81: case 82: case 83: case 84:
      return count(beatorajaJudgement(*id - 80));
    case 85: case 86: case 87: case 88: case 89:
      if (!notes || *notes <= 0) return std::numeric_limits<int>::min();
      return count(beatorajaJudgement(*id - 85)).value_or(0) * 100 / *notes;
    case 45: case 46: case 47: case 48: case 49: case 96:
      return data_.playLevelOverride
                 ? std::optional<int>(static_cast<int>(
                       std::lround(*data_.playLevelOverride)))
                 : data_.meta
                 ? std::optional<int>(data_.meta->PlayLevelText.empty()
                                          ? static_cast<int>(
                                                std::lround(data_.meta->PlayLevel))
                                          : beatorajaParseInt(
                                                data_.meta->PlayLevelText)
                                                .value_or(0))
                 : std::nullopt;
    case 102: case 115: case 155:
      return scoreRate() ? std::optional<int>(scoreRateParts(*scoreRate()).first)
                         : std::nullopt;
    case 103: case 116: case 156:
      return scoreRate() ? std::optional<int>(scoreRateParts(*scoreRate()).second)
                         : std::nullopt;
    case 104:
      return data_.state ? std::optional<int>(data_.state->combo) : std::nullopt;
    case 121: case 151: return targetScore.value_or(std::numeric_limits<int>::min());
    case 122: case 135: case 157:
      return targetRate() ? std::optional<int>(scoreRateParts(*targetRate()).first)
                          : std::optional<int>(std::numeric_limits<int>::min());
    case 123: case 136: case 158:
      return targetRate() ? std::optional<int>(scoreRateParts(*targetRate()).second)
                          : std::optional<int>(std::numeric_limits<int>::min());
    case 108:
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
      return data_.previousBest && data_.previousBest->badPoints
                 ? data_.previousBest->badPoints
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 178:
      return badPoints() && data_.previousBest && data_.previousBest->badPoints
                 ? std::optional<int>(*badPoints() -
                                      *data_.previousBest->badPoints)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 154:
      return currentScore && maximum ? resultNextRank(*currentScore, *maximum)
                                      : std::nullopt;
    case 160:
      return std::numeric_limits<int>::min();
    case 161:
    case 162:
      // TIMER_PLAY is off in AbstractResult, so TimerManager's now-time is
      // zero even though the property itself remains valid.
      return 0;
    case 163:
    case 164:
      return std::numeric_limits<int>::min();
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
                         : (data_.presentation ? data_.presentation->badPoints
                                               : std::nullopt);
    case 425:
      return data_.state ? std::optional<int>(data_.state->comboBreak)
                         : (data_.presentation ? data_.presentation->comboBreak
                                               : std::nullopt);
    case 179:
      if (data_.irCurrentUserRank) return data_.irCurrentUserRank;
      for (const auto &entry : data_.irRankingEntries) {
        if (entry.currentUser) return entry.rank;
      }
      return std::numeric_limits<int>::min();
    case 200:
    case 220:
      return std::numeric_limits<int>::min();
    case 180: return std::numeric_limits<int>::min();
    case 202: case 204: case 206: case 208: case 210: case 212:
    case 214: case 216: case 218: case 222: case 224:
    case 203: case 205: case 207: case 209: case 211: case 213:
    case 215: case 217: case 219: case 223: case 225:
    case 226: case 227: case 228: case 229:
    case 230: case 231: case 232: case 233: case 234: case 235:
    case 236: case 237: case 238: case 239: case 240: case 241: case 242:
      // A result ranking needs the source RankingData clear-count histogram.
      // The app's compact IR row snapshot intentionally does not fabricate
      // it from visible rows, so retain Integer.MIN_VALUE exactly as source.
      return std::numeric_limits<int>::min();
    case 183:
      if (const auto rate = previousRate()) return scoreRateParts(*rate).first;
      return std::numeric_limits<int>::min();
    case 184:
      if (const auto rate = previousRate()) return scoreRateParts(*rate).second;
      return std::numeric_limits<int>::min();
    case 243: case 244: case 245: case 246:
    case 247: case 248: case 249:
      if (!data_.currentScoreDateUnixSeconds ||
          *data_.currentScoreDateUnixSeconds <= 0 ||
          *data_.currentScoreDateUnixSeconds >
              std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::min();
      }
      if (*id == 243) {
        return static_cast<int>(*data_.currentScoreDateUnixSeconds);
      }
      return localCalendarField(*id - 223,
                                static_cast<std::time_t>(
                                    *data_.currentScoreDateUnixSeconds));
    case 271:
      return targetScore.value_or(std::numeric_limits<int>::min());
    case 280: case 281: case 282: case 283: case 284:
    case 285: case 286: case 287: case 288: case 289:
    case 300:
    case 320: case 321: case 322: case 323: case 324:
    case 325: case 326: case 327: case 328: case 329: case 330:
      return std::numeric_limits<int>::min();
    case 310:
      return configuration
                 ? std::optional<int>(static_cast<int>(
                       configuration->gameplayHispeed))
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 311:
      return configuration
                 ? std::optional<int>(static_cast<int>(
                       configuration->gameplayHispeed * 100.0F) % 100)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 312:
      return configuration
                 ? std::optional<int>(
                       configuration->visibleTimeDurationMilliseconds)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 313:
      return configuration
                 ? std::optional<int>(
                       configuration->visibleTimeDurationMilliseconds * 3 / 5)
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 1312: case 1313: case 1314: case 1315:
    case 1316: case 1317: case 1318: case 1319:
    case 1320: case 1321: case 1322: case 1323:
    case 1324: case 1325: case 1326: case 1327:
      // IntegerPropertyFactory creates this numeric range for every state;
      // only BMSPlayer has a LaneRenderer, so AbstractResult returns zero.
      return 0;
    case 380: case 381: case 382: case 383: case 384:
    case 385: case 386: case 387: case 388: case 389: {
      const std::size_t index = static_cast<std::size_t>(*id - 380);
      return index < data_.irRankingEntries.size()
                 ? std::optional<int>(data_.irRankingEntries[index].score)
                 : std::optional<int>(std::numeric_limits<int>::min());
    }
    case 390: case 391: case 392: case 393: case 394:
    case 395: case 396: case 397: case 398: case 399: {
      const std::size_t index = static_cast<std::size_t>(*id - 390);
      return index < data_.irRankingEntries.size()
                 ? std::optional<int>(data_.irRankingEntries[index].rank)
                 : std::optional<int>(std::numeric_limits<int>::min());
    }
    case 350:
      return data_.gameplayGraph.chart &&
                     data_.gameplayGraph.chart->normalKeyNotes
                 ? data_.gameplayGraph.chart->normalKeyNotes
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 351:
      return data_.gameplayGraph.chart &&
                     data_.gameplayGraph.chart->longKeyNotes
                 ? data_.gameplayGraph.chart->longKeyNotes
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 352:
      return data_.gameplayGraph.chart &&
                     data_.gameplayGraph.chart->normalScratchNotes
                 ? data_.gameplayGraph.chart->normalScratchNotes
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 353:
      return data_.gameplayGraph.chart &&
                     data_.gameplayGraph.chart->longScratchNotes
                 ? data_.gameplayGraph.chart->longScratchNotes
                 : std::optional<int>(std::numeric_limits<int>::min());
    case 360:
    case 361:
    case 362:
    case 363:
    case 364:
    case 365: {
      const auto *chart = data_.gameplayGraph.chart.get();
      const auto value = *id <= 361 ? (chart ? chart->peakDensity : std::nullopt)
                         : *id <= 363 ? (chart ? chart->endDensity : std::nullopt)
                                      : (chart ? chart->averageDensity : std::nullopt);
      if (!value) return std::numeric_limits<int>::min();
      return *id % 2 == 0 ? javaDoubleToInt(*value)
                          : javaDoubleToInt(*value * 100.0) % 100;
    }
    case 368:
      if (data_.gameplayGraph.chart && data_.gameplayGraph.chart->totalGauge) {
        return javaDoubleToInt(*data_.gameplayGraph.chart->totalGauge);
      }
      return data_.meta ? std::optional<int>(javaDoubleToInt(data_.meta->Total))
                        : std::optional<int>(std::numeric_limits<int>::min());
    case 370: {
      const auto lamp = data_.currentClearRankOverride
                            ? data_.currentClearRankOverride
                            : (data_.presentation ? data_.presentation->lampRank
                                                  : (data_.state ? std::optional<int>(
                                                        data_.state->getClearTypeRank())
                                                                 : std::nullopt));
      return lamp ? std::optional<int>(beatorajaClearTypeImageIndex(*lamp))
                  : std::nullopt;
    }
    case 371: {
      const auto lamp = data_.previousLampBest
                            ? std::optional<int>(data_.previousLampBest->clearType)
                            : (data_.previousBest
                                   ? std::optional<int>(data_.previousBest->clearType)
                                   : std::nullopt);
      return lamp ? std::optional<int>(beatorajaClearTypeImageIndex(*lamp))
                  : std::optional<int>(0);
    }
    case 372:
      if (data_.courseResult) return 0;
      return data_.averageJudgeMicros
                 ? std::optional<int>(static_cast<int>(
                       *data_.averageJudgeMicros / 1'000LL))
                 : data_.state != nullptr ? std::optional<int>(0)
                                          : std::optional<int>(
                                                std::numeric_limits<int>::min());
    case 373:
      if (data_.courseResult) return 0;
      return data_.averageJudgeMicros
                 ? std::optional<int>(static_cast<int>(
                       (*data_.averageJudgeMicros / 10LL) % 100LL))
                 : data_.state != nullptr ? std::optional<int>(0)
                                          : std::optional<int>(
                                                std::numeric_limits<int>::min());
    case 374: case 375:
      if (data_.courseResult) return 0;
      if (*id == 374 && data_.timingAverageMillis) {
        return static_cast<int>(*data_.timingAverageMillis);
      }
      if (*id == 375 && data_.timingAverageMillis) {
        const double average = *data_.timingAverageMillis;
        const int fraction = static_cast<int>(std::abs(average) * 100.0) % 100;
        return average < 0.0 ? -fraction : fraction;
      }
      // MusicResult initializes TimingDistribution before collecting replay
      // samples. With none, its documented defaults are Float.MAX_VALUE for
      // average and -1 for standard deviation; Java's float-to-int cast is
      // saturating, while the fractional average is 47.
      if (data_.state != nullptr) {
        return *id == 374 ? std::numeric_limits<int>::max() : 47;
      }
      return std::numeric_limits<int>::min();
    case 376:
      if (data_.courseResult) return 0;
      return data_.timingStandardDeviationMillis
                 ? std::optional<int>(static_cast<int>(
                       *data_.timingStandardDeviationMillis))
                 : data_.state != nullptr ? std::optional<int>(-1)
                                          : std::optional<int>(
                                                std::numeric_limits<int>::min());
    case 377:
      if (data_.courseResult) return 0;
      return data_.timingStandardDeviationMillis
                 ? std::optional<int>(static_cast<int>(
                       *data_.timingStandardDeviationMillis * 100.0) % 100)
                 : data_.state != nullptr ? std::optional<int>(0)
                                          : std::optional<int>(
                                                std::numeric_limits<int>::min());
    case 400:
      return data_.meta ? std::optional<int>(data_.meta->Rank) : std::nullopt;
    case 402: case 403: case 404:
      return 0;
    case 525: case 526: case 527:
      // IntegerPropertyFactory's judge-duration properties are BMSPlayer
      // only; AbstractResult returns its documented zero fallback.
      return 0;
    case 1163:
      return data_.meta
                 ? std::optional<int>(static_cast<int>(
                       (data_.meta->PlayLength / 60'000'000LL) % 60LL))
                 : std::nullopt;
    case 1164:
      return data_.meta
                 ? std::optional<int>(static_cast<int>(
                       (data_.meta->PlayLength / 1'000'000LL) % 60LL))
                 : std::nullopt;
    case 90:
      return data_.gameplayGraph.chart &&
                     data_.gameplayGraph.chart->maximumBpm > 0.0
                 ? std::optional<int>(static_cast<int>(
                       data_.gameplayGraph.chart->maximumBpm))
                 : (data_.meta ? std::optional<int>(
                       static_cast<int>(data_.meta->MaxBpm))
                               : std::nullopt);
    case 91:
      return data_.gameplayGraph.chart &&
                     data_.gameplayGraph.chart->minimumBpm > 0.0
                 ? std::optional<int>(static_cast<int>(
                       data_.gameplayGraph.chart->minimumBpm))
                 : (data_.meta ? std::optional<int>(
                       static_cast<int>(data_.meta->MinBpm))
                               : std::nullopt);
    case 92:
      return data_.gameplayGraph.chart && data_.gameplayGraph.chart->mainBpm > 0.0
                 ? std::optional<int>(static_cast<int>(
                       data_.gameplayGraph.chart->mainBpm))
                 : (data_.meta ? std::optional<int>(
                       static_cast<int>(data_.meta->Bpm))
                               : std::nullopt);
    default: return std::nullopt;
    }
  }();
  return result ? supported<std::int64_t>(*result) : unsupported<std::int64_t>();
}

SkinPropertyLookup<double> ResultSkinStateBridge::floatProperty(
    const SkinBuiltinPropertySelector &selector, SkinFloatPropertyDomain) {
  if (const auto *name = std::get_if<std::string>(&selector.value);
      name != nullptr && *name == "lua_gauge") {
    // MainStatePropertyLuaApiExporter.getGauge returns zero for result
    // MainStates, independently of the gauge that SkinGauge draws.
    return supported(0.0);
  }
  const auto id = resultFloatSelector(selector);
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
  // AbstractResult always constructs an old ScoreData and a target score
  // value. A first play therefore exposes zero-valued rate families rather
  // than an absent property; remote results still preserve their lack of
  // source ScoreData.
  const auto previousScore = data_.previousBest
                                 ? std::optional<int>(data_.previousBest->score)
                                 : data_.state != nullptr ? std::optional<int>(0)
                                                          : std::nullopt;
  const auto targetScore = data_.pacemaker
                               ? std::optional<int>(data_.pacemaker->targetScore)
                               : data_.state != nullptr ? std::optional<int>(0)
                                                        : std::nullopt;
  if (data_.context != nullptr) {
    switch (*id) {
    case 17:
      return supported(
          static_cast<double>(data_.context->settings.audioVideo.audio.masterVolume));
    case 18:
      return supported(static_cast<double>(
          data_.context->settings.audioVideo.audio.keysoundVolume));
    case 19:
      return supported(
          static_cast<double>(data_.context->settings.audioVideo.audio.bgmVolume));
    default:
      break;
    }
  }
  if (*id == 310) {
    return data_.configuration
               ? supported(static_cast<double>(data_.configuration->gameplayHispeed))
               : supported(static_cast<double>(std::numeric_limits<float>::min()));
  }
  if (*id == 165) {
    // Result scenes begin only after chart audio and BGA preparation has
    // completed, which is the completed BMSResource branch upstream.
    return supported(1.0);
  }
  if (*id == 102) {
    // Result scenes start after the resource's BGA/audio preparation branch
    // has completed. This mirrors both source load-progress selectors.
    return supported(1.0);
  }
  if (*id == 1 || *id == 4 || *id == 5 || *id == 6 || *id == 7 ||
      *id == 8 || *id == 20 || *id == 101 || *id == 103 ||
      (*id >= 105 && *id <= 109)) {
    // These RateType values are MusicSelector, BMSPlayer, or
    // SkinConfiguration-only. AbstractResult returns their source zero.
    return supported(0.0);
  }
  if (*id >= 285 && *id <= 289) {
    // AsoBMaShow has no independent rival ScoreData cache for a completed
    // result. FloatPropertyFactory returns Float.MIN_VALUE in that case.
    return supported(static_cast<double>(std::numeric_limits<float>::min()));
  }
  if (*id == 203 || *id == 205 || *id == 207 || *id == 209 ||
      *id == 211 || *id == 213 || *id == 215 || *id == 217 ||
      *id == 219 || *id == 223 || *id == 225 || *id == 227 ||
      *id == 229) {
    // The ranking snapshot retains visible rows only, not the clear-count
    // histogram required by the source IR aggregate-rate properties.
    return supported(static_cast<double>(std::numeric_limits<float>::min()));
  }
  if (*id == 1102 || *id == 1115 || *id == 155) {
    const auto value = rate();
    return value ? supported(*value) : unsupported<double>();
  }
  if (*id == 110 || *id == 111) {
    const auto value = rate();
    return value ? supported(*value) : unsupported<double>();
  }
  if (*id == 112 || *id == 113) {
    const auto value = scoreRate(previousScore);
    return value ? supported(*value) : unsupported<double>();
  }
  if (*id == 114 || *id == 115) {
    const auto value = scoreRate(targetScore);
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
    // RateType.rate_exscore delegates to createSelectedScoreRate, whose
    // AbstractResult branch is the same zero fallback as non-selection
    // MainStates.
    return supported(0.0);
  }
  if (*id == 360 || *id == 362 || *id == 367) {
    const auto *chart = data_.gameplayGraph.chart.get();
    const auto value = *id == 360 ? (chart ? chart->peakDensity : std::nullopt)
                       : *id == 362 ? (chart ? chart->endDensity : std::nullopt)
                                    : (chart ? chart->averageDensity : std::nullopt);
    return value ? supported(*value)
                 : supported(static_cast<double>(std::numeric_limits<float>::min()));
  }
  if (*id == 368) {
    if (data_.gameplayGraph.chart && data_.gameplayGraph.chart->totalGauge) {
      return supported(*data_.gameplayGraph.chart->totalGauge);
    }
    return data_.meta != nullptr
               ? supported(data_.meta->Total)
               : supported(static_cast<double>(std::numeric_limits<float>::min()));
  }
  if (*id == 374) {
    // FloatType.timing_average exposes TimingDistribution's fixed 150 ms
    // array center (not its measured mean) divided by 1000.
    return supported(0.15);
  }
  if (*id == 372 && data_.courseResult) {
    return supported(0.0);
  }
  if (*id == 372 && data_.averageJudgeMicros) {
    return supported(static_cast<double>(*data_.averageJudgeMicros) / 1'000.0);
  }
  if (*id == 376 && data_.courseResult) {
    return supported(0.0);
  }
  if (*id == 376 && data_.timingStandardDeviationMillis) {
    return supported(*data_.timingStandardDeviationMillis);
  }
  if (*id == 376 && data_.state != nullptr) {
    return supported(-1.0);
  }
  if (*id == 1107) {
    const auto value = finalGauge();
    return value ? supported(static_cast<double>(*value)) : unsupported<double>();
  }
  if (*id == 183 && previousScore && maximum && *maximum > 0) {
    return supported(static_cast<double>(*previousScore) / *maximum);
  }
  if ((*id == 122 || *id == 135 || *id == 157) && targetScore &&
      maximum && *maximum > 0) {
    return supported(static_cast<double>(*targetScore) / *maximum);
  }
  return unsupported<double>();
}

SkinPropertyLookup<std::string_view> ResultSkinStateBridge::stringProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = [&]() -> std::optional<int> {
    if (const auto *name = std::get_if<std::string>(&selector.value)) {
      return beatorajaStringPropertySelector(*name);
    }
    return integerSelector(selector);
  }();
  if (!id) return unsupported<std::string_view>();
  const auto *configuration = data_.configuration
                                  ? std::addressof(*data_.configuration)
                                  : nullptr;
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
    stringValue_ = !data_.courseTitle.empty()
                       ? data_.courseTitle
                       : (data_.presentation ? data_.presentation->title
                                             : (data_.meta ? data_.meta->Title : ""));
    break;
  case 12:
    stringValue_ = !data_.courseTitle.empty()
                       ? data_.courseTitle
                       : (data_.presentation ? data_.presentation->title
                                             : (data_.meta ? data_.meta->Title : ""));
    if (data_.courseTitle.empty() && data_.meta != nullptr &&
        !data_.meta->SubTitle.empty()) {
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
                       : (data_.presentation && data_.presentation->artist
                              ? *data_.presentation->artist
                              : "");
    break;
  case 50:
    stringValue_ = data_.skinName;
    break;
  case 51:
    stringValue_ = data_.skinAuthor;
    break;
  case 30:
  case 1000:
    stringValue_.clear();
    break;
  case 60:
    stringValue_ = configuration ? configuration->modeFilterName : "";
    break;
  case 61:
    stringValue_ = configuration ? configuration->sortId : "";
    break;
  case 62:
    stringValue_ = configuration ? configuration->difficultyFilterName : "";
    break;
  case 86:
    stringValue_ = configuration ? configuration->chartReplicationMode : "";
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
    stringValue_ = data_.tableLevel + data_.tableName;
    break;
  case 1020:
    stringValue_ = configuration ? configuration->irName : "";
    break;
  case 1021:
    stringValue_ = configuration ? configuration->irAccountName : "";
    break;
  case 119:
  case 149:
  case 220:
    stringValue_.clear();
    break;
  case 1030:
    stringValue_ = data_.meta ? data_.meta->MD5 : data_.chartMd5;
    break;
  case 1031:
    stringValue_ = data_.meta ? data_.meta->SHA256 : data_.chartSha256;
    break;
  case 120: case 121: case 122: case 123: case 124:
  case 125: case 126: case 127: case 128: case 129: {
    const std::size_t index = static_cast<std::size_t>(*id - 120);
    stringValue_ = index < data_.irRankingEntries.size()
                       ? data_.irRankingEntries[index].playerName
                       : "";
    break;
  }
  case 150: case 151: case 152: case 153: case 154:
  case 155: case 156: case 157: case 158: case 159: {
    const std::size_t index = static_cast<std::size_t>(*id - 150);
    stringValue_ = index < data_.courseTitles.size()
                       ? data_.courseTitles[index]
                       : "";
    break;
  }
  case 200: case 201: case 202: case 203: case 204:
  case 205: case 206: case 207: case 208: case 209:
  case 210: case 211: case 212: case 213: case 214:
  case 215: case 216: case 217: case 218: case 219: {
    if (configuration == nullptr) {
      stringValue_.clear();
      break;
    }
    const auto names = beatorajaTargetNeighbourNames(
        configuration->skinTargetId, configuration->skinTargetList);
    stringValue_ = names[static_cast<std::size_t>(*id - 200)];
    break;
  }
  default:
    if ((*id >= 40 && *id <= 49) || (*id >= 100 && *id <= 119) ||
        (*id >= 240 && *id <= 283) ||
        (*id >= 1040 && *id <= 1095)) {
      stringValue_.clear();
      break;
    }
    return unsupported<std::string_view>();
  }
  return supported<std::string_view>(stringValue_);
}

SkinPropertyLookup<SkinRuntimeOffset>
ResultSkinStateBridge::offsetProperty(int id) {
  if (id < 0 || id > SkinCommandPolicy::maximumBeatorajaOffsetId) {
    return unsupported<SkinRuntimeOffset>();
  }
  if (configuration_ != nullptr) {
    if (const auto found = configuration_->offsetsById.find(id);
        found != configuration_->offsetsById.end()) {
      return supported(skinRuntimeOffset(found->second));
    }
  }
  return supported(SkinRuntimeOffset{});
}

std::int64_t ResultSkinStateBridge::timerProperty(
    const SkinBuiltinPropertySelector &selector) {
  const auto id = integerSelector(selector);
  constexpr auto kTimerOff = std::numeric_limits<std::int64_t>::min();
  if (!id) return kTimerOff;
  if (const auto custom = customTimerValues_.find(*id);
      custom != customTimerValues_.end()) {
    return custom->second;
  }
  // Result timers are timestamps, not elapsed values. MusicResult and
  // CourseResult start their graph/update timers at result-scene origin.
  if (*id == 150 || *id == 151 || *id == 152) return 0;
  if (*id == 1) {
    const auto start = static_cast<std::int64_t>(
        std::max(0, model_ != nullptr ? model_->timing.inputMillis : 0)) * 1'000;
    return elapsedMillis_ * 1'000 >= start ? start : kTimerOff;
  }
  return kTimerOff;
}

void ResultSkinStateBridge::setCustomTimer(int id, std::int64_t value) {
  customTimerValues_.insert_or_assign(id, value);
}

std::span<const SkinProjectedNoteView>
ResultSkinStateBridge::projectedNotes() const noexcept { return {}; }
std::span<const SkinProjectedLongNoteView>
ResultSkinStateBridge::projectedLongNotes() const noexcept { return {}; }
std::span<const SkinProjectedLineView>
ResultSkinStateBridge::projectedLines() const noexcept { return {}; }

SkinGameplayGraphStateView
ResultSkinStateBridge::gameplayGraphState() const noexcept {
  SkinGameplayGraphStateView result =
      skinGameplayGraphStateView(data_.gameplayGraph);
  const auto type = resultGaugeType(data_);
  const GaugeProfile profile = data_.state ? data_.state->gaugeProfile
                                           : GaugeProfile::Standard;
  if (!data_.courseResult) {
    result.timingDistribution = data_.timingDistribution;
    result.timingDistributionCenter = data_.timingDistributionCenter;
    result.timingDistributionAverageMillis = data_.timingAverageMillis;
    result.timingDistributionStandardDeviationMillis =
        data_.timingStandardDeviationMillis;
  }
  if (!result.gaugeHistory.empty() || !gaugeHistory_.empty()) {
    if (result.gaugeHistory.empty()) {
      result.gaugeHistory = gaugeHistory_;
      result.gaugeRevision = gaugeRevision_;
    }
    if (!result.gaugeSupported && type) {
      result.gaugeType = gaugeTypeIndex(*type);
      result.gaugeMinimum = gaugeMinimumValue(*type, profile);
      result.gaugeMaximum = gaugeMaximumValue(*type, profile);
      result.gaugeBorder = gaugeBorderValue(*type, profile);
      result.gaugeSupported = true;
    }
  }
  if (!result.normalDistribution.empty() || !result.judgementDistribution.empty() ||
      !result.earlyLateDistribution.empty() || !result.bpmSeries.empty() ||
      !result.gaugeHistory.empty() || !result.judgeWindows.empty() ||
      !result.timingDistribution.empty() ||
      !result.recentJudgeTimingsMillis.empty()) {
    return result;
  }
  if (gaugeHistory_.empty()) return {};
  if (!type) {
    return {.gaugeHistory = gaugeHistory_, .gaugeRevision = gaugeRevision_};
  }
  return {.gaugeHistory = gaugeHistory_,
          .gaugeType = gaugeTypeIndex(*type),
          .gaugeMinimum = gaugeMinimumValue(*type, profile),
          .gaugeMaximum = gaugeMaximumValue(*type, profile),
          .gaugeBorder = gaugeBorderValue(*type, profile),
          .gaugeSupported = true,
          .gaugeRevision = gaugeRevision_};
}

SkinGaugeStateView ResultSkinStateBridge::gaugeState() const noexcept {
  const auto value = finalGauge();
  const auto type = resultGaugeType(data_);
  if (!value || !type) return {};
  const GaugeProfile profile = data_.state ? data_.state->gaugeProfile
                                           : GaugeProfile::Standard;
  return {.supported = true,
          .value = *value,
          .gaugeType = gaugeTypeIndex(*type),
          .minimum = gaugeMinimumValue(*type, profile),
          .maximum = gaugeMaximumValue(*type, profile),
          .border = gaugeBorderValue(*type, profile)};
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
