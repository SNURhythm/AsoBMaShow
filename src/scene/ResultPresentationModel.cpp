#include "../i18n/Localization.h"
#include "ResultPresentationModel.h"

#include "../ResultContracts.h"
#include "../CoursePlaySession.h"
#include "../ScoreRankUtils.h"
#include "../view/ClearLampColors.h"
#include "../view/UiTheme.h"
#include "ResultGaugeHistory.h"
#include "play/Judge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace {
std::string formatNumber(double value, int decimals = 0) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(decimals) << value;
  return output.str();
}

std::string formatSignedDelta(int delta) {
  return (delta >= 0 ? "+" : "") + std::to_string(delta);
}

std::string formatScoreRate(int score, int maxScore) {
  return formatNumber(static_cast<double>(score) * 100.0 /
                          static_cast<double>(maxScore),
                      2) +
         "%";
}

std::string formatGauge(float gauge) {
  return formatNumber(static_cast<double>(gauge), 1) + "%";
}

std::string formatDuration(long long micros) {
  if (micros <= 0) {
    return "0:00";
  }
  const long long seconds = micros / 1'000'000LL;
  const long long minutes = seconds / 60LL;
  const long long remaining = seconds % 60LL;
  std::ostringstream output;
  output << minutes << ":" << std::setw(2) << std::setfill('0') << remaining;
  return output.str();
}

std::string formatBpmValue(double value) {
  if (std::abs(value - std::round(value)) < 0.01) {
    return std::to_string(static_cast<long long>(std::llround(value)));
  }
  return formatNumber(value, 2);
}

std::string formatBpm(const bms_parser::ChartMeta &meta) {
  const double minimum = meta.MinBpm > 0.0 ? meta.MinBpm : meta.Bpm;
  const double maximum = meta.MaxBpm > 0.0 ? meta.MaxBpm : meta.Bpm;
  if (minimum > 0.0 && maximum > 0.0 && std::abs(maximum - minimum) > 0.01) {
    return formatBpmValue(minimum) + "-" + formatBpmValue(maximum) + "(" +
           formatBpmValue(meta.Bpm) + ")";
  }
  return formatBpmValue(meta.Bpm);
}

std::pair<std::string, int> nextRankTarget(int score, int maxScore) {
  if (maxScore <= 0) {
    return {"-", 0};
  }
  struct Threshold {
    const char *label;
    int numerator;
  };
  constexpr Threshold thresholds[] = {{"E", 2},   {"D", 3},  {"C", 4},
                                      {"B", 5},   {"A", 6},  {"AA", 7},
                                      {"AAA", 8}, {"MAX", 9}};
  for (const auto &threshold : thresholds) {
    const int target =
        static_cast<int>(std::ceil(maxScore * threshold.numerator / 9.0));
    if (score < target) {
      return {threshold.label, score - target};
    }
  }
  return {"MAX", 0};
}

std::optional<std::string> nonEmptyText(std::string_view value) {
  return value.empty() ? std::nullopt
                       : std::optional<std::string>(std::string(value));
}

std::optional<std::string>
nonEmptyOptional(const std::optional<std::string> &value) {
  return value && !value->empty() ? value : std::nullopt;
}

bool knownLampRank(int rank) {
  constexpr std::array ranks{
      kClearTypeFailedRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeLightAssistedEasyClearRank,
      kClearTypeEasyClearRank,
      kClearTypeNormalClearRank,
      kClearTypeHardClearRank,
      kClearTypeExHardClearRank,
      kClearTypeFullComboRank,
  };
  return std::ranges::find(ranks, rank) != ranks.end();
}

std::optional<std::string> playtypeForGame(std::string_view game) {
  if (game == "bms-7k") {
    return "7K";
  }
  if (game == "bms-14k") {
    return "14K";
  }
  return std::nullopt;
}

std::string localPlaytype(int keyMode) {
  return keyMode > 0 ? std::to_string(keyMode) + "K" : std::string{};
}

int countFor(const RhythmState &state, Judgement judgement) {
  const auto found = state.judgeCount.find(judgement);
  return found == state.judgeCount.end() ? 0 : found->second;
}

JudgementFastSlowCount timingFor(const RhythmState &state,
                                 Judgement judgement) {
  const auto found = state.judgementFastSlowCount.find(judgement);
  return found == state.judgementFastSlowCount.end() ? JudgementFastSlowCount{}
                                                     : found->second;
}

ResultJudgementRow localJudgementRow(const RhythmState &state,
                                     std::string label, Judgement judgement,
                                     Color color) {
  const auto timing = timingFor(state, judgement);
  return {.label = std::move(label),
          .color = color,
          .total = countFor(state, judgement),
          .early = timing.fast,
          .late = timing.slow};
}

struct RemoteJudgementSource {
  const char *label;
  Color color;
  const std::optional<int> *total;
  const std::optional<int> *early;
  const std::optional<int> *late;
};

std::vector<ResultJudgementRow>
remoteJudgementRows(const ir::IrRemoteScore &score) {
  if (!score.judgements.complete()) {
    return {};
  }
  const std::array sources{
      RemoteJudgementSource{"P-GREAT", ui_theme::cyan(),
                            &score.judgements.pGreat, &score.timing.earlyPGreat,
                            &score.timing.latePGreat},
      RemoteJudgementSource{"GREAT", ui_theme::lime(), &score.judgements.great,
                            &score.timing.earlyGreat, &score.timing.lateGreat},
      RemoteJudgementSource{"GOOD", ui_theme::amber(), &score.judgements.good,
                            &score.timing.earlyGood, &score.timing.lateGood},
      RemoteJudgementSource{"BAD", Color(255, 132, 96, 255),
                            &score.judgements.bad, &score.timing.earlyBad,
                            &score.timing.lateBad},
      RemoteJudgementSource{"POOR", ui_theme::coral(), &score.judgements.poor,
                            &score.timing.earlyPoor, &score.timing.latePoor},
  };

  std::vector<ResultJudgementRow> rows;
  rows.reserve(sources.size());
  for (const auto &source : sources) {
    ResultJudgementRow row{
        .label = source.label,
        .color = source.color,
        .total = **source.total,
    };
    if (source.early->has_value() && source.late->has_value()) {
      row.early = **source.early;
      row.late = **source.late;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

ResultInfoTile infoTile(ResultInfoTileKind kind, std::string label, std::string value,
                        std::optional<std::string> detail, Color accent) {
  return {.kind = kind,
          .label = std::move(label),
          .value = std::move(value),
          .detail = std::move(detail),
          .accent = accent};
}

void addOptionalMetadataTile(std::vector<ResultInfoTile> &tiles,
                             ResultInfoTileKind kind, std::string label,
                             const std::optional<std::string> &value,
                             Color accent) {
  if (value.has_value()) {
    tiles.push_back(infoTile(kind, std::move(label), *value, std::nullopt, accent));
  }
}

std::optional<std::string>
localDifficulty(const bms_parser::ChartMeta &meta,
                const ResultLocalPresentationOptions &options) {
  if (options.headerDifficultyLabelOverride.has_value()) {
    return options.headerDifficultyLabelOverride;
  }
  const int decimals =
      std::abs(meta.PlayLevel - std::round(meta.PlayLevel)) < 0.01 ? 0 : 1;
  const std::string level = "LV " + formatNumber(meta.PlayLevel, decimals);
  return options.difficultyLabel.empty()
             ? level
             : options.difficultyLabel + " / " + level;
}

ResultComparisonCard
localScoreComparison(int currentScore, int maxScore,
                     const ResultLocalPresentationOptions &options) {
  const bool hasPrevious = options.previousBest.has_value();
  const bool hasPacemaker = options.pacemaker.has_value();
  const int previousScore = hasPrevious ? options.previousBest->score : 0;
  const int previousMax = hasPrevious && options.previousBest->maxScore > 0
                              ? options.previousBest->maxScore
                              : maxScore;
  const int delta = hasPacemaker
                        ? options.pacemaker->delta
                        : (hasPrevious ? currentScore - previousScore : 0);

  ResultComparisonValue target;
  if (hasPacemaker) {
    target = {.label = options.pacemaker->label,
              .value = std::to_string(options.pacemaker->targetScore),
              .detail = options.pacemaker->usesReplayProgression
                            ? i18n::tr("result.summary.pacemaker_ghost.badge")
                            : i18n::tr("result.summary.pacemaker.badge"),
              .accent = ui_theme::cyan()};
  } else {
    target = {.label = i18n::tr("result.summary.score.best.badge"),
              .value = hasPrevious ? std::to_string(previousScore) : "NO PLAY",
              .detail = hasPrevious ? score_rank::displayLabelForScore(
                                          previousScore, previousMax)
                                    : std::string{},
              .accent = hasPrevious ? ui_theme::textPrimary()
                                    : ui_theme::textMuted()};
  }

  return {
      .title = hasPacemaker ? i18n::tr("result.summary.pacemaker.badge") : i18n::tr("result.summary.score_comparison.badge"),
      .target = std::move(target),
      .current = {.label = i18n::tr("result.summary.score.current.badge"),
                  .value = std::to_string(currentScore),
                  .detail = "MAX " + std::to_string(maxScore),
                  .accent = ui_theme::textPrimary()},
      .delta = (hasPacemaker || hasPrevious)
                   ? std::optional<std::string>(
                         std::string(hasPacemaker ? "PACEMAKER " : "DELTA ") +
                         formatSignedDelta(delta))
                   : std::optional<std::string>("DELTA --"),
      .deltaAccent = !(hasPacemaker || hasPrevious) ? ui_theme::textMuted()
                         : delta >= 0 ? ui_theme::lime() : ui_theme::coral(),
  };
}

ResultComparisonCard
localLampComparison(const RhythmState &state,
                    const ResultLocalPresentationOptions &options) {
  const ResultPreviousBestData *const previous =
      options.previousLampBest
          ? &*options.previousLampBest
          : options.previousBest ? &*options.previousBest : nullptr;
  const bool hasPrevious = previous != nullptr;
  const int previousRank =
      hasPrevious ? previous->clearType : kNoClearTypeRank;
  const int currentRank =
      options.currentClearRankOverride.value_or(state.getClearTypeRank());
  const std::string currentLabel =
      options.currentClearLabelOverride.value_or(state.getClearTypeLabel());

  return {
      .title = i18n::tr("result.summary.clear_lamp_comparison.badge"),
      .target =
          ResultComparisonValue{
              .label = i18n::tr("result.summary.lamp.best.badge"),
              .value = clearTypeRankToLabel(previousRank),
              .detail =
                  hasPrevious
                      ? "GAUGE " + formatGauge(previous->finalGauge)
                      : std::string{},
              .accent = hasPrevious ? clearLampColorForRank(previousRank)
                                    : ui_theme::textMuted()},
      .current = {.label = i18n::tr("result.summary.lamp.current.badge"),
                  .value = currentLabel,
                  .detail = "GAUGE " + formatGauge(state.currentGauge),
                  .accent = clearLampColorForRank(currentRank)},
  };
}

ResultComparisonCard
localComboComparison(const RhythmState &state,
                     const ResultLocalPresentationOptions &options) {
  const bool hasPrevious = options.previousBest.has_value();
  const int comboDelta =
      hasPrevious ? state.maxCombo - options.previousBest->maxCombo : 0;
  const int breakDelta =
      hasPrevious ? state.comboBreak - options.previousBest->comboBreak : 0;
  return {
      .title = i18n::tr("result.summary.combo_break_comparison.badge"),
      .target =
          ResultComparisonValue{
              .label = i18n::tr("result.summary.combo.best.badge"),
              .value = hasPrevious
                           ? std::to_string(options.previousBest->maxCombo)
                           : "NO PLAY",
              .detail = hasPrevious
                            ? "BREAK " + std::to_string(
                                             options.previousBest->comboBreak)
                            : std::string{},
              .accent = hasPrevious ? ui_theme::lime() : ui_theme::textMuted()},
      .current = {.label = i18n::tr("result.summary.combo.current.badge"),
                  .value = std::to_string(state.maxCombo),
                  .detail = "BREAK " + std::to_string(state.comboBreak),
                  .accent = ui_theme::lime()},
      .delta = hasPrevious ? std::optional<std::string>(
                                 "COMBO " + formatSignedDelta(comboDelta) +
                                 " / BREAK " + formatSignedDelta(breakDelta))
                           : std::optional<std::string>("COMBO -- / BREAK --"),
      .deltaAccent = !hasPrevious ? ui_theme::textMuted()
                         : comboDelta >= 0 && breakDelta <= 0
                               ? ui_theme::lime() : ui_theme::amber(),
  };
}
} // namespace

std::string_view resultInfoTileSemanticName(ResultInfoTileKind kind) noexcept {
  switch (kind) {
  case ResultInfoTileKind::NextGrade: return "NEXT GRADE";
  case ResultInfoTileKind::TotalNotes: return "TOTAL NOTES";
  case ResultInfoTileKind::Bpm: return "BPM";
  case ResultInfoTileKind::JudgeRank: return "JUDGE RANK";
  case ResultInfoTileKind::Duration: return "DURATION";
  case ResultInfoTileKind::PlayMode: return "PLAY MODE";
  case ResultInfoTileKind::BadPoints: return "BP";
  case ResultInfoTileKind::Service: return "SERVICE";
  case ResultInfoTileKind::Client: return "CLIENT";
  case ResultInfoTileKind::InputDevice: return "INPUT DEVICE";
  case ResultInfoTileKind::Random: return "RANDOM";
  case ResultInfoTileKind::GaugeType: return "GAUGE TYPE";
  case ResultInfoTileKind::Level: return "LEVEL";
  case ResultInfoTileKind::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool hasGradeCard(const ResultPresentationModel &model) noexcept {
  return model.score.has_value() && model.maxScore.has_value() &&
         *model.maxScore > 0;
}

bool hasJudgementCard(const ResultPresentationModel &model) noexcept {
  return model.judgements.size() >= 5;
}

bool hasComboBreakCard(const ResultPresentationModel &model) noexcept {
  return model.maxCombo.has_value() && model.comboBreak.has_value();
}

bool hasGaugeCard(const ResultPresentationModel &model) noexcept {
  return std::ranges::any_of(
      model.gaugeSeries, result_gauge_history::hasPresentPoints);
}

std::optional<ResultGradeCard> gradeCard(const ResultPresentationModel &model) {
  if (!hasGradeCard(model)) {
    return std::nullopt;
  }
  const std::string grade =
      score_rank::displayLabelForScore(*model.score, *model.maxScore);
  return ResultGradeCard{
      .grade = grade,
      .rate = formatScoreRate(*model.score, *model.maxScore),
      .accent = ui_theme::scoreRankColor(grade),
  };
}

std::vector<ResultJudgementRow>
timingRows(const ResultPresentationModel &model) {
  std::vector<ResultJudgementRow> rows;
  for (const auto &row : model.judgements) {
    if (row.early.has_value() && row.late.has_value()) {
      rows.push_back(row);
    }
  }
  return rows;
}

ResultPresentationModel
makeLocalResultPresentation(const bms_parser::ChartMeta &meta,
                            const RhythmState &state,
                            ResultLocalPresentationOptions options) {
  ResultPresentationModel model;
  model.title = meta.Title;
  model.artist = meta.Artist;
  model.difficulty = localDifficulty(meta, options);
  model.playtype = nonEmptyText(localPlaytype(meta.KeyMode));
  model.gaugeType = gaugeDisplayShortLabel(state.gaugeType, state.gaugeProfile);
  model.score = state.getScore();
  model.maxScore =
      result_contract::maximumScoreForNotes(meta.TotalNotes).value_or(0);
  model.lampRank =
      options.currentClearRankOverride.value_or(state.getClearTypeRank());
  model.finalGauge = state.currentGauge;
  model.maxCombo = state.maxCombo;
  model.comboBreak = state.comboBreak;
  model.scoreComparison =
      localScoreComparison(*model.score, *model.maxScore, options);
  model.lampComparison = localLampComparison(state, options);
  model.comboComparison = localComboComparison(state, options);

  const auto nextRank = nextRankTarget(*model.score, *model.maxScore);
  const std::optional<std::string> longNotes =
      meta.TotalLongNotes > 0 ? std::optional<std::string>(
                                    std::to_string(meta.TotalLongNotes) + " LN")
                              : std::nullopt;
  model.infoTiles = {
      infoTile(ResultInfoTileKind::NextGrade, i18n::tr("result.summary.next_grade.badge"), nextRank.first, formatSignedDelta(nextRank.second),
               ui_theme::amber()),
      infoTile(ResultInfoTileKind::TotalNotes, i18n::tr("result.summary.total_notes.badge"), std::to_string(meta.TotalNotes), longNotes,
               ui_theme::lime()),
      infoTile(ResultInfoTileKind::Bpm, "BPM", formatBpm(meta), std::nullopt, ui_theme::amber()),
      infoTile(ResultInfoTileKind::JudgeRank, i18n::tr("result.summary.judge_rank.badge"), Judge::getRankDescription(meta.Rank), std::nullopt,
               ui_theme::cyan()),
      infoTile(ResultInfoTileKind::Duration, i18n::tr("result.summary.duration.badge"), formatDuration(meta.PlayLength),
               meta.TotalLength > meta.PlayLength
                   ? std::optional<std::string>(
                         "BGA " + formatDuration(meta.TotalLength))
                   : std::nullopt,
               ui_theme::violetActionHover()),
      infoTile(ResultInfoTileKind::PlayMode, i18n::tr("result.summary.play_mode.badge"),
               options.playModeLabel.empty() ? "NORMAL" : options.playModeLabel,
               nonEmptyText(options.laneOrderLabel), ui_theme::amber()),
  };

  model.judgements = {
      localJudgementRow(state, "PGREAT", PGreat, ui_theme::cyan()),
      localJudgementRow(state, "GREAT", Great, ui_theme::lime()),
      localJudgementRow(state, "GOOD", Good, ui_theme::amber()),
      localJudgementRow(state, "BAD", Bad, Color(255, 132, 96, 255)),
      localJudgementRow(state, "POOR", Poor, ui_theme::coral()),
      localJudgementRow(state, "KPOOR", Kpoor, Color(255, 78, 102, 255)),
  };
  model.fast = state.fastCount;
  model.slow = state.slowCount;

  model.gaugeSeries = result_gauge_history::seriesFor(state);
  model.timingAnalytics = std::move(options.timingAnalytics);
  return model;
}

ResultPresentationModel
makeRemoteResultPresentation(const ir::IrRemoteScore &score) {
  ResultPresentationModel model;
  model.title = score.title;
  model.artist = nonEmptyText(score.artist);
  model.difficulty = nonEmptyOptional(score.difficulty);
  model.playtype = playtypeForGame(score.game);
  model.achievedAtUnixMillis = score.timeAchievedUnixMillis;
  model.service = nonEmptyText(score.service);
  model.client = nonEmptyOptional(score.client);
  model.inputDevice = nonEmptyOptional(score.inputDevice);
  model.random = nonEmptyOptional(score.random);
  model.gaugeType = nonEmptyOptional(score.gauge);
  model.score = score.score;
  const auto maximumScore =
      result_contract::maximumScoreForNotes(score.noteCount);
  if (score.noteCount > 0 && maximumScore) {
    model.maxScore = *maximumScore;
    model.scoreComparison = ResultComparisonCard{
        .title = i18n::tr("result.summary.score.badge"),
        .current = {.label = i18n::tr("result.summary.remote.current.badge"),
                    .value = std::to_string(score.score),
                    .detail = "MAX " + std::to_string(*model.maxScore),
                    .accent = ui_theme::textPrimary()},
    };
  }

  if (knownLampRank(score.lampRank)) {
    model.lampRank = score.lampRank;
    model.lampComparison = ResultComparisonCard{
        .title = i18n::tr("result.summary.clear_lamp.badge"),
        .current = {.label = i18n::tr("result.summary.remote.current.badge"),
                    .value = clearTypeRankToLabel(score.lampRank),
                    .detail = score.finalGauge
                                  ? "GAUGE " + formatGauge(*score.finalGauge)
                                  : std::string{},
                    .accent = clearLampColorForRank(score.lampRank)},
    };
  }

  model.finalGauge = score.finalGauge;
  model.maxCombo = score.maxCombo;
  model.comboBreak = score.badPoints;
  model.badPoints = score.badPoints;
  if (hasComboBreakCard(model)) {
    model.comboComparison = ResultComparisonCard{
        .title = i18n::tr("result.summary.combo_break.badge"),
        .current = {.label = i18n::tr("result.summary.remote.current.badge"),
                    .value = std::to_string(*model.maxCombo),
                    .detail = "BREAK " + std::to_string(*model.comboBreak),
                    .accent = ui_theme::lime()},
    };
  }

  if (score.noteCount > 0) {
    model.infoTiles.push_back(infoTile(ResultInfoTileKind::TotalNotes, i18n::tr("result.summary.total_notes.badge"),
                                       std::to_string(score.noteCount),
                                       std::nullopt, ui_theme::lime()));
  }
  if (score.badPoints.has_value()) {
    model.infoTiles.push_back(infoTile(ResultInfoTileKind::BadPoints, "BP", std::to_string(*score.badPoints),
                                       std::nullopt, ui_theme::coral()));
  }
  addOptionalMetadataTile(model.infoTiles, ResultInfoTileKind::Service, "SERVICE", model.service,
                          ui_theme::cyan());
  addOptionalMetadataTile(model.infoTiles, ResultInfoTileKind::Client, "CLIENT", model.client,
                          ui_theme::cyan());
  addOptionalMetadataTile(model.infoTiles, ResultInfoTileKind::InputDevice, i18n::tr("result.summary.input_device.badge"), model.inputDevice,
                          ui_theme::amber());
  addOptionalMetadataTile(model.infoTiles, ResultInfoTileKind::Random, "RANDOM", model.random,
                          ui_theme::amber());
  addOptionalMetadataTile(model.infoTiles, ResultInfoTileKind::GaugeType, i18n::tr("result.summary.gauge_type.badge"), model.gaugeType,
                          ui_theme::lime());
  addOptionalMetadataTile(model.infoTiles, ResultInfoTileKind::Level, i18n::tr("result.summary.level.badge"),
                          nonEmptyOptional(score.level), ui_theme::amber());

  model.judgements = remoteJudgementRows(score);
  model.fast = score.fast;
  model.slow = score.slow;

  ResultGaugeSeries gaugeSeries{
        .points = score.gaugeHistory,
        .label = model.gaugeType,
        .clearRank = model.lampRank,
  };
  if (result_gauge_history::hasPresentPoints(gaugeSeries)) {
    model.gaugeSeries.push_back(std::move(gaugeSeries));
  }
  model.readOnlyIrUploaded = true;
  return model;
}

std::vector<ResultCourseStagePresentation>
makeCourseStagePresentations(const CoursePlaySession &session) {
  std::vector<ResultCourseStagePresentation> stages;
  stages.reserve(session.entries.size());
  for (std::size_t index = 0; index < session.entries.size(); ++index) {
    ResultCourseStagePresentation stage{.title = session.entries[index].meta.Title};
    if (index < session.completedResults.size()) {
      const auto &completed = session.completedResults[index];
      stage.result = makeLocalResultPresentation(completed.meta, completed.state, {});
      // Combo carries across charts in a course; it is not a stage-only record.
      stage.result->maxCombo.reset();
      stage.result->comboComparison.reset();
      stage.result->badPoints = countFor(completed.state, Bad) +
                                countFor(completed.state, Poor) +
                                countFor(completed.state, Kpoor);
    }
    stages.push_back(std::move(stage));
  }
  return stages;
}
