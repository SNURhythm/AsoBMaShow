#include "scene/ChartViewerNoteGeometry.h"

#include <cmath>
#include <cstddef>
#include <iostream>

namespace {

bool near(float actual, float expected) {
  return std::fabs(actual - expected) < 0.0001F;
}

} // namespace

int main() {
  using namespace chart_viewer_note_geometry;
  int failures = 0;
  const auto check = [&](bool value, const char *label) {
    if (!value) {
      std::cerr << "FAIL: " << label << '\n';
      ++failures;
    }
  };

  const auto normal =
      invisibleNoteRectangles(10.0F, 20.0F, 8.0F, 6.0F, 0.72F, false);
  check(normal.count == 4,
        "a normal invisible note produces four border rectangles");
  bool centerCovered = false;
  for (std::size_t i = 0; i < normal.count; ++i) {
    const auto &rectangle = normal.rectangles[i];
    centerCovered =
        centerCovered ||
        (14.0F > rectangle.x &&
         14.0F < rectangle.x + rectangle.width &&
         23.0F > rectangle.y &&
         23.0F < rectangle.y + rectangle.height);
  }
  check(!centerCovered, "a normal invisible note leaves its center empty");

  const auto longNote =
      invisibleNoteRectangles(10.0F, 20.0F, 8.0F, 6.0F, 0.72F, true);
  check(longNote.count == 1,
        "an invisible long note produces one solid rectangle");
  check(near(longNote.rectangles[0].x, 10.0F) &&
            near(longNote.rectangles[0].y, 20.0F) &&
            near(longNote.rectangles[0].width, 8.0F) &&
            near(longNote.rectangles[0].height, 6.0F),
        "the invisible long-note rectangle covers the full marker");

  const auto narrow =
      invisibleNoteRectangles(0.0F, 0.0F, 0.5F, 6.0F, 0.72F, false);
  check(narrow.count == 4,
        "a narrow normal invisible note retains four border rectangles");
  check(near(narrow.rectangles[0].height, 0.25F) &&
            near(narrow.rectangles[2].width, 0.25F),
        "outline thickness is clamped to half the marker width");

  return failures == 0 ? 0 : 1;
}
