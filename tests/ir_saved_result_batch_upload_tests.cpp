#include "ir/IrSavedResultBatchUpload.h"

#include <cstdio>
#include <iostream>
#include <span>
#include <stdexcept>
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

std::string attemptId(int suffix) {
  char value[37]{};
  std::snprintf(value, sizeof(value),
                "123e4567-e89b-42d3-a456-426614174%03d", suffix);
  return value;
}

ir::IrSubmission submission(int suffix) {
  ir::IrSubmission value;
  value.attemptId = attemptId(suffix);
  value.chartMd5 = std::string(32, 'b');
  value.chartSha256 = std::string(64, 'a');
  return value;
}

ir::IrOutboxDraft draftFor(const ir::IrSubmission &submission) {
  return {
      .providerId = "tachi",
      .attemptId = submission.attemptId,
      .chartMd5 = submission.chartMd5,
      .chartSha256 = submission.chartSha256,
      .payloadJson = R"({"score":123})",
      .rulesetProof =
          {
              .rulesetId = "lr2",
              .rulesetRevision = 3,
              .validationFingerprint = std::string(64, 'c'),
          },
      .createdAtUnixMillis = 10'000,
  };
}

void testBuildsEverySubmissionThenEnqueuesValidDraftsOnce() {
  const std::vector submissions{submission(60), submission(61),
                                submission(62)};
  int buildCalls = 0;
  int enqueueCalls = 0;
  ir::IrSavedResultBatchUploadDependencies dependencies;
  dependencies.buildDraft = [&](const ir::IrSubmission &value) {
    ++buildCalls;
    if (value.attemptId == attemptId(61)) {
      return ir::BuildDraftOutcome{
          .status = ir::BuildDraftStatus::Invalid,
          .diagnostic = std::string(800, 'x') + "\nsecret"};
    }
    return ir::BuildDraftOutcome{.status = ir::BuildDraftStatus::Built,
                                 .draft = draftFor(value)};
  };
  dependencies.enqueueBatch =
      [&](std::span<const ir::IrOutboxDraft> drafts) {
        ++enqueueCalls;
        expect(drafts.size() == 2, "only valid drafts reach storage");
        expect(drafts[0].attemptId == attemptId(60) &&
                   drafts[1].attemptId == attemptId(62),
               "valid drafts retain preparation order");
        ir::IrManualBatchEnqueueOutcome outcome{.storageAvailable = true};
        outcome.items = {
            {.attemptId = drafts[0].attemptId,
             .status = ir::IrManualBatchItemStatus::Inserted},
            {.attemptId = drafts[1].attemptId,
             .status = ir::IrManualBatchItemStatus::RetryQueued},
        };
        return outcome;
      };

  const auto result = ir::executeIrSavedResultBatchUpload(
      "tachi", submissions, dependencies);

  expect(buildCalls == 3, "preparation attempts every saved score");
  expect(enqueueCalls == 1, "preparation performs one service mutation");
  expect(result.buildFailures == 1,
         "invalid construction remains isolated");
  expect(result.items.size() == 3,
         "result preserves one outcome for every prepared submission");
  expect(result.items[0].attemptId == attemptId(60) &&
             result.items[0].status ==
                 ir::IrManualBatchItemStatus::Inserted &&
             result.items[1].attemptId == attemptId(61) &&
             result.items[1].status == ir::IrManualBatchItemStatus::Failed &&
             result.items[2].attemptId == attemptId(62) &&
             result.items[2].status ==
                 ir::IrManualBatchItemStatus::RetryQueued,
         "storage and build outcomes return in submission order");
  expect(result.items[1].diagnostic.size() <= ir::kMaximumDiagnosticBytes &&
             result.items[1].diagnostic.find('\n') == std::string::npos,
         "build diagnostics are sanitized");
}

void testMissingOrThrowingDependenciesFailWithoutEscaping() {
  const std::vector submissions{submission(63), submission(64)};
  const auto unavailable = ir::executeIrSavedResultBatchUpload(
      "tachi", submissions, {});
  expect(unavailable.buildFailures == 2 && unavailable.items.size() == 2,
         "missing builder reports every score as a build failure");

  ir::IrSavedResultBatchUploadDependencies throwing;
  throwing.buildDraft = [](const ir::IrSubmission &value) {
    return ir::BuildDraftOutcome{.status = ir::BuildDraftStatus::Built,
                                 .draft = draftFor(value)};
  };
  throwing.enqueueBatch = [](std::span<const ir::IrOutboxDraft>)
      -> ir::IrManualBatchEnqueueOutcome {
    throw std::runtime_error("credential-token\x01leak");
  };
  const auto failed = ir::executeIrSavedResultBatchUpload(
      "tachi", submissions, throwing);
  expect(failed.buildFailures == 0 && failed.items.size() == 2,
         "enqueue exception preserves every prepared attempt outcome");
  expect(failed.items[0].status == ir::IrManualBatchItemStatus::Failed &&
             failed.items[1].status == ir::IrManualBatchItemStatus::Failed &&
             failed.diagnostic.find('\x01') == std::string::npos &&
             failed.diagnostic.size() <= ir::kMaximumDiagnosticBytes,
         "enqueue exception is contained and sanitized");
}

} // namespace

int main() {
  testBuildsEverySubmissionThenEnqueuesValidDraftsOnce();
  testMissingOrThrowingDependenciesFailWithoutEscaping();
  if (failures != 0) {
    std::cerr << failures << " saved-result batch upload test(s) failed\n";
    return 1;
  }
  return 0;
}
