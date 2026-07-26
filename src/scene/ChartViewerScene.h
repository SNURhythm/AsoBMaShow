#pragma once

#include "../repositories/ChartRepository.h"
#include "../repositories/ReplayRepository.h"
#include "../bms_parser.hpp"
#include "../practice/PracticeConfiguration.h"
#include "../practice/PracticeLaunchRequest.h"
#include "../practice/PracticePresetStore.h"
#include "Scene.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Button;
class ChartCanvasView;
class ReplaySummaryListItemView;
class ReplaySummaryListView;
class ScrollView;
class OverlayPortal;
class PracticePanelView;
class PlayOptionsPanelView;
class TextView;
class View;

namespace chart_viewer_practice {
struct GhostRefreshState {
  long long chartEndMicros = 0;
  practice::Configuration configuration;
  std::vector<practice::NamedPreset> namedPresets;
  std::optional<std::string> selectedPresetId;
  std::optional<practice::LaunchRequest> pendingLaunchRequest;
  std::optional<JudgedPlaybackData> ghostReplay;
  int loadedGhostReplayId = -1;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  replay::DoublePlayOption doublePlayOption =
      replay::DoublePlayOption::Normal;
  std::string visibleStatus;
};

template <typename Commit>
[[nodiscard]] bool installGhostRefreshState(
    GhostRefreshState state, long long newChartEndMicros,
    practice::PresetLoadResult loaded, const std::string &successText,
    Commit &&commit) {
  state.chartEndMicros = newChartEndMicros;
  const auto notice = loaded.notice();
  const bool usable = practice::installPresetLoadState(
      std::move(loaded), true, state.configuration, state.namedPresets,
      state.selectedPresetId);
  state.visibleStatus = notice ? "Practice presets: " + *notice : successText;
  std::forward<Commit>(commit)(std::move(state));
  return usable;
}
} // namespace chart_viewer_practice

class ChartViewerScene : public Scene {
public:
  ChartViewerScene(
      ApplicationContext &context, ChartMetaRecord record,
      std::optional<unsigned int> randomSeed = std::nullopt,
      std::optional<std::string> randomPrng = std::nullopt,
      std::optional<std::vector<int>> randomValues = std::nullopt,
      std::optional<practice::LaunchRequest> launchRequest = std::nullopt);

  void init() override;
  void onResume() override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

  void setPracticeGhostReplay(const JudgedPlaybackData &replayData);
  void setPracticeLaunchRequest(practice::LaunchRequest request);

private:
  struct RandomOption {
    size_t index = 0;
    int maxValue = 1;
    int selectedValue = 1;
    int depth = 0;
    size_t sourceLine = 0;
  };

  ChartMetaRecord record;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> selectedRandomValues;
  std::vector<RandomOption> randomOptions;
  std::unique_ptr<bms_parser::Chart> chart;
  std::vector<unsigned char> chartSourceBytes;

  View *rootLayout = nullptr;
  ChartCanvasView *canvasView = nullptr;
  TextView *titleText = nullptr;
  TextView *subtitleText = nullptr;
  TextView *statusText = nullptr;
  TextView *randomSummaryText = nullptr;
  TextView *zoomText = nullptr;
  TextView *selectionText = nullptr;
  TextView *listenPauseText = nullptr;
  Button *listenPauseButton = nullptr;
  Button *listenStopButton = nullptr;
  Button *ghostLoadButton = nullptr;
  TextView *ghostLoadButtonText = nullptr;
  Button *ghostClearButton = nullptr;
  TextView *ghostClearButtonText = nullptr;
  View *ghostModalRoot = nullptr;
  TextView *ghostModalEmptyText = nullptr;
  Button *practiceGhostReplayButton = nullptr;
  ReplaySummaryListItemView *practiceGhostReplayItem = nullptr;
  ReplaySummaryListView *ghostReplayListView = nullptr;
  View *optionsDrawerRoot = nullptr;
  TextView *viewerOptionText = nullptr;
  PlayOptionsPanelView *viewerPlayOptionsPanel = nullptr;
  View *randomDrawerRoot = nullptr;
  ScrollView *randomDrawerScroll = nullptr;
  OverlayPortal *overlayPortal = nullptr;
  PracticePanelView *practicePanel = nullptr;
  std::unique_ptr<practice::PresetStore> practicePresetStore;
  practice::Configuration practiceConfiguration;
  std::vector<practice::NamedPreset> practiceNamedPresets;
  std::optional<std::string> selectedPracticePresetId;
  long long practiceChartEndMicros = 0;
  std::optional<practice::LaunchRequest> pendingPracticeLaunchRequest;
  GameplayRuleset practiceRuleset = kDefaultGameplayRuleset;
  std::optional<RulesetDescriptor> practiceRequiredRulesetDescriptor;
  std::optional<ScoreStageProvenance> practiceReplayRulesetSnapshot;

  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;
  size_t randomDrawerPage = 0;
  bool listenActive = false;
  bool listenAudioLoaded = false;
  bool retainedListenResourcesForReload = false;
  long long listenEndMicros = 0;
  std::optional<JudgedPlaybackData> practiceGhostReplay;
  std::vector<ReplaySummary> ghostReplaySummaries;
  int selectedGhostReplayIndex = -1;
  int loadedGhostReplayId = -1;
  std::optional<std::string> viewerPlayOption;
  std::optional<long long> viewerPlayOptionSeed;
  std::optional<std::string> viewerPlayOption2;
  std::optional<long long> viewerPlayOption2Seed;
  replay::DoublePlayOption viewerDoublePlayOption =
      replay::DoublePlayOption::Normal;
  std::optional<std::string> viewerLaneOrderSummary;
  std::string viewerAssistOption = assist_options::kOff;

  void initView();
  void rebuildRandomDrawer();
  void showRandomDrawer();
  void hideRandomDrawer();
  void parseAndRefresh(std::optional<std::vector<int>> requestedValues);
  void setRandomValue(size_t index, int value);
  void refreshHeaderText();
  void updateZoomText();
  void updateSelectionText();
  void updateListenControls();
  void rebuildGhostModal();
  void showGhostModal();
  void hideGhostModal();
  void updateGhostControls();
  void updateGhostModalActions();
  void updatePracticeGhostReplayButton();
  void loadPracticeGhostReplay();
  void loadSelectedGhostReplay();
  bool applyGhostReplayData(const JudgedPlaybackData &replayData, int loadedReplayId,
                            const std::string &successText);
  void clearGhostReplay();
  void rebuildOptionsDrawer();
  void showOptionsDrawer();
  void hideOptionsDrawer();
  void refreshOptionsDrawer();
  void setViewerNamedPlayOption(const std::string &option);
  void setViewerAssistOption(const std::string &option);
  void setViewerLongNoteMode(const std::string &mode);
  void setViewerPlaybackRate(int percent);
  void setViewerPlaybackMode(const std::string &mode);
  void toggleViewerClubMode();
  void refreshViewerOptionControls();
  void setViewerLaneAssign(const std::string &notation);
  void setViewerPlayOptions(const std::optional<std::string> &option,
                            const std::optional<long long> &seed,
                            const std::optional<std::string> &option2,
                            const std::optional<long long> &seed2);
  bool applyViewerPlayOptions(bms_parser::Chart &target,
                              const char *logContext);
  void onCanvasSelectionChanged(long long timeMicros);
  void onPracticeRangeChanged(const practice::RangeSelection &range);
  void onPracticeConfigurationChanged(
      const practice::Configuration &configuration);
  void selectActivePracticeMarker(practice::Marker marker);
  void moveActivePracticeMarker(practice::TimelineDirection direction);
  void loadPracticeConfiguration(
      bool applyPendingLaunch = true,
      std::optional<std::string> chartReplacementSuccessText = std::nullopt);
  void applyPendingPracticeLaunchRequest();
  bool applyPracticePresetLoad(practice::PresetLoadResult loaded,
                               bool applyLastUsed);
  void refreshPracticePanel();
  void savePracticeAs(std::string name);
  void renamePracticePreset(std::string name);
  void updatePracticePreset();
  void deletePracticePreset();
  void retainLoadedListenResourcesForChartChange();
  void startListeningFromSelection();
  void toggleListenPause();
  void stopListening();
  void startPracticeFromSelection(bool autoPlay);
  void goBack();

  [[nodiscard]] std::vector<RandomOption>
  scanActiveRandomOptions(
      const std::vector<unsigned char> *sourceBytes = nullptr) const;
  [[nodiscard]] std::string randomSummary() const;
  [[nodiscard]] std::string viewerPlayOptionLabel() const;
};
