#include "TachiBatchManual.h"

#include "../../Uuid.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

namespace ir::tachi {
namespace {

bool isLowerHexDigest(std::string_view value, std::size_t expectedSize) {
  return value.size() == expectedSize &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<std::string_view> lampForClearRank(int clearType) {
  switch (clearType) {
  case kClearTypeFailedRank:
    return "FAILED";
  case kClearTypeAssistedEasyClearRank:
  case kClearTypeLightAssistedEasyClearRank:
    return "ASSIST CLEAR";
  case kClearTypeEasyClearRank:
    return "EASY CLEAR";
  case kClearTypeNormalClearRank:
    return "CLEAR";
  case kClearTypeHardClearRank:
    return "HARD CLEAR";
  case kClearTypeExHardClearRank:
    return "EX HARD CLEAR";
  case kClearTypeFullComboRank:
    return "FULL COMBO";
  default:
    return std::nullopt;
  }
}

BuildDraftOutcome invalid(std::string_view diagnostic) {
  return {.status = BuildDraftStatus::Invalid,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

} // namespace

BuildDraftOutcome
buildBatchManualDraft(const IrSubmission &submission) noexcept {
  try {
    if (submission.keyMode != 7 && submission.keyMode != 14) {
      return {.status = BuildDraftStatus::Unsupported,
              .diagnostic = "Bokutachi supports BMS 7K and 14K only"};
    }
    if (!uuid::isCanonicalLowerV4(submission.attemptId)) {
      return invalid("submission attempt ID is malformed");
    }
    const bool hasSha256 = !submission.chartSha256.empty();
    const bool hasMd5 = !submission.chartMd5.empty();
    if ((hasSha256 &&
         !isLowerHexDigest(submission.chartSha256, 64)) ||
        (hasMd5 && !isLowerHexDigest(submission.chartMd5, 32)) ||
        (!hasSha256 && !hasMd5)) {
      return invalid("submission chart hash is malformed");
    }

    const std::array counts{
        submission.maxCombo, submission.comboBreak, submission.pGreat,
        submission.great,    submission.good,       submission.bad,
        submission.poor,     submission.kPoor,      submission.fast,
        submission.slow,
    };
    if (std::ranges::any_of(counts, [](int value) { return value < 0; })) {
      return invalid("submission counters must not be negative");
    }
    if (submission.maxScore <= 0 || submission.score < 0 ||
        submission.score > submission.maxScore ||
        submission.maxScore % 2 != 0 ||
        submission.maxCombo > submission.maxScore / 2 ||
        submission.playedAtUnixMillis <= 0) {
      return invalid("submission score range or timestamp is invalid");
    }
    const long long expectedEx =
        static_cast<long long>(submission.pGreat) * 2LL + submission.great;
    if (expectedEx != submission.score) {
      return invalid("submission EX score disagrees with judgements");
    }
    const long long badPoints =
        static_cast<long long>(submission.bad) + submission.poor;
    if (badPoints > std::numeric_limits<int>::max()) {
      return invalid("submission BP exceeds the supported range");
    }
    if (!std::isfinite(submission.finalGauge)) {
      return invalid("submission gauge is not finite");
    }
    const auto lamp = lampForClearRank(submission.clearType);
    if (!lamp.has_value()) {
      return invalid("submission clear rank is unknown");
    }

    const std::string &identifier =
        hasSha256 ? submission.chartSha256 : submission.chartMd5;
    nlohmann::json score = {
        {"score", submission.score},
        {"lamp", *lamp},
        {"matchType", "bmsChartHash"},
        {"identifier", identifier},
        {"timeAchieved", submission.playedAtUnixMillis},
        {"judgements",
         {{"pgreat", submission.pGreat},
          {"great", submission.great},
          {"good", submission.good},
          {"bad", submission.bad},
          {"poor", submission.poor}}},
        {"optional",
         {{"fast", submission.fast},
          {"slow", submission.slow},
          {"maxCombo", submission.maxCombo},
          {"bp", static_cast<int>(badPoints)},
          {"gauge", std::clamp(submission.finalGauge, 0.0F, 100.0F)}}},
    };
    nlohmann::json document = {
        {"meta",
         {{"game", "bms"},
          {"playtype", submission.keyMode == 7 ? "7K" : "14K"},
          {"service", "AsoBMaShow"}}},
        {"scores", nlohmann::json::array({std::move(score)})},
    };
    std::string payload = document.dump();
    if (payload.size() > kMaximumPayloadBytes) {
      return invalid("submission payload exceeds the provider size limit");
    }

    return {
        .status = BuildDraftStatus::Built,
        .draft = IrOutboxDraft{
            .providerId = std::string(kProviderId),
            .attemptId = submission.attemptId,
            .chartMd5 = submission.chartMd5,
            .chartSha256 = submission.chartSha256,
            .payloadJson = std::move(payload),
            .createdAtUnixMillis = submission.playedAtUnixMillis,
        },
    };
  } catch (...) {
    return invalid("Tachi payload construction failed");
  }
}

} // namespace ir::tachi
