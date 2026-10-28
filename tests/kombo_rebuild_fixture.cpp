#include "ui/Motion.h"
#include "kombo/KomboForm.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
static std::uint64_t ticks = 1000;
std::uint64_t SDL_GetTicks() { return ticks; }
namespace rendering { int window_width=600, window_height=800; }
namespace i18n { const char *tr(const char *key) { return key; } }
namespace ui_icons { constexpr int kCollapse=0,kRotate=1,kPlus=2; }
namespace app_theme { enum class ActionCategory { Neutral }; int mainMenuBackdrop=0; }
enum class FlexDirection { Column, Row };
enum class Edge { All, Left, Top, Bottom };
constexpr int YGAlignStretch=0,YGAlignCenter=1,YGPositionTypeAbsolute=2;
struct View {
  std::vector<View *> children;
  bool enabled=true;
  View(int=0,int=0,int=0,int=0) {}
  virtual ~View() { for (auto *child : children) delete child; }
  void addView(View *view) { children.push_back(view); }
  View *findViewByName(const std::string &) { return nullptr; }
  int getY() const { return 0; }
  void *getNode() { return nullptr; }
  void setEnabled(bool value) { enabled=value; }
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
struct DropdownView { struct Option { std::string id,label; }; };
std::vector<std::function<void(const std::string &)>> rangeChoices;
std::function<void()> addLevels;
View *addLevelsButton=nullptr;
void select(View *,OverlayPortal *,const char *,std::vector<DropdownView::Option>,std::string,
            std::function<void(const std::string &)> change,const std::string &) {
  rangeChoices.push_back(std::move(change));
}
int integer(const std::string &text) { return std::stoi(text); }
int backActions=0;
View *label(View *parent, const std::string &, int=21) { auto *v=new View;parent->addView(v);return v; }
View *action(View *parent, const std::string &title, std::function<void()> callback, app_theme::ActionCategory,int) {
  if(title=="kombo.ui.back")++backActions;
  auto *view=label(parent,title);
  if(title=="kombo.ui.add_levels") { addLevels=std::move(callback); addLevelsButton=view; }
  return view;
}
struct ProgressMotionView : View {
  ui::motion::Track track;
  ProgressMotionView(ui::motion::Track value,bool) : track(value) {}
};
METER
struct KomboScene {
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
  std::optional<std::map<std::string,int>> levelTableIds_;
  std::map<std::string,std::map<std::string,int>> levelOrder_;
  kombo::Assignment assignment_;
  kombo::FieldErrors fieldErrors_;
  std::optional<std::string> rangeStartLevel_,rangeEndLevel_;
  ~KomboScene() { for(auto *view:views)delete view; }
  void addView(View *view) { views.push_back(view); }
  void back() {} void reload() {} void later(std::function<void()> callback) { callback(); }
  void dashboard(View *) {} void routine(View *) {} void editor(View *) {} void reminderSettings(View *) {} void reminderPrompt(View *) {}
  void resetChallengeModal() {}
  void layout() { scroll_->maximum=maximum; }
  void build();
  void levelRangePicker(View *,const std::vector<kombo::Level> &);
};
BUILD
RANGE_PICKER
int main() {
  KomboScene scene;
  scene.build(); assert(backActions==0);
  scene.scroll_->setScrollOffset(400); scene.build();
  assert(scene.scroll_->getScrollOffset()==400 && backActions==0);
  scene.page_=KomboScene::Page::Routine; scene.build();
  assert(scene.scroll_->getScrollOffset()==0 && backActions==1);
  scene.scroll_->setScrollOffset(250); scene.build();
  assert(scene.scroll_->getScrollOffset()==250);
  scene.page_=KomboScene::Page::Dashboard; scene.build();
  assert(scene.scroll_->getScrollOffset()==400);
  scene.maximum=120; scene.build(); assert(scene.scroll_->getScrollOffset()==120);
  backActions=0; scene.embeddedTab_=false; scene.build(); assert(backActions==1);

  const std::vector<kombo::Level> levels{{"table","2","sl2"},{"table","10","sl10"},
                                        {"table","EX","slEX"}};
  View picker;
  const auto rebuildPicker=[&] { rangeChoices.clear(); scene.levelRangePicker(&picker,levels); };
  rebuildPicker(); assert(!addLevelsButton->enabled);
  rangeChoices[0]("0"); // The first endpoint initializes a one-level range.
  assert(scene.rangeStartLevel_=="2" && scene.rangeEndLevel_=="2");
  rebuildPicker(); assert(addLevelsButton->enabled);
  rangeChoices[1]("1");
  rebuildPicker();
  rangeChoices[0]("2"); // From crossing To pulls To upward.
  assert(scene.rangeStartLevel_=="EX" && scene.rangeEndLevel_=="EX");
  rebuildPicker();
  rangeChoices[1]("0"); // To crossing From pulls From downward.
  assert(scene.rangeStartLevel_=="2" && scene.rangeEndLevel_=="2");
  rebuildPicker();
  rangeChoices[1]("2");
  rebuildPicker();
  rangeChoices[0]("1"); // Moving inside the range preserves the other endpoint.
  assert(scene.rangeStartLevel_=="10" && scene.rangeEndLevel_=="EX");
  rebuildPicker();
  addLevels();
  assert((scene.assignment_.levels==std::vector<kombo::Level>{levels[1],levels[2]}));
  rangeChoices[1]("");
  rebuildPicker(); assert(!addLevelsButton->enabled);
  assert(!scene.rangeStartLevel_ && !scene.rangeEndLevel_);
  rangeChoices[1]("1"); // Starting with To also initializes From.
  assert(scene.rangeStartLevel_=="10" && scene.rangeEndLevel_=="10");

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
