#include "replay/ReplayProfileTransfer.h"

#include "replay/BeatorajaReplayPath.h"
#include "replay/ReplayFileStore.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static unsigned long long serial = 0;
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-replay-transfer-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(++serial));
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

Bytes bytes(std::string_view value) {
  const auto raw = std::as_bytes(std::span(value.data(), value.size()));
  return {raw.begin(), raw.end()};
}

ModernReplayFileInventoryEntry install(
    const std::filesystem::path &profileRoot, int history,
    std::string attemptId, std::string_view payload, bool userDeleted = false) {
  std::string diagnostic;
  const auto stem = replay::chartStem(std::string(64, 'a'), 1, false,
                                      diagnostic);
  const auto identity = replay::pathForStem(*stem, history, diagnostic);
  replay::ReplayFileStore store(profileRoot);
  const Bytes contents = bytes(payload);
  const auto reservation = store.reserve(*identity, contents, attemptId);
  expect(reservation.reservation.has_value(), "test replay reserves");
  const auto installed = store.install(*reservation.reservation, contents);
  expect(installed.file.has_value(), "test replay installs");
  return {.owner = ModernReplayOwnerKind::ChartResult,
          .attemptId = std::move(attemptId),
          .reference = {.id = history + 1,
                        .resultId = history + 10,
                        .userDeleted = userDeleted,
                        .identity = *identity,
                        .metadata = installed.file->metadata}};
}

void write(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void testCopiesOnlyActiveVerifiedReferences() {
  TemporaryDirectory temporary;
  const auto source = temporary.path / "source";
  const auto destination = temporary.path / "destination";
  std::filesystem::create_directories(source);
  std::filesystem::create_directories(destination);
  auto active = install(source, 0, "attempt-active", "active replay");
  auto missing = install(source, 1, "attempt-missing", "missing replay");
  std::filesystem::remove(source / missing.reference.metadata.relativePath);
  auto deleted = install(source, 2, "attempt-deleted", "deleted replay", true);
  write(source / "replay" / "unreferenced.brd", "unreferenced");

  replay::ReplayProfileTransfer transfer(source, destination);
  const auto outcome = transfer.copy({active, missing, deleted});
  expect(outcome.state == replay::ReplayProfileTransferState::Succeeded &&
             outcome.copiedRelativePaths ==
                 std::vector{active.reference.metadata.relativePath} &&
             outcome.omittedMissing == 1 && outcome.omittedUserDeleted == 1,
         "transfer selects active verified references only");
  replay::ReplayFileStore destinationStore(destination);
  expect(destinationStore.inspect(active.reference.metadata).state ==
             replay::ReplayFileState::Available,
         "copied reference remains byte-for-byte verified");
  expect(!std::filesystem::exists(destination /
                                 missing.reference.metadata.relativePath) &&
             !std::filesystem::exists(destination /
                                      deleted.reference.metadata.relativePath) &&
             !std::filesystem::exists(destination / "replay" /
                                      "unreferenced.brd") &&
             std::filesystem::exists(source / "replay" /
                                     "unreferenced.brd"),
         "omitted and unrelated entries are not promoted or removed");
}

void testCorruptionAndInjectedFailureLeaveNoCopiedFiles() {
  TemporaryDirectory temporary;
  const auto source = temporary.path / "source";
  const auto destination = temporary.path / "destination";
  std::filesystem::create_directories(source);
  std::filesystem::create_directories(destination);
  auto first = install(source, 0, "attempt-first", "first replay");
  auto corrupt = install(source, 1, "attempt-corrupt", "second replay");
  write(source / corrupt.reference.metadata.relativePath, "corrupt");

  replay::ReplayProfileTransfer transfer(source, destination);
  const auto corruptOutcome = transfer.copy({first, corrupt});
  expect(corruptOutcome.state ==
             replay::ReplayProfileTransferState::SourceInvalid &&
             !std::filesystem::exists(destination /
                                      first.reference.metadata.relativePath),
         "source corruption fails closed and rolls back copied output");

  std::filesystem::remove_all(destination);
  std::filesystem::create_directories(destination);
  replay::ReplayProfileTransfer interrupted(
      source, destination,
      {.failAt = [](std::string_view point) { return point == "after-copy"; }});
  const auto interruptedOutcome = interrupted.copy({first});
  expect(interruptedOutcome.state ==
             replay::ReplayProfileTransferState::DestinationFailure &&
             !std::filesystem::exists(destination /
                                      first.reference.metadata.relativePath),
         "injected transfer failure rolls back exact copied output");
}

void testDestinationValidationRejectsMismatchAndUnreferencedFiles() {
  TemporaryDirectory temporary;
  const auto profile = temporary.path / "profile";
  std::filesystem::create_directories(profile);
  auto active = install(profile, 0, "attempt-active", "active replay");
  replay::ReplayProfileTransfer validation(profile, profile);
  expect(validation.validate({active}, true).state ==
             replay::ReplayProfileTransferState::Succeeded,
         "verified referenced-only destination validates");
  write(profile / "replay" / "unreferenced.brd", "unreferenced");
  expect(validation.validate({active}, true).state ==
             replay::ReplayProfileTransferState::DestinationInvalid,
         "unreferenced destination file is rejected");
  std::filesystem::remove(profile / "replay" / "unreferenced.brd");
  write(profile / active.reference.metadata.relativePath, "wrong");
  expect(validation.validate({active}, true).state ==
             replay::ReplayProfileTransferState::DestinationInvalid,
         "mismatched referenced destination file is rejected");
}

void testFutureCodecReplayIsOmittedWithoutFailingResultTransfer() {
  TemporaryDirectory temporary;
  const auto source = temporary.path / "source";
  const auto destination = temporary.path / "destination";
  std::filesystem::create_directories(source);
  std::filesystem::create_directories(destination);
  auto future = install(source, 0, "attempt-future", "future replay");
  future.reference.metadata.codecVersion =
      replay::BeatorajaReplayCodec::kCodecVersion + 1;

  replay::ReplayProfileTransfer transfer(source, destination);
  const auto outcome = transfer.copy({future});
  expect(outcome.state == replay::ReplayProfileTransferState::Succeeded &&
             outcome.copiedRelativePaths.empty() &&
             outcome.omittedUnsupportedCodec == 1 &&
             !std::filesystem::exists(
                 destination / future.reference.metadata.relativePath),
         "unsupported replay bytes are omitted without aborting result "
         "transfer");
}

} // namespace

int main() {
  testCopiesOnlyActiveVerifiedReferences();
  testCorruptionAndInjectedFailureLeaveNoCopiedFiles();
  testDestinationValidationRejectsMismatchAndUnreferencedFiles();
  testFutureCodecReplayIsOmittedWithoutFailingResultTransfer();
  return failures == 0 ? 0 : 1;
}
