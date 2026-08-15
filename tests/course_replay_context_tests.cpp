#if __has_include("replay/CourseReplayContext.h")
#include "replay/CourseReplayContext.h"
#include "replay/BeatorajaReplayPath.h"
#define ASOBMASHOW_HAS_COURSE_REPLAY_CONTEXT 1
#else
#define ASOBMASHOW_HAS_COURSE_REPLAY_CONTEXT 0
#endif

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

#if ASOBMASHOW_HAS_COURSE_REPLAY_CONTEXT

using namespace replay;

constexpr std::string_view kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

ScoreProvenance provenance(char hash, int keyMode) {
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = repeated(hash, 32);
  input.chartMeta.SHA256 = repeated(hash, 64);
  input.chartMeta.KeyMode = keyMode;
  input.chartMeta.Rank = 2;
  input.chartMeta.TotalNotes = 5;
  input.chartMeta.HasTotal = true;
  input.chartMeta.Total = 200.0;
  input.longNoteMode = 1;
  input.sourceJudgeRank = 2;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  input.totalNotes = 5;
  input.authoredGaugeTotal = 200.0;
  input.effectiveGaugeTotal = 200.0;
  input.gaugeType = GaugeType::Hard;
  input.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  input.gaugeAutoShiftLowerBound = GaugeType::Easy;
  input.inputDevices = {InputDeviceCategory::Keyboard};
  return makeScoreProvenance(input);
}

result_persistence::ModernCourseStageResult stage(int index, char hash,
                                                   int keyMode,
                                                   int maximumCombo,
                                                   float gauge) {
  result_persistence::ModernCourseStageResult value;
  value.stageIndex = index;
  value.score.chartPath = "library/stage-" + std::to_string(index) + ".bms";
  value.score.chartMd5 = repeated(hash, 32);
  value.score.chartSha256 = repeated(hash, 64);
  value.score.chartTitle = "Stage";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = maximumCombo;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = gauge;
  value.score.clearType = kClearTypeHardClearRank;
  value.score.provenance = provenance(hash, keyMode);
  value.keyMode = keyMode;
  value.adoptedGaugeType = GaugeType::Hard;
  value.adoptedGaugeHistory = {80.0F, gauge};
  return value;
}

result_persistence::ModernCourseResult savedResult(bool repeatedChart = false,
                                                    bool complete = false) {
  result_persistence::ModernCourseResultCapture capture{
      .attemptId = std::string(kAttemptId),
      .courseKey = "course:v1:" + repeated('c', 64),
      .legacyCourseId = 42,
      .courseName = "Context Course",
      .courseGroupName = "Tests",
      .constraintJson = R"(["no_speed","gauge_7k"])",
      .requestedPlayOption = "NORMAL",
      .assistOption = "OFF",
      .initialGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::Continue,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .longNoteMode = 1,
      .clearType = kClearTypeHardClearRank,
      .stages = {stage(0, 'a', 7, 4, 76.0F),
                 stage(1, repeatedChart ? 'a' : 'b', 14, 8, 62.5F)},
      .entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000},
                     {.totalNotes = 5, .playLengthMicros = 2'000'000},
                     {.totalNotes = 5, .playLengthMicros = 3'000'000}},
      .playedAtUnixMillis = 1'700'000'000'456LL,
  };
  if (complete) {
    capture.entryFacts.resize(2);
  }
  std::string diagnostic;
  auto result = result_persistence::captureModernCourseResult(capture,
                                                               diagnostic);
  expect(result.has_value(), "modern course result fixture captures");
  if (!result) {
    return {};
  }
  result->resultId = 17;
  return *result;
}

ReplaySetup setup(const result_persistence::ModernCourseResult &result,
                  std::size_t index) {
  const auto &saved = result.stages[index];
  ReplaySetup value;
  value.chart = {.md5 = saved.score.chartMd5,
                 .sha256 = saved.score.chartSha256,
                 .keyMode = saved.keyMode};
  value.longNoteMode = saved.score.longNoteMode;
  value.player1.option = result.requestedPlayOption;
  value.assistOption = result.assistOption;
  value.initialGaugeType = result.initialGaugeType;
  value.gaugeProfile = result.gaugeProfile;
  value.gaugeAutoShift = result.gaugeAutoShift;
  value.gaugeAutoShiftLowerBound = result.gaugeAutoShiftLowerBound;
  value.ruleset = saved.score.provenance.ruleset;
  value.playback = saved.score.provenance.playback;
  value.candidateSelection =
      saved.score.provenance.stages.front().candidateSelection;
  value.judgeWindowScalePercent =
      saved.score.provenance.judgeWindowScalePercent;
  value.startingGaugePercent =
      static_cast<float>(
          saved.score.provenance.startingGaugePercent.value_or(100));
  return value;
}

ReplayCourseDocument document(
    const result_persistence::ModernCourseResult &result) {
  ReplayCourseDocument replay;
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    ReplayPlaybackData playback;
    playback.setup = setup(result, index);
    playback.input = {
        {.songTimeMicros = 0,
         .control = {.kind = LogicalControlKind::Lane,
                     .player = 1,
                     .lane = 0},
         .pressed = true},
        {.songTimeMicros = 1,
         .control = {.kind = LogicalControlKind::Lane,
                     .player = 1,
                     .lane = 0},
         .pressed = false},
    };
    replay.playback.stages.push_back(std::move(playback));
    replay.playback.restMicrosAfterStage.push_back(
        index + 1 == result.stages.size() ? 0 : 1'000'000);
    replay.timeBounds.push_back(
        {.completionSongTimeMicros = 5'000'000 +
                                     static_cast<std::int64_t>(index)});
  }
  return replay;
}

ParsedCourseReplayFacts parsedFacts(
    const result_persistence::ModernCourseResult &result) {
  ParsedCourseReplayFacts facts;
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    const auto &saved = result.stages[index];
    facts.stages.push_back({
        .chart = {.md5 = saved.score.chartMd5,
                  .sha256 = saved.score.chartSha256,
                  .keyMode = saved.keyMode},
        .longNoteMode = saved.score.longNoteMode,
        .hasUndefinedLongNotes = false,
        .timeBounds = ReplayTimeBounds{
            .completionSongTimeMicros = 5'000'000 +
                                        static_cast<std::int64_t>(index)},
    });
  }
  return facts;
}

ModernReplayFileReference reference(
    const result_persistence::ModernCourseResult &result,
    std::vector<int> constraints = {4, 9},
    bool hasUndefinedLongNotes = false) {
  CoursePathInput input{.longNoteMode = result.longNoteMode,
                        .hasUndefinedLongNotes = hasUndefinedLongNotes,
                        .beatorajaConstraintIds = std::move(constraints)};
  for (const auto &saved : result.stages) {
    input.stageSha256.push_back(saved.score.chartSha256);
  }
  std::string diagnostic;
  const auto stem = courseStem(input, diagnostic);
  const auto identity = stem ? pathForStem(*stem, 0, diagnostic)
                             : std::nullopt;
  expect(identity.has_value(), "course replay reference fixture captures");
  return {.id = 8,
          .resultId = result.resultId,
          .userDeleted = false,
          .identity = *identity,
          .metadata = {.relativePath = identity->relativePath,
                       .sha256 = repeated('d', 64),
                       .compressedSize = 3,
                       .codecVersion = BeatorajaReplayCodec::kCodecVersion}};
}

struct Harness {
  result_persistence::ModernCourseResult result = savedResult();
  ReplayCourseDocument replay = document(result);
  ModernReplayFileReference fileReference = reference(result);
  std::vector<std::string> calls;
  ModernCourseResultReadStatus resultStatus =
      ModernCourseResultReadStatus::Loaded;
  bool attachReplay = true;
  ReplayFileState fileState = ReplayFileState::Available;
  bool unsupportedExtension = false;
  bool decodeCourse = true;
  bool throwDuringFileRead = false;
  std::optional<ReplayDecodeContext> decodeContext;

  CourseReplayContext makeContext() {
    return CourseReplayContext(CourseReplayContextDependencies{
        .loadResult = [this](std::string_view) {
          calls.emplace_back("result");
          ModernCourseResultReadOutcome outcome{.status = resultStatus};
          if (resultStatus == ModernCourseResultReadStatus::Loaded) {
            outcome.record = ModernCourseResultRecord{
                .result = result,
                .replayFile = attachReplay
                                  ? std::optional(fileReference)
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
          if (decodeCourse) {
            outcome.course = replay;
            outcome.stageSources.assign(
                replay.playback.stages.size(),
                ReplayStageDecodeSource::AsoExtension);
          }
          return outcome;
        },
    });
  }
};

void testUserDeletedCourseReferenceNeverTouchesFilesystem() {
  Harness harness;
  harness.fileReference.userDeleted = true;
  auto context = harness.makeContext();
  const auto loaded =
      context.load(kAttemptId, parsedFacts(harness.result));
  expect(loaded.state == CourseReplayContextState::FileUserDeleted &&
             loaded.resultAvailable() && !loaded.replayAvailable() &&
             loaded.replayState() == ReplayState::UserDeleted &&
             harness.calls == std::vector<std::string>{"result"},
         "durable course replay deletion is projected before file I/O");
}

void testCompletePartialRepeatedAndMixedSetupsUseStrictLoadOrder() {
  for (const auto [repeatedChart, complete] :
       {std::pair{false, false}, std::pair{false, true},
        std::pair{true, false}}) {
    Harness harness;
    harness.result = savedResult(repeatedChart, complete);
    harness.replay = document(harness.result);
    harness.fileReference = reference(harness.result);
    auto context = harness.makeContext();
    const auto loaded = context.load(kAttemptId, parsedFacts(harness.result));
    expect(loaded.state == CourseReplayContextState::Ready &&
               loaded.resultAvailable() && loaded.replayAvailable() &&
               loaded.verified &&
               loaded.verified->document == harness.replay,
           "complete, partial, repeated, and mixed course prefixes verify");
    expect(harness.calls ==
               std::vector<std::string>{"result", "file", "decode"},
           "course context loads result before verified bytes and decode");
    expect(harness.decodeContext &&
               harness.decodeContext->stageKeyModes ==
                   std::vector<int>({7, 14}) &&
               harness.decodeContext->stageTimeBounds.empty(),
           "decode receives ordered key modes without parser duration gates");
  }
}

void testParsedDurationEstimatesDoNotOverrideEmbeddedBounds() {
  Harness harness;
  auto facts = parsedFacts(harness.result);
  facts.stages[0].timeBounds =
      ReplayTimeBounds{.completionSongTimeMicros = 4'000'000};
  facts.stages[1].timeBounds =
      ReplayTimeBounds{.completionSongTimeMicros = 7'000'000};
  auto context = harness.makeContext();
  const auto loaded = context.load(kAttemptId, facts);
  expect(loaded.state == CourseReplayContextState::Ready &&
             loaded.replayAvailable(),
         "optional course duration disagreement does not disable a valid BRD");
  expect(harness.decodeContext &&
             harness.decodeContext->stageTimeBounds.empty(),
         "course decoder uses completion bounds embedded by its producer");
}

void testForcedCourseModeRetainsEffectiveStageReplayMode() {
  Harness harness;
  harness.result.longNoteMode = 2;
  harness.result.resultFingerprint =
      result_persistence::modernResultFingerprint(harness.result);
  harness.fileReference = reference(harness.result);
  auto context = harness.makeContext();
  const auto loaded = context.load(kAttemptId, parsedFacts(harness.result));
  expect(loaded.state == CourseReplayContextState::Ready &&
             loaded.resultAvailable() && loaded.replayAvailable() &&
             harness.calls ==
                 std::vector<std::string>({"result", "file", "decode"}),
         "course replay keeps each chart's effective LN mode when the course "
         "mode is forced");
}

void testMissingCorruptUnsafeAndDetachedFilesPreserveResult() {
  struct Case {
    ReplayFileState file;
    CourseReplayContextState state;
    ReplayState capability;
  };
  const std::vector<Case> cases{
      {ReplayFileState::Missing, CourseReplayContextState::FileMissing,
       ReplayState::Missing},
      {ReplayFileState::Corrupt, CourseReplayContextState::FileCorrupt,
       ReplayState::Corrupt},
      {ReplayFileState::Unsafe, CourseReplayContextState::FileUnsafe,
       ReplayState::Mismatched},
      {ReplayFileState::IoFailure, CourseReplayContextState::FileIoFailure,
       ReplayState::Missing},
  };
  for (const auto &test : cases) {
    Harness harness;
    harness.fileState = test.file;
    auto context = harness.makeContext();
    const auto loaded = context.load(kAttemptId, parsedFacts(harness.result));
    expect(loaded.state == test.state && loaded.resultAvailable() &&
               !loaded.replayAvailable() &&
               loaded.replayState() == test.capability &&
               harness.calls == std::vector<std::string>{"result", "file"},
           "file failure disables only course replay-dependent actions");
  }

  Harness detached;
  detached.attachReplay = false;
  auto detachedContext = detached.makeContext();
  const auto loaded =
      detachedContext.load(kAttemptId, parsedFacts(detached.result));
  expect(loaded.state == CourseReplayContextState::ReplayNotAttached &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "summary-only course result remains recallable");

  Harness exceptional;
  exceptional.throwDuringFileRead = true;
  auto exceptionalContext = exceptional.makeContext();
  const auto exceptionalLoad =
      exceptionalContext.load(kAttemptId, parsedFacts(exceptional.result));
  expect(exceptionalLoad.state == CourseReplayContextState::FileIoFailure &&
             exceptionalLoad.resultAvailable() &&
             !exceptionalLoad.replayAvailable(),
         "unexpected course BRD I/O still preserves result recall");
}

void testParsedPrefixIdentityOrderAndSetupRejectBeforeFileAccess() {
  Harness harness;
  auto facts = parsedFacts(harness.result);
  std::swap(facts.stages[0], facts.stages[1]);
  auto context = harness.makeContext();
  auto loaded = context.load(kAttemptId, facts);
  expect(loaded.state == CourseReplayContextState::StageMismatch &&
             harness.calls == std::vector<std::string>{"result"},
         "wrong stage order rejects before BRD access");

  harness.calls.clear();
  facts = parsedFacts(harness.result);
  facts.stages[0].chart.keyMode = 9;
  loaded = context.load(kAttemptId, facts);
  expect(loaded.state == CourseReplayContextState::StageMismatch &&
             harness.calls == std::vector<std::string>{"result"},
         "wrong stage key mode rejects before BRD access");

  harness.calls.clear();
  facts = parsedFacts(harness.result);
  ++facts.stages[1].longNoteMode;
  loaded = context.load(kAttemptId, facts);
  expect(loaded.state == CourseReplayContextState::LongNoteModeMismatch &&
             harness.calls == std::vector<std::string>{"result"},
         "wrong stage LN mode rejects before BRD access");
}

void testReferenceDecodePlaybackAndResultAgreementFailClosed() {
  Harness wrongConstraint;
  wrongConstraint.fileReference = reference(wrongConstraint.result, {4});
  auto context = wrongConstraint.makeContext();
  auto loaded =
      context.load(kAttemptId, parsedFacts(wrongConstraint.result));
  expect(loaded.state == CourseReplayContextState::ReferenceMismatch &&
             wrongConstraint.calls == std::vector<std::string>{"result"},
         "wrong constraint path identity rejects before BRD access");

  Harness futureCodec;
  ++futureCodec.fileReference.metadata.codecVersion;
  auto futureCodecContext = futureCodec.makeContext();
  loaded = futureCodecContext.load(kAttemptId,
                                   parsedFacts(futureCodec.result));
  expect(loaded.state ==
                 CourseReplayContextState::UnsupportedCodecVersion &&
             futureCodec.calls == std::vector<std::string>{"result"},
         "unsupported course codec version rejects before file access");

  Harness future;
  future.unsupportedExtension = true;
  auto futureContext = future.makeContext();
  loaded = futureContext.load(kAttemptId, parsedFacts(future.result));
  expect(loaded.state == CourseReplayContextState::UnsupportedExtension &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "unsupported Aso course extension fails closed");

  Harness wrongSetup;
  wrongSetup.replay.playback.stages[1].setup.chart.sha256 = repeated('e', 64);
  auto wrongSetupContext = wrongSetup.makeContext();
  loaded = wrongSetupContext.load(kAttemptId,
                                  parsedFacts(wrongSetup.result));
  expect(loaded.state == CourseReplayContextState::StageMismatch &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "decoded stage/result identity disagreement disables replay");

  Harness sharedSetup;
  sharedSetup.replay.playback.stages[0].setup.judgeWindowScalePercent = 75;
  auto sharedSetupContext = sharedSetup.makeContext();
  loaded = sharedSetupContext.load(kAttemptId,
                                   parsedFacts(sharedSetup.result));
  expect(loaded.state == CourseReplayContextState::SharedFactsMismatch &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "decoded course setup/result disagreement disables replay");

  Harness excessiveRest;
  excessiveRest.replay.playback.restMicrosAfterStage[0] =
      kReplayLimits.maxCourseRestMicros + 1;
  auto restContext = excessiveRest.makeContext();
  loaded = restContext.load(kAttemptId, parsedFacts(excessiveRest.result));
  expect(loaded.state == CourseReplayContextState::Ready &&
             loaded.resultAvailable() && loaded.replayAvailable(),
         "context does not repeat course envelope validation owned by codec");

  Harness malformed;
  malformed.decodeCourse = false;
  auto malformedContext = malformed.makeContext();
  loaded = malformedContext.load(kAttemptId, parsedFacts(malformed.result));
  expect(loaded.state == CourseReplayContextState::DecodeFailed &&
             loaded.resultAvailable() && !loaded.replayAvailable(),
         "chart-shaped or absent decode cannot become a course replay");

  Harness invalidResult;
  invalidResult.result.resultFingerprint = "invalid";
  auto invalidResultContext = invalidResult.makeContext();
  loaded = invalidResultContext.load(kAttemptId,
                                     parsedFacts(savedResult()));
  expect(loaded.state == CourseReplayContextState::ResultInvalid &&
             !loaded.resultAvailable() &&
             invalidResult.calls == std::vector<std::string>{"result"},
         "invalid modern course result never grants file authority");
}

#endif

} // namespace

int main() {
#if ASOBMASHOW_HAS_COURSE_REPLAY_CONTEXT
  testCompletePartialRepeatedAndMixedSetupsUseStrictLoadOrder();
  testParsedDurationEstimatesDoNotOverrideEmbeddedBounds();
  testForcedCourseModeRetainsEffectiveStageReplayMode();
  testUserDeletedCourseReferenceNeverTouchesFilesystem();
  testMissingCorruptUnsafeAndDetachedFilesPreserveResult();
  testParsedPrefixIdentityOrderAndSetupRejectBeforeFileAccess();
  testReferenceDecodePlaybackAndResultAgreementFailClosed();
#else
  expect(false, "CourseReplayContext contract is not implemented");
#endif
  if (failures != 0) {
    std::cerr << failures << " course replay context test(s) failed\n";
    return 1;
  }
  std::cout << "course replay context tests passed\n";
  return 0;
}
