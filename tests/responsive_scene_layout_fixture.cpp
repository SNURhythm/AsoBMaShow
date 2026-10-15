// Scene update and layout methods are extracted verbatim. The fixture controls
// platform insets and observes view layout requests without graphics or audio.
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <string>
#include <string_view>

namespace rendering {
int window_width=1200, window_height=800;
bool compactMenuCanvasActive=false;
constexpr int compactTouchTarget=66;
struct UiSafeAreaInsets { int top=0, left=0, bottom=0, right=0; };
UiSafeAreaInsets safe;
UiSafeAreaInsets uiSafeAreaInsets() { return safe; }
}
auto getSafeAreaInsetsUi() { return rendering::safe; }
enum class Edge { Top, Left, Bottom, Right, All };
enum class FlexDirection { Row, Column };
constexpr int YGDisplayNone=0, YGDisplayFlex=1, YGWrapWrap=1,
              YGWrapNoWrap=0, YGAlignStretch=1;
constexpr float YGUndefined=NAN, kRailWidth=350, kHeaderHeight=82, kScreenPadding=18;
struct View {
  virtual ~View()=default;
  std::array<float,4> padding{};
  int layouts=0;
  View *setPadding(Edge edge,float value) {
    if(edge==Edge::All) padding.fill(value);
    else padding.at(static_cast<int>(edge))=value;
    return this;
  }
  View *setSize(int,int) { return this; }
  View *setHeight(float) { return this; }
  View *setWidth(float) { return this; }
  View *setWidthPercent(float) { return this; }
  View *setFlex(float) { return this; }
  View *setMinWidth(float) { return this; }
  View *setMinHeight(float) { return this; }
  View *setFlexShrink(float) { return this; }
  View *setGap(float) { return this; }
  View *setFlexWrap(int) { return this; }
  View *setDisplay(int) { return this; }
  View *setAlignItems(int) { return this; }
  View *setFlexDirection(FlexDirection) { return this; }
  View *findViewByName(const char*) { return nullptr; }
  void applyYogaLayout() { ++layouts; }
};
struct TextView: View {
  enum { BOTTOM, MIDDLE };
  void setVAlign(int) {} void setAutoFitText(bool) {}
};
struct ScrollView: View {
  View *getContentView() { return nullptr; }
  void refreshContentLayout() {}
};
struct Context {
  struct {
    bool ProcessNativeControlEvents(std::string&) { return false; }
    bool ConsumeNativeControlStatus(std::string&) { return false; }
    bool ClubMode() { return false; }
  } musicPlayer;
  struct { bool musicPlayerClubModeEnabled=false; } settings;
  bool saveSettings() { return true; }
};
void SDL_Log(const char*) {}
struct LayoutFixture {
  View root, content, header, control;
  View *rootLayout=&root, *contentLayout=&content, *headerLayout=&header;
  int lastLayoutWidth=-1, lastLayoutHeight=-1;
  int lastSafeTop=-1, lastSafeLeft=-1, lastSafeBottom=-1, lastSafeRight=-1;
  bool embeddedTab=false;
  float tabTopInset=80;
};
struct MusicPlayerScene: LayoutFixture {
  Context context;
  View overlay, video;
  View *overlayPortal=&overlay, *videoOverlayRoot=&video;
  View *libraryNavButton=&control, *favoritesNavButton=&control,
       *playlistsNavButton=&control, *playerNavButton=&control,
       *statusText=&control, *libraryList=&control, *favoritesList=&control;
  void update(float); void updateLayoutInsets();
  void layoutVideoArtwork() {} void setStatus(const std::string&) {}
  void refreshActiveQueueList(bool) {} void updateVideoFullscreen() {}
  void refreshUi() {} void refreshVideoOverlay() {}
};
struct IrUploadsScene: LayoutFixture {
  View *providerStatusText=&control, *openIrSettingsButton=&control,
       *candidateCountText=&control, *refreshButton=&control,
       *selectAllButton=&control, *clearButton=&control;
  bool reloadRequested=false;
  struct { bool selectionLocked() { return false; } } controller;
  void update(float); void updateLayoutInsets();
  void refreshProfileIfNeeded() {} void applyPreparationUpdates() {}
  void observeRemoteRevisions() {} void reloadCandidates() {}
};
struct IntroScene {
  View root, content, menu;
  TextView title;
  View *rootLayout_=&root, *content_=&content, *menu_=&menu;
  TextView *title_=&title;
  int layoutWidth_=-1, layoutHeight_=-1;
  int lastSafeTop=-1, lastSafeLeft=-1, lastSafeBottom=-1, lastSafeRight=-1;
  void update(float); void layoutView(); void processNavigationInput() {}
};
SCENE_METHODS

template<class Scene> void check(Scene &scene, const std::string &edge) {
  scene.update(0);
  int *inset=edge=="top" ? &rendering::safe.top : edge=="left" ? &rendering::safe.left
             : edge=="bottom" ? &rendering::safe.bottom : &rendering::safe.right;
  const int before=scene.root.layouts;
  for(int i=0;i<3;++i) scene.update(0);
  assert(scene.root.layouts==before && "unchanged layouts should stay cached");
  *inset=37;
  scene.update(0);
  assert(scene.root.layouts==before+1 && "safe-area edge change must invalidate layout at fixed canvas size");
  for(int i=0;i<3;++i) scene.update(0);
  assert(scene.root.layouts==before+1 && "updated insets must be cached after layout");
  assert(rendering::window_width==1200 && rendering::window_height==800);
}
int main(int argc,char **argv) {
  assert(argc==3);
  const std::string sceneName=argv[1], edge=argv[2];
  if(sceneName=="intro") {
    IntroScene scene;
    check(scene,edge);
    const int index=edge=="top" ? 0 : edge=="left" ? 1 : edge=="bottom" ? 2 : 3;
    assert(scene.root.padding[index]==85);
  } else if(sceneName=="music" || sceneName=="embedded-music") {
    MusicPlayerScene scene;
    scene.embeddedTab=sceneName=="embedded-music";
    check(scene,edge);
    assert(scene.root.padding[0]==(scene.embeddedTab ? 80 : rendering::safe.top));
    assert(scene.content.padding[1]==rendering::safe.left);
    assert(scene.content.padding[2]==rendering::safe.bottom);
    assert(scene.content.padding[3]==rendering::safe.right);
    assert(scene.video.padding[1]==rendering::safe.left+24);
    assert(scene.video.padding[2]==rendering::safe.bottom+24);
    assert(scene.video.padding[3]==rendering::safe.right+24);
  } else {
    IrUploadsScene scene;
    scene.embeddedTab=sceneName=="embedded-ir";
    check(scene,edge);
    assert(scene.root.padding[0]==(scene.embeddedTab ? 80 : 0));
    assert(scene.header.padding[0]==(scene.embeddedTab ? 10 : rendering::safe.top+10));
    assert(scene.content.padding[1]==rendering::safe.left);
    assert(scene.content.padding[2]==rendering::safe.bottom);
    assert(scene.content.padding[3]==rendering::safe.right);
  }
}
