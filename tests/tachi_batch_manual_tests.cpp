#include "ir/tachi/TachiBatchManual.h"
#include "FileChecksum.h"
#include "scene/play/GameplayGaugeRules.h"
#include "scene/play/GameplayJudgeRules.h"

#include "nlohmann/json.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

ir::IrSubmission validSubmission() {
  ir::IrSubmission submission;
  submission.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  submission.keyMode = 7;
  submission.chartMd5 = repeated('b', 32);
  submission.chartSha256 = repeated('a', 64);
  submission.score = 1100;
  submission.maxScore = 1200;
  submission.maxCombo = 550;
  submission.comboBreak = 3;
  submission.pGreat = 500;
  submission.great = 100;
  submission.good = 10;
  submission.bad = 2;
  submission.poor = 1;
  submission.kPoor = 7;
  submission.fast = 30;
  submission.slow = 40;
  submission.pGreatFast = 5;
  submission.pGreatSlow = 7;
  submission.judgementTimingBreakdownAvailable = true;
  submission.earlyPGreat = 280;
  submission.latePGreat = 220;
  submission.earlyGreat = 60;
  submission.lateGreat = 40;
  submission.earlyGood = 6;
  submission.lateGood = 4;
  submission.earlyBad = 1;
  submission.lateBad = 1;
  submission.earlyPoor = 0;
  submission.latePoor = 1;
  submission.gaugeHistory = {20.0F, 31.25F, 48.5F, 82.0F};
  submission.finalGauge = 82.0F;
  submission.clearType = kClearTypeHardClearRank;
  submission.playedAtUnixMillis = 1700000000000LL;
  bms_parser::ChartMeta meta;
  meta.KeyMode = submission.keyMode;
  meta.MD5 = submission.chartMd5;
  meta.SHA256 = submission.chartSha256;
  meta.Rank = 2;
  meta.TotalNotes = submission.maxScore / 2;
  meta.HasTotal = true;
  meta.Total = 200.5;
  const auto judge =
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, meta.Rank);
  submission.provenance = makeScoreProvenance({
      .chartMeta = meta,
      .longNoteMode = 2,
      .judgeRankSource = JudgeRankSource::Chart,
      .sourceJudgeRank = meta.Rank,
      .effectiveJudgeContexts = judge.contexts,
      .totalNotes = meta.TotalNotes,
      .authoredGaugeTotal = meta.Total,
      .effectiveGaugeTotal =
          resolveEffectiveGaugeTotal(GameplayRuleset::LR2, meta),
      .candidateSelection = gameplay::CandidateSelectionMode::LR2,
      .gaugeType = GaugeType::Hard,
      .player1 = {.option = "RANDOM", .seed = 1234},
      .inputDevices = {InputDeviceCategory::Keyboard},
      .ruleset = RulesetDescriptor::For(GameplayRuleset::LR2),
  });
  return submission;
}

void expectIneligible(ir::IrSubmission submission,
                      ir::SubmissionEligibilityReason reason,
                      std::string_view diagnostic) {
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.status != ir::BuildDraftStatus::Built,
         "ineligible submission creates no draft");
  expect(!outcome.draft.has_value(), "ineligible outcome omits a draft");
  expect(outcome.reason == reason, "ineligible outcome has normalized reason");
  if (!diagnostic.empty()) {
    expect(outcome.diagnostic == diagnostic,
           "ineligible outcome has stable user text");
  }
}

nlohmann::json builtDocument(const ir::IrSubmission &submission) {
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.status == ir::BuildDraftStatus::Built,
         "valid submission builds a draft");
  expect(outcome.draft.has_value(), "built outcome contains draft");
  if (!outcome.draft.has_value()) {
    return {};
  }
  return nlohmann::json::parse(outcome.draft->payloadJson);
}

ir::IrOutboxEntry outboxEntry(ir::IrSubmission submission, std::int64_t id) {
  const auto built = ir::tachi::buildBatchManualDraft(submission);
  expect(built.draft.has_value(), "outbox batch fixture builds");
  if (!built.draft) {
    return {};
  }
  return {
      .id = id,
      .providerId = built.draft->providerId,
      .attemptId = built.draft->attemptId,
      .chartMd5 = built.draft->chartMd5,
      .chartSha256 = built.draft->chartSha256,
      .payloadJson = built.draft->payloadJson,
      .rulesetProof = built.draft->rulesetProof,
      .state = ir::IrOutboxState::Pending,
      .localResultReady = true,
      .createdAtUnixMillis = built.draft->createdAtUnixMillis,
      .updatedAtUnixMillis = built.draft->createdAtUnixMillis,
  };
}

void refreshProof(ir::IrOutboxEntry &entry) {
  const std::string input =
      "tachi-lr2-proof-v1\n3:lr2\n3\n" +
      std::to_string(entry.attemptId.size()) + ":" + entry.attemptId + "\n" +
      std::to_string(entry.chartSha256.size()) + ":" + entry.chartSha256 +
      "\n" + std::to_string(entry.payloadJson.size()) + ":" + entry.payloadJson;
  entry.rulesetProof = {.rulesetId = "lr2",
                        .rulesetRevision = 3,
                        .validationFingerprint = file_checksum::sha256(input)};
}

void testComposesCompatibleOutboxRows() {
  auto second = validSubmission();
  second.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  second.score = 1099;
  second.great += 1;
  second.pGreat -= 1;
  second.earlyPGreat -= 1;
  second.earlyGreat += 1;
  std::vector entries{outboxEntry(validSubmission(), 1),
                      outboxEntry(second, 2)};

  const auto batch = ir::tachi::buildBatchManualOutboxDocument(entries);
  expect(batch.status == ir::tachi::BuildTachiOutboxBatchStatus::Built,
         "compatible rows build a batch");
  expect(batch.document.has_value(), "compatible batch has a document");
  if (!batch.document) {
    return;
  }
  const auto json = nlohmann::json::parse(batch.document->payloadJson);
  expect(json.at("meta").at("playtype") == "7K", "batch keeps playtype");
  expect(json.at("scores").size() == entries.size(),
         "batch contains every compatible score exactly once");
  expect(batch.document->rowIds == std::vector<std::int64_t>({1, 2}),
         "batch retains compatible input order");
}

void testOutboxCompositionGroupsAndBoundsRows() {
  auto fourteen = validSubmission();
  fourteen.keyMode = 14;
  fourteen.attemptId = "123e4567-e89b-42d3-a456-426614174014";
  auto laterSeven = validSubmission();
  laterSeven.attemptId = "123e4567-e89b-42d3-a456-426614174007";
  std::vector mixed{outboxEntry(validSubmission(), 1), outboxEntry(fourteen, 2),
                    outboxEntry(laterSeven, 3)};
  const auto grouped = ir::tachi::buildBatchManualOutboxDocument(mixed);
  expect(grouped.document &&
             grouped.document->rowIds == std::vector<std::int64_t>({1, 3}) &&
             nlohmann::json::parse(grouped.document->payloadJson)
                     .at("scores")
                     .size() == 2,
         "mixed playtypes select the first compatible group in due order");

  std::vector<ir::IrOutboxEntry> many;
  many.reserve(65);
  const auto prototype = outboxEntry(validSubmission(), 1);
  for (std::int64_t id = 1; id <= 65; ++id) {
    auto entry = prototype;
    entry.id = id;
    many.push_back(std::move(entry));
  }
  many.back().rulesetProof.validationFingerprint = "invalid-out-of-batch-proof";
  const auto capped = ir::tachi::buildBatchManualOutboxDocument(many);
  expect(capped.document && capped.document->rowIds.size() == 64 &&
             nlohmann::json::parse(capped.document->payloadJson)
                     .at("scores")
                     .size() == 64,
         "64 valid rows ignore a malformed out-of-batch row 65");
}

void testOutboxCompositionRejectsInvalidRowsAndSplitsByBytes() {
  auto invalidProof = outboxEntry(validSubmission(), 1);
  invalidProof.rulesetProof.validationFingerprint = "invalid";
  const auto proofResult = ir::tachi::buildBatchManualOutboxDocument(
      std::span<const ir::IrOutboxEntry>(&invalidProof, 1));
  expect(proofResult.status ==
                 ir::tachi::BuildTachiOutboxBatchStatus::Invalid &&
             !proofResult.document && proofResult.rejectedRowId == 1,
         "composer explicitly rejects an invalid first-row proof");

  auto invalidPayload = outboxEntry(validSubmission(), 1);
  invalidPayload.payloadJson = R"({"meta":{},"scores":[]})";
  refreshProof(invalidPayload);
  const auto payloadResult = ir::tachi::buildBatchManualOutboxDocument(
      std::span<const ir::IrOutboxEntry>(&invalidPayload, 1));
  expect(payloadResult.status ==
                 ir::tachi::BuildTachiOutboxBatchStatus::Invalid &&
             !payloadResult.document && payloadResult.rejectedRowId == 1,
         "composer explicitly rejects an invalid first-row payload");

  auto validFirst = outboxEntry(validSubmission(), 1);
  auto malformedLater = validFirst;
  malformedLater.id = 2;
  malformedLater.rulesetProof.validationFingerprint = "invalid";
  auto validAfterMalformed = validFirst;
  validAfterMalformed.id = 3;
  const std::vector prefixEntries{validFirst, malformedLater,
                                  validAfterMalformed};
  const auto validPrefix =
      ir::tachi::buildBatchManualOutboxDocument(prefixEntries);
  expect(validPrefix.status ==
                 ir::tachi::BuildTachiOutboxBatchStatus::Built &&
             validPrefix.document &&
             validPrefix.document->rowIds == std::vector<std::int64_t>{1},
         "composer returns the valid prefix before a malformed later row");
  const auto rejectedLater = ir::tachi::buildBatchManualOutboxDocument(
      std::span(prefixEntries).subspan(1));
  expect(rejectedLater.status ==
                 ir::tachi::BuildTachiOutboxBatchStatus::Invalid &&
             rejectedLater.rejectedRowId == 2,
         "composer identifies the malformed row once it becomes first");

  auto heavy = outboxEntry(validSubmission(), 1);
  auto heavyJson = nlohmann::json::parse(heavy.payloadJson);
  heavyJson.at("scores").at(0).at("optional")["gaugeHistory"] =
      nlohmann::json::array();
  auto &history =
      heavyJson.at("scores").at(0).at("optional").at("gaugeHistory");
  for (int index = 0; index < 9'000; ++index) {
    history.push_back(12.5F);
  }
  heavy.payloadJson = heavyJson.dump();
  refreshProof(heavy);
  expect(heavy.payloadJson.size() < ir::tachi::kMaximumPayloadBytes,
         "gauge-heavy fixture is a valid singular payload");
  auto second = heavy;
  second.id = 2;
  std::vector heavyRows{heavy, second};
  const auto split = ir::tachi::buildBatchManualOutboxDocument(heavyRows);
  expect(split.document && split.document->rowIds.size() == 1 &&
             split.document->payloadJson.size() <=
                 ir::tachi::kMaximumPayloadBytes,
         "composer stops before the score that exceeds 64 KiB");
}

void testBuildsOneScoreBatchManual() {
  const auto submission = validSubmission();
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.status == ir::BuildDraftStatus::Built,
         "valid payload status is built");
  expect(outcome.draft.has_value(), "valid payload returns draft");
  if (!outcome.draft.has_value()) {
    return;
  }
  expect(outcome.draft->providerId == ir::tachi::kProviderId,
         "draft uses stable provider ID");
  expect(outcome.draft->attemptId == submission.attemptId,
         "draft retains attempt ID");
  expect(outcome.draft->chartMd5 == submission.chartMd5 &&
             outcome.draft->chartSha256 == submission.chartSha256,
         "draft retains chart hashes");
  expect(outcome.draft->createdAtUnixMillis == submission.playedAtUnixMillis,
         "draft retains captured play time");
  expect(outcome.reason == ir::SubmissionEligibilityReason::Eligible,
         "built draft is eligibility-normalized");
  expect(outcome.draft->rulesetProof.rulesetId == "lr2" &&
             outcome.draft->rulesetProof.rulesetRevision == 3,
         "draft contains the canonical LR2 proof identity");
  std::string fingerprintInput =
      "tachi-lr2-proof-v1\n3:lr2\n3\n" +
      std::to_string(submission.attemptId.size()) + ":" + submission.attemptId +
      "\n" + std::to_string(submission.chartSha256.size()) + ":" +
      submission.chartSha256 + "\n" +
      std::to_string(outcome.draft->payloadJson.size()) + ":" +
      outcome.draft->payloadJson;
  expect(outcome.draft->rulesetProof.validationFingerprint ==
             file_checksum::sha256(fingerprintInput),
         "proof fingerprint binds ruleset, attempt, chart, and frozen payload");

  const auto document = nlohmann::json::parse(outcome.draft->payloadJson);
  expect(document.at("meta").at("game") == "bms", "game is BMS");
  expect(document.at("meta").at("playtype") == "7K", "7K playtype maps");
  expect(document.at("meta").at("service") == "AsoBMaShow",
         "service identifies application");
  expect(document.at("scores").size() == 1, "payload contains one score");
  const auto &score = document.at("scores").at(0);
  expect(score.at("score") == 1100, "payload uses EX score");
  expect(score.at("lamp") == "HARD CLEAR", "payload maps hard clear");
  expect(score.at("matchType") == "bmsChartHash",
         "payload uses BMS hash matching");
  expect(score.at("identifier") == repeated('a', 64),
         "payload prefers SHA-256");
  expect(score.at("timeAchieved") == 1700000000000LL,
         "payload uses captured timestamp");
  expect(score.at("judgements").size() == 5,
         "payload emits only Tachi BMS judgements");
  expect(!score.at("judgements").contains("kpoor"), "payload omits KPoor");
  expect(score.at("optional").at("bp") == 10,
         "BP includes bad, poor, and KPoor");
  expect(score.at("optional").at("maxCombo") == 550,
         "payload includes max combo");
  expect(score.at("optional").at("gauge") == 82.0,
         "payload includes final gauge");
  expect(score.at("optional").at("fast") == 25,
         "submitted fast excludes early PGREAT");
  expect(score.at("optional").at("slow") == 33,
         "submitted slow excludes late PGREAT");
  expect(score.at("optional").at("epg") == 280 &&
             score.at("optional").at("lpg") == 220 &&
             score.at("optional").at("egr") == 60 &&
             score.at("optional").at("lgr") == 40 &&
             score.at("optional").at("egd") == 6 &&
             score.at("optional").at("lgd") == 4 &&
             score.at("optional").at("ebd") == 1 &&
             score.at("optional").at("lbd") == 1 &&
             score.at("optional").at("epr") == 0 &&
             score.at("optional").at("lpr") == 1,
         "payload includes authentic LR2 judgement timing breakdown");
  expect(score.at("optional").at("gaugeHistory") ==
             nlohmann::json::array({20.0, 31.25, 48.5, 82.0}),
         "payload includes complete fitting gauge history");
  expect(!document.contains("rulesetProof") &&
             outcome.draft->payloadJson.find("validationFingerprint") ==
                 std::string::npos,
         "Batch Manual payload does not embed local proof metadata");
}

void testMapsPlaytypesAndHashFallback() {
  auto submission = validSubmission();
  submission.keyMode = 14;
  submission.provenance.stages.front().chartSha256.clear();
  submission.chartSha256.clear();
  const auto document = builtDocument(submission);
  expect(document.at("meta").at("playtype") == "14K", "14K playtype maps");
  expect(document.at("scores").at(0).at("identifier") == repeated('b', 32),
         "MD5 is used when SHA-256 is absent");

  submission.keyMode = 5;
  const auto unsupported = ir::tachi::buildBatchManualDraft(submission);
  expect(unsupported.status == ir::BuildDraftStatus::Unsupported,
         "unsupported key mode is not retryable invalid data");
  expect(!unsupported.draft.has_value(),
         "unsupported key mode creates no draft");
}

void testCanonicalLr2EligibilityMatrix() {
  for (const int keyMode : {7, 14}) {
    auto submission = validSubmission();
    submission.keyMode = keyMode;
    const auto built = ir::tachi::buildBatchManualDraft(submission);
    expect(built.status == ir::BuildDraftStatus::Built,
           "canonical LR2 7K and 14K submissions build");
  }
  for (const int keyMode : {1, 4, 5, 6, 8, 9, 10, 11, 24, 48, 127}) {
    auto submission = validSubmission();
    submission.keyMode = keyMode;
    expect(ir::tachi::buildBatchManualDraft(submission).status ==
               ir::BuildDraftStatus::Unsupported,
           "Tachi alone narrows the open gameplay key-count domain to 7K "
           "and 14K");
  }
  for (const auto gauge :
       {GaugeType::AssistedEasy, GaugeType::Easy, GaugeType::Normal,
        GaugeType::Hard, GaugeType::ExHard, GaugeType::Hazard}) {
    auto submission = validSubmission();
    submission.provenance.gaugeType = gauge;
    expect(ir::tachi::buildBatchManualDraft(submission).status ==
               ir::BuildDraftStatus::Built,
           "every LR2 gauge remains submission eligible");
  }
  for (const auto shift :
       {GaugeAutoShiftMode::None, GaugeAutoShiftMode::SelectToUnder,
        GaugeAutoShiftMode::Continue, GaugeAutoShiftMode::SurvivalToGroove,
        GaugeAutoShiftMode::BestClear}) {
    auto submission = validSubmission();
    submission.provenance.gaugeAutoShift = shift;
    expect(ir::tachi::buildBatchManualDraft(submission).status ==
               ir::BuildDraftStatus::Built,
           "every LR2 gauge auto-shift remains submission eligible");
  }
  for (int longNoteMode = 0; longNoteMode <= 3; ++longNoteMode) {
    auto submission = validSubmission();
    submission.provenance.stages.front().longNoteMode = longNoteMode;
    expect(ir::tachi::buildBatchManualDraft(submission).status ==
               ir::BuildDraftStatus::Built,
           "every supported long-note mode remains eligible");
  }
  for (const char *option :
       {"NORMAL", "MIRROR", "RANDOM", "R-RANDOM", "S-RANDOM", "SPIRAL",
        "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX", "ASSIGN:S1234567"}) {
    auto submission = validSubmission();
    submission.provenance.player1.option = option;
    expect(ir::tachi::buildBatchManualDraft(submission).status ==
               ir::BuildDraftStatus::Built,
           "supported note-layout options remain eligible");
  }
}

void testReplayEligibilityAndMarkerCompatibility() {
  const ir::IrSubmission submission = validSubmission();
  bms_parser::ChartMeta meta;
  meta.KeyMode = submission.keyMode;
  meta.MD5 = submission.chartMd5;
  meta.SHA256 = submission.chartSha256;
  meta.TotalNotes = submission.maxScore / 2;

  const auto eligible = [&](const ScoreProvenance &provenance,
                            std::string_view attemptId =
                                "123e4567-e89b-42d3-a456-426614174000",
                            bool hasFingerprint = true) {
    return ir::tachi::isReplayEligibleForBokutachi(attemptId, hasFingerprint,
                                                   meta, provenance);
  };

  const auto show = [&](std::optional<ir::IrOutboxState> state,
                        const ScoreProvenance &provenance,
                        std::string_view attemptId =
                            "123e4567-e89b-42d3-a456-426614174000",
                        bool hasFingerprint = true) {
    return ir::tachi::shouldShowReplayUploadMarker(attemptId, hasFingerprint,
                                                   meta, provenance, state);
  };

  expect(eligible(submission.provenance),
         "canonical replay proof is Bokutachi eligible");
  expect(!eligible(submission.provenance, "", true),
         "missing attempt identity is ineligible");
  expect(!eligible(submission.provenance,
                   "123e4567-e89b-42d3-a456-426614174000", false),
         "missing canonical fingerprint is ineligible");

  ScoreProvenance modified = submission.provenance;
  modified.assistOption = assist_options::kDrag;
  modified.eligibility = ScoreEligibility::Modified;
  expect(!eligible(modified), "modified result is ineligible");
  ScoreProvenance beatoraja = submission.provenance;
  beatoraja.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  expect(!eligible(beatoraja), "Beatoraja result is ineligible");

  const int totalNotes = meta.TotalNotes;
  meta.TotalNotes = 0;
  expect(!eligible(submission.provenance),
         "replay without a positive note count is ineligible");
  meta.TotalNotes = totalNotes;

  expect(show(std::nullopt, submission.provenance),
         "compatibility marker shows eligible result without outbox row");
  for (const auto state :
       {ir::IrOutboxState::Pending, ir::IrOutboxState::Uploading,
        ir::IrOutboxState::AwaitingRemoteResult,
        ir::IrOutboxState::BlockedConfiguration,
        ir::IrOutboxState::FailedPermanent}) {
    expect(show(state, submission.provenance),
           "unfinished outbox result shows the marker");
  }
  expect(!show(ir::IrOutboxState::Succeeded, submission.provenance),
         "compatibility marker hides a successful outbox result");
}

void testRejectsNonCanonicalLr2Proof() {
  auto submission = validSubmission();
  submission.provenance.ruleset =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  expectIneligible(submission, ir::SubmissionEligibilityReason::RulesetMismatch,
                   "Beatoraja ruleset scores cannot be submitted.");

  submission = validSubmission();
  submission.provenance.ruleset = RulesetDescriptor::Legacy();
  submission.provenance.eligibility = ScoreEligibility::LegacyUnverified;
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::UnverifiedProvenance, {});

  submission = validSubmission();
  submission.provenance.ruleset.version = 4;
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::UnsupportedRulesetRevision,
                   "This ruleset revision is not supported by Bokutachi.");

  submission = validSubmission();
  submission.provenance.stages.push_back(submission.provenance.stages.front());
  expectIneligible(submission, ir::SubmissionEligibilityReason::CourseResult,
                   {});

  submission = validSubmission();
  submission.provenance.stages.front().sourceJudgeRank.reset();
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::UnverifiedProvenance, {});

  submission = validSubmission();
  submission.provenance.stages.front()
      .effectiveJudgeWindows.front()
      .lateMicros += 1;
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::ModifiedJudgePolicy,
                   "Modified judge windows cannot be submitted.");

  submission = validSubmission();
  submission.provenance.stages.front().effectiveGaugeTotal += 1.0;
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::ModifiedGaugeTotal, {});

  submission = validSubmission();
  submission.provenance.stages.front().candidateSelection =
      gameplay::CandidateSelectionMode::Lowest;
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::ModifiedJudgePolicy, {});

  const auto expectModifiedAttempt = [](auto mutate) {
    auto value = validSubmission();
    mutate(value.provenance);
    expectIneligible(value, ir::SubmissionEligibilityReason::ModifiedAttempt,
                     {});
  };
  expectModifiedAttempt([](auto &value) { value.autoPlay = true; });
  expectModifiedAttempt([](auto &value) { value.practice = true; });
  expectModifiedAttempt(
      [](auto &value) { value.assistOption = assist_options::kDrag; });
  expectModifiedAttempt([](auto &value) { value.playback.percent = 90; });
  expectModifiedAttempt(
      [](auto &value) { value.judgeWindowScalePercent = 95; });
  expectModifiedAttempt([](auto &value) { value.startingGaugePercent = 100; });

  submission = validSubmission();
  submission.keyMode = 9;
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::UnsupportedKeyMode, {});

  submission = validSubmission();
  submission.provenance.gaugeProfile = GaugeProfile::CourseLR2;
  expectIneligible(submission, ir::SubmissionEligibilityReason::CourseResult,
                   {});

  submission = validSubmission();
  submission.provenance.stages.front().effectiveJudgeWindows.pop_back();
  expectIneligible(submission,
                   ir::SubmissionEligibilityReason::InvalidSubmission, {});
}

void testMapsEveryLamp() {
  constexpr std::array cases{
      std::pair{kClearTypeFailedRank, "FAILED"},
      std::pair{kClearTypeAssistedEasyClearRank, "ASSIST CLEAR"},
      std::pair{kClearTypeLightAssistedEasyClearRank, "ASSIST CLEAR"},
      std::pair{kClearTypeEasyClearRank, "EASY CLEAR"},
      std::pair{kClearTypeNormalClearRank, "CLEAR"},
      std::pair{kClearTypeHardClearRank, "HARD CLEAR"},
      std::pair{kClearTypeExHardClearRank, "EX HARD CLEAR"},
      std::pair{kClearTypeFullComboRank, "FULL COMBO"},
  };
  for (const auto &[rank, expectedLamp] : cases) {
    auto submission = validSubmission();
    submission.clearType = rank;
    const auto document = builtDocument(submission);
    expect(document.at("scores").at(0).at("lamp") == expectedLamp,
           std::string("lamp maps to ") + expectedLamp);
  }
}

void testClampsGauge() {
  auto submission = validSubmission();
  submission.finalGauge = -4.0F;
  submission.gaugeHistory = {-4.0F, 120.0F};
  auto document = builtDocument(submission);
  expect(document.at("scores").at(0).at("optional").at("gauge") == 0.0,
         "negative gauge clamps to zero");
  expect(document.at("scores").at(0).at("optional").at("gaugeHistory") ==
             nlohmann::json::array({0.0, 100.0}),
         "gauge history clamps to the BMS percentage range");

  submission.finalGauge = 120.0F;
  document = builtDocument(submission);
  expect(document.at("scores").at(0).at("optional").at("gauge") == 100.0,
         "high gauge clamps to one hundred");
}

void testGaugeHistoryDownsamplesWithinPayloadLimit() {
  auto submission = validSubmission();
  submission.gaugeHistory.clear();
  for (int index = 0; index < 30'000; ++index) {
    submission.gaugeHistory.push_back(
        static_cast<float>((index * 37) % 10'001) / 100.0F);
  }
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.draft.has_value(), "oversized history still builds");
  if (!outcome.draft) {
    return;
  }
  expect(outcome.draft->payloadJson.size() <= ir::tachi::kMaximumPayloadBytes,
         "downsampled payload respects provider limit");
  const auto document = nlohmann::json::parse(outcome.draft->payloadJson);
  const auto &history =
      document.at("scores").at(0).at("optional").at("gaugeHistory");
  expect(history.size() < submission.gaugeHistory.size() && history.size() >= 2,
         "oversized history is reduced but retained");
  expect(history.front() == submission.gaugeHistory.front() &&
             history.back() == submission.gaugeHistory.back(),
         "downsampling preserves endpoints");

  const auto repeated = ir::tachi::buildBatchManualDraft(submission);
  expect(repeated.draft &&
             repeated.draft->payloadJson == outcome.draft->payloadJson,
         "downsampling is deterministic");
}

void testCompactGaugeHistoryUsesSerializedPayloadBudget() {
  auto submission = validSubmission();
  submission.gaugeHistory.assign(50'000, 0.0F);
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.draft.has_value(),
         "compact oversized gauge history remains submittable");
  if (!outcome.draft) {
    return;
  }
  expect(outcome.draft->payloadJson.size() <= ir::tachi::kMaximumPayloadBytes,
         "compact gauge history includes all JSON overhead in its budget");
  auto document = nlohmann::json::parse(outcome.draft->payloadJson);
  auto &history = document.at("scores").at(0).at("optional").at("gaugeHistory");
  expect(history.size() >= 2 && history.size() < submission.gaugeHistory.size(),
         "compact gauge history is downsampled without dropping endpoints");
  history.push_back(0.0F);
  expect(document.dump().size() > ir::tachi::kMaximumPayloadBytes,
         "compact gauge history uses the maximum serialized sample budget");
}

void testEmptyGaugeHistoryIsOmitted() {
  auto submission = validSubmission();
  submission.gaugeHistory.clear();
  const auto document = builtDocument(submission);
  expect(!document.at("scores").at(0).at("optional").contains("gaugeHistory"),
         "empty gauge history remains optional");
}

void testRejectsMalformedSubmission() {
  auto expectInvalid = [](ir::IrSubmission submission,
                          std::string_view message) {
    const auto outcome = ir::tachi::buildBatchManualDraft(submission);
    expect(outcome.status == ir::BuildDraftStatus::Invalid, message);
    expect(!outcome.draft.has_value(), "invalid input creates no draft");
  };

  auto submission = validSubmission();
  submission.chartSha256 = "bad";
  expectInvalid(submission, "malformed SHA-256 is invalid");

  submission = validSubmission();
  submission.chartSha256.clear();
  submission.chartMd5.clear();
  expectInvalid(submission, "missing chart hash is invalid");

  submission = validSubmission();
  submission.score = 1101;
  expectInvalid(submission, "inconsistent EX score is invalid");

  submission = validSubmission();
  submission.poor = -1;
  expectInvalid(submission, "negative judgement is invalid");

  submission = validSubmission();
  submission.playedAtUnixMillis = 0;
  expectInvalid(submission, "nonpositive timestamp is invalid");

  submission = validSubmission();
  submission.finalGauge = std::numeric_limits<float>::quiet_NaN();
  expectInvalid(submission, "non-finite gauge is invalid");

  submission = validSubmission();
  submission.pGreatFast = submission.fast + 1;
  expectInvalid(submission, "PGREAT fast cannot exceed aggregate fast");

  submission = validSubmission();
  submission.pGreatSlow = submission.slow + 1;
  expectInvalid(submission, "PGREAT slow cannot exceed aggregate slow");

  submission = validSubmission();
  submission.pGreatFast = -1;
  expectInvalid(submission, "PGREAT timing counts cannot be negative");

  submission = validSubmission();
  --submission.earlyPGreat;
  expectInvalid(submission,
                "incomplete LR2 judgement timing breakdown is invalid");

  submission = validSubmission();
  --submission.lateGood;
  expectInvalid(submission, "incomplete GOOD timing breakdown is invalid");

  submission = validSubmission();
  submission.earlyPoor = -1;
  expectInvalid(submission, "negative POOR timing breakdown is invalid");

  submission = validSubmission();
  submission.gaugeHistory[1] = std::numeric_limits<float>::quiet_NaN();
  expectInvalid(submission, "non-finite gauge history is invalid");

  submission = validSubmission();
  submission.clearType = 12345;
  expectInvalid(submission, "unknown clear rank is invalid");

  submission = validSubmission();
  submission.bad = std::numeric_limits<int>::max();
  submission.poor = 0;
  submission.kPoor = 1;
  expectInvalid(submission, "KPOOR participates in BP overflow validation");
}

void testPayloadNeverContainsCredentialMaterial() {
  const auto outcome = ir::tachi::buildBatchManualDraft(validSubmission());
  expect(outcome.draft.has_value(), "credential hygiene draft builds");
  if (!outcome.draft.has_value()) {
    return;
  }
  expect(outcome.draft->payloadJson.find("sentinel-api-key") ==
             std::string::npos,
         "payload contains no API key sentinel");
  expect(outcome.draft->payloadJson.find("Authorization") == std::string::npos,
         "payload contains no authorization header");
  expect(outcome.draft->payloadJson.size() <= ir::tachi::kMaximumPayloadBytes,
         "payload remains within provider cap");
}

} // namespace

int main() {
  testComposesCompatibleOutboxRows();
  testOutboxCompositionGroupsAndBoundsRows();
  testOutboxCompositionRejectsInvalidRowsAndSplitsByBytes();
  testBuildsOneScoreBatchManual();
  testCanonicalLr2EligibilityMatrix();
  testReplayEligibilityAndMarkerCompatibility();
  testRejectsNonCanonicalLr2Proof();
  testMapsPlaytypesAndHashFallback();
  testMapsEveryLamp();
  testClampsGauge();
  testGaugeHistoryDownsamplesWithinPayloadLimit();
  testCompactGaugeHistoryUsesSerializedPayloadBudget();
  testEmptyGaugeHistoryIsOmitted();
  testRejectsMalformedSubmission();
  testPayloadNeverContainsCredentialMaterial();
  if (failures != 0) {
    std::cerr << failures << " Tachi batch manual test(s) failed\n";
    return 1;
  }
  std::cout << "Tachi batch manual tests passed\n";
  return 0;
}
