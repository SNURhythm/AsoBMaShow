#include "CanonicalDigest.h"
#include "DurablePayloadLimits.h"
#include "replay/ReplayFormat.h"
#include "replay/ReplayLimits.h"

#include <iostream>
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

void testCanonicalDigestAuthorityIsShared() {
  const std::string lower(64, 'a');
  const std::string upper(64, 'A');
  expect(canonical_digest::isCanonicalLowerHex(lower, 64) &&
             replay::isCanonicalLowerHex(lower, 64),
         "root and replay APIs accept the same canonical digest");
  expect(!canonical_digest::isCanonicalLowerHex(upper, 64) &&
             !replay::isCanonicalLowerHex(upper, 64),
         "root and replay APIs reject uppercase through one implementation");
}

void testDurableLimitsArePinnedAndConsumedByReplay() {
  expect(durable_payload::kMaximumStringBytes == 16U * 1024U,
         "durable string bound is 16 KiB");
  expect(durable_payload::kMaximumResultGaugeSamples == 1'000'000U,
         "result gauge-history bound is one million samples");
  expect(durable_payload::kMaximumCourseStages == 256U,
         "course stage bound is shared across durable domains");
  expect(durable_payload::kMaximumIrSnapshotBytes == 16U * 1024U * 1024U,
         "postponed IR snapshot bound is 16 MiB");
  expect(replay::kReplayLimits.maxStringBytes ==
                 durable_payload::kMaximumStringBytes &&
             replay::kReplayLimits.maxCourseStages ==
                 durable_payload::kMaximumCourseStages,
         "replay limits consume the root durable authorities");
}

void testInclusiveCollectionAndStringBounds() {
  expect(durable_payload::withinLimit(
             durable_payload::kMaximumResultGaugeSamples,
             durable_payload::kMaximumResultGaugeSamples) &&
             !durable_payload::withinLimit(
                 durable_payload::kMaximumResultGaugeSamples + 1,
                 durable_payload::kMaximumResultGaugeSamples),
         "durable collection bounds are inclusive");
  expect(durable_payload::validString(std::string_view{}, true) &&
             !durable_payload::validString(std::string_view{}, false) &&
             durable_payload::validString(
                 std::string(durable_payload::kMaximumStringBytes, 'x'),
                 false) &&
             !durable_payload::validString(
                 std::string(durable_payload::kMaximumStringBytes + 1, 'x'),
                 false),
         "durable strings share empty and byte-bound handling");
}

} // namespace

int main() {
  testCanonicalDigestAuthorityIsShared();
  testDurableLimitsArePinnedAndConsumedByReplay();
  testInclusiveCollectionAndStringBounds();
  if (failures != 0) {
    std::cerr << failures << " durable payload contract test(s) failed\n";
    return 1;
  }
  std::cout << "durable payload contract tests passed\n";
  return 0;
}
