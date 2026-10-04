"""Exercise the production drawable-to-UI transform without a graphics device."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract


class UiScaleTests(unittest.TestCase):
    def test_main_menu_layout_uses_real_yoga_for_rotation(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/scene/MainMenuScene.cpp").read_text(),
                         "void MainMenuScene::updatePanelLayout()") + "\n" + extract(
                             (root / "src/scene/MainMenuScene.cpp").read_text(),
                             "void MainMenuScene::updateMenuPresentation(bool portrait)")
        source = r'''
#include <yoga/Yoga.h>
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>
enum class FlexDirection { Column=YGFlexDirectionColumn, Row=YGFlexDirectionRow };
enum class Edge { Left=YGEdgeLeft, Right=YGEdgeRight, Top=YGEdgeTop, Bottom=YGEdgeBottom, All=YGEdgeAll };
struct View {
  struct LayoutBatchScope {};
  YGNodeRef node=YGNodeNew();
  std::string name;
  std::vector<View*> children;
  View* parent=nullptr;
  bool visible=true;
  explicit View(std::string n): name(n) {}
  View* setWidth(float v) { YGNodeStyleSetWidth(node,v);return this; }
  View* setHeight(float v) { YGNodeStyleSetHeight(node,v);return this; }
  View* setWidthPercent(float v) { YGNodeStyleSetWidthPercent(node,v);return this; }
  View* setMinWidth(float v) { YGNodeStyleSetMinWidth(node,v);return this; }
  View* setMinHeight(float v) { YGNodeStyleSetMinHeight(node,v);return this; }
  View* setFlex(float v) { YGNodeStyleSetFlex(node,v);return this; }
  View* setFlexShrink(float v) { YGNodeStyleSetFlexShrink(node,v);return this; }
  View* setFlexDirection(FlexDirection v) { YGNodeStyleSetFlexDirection(node,YGFlexDirection(v));return this; }
  View* setGap(float v) { YGNodeStyleSetGap(node,YGGutterAll,v);return this; }
  View* setPadding(Edge e,float v) { YGNodeStyleSetPadding(node,YGEdge(e),v);return this; }
  View* setAlignItems(YGAlign v) { YGNodeStyleSetAlignItems(node,v);return this; }
  void setAutoFitText(bool) {}
  void setVisible(bool v) { visible=v; }
  bool getVisible() { return visible; }
  void setDisplay(YGDisplay v) { YGNodeStyleSetDisplay(node,v); }
  bool moveTo(View& target) { if(parent==&target)return true; if(parent) { YGNodeRemoveChild(parent->node,node); std::erase(parent->children,this); } target.add(*this); return true; }
  void add(View& v) { v.parent=this; children.push_back(&v);YGNodeInsertChild(node,v.node,children.size()-1); }
  auto& getChildren() {return children;}
  View* findViewByName(const std::string &n) { if(name==n)return this; for(auto* c:children)if(auto* found=c->findViewByName(n))return found;return nullptr; }
};
struct Button:View { View content{"content"}; using View::View; View* getContentView() { return &content; } };
struct ScrollView:View { using View::View; void refreshContentLayout() {} };
struct ChartDetails:View { View best{"best"}; ChartDetails():View("chart") { add(best); } void setScoreContainer(View* v) { best.moveTo(v?*v:*this); } };
namespace rendering { int window_width=1080,window_height=1920; }
struct SafeAreaInsets { int top=30,right=0,bottom=20,left=0; };
SafeAreaInsets getSafeAreaInsetsUi() { return {}; }
constexpr int kRootPadding=28,kLibraryPanelWidth=320,kDetailsPanelWidth=500,kDetailsContentWidth=460;
constexpr int kPortraitMenuActionHeight=64,kMenuActionHeight=84;
struct MainMenuScene {
  View* rootLayout;
  View *detailsContent_, *detailsControlsContent_;
  ScrollView *detailsControlsScroll_, *tutorialRightScroll_=nullptr;
  ChartDetails* chartDetailsView_;
  Button *readyPlayOptionsButton;
  View *chartActionsRow,*unzipButtonSlot,*findBmsButtonSlot,*replayStatusText,*replayButtonSlot;
  Button *replayButton,*rankingsButton,*startButton,*unzipButton,*findBmsButton;
  View *searchBox,*chartFilterButton,*chartSortButton,*replayButtonText,*rankingsButtonText;
  void updatePanelLayout(); void updateMenuPresentation(bool portrait);
};
PRODUCTION_METHOD
int main() {
  View root("root"),browser("mainMenuBrowser"),library("mainMenuLibrary"),songs("mainMenuSongs"),details("mainMenuDetails"),actions("mainMenuLibraryActions"),button("button"),primary("mainMenuPrimaryActions"),controls("mainMenuControls"),list("list"),records("mainMenuRecordActions"),toolbar("mainMenuToolbar"),title("mainMenuTitle"),content("content"),controlsContent("controlsContent"),tools("tools"),unzipSlot("unzip"),findSlot("find"),status("status"),replaySlot("replay"),search("search"),filter("filter"),sort("sort"),replayText("replayText"),rankingText("rankingText");
  ScrollView scroll("mainMenuDetailsScroll"),controlsScroll("controlsScroll");
  ChartDetails chart;
  Button settings("mainMenuSettings"),options("options"),replay("replay"),ranking("ranking"),start("start"),unzip("unzip"),find("find");
  root.setPadding(Edge::All,28)->setGap(24)->setAlignItems(YGAlignStretch);
  browser.setFlexDirection(FlexDirection::Row)->setGap(24)->setMinWidth(0)->setMinHeight(0);
  root.add(browser);root.add(details);browser.add(library);browser.add(songs);
  library.add(actions);actions.add(button);library.add(list);list.setFlex(1);
  library.setPadding(Edge::All,14);button.setWidth(292)->setHeight(84);
  songs.setFlex(1)->setMinWidth(0)->setMinHeight(0)->setPadding(Edge::All,16);
  songs.add(toolbar);toolbar.setFlexDirection(FlexDirection::Row)->setGap(12);
  toolbar.add(title);title.setMinWidth(280)->setFlex(1);
  std::array<View,4> headerButtons={View("add"),View("refresh"),View("search"),View("tasks")};
  int widths[]={112,122,154,142};
  for(int i=0;i<4;++i){toolbar.add(headerButtons[i]);headerButtons[i].setWidth(widths[i]);}
  details.setGap(12)->setPadding(Edge::Bottom,16);details.add(scroll);details.add(primary);details.add(controls);
  controls.setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(8);
  controls.setFlex(1)->setMinWidth(0)->setMinHeight(0);controls.add(controlsScroll);
  controlsScroll.setWidthPercent(100)->setFlex(1)->setMinHeight(0);
  primary.setFlexShrink(0);scroll.setFlex(1)->setMinWidth(0)->setMinHeight(0);
  primary.add(start);primary.add(records);primary.add(settings);
  records.setFlexDirection(FlexDirection::Row);records.add(replaySlot);records.add(ranking);
  replaySlot.setFlex(1)->setMinWidth(0);ranking.setFlex(1)->setMinWidth(0);
  replaySlot.add(replay);replay.setWidthPercent(100);
  content.add(chart);content.add(options);content.add(tools);content.add(unzipSlot);content.add(findSlot);content.add(status);
  unzipSlot.add(unzip);findSlot.add(find);unzipSlot.setVisible(false);findSlot.setVisible(false);
  MainMenuScene scene{&root,&content,&controlsContent,&controlsScroll,nullptr,&chart,&options,&tools,&unzipSlot,&findSlot,&status,&replaySlot,&replay,&ranking,&start,&unzip,&find,&search,&filter,&sort,&replayText,&rankingText};
  for (auto dimensions : {std::pair{1080,1920},std::pair{1080,1440},std::pair{1080,1100},std::pair{1920,1080},std::pair{1080,1920}}) {
    rendering::window_width=dimensions.first;rendering::window_height=dimensions.second;
    root.setWidth(dimensions.first)->setHeight(dimensions.second);
    root.setPadding(Edge::Top,58)->setPadding(Edge::Bottom,48);
    scene.updatePanelLayout();YGNodeCalculateLayout(root.node,dimensions.first,dimensions.second,YGDirectionLTR);
    float bh=YGNodeLayoutGetHeight(browser.node),dh=YGNodeLayoutGetHeight(details.node);
    if(dimensions.second>dimensions.first) {
      const float usable=dimensions.second-106-24;
      assert(std::abs(bh/usable-.6)<.01 && std::abs(dh/usable-.4)<.01);
      assert(YGNodeLayoutGetLeft(songs.node)>YGNodeLayoutGetLeft(library.node));
      assert(std::abs(YGNodeLayoutGetHeight(library.node)-bh)<1);
      assert(std::abs(YGNodeLayoutGetWidth(library.node)/(YGNodeLayoutGetWidth(browser.node)-24)-.3)<.02);
      assert(YGNodeLayoutGetTop(details.node)>=YGNodeLayoutGetTop(browser.node)+bh);
      assert(!title.visible && chart.best.parent==&controlsContent && options.parent==&controlsContent && tools.parent==&controlsContent);
      assert(primary.parent==&controls && settings.parent==&records);
      assert(YGNodeLayoutGetLeft(headerButtons.back().node)+YGNodeLayoutGetWidth(headerButtons.back().node)<=YGNodeLayoutGetWidth(toolbar.node)+1);
      assert(YGNodeLayoutGetHeight(scroll.node)>0 && YGNodeLayoutGetHeight(primary.node)==136);
      assert(YGNodeLayoutGetLeft(controls.node)>YGNodeLayoutGetLeft(scroll.node));
      assert(std::abs(YGNodeLayoutGetWidth(controls.node)-YGNodeLayoutGetWidth(scroll.node))<1);
      assert(YGNodeLayoutGetWidth(button.node)<=YGNodeLayoutGetWidth(library.node)-28+1);
    } else {
      assert(YGNodeLayoutGetWidth(library.node)==320 && YGNodeLayoutGetWidth(details.node)==500);
      assert(std::abs(bh-dh)<1);
      assert(title.visible && !controls.visible && chart.best.parent==&chart && options.parent==&content && tools.parent==&content);
      assert(primary.parent==&details && settings.parent==&primary);
      assert(YGNodeLayoutGetHeight(primary.node)==280 && YGNodeLayoutGetHeight(button.node)==84);
    }
    if(dimensions.second<=dimensions.first)continue;
    assert(YGNodeLayoutGetHeight(controlsScroll.node)>0);
    assert(YGNodeLayoutGetTop(primary.node)>=YGNodeLayoutGetHeight(controlsScroll.node));
    assert(YGNodeLayoutGetTop(primary.node)+YGNodeLayoutGetHeight(primary.node)<=YGNodeLayoutGetHeight(controls.node)+1);
    assert(YGNodeLayoutGetLeft(controls.node)+YGNodeLayoutGetWidth(controls.node)<=YGNodeLayoutGetWidth(details.node)+1);
  }
}
'''.replace("PRODUCTION_METHOD", method)
        with tempfile.TemporaryDirectory(prefix="asobmashow-menu-yoga-") as temp:
            path = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            path.write_text(source)
            subprocess.run(["c++", "-std=c++20", "-I", str(root / "yoga"), str(path),
                            os.environ.get("ASOBMASHOW_TEST_YOGA_LIBRARY", str(root / "cmake-build-debug/yoga/yoga/libyogacore.a")), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_gameplay_title_uses_bottom_right_only_in_portrait(self):
        root = Path(__file__).resolve().parents[1]
        renderer = (root / "src/scene/play/BMSRenderer.cpp").read_text()
        methods = extract(renderer, "float BMSRenderer::gameplayHudTitleWidth() const") + "\n" + extract(
            renderer, "std::array<float, 4> BMSRenderer::gameplayHudTitleRect() const")
        source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include "settings/PresentationOrientation.h"
constexpr float kHudMargin=28;
namespace rendering {
int window_width=1080,window_height=1920;
struct Insets { int top=30,right=10,bottom=40,left=20; };
Insets uiSafeAreaInsets() { return {}; }
}
float baseGameplayHudTitleWidth() { return 430; }
float gameplayHudMetricsWidth() { return 430; }
struct BMSRenderer {
  player_settings::PresentationOrientation presentationOrientation=player_settings::PresentationOrientation::Portrait;
  float gameplayHudTitleWidth() const;
  std::array<float,4> gameplayHudTitleRect() const;
  float projectedLaneLeftUiInBand(float,float) const { return 200; }
};
PRODUCTION_METHODS
int main() {
  BMSRenderer renderer;
  for (int height : {1100,1440,1920,2340}) {
    rendering::window_height=height;
    const auto rect=renderer.gameplayHudTitleRect();
    assert(rect[0]+rect[2]==1080-10-28);
    assert(rect[1]+rect[3]==height-40-28);
    assert(rect[0]>=20+28+430+28 && rect[2]>=430);
  }
  renderer.presentationOrientation=player_settings::PresentationOrientation::Landscape;
  const auto rect=renderer.gameplayHudTitleRect();
  assert(rect[0]==20+28 && rect[1]==30+28 && rect[2]==200-48-18);
}
'''.replace("PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="asobmashow-title-hud-") as temp:
            path = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            path.write_text(source)
            subprocess.run(["c++", "-std=c++20", "-I", str(root / "src"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

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

    def test_orientation_numeric_controls_accept_their_own_ranges(self):
        root = Path(__file__).resolve().parents[1]
        shared = (root / "src/scene/SettingsSceneShared.h").read_text()
        methods = "\n".join(extract(shared, "inline float " + name + "(")
                            for name in ["clampLaneAngle", "clampLaneLength", "clampPlayAreaWidth"])
        source = r'''
#include "settings/PresentationGeometryPolicy.h"
#include <algorithm>
#include <cmath>
#include <cassert>
#include <limits>
struct AppSettings {
  player_settings::PresentationOrientation orientation = player_settings::PresentationOrientation::Landscape;
  auto geometryPolicy() const { return player_settings::presentationGeometryPolicy(orientation); }
};
PRODUCTION_METHODS
int main() {
  AppSettings settings;
  assert(clampLaneLength(settings, 32) == 12);
  assert(clampPlayAreaWidth(settings, 16) == 12);
  settings.orientation = player_settings::PresentationOrientation::Portrait;
  assert(clampLaneLength(settings, 32) == 32);
  assert(clampPlayAreaWidth(settings, 15) == 15);
  assert(clampPlayAreaWidth(settings, 1) == 2);
  assert(clampLaneAngle(settings, std::numeric_limits<float>::quiet_NaN()) == 0);
  assert(clampLaneLength(settings, std::numeric_limits<float>::infinity()) == 16);
}
'''.replace("PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="asobmashow-orientation-controls-") as temp:
            path = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            path.write_text(source)
            subprocess.run(["c++", "-std=c++20", "-I", str(root / "src"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_portrait_camera_keeps_all_eight_lanes_in_view(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/main.cpp").read_text(),
                         "void resetViewTransform(uint16_t bgaWidth, uint16_t bgaHeight,")
        source = r'''
#include <algorithm>
#include "rendering/PortraitPlayfieldFraming.h"
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
  Camera& setFov(float) { return *this; }
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
struct Insets { int top=0, right=0, bottom=0, left=0; };
Insets uiSafeAreaInsets() { return {}; }
Camera game_camera;
Camera* main_camera = &game_camera;
}
namespace gameplay_geometry { constexpr float kPlayAreaCenterX = 4; }
struct AppSettings {
  float laneLength = 8, laneAngleDegrees = 13.4;
  player_settings::PresentationOrientation orientation = player_settings::PresentationOrientation::Landscape;
  const AppSettings &presentation() const { return *this; }
  auto activePresentationOrientation() const { return orientation; }
  float playAreaWidthForKeyMode(int) const { return 8; }
};
PRODUCTION_METHOD
int main() {
  using namespace rendering;
  AppSettings settings;
  resetViewTransform(1920,1080,4,5,6,settings);
  assert(std::abs(game_camera.eye.z + 2.1f) < .0001f);
  for (int height : {1440, 1920, 2340}) {
    settings.orientation = player_settings::PresentationOrientation::Portrait;
    settings.laneLength = 16;
    settings.laneAngleDegrees = 0;
    window_width = 1080;
    window_height = height;
    resetViewTransform(1080,height,4,5,6,settings);
    const float angle = bx::toRad(settings.presentation().laneAngleDegrees);
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
            subprocess.run(["c++", "-std=c++23", "-I", str(root / "src"), str(path), "-o", str(binary)], check=True)
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
