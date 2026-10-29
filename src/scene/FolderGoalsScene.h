#pragma once
#include "Scene.h"
#include "SceneReturnTarget.h"
#include "../goals/FolderGoal.h"
#include "../goals/GoalNavigation.h"
class ScrollView;
class OverlayPortal;
class FolderGoalsScene final : public Scene {
public:
  using SelectFolder=std::function<void(const folder_goals::Source &,int)>;
  using SelectChart=std::function<void(const ChartMetaRecord &,int)>;
  FolderGoalsScene(ApplicationContext &,SceneReturnTarget,std::optional<folder_goals::Source> = {},SelectFolder = {},
                   SelectChart = {},std::shared_ptr<goal_navigation::State> = {});
  void init() override;
  void update(float) override;
  void onPause() override;
  void onResume() override;
  void onLanguageChanged() override;
  void onPresentationOrientationChanged() override;
  bool usesResponsiveMenuCanvas() const override { return true; }
  bool needsContinuousPresentation() const override { return false; }
  EventHandleResult handleEvents(SDL_Event &) override;
private:
  void renderScene() override {}
  void cleanupScene() override;
  bool checkProfile();
  void later(std::function<void()>);
  void reload();
  void switchGoalKind();
  void choose(folder_goals::Source,int,folder_goals::Type);
  void saveGoal();
  void removeGoal();
  void build();
  void layout();
  void back();
  void navigate();
  SceneReturnTarget returnTarget_;
  SelectFolder onSelect_;
  SelectChart onSelectChart_;
  std::shared_ptr<goal_navigation::State> navigationState_;
  std::optional<folder_goals::Source> initial_;
  std::string profile_,message_,rate_="100";
  std::vector<folder_goals::GoalProgress> goals_;
  folder_goals::Goal draft_;
  std::optional<folder_goals::Progress> progress_;
  bool detail_=false,editing_=false,confirmRemove_=false,failed_=false,achieved_=false;
  int offset_=0;
  View *root_=nullptr;
  View *content_=nullptr;
  ScrollView *navigationScroll_=nullptr;
  View *progressPanel_=nullptr;
  ScrollView *scroll_=nullptr;
  OverlayPortal *portal_=nullptr;
};
