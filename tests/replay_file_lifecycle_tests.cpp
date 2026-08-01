#include "replay/ReplayFileLifecycle.h"

#include <filesystem>
#include <iostream>
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

replay::ReplayFileMetadata metadata() {
  return {
      .relativePath =
          std::filesystem::path("replay") / (std::string(64, 'a') + ".brd"),
      .sha256 = std::string(64, 'b'),
      .compressedSize = 42,
      .codecVersion = 3,
  };
}

void testOrderedOwnershipTransition() {
  auto lifecycle = replay::reservedReplayFileLifecycle("attempt-1");
  expect(lifecycle.state == replay::ReplayFileLifecycleState::Reserved &&
             !lifecycle.receipt.has_value(),
         "reservation starts without an ownership receipt");

  auto transition = replay::advanceReplayFileLifecycle(
      lifecycle, replay::ReplayFileLifecycleEvent::TemporaryWritten);
  expect(transition.lifecycle &&
             transition.lifecycle->state ==
                 replay::ReplayFileLifecycleState::TemporaryWritten,
         "private durable temporary write advances lifecycle");
  lifecycle = *transition.lifecycle;

  const replay::ReplayFileOwnershipReceipt receipt{.attemptToken = "attempt-1",
                                                   .metadata = metadata()};
  transition = replay::advanceReplayFileLifecycle(
      lifecycle, replay::ReplayFileLifecycleEvent::InstallVerified, receipt);
  expect(transition.lifecycle &&
             transition.lifecycle->state ==
                 replay::ReplayFileLifecycleState::InstalledUnassociated &&
             transition.lifecycle->receipt == receipt,
         "verified installation records exact attempt and bytes");
  lifecycle = *transition.lifecycle;

  transition = replay::advanceReplayFileLifecycle(
      lifecycle, replay::ReplayFileLifecycleEvent::AssociationAcknowledged,
      receipt);
  expect(transition.lifecycle &&
             transition.lifecycle->state ==
                 replay::ReplayFileLifecycleState::Associated,
         "only an exact database acknowledgement establishes ownership");
  lifecycle = *transition.lifecycle;

  transition = replay::advanceReplayFileLifecycle(
      lifecycle, replay::ReplayFileLifecycleEvent::Finalized, receipt);
  expect(transition.lifecycle &&
             transition.lifecycle->state ==
                 replay::ReplayFileLifecycleState::Finalized,
         "acknowledged association can be finalized");
}

void testSkippingOrChangingProofFailsClosed() {
  auto reserved = replay::reservedReplayFileLifecycle("attempt-1");
  const replay::ReplayFileOwnershipReceipt receipt{.attemptToken = "attempt-1",
                                                   .metadata = metadata()};
  expect(!replay::advanceReplayFileLifecycle(
              reserved,
              replay::ReplayFileLifecycleEvent::AssociationAcknowledged,
              receipt)
              .lifecycle,
         "reservation cannot skip physical installation");
  expect(
      !replay::advanceReplayFileLifecycle(
           reserved, replay::ReplayFileLifecycleEvent::InstallVerified, receipt)
           .lifecycle,
      "installation cannot skip the durable temporary state");

  auto temporary =
      *replay::advanceReplayFileLifecycle(
           reserved, replay::ReplayFileLifecycleEvent::TemporaryWritten)
           .lifecycle;
  auto installed =
      *replay::advanceReplayFileLifecycle(
           temporary, replay::ReplayFileLifecycleEvent::InstallVerified,
           receipt)
           .lifecycle;
  auto wrongAttempt = receipt;
  wrongAttempt.attemptToken = "attempt-2";
  expect(!replay::advanceReplayFileLifecycle(
              installed,
              replay::ReplayFileLifecycleEvent::AssociationAcknowledged,
              wrongAttempt)
              .lifecycle,
         "different attempt cannot acknowledge installed bytes");
  auto wrongBytes = receipt;
  wrongBytes.metadata.sha256[0] = 'c';
  expect(!replay::advanceReplayFileLifecycle(
              installed,
              replay::ReplayFileLifecycleEvent::AssociationAcknowledged,
              wrongBytes)
              .lifecycle,
         "different bytes cannot acknowledge installed path");
}

void testIdempotenceAndAbandonment() {
  auto reserved = replay::reservedReplayFileLifecycle("attempt-1");
  const replay::ReplayFileOwnershipReceipt receipt{.attemptToken = "attempt-1",
                                                   .metadata = metadata()};
  auto temporary =
      *replay::advanceReplayFileLifecycle(
           reserved, replay::ReplayFileLifecycleEvent::TemporaryWritten)
           .lifecycle;
  expect(replay::advanceReplayFileLifecycle(
             temporary, replay::ReplayFileLifecycleEvent::TemporaryWritten)
                 .lifecycle == std::optional(temporary),
         "temporary-write acknowledgement is idempotent");
  auto installed =
      *replay::advanceReplayFileLifecycle(
           temporary, replay::ReplayFileLifecycleEvent::InstallVerified,
           receipt)
           .lifecycle;
  expect(
      replay::advanceReplayFileLifecycle(
          installed, replay::ReplayFileLifecycleEvent::InstallVerified, receipt)
              .lifecycle == std::optional(installed),
      "exact installation acknowledgement is idempotent");

  expect(replay::advanceReplayFileLifecycle(
             installed, replay::ReplayFileLifecycleEvent::Abandon, receipt)
                 .lifecycle->state ==
             replay::ReplayFileLifecycleState::Abandoned,
         "unassociated installation may be explicitly abandoned");
  auto associated =
      *replay::advanceReplayFileLifecycle(
           installed, replay::ReplayFileLifecycleEvent::AssociationAcknowledged,
           receipt)
           .lifecycle;
  expect(!replay::advanceReplayFileLifecycle(
              associated, replay::ReplayFileLifecycleEvent::Abandon, receipt)
              .lifecycle,
         "associated file cannot be abandoned outside database ownership");
}

} // namespace

int main() {
  testOrderedOwnershipTransition();
  testSkippingOrChangingProofFailsClosed();
  testIdempotenceAndAbandonment();
  if (failures != 0) {
    std::cerr << failures << " replay lifecycle test(s) failed\n";
    return 1;
  }
  std::cout << "replay lifecycle tests passed\n";
  return 0;
}
