#include "ir/IrOutboxModels.h"
#include "ir/IrReceiptModels.h"

#include <cstdint>
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

template <typename T>
concept HasApiKeyMember = requires(T value) { value.apiKey; };

template <typename T>
concept HasAuthorizationMember = requires(T value) { value.authorization; };

template <typename T>
concept HasCredentialMember = requires(T value) { value.credential; };

static_assert(!HasApiKeyMember<ir::IrSuccessfulReceiptDraft>);
static_assert(!HasAuthorizationMember<ir::IrSuccessfulReceiptDraft>);
static_assert(!HasCredentialMember<ir::IrSuccessfulReceiptDraft>);
static_assert(!HasApiKeyMember<ir::IrSubmissionReceipt>);
static_assert(!HasAuthorizationMember<ir::IrSubmissionReceipt>);
static_assert(!HasCredentialMember<ir::IrSubmissionReceipt>);

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

ir::IrSuccessfulReceiptDraft validDraft() {
  return {
      .serverOrigin = "https://boku.tachi.ac",
      .remoteUserId = 42,
      .remoteChartId = "chart-123",
      .remoteScoreId = "score-456",
      .source = ir::IrReceiptConfirmationSource::Submission,
      .observedInSnapshot = false,
      .confirmedAtUnixMillis = 1'700'000'000'123,
  };
}

ir::IrSubmissionReceipt validReceipt() {
  return {
      .id = 7,
      .providerId = "tachi",
      .serverOrigin = "https://boku.tachi.ac",
      .replayId = 11,
      .attemptId = "123e4567-e89b-42d3-a456-426614174000",
      .chartMd5 = repeated('b', 32),
      .chartSha256 = repeated('a', 64),
      .remoteUserId = 42,
      .remoteChartId = "chart-123",
      .remoteScoreId = "score-456",
      .source = ir::IrReceiptConfirmationSource::Submission,
      .observedInSnapshot = false,
      .confirmedAtUnixMillis = 1'700'000'000'123,
  };
}

void expectInvalidDraft(const ir::IrSuccessfulReceiptDraft &draft,
                        std::string_view message) {
  std::string diagnostic;
  expect(!ir::validateIrSuccessfulReceiptDraft(draft, diagnostic), message);
  expect(!diagnostic.empty(), "invalid receipt draft has a diagnostic");
  expect(diagnostic.size() <= ir::kMaximumDiagnosticBytes,
         "receipt draft diagnostic is bounded");
}

void expectInvalidReceipt(const ir::IrSubmissionReceipt &receipt,
                          std::string_view message) {
  std::string diagnostic;
  expect(!ir::validateIrSubmissionReceipt(receipt, diagnostic), message);
  expect(!diagnostic.empty(), "invalid receipt has a diagnostic");
  expect(diagnostic.size() <= ir::kMaximumDiagnosticBytes,
         "receipt diagnostic is bounded");
}

void testValidReceiptModels() {
  std::string diagnostic;
  auto draft = validDraft();
  expect(ir::validateIrSuccessfulReceiptDraft(draft, diagnostic),
         "normalized HTTPS receipt draft is valid");
  expect(diagnostic.empty(), "valid receipt draft clears its diagnostic");

  draft.serverOrigin = "http://localhost:3000";
  draft.remoteChartId = repeated('c', ir::kMaximumIrRemoteValueBytes);
  draft.remoteScoreId = repeated('s', ir::kMaximumIrRemoteValueBytes);
  draft.source = ir::IrReceiptConfirmationSource::Snapshot;
  draft.observedInSnapshot = true;
  expect(ir::validateIrSuccessfulReceiptDraft(draft, diagnostic),
         "explicit normalized HTTP origin and bounded remote IDs are valid");

  draft.remoteUserId.reset();
  draft.remoteChartId.clear();
  draft.remoteScoreId.clear();
  expect(ir::validateIrSuccessfulReceiptDraft(draft, diagnostic),
         "optional remote identity values may be absent");

  auto receipt = validReceipt();
  expect(ir::validateIrSubmissionReceipt(receipt, diagnostic),
         "canonical submission receipt is valid");
  receipt.chartMd5.clear();
  receipt.remoteUserId.reset();
  receipt.remoteChartId.clear();
  receipt.remoteScoreId.clear();
  receipt.source = ir::IrReceiptConfirmationSource::Snapshot;
  receipt.observedInSnapshot = true;
  expect(ir::validateIrSubmissionReceipt(receipt, diagnostic),
         "receipt permits optional MD5 and remote identity values");
}

void testInvalidReceiptDrafts() {
  auto draft = validDraft();
  draft.serverOrigin = "HTTPS://BOKU.TACHI.AC:443/";
  expectInvalidDraft(draft, "non-normalized receipt origin is rejected");

  draft = validDraft();
  draft.serverOrigin = "https://user:secret@example.test";
  expectInvalidDraft(draft, "credential-bearing receipt origin is rejected");

  draft = validDraft();
  draft.remoteUserId = 0;
  expectInvalidDraft(draft, "non-positive remote user ID is rejected");

  draft = validDraft();
  draft.remoteChartId = "chart\nsecret";
  expectInvalidDraft(draft, "control characters in remote chart ID are rejected");

  draft = validDraft();
  draft.remoteScoreId = "score\x7fsecret";
  expectInvalidDraft(draft, "control characters in remote score ID are rejected");

  draft = validDraft();
  draft.remoteScoreId =
      repeated('s', ir::kMaximumIrRemoteValueBytes + 1);
  expectInvalidDraft(draft, "oversized remote score ID is rejected");

  draft = validDraft();
  draft.source = static_cast<ir::IrReceiptConfirmationSource>(99);
  expectInvalidDraft(draft, "unknown receipt confirmation source is rejected");

  draft = validDraft();
  draft.confirmedAtUnixMillis = 0;
  expectInvalidDraft(draft, "non-positive receipt confirmation time is rejected");
}

void testInvalidStoredReceipts() {
  auto receipt = validReceipt();
  receipt.id = 0;
  expectInvalidReceipt(receipt, "non-positive receipt row ID is rejected");

  receipt = validReceipt();
  receipt.providerId = "Tachi";
  expectInvalidReceipt(receipt, "non-canonical provider ID is rejected");

  receipt = validReceipt();
  receipt.replayId = 0;
  expectInvalidReceipt(receipt, "non-positive replay ID is rejected");

  receipt = validReceipt();
  receipt.attemptId = "123E4567-E89B-42D3-A456-426614174000";
  expectInvalidReceipt(receipt, "non-canonical attempt UUID is rejected");

  receipt = validReceipt();
  receipt.attemptId = "123e4567-e89b-12d3-a456-426614174000";
  expectInvalidReceipt(receipt, "non-v4 attempt UUID is rejected");

  receipt = validReceipt();
  receipt.chartMd5 = repeated('A', 32);
  expectInvalidReceipt(receipt, "non-lowercase chart MD5 is rejected");

  receipt = validReceipt();
  receipt.chartSha256 = repeated('g', 64);
  expectInvalidReceipt(receipt, "malformed chart SHA-256 is rejected");

  receipt = validReceipt();
  receipt.remoteChartId = repeated('c', ir::kMaximumIrRemoteValueBytes + 1);
  expectInvalidReceipt(receipt, "oversized remote chart ID is rejected");

  receipt = validReceipt();
  receipt.confirmedAtUnixMillis = -1;
  expectInvalidReceipt(receipt, "negative confirmation time is rejected");
}

} // namespace

int main() {
  testValidReceiptModels();
  testInvalidReceiptDrafts();
  testInvalidStoredReceipts();
  if (failures != 0) {
    std::cerr << failures << " receipt model assertion(s) failed\n";
    return 1;
  }
  std::cout << "IR receipt model tests passed\n";
  return 0;
}
