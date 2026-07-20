#include "IrScoreReconciliation.h"

#include "IrProfileSettings.h"
#include "../Uuid.h"
#include "../scene/play/GameplayGaugeTypes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace ir {
namespace {

struct ProofKey {
  std::string_view game;
  std::string_view hash;
  int score = 0;
  int lampRank = 0;

  bool operator==(const ProofKey &) const = default;
};

struct ProofKeyHash {
  std::size_t operator()(const ProofKey &key) const noexcept {
    std::size_t value = std::hash<std::string_view>{}(key.game);
    value ^= std::hash<std::string_view>{}(key.hash) + 0x9e3779b9U +
             (value << 6U) + (value >> 2U);
    value ^= std::hash<int>{}(key.score) + 0x9e3779b9U + (value << 6U) +
             (value >> 2U);
    value ^= std::hash<int>{}(key.lampRank) + 0x9e3779b9U + (value << 6U) +
             (value >> 2U);
    return value;
  }
};

using ProofIndex =
    std::unordered_map<ProofKey, std::vector<const IrRemoteScore *>,
                       ProofKeyHash>;

IrScoreReconciliationPlan invalidPlan(std::string_view diagnostic) {
  return {.status = IrScoreReconciliationPlan::Status::Invalid,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
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

bool lowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

bool knownLampRank(int value) {
  constexpr std::array ranks{
      kClearTypeFailedRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeLightAssistedEasyClearRank,
      kClearTypeEasyClearRank,
      kClearTypeNormalClearRank,
      kClearTypeHardClearRank,
      kClearTypeExHardClearRank,
      kClearTypeFullComboRank,
  };
  return std::ranges::find(ranks, value) != ranks.end();
}

bool validLocalCandidate(const IrLocalReceiptCandidate &candidate,
                         std::string &diagnostic) {
  if (candidate.replayId <= 0 ||
      !uuid::isCanonicalLowerV4(candidate.attemptId)) {
    diagnostic = "IR reconciliation local identity is invalid";
    return false;
  }
  if (candidate.keyMode != 0 && candidate.keyMode != 7 &&
      candidate.keyMode != 14) {
    diagnostic = "IR reconciliation local key mode is invalid";
    return false;
  }
  if ((!candidate.chartMd5.empty() && !lowerHex(candidate.chartMd5, 32)) ||
      (!candidate.chartSha256.empty() &&
       !lowerHex(candidate.chartSha256, 64)) ||
      (candidate.chartMd5.empty() && candidate.chartSha256.empty())) {
    diagnostic = "IR reconciliation local chart hash is invalid";
    return false;
  }
  if (candidate.score < 0 || !knownLampRank(candidate.lampRank)) {
    diagnostic = "IR reconciliation local score or lamp is invalid";
    return false;
  }
  if (candidate.outboxRowId.has_value() != candidate.outboxState.has_value()) {
    diagnostic = "IR reconciliation outbox identity is incomplete";
    return false;
  }
  if (candidate.outboxRowId &&
      (*candidate.outboxRowId <= 0 ||
       !isKnownIrOutboxState(static_cast<int>(*candidate.outboxState)))) {
    diagnostic = "IR reconciliation outbox row is invalid";
    return false;
  }
  return true;
}

bool hashesAgree(const IrLocalReceiptCandidate &local,
                 const IrRemoteScore &remote) {
  if (!local.chartSha256.empty() && !remote.chartSha256.empty()) {
    if (local.chartSha256 != remote.chartSha256) {
      return false;
    }
    return local.chartMd5.empty() || remote.chartMd5.empty() ||
           local.chartMd5 == remote.chartMd5;
  }
  return !local.chartMd5.empty() && !remote.chartMd5.empty() &&
         local.chartMd5 == remote.chartMd5;
}

std::array<std::string_view, 2> candidateGames(int keyMode) {
  if (keyMode == 7) {
    return {"bms-7k", {}};
  }
  if (keyMode == 14) {
    return {"bms-14k", {}};
  }
  return {"bms-7k", "bms-14k"};
}

const IrRemoteScore *findUniqueProofMatch(const IrLocalReceiptCandidate &local,
                                          const ProofIndex &sha256Index,
                                          const ProofIndex &md5Index) {
  std::unordered_set<const IrRemoteScore *> possible;
  const auto games = candidateGames(local.keyMode);
  for (const std::string_view game : games) {
    if (game.empty()) {
      continue;
    }
    if (!local.chartSha256.empty()) {
      const auto found = sha256Index.find({.game = game,
                                           .hash = local.chartSha256,
                                           .score = local.score,
                                           .lampRank = local.lampRank});
      if (found != sha256Index.end()) {
        possible.insert(found->second.begin(), found->second.end());
      }
    }
    if (!local.chartMd5.empty()) {
      const auto found = md5Index.find({.game = game,
                                        .hash = local.chartMd5,
                                        .score = local.score,
                                        .lampRank = local.lampRank});
      if (found != md5Index.end()) {
        possible.insert(found->second.begin(), found->second.end());
      }
    }
  }

  const IrRemoteScore *match = nullptr;
  bool disagreement = false;
  for (const IrRemoteScore *remote : possible) {
    if (!hashesAgree(local, *remote)) {
      disagreement = true;
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = remote;
  }
  return disagreement ? nullptr : match;
}

IrSubmissionReceipt snapshotReceipt(std::string_view providerId,
                                    std::string_view serverOrigin,
                                    const IrLocalReceiptCandidate &local,
                                    const IrRemoteScore &remote,
                                    std::int64_t confirmedAtUnixMillis) {
  return {
      .id = 0,
      .providerId = std::string(providerId),
      .serverOrigin = std::string(serverOrigin),
      .replayId = local.replayId,
      .attemptId = local.attemptId,
      .chartMd5 = local.chartMd5.empty() ? remote.chartMd5 : local.chartMd5,
      .chartSha256 =
          local.chartSha256.empty() ? remote.chartSha256 : local.chartSha256,
      .remoteUserId = remote.remoteUserId,
      .remoteChartId = remote.remoteChartId,
      .remoteScoreId = remote.remoteScoreId,
      .source = IrReceiptConfirmationSource::Snapshot,
      .observedInSnapshot = true,
      .confirmedAtUnixMillis = confirmedAtUnixMillis,
  };
}

void recordRepresentedOutbox(const IrLocalReceiptCandidate &local,
                             bool submissionOwnsDelivery,
                             IrScoreReconciliationPlan &plan) {
  if (!local.outboxRowId || !local.outboxState) {
    return;
  }
  switch (*local.outboxState) {
  case IrOutboxState::Pending:
  case IrOutboxState::BlockedConfiguration:
  case IrOutboxState::FailedPermanent:
    plan.settledOutboxRowIds.push_back(*local.outboxRowId);
    break;
  case IrOutboxState::Succeeded:
    if (submissionOwnsDelivery) {
      plan.purgedSucceededOutboxRowIds.push_back(*local.outboxRowId);
    }
    break;
  case IrOutboxState::Uploading:
  case IrOutboxState::AwaitingRemoteResult:
    break;
  }
}

} // namespace

IrScoreReconciliationPlan planScoreReconciliation(
    std::string_view providerId, std::string_view serverOrigin,
    std::span<const IrLocalReceiptCandidate> local,
    std::span<const IrRemoteScore> remote, std::int64_t confirmedAtUnixMillis) {
  IrScoreReconciliationPlan plan;
  const auto normalizedOrigin = normalizeServerOrigin(serverOrigin);
  if (!validProviderId(providerId) || !normalizedOrigin ||
      *normalizedOrigin != serverOrigin || confirmedAtUnixMillis <= 0) {
    return invalidPlan("IR reconciliation identity or confirmation time is "
                       "invalid");
  }
  if (local.size() > kMaximumIrRemoteScoreSnapshotEntries ||
      remote.size() > kMaximumIrRemoteScoreSnapshotEntries) {
    return invalidPlan("IR reconciliation input is oversized");
  }
  std::unordered_set<int> replayIds;
  std::unordered_set<std::string_view> attemptIds;
  std::unordered_set<std::int64_t> outboxIds;
  replayIds.reserve(local.size());
  attemptIds.reserve(local.size());
  outboxIds.reserve(local.size());
  for (const auto &candidate : local) {
    std::string diagnostic;
    if (!validLocalCandidate(candidate, diagnostic)) {
      return invalidPlan(diagnostic);
    }
    if (!replayIds.emplace(candidate.replayId).second ||
        !attemptIds.emplace(candidate.attemptId).second ||
        (candidate.outboxRowId &&
         !outboxIds.emplace(*candidate.outboxRowId).second)) {
      return invalidPlan("IR reconciliation local identities are duplicated");
    }
    if (candidate.outboxState == IrOutboxState::Uploading) {
      return invalidPlan(
          "IR reconciliation cannot run with active outbox work");
    }
    if (candidate.currentReceipt &&
        candidate.currentReceipt->providerId == providerId &&
        candidate.currentReceipt->serverOrigin == serverOrigin) {
      if (!validateIrSubmissionReceipt(*candidate.currentReceipt, diagnostic) ||
          candidate.currentReceipt->replayId != candidate.replayId ||
          candidate.currentReceipt->attemptId != candidate.attemptId ||
          (!candidate.chartMd5.empty() &&
           !candidate.currentReceipt->chartMd5.empty() &&
           candidate.currentReceipt->chartMd5 != candidate.chartMd5) ||
          (!candidate.chartSha256.empty() &&
           candidate.currentReceipt->chartSha256 != candidate.chartSha256)) {
        return invalidPlan(diagnostic.empty()
                               ? "IR reconciliation receipt disagrees with "
                                 "its local replay"
                               : diagnostic);
      }
    }
  }
  std::unordered_map<std::string_view, const IrRemoteScore *> remoteById;
  ProofIndex sha256Index;
  ProofIndex md5Index;
  remoteById.reserve(remote.size());
  sha256Index.reserve(remote.size());
  md5Index.reserve(remote.size());
  for (const auto &score : remote) {
    std::string diagnostic;
    if (!validateIrRemoteScore(score, diagnostic)) {
      return invalidPlan(diagnostic);
    }
    if (!remoteById.emplace(score.remoteScoreId, &score).second) {
      return invalidPlan("IR reconciliation remote identities are duplicated");
    }
    if (!score.chartSha256.empty()) {
      sha256Index[{.game = score.game,
                   .hash = score.chartSha256,
                   .score = score.score,
                   .lampRank = score.lampRank}]
          .push_back(&score);
    }
    if (!score.chartMd5.empty()) {
      md5Index[{.game = score.game,
                .hash = score.chartMd5,
                .score = score.score,
                .lampRank = score.lampRank}]
          .push_back(&score);
    }
  }

  for (const auto &candidate : local) {
    const auto preserveAmbiguousSubmission = [&] {
      if (candidate.currentReceipt &&
          candidate.currentReceipt->source ==
              IrReceiptConfirmationSource::Submission &&
          !candidate.currentReceipt->observedInSnapshot) {
        ++plan.ambiguousReceiptsPreserved;
      }
    };
    if (candidate.currentReceipt &&
        (candidate.currentReceipt->providerId != providerId ||
         candidate.currentReceipt->serverOrigin != serverOrigin)) {
      continue;
    }
    if (candidate.currentReceipt) {
      const auto found =
          remoteById.find(candidate.currentReceipt->remoteScoreId);
      if (found != remoteById.end()) {
        IrSubmissionReceipt receipt = *candidate.currentReceipt;
        receipt.remoteUserId = found->second->remoteUserId;
        receipt.remoteChartId = found->second->remoteChartId;
        receipt.observedInSnapshot = true;
        receipt.confirmedAtUnixMillis = confirmedAtUnixMillis;
        const bool submissionOwnsDelivery =
            receipt.source == IrReceiptConfirmationSource::Submission;
        plan.upsertedReceipts.push_back(std::move(receipt));
        recordRepresentedOutbox(candidate, submissionOwnsDelivery, plan);
        continue;
      }
      if (candidate.currentReceipt->observedInSnapshot) {
        plan.deletedReceiptIds.push_back(candidate.currentReceipt->id);
        continue;
      }
    }
    if (!candidate.eligible) {
      preserveAmbiguousSubmission();
      continue;
    }
    const IrRemoteScore *match =
        findUniqueProofMatch(candidate, sha256Index, md5Index);
    if (match == nullptr || match->chartSha256.empty()) {
      preserveAmbiguousSubmission();
      continue;
    }
    IrSubmissionReceipt receipt = snapshotReceipt(
        providerId, serverOrigin, candidate, *match, confirmedAtUnixMillis);
    if (candidate.currentReceipt) {
      receipt.id = candidate.currentReceipt->id;
      receipt.source = candidate.currentReceipt->source;
    }
    const bool submissionOwnsDelivery =
        receipt.source == IrReceiptConfirmationSource::Submission;
    plan.upsertedReceipts.push_back(std::move(receipt));
    recordRepresentedOutbox(candidate, submissionOwnsDelivery, plan);
  }
  std::ranges::sort(plan.upsertedReceipts, {}, &IrSubmissionReceipt::replayId);
  std::ranges::sort(plan.deletedReceiptIds);
  std::ranges::sort(plan.settledOutboxRowIds);
  std::ranges::sort(plan.purgedSucceededOutboxRowIds);
  return plan;
}

} // namespace ir
