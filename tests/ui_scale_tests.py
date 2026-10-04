"""Exercise the production drawable-to-UI transform without a graphics device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract


class UiScaleTests(unittest.TestCase):
    def test_result_rotation_resizes_custom_controls_and_safe_area(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/scene/ResultScene.cpp").read_text(),
                         "void ResultScene::resizeResultLayout()")
        source = r'''
#include <array>
#include <cassert>
#include <initializer_list>
constexpr int YGUndefined = -1;
enum class Edge { Top, Right, Bottom, Left };
struct View {
  struct LayoutBatchScope {};
  int width = 0, height = 0, minimum = 0;
  std::array<int, 4> margins{};
  void setSize(int w, int h) { width=w; height=h; }
  void setWidth(int w) { width=w; }
  void setHeight(int h) { height=h; }
  void setMinHeight(int h) { minimum=h; }
  View* setMargin(Edge edge, int n) { margins[int(edge)]=n; return this; }
};
struct Button : View {};
struct ScrollView : View { void refreshContentLayout() {} };
struct DefaultSkin { static void resizeResultLayout(View*, int, int) {} };
namespace rendering {
int window_width=1920, window_height=1080;
struct Insets { int top=0, right=0, bottom=0, left=0; } safe;
Insets uiSafeAreaInsets() { return safe; }
}
struct ResultScene {
  View viewport, root, controls, confirmation;
  Button restore;
  View *viewportLayout=&viewport, *rootLayout=&root;
  View *resultTouchControlsOverlay=&controls;
  Button *resultTouchControlsRestore=&restore;
  View *courseExitConfirmation=&confirmation;
  ScrollView *resultScroll=nullptr;
  std::array<int, 6> resultLayoutSignature{};
  void resizeResultLayout();
};
PRODUCTION_METHOD
int main() {
  ResultScene scene;
  scene.resizeResultLayout();
  rendering::window_width=1080;
  rendering::window_height=2340;
  rendering::safe={140, 0, 90, 0};
  scene.resizeResultLayout();
  for (View* view : std::array<View*, 5>{&scene.viewport, &scene.root, &scene.controls,
                     &scene.restore, &scene.confirmation}) {
    assert(view->width==1080 && view->height==2340);
  }
  ScrollView scroll;
  scene.resultScroll=&scroll;
  scene.resultLayoutSignature={};
  scene.resizeResultLayout();
  assert(scene.root.width==1080 && scene.root.minimum==2110);
  assert(scroll.margins[0]==140 && scroll.margins[2]==90);
  rendering::safe={160, 0, 100, 0};
  scene.resizeResultLayout();
  assert(scene.root.minimum==2080 && scroll.margins[0]==160);
  rendering::window_width=2340;
  rendering::window_height=1080;
  rendering::safe={0, 140, 60, 140};
  scene.resizeResultLayout();
  assert(scene.root.width==2060 && scene.root.minimum==1020);
  assert(scene.controls.width==2340 && scene.controls.height==1080);
}
'''.replace("PRODUCTION_METHOD", method)
        with tempfile.TemporaryDirectory(prefix="asobmashow-result-rotation-") as temp:
            path = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            path.write_text(source)
            subprocess.run(["c++", "-std=c++20", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_portrait_camera_keeps_all_eight_lanes_in_view(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/main.cpp").read_text(),
                         "void resetViewTransform(uint16_t bgaWidth, uint16_t bgaHeight,")
        source = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
namespace bx {
struct Vec3 { float x, y, z; };
float toRad(float v) { return v * 3.14159265359f / 180; }
template<class... T> void mtxOrtho(T...) {}
}
namespace bgfx {
using ViewId = uint16_t;
struct Caps { bool homogeneousDepth = true; };
Caps* getCaps() { static Caps caps; return &caps; }
template<class... T> void setViewTransform(T...) {}
template<class... T> void setViewRect(T...) {}
}
struct Camera {
  bx::Vec3 eye{}, at{};
  float aspect = 0;
  Camera& edit() { return *this; }
  Camera& setPosition(bx::Vec3 p) { eye = p; return *this; }
  Camera& setLookAt(bx::Vec3 p) { at = p; return *this; }
  Camera& setAspectRatio(float a) { aspect = a; return *this; }
  template<class... T> Camera& setViewRect(T...) { return *this; }
  Camera& commit() { return *this; }
  void render() {}
};
namespace rendering {
int window_width = 1920, window_height = 1080, render_width = 1920, render_height = 1080;
int ui_offset_x = 0, ui_offset_y = 0, ui_view_width = 1920, ui_view_height = 1080;
constexpr int ui_view = 0, bga_view = 1, bga_layer_view = 2, clear_view = 3;
Camera game_camera;
Camera* main_camera = &game_camera;
}
namespace gameplay_geometry { constexpr float kPlayAreaCenterX = 4; }
struct AppSettings { float laneLength = 8, laneAngleDegrees = 13.4; };
PRODUCTION_METHOD
int main() {
  using namespace rendering;
  AppSettings settings;
  resetViewTransform(1920,1080,4,5,6,settings);
  assert(std::abs(game_camera.eye.z + 2.1f) < .0001f);
  for (int height : {1440, 1920, 2340}) {
    window_width = 1080;
    window_height = height;
    resetViewTransform(1080,height,4,5,6,settings);
    const float angle = bx::toRad(settings.laneAngleDegrees);
    const float depthAtJudge = -game_camera.eye.y * std::sin(angle)
                              - game_camera.eye.z * std::cos(angle);
    const float visibleWidth = 2 * depthAtJudge * std::tan(bx::toRad(60))
                               * game_camera.aspect;
    assert(visibleWidth >= 8 && "portrait camera must show scratch and all seven keys");
  }
}
'''.replace("PRODUCTION_METHOD", method)
        with tempfile.TemporaryDirectory(prefix="asobmashow-portrait-camera-") as temp:
            path = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            path.write_text(source)
            subprocess.run(["c++", "-std=c++23", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_portrait_and_landscape_keep_readable_units_and_valid_coordinates(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/main.cpp").read_text(),
                         "void rendering::updateUIScale(int renderW, int renderH)")
        source = r'''
#include <cassert>
#include <cmath>
namespace rendering {
constexpr int design_width = 1920, design_height = 1080;
int render_width = 1, render_height = 1;
int window_width = 1920, window_height = 1080;
int ui_view_width = 1920, ui_view_height = 1080;
int ui_offset_x = 0, ui_offset_y = 0;
float ui_scale_x = 1, ui_scale_y = 1;
void updateUIScale(int, int);
}
PRODUCTION_METHOD
int main() {
  using namespace rendering;
  updateUIScale(1920, 1080);
  assert(window_width == 1920 && window_height == 1080);
  updateUIScale(1080, 1920);
  assert(window_width == 1080 && window_height == 1920 &&
         "portrait must not shrink a 1920-unit desktop across a phone");
  assert(ui_scale_x == 1 && ui_scale_y == 1);
  updateUIScale(1170, 2532);
  assert(window_width == 1080 && window_height == 2337);
  assert(std::abs(585 / ui_scale_x - 540) < .01f);
  assert(std::abs(1266 / ui_scale_y - 1168.6154f) < .01f);
  updateUIScale(2532, 1170);
  assert(window_width == 1920 && window_height == 887);
  updateUIScale(0, 0);
  assert(render_width == 2532 && render_height == 1170 &&
         "transient zero-sized drawables must preserve the last valid transform");
  assert(std::isfinite(ui_scale_x) && ui_scale_x > 0);
}
'''.replace("PRODUCTION_METHOD", method)
        with tempfile.TemporaryDirectory(prefix="asobmashow-ui-scale-") as temp:
            source_path = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            source_path.write_text(source)
            subprocess.run(["c++", "-std=c++23", str(source_path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
