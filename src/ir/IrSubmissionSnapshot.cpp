#include "IrSubmissionSnapshot.h"

#include "../FileChecksum.h"
#include "../Uuid.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ir {
namespace {

using Json = nlohmann::ordered_json;

bool lowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

bool knownClearRank(int value) {
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

bool exactKeys(const Json &value,
               std::initializer_list<std::string_view> keys) {
  if (!value.is_object() || value.size() != keys.size()) {
    return false;
  }
  return std::ranges::all_of(keys, [&](std::string_view key) {
    return value.contains(std::string(key));
  });
}

Json submissionToJson(const IrSubmission &submission) {
  Json value = Json::object();
  value["attemptId"] = submission.attemptId;
  value["keyMode"] = submission.keyMode;
  value["chartMd5"] = submission.chartMd5;
  value["chartSha256"] = submission.chartSha256;
  value["score"] = submission.score;
  value["maxScore"] = submission.maxScore;
  value["maxCombo"] = submission.maxCombo;
  value["comboBreak"] = submission.comboBreak;
  value["pGreat"] = submission.pGreat;
  value["great"] = submission.great;
  value["good"] = submission.good;
  value["bad"] = submission.bad;
  value["poor"] = submission.poor;
  value["kPoor"] = submission.kPoor;
  value["fast"] = submission.fast;
  value["slow"] = submission.slow;
  value["pGreatFast"] = submission.pGreatFast;
  value["pGreatSlow"] = submission.pGreatSlow;
  value["judgementTimingBreakdownAvailable"] =
      submission.judgementTimingBreakdownAvailable;
  value["earlyPGreat"] = submission.earlyPGreat;
  value["latePGreat"] = submission.latePGreat;
  value["earlyGreat"] = submission.earlyGreat;
  value["lateGreat"] = submission.lateGreat;
  value["earlyGood"] = submission.earlyGood;
  value["lateGood"] = submission.lateGood;
  value["earlyBad"] = submission.earlyBad;
  value["lateBad"] = submission.lateBad;
  value["earlyPoor"] = submission.earlyPoor;
  value["latePoor"] = submission.latePoor;
  value["gaugeHistory"] = submission.gaugeHistory;
  value["finalGauge"] = submission.finalGauge;
  value["clearType"] = submission.clearType;
  value["playedAtUnixMillis"] = submission.playedAtUnixMillis;
  value["provenance"] =
      Json::parse(serializeScoreProvenance(submission.provenance));
  return value;
}

Json fingerprintPayload(const IrSubmissionSnapshot &snapshot) {
  Json value = Json::object();
  value["schemaVersion"] = snapshot.schemaVersion;
  value["submission"] = submissionToJson(snapshot.submission);
  return value;
}

std::string calculatedFingerprint(const IrSubmissionSnapshot &snapshot) {
  return file_checksum::sha256(fingerprintPayload(snapshot).dump());
}

bool validateSubmission(const IrSubmission &submission,
                        std::string &diagnostic) {
  if (!uuid::isCanonicalLowerV4(submission.attemptId) ||
      submission.keyMode <= 0 ||
      (!submission.chartMd5.empty() && !lowerHex(submission.chartMd5, 32)) ||
      !lowerHex(submission.chartSha256, 64) ||
      submission.playedAtUnixMillis <= 0) {
    diagnostic = "IR snapshot identity is invalid";
    return false;
  }
  const std::array counts{
      submission.score,      submission.maxScore,   submission.maxCombo,
      submission.comboBreak, submission.pGreat,     submission.great,
      submission.good,       submission.bad,        submission.poor,
      submission.kPoor,      submission.fast,       submission.slow,
      submission.pGreatFast, submission.pGreatSlow, submission.earlyPGreat,
      submission.latePGreat, submission.earlyGreat, submission.lateGreat,
      submission.earlyGood,  submission.lateGood,   submission.earlyBad,
      submission.lateBad,    submission.earlyPoor,  submission.latePoor,
  };
  if (std::ranges::any_of(counts, [](int value) { return value < 0; }) ||
      submission.maxScore <= 0 || submission.score > submission.maxScore ||
      submission.maxScore % 2 != 0 ||
      submission.maxCombo > submission.maxScore / 2 ||
      static_cast<std::int64_t>(submission.pGreat) * 2LL + submission.great !=
          submission.score ||
      !std::isfinite(submission.finalGauge) || submission.finalGauge < 0.0F ||
      !knownClearRank(submission.clearType) ||
      std::ranges::any_of(submission.gaugeHistory,
                          [](float value) { return !std::isfinite(value); })) {
    diagnostic = "IR snapshot result facts are invalid";
    return false;
  }
  if (submission.judgementTimingBreakdownAvailable) {
    const auto timingPairMatches = [](int early, int late, int total) {
      return static_cast<std::int64_t>(early) +
                 static_cast<std::int64_t>(late) ==
             static_cast<std::int64_t>(total);
    };
    if (!timingPairMatches(submission.earlyPGreat, submission.latePGreat,
                           submission.pGreat) ||
        !timingPairMatches(submission.earlyGreat, submission.lateGreat,
                           submission.great) ||
        !timingPairMatches(submission.earlyGood, submission.lateGood,
                           submission.good) ||
        !timingPairMatches(submission.earlyBad, submission.lateBad,
                           submission.bad) ||
        !timingPairMatches(submission.earlyPoor, submission.latePoor,
                           submission.poor) ||
        submission.pGreatFast > submission.pGreat ||
        submission.pGreatSlow > submission.pGreat) {
      diagnostic = "IR snapshot timing facts are inconsistent";
      return false;
    }
  } else if (submission.pGreatFast != 0 || submission.pGreatSlow != 0 ||
             submission.earlyPGreat != 0 || submission.latePGreat != 0 ||
             submission.earlyGreat != 0 || submission.lateGreat != 0 ||
             submission.earlyGood != 0 || submission.lateGood != 0 ||
             submission.earlyBad != 0 || submission.lateBad != 0 ||
             submission.earlyPoor != 0 || submission.latePoor != 0) {
    diagnostic = "IR snapshot has timing facts without a timing breakdown";
    return false;
  }
  std::string provenanceDiagnostic;
  if (!serializeValidatedScoreProvenance(submission.provenance,
                                         provenanceDiagnostic)) {
    diagnostic = "IR snapshot provenance is invalid";
    return false;
  }
  return true;
}

IrSubmission submissionFromJson(const Json &value) {
  if (!exactKeys(value, {"attemptId",
                         "keyMode",
                         "chartMd5",
                         "chartSha256",
                         "score",
                         "maxScore",
                         "maxCombo",
                         "comboBreak",
                         "pGreat",
                         "great",
                         "good",
                         "bad",
                         "poor",
                         "kPoor",
                         "fast",
                         "slow",
                         "pGreatFast",
                         "pGreatSlow",
                         "judgementTimingBreakdownAvailable",
                         "earlyPGreat",
                         "latePGreat",
                         "earlyGreat",
                         "lateGreat",
                         "earlyGood",
                         "lateGood",
                         "earlyBad",
                         "lateBad",
                         "earlyPoor",
                         "latePoor",
                         "gaugeHistory",
                         "finalGauge",
                         "clearType",
                         "playedAtUnixMillis",
                         "provenance"})) {
    throw std::runtime_error("IR snapshot submission fields are malformed");
  }
  std::string provenanceDiagnostic;
  auto provenance = deserializeScoreProvenance(value.at("provenance").dump(),
                                               provenanceDiagnostic);
  if (!provenance.has_value()) {
    throw std::runtime_error("IR snapshot provenance is malformed");
  }
  return {
      .attemptId = value.at("attemptId").get<std::string>(),
      .keyMode = value.at("keyMode").get<int>(),
      .chartMd5 = value.at("chartMd5").get<std::string>(),
      .chartSha256 = value.at("chartSha256").get<std::string>(),
      .score = value.at("score").get<int>(),
      .maxScore = value.at("maxScore").get<int>(),
      .maxCombo = value.at("maxCombo").get<int>(),
      .comboBreak = value.at("comboBreak").get<int>(),
      .pGreat = value.at("pGreat").get<int>(),
      .great = value.at("great").get<int>(),
      .good = value.at("good").get<int>(),
      .bad = value.at("bad").get<int>(),
      .poor = value.at("poor").get<int>(),
      .kPoor = value.at("kPoor").get<int>(),
      .fast = value.at("fast").get<int>(),
      .slow = value.at("slow").get<int>(),
      .pGreatFast = value.at("pGreatFast").get<int>(),
      .pGreatSlow = value.at("pGreatSlow").get<int>(),
      .judgementTimingBreakdownAvailable =
          value.at("judgementTimingBreakdownAvailable").get<bool>(),
      .earlyPGreat = value.at("earlyPGreat").get<int>(),
      .latePGreat = value.at("latePGreat").get<int>(),
      .earlyGreat = value.at("earlyGreat").get<int>(),
      .lateGreat = value.at("lateGreat").get<int>(),
      .earlyGood = value.at("earlyGood").get<int>(),
      .lateGood = value.at("lateGood").get<int>(),
      .earlyBad = value.at("earlyBad").get<int>(),
      .lateBad = value.at("lateBad").get<int>(),
      .earlyPoor = value.at("earlyPoor").get<int>(),
      .latePoor = value.at("latePoor").get<int>(),
      .gaugeHistory = value.at("gaugeHistory").get<std::vector<float>>(),
      .finalGauge = value.at("finalGauge").get<float>(),
      .clearType = value.at("clearType").get<int>(),
      .playedAtUnixMillis = value.at("playedAtUnixMillis").get<std::int64_t>(),
      .provenance = std::move(*provenance),
  };
}

} // namespace

std::optional<IrSubmissionSnapshot> captureIrSubmissionSnapshot(
    const result_persistence::PersistedChartResult &result,
    std::string &diagnostic) noexcept {
  diagnostic.clear();
  if (!result_persistence::validatePersistedChartResult(result, diagnostic) ||
      result.resultFingerprint.empty()) {
    if (diagnostic.empty()) {
      diagnostic = "IR snapshot requires an integrity-checked result";
    }
    return std::nullopt;
  }
  const auto built = makeIrSubmission(result);
  if (!built.value.has_value()) {
    diagnostic = built.diagnostic;
    return std::nullopt;
  }
  try {
    IrSubmissionSnapshot snapshot{.submission = *built.value};
    snapshot.fingerprint = calculatedFingerprint(snapshot);
    return snapshot;
  } catch (...) {
    diagnostic = "IR snapshot capture failed";
    return std::nullopt;
  }
}

std::optional<std::string>
serializeIrSubmissionSnapshot(const IrSubmissionSnapshot &snapshot,
                              std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    if (snapshot.schemaVersion != IrSubmissionSnapshot::kSchemaVersion) {
      diagnostic = "IR snapshot schema version is unsupported";
      return std::nullopt;
    }
    if (!validateSubmission(snapshot.submission, diagnostic)) {
      return std::nullopt;
    }
    const std::string expected = calculatedFingerprint(snapshot);
    if (!lowerHex(snapshot.fingerprint, 64) ||
        snapshot.fingerprint != expected) {
      diagnostic = "IR snapshot fingerprint is malformed or inconsistent";
      return std::nullopt;
    }
    Json root = fingerprintPayload(snapshot);
    root["fingerprint"] = snapshot.fingerprint;
    std::string serialized = root.dump();
    if (serialized.size() > kMaximumIrSubmissionSnapshotBytes) {
      diagnostic = "IR snapshot is oversized";
      return std::nullopt;
    }
    return serialized;
  } catch (...) {
    diagnostic = "IR snapshot serialization failed";
    return std::nullopt;
  }
}

std::optional<IrSubmissionSnapshot>
deserializeIrSubmissionSnapshot(std::string_view serialized,
                                std::string_view expectedFingerprint,
                                std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    if (serialized.empty() ||
        serialized.size() > kMaximumIrSubmissionSnapshotBytes) {
      diagnostic = "IR snapshot is empty or oversized";
      return std::nullopt;
    }
    const Json root = Json::parse(serialized.begin(), serialized.end());
    if (!exactKeys(root, {"schemaVersion", "submission", "fingerprint"})) {
      diagnostic = "IR snapshot fields are malformed";
      return std::nullopt;
    }
    IrSubmissionSnapshot snapshot{
        .schemaVersion = root.at("schemaVersion").get<int>(),
        .submission = submissionFromJson(root.at("submission")),
        .fingerprint = root.at("fingerprint").get<std::string>(),
    };
    std::string serializationDiagnostic;
    const auto canonical =
        serializeIrSubmissionSnapshot(snapshot, serializationDiagnostic);
    if (!canonical.has_value()) {
      diagnostic = std::move(serializationDiagnostic);
      return std::nullopt;
    }
    if (*canonical != serialized) {
      diagnostic = "IR snapshot JSON is not canonical";
      return std::nullopt;
    }
    if (!expectedFingerprint.empty() &&
        snapshot.fingerprint != expectedFingerprint) {
      diagnostic = "IR snapshot fingerprint does not match its record";
      return std::nullopt;
    }
    return snapshot;
  } catch (...) {
    diagnostic = "IR snapshot deserialization failed";
    return std::nullopt;
  }
}

} // namespace ir
