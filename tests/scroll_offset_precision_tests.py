import re
import unittest

from tests import music_select_error_flow_contract_tests as fixture_tools


class ScrollOffsetPrecisionTests(unittest.TestCase):
    def test_small_steps_survive_large_offsets_and_stop_at_boundaries(self):
        root = fixture_tools.ROOT
        for name in ("ScrollView", "RecyclerView"):
            with self.subTest(view=name):
                header = (root / f"src/view/{name}.h").read_text()
                source = ((root / "src/view/ScrollView.cpp").read_text()
                          if name == "ScrollView" else header)
                field = re.search(r"^  (?:float|double) scrollOffset[^;]*;", header, re.M).group(0)
                prefix = "ScrollView::" if name == "ScrollView" else ""
                methods = "\n".join(
                    signature + fixture_tools.function_body(source, signature)
                    for signature in (
                        f"{'inline ' if not prefix else ''}void {prefix}clampScrollOffset()",
                        f"{'inline ' if not prefix else ''}bool {prefix}scrollBy(float delta)"))
                methods = methods.replace("ScrollView::", "")
                fixture = r'''
#include <algorithm>
#include <cmath>
#include <stdexcept>
struct Content { int getHeight() const { return 5000000; } };
struct Subject {
  FIELD
  Content content;
  Content *contentView = &content;
  int itemHeight = 100;
  int itemCount() const { return 50000; }
  int getContentHeight() const { return 200; }
  int getScrollContentHeight() const { return 200; }
  void updateContentPosition() {}
  void revealScrollbar() {}
  void updateVisibleItems() {}
  METHODS
};
int main() {
  Subject view;
  view.scrollOffset = 4194304;
  for (int frame = 0; frame < 1000; ++frame) {
    if (!view.scrollBy(0.003F))
      throw std::runtime_error("a fractional step was mistaken for an edge");
  }
  if (std::fabs(view.scrollOffset - 4194307.0) > 0.01)
    throw std::runtime_error("fractional scroll distance was lost");
  view.scrollOffset = 4999800;
  if (view.scrollBy(1.0F))
    throw std::runtime_error("outward momentum must stop at the bottom edge");
  view.scrollOffset = 0;
  if (view.scrollBy(-1.0F))
    throw std::runtime_error("outward momentum must stop at the top edge");
}
'''
                fixture_tools.MusicSelectSceneBehaviorTests().compile_and_run(
                    fixture.replace("FIELD", field).replace("METHODS", methods))


if __name__ == "__main__":
    unittest.main()
