#include "IrOutboxModels.h"

#include "../Uuid.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>

namespace ir {
namespace {

std::size_t utf8Boundary(std::string_view value, std::size_t maximum) {
  std::size_t length = std::min(value.size(), maximum);
  while (length > 0 && length < value.size() &&
         (static_cast<unsigned char>(value[length]) & 0xc0U) == 0x80U) {
    --length;
  }
  return length;
}

bool lowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

bool validProviderId(std::string_view value) {
  if (value.empty() || value.size() > kMaximumIrProviderIdBytes ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-';
  });
}

bool validPayload(std::string_view payload) {
  return !payload.empty() && payload.size() <= kMaximumIrPayloadBytes &&
         nlohmann::json::accept(payload);
}

bool validRemotePair(const IrOutboxEntry &entry) {
  const bool hasJob = !entry.remoteJobId.empty();
  const bool hasOrigin = !entry.remoteOrigin.empty();
  if (hasJob != hasOrigin ||
      entry.remoteJobId.size() > kMaximumIrRemoteValueBytes ||
      entry.remoteOrigin.size() > kMaximumIrRemoteValueBytes) {
    return false;
  }
  switch (entry.state) {
  case IrOutboxState::AwaitingRemoteResult:
    return hasJob;
  case IrOutboxState::Uploading:
    return true;
  case IrOutboxState::Pending:
  case IrOutboxState::BlockedConfiguration:
  case IrOutboxState::FailedPermanent:
  case IrOutboxState::Succeeded:
    return !hasJob;
  }
  return false;
}

} // namespace

std::string sanitizeDiagnostic(std::string_view value) {
  std::string result(
      value.substr(0, utf8Boundary(value, kMaximumDiagnosticBytes)));
  for (char &character : result) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if ((byte < 0x20U && character != '\n' && character != '\t') ||
        byte == 0x7fU) {
      character = ' ';
    }
  }
  return result;
}

bool isKnownIrOutboxState(int value) noexcept {
  return value >= static_cast<int>(IrOutboxState::Pending) &&
         value <= static_cast<int>(IrOutboxState::Succeeded);
}

bool validateIrOutboxDraft(const IrOutboxDraft &draft,
                           std::string &diagnostic) noexcept {
  try {
    if (!validProviderId(draft.providerId)) {
      diagnostic = "IR provider ID is invalid";
    } else if (!uuid::isCanonicalLowerV4(draft.attemptId)) {
      diagnostic = "IR attempt ID is invalid";
    } else if (!draft.chartMd5.empty() && !lowerHex(draft.chartMd5, 32)) {
      diagnostic = "IR chart MD5 is invalid";
    } else if (!lowerHex(draft.chartSha256, 64)) {
      diagnostic = "IR chart SHA-256 is invalid";
    } else if (!validPayload(draft.payloadJson)) {
      diagnostic = "IR payload is malformed or oversized";
    } else if (draft.createdAtUnixMillis < 0) {
      diagnostic = "IR creation time is invalid";
    } else {
      diagnostic.clear();
      return true;
    }
    return false;
  } catch (...) {
    diagnostic = "IR draft validation failed";
    return false;
  }
}

bool validateIrOutboxEntry(const IrOutboxEntry &entry,
                           std::string &diagnostic) noexcept {
  try {
    const IrOutboxDraft draft{
        .providerId = entry.providerId,
        .attemptId = entry.attemptId,
        .chartMd5 = entry.chartMd5,
        .chartSha256 = entry.chartSha256,
        .payloadJson = entry.payloadJson,
        .createdAtUnixMillis = entry.createdAtUnixMillis,
    };
    std::string draftDiagnostic;
    if (entry.id <= 0 || !validateIrOutboxDraft(draft, draftDiagnostic)) {
      diagnostic = entry.id <= 0 ? "IR outbox row ID is invalid"
                                 : std::move(draftDiagnostic);
      return false;
    }
    if (entry.requestAttemptCount < 0 || entry.consecutiveFailureCount < 0 ||
        entry.updatedAtUnixMillis < 0 ||
        (entry.nextAttemptAtUnixMillis &&
         *entry.nextAttemptAtUnixMillis < 0) ||
        (entry.completedAtUnixMillis && *entry.completedAtUnixMillis < 0)) {
      diagnostic = "IR outbox counters or times are invalid";
      return false;
    }
    if (!validRemotePair(entry)) {
      diagnostic = "IR outbox remote job state is invalid";
      return false;
    }
    if ((!entry.localResultReady &&
         entry.state != IrOutboxState::Pending) ||
        (entry.nextRequestUserIntent &&
         entry.state != IrOutboxState::Pending)) {
      diagnostic = "IR outbox readiness or user-intent state is invalid";
      return false;
    }
    if (entry.lastErrorCode.size() > kMaximumIrErrorCodeBytes ||
        entry.lastErrorMessage.size() > kMaximumDiagnosticBytes) {
      diagnostic = "IR outbox error fields are oversized";
      return false;
    }
    if ((entry.state == IrOutboxState::Succeeded) !=
        entry.completedAtUnixMillis.has_value()) {
      diagnostic = "IR outbox completion state is invalid";
      return false;
    }
    diagnostic.clear();
    return true;
  } catch (...) {
    diagnostic = "IR outbox row validation failed";
    return false;
  }
}

} // namespace ir
