#pragma once
#include "Scene.h"
#include "SceneReturnTarget.h"
#include "../streak/StreakModel.h"
#include "../streak/StreakForm.h"
#include "../streak/StreakSounds.h"
#include "../streak/StreakRoundMotion.h"
class ScrollView;
class OverlayPortal;
class StreakScene final : public Scene {
public:
  StreakScene(ApplicationContext &, SceneReturnTarget = {});
  void configureTab(float topInset, std::function<void()> onClose);
  void setTabTopInset(float topInset);
  void onPause() override;
  void onResume() override;
  void init() override;
  void update(float) override;
  bool usesResponsiveMenuCanvas() const override { return true; }
  bool needsContinuousPresentation() const override { return false; }
  void onPresentationOrientationChanged() override;
  void onLanguageChanged() override;
  EventHandleResult handleEvents(SDL_Event &) override;
private:
  streak::Sounds streakSounds_;
  void renderScene() override {}
  void cleanupScene() override;
  void reload();
  std::set<std::string> unavailableAssignments(const streak::Day &);
  void build();
  void layout();
  void later(std::function<void()> action);
  void back();
  void dashboard(View *);
  void routine(View *);
  void editor(View *);
  void editAssignment(int index);
  void acceptAssignment();
  void saveRoutine();
  SceneReturnTarget returnTarget_;
  bool embeddedTab_ = false;
  float tabTopInset_ = 0;
  std::function<void()> tabClose_;
  std::string profileId_, message_;
  streak::State state_;
  std::optional<streak::PlayProgress> streakProgress_;
  std::uint64_t streakProgressStartedAt_ = 0;
  streak::RoundPromotionCue roundCue_;
  streak::Config draft_;
  streak::Assignment assignment_;
  streak::FieldErrors fieldErrors_;
  std::string daily_, weekly_, monthly_, name_, count_, points_, rate_;
  enum class Page { Dashboard, Routine, Assignment } page_ = Page::Dashboard;
  int editingIndex_ = -1, tableId_ = 0;
  bool loadFailed_ = false;
  bool showOptional_ = false;
  bool adjustingToday_ = false;
  View *root_ = nullptr;
  ScrollView *scroll_ = nullptr;
  OverlayPortal *portal_ = nullptr;
  streak::DayId checkedDay_ = 0;
  float elapsed_ = 0;
};
