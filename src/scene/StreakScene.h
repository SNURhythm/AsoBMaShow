#pragma once
#include "Scene.h"
#include <array>
#include "SceneReturnTarget.h"
#include "../streak/StreakModel.h"
#include "../notifications/LocalNotifications.h"
#include "../notifications/ReminderPreferences.h"
#include "../streak/StreakForm.h"
#include "../streak/StreakSounds.h"
#include "../streak/StreakRoundMotion.h"
class ScrollView;
class OverlayPortal;
class StreakScene final : public Scene {
public:
  StreakScene(ApplicationContext &, SceneReturnTarget = {},
              std::function<void(const std::string &)> openAssignment = {});
  void configureTab(float topInset, std::function<void()> onClose,
                    std::function<void(const std::string &)> openAssignment = {});
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
  void resetChallengeModal();
  std::string pendingResetAssignment_;
  streak::DayId pendingResetDay_ = 0;
  View *resetOverlay_ = nullptr;
  void reminderSettings(View *);
  void reminderPrompt(View *);
  bool saveReminders(reminders::Preferences);
  reminders::Permission reminderPermission_ = reminders::Permission::Unknown;
  void routine(View *);
  void editor(View *);
  void levelRangePicker(View *, const std::vector<streak::Level> &);
  std::string assignmentLevelsText(const streak::Assignment &);
  std::optional<std::map<std::string, int>> levelTableIds_;
  std::map<std::string, std::map<std::string, int>> levelOrder_;
  void editAssignment(int index);
  void acceptAssignment();
  void saveRoutine();
  SceneReturnTarget returnTarget_;
  bool embeddedTab_ = false;
  float tabTopInset_ = 0;
  std::function<void()> tabClose_;
  std::function<void(const std::string &)> openAssignment_;
  std::string profileId_, message_;
  streak::State state_;
  std::optional<streak::PlayProgress> streakProgress_;
  std::uint64_t streakProgressStartedAt_ = 0;
  streak::RoundPromotionCue roundCue_;
  bool goalIgnitionSoundPending_ = false;
  std::optional<std::uint64_t> goalIgnitionStartedAt_;
  std::array<ui::motion::Track, 3> meterTracks_;
  std::array<float, 3> meterTargets_{-1, -1, -1};
  streak::Config draft_;
  streak::Assignment assignment_;
  streak::FieldErrors fieldErrors_;
  std::string daily_, weekly_, monthly_, name_, count_, points_, rate_;
  enum class Page { Dashboard, Routine, Assignment, Reminders, ReminderPrompt } page_ = Page::Dashboard;
  Page renderedPage_ = Page::Dashboard;
  std::array<float, 5> scrollOffsets_{};
  int editingIndex_ = -1, tableId_ = 0;
  std::optional<std::string> rangeStartLevel_, rangeEndLevel_;
  bool loadFailed_ = false;
  bool showOptional_ = false;
  bool adjustingToday_ = false;
  View *root_ = nullptr;
  ScrollView *scroll_ = nullptr;
  OverlayPortal *portal_ = nullptr;
  streak::DayId checkedDay_ = 0;
  float elapsed_ = 0;
};
