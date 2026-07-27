#include "ReplayFileLifecycle.h"

#include "BeatorajaReplayPath.h"
#include "ReplayFormat.h"

#include <string_view>

namespace replay {
namespace {

bool validReceipt(const ReplayFileOwnershipReceipt &receipt,
                  std::string_view attemptToken) {
  std::string diagnostic;
  return !attemptToken.empty() && receipt.attemptToken == attemptToken &&
         isCanonicalReplayRelativePath(receipt.metadata.relativePath,
                                       diagnostic) &&
         isCanonicalLowerHex(receipt.metadata.sha256, 64) &&
         receipt.metadata.compressedSize > 0 &&
         receipt.metadata.codecVersion > 0;
}

ReplayFileLifecycleTransition rejected(std::string diagnostic) {
  return {.diagnostic = std::move(diagnostic)};
}

bool exactReceipt(const ReplayFileLifecycle &current,
                  const std::optional<ReplayFileOwnershipReceipt> &receipt) {
  return receipt && current.receipt && *receipt == *current.receipt;
}

} // namespace

ReplayFileLifecycle reservedReplayFileLifecycle(std::string attemptToken) {
  return {.state = ReplayFileLifecycleState::Reserved,
          .attemptToken = std::move(attemptToken)};
}

ReplayFileLifecycleTransition
advanceReplayFileLifecycle(const ReplayFileLifecycle &current,
                           ReplayFileLifecycleEvent event,
                           std::optional<ReplayFileOwnershipReceipt> receipt) {
  if (current.attemptToken.empty()) {
    return rejected("Replay file lifecycle has no attempt token");
  }
  ReplayFileLifecycle next = current;
  switch (current.state) {
  case ReplayFileLifecycleState::Reserved:
    if (event == ReplayFileLifecycleEvent::TemporaryWritten && !receipt) {
      next.state = ReplayFileLifecycleState::TemporaryWritten;
      return {.lifecycle = std::move(next)};
    }
    if (event == ReplayFileLifecycleEvent::Abandon && !receipt) {
      next.state = ReplayFileLifecycleState::Abandoned;
      return {.lifecycle = std::move(next)};
    }
    break;
  case ReplayFileLifecycleState::TemporaryWritten:
    if (event == ReplayFileLifecycleEvent::TemporaryWritten && !receipt) {
      return {.lifecycle = current};
    }
    if (event == ReplayFileLifecycleEvent::InstallVerified && receipt &&
        validReceipt(*receipt, current.attemptToken)) {
      next.state = ReplayFileLifecycleState::InstalledUnassociated;
      next.receipt = std::move(receipt);
      return {.lifecycle = std::move(next)};
    }
    if (event == ReplayFileLifecycleEvent::Abandon && !receipt) {
      next.state = ReplayFileLifecycleState::Abandoned;
      return {.lifecycle = std::move(next)};
    }
    break;
  case ReplayFileLifecycleState::InstalledUnassociated:
    if (event == ReplayFileLifecycleEvent::InstallVerified &&
        exactReceipt(current, receipt)) {
      return {.lifecycle = current};
    }
    if (event == ReplayFileLifecycleEvent::AssociationAcknowledged &&
        exactReceipt(current, receipt)) {
      next.state = ReplayFileLifecycleState::Associated;
      return {.lifecycle = std::move(next)};
    }
    if (event == ReplayFileLifecycleEvent::Abandon &&
        exactReceipt(current, receipt)) {
      next.state = ReplayFileLifecycleState::Abandoned;
      return {.lifecycle = std::move(next)};
    }
    break;
  case ReplayFileLifecycleState::Associated:
    if (event == ReplayFileLifecycleEvent::AssociationAcknowledged &&
        exactReceipt(current, receipt)) {
      return {.lifecycle = current};
    }
    if (event == ReplayFileLifecycleEvent::Finalized &&
        exactReceipt(current, receipt)) {
      next.state = ReplayFileLifecycleState::Finalized;
      return {.lifecycle = std::move(next)};
    }
    break;
  case ReplayFileLifecycleState::Finalized:
    if (event == ReplayFileLifecycleEvent::Finalized &&
        exactReceipt(current, receipt)) {
      return {.lifecycle = current};
    }
    break;
  case ReplayFileLifecycleState::Abandoned:
    if (event == ReplayFileLifecycleEvent::Abandon &&
        (!receipt || exactReceipt(current, receipt))) {
      return {.lifecycle = current};
    }
    break;
  }
  return rejected("Replay file lifecycle transition is invalid");
}

} // namespace replay
