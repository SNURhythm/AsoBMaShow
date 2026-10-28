#pragma once
#include "Scene.h"
#include <array>
#include "SceneReturnTarget.h"
#include "../kombo/KomboModel.h"
#include "../notifications/LocalNotifications.h"
#include "../notifications/ReminderPreferences.h"
#include "../kombo/KomboForm.h"
#include "../kombo/KomboSounds.h"
#include "../kombo/KomboRoundMotion.h"
class ScrollView;
class OverlayPortal;
class KomboScene final : public Scene {
public:
  KomboScene(ApplicationContext &, SceneReturnTarget = {},
              std::function<void(const std::string &)> openAssignment = {},
              std::function<void()> openGoals = {});
  void configureTab(float topInset, std::function<void()> onClose,
                    std::function<void(const std::string &)> openAssignment = {},
              std::function<void()> openGoals = {});
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
  kombo::Sounds komboSounds_;
  void renderScene() override {}
  void cleanupScene() override;
  void reload();
  std::set<std::string> unavailableAssignments(const kombo::Day &);
  void build();
  void layout();
  void later(std::function<void()> action);
  void back();
  void dashboard(View *);
  void resetChallengeModal();
  std::string pendingResetAssignment_;
  kombo::DayId pendingResetDay_ = 0;
  View *resetOverlay_ = nullptr;
  void reminderSettings(View *);
  void reminderPrompt(View *);
  bool saveReminders(reminders::Preferences);
  reminders::Permission reminderPermission_ = reminders::Permission::Unknown;
  void routine(View *);
  void editor(View *);
  void levelRangePicker(View *, const std::vector<kombo::Level> &);
  std::string assignmentLevelsText(const kombo::Assignment &);
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
  std::function<void()> openGoals_;
  std::string profileId_, message_;
  kombo::State state_;
  std::optional<kombo::PlayProgress> komboProgress_;
  std::uint64_t komboProgressStartedAt_ = 0;
  kombo::RoundPromotionCue roundCue_;
  bool goalIgnitionSoundPending_ = false;
  std::optional<std::uint64_t> goalIgnitionStartedAt_;
  std::array<ui::motion::Track, 3> meterTracks_;
  std::array<float, 3> meterTargets_{-1, -1, -1};
  kombo::Config draft_;
  kombo::Assignment assignment_;
  kombo::FieldErrors fieldErrors_;
  std::string daily_, weekly_, monthly_, name_, count_, rate_;
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
  kombo::DayId checkedDay_ = 0;
  float elapsed_ = 0;
};
