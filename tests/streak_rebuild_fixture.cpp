#include "ui/Motion.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
static std::uint64_t ticks = 1000;
std::uint64_t SDL_GetTicks() { return ticks; }
namespace rendering { int window_width=600, window_height=800; }
namespace i18n { const char *tr(const char *key) { return key; } }
namespace ui_icons { constexpr int kCollapse=0,kRotate=1; }
namespace app_theme { enum class ActionCategory { Neutral }; int mainMenuBackdrop=0; }
enum class FlexDirection { Column, Row };
enum class Edge { All, Left, Top, Bottom };
constexpr int YGAlignStretch=0,YGAlignCenter=1,YGPositionTypeAbsolute=2;
struct View {
  std::vector<View *> children;
  View(int=0,int=0,int=0,int=0) {}
  virtual ~View() { for (auto *child : children) delete child; }
  void addView(View *view) { children.push_back(view); }
  View *findViewByName(const std::string &) { return nullptr; }
  int getY() const { return 0; }
  void *getNode() { return nullptr; }
#define STYLE(name) template<class... Args> View *name(Args...) { return this; }
  STYLE(setFlexDirection) STYLE(setAlignItems) STYLE(setPadding) STYLE(setGap)
  STYLE(setThemedBackgroundColor) STYLE(setPositionType) STYLE(setPosition)
  STYLE(setZIndex) STYLE(setFlexShrink) STYLE(setFlex) STYLE(setMinHeight)
  STYLE(setWidthPercent) STYLE(setWidth)
#undef STYLE
};
void YGNodeStyleSetMaxWidth(void *, int) {}
struct ScrollView : View {
  float offset=0, maximum=1000;
  void setContentView(View *view) { addView(view); }
  float getScrollOffset() const { return offset; }
  void setScrollOffset(float value) { offset=std::clamp(value,0.0F,maximum); }
};
struct OverlayPortal : View { using View::View; };
int backActions=0;
View *label(View *parent, const std::string &, int=21) { auto *v=new View;parent->addView(v);return v; }
View *action(View *parent, const std::string &title, std::function<void()>, app_theme::ActionCategory,int) {
  if(title=="streak.ui.back")++backActions;
  return label(parent,title);
}
struct ProgressMotionView : View {
  ui::motion::Track track;
  ProgressMotionView(ui::motion::Track value,bool) : track(value) {}
};
METER
struct StreakScene {
  enum class Page { Dashboard, Routine, Assignment, Reminders, ReminderPrompt } page_=Page::Dashboard,renderedPage_=Page::Dashboard;
  std::array<float,5> scrollOffsets_{};
  bool embeddedTab_=true,goalIgnitionSoundPending_=false,loadFailed_=false;
  int roundCue_=0;
  float maximum=1000;
  std::string message_;
  std::vector<View *> views;
  View *root_=nullptr,*resetOverlay_=nullptr;
  ScrollView *scroll_=nullptr;
  OverlayPortal *portal_=nullptr;
  ~StreakScene() { for(auto *view:views)delete view; }
  void addView(View *view) { views.push_back(view); }
  void back() {} void reload() {} void later(std::function<void()>) {}
  void dashboard(View *) {} void routine(View *) {} void editor(View *) {} void reminderSettings(View *) {} void reminderPrompt(View *) {}
  void resetChallengeModal() {}
  void layout() { scroll_->maximum=maximum; }
  void build();
};
BUILD
int main() {
  StreakScene scene;
  scene.build(); assert(backActions==0);
  scene.scroll_->setScrollOffset(400); scene.build();
  assert(scene.scroll_->getScrollOffset()==400 && backActions==0);
  scene.page_=StreakScene::Page::Routine; scene.build();
  assert(scene.scroll_->getScrollOffset()==0 && backActions==1);
  scene.scroll_->setScrollOffset(250); scene.build();
  assert(scene.scroll_->getScrollOffset()==250);
  scene.page_=StreakScene::Page::Dashboard; scene.build();
  assert(scene.scroll_->getScrollOffset()==400);
  scene.maximum=120; scene.build(); assert(scene.scroll_->getScrollOffset()==120);
  backActions=0; scene.embeddedTab_=false; scene.build(); assert(backActions==1);

  ui::motion::Track track;
  float target=-1;
  View initial;
  meter(&initial,50,100,track,target);
  ticks+=400;
  const float halfway=track.value(ticks);
  assert(halfway>0 && halfway<.5F);
  View rebuilt;
  meter(&rebuilt,50,100,track,target);
  assert(static_cast<ProgressMotionView *>(rebuilt.children[0])->track.value(ticks)==halfway);
  meter(&rebuilt,80,100,track,target);
  assert(track.value(ticks)==halfway); // New totals start at the current displayed value.
  ticks+=1000;
  assert(track.value(ticks)==.8F);
  meter(&rebuilt,80,100,track,target);
  assert(!track.active(ticks) && track.value(ticks)==.8F);
}
