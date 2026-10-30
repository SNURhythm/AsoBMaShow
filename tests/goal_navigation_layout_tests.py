"""Exercise shared goal columns and production card construction with real Yoga."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from support.fixture_compiler import FixtureCompiler


class GoalNavigationLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        helper = (root / "src/view/GoalListRow.h").read_text()
        helper = "\n".join(line for line in helper.splitlines()
                           if not line.startswith(("#include", "#pragma")))
        source = r'''
#include "goals/GoalNavigation.h"
#include <yoga/Yoga.h>
#include <cassert>
#include <cmath>
#include <functional>
#include <string>
#include <vector>
namespace rendering { bool compactMenuCanvasActive = false; }
namespace app_theme {
float panelRadius() { return 0; }
int panel() { return 0; } int controlHover() { return 1; }
int controlPressed() { return 2; } int hairlineSubtle() { return 0; }
int hairlineStrong() { return 1; } int accentBorder() { return 2; }
}
enum class Edge { All = YGEdgeAll, Bottom = YGEdgeBottom };
enum class FlexDirection { Column = YGFlexDirectionColumn };
// Only rendering and event boundaries are stubbed; layout uses actual Yoga.
struct View {
  YGNodeRef node = YGNodeNew();
  std::vector<View *> children;
  virtual ~View() {
    for (auto *child : children) delete child;
    YGNodeFree(node);
  }
  View *setWidthPercent(float value) { YGNodeStyleSetWidthPercent(node, value); return this; }
  View *setMinWidth(float value) { YGNodeStyleSetMinWidth(node, value); return this; }
  View *setMinHeight(float value) { YGNodeStyleSetMinHeight(node, value); return this; }
  View *setFlexShrink(float value) { YGNodeStyleSetFlexShrink(node, value); return this; }
  View *setFlexDirection(FlexDirection value) { YGNodeStyleSetFlexDirection(node, YGFlexDirection(value)); return this; }
  View *setPadding(Edge edge, float value) { YGNodeStyleSetPadding(node, YGEdge(edge), value); return this; }
  View *setMargin(Edge edge, float value) { YGNodeStyleSetMargin(node, YGEdge(edge), value); return this; }
  View *setGap(float value) { YGNodeStyleSetGap(node, YGGutterAll, value); return this; }
  void setCornerRadius(float) {}
  void setName(const std::string &) {}
  void addView(View *child) {
    YGNodeInsertChild(node, child->node, children.size());
    children.push_back(child);
  }
};
struct Button : View {
  std::function<void()> click;
  void setStyledBorderWidth(int) {}
  void setThemedBackgroundColors(int (*)(), int (*)(), int (*)()) {}
  void setThemedBorderColors(int (*)(), int (*)(), int (*)()) {}
  void setOnClickListener(std::function<void()> value) { click = std::move(value); }
};
PRODUCTION_HELPER
void columns() {
  using namespace goal_navigation;
  assert(!stackedNavigation(900, false));
  assert(stackedNavigation(899, false));
  assert(stackedNavigation(1920, true));
  const auto wide = columns(1920, true);
  assert(wide.navigationWidth == 280 && wide.bodyWidth == 960);
  assert(wide.leftPadding + wide.navigationWidth + columnGap == 480);
  assert(wide.rightPadding == 480);
  for (float width : {320.f, 585.f, 899.f, 960.f, 1266.f, 1920.f}) {
    const auto stacked = columns(width, true, true);
    assert(stacked.navigationWidth == 0);
    assert(stacked.bodyWidth == std::min(width, 960.f));
    assert(stacked.leftPadding == stacked.rightPadding);
    assert(stacked.leftPadding + stacked.bodyWidth + stacked.rightPadding == width);
    const auto detail = columns(width, false);
    assert(detail.bodyWidth == stacked.bodyWidth && detail.leftPadding == stacked.leftPadding);
  }
  assert(columns(-10, false, true).bodyWidth == 0);
}
YGSize longLabel(YGNodeConstRef, float width, YGMeasureMode mode, float, YGMeasureMode) {
  const float measuredWidth = mode == YGMeasureModeUndefined ? 1200 : std::min(width, 1200.f);
  return {measuredWidth, std::ceil(1200 / std::max(1.f, measuredWidth)) * 24};
}
void rows() {
  for (bool compact : {false, true}) {
    rendering::compactMenuCanvasActive = compact;
    View parent;
    YGNodeStyleSetWidth(parent.node, 960);
    auto *content = goal_ui::clickableRow(&parent, [] {});
    auto *card = parent.children.front();
    assert(card->children.size() == 1 && card->children.front() == content);
    YGNodeCalculateLayout(parent.node, YGUndefined, YGUndefined, YGDirectionLTR);
    assert(YGNodeLayoutGetHeight(card->node) == (compact ? 80 : 64));
    auto *target = new View();
    YGNodeSetMeasureFunc(target->node, longLabel);
    content->addView(target);
    auto *status = new View();
    YGNodeStyleSetHeight(status->node, 24);
    content->addView(status);
    auto *progress = new View();
    YGNodeStyleSetHeight(progress->node, 12);
    content->addView(progress);
    YGNodeCalculateLayout(parent.node, YGUndefined, YGUndefined, YGDirectionLTR);
    const float wideHeight = YGNodeLayoutGetHeight(card->node);
    for (float width : {320.f, 585.f, 960.f}) {
      YGNodeStyleSetWidth(parent.node, width);
      YGNodeCalculateLayout(parent.node, YGUndefined, YGUndefined, YGDirectionLTR);
      const float inset = compact ? 16 : 12;
      assert(YGNodeLayoutGetWidth(card->node) == width);
      assert(YGNodeLayoutGetWidth(content->node) == width - 2 * inset);
      assert(YGNodeLayoutGetHeight(card->node) >= (compact ? 80 : 64));
      assert(YGNodeLayoutGetHeight(card->node) >= wideHeight);
      assert(YGNodeLayoutGetHeight(card->node) == YGNodeLayoutGetHeight(content->node) + 2 * inset);
      assert(YGNodeLayoutGetTop(progress->node) + YGNodeLayoutGetHeight(progress->node)
             <= YGNodeLayoutGetHeight(content->node));
      if (width == 320) assert(YGNodeLayoutGetHeight(card->node) > wideHeight);
    }
  }
}
int main(int argc, char **argv) {
  assert(argc == 2);
  if (std::string(argv[1]) == "columns") columns(); else rows();
}
'''.replace("PRODUCTION_HELPER", helper)
        cls.temp = tempfile.TemporaryDirectory(prefix="asobmashow-goal-layout-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        path = directory / "layout.cpp"
        compiler = FixtureCompiler.from_environment()
        cls.binary = directory / ("layout" + compiler.executable_suffix)
        path.write_text(source)
        compiler.build([path], cls.binary, directory,
                       includes=[root / "src", root / "yoga"],
                       objects=[os.environ.get("ASOBMASHOW_TEST_YOGA_LIBRARY",
                                  str(root / "cmake-build-debug/yoga/yoga/libyogacore.a"))])

    def test_desktop_centering_and_compact_full_width(self):
        subprocess.run([str(self.binary), "columns"], check=True)

    def test_long_cards_grow_and_relayout_at_narrow_widths(self):
        subprocess.run([str(self.binary), "rows"], check=True)


if __name__ == "__main__":
    unittest.main()
