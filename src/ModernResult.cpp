#include "ModernResult.h"

#include "BmsMetadataText.h"
#include "DurablePayloadLimits.h"
#include "FileChecksum.h"
#include "Utils.h"
#include "Uuid.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace result_persistence {
namespace {

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

  void string(std::string_view value) {
    integer(static_cast<std::uint64_t>(value.size()));
    const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  template <typename Value, typename Append>
  void optional(const std::optional<Value> &value, Append append) {
    boolean(value.has_value());
    if (value) {
      append(*value);
    }
  }

  template <typename Value, typename Append>
  void vector(const std::vector<Value> &values, Append append) {
    integer(static_cast<std::uint64_t>(values.size()));
    for (const auto &value : values) {
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

bool sameFloatBits(float left, float right) noexcept {
  return std::bit_cast<std::uint32_t>(left) ==
         std::bit_cast<std::uint32_t>(right);
}

int judgementCount(const RhythmState &state, Judgement judgement) {
  const auto found = state.judgeCount.find(judgement);
  return found == state.judgeCount.end() ? 0 : found->second;
}

bool sameFloatVector(std::span<const float> left,
                     std::span<const float> right) noexcept {
  return left.size() == right.size() &&
         std::ranges::equal(left, right, sameFloatBits);
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

bool knownGaugeAutoShift(GaugeAutoShiftMode value) noexcept {
  const int index = static_cast<int>(value);
  return index >= static_cast<int>(GaugeAutoShiftMode::None) &&
         index <= static_cast<int>(GaugeAutoShiftMode::BestClear);
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

bool validScoreStrings(const ChartScoreWrite &score) noexcept {
  return durable_payload::validString(score.chartPath, false) &&
         durable_payload::validString(score.chartMd5, true) &&
         durable_payload::validString(score.chartSha256, false) &&
         durable_payload::validString(score.chartTitle, true) &&
         durable_payload::validString(score.chartArtist, true);
}

bool validateScore(const ChartScoreWrite &score,
                   std::int64_t maximumAllowedCombo, std::string &diagnostic) {
  if (!validScoreStrings(score) || !hasProjectableChartIdentity(score)) {
    diagnostic = "chart result identity or display facts are invalid";
    return false;
  }
  const std::array counts{score.score,      score.maxScore, score.maxCombo,
                          score.comboBreak, score.pGreat,   score.great,
                          score.good,       score.bad,      score.poor,
                          score.kPoor,      score.fast,     score.slow};
  if (std::ranges::any_of(counts, [](int value) { return value < 0; }) ||
      score.longNoteMode < 0 || score.longNoteMode > 3) {
    diagnostic = "chart result counters or long-note mode are invalid";
    return false;
  }
  if (score.maxScore <= 0 || score.maxScore % 2 != 0 ||
      score.score > score.maxScore ||
      static_cast<std::int64_t>(score.maxCombo) > maximumAllowedCombo ||
      static_cast<std::int64_t>(score.pGreat) * 2LL + score.great !=
          score.score) {
    diagnostic = "chart result score arithmetic is inconsistent";
    return false;
  }
  if (!std::isfinite(score.finalGauge) || score.finalGauge < 0.0F ||
      !knownClearRank(score.clearType)) {
    diagnostic = "chart result gauge or clear type is invalid";
    return false;
  }
  std::string provenanceDiagnostic;
  if (!serializeValidatedScoreProvenance(score.provenance,
                                         provenanceDiagnostic)) {
    diagnostic = "chart result provenance is invalid";
    return false;
  }
  return true;
}

bool validateTiming(const ChartScoreWrite &score,
                    const std::optional<ChartJudgementTiming> &timing,
                    std::string &diagnostic) {
  if (!timing) {
    return true;
  }
  const std::array totals{score.pGreat, score.great, score.good, score.bad,
                          score.kPoor,  score.poor,  0};
  std::int64_t fast = 0;
  std::int64_t slow = 0;
  for (int index = 0; index < JudgementCount; ++index) {
    const auto judgement = static_cast<Judgement>(index);
    const auto &count = timing->byJudgement[static_cast<std::size_t>(index)];
    if (count.fast < 0 || count.slow < 0) {
      diagnostic = "judgement timing cannot be negative";
      return false;
    }
    if (judgement == Kpoor || judgement == None) {
      if (count.fast != 0 || count.slow != 0) {
        diagnostic = "KPOOR and NONE cannot have judgement timing";
        return false;
      }
      continue;
    }
    if (static_cast<std::int64_t>(count.fast) + count.slow >
        totals[static_cast<std::size_t>(index)]) {
      diagnostic = "judgement timing exceeds its result total";
      return false;
    }
    fast += count.fast;
    slow += count.slow;
  }
  if (fast != score.fast || slow != score.slow) {
    diagnostic = "judgement timing disagrees with aggregate timing";
    return false;
  }
  return true;
}

bool validateResultFacts(const ChartScoreWrite &score, int keyMode,
                         GaugeType adoptedGaugeType,
                         std::span<const float> gaugeHistory,
                         const std::optional<ChartJudgementTiming> &timing,
                         std::int64_t maximumAllowedCombo,
                         std::string &diagnostic) {
  if (keyMode <= 0 || !knownGaugeType(adoptedGaugeType)) {
    diagnostic = "chart result key mode or adopted gauge is invalid";
    return false;
  }
  if (!validateScore(score, maximumAllowedCombo, diagnostic) ||
      !validateTiming(score, timing, diagnostic)) {
    return false;
  }
  if (!durable_payload::withinLimit(
          gaugeHistory.size(), durable_payload::kMaximumResultGaugeSamples) ||
      std::ranges::any_of(gaugeHistory,
                          [](float value) { return !std::isfinite(value); })) {
    diagnostic = "adopted gauge history is invalid or oversized";
    return false;
  }
  return true;
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
  encoder.string(serializeScoreProvenance(score.provenance));
}

void appendGaugeHistory(CanonicalEncoder &encoder,
                        const std::vector<float> &history) {
  encoder.vector(history, [&](float value) { encoder.float32(value); });
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

bool canonicalCourseKey(std::string_view value) noexcept {
  constexpr std::string_view prefix = "course:v1:";
  return value.starts_with(prefix) &&
         canonical_digest::isCanonicalLowerHex(value.substr(prefix.size()), 64);
}

ResultFactAgreement disagreement(ResultFactAgreementIssue issue,
                                 std::string diagnostic) {
  return {.issue = issue, .diagnostic = std::move(diagnostic)};
}

bool sameScoreOutcome(const ChartScoreWrite &left,
                      const ChartScoreWrite &right) noexcept {
  return left.score == right.score && left.maxScore == right.maxScore &&
         left.maxCombo == right.maxCombo &&
         left.comboBreak == right.comboBreak && left.pGreat == right.pGreat &&
         left.great == right.great && left.good == right.good &&
         left.bad == right.bad && left.poor == right.poor &&
         left.kPoor == right.kPoor && left.fast == right.fast &&
         left.slow == right.slow &&
         sameFloatBits(left.finalGauge, right.finalGauge) &&
         left.clearType == right.clearType;
}

} // namespace

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

ChartScoreWrite captureChartScoreWrite(const bms_parser::ChartMeta &meta,
                                       const RhythmState &state,
                                       const ScoreProvenance &provenance,
                                       int storageLongNoteMode) {
  const int maximumScore =
      meta.TotalNotes > 0 &&
              meta.TotalNotes <= std::numeric_limits<int>::max() / 2
          ? meta.TotalNotes * 2
          : -1;
  return {
      .chartPath =
          Utils::GetStoragePathUtf8RelativeToDocuments(meta.BmsPath, "BMS/"),
      .chartMd5 = normalizedHash(meta.MD5),
      .chartSha256 = normalizedHash(meta.SHA256),
      .chartTitle = meta.Title,
      .chartArtist = meta.Artist,
      .longNoteMode = storageLongNoteMode,
      .score = state.getScore(),
      .maxScore = maximumScore,
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

std::optional<ModernChartResult> captureModernChartResult(
    std::string attemptId, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &provenance,
    int storageLongNoteMode, std::int64_t playedAtUnixMillis,
    std::string &diagnostic) noexcept {
  try {
    ModernChartResult result{
        .attemptId = std::move(attemptId),
        .score = captureChartScoreWrite(meta, state, provenance,
                                        storageLongNoteMode),
        .keyMode = meta.KeyMode,
        .adoptedGaugeType = state.gaugeType,
        .adoptedGaugeHistory = state.gaugeHistoryFor(state.gaugeType),
        .judgementTiming = captureChartJudgementTiming(state),
        .playedAtUnixMillis = playedAtUnixMillis,
    };
    result.resultFingerprint = modernResultFingerprint(result);
    if (!validateModernChartResult(result, diagnostic)) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    diagnostic = "modern chart result capture failed";
    return std::nullopt;
  }
}

std::optional<ModernCourseStageResult> captureModernCourseStageResult(
    int stageIndex, const bms_parser::ChartMeta &meta, const RhythmState &state,
    const ScoreProvenance &provenance, int storageLongNoteMode,
    std::string &diagnostic) noexcept {
  try {
    diagnostic.clear();
    ModernCourseStageResult result{
        .stageIndex = stageIndex,
        .score = captureChartScoreWrite(meta, state, provenance,
                                        storageLongNoteMode),
        .keyMode = meta.KeyMode,
        .adoptedGaugeType = state.gaugeType,
        .adoptedGaugeHistory = state.gaugeHistoryFor(state.gaugeType),
        .judgementTiming = captureChartJudgementTiming(state),
    };
    if (stageIndex < 0 ||
        static_cast<std::size_t>(stageIndex) >=
            durable_payload::kMaximumCourseStages ||
        !validateResultFacts(result.score, result.keyMode,
                             result.adoptedGaugeType,
                             result.adoptedGaugeHistory, result.judgementTiming,
                             std::numeric_limits<int>::max(), diagnostic)) {
      if (diagnostic.empty()) {
        diagnostic = "modern course stage index is invalid";
      }
      return std::nullopt;
    }
    return result;
  } catch (...) {
    diagnostic = "modern course stage capture failed";
    return std::nullopt;
  }
}

bool validateModernChartResult(const ModernChartResult &result,
                               std::string &diagnostic) noexcept {
  try {
    diagnostic.clear();
    if (result.resultId < 0 || !uuid::isCanonicalLowerV4(result.attemptId) ||
        result.playedAtUnixMillis <= 0) {
      diagnostic = "modern chart result identity or completion time is invalid";
      return false;
    }
    if (!validateResultFacts(result.score, result.keyMode,
                             result.adoptedGaugeType,
                             result.adoptedGaugeHistory, result.judgementTiming,
                             result.score.maxScore / 2, diagnostic)) {
      return false;
    }
    if (!canonical_digest::isCanonicalLowerHex(result.resultFingerprint, 64) ||
        result.resultFingerprint != modernResultFingerprint(result)) {
      diagnostic = "modern chart result fingerprint is inconsistent";
      return false;
    }
    return true;
  } catch (...) {
    diagnostic = "modern chart result validation failed";
    return false;
  }
}

std::string modernResultFingerprint(const ModernChartResult &result) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-modern-chart-result-v1");
  encoder.string(result.attemptId);
  appendScore(encoder, result.score);
  encoder.integer(static_cast<std::int32_t>(result.keyMode));
  encoder.enumeration(result.adoptedGaugeType);
  appendGaugeHistory(encoder, result.adoptedGaugeHistory);
  appendTiming(encoder, result.judgementTiming);
  encoder.integer(result.playedAtUnixMillis);
  return encoder.finish();
}

bool validateModernCourseResult(const ModernCourseResult &result,
                                std::string &diagnostic) noexcept {
  try {
    diagnostic.clear();
    if (result.resultId < 0 || result.legacyCourseId < 0 ||
        !uuid::isCanonicalLowerV4(result.attemptId) ||
        result.playedAtUnixMillis <= 0 ||
        !canonicalCourseKey(result.courseKey)) {
      diagnostic =
          "modern course result identity or completion time is invalid";
      return false;
    }
    if (!durable_payload::validString(result.courseName, false) ||
        !durable_payload::validString(result.courseGroupName, true) ||
        !durable_payload::validString(result.constraintJson, true) ||
        !durable_payload::validString(result.requestedPlayOption, false) ||
        !durable_payload::validString(result.assistOption, false)) {
      diagnostic = "modern course result strings are invalid";
      return false;
    }
    if (result.totalCharts <= 0 || result.completedCharts <= 0 ||
        result.completedCharts > result.totalCharts ||
        static_cast<std::size_t>(result.totalCharts) >
            durable_payload::kMaximumCourseStages ||
        result.stages.size() !=
            static_cast<std::size_t>(result.completedCharts) ||
        result.entryFacts.size() !=
            static_cast<std::size_t>(result.totalCharts)) {
      diagnostic = "modern course completion prefix is malformed";
      return false;
    }
    if (!knownGaugeType(result.initialGaugeType) ||
        !knownGaugeProfile(result.gaugeProfile) ||
        !knownGaugeAutoShift(result.gaugeAutoShift) ||
        !knownGaugeType(result.gaugeAutoShiftLowerBound) ||
        result.longNoteMode < 0 || result.longNoteMode > 3 ||
        result.finalScore < 0 || result.maxScore <= 0 ||
        result.finalScore > result.maxScore || result.maxCombo < 0 ||
        !std::isfinite(result.finalGauge) || result.finalGauge < 0.0F ||
        !knownClearRank(result.clearType)) {
      diagnostic = "modern course setup or aggregate facts are invalid";
      return false;
    }

    std::int64_t maximumScore = 0;
    for (const auto &entry : result.entryFacts) {
      if (entry.totalNotes <= 0 || entry.playLengthMicros < 0 ||
          entry.totalNotes > std::numeric_limits<int>::max() / 2) {
        diagnostic = "modern course entry facts are invalid";
        return false;
      }
      maximumScore += static_cast<std::int64_t>(entry.totalNotes) * 2LL;
    }
    if (maximumScore != result.maxScore) {
      diagnostic = "modern course maximum score disagrees with entries";
      return false;
    }

    std::int64_t completedNotes = 0;
    std::int64_t stageScore = 0;
    std::size_t gaugeSamples = 0;
    int previousMaximumCombo = 0;
    std::vector<ScoreProvenance> stageProvenance;
    stageProvenance.reserve(result.stages.size());
    for (std::size_t index = 0; index < result.stages.size(); ++index) {
      const auto &stage = result.stages[index];
      completedNotes += result.entryFacts[index].totalNotes;
      if (stage.stageIndex != static_cast<int>(index) ||
          !validateResultFacts(stage.score, stage.keyMode,
                               stage.adoptedGaugeType,
                               stage.adoptedGaugeHistory, stage.judgementTiming,
                               completedNotes, diagnostic)) {
        if (diagnostic.empty()) {
          diagnostic = "modern course stage order is malformed";
        }
        return false;
      }
      if (static_cast<std::int64_t>(stage.score.maxScore) !=
          static_cast<std::int64_t>(result.entryFacts[index].totalNotes) *
              2LL) {
        diagnostic = "modern course stage maximum disagrees with its entry";
        return false;
      }
      if (stage.score.maxCombo < previousMaximumCombo) {
        diagnostic = "modern course maximum combo cannot decrease";
        return false;
      }
      previousMaximumCombo = stage.score.maxCombo;
      stageScore += stage.score.score;
      if (gaugeSamples > durable_payload::kMaximumResultGaugeSamples -
                             stage.adoptedGaugeHistory.size()) {
        diagnostic = "modern course gauge history is oversized";
        return false;
      }
      gaugeSamples += stage.adoptedGaugeHistory.size();
      stageProvenance.push_back(stage.score.provenance);
    }
    if (stageScore != result.finalScore ||
        previousMaximumCombo != result.maxCombo ||
        static_cast<std::int64_t>(result.maxCombo) > completedNotes ||
        !sameFloatBits(result.finalGauge,
                       result.stages.back().score.finalGauge)) {
      diagnostic = "modern course aggregate disagrees with ordered stages";
      return false;
    }
    if (mergeCourseProvenance(stageProvenance) != result.provenance) {
      diagnostic = "modern course provenance disagrees with ordered stages";
      return false;
    }
    std::string provenanceDiagnostic;
    if (!serializeValidatedScoreProvenance(result.provenance,
                                           provenanceDiagnostic)) {
      diagnostic = "modern course provenance is invalid";
      return false;
    }
    if (!canonical_digest::isCanonicalLowerHex(result.resultFingerprint, 64) ||
        result.resultFingerprint != modernResultFingerprint(result)) {
      diagnostic = "modern course result fingerprint is inconsistent";
      return false;
    }
    return true;
  } catch (...) {
    diagnostic = "modern course result validation failed";
    return false;
  }
}

std::string modernResultFingerprint(const ModernCourseResult &result) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-modern-course-result-v1");
  encoder.string(result.attemptId);
  encoder.string(result.courseKey);
  encoder.integer(static_cast<std::int32_t>(result.legacyCourseId));
  encoder.string(result.courseName);
  encoder.string(result.courseGroupName);
  encoder.string(result.constraintJson);
  encoder.integer(static_cast<std::int32_t>(result.completedCharts));
  encoder.integer(static_cast<std::int32_t>(result.totalCharts));
  encoder.string(result.requestedPlayOption);
  encoder.string(result.assistOption);
  encoder.enumeration(result.initialGaugeType);
  encoder.enumeration(result.gaugeProfile);
  encoder.enumeration(result.gaugeAutoShift);
  encoder.enumeration(result.gaugeAutoShiftLowerBound);
  encoder.integer(static_cast<std::int32_t>(result.longNoteMode));
  encoder.integer(static_cast<std::int32_t>(result.finalScore));
  encoder.integer(static_cast<std::int32_t>(result.maxScore));
  encoder.integer(static_cast<std::int32_t>(result.maxCombo));
  encoder.float32(result.finalGauge);
  encoder.integer(static_cast<std::int32_t>(result.clearType));
  encoder.string(serializeScoreProvenance(result.provenance));
  encoder.vector(result.stages, [&](const ModernCourseStageResult &stage) {
    encoder.integer(static_cast<std::int32_t>(stage.stageIndex));
    appendScore(encoder, stage.score);
    encoder.integer(static_cast<std::int32_t>(stage.keyMode));
    encoder.enumeration(stage.adoptedGaugeType);
    appendGaugeHistory(encoder, stage.adoptedGaugeHistory);
    appendTiming(encoder, stage.judgementTiming);
  });
  encoder.vector(result.entryFacts, [&](const ModernCourseEntryFacts &entry) {
    encoder.integer(static_cast<std::int32_t>(entry.totalNotes));
    encoder.integer(entry.playLengthMicros);
  });
  encoder.integer(result.playedAtUnixMillis);
  return encoder.finish();
}

ResultFactAgreement
compareModernChartResultFacts(const ModernChartResult &expected,
                              const ModernChartResult &actual) noexcept {
  try {
    if (expected.score.chartSha256 != actual.score.chartSha256 ||
        expected.score.chartMd5 != actual.score.chartMd5) {
      return disagreement(ResultFactAgreementIssue::ChartIdentity,
                          "chart identities differ");
    }
    if (expected.keyMode != actual.keyMode ||
        expected.score.longNoteMode != actual.score.longNoteMode) {
      return disagreement(ResultFactAgreementIssue::Setup,
                          "key mode or long-note mode differs");
    }
    if (!sameScoreOutcome(expected.score, actual.score)) {
      return disagreement(ResultFactAgreementIssue::Score,
                          "score outcome facts differ");
    }
    if (expected.score.provenance != actual.score.provenance) {
      return disagreement(ResultFactAgreementIssue::Provenance,
                          "score provenance differs");
    }
    if (expected.adoptedGaugeType != actual.adoptedGaugeType ||
        !sameFloatVector(expected.adoptedGaugeHistory,
                         actual.adoptedGaugeHistory)) {
      return disagreement(ResultFactAgreementIssue::AdoptedGauge,
                          "adopted gauge facts differ");
    }
    if (expected.judgementTiming != actual.judgementTiming) {
      return disagreement(ResultFactAgreementIssue::JudgementTiming,
                          "judgement timing differs");
    }
    return {};
  } catch (...) {
    return {.issue = ResultFactAgreementIssue::Score,
            .diagnostic = "result fact comparison failed"};
  }
}

ResultFactAgreement
compareModernCourseResultFacts(const ModernCourseResult &expected,
                               const ModernCourseResult &actual) noexcept {
  try {
    if (expected.courseKey != actual.courseKey) {
      return disagreement(ResultFactAgreementIssue::CourseIdentity,
                          "course identities differ");
    }
    if (expected.completedCharts != actual.completedCharts ||
        expected.totalCharts != actual.totalCharts ||
        expected.stages.size() != actual.stages.size() ||
        expected.entryFacts != actual.entryFacts) {
      return disagreement(ResultFactAgreementIssue::CourseShape,
                          "course shapes differ");
    }
    if (expected.requestedPlayOption != actual.requestedPlayOption ||
        expected.assistOption != actual.assistOption ||
        expected.initialGaugeType != actual.initialGaugeType ||
        expected.gaugeProfile != actual.gaugeProfile ||
        expected.gaugeAutoShift != actual.gaugeAutoShift ||
        expected.gaugeAutoShiftLowerBound != actual.gaugeAutoShiftLowerBound ||
        expected.longNoteMode != actual.longNoteMode) {
      return disagreement(ResultFactAgreementIssue::Setup,
                          "course setup facts differ");
    }
    if (expected.finalScore != actual.finalScore ||
        expected.maxScore != actual.maxScore ||
        expected.maxCombo != actual.maxCombo ||
        !sameFloatBits(expected.finalGauge, actual.finalGauge) ||
        expected.clearType != actual.clearType) {
      return disagreement(ResultFactAgreementIssue::CourseAggregate,
                          "course aggregate facts differ");
    }
    if (expected.provenance != actual.provenance) {
      return disagreement(ResultFactAgreementIssue::Provenance,
                          "course provenance differs");
    }
    for (std::size_t index = 0; index < expected.stages.size(); ++index) {
      const auto &left = expected.stages[index];
      const auto &right = actual.stages[index];
      ModernChartResult expectedStage{
          .score = left.score,
          .keyMode = left.keyMode,
          .adoptedGaugeType = left.adoptedGaugeType,
          .adoptedGaugeHistory = left.adoptedGaugeHistory,
          .judgementTiming = left.judgementTiming,
      };
      ModernChartResult actualStage{
          .score = right.score,
          .keyMode = right.keyMode,
          .adoptedGaugeType = right.adoptedGaugeType,
          .adoptedGaugeHistory = right.adoptedGaugeHistory,
          .judgementTiming = right.judgementTiming,
      };
      const auto agreement =
          compareModernChartResultFacts(expectedStage, actualStage);
      if (!agreement.agrees()) {
        return agreement;
      }
    }
    return {};
  } catch (...) {
    return {.issue = ResultFactAgreementIssue::CourseAggregate,
            .diagnostic = "course result fact comparison failed"};
  }
}

} // namespace result_persistence
