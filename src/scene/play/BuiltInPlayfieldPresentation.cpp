#include "BuiltInPlayfieldPresentation.h"

#include "BMSRenderer.h"

#include <memory>
#include <utility>

std::unique_ptr<BuiltInPlayfieldPresentation>
createBuiltInPlayfieldPresentation(
    BuiltInPlayfieldPresentationCreateInfo creation) {
  auto presentation = std::make_unique<BMSRenderer>(
      &creation.chart, creation.timingWindows, creation.visibleTimeGreenNumber,
      creation.renderHud, creation.playbackRate);
  presentation->setReplayData(creation.replayData);
  return presentation;
}
