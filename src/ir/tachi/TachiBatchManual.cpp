#include "TachiBatchManual.h"

#include "../../CanonicalDigest.h"
#include "../../FileChecksum.h"
#include "../../Uuid.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <span>
#include <vector>

namespace ir::tachi {
namespace {

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
          .reason = SubmissionEligibilityReason::InvalidSubmission,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

BuildDraftOutcome ineligible(const SubmissionEligibilityOutcome &eligibility) {
  return {
      .status =
          eligibility.reason == SubmissionEligibilityReason::InvalidSubmission
              ? BuildDraftStatus::Invalid
              : BuildDraftStatus::Unsupported,
      .reason = eligibility.reason,
      .diagnostic = sanitizeDiagnostic(eligibility.diagnostic),
  };
}

BuildTachiOutboxBatchOutcome
invalidOutboxBatch(std::string_view diagnostic,
                   std::optional<std::int64_t> rejectedRowId = std::nullopt) {
  return {.status = BuildTachiOutboxBatchStatus::Invalid,
          .rejectedRowId = rejectedRowId,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

std::string proofFingerprintInput(const IrOutboxEntry &entry) {
  const auto &proof = entry.rulesetProof;
  std::string input = "tachi-lr2-proof-v1\n";
  input +=
      std::to_string(proof.rulesetId.size()) + ":" + proof.rulesetId + "\n";
  input += std::to_string(proof.rulesetRevision) + "\n";
  input +=
      std::to_string(entry.attemptId.size()) + ":" + entry.attemptId + "\n";
  input +=
      std::to_string(entry.chartSha256.size()) + ":" + entry.chartSha256 + "\n";
  input += std::to_string(entry.payloadJson.size()) + ":" + entry.payloadJson;
  return input;
}

bool hasValidStoredProof(const IrOutboxEntry &entry) {
  const auto &proof = entry.rulesetProof;
  return entry.id > 0 && entry.providerId == kProviderId &&
         proof.rulesetId == "lr2" &&
         proof.rulesetRevision == RulesetDescriptor::kCurrentVersion &&
         proof.validationFingerprint ==
             file_checksum::sha256(proofFingerprintInput(entry));
}

std::vector<std::size_t> balancedSampleOrder(std::size_t sourceSize) {
  if (sourceSize == 0) {
    return {};
  }

  std::vector<std::size_t> order;
  order.reserve(sourceSize);
  order.push_back(0);
  if (sourceSize == 1) {
    return order;
  }
  order.push_back(sourceSize - 1);

  struct Interval {
    std::size_t left = 0;
    std::size_t right = 0;
  };
  const auto lowerPriority = [](const Interval &left, const Interval &right) {
    const std::size_t leftWidth = left.right - left.left;
    const std::size_t rightWidth = right.right - right.left;
    if (leftWidth != rightWidth) {
      return leftWidth < rightWidth;
    }
    return left.left > right.left;
  };
  std::priority_queue<Interval, std::vector<Interval>, decltype(lowerPriority)>
      intervals(lowerPriority);
  intervals.push({.left = 0, .right = sourceSize - 1});

  while (!intervals.empty()) {
    const Interval interval = intervals.top();
    intervals.pop();
    if (interval.right - interval.left <= 1) {
      continue;
    }
    const std::size_t middle =
        interval.left + (interval.right - interval.left) / 2;
    order.push_back(middle);
    if (middle - interval.left > 1) {
      intervals.push({.left = interval.left, .right = middle});
    }
    if (interval.right - middle > 1) {
      intervals.push({.left = middle, .right = interval.right});
    }
  }
  return order;
}

std::string validationFingerprint(const IrSubmission &submission,
                                  std::string_view payload) {
  const auto &ruleset = submission.provenance.ruleset;
  std::string input = "tachi-lr2-proof-v1\n";
  input += std::to_string(ruleset.id.size()) + ":" + ruleset.id + "\n";
  input += std::to_string(ruleset.version) + "\n";
  input += std::to_string(submission.attemptId.size()) + ":" +
           submission.attemptId + "\n";
  input += std::to_string(submission.chartSha256.size()) + ":" +
           submission.chartSha256 + "\n";
  input += std::to_string(payload.size()) + ":";
  input.append(payload);
  return file_checksum::sha256(input);
}

} // namespace

BuildDraftOutcome
buildBatchManualDraft(const IrSubmission &submission) noexcept {
  try {
    if (submission.keyMode != 7 && submission.keyMode != 14) {
      return ineligible(validateBokutachiEligibility(submission));
    }
    if (!uuid::isCanonicalLowerV4(submission.attemptId)) {
      return invalid("submission attempt ID is malformed");
    }
    const bool hasSha256 = !submission.chartSha256.empty();
    const bool hasMd5 = !submission.chartMd5.empty();
    if ((hasSha256 && !canonical_digest::isCanonicalLowerHex(
                          submission.chartSha256, 64)) ||
        (hasMd5 && !canonical_digest::isCanonicalLowerHex(
                       submission.chartMd5, 32)) ||
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
    if (submission.pGreatFast < 0 || submission.pGreatSlow < 0 ||
        submission.pGreatFast > submission.fast ||
        submission.pGreatSlow > submission.slow) {
      return invalid("submission PGREAT timing breakdown is invalid");
    }
    if (submission.judgementTimingBreakdownAvailable &&
        (submission.earlyPGreat < 0 || submission.latePGreat < 0 ||
         submission.earlyGreat < 0 || submission.lateGreat < 0 ||
         submission.earlyGood < 0 || submission.lateGood < 0 ||
         submission.earlyBad < 0 || submission.lateBad < 0 ||
         submission.earlyPoor < 0 || submission.latePoor < 0 ||
         static_cast<long long>(submission.earlyPGreat) +
                 submission.latePGreat !=
             submission.pGreat ||
         static_cast<long long>(submission.earlyGreat) + submission.lateGreat !=
             submission.great ||
         static_cast<long long>(submission.earlyGood) + submission.lateGood !=
             submission.good ||
         static_cast<long long>(submission.earlyBad) + submission.lateBad !=
             submission.bad ||
         static_cast<long long>(submission.earlyPoor) + submission.latePoor !=
             submission.poor ||
         submission.pGreatFast > submission.earlyPGreat ||
         submission.pGreatSlow > submission.latePGreat)) {
      return invalid("submission LR2 judgement timing breakdown is invalid");
    }
    if (std::ranges::any_of(submission.gaugeHistory, [](float value) {
          return !std::isfinite(value);
        })) {
      return invalid("submission gauge history is not finite");
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
    const long long badPoints = static_cast<long long>(submission.bad) +
                                submission.poor + submission.kPoor;
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
    const auto eligibility = validateBokutachiEligibility(submission);
    if (!eligibility.eligible()) {
      return ineligible(eligibility);
    }

    const std::string &identifier =
        hasSha256 ? submission.chartSha256 : submission.chartMd5;
    const int fast = submission.fast - submission.pGreatFast;
    const int slow = submission.slow - submission.pGreatSlow;
    const auto sampledHistory = [&](std::span<const std::size_t> indices) {
      nlohmann::json history = nlohmann::json::array();
      for (const std::size_t index : indices) {
        history.push_back(
            std::clamp(submission.gaugeHistory[index], 0.0F, 100.0F));
      }
      return history;
    };
    const auto makeDocument = [&](std::span<const std::size_t> indices) {
      nlohmann::json optional = {
          {"fast", fast},
          {"slow", slow},
          {"maxCombo", submission.maxCombo},
          {"bp", static_cast<int>(badPoints)},
          {"gauge", std::clamp(submission.finalGauge, 0.0F, 100.0F)},
      };
      if (submission.judgementTimingBreakdownAvailable) {
        optional["epg"] = submission.earlyPGreat;
        optional["lpg"] = submission.latePGreat;
        optional["egr"] = submission.earlyGreat;
        optional["lgr"] = submission.lateGreat;
        optional["egd"] = submission.earlyGood;
        optional["lgd"] = submission.lateGood;
        optional["ebd"] = submission.earlyBad;
        optional["lbd"] = submission.lateBad;
        optional["epr"] = submission.earlyPoor;
        optional["lpr"] = submission.latePoor;
      }
      if (!submission.gaugeHistory.empty()) {
        optional["gaugeHistory"] = sampledHistory(indices);
      }
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
          {"optional", std::move(optional)},
      };
      return nlohmann::json{
          {"meta",
           {{"game", "bms"},
            {"playtype", submission.keyMode == 7 ? "7K" : "14K"},
            {"service", "AsoBMaShow"}}},
          {"scores", nlohmann::json::array({std::move(score)})},
      };
    };

    const std::size_t minimumSamples =
        std::min<std::size_t>(2, submission.gaugeHistory.size());
    std::vector<std::size_t> allIndices(submission.gaugeHistory.size());
    std::iota(allIndices.begin(), allIndices.end(), 0);
    std::vector<std::size_t> selected = allIndices;
    std::string payload = makeDocument(allIndices).dump();
    if (payload.size() > kMaximumPayloadBytes &&
        !submission.gaugeHistory.empty()) {
      const std::string emptyPayload =
          makeDocument(std::span<const std::size_t>{}).dump();
      if (emptyPayload.size() > kMaximumPayloadBytes) {
        return invalid("submission payload exceeds the provider size limit");
      }

      selected.clear();
      // The empty document still emits gaugeHistory:[], so this baseline
      // includes the property name and array delimiters.
      std::size_t selectedPayloadSize = emptyPayload.size();
      for (const std::size_t index :
           balancedSampleOrder(submission.gaugeHistory.size())) {
        const float value =
            std::clamp(submission.gaugeHistory[index], 0.0F, 100.0F);
        const std::size_t separatorSize = selected.empty() ? 0 : 1;
        const std::size_t tokenSize = nlohmann::json(value).dump().size();
        const std::size_t available =
            kMaximumPayloadBytes - selectedPayloadSize;
        if (separatorSize + tokenSize > available) {
          break;
        }
        selectedPayloadSize += separatorSize + tokenSize;
        selected.push_back(index);
      }
      std::ranges::sort(selected);
      payload = makeDocument(selected).dump();
      while (payload.size() > kMaximumPayloadBytes &&
             selected.size() > minimumSamples) {
        selected.erase(selected.end() - 2);
        payload = makeDocument(selected).dump();
      }
    }

    if (selected.size() < minimumSamples ||
        payload.size() > kMaximumPayloadBytes) {
      return invalid("submission payload exceeds the provider size limit");
    }

    const IrRulesetProof proof{
        .rulesetId = submission.provenance.ruleset.id,
        .rulesetRevision = submission.provenance.ruleset.version,
        .validationFingerprint = validationFingerprint(submission, payload),
    };

    return {
        .status = BuildDraftStatus::Built,
        .reason = SubmissionEligibilityReason::Eligible,
        .draft =
            IrOutboxDraft{
                .providerId = std::string(kProviderId),
                .attemptId = submission.attemptId,
                .chartMd5 = submission.chartMd5,
                .chartSha256 = submission.chartSha256,
                .payloadJson = std::move(payload),
                .rulesetProof = proof,
                .createdAtUnixMillis = submission.playedAtUnixMillis,
            },
    };
  } catch (...) {
    return invalid("Tachi payload construction failed");
  }
}

BuildTachiOutboxBatchOutcome buildBatchManualOutboxDocument(
    std::span<const IrOutboxEntry> entries) noexcept {
  try {
    if (entries.empty()) {
      return invalidOutboxBatch("Tachi outbox batch is empty");
    }

    nlohmann::json batchMeta;
    nlohmann::json batchScores = nlohmann::json::array();
    std::string selectedPlaytype;
    std::vector<std::int64_t> rowIds;
    rowIds.reserve(std::min<std::size_t>(entries.size(), 64));
    std::string payloadJson;

    for (const auto &entry : entries) {
      if (rowIds.size() == 64) {
        break;
      }
      try {
        if (!hasValidStoredProof(entry) || entry.payloadJson.empty() ||
            entry.payloadJson.size() > kMaximumPayloadBytes) {
          if (rowIds.empty()) {
            return invalidOutboxBatch(
                "Tachi outbox row has an invalid ruleset proof or payload",
                entry.id);
          }
          break;
        }

        const nlohmann::json source = nlohmann::json::parse(entry.payloadJson);
        if (!source.is_object()) {
          if (rowIds.empty()) {
            return invalidOutboxBatch(
                "Tachi outbox payload is not an object", entry.id);
          }
          break;
        }
        const auto meta = source.find("meta");
        const auto scores = source.find("scores");
        if (meta == source.end() || !meta->is_object() ||
            scores == source.end() || !scores->is_array() ||
            scores->size() != 1 || !scores->front().is_object()) {
          if (rowIds.empty()) {
            return invalidOutboxBatch(
                "Tachi outbox payload must contain one meta object and score",
                entry.id);
          }
          break;
        }
        const auto playtype = meta->find("playtype");
        if (playtype == meta->end() || !playtype->is_string()) {
          if (rowIds.empty()) {
            return invalidOutboxBatch(
                "Tachi outbox payload playtype is missing or invalid",
                entry.id);
          }
          break;
        }
        const auto &playtypeText = playtype->get_ref<const std::string &>();
        if (playtypeText != "7K" && playtypeText != "14K") {
          if (rowIds.empty()) {
            return invalidOutboxBatch(
                "Tachi outbox payload playtype is unsupported", entry.id);
          }
          break;
        }

        if (selectedPlaytype.empty()) {
          selectedPlaytype = playtypeText;
          batchMeta = *meta;
        } else if (playtypeText != selectedPlaytype) {
          continue;
        }
        nlohmann::json candidateScores = batchScores;
        candidateScores.push_back(scores->front());
        const std::string candidatePayload = nlohmann::json{
            {"meta", batchMeta},
            {"scores", std::move(candidateScores)}}.dump();
        if (candidatePayload.size() > kMaximumPayloadBytes) {
          if (rowIds.empty()) {
            return invalidOutboxBatch(
                "Tachi outbox score exceeds the provider size limit",
                entry.id);
          }
          break;
        }
        batchScores.push_back(scores->front());
        rowIds.push_back(entry.id);
        payloadJson = candidatePayload;
      } catch (...) {
        if (rowIds.empty()) {
          return invalidOutboxBatch(
              "Tachi outbox row could not be parsed", entry.id);
        }
        break;
      }
    }

    if (rowIds.empty()) {
      return invalidOutboxBatch("Tachi outbox batch has no compatible rows");
    }
    return {
        .status = BuildTachiOutboxBatchStatus::Built,
        .document =
            TachiOutboxBatchDocument{
                .rowIds = std::move(rowIds),
                .playtype = std::move(selectedPlaytype),
                .payloadJson = std::move(payloadJson),
            },
    };
  } catch (...) {
    return invalidOutboxBatch("Tachi outbox batch construction failed");
  }
}

} // namespace ir::tachi
