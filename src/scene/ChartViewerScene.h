#pragma once

#include "../ChartDBHelper.h"
#include "../ReplayDBHelper.h"
#include "../bms_parser.hpp"
#include "Scene.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class ChartCanvasView;
class ReplaySummaryListItemView;
class ReplaySummaryListView;
class ScrollView;
class TextInputBox;
class TextView;
class View;

class ChartViewerScene : public Scene {
public:
  ChartViewerScene(
      ApplicationContext &context, ChartMetaRecord record,
      std::optional<unsigned int> randomSeed = std::nullopt,
      std::optional<std::string> randomPrng = std::nullopt,
      std::optional<std::vector<int>> randomValues = std::nullopt);

  void init() override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

  void setPracticeGhostReplay(const ReplayData &replayData);

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
  TextView *viewerAssistOptionText = nullptr;
  Button *viewerAssistOffButton = nullptr;
  Button *viewerAssistDragButton = nullptr;
  TextView *viewerAssistOffButtonText = nullptr;
  TextView *viewerAssistDragButtonText = nullptr;
  TextInputBox *laneAssignInput = nullptr;
  TextView *laneAssignStatusText = nullptr;
  View *randomDrawerRoot = nullptr;
  ScrollView *randomDrawerScroll = nullptr;

  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;
  size_t randomDrawerPage = 0;
  bool listenActive = false;
  bool listenAudioLoaded = false;
  long long listenEndMicros = 0;
  std::optional<ReplayData> practiceGhostReplay;
  std::vector<ReplaySummary> ghostReplaySummaries;
  int selectedGhostReplayIndex = -1;
  int loadedGhostReplayId = -1;
  std::optional<std::string> viewerPlayOption;
  std::optional<long long> viewerPlayOptionSeed;
  std::optional<std::string> viewerPlayOption2;
  std::optional<long long> viewerPlayOption2Seed;
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
  bool applyGhostReplayData(const ReplayData &replayData, int loadedReplayId,
                            const std::string &successText);
  void clearGhostReplay();
  void rebuildOptionsDrawer();
  void showOptionsDrawer();
  void hideOptionsDrawer();
  void refreshOptionsDrawer();
  void setViewerNamedPlayOption(const std::string &option);
  void setViewerAssistOption(const std::string &option);
  void refreshViewerAssistOptionButtons();
  void setViewerLaneAssign(const std::string &notation);
  void setViewerPlayOptions(const std::optional<std::string> &option,
                            const std::optional<long long> &seed,
                            const std::optional<std::string> &option2,
                            const std::optional<long long> &seed2);
  bool applyViewerPlayOptions(bms_parser::Chart &target,
                              const char *logContext);
  void onCanvasSelectionChanged(long long timeMicros);
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
  [[nodiscard]] std::string defaultLaneAssignNotation() const;
};
