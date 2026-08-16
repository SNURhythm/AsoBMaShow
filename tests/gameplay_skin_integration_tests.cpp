#include "skin/GameplaySkinActivationRequest.h"

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

void activationRequestBoundaryIsDefaultEmpty() {
  skin::AcquireGameplaySkinForNextChart acquire;
  expect(!acquire, "the injected next-chart acquisition callback defaults empty");
  const skin::GameplaySkinAcquisition noSelection;
  expect(noSelection.disposition ==
                 skin::GameplaySkinAcquisitionDisposition::BuiltIn &&
             !noSelection.request && !noSelection.failure,
         "an unselected trait is the only acquisition state that selects "
         "built-in gameplay");

  const skin::GameplaySkinAcquisition selectedFailure{
      .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
      .failure = skin::GameplaySkinAcquisitionFailure{
          .diagnostic = {.code = "skin.test.selected_failure",
                         .message = "selected skin failed",
                         .severity = skin::DiagnosticSeverity::Error}}};
  expect(selectedFailure.disposition ==
                 skin::GameplaySkinAcquisitionDisposition::Failed &&
             !selectedFailure.request && selectedFailure.failure,
         "a selected skin failure cannot be represented as built-in gameplay");
}

} // namespace

int main() {
  activationRequestBoundaryIsDefaultEmpty();
  if (failures == 0) {
    std::cout << "gameplay skin integration tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
