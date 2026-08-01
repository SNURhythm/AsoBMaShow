#include "IrSubmissionSnapshot.h"

#include "../CanonicalDigest.h"
#include "../FileChecksum.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

namespace ir {
namespace {

using Json = nlohmann::ordered_json;

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
  if (!provenance) {
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

std::optional<IrSubmissionSnapshot>
captureIrSubmissionSnapshot(const result_persistence::ModernChartResult &result,
                            std::string &diagnostic) noexcept {
  diagnostic.clear();
  const auto built = makeIrSubmission(result);
  if (!built.value) {
    diagnostic = built.diagnostic;
    return std::nullopt;
  }
  try {
    IrSubmissionSnapshot snapshot{.submission = *built.value};
    snapshot.fingerprint = calculatedFingerprint(snapshot);
    const auto serialized = serializeIrSubmissionSnapshot(snapshot, diagnostic);
    if (!serialized) {
      return std::nullopt;
    }
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
    if (!validateIrSubmission(snapshot.submission, diagnostic)) {
      return std::nullopt;
    }
    const std::string expected = calculatedFingerprint(snapshot);
    if (!canonical_digest::isCanonicalLowerHex(snapshot.fingerprint, 64) ||
        snapshot.fingerprint != expected) {
      diagnostic = "IR snapshot fingerprint is malformed or inconsistent";
      return std::nullopt;
    }
    Json root = fingerprintPayload(snapshot);
    root["fingerprint"] = snapshot.fingerprint;
    std::string serialized = root.dump();
    if (!durable_payload::withinLimit(
            serialized.size(), durable_payload::kMaximumIrSnapshotBytes)) {
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
        !durable_payload::withinLimit(
            serialized.size(), durable_payload::kMaximumIrSnapshotBytes)) {
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
    if (!canonical) {
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
