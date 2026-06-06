#pragma once

#include "../ChartDBHelper.h"
#include "../bms_parser.hpp"
#include "Scene.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class ChartCanvasView;
class ScrollView;
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

private:
  struct RandomOption {
    size_t index = 0;
    int maxValue = 1;
    int selectedValue = 1;
    int depth = 0;
  };

  ChartMetaRecord record;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> selectedRandomValues;
  std::vector<RandomOption> randomOptions;
  std::unique_ptr<bms_parser::Chart> chart;

  View *rootLayout = nullptr;
  ChartCanvasView *canvasView = nullptr;
  TextView *titleText = nullptr;
  TextView *subtitleText = nullptr;
  TextView *statusText = nullptr;
  TextView *randomSummaryText = nullptr;
  TextView *zoomText = nullptr;
  View *randomDrawerRoot = nullptr;
  ScrollView *randomDrawerScroll = nullptr;

  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;

  void initView();
  void rebuildRandomDrawer();
  void showRandomDrawer();
  void hideRandomDrawer();
  void parseAndRefresh(std::optional<std::vector<int>> requestedValues);
  void setRandomValue(size_t index, int value);
  void refreshHeaderText();
  void updateZoomText();
  void goBack();

  [[nodiscard]] std::vector<RandomOption> scanActiveRandomOptions() const;
  [[nodiscard]] std::string randomSummary() const;
};
