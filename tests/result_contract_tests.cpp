#include "ResultContracts.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

void testSharedSetupDomains() {
  for (const int keyMode : {5, 7, 9, 10, 14, 24, 48}) {
    expect(result_contract::isSupportedKeyMode(keyMode),
           "every supported play key mode is accepted");
  }
  for (const int keyMode : {-1, 0, 1, 6, 8, 13, 49}) {
    expect(!result_contract::isSupportedKeyMode(keyMode),
           "unknown key modes are rejected by the shared authority");
  }
  expect(
      result_contract::isKnownGaugeType(GaugeType::Hazard) &&
          result_contract::isKnownGaugeProfile(GaugeProfile::Standard24Keys) &&
          result_contract::isKnownGaugeAutoShift(
              GaugeAutoShiftMode::BestClear) &&
          result_contract::isKnownClearRank(kClearTypeFullComboRank),
      "known setup and result enum endpoints are accepted");
  expect(!result_contract::isKnownGaugeType(static_cast<GaugeType>(99)) &&
             !result_contract::isKnownGaugeProfile(
                 static_cast<GaugeProfile>(99)) &&
             !result_contract::isKnownGaugeAutoShift(
                 static_cast<GaugeAutoShiftMode>(99)) &&
             !result_contract::isKnownClearRank(301),
         "unknown setup and result enum values fail closed");
}

void testSharedChartIdentityAgreement() {
  const result_contract::ChartIdentity expected{
      .md5 = repeated('b', 32), .sha256 = repeated('a', 64), .keyMode = 7};
  auto selected = expected;
  expect(result_contract::canonicalChartIdentity(expected, true) &&
             result_contract::compareChartIdentity(expected, selected) ==
                 result_contract::ChartIdentityMatch::Match,
         "canonical chart identity agrees exactly");
  selected.md5 = repeated('c', 32);
  expect(result_contract::compareChartIdentity(expected, selected) ==
             result_contract::ChartIdentityMatch::Md5Mismatch,
         "optional MD5 agreement is enforced when recorded");
  auto shaOnly = expected;
  shaOnly.md5.clear();
  expect(result_contract::canonicalChartIdentity(shaOnly, false) &&
             result_contract::compareChartIdentity(shaOnly, selected) ==
                 result_contract::ChartIdentityMatch::Match,
         "an explicitly optional recorded MD5 does not invent agreement");
  selected = expected;
  selected.keyMode = 14;
  expect(result_contract::compareChartIdentity(expected, selected) ==
             result_contract::ChartIdentityMatch::KeyModeMismatch,
         "key mode participates in identity agreement");
}

void testSharedResultArithmetic() {
  const std::vector<float> history{20.0F, 80.0F};
  result_contract::ResultOutcomeFacts facts{
      .score = 7,
      .maxScore = 10,
      .maxCombo = 4,
      .comboBreak = 1,
      .pGreat = 3,
      .great = 1,
      .good = 1,
      .finalGauge = 80.0F,
      .clearType = kClearTypeNormalClearRank,
      .gaugeHistory = history,
  };
  expect(result_contract::validResultOutcome(facts, 5),
         "valid compact result arithmetic is accepted");
  facts.pGreat = 4;
  expect(!result_contract::validResultOutcome(facts, 5),
         "score and judgement disagreement is rejected centrally");
  facts.pGreat = 3;
  facts.maxCombo = 6;
  expect(!result_contract::validResultOutcome(facts, 5),
         "the caller-supplied combo boundary is enforced centrally");
}

} // namespace

int main() {
  testSharedSetupDomains();
  testSharedChartIdentityAgreement();
  testSharedResultArithmetic();
  if (failures != 0) {
    std::cerr << failures << " result contract test(s) failed\n";
    return 1;
  }
  std::cout << "result contract tests passed\n";
  return 0;
}
