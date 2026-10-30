#include "goals/GoalNavigation.h"
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
struct Scene { virtual ~Scene()=default; };
struct Manager {
  std::shared_ptr<Scene> next;
  template<class T> void changeScene(std::unique_ptr<T> scene) { next=std::move(scene); }
};
struct Context {
  struct { struct Profile { std::string id="one"; } profile; const Profile &activeProfile() { return profile; } } profileManager;
  Manager *sceneManager;
};
using Callback=std::function<void(int,int)>;
struct Base : Scene {
  Context &context;
  int returnTarget_;
  std::function<void()> onBrowse_;
  std::shared_ptr<goal_navigation::State> navigationState_;
  std::vector<std::function<bool()>> pending;
  Base(Context &c,int target,std::shared_ptr<goal_navigation::State> state)
      : context(c),returnTarget_(target),navigationState_(state ? state : std::make_shared<goal_navigation::State>()) {}
  void defer(std::function<bool()> f,int,bool) { pending.push_back(std::move(f)); }
  void flush() { auto work=std::exchange(pending,{}); for(auto &f:work) f(); }
};
struct ChartGoalsScene : Base {
  using OverviewTab=goal_navigation::ChartTab;
  Callback onSelectChart_,onSelectFolder_;
  OverviewTab overviewTab_;
  ChartGoalsScene(Context &c,int target,std::optional<int>,Callback chart,std::optional<int>,bool,Callback folder,
      std::shared_ptr<goal_navigation::State> state, std::function<void()> browse={})
      : Base(c,target,state),onSelectChart_(chart),onSelectFolder_(folder),overviewTab_(navigationState_->chartTab) { onBrowse_=std::move(browse); }
  void rememberNavigation() { navigationState_->chartTab=overviewTab_; }
  void switchGoalKind();
};
struct FolderGoalsScene : Base {
  Callback onSelect_,onSelectChart_;
  bool achieved_;
  FolderGoalsScene(Context &c,int target,std::optional<int>,Callback folder,Callback chart,
      std::shared_ptr<goal_navigation::State> state, std::function<void()> browse={})
      : Base(c,target,state),onSelect_(folder),onSelectChart_(chart),achieved_(navigationState_->folderAchieved) { onBrowse_=std::move(browse); }
  void rememberScroll() {}
  void switchGoalKind();
};
METHODS
int main() {
  Manager manager; Context context{.sceneManager=&manager};
  int chartSelections=0,folderSelections=0,browses=0;
  auto chartCallback=[&](int record,int mode) { assert(record==11 && mode==2); ++chartSelections; };
  auto folderCallback=[&](int source,int mode) { assert(source==22 && mode==3); ++folderSelections; };
  ChartGoalsScene chart(context,77,{},chartCallback,{},false,folderCallback,{},[&]{++browses;});
  chart.overviewTab_=goal_navigation::ChartTab::Recommendations;
  chart.switchGoalKind(); assert(!manager.next); chart.flush();
  auto folder=std::dynamic_pointer_cast<FolderGoalsScene>(manager.next);
  assert(folder && folder->returnTarget_==77 && !folder->achieved_);
  folder->onSelect_(22,3); folder->onSelectChart_(11,2);
  assert(chartSelections==1 && folderSelections==1);
  folder->achieved_=true; folder->switchGoalKind(); folder->flush();
  auto returned=std::dynamic_pointer_cast<ChartGoalsScene>(manager.next);
  assert(returned && returned->returnTarget_==77 && returned->overviewTab_==goal_navigation::ChartTab::Recommendations);
  returned->onSelectChart_(11,2); returned->onSelectFolder_(22,3);
  assert(chartSelections==2 && folderSelections==2);
  returned->switchGoalKind(); returned->flush();
  auto remembered=std::dynamic_pointer_cast<FolderGoalsScene>(manager.next);
  assert(remembered && remembered->achieved_);
  remembered->onBrowse_(); assert(browses==1);
  remembered->switchGoalKind(); context.profileManager.profile.id="two"; remembered->flush();
  assert(manager.next==remembered);
  returned->switchGoalKind(); context.profileManager.profile.id="three"; returned->flush();
  assert(manager.next==remembered);
}
