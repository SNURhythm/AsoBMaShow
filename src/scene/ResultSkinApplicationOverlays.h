#pragma once

struct ResultSkinApplicationOverlayState {
  bool selectedSkin = false;
  bool hasPersistenceResult = false;
  bool courseStage = false;
  bool savedResultBrowsing = false;
};

struct ResultSkinApplicationOverlays {
  bool showsPersistenceRecovery = false;
  bool buildsCourseExitConfirmation = false;
};

[[nodiscard]] constexpr ResultSkinApplicationOverlays
makeResultSkinApplicationOverlays(const ResultSkinApplicationOverlayState &state) {
  return {.showsPersistenceRecovery = state.hasPersistenceResult,
          .buildsCourseExitConfirmation =
              state.courseStage && !state.savedResultBrowsing};
}
