#include "replay/ChartReplayPersistence.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testContractSurfaceNamesEveryRecoverableBoundary() {
  using replay::ChartReplayPersistenceState;
  expect(ChartReplayPersistenceState::SavedWithReplay !=
             ChartReplayPersistenceState::SavedWithoutReplay,
         "replay attachment is reported independently from result durability");
  expect(ChartReplayPersistenceState::PendingScore !=
             ChartReplayPersistenceState::PendingAcknowledgement,
         "score projection and acknowledgement remain distinct recovery states");
  expect(ChartReplayPersistenceState::Retryable !=
             ChartReplayPersistenceState::IntegrityConflict,
         "retryable storage failures are distinct from identity conflicts");
}

} // namespace

int main() {
  testContractSurfaceNamesEveryRecoverableBoundary();
  if (failures != 0) {
    std::cerr << failures << " chart replay persistence test(s) failed\n";
    return 1;
  }
  std::cout << "chart replay persistence tests passed\n";
  return 0;
}
