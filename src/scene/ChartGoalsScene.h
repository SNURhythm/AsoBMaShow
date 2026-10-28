#pragma once
#include "Scene.h"
#include "SceneReturnTarget.h"
#include "../goals/ChartGoal.h"

class ScrollView;
class OverlayPortal;
class ChartGoalsScene final : public Scene {
public:
  using SelectChart = std::function<void(const ChartMetaRecord &, int)>;
  ChartGoalsScene(ApplicationContext &, SceneReturnTarget,
                  std::optional<ChartMetaRecord> initial = {}, SelectChart = {},
                  std::optional<int> initialMode = {}, bool initialRecommendations = false);
  void init() override;
  void onPause() override;
  void onResume() override;
  void update(float) override;
  bool usesResponsiveMenuCanvas() const override { return true; }
  bool needsContinuousPresentation() const override { return false; }
  void onPresentationOrientationChanged() override;
  void onLanguageChanged() override;
  EventHandleResult handleEvents(SDL_Event &) override;
private:
  void renderScene() override {}
  void cleanupScene() override;
  bool checkProfile();
  void later(std::function<void()>);
  void reload();
  void loadRecommendations(int rank, int offset);
  void recommendGoal(const ChartMetaRecord &, int mode, int rank);
  void changeDraftMode(int mode);
  struct RecommendationGroup {
    int count = 0, offset = 0;
    bool expanded = false;
    std::vector<std::pair<GoalRecommendation, ChartMetaRecord>> rows;
  };
  std::map<int, RecommendationGroup> recommendations_;
  std::vector<DifficultyTableInfo> recommendationTables_;
  int recommendationTable_ = 0;
  bool recommendationSortTable_ = false;
  enum class OverviewTab { Ongoing, Recommendations, Achieved };
  OverviewTab overviewTab_ = OverviewTab::Ongoing;
  std::optional<int> recommendationRank_;
  ScrollView *navigationScroll_ = nullptr;
  float navigationOffset_ = 0;
  std::map<std::string, float> scrollOffsets_;
  std::string renderedScrollKey_;
  std::string scrollKey() const;
  void showGoal(chart_goals::Goal);
  void saveGoal();
  void removeGoal();
  void resolveChart();
  void navigate(bool practice);
  void build();
  void layout();
  void back();
  SceneReturnTarget returnTarget_;
  SelectChart onSelectChart_;
  std::optional<ChartMetaRecord> initial_, resolved_;
  std::optional<int> initialMode_;
  bool initialRecommendations_ = false;
  std::string profileId_, message_, customRate_;
  chart_goals::ProgressPage page_;
  std::optional<chart_goals::GoalProgress> selected_;
  chart_goals::Goal draft_;
  bool editing_ = false, customScore_ = false, confirmRemove_ = false, loadFailed_ = false;
  int offset_ = 0, achievedOffset_ = 0;
  View *root_ = nullptr;
  ScrollView *scroll_ = nullptr;
  OverlayPortal *portal_ = nullptr;
};
