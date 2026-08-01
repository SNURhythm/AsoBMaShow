#include "IrOutboxModels.h"

#include "../CanonicalDigest.h"
#include "../Uuid.h"

#include "nlohmann/json.hpp"

#include <algorithm>

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

bool validPayload(std::string_view payload) {
  return !payload.empty() && payload.size() <= kMaximumIrPayloadBytes &&
         nlohmann::json::accept(payload);
}

bool validRulesetProof(const IrRulesetProof &proof) {
  return ir::isValidProviderId(proof.rulesetId) &&
         proof.rulesetRevision > 0 &&
         canonical_digest::isCanonicalLowerHex(proof.validationFingerprint,
                                                64);
}

bool legacyRulesetProof(const IrRulesetProof &proof) {
  return proof.rulesetId == "legacy-unknown" && proof.rulesetRevision == 0 &&
         proof.validationFingerprint.empty();
}

bool validateDraft(const IrOutboxDraft &draft, bool allowLegacyProof,
                   std::string &diagnostic) {
  if (!ir::isValidProviderId(draft.providerId)) {
    diagnostic = "IR provider ID is invalid";
  } else if (!uuid::isCanonicalLowerV4(draft.attemptId)) {
    diagnostic = "IR attempt ID is invalid";
  } else if (!draft.chartMd5.empty() &&
             !canonical_digest::isCanonicalLowerHex(draft.chartMd5, 32)) {
    diagnostic = "IR chart MD5 is invalid";
  } else if (!canonical_digest::isCanonicalLowerHex(draft.chartSha256, 64)) {
    diagnostic = "IR chart SHA-256 is invalid";
  } else if (!validPayload(draft.payloadJson)) {
    diagnostic = "IR payload is malformed or oversized";
  } else if (!validRulesetProof(draft.rulesetProof) &&
             !(allowLegacyProof && legacyRulesetProof(draft.rulesetProof))) {
    diagnostic = "IR ruleset proof is invalid";
  } else if (draft.createdAtUnixMillis < 0) {
    diagnostic = "IR creation time is invalid";
  } else {
    diagnostic.clear();
    return true;
  }
  return false;
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
  case IrOutboxState::BlockedConfiguration:
    return true;
  case IrOutboxState::Pending:
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
    return validateDraft(draft, false, diagnostic);
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
        .rulesetProof = entry.rulesetProof,
        .createdAtUnixMillis = entry.createdAtUnixMillis,
    };
    const bool legacyProof = legacyRulesetProof(entry.rulesetProof);
    std::string draftDiagnostic;
    if (entry.id <= 0 ||
        !validateDraft(draft, legacyProof, draftDiagnostic)) {
      diagnostic = entry.id <= 0 ? "IR outbox row ID is invalid"
                                 : std::move(draftDiagnostic);
      return false;
    }
    if (entry.requestAttemptCount < 0 || entry.consecutiveFailureCount < 0 ||
        entry.remotePollCount < 0 || entry.updatedAtUnixMillis < 0 ||
        (entry.nextAttemptAtUnixMillis && *entry.nextAttemptAtUnixMillis < 0) ||
        (entry.completedAtUnixMillis && *entry.completedAtUnixMillis < 0)) {
      diagnostic = "IR outbox counters or times are invalid";
      return false;
    }
    if (!validRemotePair(entry)) {
      diagnostic = "IR outbox remote job state is invalid";
      return false;
    }
    if (legacyProof &&
        (entry.state != IrOutboxState::BlockedConfiguration ||
         entry.lastErrorCode != "legacy_ruleset_proof_missing" ||
         entry.nextAttemptAtUnixMillis.has_value() ||
         entry.nextRequestUserIntent)) {
      diagnostic = "legacy IR ruleset proof state is invalid";
      return false;
    }
    const bool blockedPendingIntent =
        entry.state == IrOutboxState::BlockedConfiguration &&
        entry.remoteJobId.empty();
    if ((!legacyProof && !entry.localResultReady &&
         entry.state != IrOutboxState::Pending) ||
        (entry.nextRequestUserIntent && entry.state != IrOutboxState::Pending &&
         !blockedPendingIntent)) {
      diagnostic = "IR outbox readiness or user-intent state is invalid";
      return false;
    }
    if (entry.lastErrorCode.size() > kMaximumIrErrorCodeBytes ||
        entry.lastErrorMessage.size() > kMaximumDiagnosticBytes) {
      diagnostic = "IR outbox error fields are oversized";
      return false;
    }
    if (!legacyProof &&
        ((entry.state == IrOutboxState::Succeeded) !=
         entry.completedAtUnixMillis.has_value())) {
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
