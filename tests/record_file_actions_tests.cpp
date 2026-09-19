#include "scene/RecordFileActions.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Fixture {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("asobmashow-record-file-actions-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  ReplayRepository repository{path / "replay.db"};
  replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = "123e4567-e89b-42d3-a456-426614174001",
  };

  Fixture() {
    assert(std::filesystem::create_directories(path));
    assert(repository.EnsureSchema());
    result_persistence::ModernChartResult value;
    value.attemptId = request.attemptId;
    value.score.chartPath = "library/chart.bms";
    value.score.chartMd5 = std::string(32, 'b');
    value.score.chartSha256 = std::string(64, 'a');
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
    value.score.provenance = ScoreProvenance::Legacy();
    value.keyMode = 7;
    value.adoptedGaugeType = GaugeType::Normal;
    value.adoptedGaugeHistory = {20.0F, 82.5F};
    value.playedAtUnixMillis = 1'700'000'000'001LL;
    value.resultFingerprint = result_persistence::modernResultFingerprint(value);
    replay::ReplayFileStore store(path);
    const auto reserved = repository.ReserveModernReplayPath(
        value.attemptId, value.score.chartSha256, value.playedAtUnixMillis);
    assert(reserved.reservation);
    const std::vector bytes{std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08}};
    const auto reservation = store.reserve(reserved.reservation->identity, bytes,
                                            value.attemptId);
    assert(reservation.reservation);
    const auto installed = store.install(*reservation.reservation, bytes);
    assert(installed.file);
    const ModernReplayFileAttachment attachment{
        .identity = reserved.reservation->identity,
        .metadata = installed.file->metadata,
    };
    assert(repository.StageModernChartResult(value, std::nullopt, attachment)
               .status == ModernChartStageStatus::Staged);
  }
  ~Fixture() {
    repository.Shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::optional<RecordFileActions::Feedback> waitForFeedback(
    RecordFileActions &actions) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    if (auto feedback = actions.poll()) return feedback;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (actions.active() && std::chrono::steady_clock::now() < deadline);
  return std::nullopt;
}

void testShareRequestRetainsSnapshotUntilDetachedWorkFinishes() {
  Fixture fixture;
  std::promise<void> release;
  const auto released = release.get_future().share();
  auto entered = std::make_shared<std::promise<void>>();
  auto entry = entered->get_future();
  auto completed = std::make_shared<std::promise<void>>();
  auto completion = completed->get_future();
  std::weak_ptr<void> lifetime;
  std::filesystem::path sharedPath;
  RecordFileActions actions(fixture.repository,
      [&](PlatformDocumentExportRequest request) {
        expect(request.mimeType == "application/gzip" &&
                   request.maxBytes == replay::kReplayLimits.maxCompressedBytes &&
                   std::filesystem::path(request.suggestedName).extension() == ".brd",
               "sharing passes BRD MIME, size bound and suggested name");
        expect(request.sourceLifetime && std::filesystem::exists(request.localPath),
               "sharing exports a live prepared snapshot");
        lifetime = request.sourceLifetime;
        sharedPath = request.localPath;
        return platform_document_handoff::detail::StartOperation(
            [request = std::move(request), released, entered, completed](
                const std::atomic_bool &) {
              entered->set_value();
              released.wait();
              const bool sourceExists = std::filesystem::exists(request.localPath);
              completed->set_value();
              return PlatformDocumentHandoffResult{
                  .status = sourceExists ? PlatformDocumentHandoffStatus::Succeeded
                                         : PlatformDocumentHandoffStatus::Failed};
            }, {});
      });
  const auto started = actions.share(fixture.request);
  expect(actions.active() && !started.failed && !started.reloadRecords &&
             !started.message.empty(),
         "successful share start exposes active handoff without reloading rows");
  if (entry.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    release.set_value();
    expect(false, "export worker starts reading before scene close");
    return;
  }
  actions.close();
  expect(!actions.active() && !actions.poll(),
         "closing handoff immediately releases scene busy state");
  expect(!lifetime.expired() && std::filesystem::exists(sharedPath),
         "closing scene preserves snapshot while detached export still reads");
  release.set_value();
  if (completion.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    expect(false, "detached export worker completes after release");
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(lifetime.expired(), "snapshot ownership ends after detached work finishes");
}

void testFailedStartDoesNotRemainBusy() {
  Fixture fixture;
  RecordFileActions actions(fixture.repository,
      [](PlatformDocumentExportRequest) {
        return platform_document_handoff::PlatformDocumentHandoffOperation{};
      });
  const auto feedback = actions.share(fixture.request);
  expect(feedback.failed && !feedback.message.empty() && !actions.active() &&
             !actions.poll(),
         "failed handoff start reports failure without leaving the modal busy");
}

void testCompletionFeedbackClearsBusyExactlyOnce() {
  for (const auto status : {PlatformDocumentHandoffStatus::Succeeded,
                            PlatformDocumentHandoffStatus::Cancelled,
                            PlatformDocumentHandoffStatus::Failed}) {
    Fixture fixture;
    RecordFileActions actions(fixture.repository,
        [status](PlatformDocumentExportRequest request) {
          return platform_document_handoff::detail::StartOperation(
              [status, request = std::move(request)](const std::atomic_bool &) {
                return PlatformDocumentHandoffResult{
                    .status = status,
                    .message = status == PlatformDocumentHandoffStatus::Failed
                        ? "Provider\001rejected the file" : ""};
              }, {});
        });
    const auto started = actions.share(fixture.request);
    const auto feedback = waitForFeedback(actions);
    expect(feedback && !feedback->message.empty() && !feedback->reloadRecords &&
               feedback->failed == (status == PlatformDocumentHandoffStatus::Failed) &&
               !actions.active() && !actions.poll(),
           "completion delivers status once and clears busy for every outcome");
    if (feedback && status == PlatformDocumentHandoffStatus::Failed) {
      expect(feedback->message.find("Provider") != std::string::npos &&
                 feedback->message.find('\001') == std::string::npos,
             "detailed handoff errors are sanitized before presentation");
    }
  }
}

void testDeletionKeepsHistoryAndUnavailableShareReloads() {
  Fixture fixture;
  RecordFileActions actions(fixture.repository,
      [](PlatformDocumentExportRequest) {
        std::abort();
        return platform_document_handoff::PlatformDocumentHandoffOperation{};
      });
  const auto removed = actions.remove(fixture.request);
  const auto stored = fixture.repository.LoadModernChartResultByAttempt(
      fixture.request.attemptId);
  expect(!removed.failed && removed.reloadRecords && stored.record &&
             stored.record->result.score.score == 7 && stored.record->replayFile &&
             stored.record->replayFile->userDeleted,
         "deleting replay marks file deleted and keeps the result history");
  const auto unavailable = actions.share(fixture.request);
  expect(unavailable.failed && unavailable.reloadRecords && !actions.active() &&
             !unavailable.message.empty(),
         "sharing deleted replay returns a diagnostic and refreshes availability");
  const auto invalid = actions.remove({.attemptId = "invalid"});
  expect(invalid.failed && !invalid.reloadRecords && !invalid.message.empty(),
         "failed deletion reports a diagnostic without claiming changed rows");
}

} // namespace

int main() {
  testShareRequestRetainsSnapshotUntilDetachedWorkFinishes();
  testFailedStartDoesNotRemainBusy();
  testCompletionFeedbackClearsBusyExactlyOnce();
  testDeletionKeepsHistoryAndUnavailableShareReloads();
  return failures == 0 ? 0 : 1;
}
