#include "ir/IrSubmissionSnapshot.h"
#include "replay/ReplayPlaybackData.h"

#include "nlohmann/json.hpp"

#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

result_persistence::PersistedChartResult validResult() {
  result_persistence::PersistedChartResult result;
  result.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  result.score.chartPath = "sample/song.bms";
  result.score.chartMd5 = repeated('b', 32);
  result.score.chartSha256 = repeated('a', 64);
  result.score.chartTitle = "Title";
  result.score.chartArtist = "Artist";
  result.score.longNoteMode = 1;
  result.score.score = 7;
  result.score.maxScore = 10;
  result.score.maxCombo = 4;
  result.score.comboBreak = 1;
  result.score.pGreat = 3;
  result.score.great = 1;
  result.score.good = 1;
  result.score.fast = 2;
  result.score.slow = 1;
  result.score.finalGauge = 82.5F;
  result.score.clearType = kClearTypeNormalClearRank;
  result.score.provenance = ScoreProvenance::Legacy();
  result.keyMode = 7;
  result.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  result.judgementTiming = result_persistence::ChartJudgementTiming{};
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 1};
  result.judgementTiming->byJudgement[Great] = {.fast = 1, .slow = 0};
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

ir::IrSubmissionSnapshot
capture(const result_persistence::PersistedChartResult &result) {
  std::string diagnostic;
  auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  expect(snapshot.has_value(), "valid result captures an IR snapshot");
  if (!snapshot.has_value()) {
    std::cerr << "capture diagnostic: " << diagnostic << '\n';
    return {};
  }
  return *snapshot;
}

void testCaptureAndReplayIndependence() {
  const auto result = validResult();
  const auto snapshot = capture(result);
  expect(snapshot.schemaVersion == ir::IrSubmissionSnapshot::kSchemaVersion,
         "snapshot uses the current schema");
  expect(snapshot.submission.attemptId == *result.attemptId &&
             snapshot.submission.keyMode == result.keyMode &&
             snapshot.submission.playedAtUnixMillis ==
                 result.playedAtUnixMillis,
         "snapshot contains provider-neutral completed-result facts");
  expect(snapshot.submission.judgementTimingBreakdownAvailable &&
             snapshot.submission.earlyPGreat == 2 &&
             snapshot.submission.latePGreat == 1 &&
             snapshot.submission.pGreatFast == 1 &&
             snapshot.submission.pGreatSlow == 1,
         "snapshot preserves judgement timing facts");

  replay::ReplayPlaybackData replay;
  replay.setup.chartSha256 = repeated('c', 64);
  replay.input.emplace_back();
  replay.touchSamples.emplace_back();
  replay.laneCoverEvents.emplace_back();
  replay.legacy = replay::LegacyPlaybackTrack{};
  const auto resultFingerprint = result.resultFingerprint;
  const auto snapshotFingerprint = snapshot.fingerprint;
  replay.setup.chartSha256[0] = 'd';
  replay.input.front().pressed = true;
  replay.touchSamples.front().x = 0.75F;
  replay.laneCoverEvents.front().noteStartPositionPercent = 90;
  replay.legacy->events.emplace_back();
  expect(result.resultFingerprint == resultFingerprint &&
             snapshot.fingerprint == snapshotFingerprint,
         "raw replay mutation cannot affect result or snapshot integrity");

  auto changedResult = result;
  ++changedResult.playedAtUnixMillis;
  changedResult.resultFingerprint =
      result_persistence::resultFingerprint(changedResult);
  expect(capture(changedResult).fingerprint != snapshot.fingerprint,
         "IR play-time mutation changes snapshot fingerprint");
  changedResult = result;
  changedResult.adoptedGaugeHistory.push_back(81.0F);
  changedResult.resultFingerprint =
      result_persistence::resultFingerprint(changedResult);
  expect(capture(changedResult).fingerprint != snapshot.fingerprint,
         "IR gauge-history mutation changes snapshot fingerprint");
  changedResult = result;
  changedResult.score.provenance.clubMode = true;
  changedResult.resultFingerprint =
      result_persistence::resultFingerprint(changedResult);
  expect(capture(changedResult).fingerprint != snapshot.fingerprint,
         "IR provenance mutation changes snapshot fingerprint");
}

void testCanonicalSerializationAndTamperChecks() {
  const auto snapshot = capture(validResult());
  std::string diagnostic;
  const auto serialized =
      ir::serializeIrSubmissionSnapshot(snapshot, diagnostic);
  expect(serialized.has_value() && diagnostic.empty(),
         "valid snapshot serializes canonically");
  if (!serialized) {
    return;
  }
  constexpr std::string_view expectedJson =
      R"json({"schemaVersion":1,"submission":{"attemptId":"123e4567-e89b-42d3-a456-426614174000","keyMode":7,"chartMd5":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","chartSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","score":7,"maxScore":10,"maxCombo":4,"comboBreak":1,"pGreat":3,"great":1,"good":1,"bad":0,"poor":0,"kPoor":0,"fast":2,"slow":1,"pGreatFast":1,"pGreatSlow":1,"judgementTimingBreakdownAvailable":true,"earlyPGreat":2,"latePGreat":1,"earlyGreat":1,"lateGreat":0,"earlyGood":1,"lateGood":0,"earlyBad":0,"lateBad":0,"earlyPoor":0,"latePoor":0,"gaugeHistory":[20.0,48.5,82.5],"finalGauge":82.5,"clearType":300,"playedAtUnixMillis":1700000000123,"provenance":{"schemaVersion":4,"ruleset":{"id":"legacy-unknown","version":0,"scoringModel":"legacy-unknown","judgementModel":"legacy-unknown","gaugeModel":"legacy-unknown"},"stages":[],"gaugeType":"normal","gaugeProfile":"standard","gaugeAutoShift":0,"gaugeAutoShiftLowerBound":"assisted-easy","player1":{"option":"NORMAL","seed":null},"player2":{"option":"NORMAL","seed":null},"assistOption":"OFF","inputDevices":[],"autoPlay":false,"practice":false,"clubMode":false,"playback":{"percent":100,"mode":"pitch-shift"},"judgeWindowScalePercent":100,"startingGaugePercent":null,"eligibility":"legacy-unverified"}},"fingerprint":"220f0f820d9f54d3f8ead81cc092049b36a863b6cad7ead4665a5ff1ee8d778b"})json";
  expect(*serialized == expectedJson,
         "snapshot JSON matches the exact schema-1 canonical fixture");
  auto decoded = ir::deserializeIrSubmissionSnapshot(
      *serialized, snapshot.fingerprint, diagnostic);
  expect(decoded.has_value() && *decoded == snapshot,
         "canonical snapshot JSON round trips exactly");
  auto serializedAgain =
      decoded ? ir::serializeIrSubmissionSnapshot(*decoded, diagnostic)
              : std::nullopt;
  expect(serializedAgain == serialized,
         "round-trip serialization is byte-for-byte stable");

  expect(!ir::deserializeIrSubmissionSnapshot(*serialized, repeated('0', 64),
                                              diagnostic),
         "record fingerprint mismatch is rejected");

  auto root = nlohmann::ordered_json::parse(*serialized);
  root["fingerprint"] = repeated('0', 64);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "payload fingerprint tampering is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root["schemaVersion"] = 2;
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "unknown snapshot version is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root.erase("submission");
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "missing required snapshot field is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root["unexpected"] = true;
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "extra snapshot field is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root["submission"]["unexpected"] = true;
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "extra submission field is rejected");
  expect(
      !ir::deserializeIrSubmissionSnapshot(" " + *serialized, {}, diagnostic),
      "noncanonical JSON spelling is rejected");

  auto wrongSchema = snapshot;
  ++wrongSchema.schemaVersion;
  expect(!ir::serializeIrSubmissionSnapshot(wrongSchema, diagnostic),
         "unsupported in-memory schema is rejected");
  auto changed = snapshot;
  ++changed.submission.good;
  expect(!ir::serializeIrSubmissionSnapshot(changed, diagnostic),
         "field mutation with stale fingerprint is rejected");
}

void testValidationAndFloatCanonicalization() {
  std::string diagnostic;
  auto invalid = validResult();
  invalid.attemptId.reset();
  invalid.resultFingerprint.clear();
  expect(!ir::captureIrSubmissionSnapshot(invalid, diagnostic),
         "migrated result without attempt ID cannot enter IR");
  invalid = validResult();
  invalid.playedAtUnixMillis = 0;
  invalid.resultFingerprint.clear();
  expect(!ir::captureIrSubmissionSnapshot(invalid, diagnostic),
         "zero play completion time cannot enter IR");
  invalid = validResult();
  invalid.adoptedGaugeHistory.push_back(std::numeric_limits<float>::infinity());
  invalid.resultFingerprint.clear();
  expect(!ir::captureIrSubmissionSnapshot(invalid, diagnostic),
         "nonfinite gauge sample cannot enter IR");

  auto positive = validResult();
  positive.adoptedGaugeHistory = {0.0F};
  positive.resultFingerprint = result_persistence::resultFingerprint(positive);
  auto negative = positive;
  negative.adoptedGaugeHistory = {-0.0F};
  negative.resultFingerprint = result_persistence::resultFingerprint(negative);
  const auto positiveSnapshot = capture(positive);
  const auto negativeSnapshot = capture(negative);
  expect(positiveSnapshot.fingerprint != negativeSnapshot.fingerprint,
         "snapshot canonicalization preserves signed-zero float bits");
}

} // namespace

int main() {
  testCaptureAndReplayIndependence();
  testCanonicalSerializationAndTamperChecks();
  testValidationAndFloatCanonicalization();
  if (failures != 0) {
    std::cerr << failures << " IR submission snapshot test(s) failed\n";
    return 1;
  }
  std::cout << "IR submission snapshot tests passed\n";
  return 0;
}
