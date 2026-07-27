#include "replay/ReplayFileActionService.h"

#include "repositories/ReplayRepository.h"
#include "replay/ReplayFileStore.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-replay-actions-" + std::to_string(stamp));
    assert(std::filesystem::create_directories(path));
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

result_persistence::ModernChartResult result(int suffix, char hash) {
  result_persistence::ModernChartResult value;
  value.attemptId = "123e4567-e89b-42d3-a456-42661417400" +
                    std::to_string(suffix);
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = repeated(hash, 32);
  value.score.chartSha256 = repeated(hash, 64);
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
  value.playedAtUnixMillis = 1'700'000'000'000LL + suffix;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  std::string diagnostic;
  assert(result_persistence::validateModernChartResult(value, diagnostic));
  return value;
}

struct InstalledResult {
  result_persistence::ModernChartResult result;
  ModernReplayFileReference reference;
};

InstalledResult installResult(ReplayRepository &repository,
                              replay::ReplayFileStore &store, int suffix,
                              char hash) {
  const auto completed = result(suffix, hash);
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.status == ModernReplayReservationStatus::Reserved &&
         reserved.reservation);
  const std::vector bytes{std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08},
                          std::byte{static_cast<unsigned char>(suffix)}};
  const auto fileReservation = store.reserve(
      reserved.reservation->identity, bytes, completed.attemptId);
  assert(fileReservation.reservation);
  const auto installed = store.install(*fileReservation.reservation, bytes);
  assert(installed.state == replay::ReplayInstallState::InstalledVerified &&
         installed.file);
  const ModernReplayFileAttachment attachment{
      .identity = reserved.reservation->identity,
      .metadata = installed.file->metadata};
  const auto staged = repository.StageModernChartResult(
      completed, std::nullopt, attachment, {});
  assert(staged.status == ModernChartStageStatus::Staged);
  const auto loaded =
      repository.LoadModernChartResultByAttempt(completed.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded &&
         loaded.record && loaded.record->replayFile);
  return {.result = loaded.record->result,
          .reference = *loaded.record->replayFile};
}

void testVerifiedShareUsesStableSnapshotAndDeleteKeepsResult() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 1, 'a');
  replay::ReplayFileActionService actions(repository, store);
  const replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId};

  const auto inspected = actions.inspect(request);
  assert(inspected.state == replay::ReplayFileActionState::Verified);
  auto shared = actions.prepareShare(request);
  assert(shared.state == replay::ReplayFileActionState::Verified &&
         shared.share && std::filesystem::exists(shared.share->sourcePath) &&
         shared.share->suggestedFilename ==
             installed.reference.metadata.relativePath.filename().string());
  const auto original = temporary.path / installed.reference.metadata.relativePath;
  assert(shared.share->sourcePath != original);

  const auto removed = actions.remove(request);
  assert(removed.state == replay::ReplayFileActionState::UserDeleted &&
         removed.changed && !removed.cleanupPending &&
         !std::filesystem::exists(original) &&
         std::filesystem::exists(shared.share->sourcePath));
  const auto loaded =
      repository.LoadModernChartResultByAttempt(installed.result.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded &&
         loaded.record && loaded.record->result == installed.result &&
         loaded.record->replayFile && loaded.record->replayFile->userDeleted);
  const auto repeated = actions.remove(request);
  assert(repeated.state == replay::ReplayFileActionState::UserDeleted &&
         !repeated.changed && !repeated.cleanupPending);
}

void testCorruptOwnedEntryRemainsDeletable() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 2, 'b');
  const auto path = temporary.path / installed.reference.metadata.relativePath;
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "corrupt";
  }
  replay::ReplayFileActionService actions(repository, store);
  const replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId};
  assert(actions.inspect(request).state ==
         replay::ReplayFileActionState::Corrupt);
  const auto removed = actions.remove(request);
  assert(removed.state == replay::ReplayFileActionState::UserDeleted &&
         removed.changed && !std::filesystem::exists(path));
}

} // namespace

int main() {
  testVerifiedShareUsesStableSnapshotAndDeleteKeepsResult();
  testCorruptOwnedEntryRemainsDeletable();
  return 0;
}
