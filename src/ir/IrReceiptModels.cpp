#include "IrReceiptModels.h"

#include "IrOutboxModels.h"
#include "IrProfileSettings.h"
#include "../Uuid.h"

#include <algorithm>
#include <string_view>

namespace ir {
namespace {

bool isLowerHexDigest(std::string_view value,
                      std::size_t expectedSize) noexcept {
  return value.size() == expectedSize &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool isValidProviderId(std::string_view value) noexcept {
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

bool isBoundedRemoteId(std::string_view value) noexcept {
  return value.size() <= kMaximumIrRemoteValueBytes &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character < 0x20U || character == 0x7fU;
         });
}

bool isKnownSource(IrReceiptConfirmationSource source) noexcept {
  switch (source) {
  case IrReceiptConfirmationSource::Submission:
  case IrReceiptConfirmationSource::Snapshot:
    return true;
  }
  return false;
}

bool validateDraft(const IrSuccessfulReceiptDraft &draft,
                   std::string &diagnostic) {
  const auto normalizedOrigin = normalizeServerOrigin(draft.serverOrigin);
  if (!normalizedOrigin || *normalizedOrigin != draft.serverOrigin) {
    diagnostic = "IR receipt server origin is invalid or not normalized";
  } else if (draft.remoteUserId && *draft.remoteUserId <= 0) {
    diagnostic = "IR receipt remote user ID is invalid";
  } else if (!isBoundedRemoteId(draft.remoteChartId)) {
    diagnostic = "IR receipt remote chart ID is invalid";
  } else if (!isBoundedRemoteId(draft.remoteScoreId)) {
    diagnostic = "IR receipt remote score ID is invalid";
  } else if (!isKnownSource(draft.source)) {
    diagnostic = "IR receipt confirmation source is invalid";
  } else if (draft.confirmedAtUnixMillis <= 0) {
    diagnostic = "IR receipt confirmation time is invalid";
  } else {
    diagnostic.clear();
    return true;
  }
  return false;
}

} // namespace

bool validateIrSuccessfulReceiptDraft(const IrSuccessfulReceiptDraft &draft,
                                      std::string &diagnostic) noexcept {
  try {
    return validateDraft(draft, diagnostic);
  } catch (...) {
    diagnostic = "IR receipt draft validation failed";
    return false;
  }
}

bool validateIrSubmissionReceipt(const IrSubmissionReceipt &receipt,
                                 std::string &diagnostic) noexcept {
  try {
    if (receipt.id <= 0) {
      diagnostic = "IR receipt row ID is invalid";
    } else if (!isValidProviderId(receipt.providerId)) {
      diagnostic = "IR receipt provider ID is invalid";
    } else if (receipt.replayId <= 0) {
      diagnostic = "IR receipt replay ID is invalid";
    } else if (!uuid::isCanonicalLowerV4(receipt.attemptId)) {
      diagnostic = "IR receipt attempt ID is invalid";
    } else if (!receipt.chartMd5.empty() &&
               !isLowerHexDigest(receipt.chartMd5, 32)) {
      diagnostic = "IR receipt chart MD5 is invalid";
    } else if (!isLowerHexDigest(receipt.chartSha256, 64)) {
      diagnostic = "IR receipt chart SHA-256 is invalid";
    } else {
      const IrSuccessfulReceiptDraft draft{
          .serverOrigin = receipt.serverOrigin,
          .remoteUserId = receipt.remoteUserId,
          .remoteChartId = receipt.remoteChartId,
          .remoteScoreId = receipt.remoteScoreId,
          .source = receipt.source,
          .observedInSnapshot = receipt.observedInSnapshot,
          .confirmedAtUnixMillis = receipt.confirmedAtUnixMillis,
      };
      return validateDraft(draft, diagnostic);
    }
    return false;
  } catch (...) {
    diagnostic = "IR receipt row validation failed";
    return false;
  }
}

} // namespace ir
