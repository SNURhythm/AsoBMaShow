#include "ResultPersistenceModel.h"

#include "BmsMetadataText.h"
#include "FileChecksum.h"
#include "Utils.h"
#include "Uuid.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace result_persistence {
namespace {

constexpr int kMaximumCourseEntries = 256;

using asobmshow::bms_metadata::normalizedHash;

class CanonicalEncoder {
public:
  void boolean(bool value) {
    bytes_.push_back(value ? std::byte{1} : std::byte{0});
  }

  template <typename Integer>
    requires(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>)
  void integer(Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t offset = 0; offset < sizeof(Unsigned); ++offset) {
      const std::size_t shift = (sizeof(Unsigned) - offset - 1U) * 8U;
      bytes_.push_back(static_cast<std::byte>((bits >> shift) &
                                              static_cast<Unsigned>(0xffU)));
    }
  }

  template <typename Enum>
    requires std::is_enum_v<Enum>
  void enumeration(Enum value) {
    integer(static_cast<std::int32_t>(value));
  }

  void float32(float value) { integer(std::bit_cast<std::uint32_t>(value)); }
  void float64(double value) { integer(std::bit_cast<std::uint64_t>(value)); }

  void string(std::string_view value) {
    integer(static_cast<std::uint64_t>(value.size()));
    const auto raw = std::as_bytes(std::span(value.data(), value.size()));
    bytes_.insert(bytes_.end(), raw.begin(), raw.end());
  }

  template <typename Value, typename Append>
  void optional(const std::optional<Value> &value, Append append) {
    boolean(value.has_value());
    if (value.has_value()) {
      append(*value);
    }
  }

  template <typename Value, typename Append>
  void vector(const std::vector<Value> &values, Append append) {
    integer(static_cast<std::uint64_t>(values.size()));
    for (const Value &value : values) {
      append(value);
    }
  }

  [[nodiscard]] std::string finish() const {
    file_checksum::Sha256 hash;
    hash.update(bytes_);
    return hash.finalHex();
  }

private:
  std::vector<std::byte> bytes_;
};

void appendProvenance(CanonicalEncoder &encoder,
                      const ScoreProvenance &provenance) {
  encoder.integer(static_cast<std::int32_t>(provenance.schemaVersion));
  encoder.string(provenance.ruleset.id);
  encoder.integer(static_cast<std::int32_t>(provenance.ruleset.version));
  encoder.string(provenance.ruleset.scoringModel);
  encoder.string(provenance.ruleset.judgementModel);
  encoder.string(provenance.ruleset.gaugeModel);
  encoder.vector(provenance.stages, [&](const ScoreStageProvenance &stage) {
    encoder.string(stage.chartMd5);
    encoder.string(stage.chartSha256);
    encoder.integer(static_cast<std::int32_t>(stage.longNoteMode));
    encoder.optional(stage.chartRandomSeed,
                     [&](std::uint64_t value) { encoder.integer(value); });
    encoder.optional(stage.chartRandomPrng,
                     [&](const std::string &value) { encoder.string(value); });
    encoder.vector(stage.chartRandomValues, [&](int value) {
      encoder.integer(static_cast<std::int32_t>(value));
    });
    if (provenance.schemaVersion >=
        ScoreProvenance::kDoublePlayOptionSchemaVersion) {
      encoder.optional(stage.doublePlayOption, [&](auto value) {
        encoder.enumeration(value);
      });
    }
    encoder.enumeration(stage.judgeRankSource);
    encoder.optional(stage.sourceJudgeRank, [&](int value) {
      encoder.integer(static_cast<std::int32_t>(value));
    });
    encoder.integer(static_cast<std::int32_t>(stage.totalNotes));
    encoder.optional(stage.authoredGaugeTotal,
                     [&](double value) { encoder.float64(value); });
    encoder.float64(stage.effectiveGaugeTotal);
    encoder.enumeration(stage.candidateSelection);
    encoder.vector(
        stage.effectiveJudgeWindows, [&](const JudgeWindowProvenance &window) {
          encoder.enumeration(window.context);
          encoder.enumeration(window.judgement);
          encoder.integer(static_cast<std::int64_t>(window.earlyMicros));
          encoder.integer(static_cast<std::int64_t>(window.lateMicros));
        });
  });
  encoder.integer(
      static_cast<std::int32_t>(gaugeTypeIndex(provenance.gaugeType)));
  encoder.enumeration(provenance.gaugeProfile);
  encoder.integer(static_cast<std::int32_t>(
      gaugeAutoShiftModeValue(provenance.gaugeAutoShift)));
  encoder.integer(static_cast<std::int32_t>(
      gaugeTypeIndex(provenance.gaugeAutoShiftLowerBound)));
  const auto appendPlayer = [&](const PlayerOptionProvenance &player) {
    encoder.string(player.option);
    encoder.optional(player.seed,
                     [&](std::int64_t value) { encoder.integer(value); });
  };
  appendPlayer(provenance.player1);
  appendPlayer(provenance.player2);
  encoder.string(provenance.assistOption);
  encoder.vector(provenance.inputDevices, [&](InputDeviceCategory device) {
    encoder.enumeration(device);
  });
  encoder.boolean(provenance.autoPlay);
  encoder.boolean(provenance.practice);
  encoder.boolean(provenance.clubMode);
  encoder.integer(static_cast<std::int32_t>(provenance.playback.percent));
  encoder.enumeration(provenance.playback.mode);
  encoder.integer(
      static_cast<std::int32_t>(provenance.judgeWindowScalePercent));
  encoder.optional(provenance.startingGaugePercent, [&](int value) {
    encoder.integer(static_cast<std::int32_t>(value));
  });
  encoder.enumeration(provenance.eligibility);
}

void appendScore(CanonicalEncoder &encoder, const ChartScoreWrite &score) {
  encoder.string(score.chartPath);
  encoder.string(score.chartMd5);
  encoder.string(score.chartSha256);
  encoder.string(score.chartTitle);
  encoder.string(score.chartArtist);
  encoder.integer(static_cast<std::int32_t>(score.longNoteMode));
  encoder.integer(static_cast<std::int32_t>(score.score));
  encoder.integer(static_cast<std::int32_t>(score.maxScore));
  encoder.integer(static_cast<std::int32_t>(score.maxCombo));
  encoder.integer(static_cast<std::int32_t>(score.comboBreak));
  encoder.integer(static_cast<std::int32_t>(score.pGreat));
  encoder.integer(static_cast<std::int32_t>(score.great));
  encoder.integer(static_cast<std::int32_t>(score.good));
  encoder.integer(static_cast<std::int32_t>(score.bad));
  encoder.integer(static_cast<std::int32_t>(score.poor));
  encoder.integer(static_cast<std::int32_t>(score.kPoor));
  encoder.integer(static_cast<std::int32_t>(score.fast));
  encoder.integer(static_cast<std::int32_t>(score.slow));
  encoder.float32(score.finalGauge);
  encoder.integer(static_cast<std::int32_t>(score.clearType));
  appendProvenance(encoder, score.provenance);
}

void appendTiming(CanonicalEncoder &encoder,
                  const std::optional<ChartJudgementTiming> &timing) {
  encoder.optional(timing, [&](const ChartJudgementTiming &value) {
    for (const auto &count : value.byJudgement) {
      encoder.integer(static_cast<std::int32_t>(count.fast));
      encoder.integer(static_cast<std::int32_t>(count.slow));
    }
  });
}

void appendGaugeHistory(CanonicalEncoder &encoder,
                        const std::vector<float> &history) {
  encoder.vector(history, [&](float value) { encoder.float32(value); });
}

bool lowerHex(std::string_view value, std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

bool knownClearRank(int value) noexcept {
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
  return std::ranges::find(ranks, value) != ranks.end();
}

bool knownGaugeType(GaugeType value) noexcept {
  const int index = static_cast<int>(value);
  return index >= 0 && index < static_cast<int>(kGaugeTypeCount);
}

bool knownGaugeProfile(GaugeProfile value) noexcept {
  const int index = static_cast<int>(value);
  return index >= static_cast<int>(GaugeProfile::Standard) &&
         index <= static_cast<int>(GaugeProfile::Standard24Keys);
}

bool knownGaugeAutoShiftMode(GaugeAutoShiftMode value) noexcept {
  const int index = static_cast<int>(value);
  return index >= static_cast<int>(GaugeAutoShiftMode::None) &&
         index <= static_cast<int>(GaugeAutoShiftMode::BestClear);
}

std::string legacyResultFingerprintV2(const PersistedChartResult &result) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-chart-result-v2");
  encoder.optional(result.attemptId,
                   [&](const std::string &value) { encoder.string(value); });
  appendScore(encoder, result.score);
  encoder.integer(static_cast<std::int32_t>(result.keyMode));
  appendGaugeHistory(encoder, result.adoptedGaugeHistory);
  appendTiming(encoder, result.judgementTiming);
  encoder.integer(result.playedAtUnixMillis);
  return encoder.finish();
}

std::string legacyResultFingerprintV2(const PersistedCourseResult &result) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-course-result-v2");
  encoder.optional(result.attemptId,
                   [&](const std::string &value) { encoder.string(value); });
  encoder.string(result.courseKey);
  encoder.integer(static_cast<std::int32_t>(result.legacyCourseId));
  encoder.string(result.courseName);
  encoder.string(result.courseGroupName);
  encoder.string(result.constraintJson);
  encoder.integer(static_cast<std::int32_t>(result.completedCharts));
  encoder.integer(static_cast<std::int32_t>(result.totalCharts));
  encoder.string(result.requestedPlayOption);
  encoder.string(result.assistOption);
  encoder.integer(
      static_cast<std::int32_t>(gaugeTypeIndex(result.initialGaugeType)));
  encoder.enumeration(result.gaugeProfile);
  encoder.integer(static_cast<std::int32_t>(
      gaugeAutoShiftModeValue(result.gaugeAutoShift)));
  encoder.integer(static_cast<std::int32_t>(
      gaugeTypeIndex(result.gaugeAutoShiftLowerBound)));
  encoder.integer(static_cast<std::int32_t>(result.longNoteMode));
  encoder.integer(static_cast<std::int32_t>(result.finalScore));
  encoder.integer(static_cast<std::int32_t>(result.maxScore));
  encoder.integer(static_cast<std::int32_t>(result.maxCombo));
  encoder.float32(result.finalGauge);
  encoder.integer(static_cast<std::int32_t>(result.clearType));
  appendProvenance(encoder, result.provenance);
  encoder.vector(result.stages, [&](const PersistedCourseStageResult &stage) {
    encoder.integer(static_cast<std::int32_t>(stage.stageIndex));
    appendScore(encoder, stage.score);
    encoder.integer(static_cast<std::int32_t>(stage.keyMode));
    appendGaugeHistory(encoder, stage.adoptedGaugeHistory);
    appendTiming(encoder, stage.judgementTiming);
  });
  encoder.integer(result.playedAtUnixMillis);
  return encoder.finish();
}

std::string legacyResultFingerprintV3(const PersistedCourseResult &result) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-course-result-v3");
  encoder.optional(result.attemptId,
                   [&](const std::string &value) { encoder.string(value); });
  encoder.string(result.courseKey);
  encoder.integer(static_cast<std::int32_t>(result.legacyCourseId));
  encoder.string(result.courseName);
  encoder.string(result.courseGroupName);
  encoder.string(result.constraintJson);
  encoder.integer(static_cast<std::int32_t>(result.completedCharts));
  encoder.integer(static_cast<std::int32_t>(result.totalCharts));
  encoder.string(result.requestedPlayOption);
  encoder.string(result.assistOption);
  encoder.integer(
      static_cast<std::int32_t>(gaugeTypeIndex(result.initialGaugeType)));
  encoder.enumeration(result.gaugeProfile);
  encoder.integer(static_cast<std::int32_t>(
      gaugeAutoShiftModeValue(result.gaugeAutoShift)));
  encoder.integer(static_cast<std::int32_t>(
      gaugeTypeIndex(result.gaugeAutoShiftLowerBound)));
  encoder.integer(static_cast<std::int32_t>(result.longNoteMode));
  encoder.integer(static_cast<std::int32_t>(result.finalScore));
  encoder.integer(static_cast<std::int32_t>(result.maxScore));
  encoder.integer(static_cast<std::int32_t>(result.maxCombo));
  encoder.float32(result.finalGauge);
  encoder.integer(static_cast<std::int32_t>(result.clearType));
  appendProvenance(encoder, result.provenance);
  encoder.vector(result.stages, [&](const PersistedCourseStageResult &stage) {
    encoder.integer(static_cast<std::int32_t>(stage.stageIndex));
    appendScore(encoder, stage.score);
    encoder.integer(static_cast<std::int32_t>(stage.keyMode));
    encoder.enumeration(stage.adoptedGaugeType);
    appendGaugeHistory(encoder, stage.adoptedGaugeHistory);
    appendTiming(encoder, stage.judgementTiming);
  });
  encoder.integer(result.playedAtUnixMillis);
  return encoder.finish();
}

bool hasLegacyAdoptedGauge(const PersistedChartResult &result) noexcept {
  return result.adoptedGaugeType == result.score.provenance.gaugeType;
}

bool hasLegacyAdoptedGauges(const PersistedCourseResult &result) noexcept {
  return std::ranges::all_of(result.stages, [](const auto &stage) {
    return stage.adoptedGaugeType == stage.score.provenance.gaugeType;
  });
}

bool hasMigrationBackfilledEntryFacts(
    const PersistedCourseResult &result) noexcept {
  if (result.entryFacts.size() !=
      static_cast<std::size_t>(result.totalCharts)) {
    return false;
  }
  for (std::size_t index = 0; index < result.entryFacts.size(); ++index) {
    const auto &facts = result.entryFacts[index];
    if (facts.playLengthMicros != 0 ||
        (index >= result.stages.size() && facts.totalNotes != 0)) {
      return false;
    }
  }
  return true;
}

bool canonicalCourseKey(std::string_view value) noexcept {
  constexpr std::string_view prefix = "course:v1:";
  return value.starts_with(prefix) && value.size() == prefix.size() + 64U &&
         lowerHex(value.substr(prefix.size()), 64);
}

int judgementCount(const RhythmState &state, Judgement judgement) {
  const auto found = state.judgeCount.find(judgement);
  return found == state.judgeCount.end() ? 0 : found->second;
}

ChartJudgementTiming captureChartJudgementTiming(const RhythmState &state) {
  ChartJudgementTiming timing;
  for (int index = 0; index < JudgementCount; ++index) {
    const auto judgement = static_cast<Judgement>(index);
    const auto found = state.judgementFastSlowCount.find(judgement);
    if (found != state.judgementFastSlowCount.end()) {
      timing.byJudgement[static_cast<std::size_t>(index)] = found->second;
    }
  }
  return timing;
}

bool validateScore(const ChartScoreWrite &score, std::string &diagnostic) {
  if (!hasProjectableChartIdentity(score)) {
    diagnostic = "chart identity is not projectable";
    return false;
  }
  const std::array counts{score.score,      score.maxScore, score.maxCombo,
                          score.comboBreak, score.pGreat,   score.great,
                          score.good,       score.bad,      score.poor,
                          score.kPoor,      score.fast,     score.slow};
  if (std::ranges::any_of(counts, [](int value) { return value < 0; }) ||
      score.longNoteMode < 0) {
    diagnostic = "score counters must not be negative";
    return false;
  }
  if (score.maxScore <= 0 || (score.maxScore % 2) != 0 ||
      score.score > score.maxScore || score.maxCombo > score.maxScore / 2 ||
      static_cast<std::int64_t>(score.pGreat) * 2LL + score.great !=
          score.score) {
    diagnostic = "score range is inconsistent with result counters";
    return false;
  }
  if (!std::isfinite(score.finalGauge) || score.finalGauge < 0.0F ||
      !knownClearRank(score.clearType)) {
    diagnostic = "score gauge or clear rank is invalid";
    return false;
  }
  std::string provenanceDiagnostic;
  if (!serializeValidatedScoreProvenance(score.provenance,
                                         provenanceDiagnostic)) {
    diagnostic = "result provenance is invalid";
    return false;
  }
  return true;
}

bool validateTiming(const ChartScoreWrite &score,
                    const std::optional<ChartJudgementTiming> &timing,
                    std::string &diagnostic) {
  if (!timing.has_value()) {
    return true;
  }
  const std::array totals{score.pGreat, score.great, score.good, score.bad,
                          score.kPoor,  score.poor,  0};
  std::int64_t fast = 0;
  std::int64_t slow = 0;
  for (int index = 0; index < JudgementCount; ++index) {
    const auto &count = timing->byJudgement[static_cast<std::size_t>(index)];
    if (count.fast < 0 || count.slow < 0) {
      diagnostic = "captured judgement timing must not be negative";
      return false;
    }
    const auto judgement = static_cast<Judgement>(index);
    if (judgement == Kpoor || judgement == None) {
      if (count.fast != 0 || count.slow != 0) {
        diagnostic = "KPOOR and NONE cannot have captured timing";
        return false;
      }
      continue;
    }
    if (static_cast<std::int64_t>(count.fast) + count.slow >
        totals[static_cast<std::size_t>(index)]) {
      diagnostic = "captured judgement timing exceeds result totals";
      return false;
    }
    fast += count.fast;
    slow += count.slow;
  }
  if (fast != score.fast || slow != score.slow) {
    diagnostic = "captured judgement timing disagrees with aggregate timing";
    return false;
  }
  return true;
}

bool validateResultFacts(const ChartScoreWrite &score, int keyMode,
                         const std::vector<float> &gaugeHistory,
                         const std::optional<ChartJudgementTiming> &timing,
                         std::string &diagnostic) {
  if (keyMode <= 0) {
    diagnostic = "chart key mode must be positive";
    return false;
  }
  if (!validateScore(score, diagnostic) ||
      !validateTiming(score, timing, diagnostic)) {
    return false;
  }
  if (std::ranges::any_of(gaugeHistory,
                          [](float value) { return !std::isfinite(value); })) {
    diagnostic = "adopted gauge history must be finite";
    return false;
  }
  return true;
}

} // namespace

bool hasProjectableChartIdentity(const ChartScoreWrite &score) noexcept {
  return lowerHex(score.chartSha256, 64) &&
         (score.chartMd5.empty() || lowerHex(score.chartMd5, 32));
}

ChartScoreWrite captureChartScoreWrite(const bms_parser::ChartMeta &meta,
                                       const RhythmState &state,
                                       const ScoreProvenance &provenance,
                                       int storageLongNoteMode) {
  return {
      .chartPath =
          Utils::GetStoragePathUtf8RelativeToDocuments(meta.BmsPath, "BMS/"),
      .chartMd5 = normalizedHash(meta.MD5),
      .chartSha256 = normalizedHash(meta.SHA256),
      .chartTitle = meta.Title,
      .chartArtist = meta.Artist,
      .longNoteMode = storageLongNoteMode,
      .score = state.getScore(),
      .maxScore = meta.TotalNotes * 2,
      .maxCombo = state.maxCombo,
      .comboBreak = state.comboBreak,
      .pGreat = judgementCount(state, PGreat),
      .great = judgementCount(state, Great),
      .good = judgementCount(state, Good),
      .bad = judgementCount(state, Bad),
      .poor = judgementCount(state, Poor),
      .kPoor = judgementCount(state, Kpoor),
      .fast = state.fastCount,
      .slow = state.slowCount,
      .finalGauge = state.currentGauge,
      .clearType = state.getClearTypeRank(),
      .provenance = provenance,
  };
}

std::optional<PersistedChartResult> capturePersistedChartResult(
    std::string attemptId, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &provenance,
    int storageLongNoteMode, std::int64_t playedAtUnixMillis,
    std::string &diagnostic) {
  diagnostic.clear();
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    diagnostic = "attempt ID is not a canonical version-4 UUID";
    return std::nullopt;
  }
  PersistedChartResult result{
      .attemptId = std::move(attemptId),
      .score =
          captureChartScoreWrite(meta, state, provenance, storageLongNoteMode),
      .keyMode = meta.KeyMode,
      .adoptedGaugeType = state.gaugeType,
      .adoptedGaugeHistory = state.gaugeHistoryFor(state.gaugeType),
      .judgementTiming = captureChartJudgementTiming(state),
      .playedAtUnixMillis = playedAtUnixMillis,
  };
  if (!validatePersistedChartResult(result, diagnostic)) {
    return std::nullopt;
  }
  result.resultFingerprint = resultFingerprint(result);
  return result;
}

PersistedCourseStageResult capturePersistedCourseStageResult(
    int stageIndex, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &provenance,
    int storageLongNoteMode) {
  return {
      .stageIndex = stageIndex,
      .score =
          captureChartScoreWrite(meta, state, provenance, storageLongNoteMode),
      .keyMode = meta.KeyMode,
      .adoptedGaugeType = state.gaugeType,
      .adoptedGaugeHistory = state.gaugeHistoryFor(state.gaugeType),
      .judgementTiming = captureChartJudgementTiming(state),
  };
}

bool validatePersistedChartResult(const PersistedChartResult &result,
                                  std::string &diagnostic) noexcept {
  try {
    diagnostic.clear();
    if (result.resultId < 0 || result.playedAtUnixMillis < 0) {
      diagnostic = "result identity or play time is invalid";
      return false;
    }
    if (result.attemptId.has_value() &&
        !uuid::isCanonicalLowerV4(*result.attemptId)) {
      diagnostic = "attempt ID is not a canonical version-4 UUID";
      return false;
    }
    if (!knownGaugeType(result.adoptedGaugeType)) {
      diagnostic = "adopted gauge type is invalid";
      return false;
    }
    if (!validateResultFacts(result.score, result.keyMode,
                             result.adoptedGaugeHistory, result.judgementTiming,
                             diagnostic)) {
      return false;
    }
    if (!result.resultFingerprint.empty() &&
        (!lowerHex(result.resultFingerprint, 64) ||
         (result.resultFingerprint != resultFingerprint(result) &&
          (!hasLegacyAdoptedGauge(result) ||
           result.resultFingerprint != legacyResultFingerprintV2(result))))) {
      diagnostic = "result fingerprint is malformed or inconsistent";
      return false;
    }
    return true;
  } catch (...) {
    diagnostic = "result validation failed";
    return false;
  }
}

std::string resultFingerprint(const PersistedChartResult &result) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-chart-result-v3");
  encoder.optional(result.attemptId,
                   [&](const std::string &value) { encoder.string(value); });
  appendScore(encoder, result.score);
  encoder.integer(static_cast<std::int32_t>(result.keyMode));
  encoder.enumeration(result.adoptedGaugeType);
  appendGaugeHistory(encoder, result.adoptedGaugeHistory);
  appendTiming(encoder, result.judgementTiming);
  encoder.integer(result.playedAtUnixMillis);
  return encoder.finish();
}

bool validatePersistedCourseResult(const PersistedCourseResult &result,
                                   std::string &diagnostic) noexcept {
  try {
    diagnostic.clear();
    if (result.resultId < 0 || result.playedAtUnixMillis < 0 ||
        result.legacyCourseId < 0) {
      diagnostic = "course result identity or play time is invalid";
      return false;
    }
    if (result.attemptId.has_value() &&
        !uuid::isCanonicalLowerV4(*result.attemptId)) {
      diagnostic = "course attempt ID is not a canonical version-4 UUID";
      return false;
    }
    if (!canonicalCourseKey(result.courseKey)) {
      diagnostic = "course identity is malformed";
      return false;
    }
    if (result.totalCharts <= 0 ||
        result.totalCharts > kMaximumCourseEntries ||
        result.completedCharts <= 0 ||
        result.completedCharts > result.totalCharts ||
        result.stages.size() !=
            static_cast<std::size_t>(result.completedCharts) ||
        result.entryFacts.size() !=
            static_cast<std::size_t>(result.totalCharts)) {
      diagnostic = "course completion prefix is malformed";
      return false;
    }
    for (const auto &facts : result.entryFacts) {
      if (facts.totalNotes < 0 || facts.playLengthMicros < 0) {
        diagnostic = "course entry facts are invalid";
        return false;
      }
    }
    if (!knownGaugeType(result.initialGaugeType) ||
        !knownGaugeProfile(result.gaugeProfile) ||
        !knownGaugeAutoShiftMode(result.gaugeAutoShift) ||
        !knownGaugeType(result.gaugeAutoShiftLowerBound)) {
      diagnostic = "course gauge configuration is invalid";
      return false;
    }
    if (result.longNoteMode < 0 || result.longNoteMode > 3 ||
        result.finalScore < 0 || result.maxScore <= 0 ||
        result.finalScore > result.maxScore || result.maxCombo < 0 ||
        !std::isfinite(result.finalGauge) || result.finalGauge < 0.0F ||
        !knownClearRank(result.clearType)) {
      diagnostic = "course result facts are invalid";
      return false;
    }
    std::int64_t stageScore = 0;
    std::int64_t courseMaxScore = 0;
    for (const auto &facts : result.entryFacts) {
      courseMaxScore += static_cast<std::int64_t>(facts.totalNotes) * 2;
    }
    for (std::size_t index = 0; index < result.stages.size(); ++index) {
      const auto &stage = result.stages[index];
      if (stage.stageIndex != static_cast<int>(index) ||
          !knownGaugeType(stage.adoptedGaugeType) ||
          !validateResultFacts(stage.score, stage.keyMode,
                               stage.adoptedGaugeHistory, stage.judgementTiming,
                               diagnostic)) {
        if (diagnostic.empty()) {
          diagnostic = "course stage ordering is malformed";
        }
        return false;
      }
      stageScore += stage.score.score;
      if (static_cast<std::int64_t>(result.entryFacts[index].totalNotes) * 2 !=
          stage.score.maxScore) {
        diagnostic = "course entry note count disagrees with stage result";
        return false;
      }
    }
    const bool hasCurrentBoundProvenance =
        result.provenance.schemaVersion >=
            ScoreProvenance::kDoublePlayOptionSchemaVersion &&
        result.provenance.eligibility !=
            ScoreEligibility::LegacyUnverified &&
        std::ranges::all_of(result.stages, [](const auto &stage) {
          return stage.score.provenance.schemaVersion >=
                     ScoreProvenance::kDoublePlayOptionSchemaVersion &&
                 stage.score.provenance.eligibility !=
                     ScoreEligibility::LegacyUnverified;
        });
    if (hasCurrentBoundProvenance) {
      std::vector<ScoreProvenance> stageProvenance;
      stageProvenance.reserve(result.stages.size());
      for (const auto &stage : result.stages) {
        stageProvenance.push_back(stage.score.provenance);
      }
      if (mergeCourseProvenance(stageProvenance) != result.provenance) {
        diagnostic =
            "course provenance disagrees with its ordered stage proofs";
        return false;
      }
    }
    if (stageScore != result.finalScore || courseMaxScore != result.maxScore) {
      diagnostic = "course totals disagree with entries or ordered stages";
      return false;
    }
    std::string provenanceDiagnostic;
    if (!serializeValidatedScoreProvenance(result.provenance,
                                           provenanceDiagnostic)) {
      diagnostic = "course provenance is invalid";
      return false;
    }
    if (!result.resultFingerprint.empty() &&
        (!lowerHex(result.resultFingerprint, 64) ||
         (result.resultFingerprint != resultFingerprint(result) &&
          (!hasMigrationBackfilledEntryFacts(result) ||
           result.resultFingerprint != legacyResultFingerprintV3(result)) &&
          (!hasMigrationBackfilledEntryFacts(result) ||
           !hasLegacyAdoptedGauges(result) ||
           result.resultFingerprint != legacyResultFingerprintV2(result))))) {
      diagnostic = "course result fingerprint is malformed or inconsistent";
      return false;
    }
    return true;
  } catch (...) {
    diagnostic = "course result validation failed";
    return false;
  }
}

std::string resultFingerprint(const PersistedCourseResult &result) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-course-result-v4");
  encoder.optional(result.attemptId,
                   [&](const std::string &value) { encoder.string(value); });
  encoder.string(result.courseKey);
  encoder.integer(static_cast<std::int32_t>(result.legacyCourseId));
  encoder.string(result.courseName);
  encoder.string(result.courseGroupName);
  encoder.string(result.constraintJson);
  encoder.integer(static_cast<std::int32_t>(result.completedCharts));
  encoder.integer(static_cast<std::int32_t>(result.totalCharts));
  encoder.string(result.requestedPlayOption);
  encoder.string(result.assistOption);
  encoder.integer(
      static_cast<std::int32_t>(gaugeTypeIndex(result.initialGaugeType)));
  encoder.enumeration(result.gaugeProfile);
  encoder.integer(static_cast<std::int32_t>(
      gaugeAutoShiftModeValue(result.gaugeAutoShift)));
  encoder.integer(static_cast<std::int32_t>(
      gaugeTypeIndex(result.gaugeAutoShiftLowerBound)));
  encoder.integer(static_cast<std::int32_t>(result.longNoteMode));
  encoder.integer(static_cast<std::int32_t>(result.finalScore));
  encoder.integer(static_cast<std::int32_t>(result.maxScore));
  encoder.integer(static_cast<std::int32_t>(result.maxCombo));
  encoder.float32(result.finalGauge);
  encoder.integer(static_cast<std::int32_t>(result.clearType));
  appendProvenance(encoder, result.provenance);
  encoder.vector(result.stages, [&](const PersistedCourseStageResult &stage) {
    encoder.integer(static_cast<std::int32_t>(stage.stageIndex));
    appendScore(encoder, stage.score);
    encoder.integer(static_cast<std::int32_t>(stage.keyMode));
    encoder.enumeration(stage.adoptedGaugeType);
    appendGaugeHistory(encoder, stage.adoptedGaugeHistory);
    appendTiming(encoder, stage.judgementTiming);
  });
  encoder.vector(result.entryFacts, [&](const PersistedCourseEntryFacts &facts) {
    encoder.integer(static_cast<std::int32_t>(facts.totalNotes));
    encoder.integer(facts.playLengthMicros);
  });
  encoder.integer(result.playedAtUnixMillis);
  return encoder.finish();
}

} // namespace result_persistence
