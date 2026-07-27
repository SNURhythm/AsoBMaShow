#include "replay/ChartReplayContext.h"

#include "ScoreProvenance.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace replay;

constexpr std::string_view kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

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

result_persistence::ModernChartResult savedResult() {
  result_persistence::ModernChartResult value;
  value.resultId = 17;
  value.attemptId = std::string(kAttemptId);
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = repeated('b', 32);
  value.score.chartSha256 = repeated('a', 64);
  value.score.chartTitle = "Title";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = 4;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = 82.5F;
  value.score.clearType = kClearTypeNormalClearRank;

  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta.MD5 = value.score.chartMd5;
  provenance.chartMeta.SHA256 = value.score.chartSha256;
  provenance.chartMeta.KeyMode = 7;
  provenance.chartMeta.Rank = 2;
  provenance.chartMeta.TotalNotes = 5;
  provenance.chartMeta.HasTotal = true;
  provenance.chartMeta.Total = 200.0;
  provenance.longNoteMode = value.score.longNoteMode;
  provenance.sourceJudgeRank = 2;
  provenance.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  provenance.totalNotes = 5;
  provenance.authoredGaugeTotal = 200.0;
  provenance.effectiveGaugeTotal = 200.0;
  provenance.startingGaugePercent = 20;
  provenance.inputDevices = {InputDeviceCategory::Keyboard};
  value.score.provenance = makeScoreProvenance(provenance);
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'123LL;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  return value;
}

ReplayChartDocument replayDocument(
    const result_persistence::ModernChartResult &saved) {
  ReplayChartDocument replay;
  replay.timeBounds = {.completionSongTimeMicros = 5'000'000};
  replay.playback.setup.chart = {.md5 = saved.score.chartMd5,
                                 .sha256 = saved.score.chartSha256,
                                 .keyMode = saved.keyMode};
  replay.playback.setup.longNoteMode = saved.score.longNoteMode;
  replay.playback.setup.initialGaugeType = saved.score.provenance.gaugeType;
  replay.playback.setup.gaugeProfile = saved.score.provenance.gaugeProfile;
  replay.playback.setup.gaugeAutoShift = saved.score.provenance.gaugeAutoShift;
  replay.playback.setup.gaugeAutoShiftLowerBound =
      saved.score.provenance.gaugeAutoShiftLowerBound;
  replay.playback.setup.ruleset = saved.score.provenance.ruleset;
  replay.playback.setup.playback = saved.score.provenance.playback;
  replay.playback.setup.candidateSelection =
      saved.score.provenance.stages.front().candidateSelection;
  replay.playback.setup.judgeWindowScalePercent =
      saved.score.provenance.judgeWindowScalePercent;
  replay.playback.setup.startingGaugePercent = 20.0F;
  replay.playback.input = {
      {.songTimeMicros = -1'000,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1'000,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  return replay;
}

ModernReplayFileReference replayReference(
    const result_persistence::ModernChartResult &saved) {
  std::string diagnostic;
  const auto stem = chartStem(saved.score.chartSha256,
                              saved.score.longNoteMode, false, diagnostic);
  const auto identity = stem ? pathForStem(*stem, 0, diagnostic) : std::nullopt;
  return {.id = 8,
          .resultId = saved.resultId,
          .identity = *identity,
          .metadata = {.relativePath = identity->relativePath,
                       .sha256 = repeated('c', 64),
                       .compressedSize = 3,
                       .codecVersion = BeatorajaReplayCodec::kCodecVersion}};
}

ParsedChartReplayFacts parsedFacts(
    const result_persistence::ModernChartResult &saved) {
  return {.chart = {.md5 = saved.score.chartMd5,
                    .sha256 = saved.score.chartSha256,
                    .keyMode = saved.keyMode},
          .longNoteMode = saved.score.longNoteMode,
          .timeBounds = ReplayTimeBounds{
              .completionSongTimeMicros = 5'000'000}};
}

ParsedChartReplayFacts identityOnlyFacts(
    const result_persistence::ModernChartResult &saved) {
  auto facts = parsedFacts(saved);
  facts.timeBounds.reset();
  return facts;
}

struct Harness {
  result_persistence::ModernChartResult result = savedResult();
  ReplayChartDocument replay = replayDocument(result);
  std::vector<std::string> calls;
  ModernChartResultReadStatus resultStatus =
      ModernChartResultReadStatus::Loaded;
  bool attachReplay = true;
  ReplayFileState fileState = ReplayFileState::Available;
  bool throwDuringFileRead = false;
  bool unsupportedExtension = false;
  bool decodeChart = true;
  std::optional<ReplayDecodeContext> decodeContext;

  ChartReplayContext makeContext() {
    return ChartReplayContext(ChartReplayContextDependencies{
        .loadResult = [this](std::string_view) {
          calls.emplace_back("result");
          ModernChartResultReadOutcome outcome{.status = resultStatus};
          if (resultStatus == ModernChartResultReadStatus::Loaded) {
            outcome.record = ModernChartResultRecord{
                .result = result,
                .replayFile = attachReplay
                                  ? std::optional(replayReference(result))
                                  : std::nullopt};
          }
          return outcome;
        },
        .readVerifiedFile = [this](const ReplayFileMetadata &) {
          calls.emplace_back("file");
          if (throwDuringFileRead) {
            throw std::runtime_error("injected file read failure");
          }
          ReplayFileReadOutcome outcome{.state = fileState};
          if (fileState == ReplayFileState::Available) {
            outcome.bytes = std::vector<std::byte>{std::byte{1}, std::byte{2},
                                                   std::byte{3}};
          }
          return outcome;
        },
        .decode = [this](std::span<const std::byte>,
                         const ReplayDecodeContext &context) {
          calls.emplace_back("decode");
          decodeContext = context;
          ReplayDecodeOutcome outcome;
          outcome.unsupportedAsoExtension = unsupportedExtension;
          if (decodeChart) {
            outcome.chart = replay;
            outcome.stageSources = {ReplayStageDecodeSource::AsoExtension};
          }
          return outcome;
        }});
  }
};

void testVerifiedContextUsesStrictLoadOrder() {
  Harness harness;
  auto context = harness.makeContext();
  const auto loaded = context.load(kAttemptId, parsedFacts(harness.result));
  expect(loaded.state == ChartReplayContextState::Ready &&
             loaded.resultAvailable() && loaded.replayAvailable() &&
             loaded.verified && loaded.verified->document == harness.replay,
         "verified modern replay exposes saved result and decoded document");
  expect(harness.calls == std::vector<std::string>{"result", "file", "decode"},
         "context loads result before verified bytes and decode");
  expect(loaded.replayState() == ReplayState::Verified,
         "ready context maps to the shared verified capability state");
}

void testEmbeddedAsoCompletionBoundNeedsNoConsumerEstimate() {
  Harness harness;
  auto context = harness.makeContext();
  const auto loaded = context.load(kAttemptId, identityOnlyFacts(harness.result));
  expect(loaded.state == ChartReplayContextState::Ready &&
             loaded.replayAvailable(),
         "verified local replay can use its embedded completion bound");
  expect(harness.decodeContext.has_value() &&
             harness.decodeContext->stageKeyModes ==
                 std::vector<int>{harness.result.keyMode} &&
             harness.decodeContext->stageTimeBounds.empty(),
         "context does not invent an exact completion bound in a consumer");
}

void testFileFailuresRetainResultAndFailReplayClosed() {
  struct Case {
    ReplayFileState file;
    ChartReplayContextState state;
    ReplayState capabilityState;
  };
  const std::vector<Case> cases{
      {ReplayFileState::Missing, ChartReplayContextState::FileMissing,
       ReplayState::Missing},
      {ReplayFileState::Corrupt, ChartReplayContextState::FileCorrupt,
       ReplayState::Corrupt},
      {ReplayFileState::Unsafe, ChartReplayContextState::FileUnsafe,
       ReplayState::Mismatched},
      {ReplayFileState::IoFailure, ChartReplayContextState::FileIoFailure,
       ReplayState::Missing},
  };
  for (const auto &test : cases) {
    Harness harness;
    harness.fileState = test.file;
    auto context = harness.makeContext();
    const auto loaded = context.load(kAttemptId, parsedFacts(harness.result));
    expect(loaded.state == test.state && loaded.resultAvailable() &&
               !loaded.replayAvailable() &&
               loaded.replayState() == test.capabilityState,
           "file failure disables only replay-dependent state");
    expect(harness.calls == std::vector<std::string>{"result", "file"},
           "failed file verification prevents decode");
  }

  Harness absent;
  absent.attachReplay = false;
  auto context = absent.makeContext();
  const auto loaded = context.load(kAttemptId, parsedFacts(absent.result));
  expect(loaded.state == ChartReplayContextState::ReplayNotAttached &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "result without file reference remains recallable");

  Harness exceptional;
  exceptional.throwDuringFileRead = true;
  auto exceptionalContext = exceptional.makeContext();
  const auto exceptionalLoad =
      exceptionalContext.load(kAttemptId, parsedFacts(exceptional.result));
  expect(exceptionalLoad.state == ChartReplayContextState::FileIoFailure &&
             exceptionalLoad.resultAvailable() &&
             !exceptionalLoad.replayAvailable(),
         "unexpected replay I/O failure still preserves result recall");
}

void testParsedIdentityAndLongNoteAgreementPrecedeFileAccess() {
  Harness harness;
  auto context = harness.makeContext();
  auto selected = parsedFacts(harness.result);
  selected.chart.sha256 = repeated('d', 64);
  auto loaded = context.load(kAttemptId, selected);
  expect(loaded.state == ChartReplayContextState::ChartMismatch &&
             loaded.resultAvailable() && !loaded.replayAvailable() &&
             harness.calls == std::vector<std::string>{"result"},
         "wrong parsed chart hash rejects before file access");

  harness.calls.clear();
  selected = parsedFacts(harness.result);
  selected.chart.keyMode = 14;
  loaded = context.load(kAttemptId, selected);
  expect(loaded.state == ChartReplayContextState::ChartMismatch &&
             harness.calls == std::vector<std::string>{"result"},
         "wrong parsed key mode rejects before file access");

  harness.calls.clear();
  selected = parsedFacts(harness.result);
  ++selected.longNoteMode;
  loaded = context.load(kAttemptId, selected);
  expect(loaded.state == ChartReplayContextState::LongNoteModeMismatch &&
             harness.calls == std::vector<std::string>{"result"},
         "wrong effective long-note mode rejects before file access");
}

void testDecodedReplayMustAgreeWithResultAndSupportedContract() {
  Harness harness;
  harness.replay.playback.setup.judgeWindowScalePercent = 75;
  auto context = harness.makeContext();
  auto loaded = context.load(kAttemptId, parsedFacts(harness.result));
  expect(loaded.state == ChartReplayContextState::SharedFactsMismatch &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "decoded setup disagreement cannot enable replay actions");

  Harness future;
  future.unsupportedExtension = true;
  auto futureContext = future.makeContext();
  loaded = futureContext.load(kAttemptId, parsedFacts(future.result));
  expect(loaded.state == ChartReplayContextState::UnsupportedExtension &&
             loaded.resultAvailable() && !loaded.replayAvailable() &&
             loaded.replayState() == ReplayState::UnsupportedExtension,
         "future Aso extension fails closed even with stock fallback bytes");

  Harness malformed;
  malformed.decodeChart = false;
  auto malformedContext = malformed.makeContext();
  loaded = malformedContext.load(kAttemptId, parsedFacts(malformed.result));
  expect(loaded.state == ChartReplayContextState::DecodeFailed &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "non-chart BRD cannot become a chart replay context");
}

void testInvalidResultNeverTouchesReplayFile() {
  Harness harness;
  harness.result.resultFingerprint = "invalid";
  auto context = harness.makeContext();
  const auto loaded = context.load(kAttemptId, parsedFacts(harness.result));
  expect(loaded.state == ChartReplayContextState::ResultInvalid &&
             !loaded.resultAvailable() && !loaded.replayAvailable() &&
             harness.calls == std::vector<std::string>{"result"},
         "invalid result row is rejected before replay ownership is trusted");
}

} // namespace

int main() {
  testVerifiedContextUsesStrictLoadOrder();
  testEmbeddedAsoCompletionBoundNeedsNoConsumerEstimate();
  testFileFailuresRetainResultAndFailReplayClosed();
  testParsedIdentityAndLongNoteAgreementPrecedeFileAccess();
  testDecodedReplayMustAgreeWithResultAndSupportedContract();
  testInvalidResultNeverTouchesReplayFile();
  if (failures != 0) {
    std::cerr << failures << " chart replay context test(s) failed\n";
    return 1;
  }
  std::cout << "chart replay context tests passed\n";
  return 0;
}
