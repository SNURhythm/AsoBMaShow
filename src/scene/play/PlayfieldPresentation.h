#pragma once

#include "PlayfieldPresentationEvents.h"
#include "PlayfieldVisualState.h"

class PlayfieldPresentation : public IPlayfieldPresentationEvents {
public:
  ~PlayfieldPresentation() override = default;

  virtual void configure(const PlayfieldPresentationConfig &configuration) = 0;
  virtual void reset() = 0;
  virtual void refreshGeometry() = 0;
};
