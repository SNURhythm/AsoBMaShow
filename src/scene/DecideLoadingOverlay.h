#pragma once

#include "../view/BlockingOverlayView.h"
#include "../repositories/ChartRepository.h"

#include <string>

// Full-screen loading ("decide") overlay shown the moment a chart starts, so
// the user sees a transition immediately instead of an unresponsive selector
// while the chart parse + jukebox load runs off the UI thread.
//
// It is a blocking overlay: it consumes every input event, so the underlying
// MusicSelect/MainMenu scene cannot scroll or change selection while loading.
// The built-in view shows the chart title, artist, difficulty, and the stage
// image. It is intentionally a plain View so a future Lua decide skin can
// replace its content without changing the scene plumbing.
class DecideLoadingOverlay : public BlockingOverlayView {
public:
  DecideLoadingOverlay(int x, int y, int width, int height,
                       const ChartMetaRecord &record);

  // Sets the chart metadata to display. Safe to call before first render.
  void setChart(const ChartMetaRecord &record);

  [[nodiscard]] const std::string &titleText() const { return titleText_; }
  [[nodiscard]] const std::string &artistText() const { return artistText_; }
  [[nodiscard]] const std::string &difficultyText() const {
    return difficultyText_;
  }

private:
  void rebuild();
  std::string titleText_;
  std::string artistText_;
  std::string difficultyText_;
  std::string stageFileResourcePath_;
};