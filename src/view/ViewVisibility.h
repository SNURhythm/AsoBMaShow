#pragma once

#include "View.h"

namespace view_visibility {
// For conditional rows: hide rendering/input and remove layout space together.
// Use setVisible alone only when the hidden view should retain its layout space.
inline void setLayoutVisible(View *view, bool visible) {
  view->setVisible(visible);
  view->setDisplay(visible ? YGDisplayFlex : YGDisplayNone);
}
} // namespace view_visibility
