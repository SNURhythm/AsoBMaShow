#pragma once

struct ResultSkinFailurePresentation {
  bool showNotice = false;
  bool restoreTouchControls = false;
};

[[nodiscard]] constexpr ResultSkinFailurePresentation
makeResultSkinFailurePresentation(bool renderFailed) {
  return {.showNotice = renderFailed, .restoreTouchControls = renderFailed};
}
