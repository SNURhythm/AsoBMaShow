#pragma once

#include "../SkinProfileSettings.h"
#include "BeatorajaSkinModel.h"

namespace skin {

struct AuthoredSize {
  double width = 0.0;
  double height = 0.0;
};

struct AuthoredPoint {
  double x = 0.0;
  double y = 0.0;
};

using AuthoredRect = SkinAuthoredRect;

struct UiLogicalRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

struct Affine2D {
  double m00 = 1.0;
  double m01 = 0.0;
  double tx = 0.0;
  double m10 = 0.0;
  double m11 = 1.0;
  double ty = 0.0;
};

struct PlaySkinViewport {
  Affine2D authoredToUi;
  Affine2D uiToAuthored;
  AuthoredRect drawableAuthoredBounds;
  UiLogicalRect safeUiBounds;
  bool valid = false;
};

PlaySkinViewport evaluatePlaySkinViewport(AuthoredSize authoredSize,
                                          UiLogicalRect safeUiBounds,
                                          const ViewportSettings &settings);

} // namespace skin
