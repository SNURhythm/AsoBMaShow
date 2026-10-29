#include "FolderGoalsScene.h"
#include "SceneManager.h"
#include "../goals/ChartGoal.h"
#include "../goals/FolderGoalPresentation.h"
#include "../LongNoteModeUtils.h"
#include "../rendering/ResponsiveUi.h"
#include "../view/ActionButtonStyle.h"
#include "../view/Button.h"
#include "../view/DropdownView.h"
#include "../view/GoalProgressBar.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif
namespace {
TextView *text(View *parent,const std::string &value,int size=20) {
  auto *v=new TextView(app_theme::font,size);
  v->setText(value); v->setWrap(true); v->setMinHeight(size+10)->setFlexShrink(0);
  v->setThemedColor(app_theme::textPrimary); parent->addView(v); return v;
}
Button *button(View *parent,const std::string &value,std::function<void()> click,bool primary=false) {
  auto *b=new Button(); b->setHeight(rendering::compactMenuCanvasActive ? 66 : 52)->setMinWidth(0)->setFlexShrink(0)->setCornerRadius(16);
  auto *label=new TextView(app_theme::font,21); label->setText(value); label->setAlign(TextView::CENTER); label->setVAlign(TextView::MIDDLE);
  b->setContentView(label); action_button::applyStyle(b,label,primary ? app_theme::ActionCategory::Primary : app_theme::ActionCategory::Neutral);
  b->setOnClickListener(std::move(click)); parent->addView(b); return b;
}
View *row(View *parent) {
  auto *v=new View(); v->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0); parent->addView(v); return v;
}
View *card(View *parent) {
  auto *v=new View(); v->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::All,22)
      ->setCornerRadius(24)->setFlexShrink(0)->setThemedBackgroundColor(app_theme::panel);
  parent->addView(v); return v;
}
void progress(View *parent,const folder_goals::Goal &goal,const folder_goals::Progress &p) {
  text(parent,folder_goals::targetText(goal),21);
  if (!p.sourceAvailable) { text(parent,i18n::tr("folder_goals.ui.unavailable")); return; }
  auto *numbers=row(parent);
  auto *value=text(numbers,folder_goals::percentText(p.percent),36); value->setFlex(1)->setMinWidth(0);
  value->setThemedColor(p.achieved ? app_theme::lime : app_theme::textPrimary);
  auto *state=text(numbers,i18n::tr(p.achieved ? "goals.ui.achieved" : "goals.ui.in_progress"),18);
  state->setVAlign(TextView::MIDDLE);
  parent->addView(new GoalProgressBar(p.percent,goal.target.percent/100.0,
      goal.target.type==folder_goals::Type::Lamp ? goal.target.lamp : -1,p.achieved));
  text(parent,i18n::format("folder_goals.ui.target_marker",{{"percent",folder_goals::percentText(goal.target.percent/100.0)}}),17)
      ->setThemedColor(app_theme::textSecondary);
  if (goal.target.type==folder_goals::Type::Lamp)
    text(parent,i18n::format("folder_goals.ui.counts",{{"count",std::to_string(p.qualifying)},{"total",std::to_string(p.available)},
        {"required",std::to_string(p.required)}}),18);
  else text(parent,i18n::format("folder_goals.ui.available",{{"count",std::to_string(p.available)}}),18);
  if (p.missing) text(parent,i18n::format("folder_goals.ui.missing",{{"count",std::to_string(p.missing)}}),17)->setThemedColor(app_theme::textSecondary);
  if (p.unknownScore) text(parent,i18n::format("folder_goals.ui.unknown",{{"count",std::to_string(p.unknownScore)}}),17);
  if (!p.available) text(parent,i18n::tr("folder_goals.ui.empty_source"),18);
}
}
FolderGoalsScene::FolderGoalsScene(ApplicationContext &c,SceneReturnTarget target,
    std::optional<folder_goals::Source> initial,SelectFolder select)
    : Scene(c),returnTarget_(std::move(target)),onSelect_(std::move(select)),initial_(std::move(initial)),
      profile_(c.profileManager.activeProfile().id) {}
bool FolderGoalsScene::checkProfile() {
  if (profile_==context.profileManager.activeProfile().id) return true;
  profile_=context.profileManager.activeProfile().id;
  initial_.reset(); goals_.clear(); draft_={}; progress_.reset();
  detail_=editing_=confirmRemove_=failed_=achieved_=false; offset_=0; rate_="100"; message_.clear();
  return false;
}
void FolderGoalsScene::later(std::function<void()> f) {
  const auto profile=profile_;
  defer([this,profile,f=std::move(f)] {
    if (!checkProfile() || profile!=profile_) { reload(); build(); return true; }
    f(); build(); return true;
  },0,true);
}
void FolderGoalsScene::init() {
  reload();
  if (initial_ && !failed_) choose(*initial_,long_note_mode::valueFromId(context.settings.selectedLnMode),folder_goals::Type::Lamp);
  build();
}
void FolderGoalsScene::reload() {
  auto loaded=context.scoreRepository.LoadFolderGoals(message_);
  failed_=!loaded; goals_=loaded.value_or(std::vector<folder_goals::GoalProgress>{});
  if (detail_ && !failed_) {
    auto preview=context.scoreRepository.PreviewFolderGoal(draft_,message_);
    progress_=preview ? std::optional(preview->progress) : std::nullopt;
    auto stored=std::find_if(goals_.begin(),goals_.end(),[this](const auto &g){return folder_goals::key(g.goal)==folder_goals::key(draft_);});
    if (stored!=goals_.end() && !editing_) draft_=stored->goal;
  }
}
void FolderGoalsScene::choose(folder_goals::Source source,int mode,folder_goals::Type type) {
  draft_={.source=std::move(source),.longNoteMode=std::clamp(mode,1,3),.target={.type=type}};
  const auto found=std::find_if(goals_.begin(),goals_.end(),[this](const auto &g){return folder_goals::key(g.goal)==folder_goals::key(draft_);});
  editing_=found==goals_.end();
  if (!editing_) draft_=found->goal;
  detail_=true; confirmRemove_=false; message_.clear();
  rate_=folder_goals::percentText(draft_.target.percent/100.0); rate_.pop_back();
  auto preview=context.scoreRepository.PreviewFolderGoal(draft_,message_);
  progress_=preview ? std::optional(preview->progress) : std::nullopt;
}
void FolderGoalsScene::saveGoal() {
  auto value=chart_goals::parseRate(rate_);
  if (!value || *value==0) { message_=i18n::tr("folder_goals.ui.invalid_percent"); return; }
  draft_.target.percent=*value;
  if (!context.scoreRepository.SaveFolderGoal(draft_,message_)) return;
  editing_=false; reload();
}
void FolderGoalsScene::removeGoal() {
  if (!context.scoreRepository.RemoveFolderGoal(draft_,message_)) return;
  detail_=editing_=confirmRemove_=false; progress_.reset(); reload();
}
void FolderGoalsScene::navigate() {
  if (!onSelect_ || !progress_ || !progress_->sourceAvailable) return;
  const auto source=draft_.source; const int mode=draft_.longNoteMode; const auto profile=profile_;
  defer([this,source,mode,profile] {
    if (!checkProfile() || profile_!=profile) { reload(); build(); return true; }
    auto callback=onSelect_; auto target=returnTarget_; auto *manager=context.sceneManager;
    callback(source,mode); (void)returnToScene(*manager,target); return false;
  },0,true);
}
void FolderGoalsScene::build() {
  for (auto *v:views) delete v; views.clear();
  progressPanel_=nullptr;
  root_=new View(0,0,rendering::window_width,rendering::window_height);
  root_->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::All,18)->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(root_);
  button(root_,i18n::tr("kombo.ui.back"),[this]{back();})->setWidth(140);
  scroll_=new ScrollView(); scroll_->setFlex(1)->setMinHeight(0); root_->addView(scroll_);
  auto *outer=new View(); outer->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignCenter);
  auto *body=new View(); body->setWidthPercent(100)->setGap(16)->setFlexDirection(FlexDirection::Column)->setPadding(Edge::Bottom,24);
  YGNodeStyleSetMaxWidth(body->getNode(),960); outer->addView(body); scroll_->setContentView(outer);
  portal_=new OverlayPortal(0,0,rendering::window_width,rendering::window_height);
  portal_->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Left,0)->setPosition(Edge::Top,0)->setZIndex(900);
  text(body,i18n::tr("folder_goals.ui.title"),30);
  if (failed_) button(body,i18n::tr("kombo.ui.retry"),[this]{later([this]{reload();});});
  else if (detail_) {
    auto *panel=card(body);
    text(panel,draft_.source.name.empty() ? draft_.source.identity : draft_.source.name,27);
    text(panel,i18n::tr("folder_goals.ui.scope_help"),17)->setThemedColor(app_theme::textSecondary);
    auto *modes=row(panel);
    for (int mode:{1,2,3}) button(modes,long_note_mode::idFromValue(mode),[this,mode]{later([this,mode]{choose(draft_.source,mode,draft_.target.type);});},mode==draft_.longNoteMode)->setFlex(1);
    auto *types=row(panel);
    for (auto type:{folder_goals::Type::Lamp,folder_goals::Type::AverageEx})
      button(types,i18n::tr(type==folder_goals::Type::Lamp ? "folder_goals.ui.lamp" : "folder_goals.ui.ex"),
          [this,type]{later([this,type]{choose(draft_.source,draft_.longNoteMode,type);});},type==draft_.target.type)->setFlex(1);
    progressPanel_=new View();
    progressPanel_->setFlexDirection(FlexDirection::Column)->setGap(12)->setFlexShrink(0);
    panel->addView(progressPanel_);
    if (progress_) progress(progressPanel_,draft_,*progress_);
    if (editing_) {
      if (draft_.target.type==folder_goals::Type::Lamp) {
        text(panel,i18n::tr("folder_goals.ui.minimum_lamp"),18);
        auto state=std::make_shared<DropdownView::State>(); state->selectedId=std::to_string(draft_.target.lamp); state->maxVisibleItems=7;
        for (int lamp:{0,50,100,200,300,400,500,600}) state->options.push_back({.id=std::to_string(lamp),.label=kombo::lampLabel(lamp),.leadingColor=clearLampColorForRank(lamp)});
        auto *dropdown=new DropdownView({.onOptionSelected=[this](const std::string &id){later([this,id]{
          draft_.target.lamp=std::stoi(id);
          auto preview=context.scoreRepository.PreviewFolderGoal(draft_,message_);
          progress_=preview ? std::optional(preview->progress) : std::nullopt;
        });}},portal_);
        dropdown->setHeight(60)->setFlexShrink(0); dropdown->setTriggerWidth(300); dropdown->refresh(*state); panel->addView(dropdown);
      }
      text(panel,i18n::tr("folder_goals.ui.target_percent"),18);
      auto *input=new TextInputBox(app_theme::font,24);
      input->setHeight(60)->setPadding(Edge::All,12)->setCornerRadius(14)->setFlexShrink(0);
      input->setThemedColor(app_theme::textPrimary); input->setThemedBackgroundColor(app_theme::insetSurface);
      input->setEditingText(rate_);
      input->onTextChanged([this](const auto &value) {
        rate_=value;
        auto parsed=chart_goals::parseRate(value);
        if (!parsed || *parsed==0) return;
        draft_.target.percent=*parsed;
        const auto profile=profile_;
        defer([this,profile] {
          if (profile!=context.profileManager.activeProfile().id || !editing_ || !progressPanel_ || !progress_) return true;
          auto preview=*progress_;
          preview.required=(preview.available*draft_.target.percent+9999)/10000;
          preview.achieved=preview.available>0 && (draft_.target.type==folder_goals::Type::Lamp
              ? preview.qualifying>=preview.required : preview.percent>=draft_.target.percent/100.0);
          progressPanel_->clearChildren(); progress(progressPanel_,draft_,preview); layout();
          return true;
        },0,true);
      });
      panel->addView(input);
      button(panel,i18n::tr("goals.ui.save"),[this]{later([this]{saveGoal();});},true)->setEnabled(progress_ && progress_->sourceAvailable && progress_->available>0);
    } else {
      text(panel,i18n::format("goals.ui.set_date",{{"date",chart_goals::localDateTime(draft_.setAt)}}),17);
      if (draft_.achievedAt) text(panel,i18n::format("goals.ui.achieved_date",{{"date",chart_goals::localDateTime(draft_.achievedAt)}}),17);
      button(panel,i18n::tr("folder_goals.ui.open"),[this]{navigate();},true)->setEnabled(onSelect_ && progress_ && progress_->sourceAvailable);
      button(panel,i18n::tr("goals.ui.edit"),[this]{later([this]{editing_=true;});});
      if (confirmRemove_) {
        text(panel,i18n::tr("goals.ui.remove_confirm"));
        button(panel,i18n::tr("goals.ui.remove"),[this]{later([this]{removeGoal();});});
        button(panel,i18n::tr("goals.ui.cancel"),[this]{later([this]{confirmRemove_=false;});});
      } else button(panel,i18n::tr("goals.ui.remove"),[this]{later([this]{confirmRemove_=true;});});
    }
    text(body,i18n::tr("folder_goals.ui.live_help"),17)->setThemedColor(app_theme::textSecondary);
  } else {
    text(body,i18n::tr("folder_goals.ui.intro"),19);
    auto *tabs=row(body);
    for (bool achieved:{false,true}) button(tabs,i18n::tr(achieved ? "goals.ui.achieved" : "goals.ui.in_progress"),
        [this,achieved]{later([this,achieved]{achieved_=achieved;offset_=0;});},achieved_==achieved)->setFlex(1);
    std::vector<const folder_goals::GoalProgress *> entries;
    for (const auto &g:goals_) if (g.progress.achieved==achieved_) entries.push_back(&g);
    offset_=std::min(offset_,entries.empty() ? 0 : static_cast<int>((entries.size()-1)/20*20));
    if (entries.empty()) text(body,i18n::tr("folder_goals.ui.empty"),19);
    for (int i=offset_;i<std::min(static_cast<int>(entries.size()),offset_+20);++i) {
      const auto &g=*entries[i]; auto *panel=card(body);
      text(panel,g.goal.source.name,25);
      text(panel,long_note_mode::idFromValue(g.goal.longNoteMode),17)->setThemedColor(app_theme::textSecondary);
      progress(panel,g.goal,g.progress);
      button(panel,i18n::tr("folder_goals.ui.details"),[this,goal=g.goal]{later([this,goal]{choose(goal.source,goal.longNoteMode,goal.target.type);});});
    }
    if (offset_>0) button(body,i18n::tr("goals.ui.previous"),[this]{later([this]{offset_-=20;});});
    if (offset_+20<static_cast<int>(entries.size())) button(body,i18n::tr("goals.ui.next"),[this]{later([this]{offset_+=20;});});
  }
  if (!message_.empty()) text(body,message_,19)->setThemedColor(app_theme::coral);
  addView(portal_); layout();
}
void FolderGoalsScene::layout() {
  if (!root_) return;
  root_->setSize(rendering::window_width,rendering::window_height); portal_->setSize(rendering::window_width,rendering::window_height);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto safe=GetIOSSafeAreaInsetsNormalized();
  root_->setPadding(Edge::Top,18+safe.top*rendering::window_height)->setPadding(Edge::Bottom,18+safe.bottom*rendering::window_height)
      ->setPadding(Edge::Left,18+safe.left*rendering::window_width)->setPadding(Edge::Right,18+safe.right*rendering::window_width);
#endif
  root_->applyYogaLayout(); scroll_->refreshContentLayout();
}
void FolderGoalsScene::back() {
  if (confirmRemove_) { later([this]{confirmRemove_=false;}); return; }
  if (detail_) { later([this]{detail_=editing_=false;message_.clear();reload();}); return; }
  defer([this]{(void)returnToScene(*context.sceneManager,returnTarget_);return false;},0,true);
}
void FolderGoalsScene::onPause() {
  const auto finish=[](View *v,const auto &self)->void {
    if (auto *input=dynamic_cast<TextInputBox *>(v)) input->endEditing();
    for (auto *child:v->getChildren()) self(child,self);
  };
  for (auto *v:views) {v->cancelPointerInput();finish(v,finish);}
}
void FolderGoalsScene::onResume() {checkProfile();reload();build();}
void FolderGoalsScene::onLanguageChanged() {build();}
void FolderGoalsScene::onPresentationOrientationChanged() {layout();}
void FolderGoalsScene::update(float) {
  if (!checkProfile()) {reload();build();}
  if (root_ && (root_->getWidth()!=rendering::window_width || root_->getHeight()!=rendering::window_height)) layout();
}
EventHandleResult FolderGoalsScene::handleEvents(SDL_Event &e) {
  for (auto it=views.rbegin();it!=views.rend();++it) if (!(*it)->handleEvents(e)) return {};
  if (e.type==SDL_EVENT_KEY_DOWN && e.key.key==SDLK_ESCAPE) back();
  return {};
}
void FolderGoalsScene::cleanupScene() {root_=nullptr;scroll_=nullptr;portal_=nullptr;progressPanel_=nullptr;}
