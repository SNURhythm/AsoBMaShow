#include "FileChecksum.h"
#include "ModernResult.h"
#include "ir/IrSubmissionSnapshot.h"
#include "replay/ReplayFileLifecycle.h"
#include "replay/ReplayPlayback.h"

#include "nlohmann/json.hpp"

#include <iostream>
#include <limits>
#include <optional>
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

result_persistence::ModernChartResult validResult() {
  result_persistence::ModernChartResult result;
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
  result.adoptedGaugeType = GaugeType::Normal;
  result.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  result.judgementTiming = result_persistence::ChartJudgementTiming{};
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 1};
  result.judgementTiming->byJudgement[Great] = {.fast = 1, .slow = 0};
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return result;
}

ir::IrSubmissionSnapshot
capture(const result_persistence::ModernChartResult &result) {
  std::string diagnostic;
  auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  expect(snapshot.has_value(), "valid result captures an IR snapshot");
  if (!snapshot) {
    std::cerr << "capture diagnostic: " << diagnostic << '\n';
    return {};
  }
  return *snapshot;
}

void refreshJsonFingerprint(nlohmann::ordered_json &root) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["schemaVersion"] = root["schemaVersion"];
  payload["submission"] = root["submission"];
  root["fingerprint"] = file_checksum::sha256(payload.dump());
}

void testCaptureAndReplayIndependence() {
  const auto result = validResult();
  const auto snapshot = capture(result);
  expect(snapshot.schemaVersion == ir::IrSubmissionSnapshot::kSchemaVersion &&
             snapshot.submission.attemptId == result.attemptId &&
             snapshot.submission.keyMode == result.keyMode &&
             snapshot.submission.playedAtUnixMillis ==
                 result.playedAtUnixMillis,
         "snapshot contains provider-neutral completion facts");
  expect(snapshot.submission.judgementTimingBreakdownAvailable &&
             snapshot.submission.earlyPGreat == 2 &&
             snapshot.submission.latePGreat == 1 &&
             snapshot.submission.pGreatFast == 1 &&
             snapshot.submission.pGreatSlow == 1,
         "snapshot preserves detailed judgement timing");

  replay::ReplayPlaybackData playback;
  playback.setup.chart.sha256 = repeated('c', 64);
  playback.input.emplace_back();
  playback.touchSamples.emplace_back();
  playback.laneCoverEvents.emplace_back();
  replay::ReplayFileMetadata file{
      .relativePath = "replay/ignored.brd",
      .sha256 = repeated('d', 64),
      .compressedSize = 12,
      .codecVersion = 3,
  };
  const auto fingerprint = snapshot.fingerprint;
  playback.setup.chart.sha256[0] = 'e';
  playback.input.front().pressed = true;
  playback.touchSamples.front().x = 0.75F;
  playback.laneCoverEvents.front().noteStartPositionPercent = 90;
  file.sha256[0] = 'f';
  expect(snapshot.fingerprint == fingerprint,
         "raw playback and replay-file mutations cannot affect IR integrity");

  auto changed = result;
  ++changed.playedAtUnixMillis;
  changed.resultFingerprint =
      result_persistence::modernResultFingerprint(changed);
  expect(capture(changed).fingerprint != fingerprint,
         "completion time is authenticated by the snapshot");
  changed = result;
  changed.adoptedGaugeHistory.push_back(81.0F);
  changed.resultFingerprint =
      result_persistence::modernResultFingerprint(changed);
  expect(capture(changed).fingerprint != fingerprint,
         "gauge history is authenticated by the snapshot");
  changed = result;
  changed.score.provenance.clubMode = true;
  changed.resultFingerprint =
      result_persistence::modernResultFingerprint(changed);
  expect(capture(changed).fingerprint != fingerprint,
         "provenance is authenticated by the snapshot");
}

void testCanonicalRoundTripAndTamperChecks() {
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
      R"json({"schemaVersion":1,"submission":{"attemptId":"123e4567-e89b-42d3-a456-426614174000","keyMode":7,"chartMd5":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","chartSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","score":7,"maxScore":10,"maxCombo":4,"comboBreak":1,"pGreat":3,"great":1,"good":1,"bad":0,"poor":0,"kPoor":0,"fast":2,"slow":1,"pGreatFast":1,"pGreatSlow":1,"judgementTimingBreakdownAvailable":true,"earlyPGreat":2,"latePGreat":1,"earlyGreat":1,"lateGreat":0,"earlyGood":1,"lateGood":0,"earlyBad":0,"lateBad":0,"earlyPoor":0,"latePoor":0,"gaugeHistory":[20.0,48.5,82.5],"finalGauge":82.5,"clearType":300,"playedAtUnixMillis":1700000000123,"provenance":{"schemaVersion":5,"ruleset":{"id":"legacy-unknown","version":0,"scoringModel":"legacy-unknown","judgementModel":"legacy-unknown","gaugeModel":"legacy-unknown"},"stages":[],"gaugeType":"normal","gaugeProfile":"standard","gaugeAutoShift":0,"gaugeAutoShiftLowerBound":"assisted-easy","player1":{"option":"NORMAL","seed":null},"player2":{"option":"NORMAL","seed":null},"doublePlayFlip":false,"assistOption":"OFF","inputDevices":[],"autoPlay":false,"practice":false,"clubMode":false,"playback":{"percent":100,"mode":"pitch-shift"},"judgeWindowScalePercent":100,"startingGaugePercent":null,"eligibility":"legacy-unverified"}},"fingerprint":"72b0a1e5e2c06bac59067601208c7da7344721ce51eec100065cf97636750196"})json";
  if (*serialized != expectedJson) {
    std::cerr << "actual snapshot JSON: " << *serialized << '\n';
  }
  expect(*serialized == expectedJson,
         "snapshot JSON matches the exact schema-1 golden fixture");
  const auto decoded = ir::deserializeIrSubmissionSnapshot(
      *serialized, snapshot.fingerprint, diagnostic);
  expect(decoded == snapshot, "canonical snapshot round trips exactly");
  const auto serializedAgain =
      decoded ? ir::serializeIrSubmissionSnapshot(*decoded, diagnostic)
              : std::nullopt;
  expect(serializedAgain == serialized,
         "round-trip serialization is byte-for-byte stable");

  expect(!ir::deserializeIrSubmissionSnapshot(*serialized, repeated('0', 64),
                                              diagnostic),
         "database fingerprint disagreement is rejected");
  auto root = nlohmann::ordered_json::parse(*serialized);
  root["fingerprint"] = repeated('0', 64);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "payload fingerprint tampering is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root["schemaVersion"] = 2;
  refreshJsonFingerprint(root);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "unknown snapshot schema is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root.erase("submission");
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "missing root field is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root["unexpected"] = true;
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "extra root field is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root["submission"]["unexpected"] = true;
  refreshJsonFingerprint(root);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "extra submission field is rejected");
  root = nlohmann::ordered_json::parse(*serialized);
  root["submission"].erase("good");
  refreshJsonFingerprint(root);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "missing submission field is rejected");
  expect(
      !ir::deserializeIrSubmissionSnapshot(" " + *serialized, {}, diagnostic),
      "noncanonical JSON spelling is rejected");

  auto stale = snapshot;
  ++stale.submission.good;
  expect(!ir::serializeIrSubmissionSnapshot(stale, diagnostic),
         "in-memory mutation with a stale fingerprint is rejected");
}

void testStrictValidationAndBounds() {
  std::string diagnostic;
  auto invalidSubmission = capture(validResult()).submission;
  invalidSubmission.keyMode = 6;
  expect(!ir::validateIrSubmission(invalidSubmission, diagnostic),
         "IR validation consumes the shared supported-key-mode contract");

  auto invalid = validResult();
  invalid.adoptedGaugeHistory.push_back(std::numeric_limits<float>::infinity());
  invalid.resultFingerprint =
      result_persistence::modernResultFingerprint(invalid);
  expect(!ir::captureIrSubmissionSnapshot(invalid, diagnostic),
         "non-finite modern result facts cannot enter an IR snapshot");
  invalid = validResult();
  invalid.judgementTiming->byJudgement[PGreat].fast = 3;
  invalid.resultFingerprint =
      result_persistence::modernResultFingerprint(invalid);
  expect(!ir::captureIrSubmissionSnapshot(invalid, diagnostic),
         "timing disagreement cannot enter an IR snapshot");

  auto positiveZero = validResult();
  positiveZero.adoptedGaugeHistory = {0.0F};
  positiveZero.resultFingerprint =
      result_persistence::modernResultFingerprint(positiveZero);
  auto negativeZero = positiveZero;
  negativeZero.adoptedGaugeHistory = {-0.0F};
  negativeZero.resultFingerprint =
      result_persistence::modernResultFingerprint(negativeZero);
  expect(capture(positiveZero).fingerprint != capture(negativeZero).fingerprint,
         "snapshot canonicalization preserves signed-zero float bits");

  const auto snapshot = capture(validResult());
  const auto serialized =
      ir::serializeIrSubmissionSnapshot(snapshot, diagnostic);
  expect(serialized.has_value(),
         "valid serialization is available for malformed fixtures");
  if (!serialized) {
    return;
  }
  auto root = nlohmann::ordered_json::parse(*serialized);
  root["submission"]["provenance"]["schemaVersion"] = 999;
  refreshJsonFingerprint(root);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "invalid provenance is rejected even with a matching fingerprint");
  root = nlohmann::ordered_json::parse(*serialized);
  root["submission"]["earlyPGreat"] = std::numeric_limits<int>::max();
  root["submission"]["latePGreat"] = 1;
  refreshJsonFingerprint(root);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "timing arithmetic rejects overflow safely");
  root = nlohmann::ordered_json::parse(*serialized);
  root["submission"]["pGreatFast"] = 3;
  root["submission"]["pGreatSlow"] = 3;
  refreshJsonFingerprint(root);
  expect(!ir::deserializeIrSubmissionSnapshot(root.dump(), {}, diagnostic),
         "PGREAT fast and slow evidence cannot exceed the result total");
  expect(!ir::deserializeIrSubmissionSnapshot(
             std::string(ir::kMaximumIrSubmissionSnapshotBytes + 1U, 'x'), {},
             diagnostic),
         "oversized snapshot input is rejected before parsing");

  auto large = validResult();
  large.adoptedGaugeHistory.assign(100'000, 20.0F);
  large.resultFingerprint = result_persistence::modernResultFingerprint(large);
  const auto largeSnapshot = capture(large);
  const auto largeSerialized =
      ir::serializeIrSubmissionSnapshot(largeSnapshot, diagnostic);
  expect(largeSerialized && largeSerialized->size() > 256U * 1024U &&
             largeSerialized->size() <= ir::kMaximumIrSubmissionSnapshotBytes,
         "large valid snapshot uses the shared 16 MiB payload ceiling");
  expect(largeSerialized && ir::deserializeIrSubmissionSnapshot(
                                *largeSerialized, largeSnapshot.fingerprint,
                                diagnostic) == largeSnapshot,
         "large valid snapshot round trips below the shared ceiling");
}

} // namespace

int main() {
  testCaptureAndReplayIndependence();
  testCanonicalRoundTripAndTamperChecks();
  testStrictValidationAndBounds();
  if (failures != 0) {
    std::cerr << failures << " IR submission snapshot test(s) failed\n";
    return 1;
  }
  std::cout << "IR submission snapshot tests passed\n";
  return 0;
}
